# -*- coding: utf-8 -*-
"""
Muhtas v5 - Tool Calling Fine-Tune (Colab / Unsloth)
=====================================================
v4'ten fark: Model artık sensör verisi enjekte edilmiş "Soru:/Cevap:" formatı
yerine, hangi tool'u çağıracağına KENDISI karar veriyor.

Her eğitim örneği:
  [system] → [user] → [assistant: <tool_call>...] → [tool: veri] → [assistant: cevap]

Loss hesabı: yalnızca assistant turları (tool call JSON + final cevap).
System, user, tool result maskeleniyor.
"""

# %% [0] Google Drive'i bağla
from google.colab import drive
drive.mount('/content/drive')

import os
DRIVE_ROOT = "/content/drive/MyDrive/muhtas_finetune"
CKPT_DIR   = f"{DRIVE_ROOT}/checkpoints_v5"
DATA_DIR   = f"{DRIVE_ROOT}/dataset"
FINAL_DIR  = f"{DRIVE_ROOT}/final_model"
for d in (CKPT_DIR, DATA_DIR, FINAL_DIR):
    os.makedirs(d, exist_ok=True)

print(f"Drive bağlandı.")
print(f"  Checkpoint'ler -> {CKPT_DIR}")
print(f"  NOT: dataset_v5_train.jsonl + dataset_v5_val.jsonl'i {DATA_DIR}/ içine yükle.")

# %% [1] Kurulum
!pip install -q unsloth

# %% [2] Modeli yükle
from unsloth import FastLanguageModel
import torch

# Tool calling konuşmaları v4'ten daha uzun (system + tool tanımları + tool result).
# Token analizi: ortalama ~280, p99 ~450 → 512 biraz dar, 768 güvenli.
max_seq_length = 768
dtype          = None
load_in_4bit   = True

model, tokenizer = FastLanguageModel.from_pretrained(
    model_name     = "unsloth/Qwen3-1.7B",
    max_seq_length = max_seq_length,
    dtype          = dtype,
    load_in_4bit   = load_in_4bit,
)

# %% [3] LoRA adaptörü
model = FastLanguageModel.get_peft_model(
    model,
    r              = 16,
    target_modules = ["q_proj","k_proj","v_proj","o_proj","gate_proj","up_proj","down_proj"],
    lora_alpha     = 32,
    lora_dropout   = 0,
    bias           = "none",
    use_gradient_checkpointing = "unsloth",
    random_state   = 42,
    use_rslora     = False,
)

# %% [4] Dataset yükle ve formatla
from datasets import load_dataset
import json

train_path = f"{DATA_DIR}/dataset_v5_train.jsonl"
val_path   = f"{DATA_DIR}/dataset_v5_val.jsonl"
assert os.path.isfile(train_path), f"Bulunamadı: {train_path}"
assert os.path.isfile(val_path),   f"Bulunamadı: {val_path}"

train_ds = load_dataset("json", data_files=train_path, split="train")
val_ds   = load_dataset("json", data_files=val_path,   split="train")

# Tool tanımları - dashboard.cpp'deki TOOLS ile BIREBIR AYNI olmali.
TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "get_current",
            "description": (
                "Bir sensorun anlik olcumlerini getirir. "
                "Sicaklik, nem, basinc, ivme, gyro, manyetik gibi gercek zamanli "
                "deger istendiginde kullan."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "sensor": {
                        "type": "string",
                        "enum": ["bme280","mpu6050","qmc5883l","all"],
                        "description": (
                            "bme280=sicaklik/nem/basinc, mpu6050=ivme/gyro, "
                            "qmc5883l=manyetik/heading, all=tum sensorler"
                        )
                    }
                },
                "required": ["sensor"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "get_history_stats",
            "description": (
                "Son N saniyenin istatistiklerini getirir: ortalama, min, max, trend. "
                "'Normal mi?', 'Artiyor mu?' tarzi sorular icin kullan."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "sensor": {
                        "type": "string",
                        "enum": ["bme280","mpu6050","qmc5883l"]
                    },
                    "metric": {
                        "type": "string",
                        "description": (
                            "Metrik: temperature_c, humidity_pct, pressure_hpa, "
                            "accel_g.x/y/z, gyro_dps.x/y/z, heading_deg"
                        )
                    },
                    "seconds": {
                        "type": "integer",
                        "description": "Kac saniyelik gecmis (varsayilan: 60)"
                    }
                },
                "required": ["sensor","metric"]
            }
        }
    }
]

def format_chat(examples):
    texts = []
    for messages in examples["messages"]:
        text = tokenizer.apply_chat_template(
            messages,
            tools=TOOLS,                   # tool tanımları template'e enjekte edilir
            tokenize=False,
            add_generation_prompt=False,
            enable_thinking=False,         # start_server.sh ile tutarlı
        )
        texts.append(text)
    return {"text": texts}

train_ds = train_ds.map(format_chat, batched=True)
val_ds   = val_ds.map(format_chat, batched=True)

# Format doğrulaması: tool_call bloğu var mı?
sample = train_ds[0]["text"]
assert "<tool_call>" in sample, "HATA: tool_call bloğu yok! Template sorunu."
print("Format OK — tool_call bloğu mevcut.")
print(sample[:500])

# %% [5] Trainer
from trl import SFTTrainer, SFTConfig
from unsloth.chat_templates import train_on_responses_only

trainer = SFTTrainer(
    model              = model,
    tokenizer          = tokenizer,
    train_dataset      = train_ds,
    eval_dataset       = val_ds,
    dataset_text_field = "text",
    max_seq_length     = max_seq_length,
    dataset_num_proc   = 2,
    packing            = False,
    args = SFTConfig(
        per_device_train_batch_size = 4,    # tool calling örnekleri daha uzun → batch düşük
        gradient_accumulation_steps = 8,    # efektif batch = 32 korunuyor
        warmup_ratio    = 0.05,
        num_train_epochs = 3,
        learning_rate   = 2e-4,
        lr_scheduler_type = "cosine",
        optim           = "adamw_8bit",
        weight_decay    = 0.01,
        logging_steps   = 10,
        eval_strategy   = "steps",
        eval_steps      = 50,
        save_strategy   = "steps",
        save_steps      = 50,
        save_total_limit = 3,
        load_best_model_at_end = True,
        metric_for_best_model  = "eval_loss",
        seed       = 42,
        output_dir = CKPT_DIR,
        report_to  = "none",
    ),
)

# Yalnızca assistant turlarından loss hesapla.
# Tool calling örneklerinde HER İKİ assistant turu da eğitilir:
#   1. <tool_call>...</tool_call> — hangi tool, hangi argümanlar
#   2. Final Türkçe cevap
# System, user ve tool result turları maskeleniyor.
trainer = train_on_responses_only(
    trainer,
    instruction_part = "<|im_start|>user\n",
    response_part    = "<|im_start|>assistant\n",
)

# %% [6] Eğitim + resume desteği
last_checkpoint = None
if os.path.isdir(CKPT_DIR):
    ckpts = [d for d in os.listdir(CKPT_DIR) if d.startswith("checkpoint-")]
    if ckpts:
        last_checkpoint = os.path.join(
            CKPT_DIR, sorted(ckpts, key=lambda x: int(x.split("-")[1]))[-1]
        )
        print(f"Checkpoint bulundu, devam ediliyor: {last_checkpoint}")

trainer_stats = trainer.train(resume_from_checkpoint=last_checkpoint)

# %% [7] Duman testi - tool calling çalışıyor mu?
FastLanguageModel.for_inference(model)

SYSPROMPT = (
    "Sen Muhtas adli bir sensor asistanisin. "
    "PCB uzerinde su sensorler var: "
    "BME280 (sicaklik/nem/basinc), MPU6500 (ivme/jiroskop), QMC5883L (manyetik/heading). "
    "Sensor verisi gerektiginde ilgili tool'u cagir, sonra kisa Turkce cevap ver. "
    "Bu sensorlerde OLMAYAN olcumler (CO2, GPS, ses, hava kalitesi, UV vb.) icin "
    "tool cagirma; 'bu olcum icin sensorum yok' diyerek nazikce reddet. "
    "Sayilari uydurma, sadece tool sonucundaki degerleri kullan."
)

test_prompts = [
    "sıcaklık kaç",           # beklenti: <tool_call> get_current bme280
    "hava kalitesi nasıl",     # beklenti: tool YOK, direkt ret
    "CO2 miktarı ne",          # beklenti: tool YOK, direkt ret
]

for prompt in test_prompts:
    messages = [
        {"role": "system", "content": SYSPROMPT},
        {"role": "user",   "content": prompt},
    ]
    inputs = tokenizer.apply_chat_template(
        messages, tools=TOOLS,
        tokenize=True, add_generation_prompt=True,
        enable_thinking=False, return_tensors="pt",
    ).to("cuda")
    out = model.generate(input_ids=inputs, max_new_tokens=80,
                         temperature=0.1, do_sample=True)
    response = tokenizer.decode(out[0][inputs.shape[1]:], skip_special_tokens=False)
    print(f"\n>>> {prompt}")
    has_tc = "<tool_call>" in response
    print(f"    tool_call={'VAR ✓' if has_tc else 'YOK'}")
    print(f"    {response[:200]}")

# %% [8] LoRA adaptörü → Drive
lora_dir = f"{FINAL_DIR}/qwen3-1.7b-sensor-v5-lora"
model.save_pretrained(lora_dir)
tokenizer.save_pretrained(lora_dir)
print(f"LoRA kaydedildi -> {lora_dir}")

# %% [9] GGUF → yerel diske → Drive'a kopyala
import shutil, glob
local_gguf = "qwen3-1.7b-sensor-v5"
model.save_pretrained_gguf(local_gguf, tokenizer, quantization_method="q4_k_m")

gguf_files = glob.glob(f"{local_gguf}_gguf/*.gguf") or glob.glob(f"{local_gguf}/*.gguf")
for f in gguf_files:
    dst = f"{FINAL_DIR}/{os.path.basename(f)}"
    shutil.copy(f, dst)
    print(f"Drive'a kopyalandı -> {dst}")

print("\nBitti. Drive > muhtas_finetune > final_model/ kontrol et.")
print("start_server.sh içinde MODEL_FILE'ı bu yeni .gguf ile güncelle.")
