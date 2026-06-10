# -*- coding: utf-8 -*-
"""
Muhtas Sensor Asistani - Qwen3-1.7B Fine-Tune (Colab / Unsloth)
================================================================
KULLANIM: Bu dosyayi Colab'da hücre hücre çalıştır (her "# %%" bir hücre
sınırıdır - kopyala/yapıştır ya da `jupytext` ile .ipynb'ye çevir).
Runtime > Değiştir > T4 GPU (ücretsiz katman yeterli).

NEDEN BU SECIMLER?
- Taban model: Qwen3-1.7B (start_server.sh zaten bu aileyi RPi4'te
  çalıştırıyor; ayni mimari + chat template ile devam = uyum garantisi)
- Yöntem: QLoRA (4-bit + LoRA) - 1.7B model + ~3.5K dar-alan örnek için
  full fine-tune'a gerek yok; LoRA hem T4'e (16GB) rahat sığar hem de
  hızlı, hem de taban modelin genel yeteneklerini bozmaz (catastrophic
  forgetting riski düşük).
- Taban olarak ONCEKI fine-tune'lardan biri DEGIL, orijinal Qwen3-1.7B
  kullaniliyor: dataset_v3 -> v4 gecisinde format/içerik önemli ölçüde
  değişti (bkz. fix_anomaly_examples.py); eski checkpoint'in üstüne
  eğitmek eski hatalı örüntüleri taşıma riski taşır. Temiz başlamak
  daha güvenli.

Cikti: GGUF (q4_k_m) - start_server.sh'nin doğrudan kullanabileceği format.
"""

# %% [0] Google Drive'i bağla (gece boyu çalışacağı için ŞART)
# NEDEN: Colab oturumu rastgele kopabilir (inaktivite/zaman aşımı). Checkpoint'ler
# ve final çıktı yerel diske ("/content/...") yazılırsa, oturum koptuğunda
# UYANDIĞINDA HER ŞEY SİLİNMİŞ olur. Drive'a yazarsak hem ara checkpoint'ler
# hem final model kalıcı olur, hatta kopma olursa kaldığı yerden devam edebiliriz.
from google.colab import drive
drive.mount('/content/drive')

import os
DRIVE_ROOT = "/content/drive/MyDrive/muhtas_finetune"
CKPT_DIR   = f"{DRIVE_ROOT}/checkpoints"   # eğitim SIRASINDA buraya yazılır (resume için)
DATA_DIR   = f"{DRIVE_ROOT}/dataset"       # dataset_v4_*.jsonl'i BİR KERE buraya yükle
FINAL_DIR  = f"{DRIVE_ROOT}/final_model"   # LoRA adaptörü + GGUF eğitim sonunda buraya kopyalanır
for d in (CKPT_DIR, DATA_DIR, FINAL_DIR):
    os.makedirs(d, exist_ok=True)

print(f"Drive bağlandı.")
print(f"  Checkpoint'ler -> {CKPT_DIR}")
print(f"  Dataset        -> {DATA_DIR}  (dataset_v4_train.jsonl + dataset_v4_val.jsonl'i BİR KEZ buraya yükle)")
print(f"  Final model    -> {FINAL_DIR}")
print("NOT: Dataset'i Drive'a koyduktan sonra sonraki gece çalıştırmalarında")
print("     tekrar Colab'a upload etmene gerek kalmaz, hep oradan okunur.")

# %% [1] Kurulum
# Unsloth; transformers/peft/trl/bitsandbytes/accelerate'i de birlikte getirir.
# get_chat_template importu bazı sürümlerde gerekebiliyor, sorun çıkarsa
# `pip install -q unsloth unsloth_zoo` ile dene.
!pip install -q unsloth

# %% [2] Modeli yükle (4-bit QLoRA)
from unsloth import FastLanguageModel
import torch

# Dataset analizi (dataset_v4_train.jsonl, 3356 örnek):
#   ortalama ~127 token, p99 ~242, maksimum ~250 token (sistem+kullanıcı+asistan toplamı)
# 512, bolca pay bırakarak hem güvenli hem de gereksiz bellek/hız kaybı yaratmıyor.
# (2048 gibi büyük bir değer hem yavaşlatır hem VRAM'i boşuna tüketir.)
max_seq_length = 512
dtype          = None   # None -> Unsloth GPU'ya göre otomatik seçer (T4: float16, A100: bfloat16)
load_in_4bit   = True   # QLoRA: ~1.7B modeli 4-bit'e indirger, T4 16GB'da rahat çalışır

model, tokenizer = FastLanguageModel.from_pretrained(
    model_name     = "unsloth/Qwen3-1.7B",   # taban: Qwen3-1.7B Instruct (Unsloth derlemesi, hızlı indirme)
    max_seq_length = max_seq_length,
    dtype          = dtype,
    load_in_4bit   = load_in_4bit,
)

# %% [3] LoRA adaptörü ekle
model = FastLanguageModel.get_peft_model(
    model,
    r = 16,
    # NEDEN 16: 1.7B model + ~3.5K örnekten oluşan DAR ALAN (sensör asistanı)
    # için 16 yeterli kapasiteyi sağlıyor. 32-64 gibi yüksek rank'lar bu
    # büyüklükte bir dataset'te ezberlemeye (overfit) ve eğitim süresinin
    # uzamasına yol açar; 8 ise yeni davranışları (örn. düzeltilmiş
    # [yuksek]/[dusuk] formatı) öğrenmek için sınırlı kalabilir.
    target_modules = ["q_proj", "k_proj", "v_proj", "o_proj",
                      "gate_proj", "up_proj", "down_proj"],
    lora_alpha  = 32,     # 2*r kuralı - LoRA için yaygın ve dengeli başlangıç
    lora_dropout = 0,     # Unsloth: 0 -> en optimize edilmiş çekirdek; az epoch + küçük
                          # veri setinde dropout'un overfit'i önleme katkısı sınırlı
    bias = "none",
    use_gradient_checkpointing = "unsloth",   # bellek tasarrufu (kısa dizilerde de zararsız)
    random_state = 42,    # rebuild_dataset.py'deki seed (42) ile tutarlı, tekrarlanabilirlik için
    use_rslora  = False,
)

# %% [4] Dataset'i yükle ve Qwen3 chat template'ine çevir
from datasets import load_dataset

# Dataset Drive'dan okunuyor (DATA_DIR, hücre [0]'da tanımlandı).
# İlk çalıştırmadan önce dataset_v4_train.jsonl + dataset_v4_val.jsonl
# dosyalarını Drive > muhtas_finetune > dataset/ içine BİR KEZ yükle;
# sonraki gece çalıştırmalarında tekrar upload etmene gerek kalmaz.
train_path = f"{DATA_DIR}/dataset_v4_train.jsonl"
val_path   = f"{DATA_DIR}/dataset_v4_val.jsonl"
assert os.path.isfile(train_path), f"Dataset bulunamadi: {train_path} - once Drive'a yukle!"
assert os.path.isfile(val_path),   f"Dataset bulunamadi: {val_path} - once Drive'a yukle!"

train_ds = load_dataset("json", data_files=train_path, split="train")
val_ds   = load_dataset("json", data_files=val_path,   split="train")

def format_chat(examples):
    texts = []
    for messages in examples["messages"]:
        text = tokenizer.apply_chat_template(
            messages,
            tokenize=False,
            add_generation_prompt=False,
            # KRITIK: dashboard.cpp llama-server'a istek atarken
            # `chat_template_kwargs: {"enable_thinking": false}` gönderiyor.
            # Eğitim ve üretim arasında <think> bloğu farkı oluşmasın diye
            # burada da aynı bayrağı kapalı tutuyoruz - aksi halde model
            # üretimde göremeyeceği bir kalıba göre eğitilmiş olur.
            enable_thinking=False,
        )
        texts.append(text)
    return {"text": texts}

train_ds = train_ds.map(format_chat, batched=True)
val_ds   = val_ds.map(format_chat, batched=True)

print(train_ds[0]["text"])   # formatın <|im_start|>system/user/assistant şeklinde çıktığını doğrula

# %% [5] Trainer kurulumu - SADECE asistan cevaplarından loss hesapla
from trl import SFTTrainer, SFTConfig
from unsloth.chat_templates import train_on_responses_only

trainer = SFTTrainer(
    model = model,
    tokenizer = tokenizer,
    train_dataset = train_ds,
    eval_dataset  = val_ds,
    dataset_text_field = "text",
    max_seq_length = max_seq_length,
    dataset_num_proc = 2,
    packing = False,
    # NEDEN packing=False: konuşmalar zaten kısa (ort. 127 token). Packing
    # birden fazla örneği tek diziye sıkıştırır; loss-mask hesaplamasını
    # karmaşıklaştırır ve "Soru:...Cevap:" sınırlarının bulanmasına yol
    # açabilir - bu projede hız kazancı, riske değmez.
    args = SFTConfig(
        per_device_train_batch_size = 8,
        gradient_accumulation_steps = 4,    # efektif batch = 32
        warmup_ratio   = 0.05,
        num_train_epochs = 3,
        # NEDEN 3 EPOCH: 3356 örnek x 3 = ~315 adım (effektif batch 32 ile).
        # Dar alanlı (sensör asistanı) bir dataset'te 2-3 epoch genelde
        # yeterli; 5+ epoch küçük dataset'lerde ezberlemeye/yanıt çeşitliliğinin
        # azalmasına yol açar (eski dataset'te templated cevap örüntüleri -
        # "Evet, nem orani..." x12 - muhtemelen aşırı epoch'un izi).
        learning_rate  = 2e-4,
        # LoRA için standart aralık (1e-4 - 3e-4). Full fine-tune'a göre
        # ~10x yüksek olması normal: yalnızca küçük adaptör matrisleri eğitiliyor.
        lr_scheduler_type = "cosine",
        optim          = "adamw_8bit",      # bellek tasarruflu optimizer (Unsloth önerisi)
        weight_decay   = 0.01,
        logging_steps  = 10,
        eval_strategy  = "steps",
        eval_steps     = 50,
        save_strategy  = "steps",
        save_steps     = 50,
        save_total_limit = 3,
        load_best_model_at_end = True,
        metric_for_best_model  = "eval_loss",
        # NEDEN load_best_model_at_end: dar-alan + kısa eğitimde en düşük
        # eval_loss genelde son adımda olmayabilir; bu otomatik olarak
        # "ezberlemeden önceki" en iyi checkpoint'i geri yükler.
        seed = 42,
        # NEDEN DOGRUDAN DRIVE: LoRA checkpoint'leri küçüktür (yalnızca adaptör
        # ağırlıkları + optimizer durumu, ~tüm modelin değil) - Drive'a yazmanın
        # I/O maliyeti ihmal edilebilir düzeyde, ama oturum gece yarısı koparsa
        # son checkpoint Drive'da kalıcı olur ve aşağıdaki resume mantığıyla
        # kaldığı yerden devam edilebilir. Yerel "outputs" kullansaydık ve
        # oturum koparsaydı sabah hiçbir şey kalmamış olurdu.
        output_dir = CKPT_DIR,
        report_to  = "none",
    ),
)

# Sistem+kullanıcı kısmını maskele, yalnızca "<|im_start|>assistant\n" sonrası
# üretilen tokenlardan loss hesapla.
# NEDEN ONEMLI: Aksi halde model "Soru: ... Veri:\nSensor verisi:\n- sicaklik:..."
# kalıbını da ezberlemeye/tahmin etmeye çalışır (bu zaten sabit, modelin
# URETMESI gereken kisim degil) - asil hedef olan dogal Turkce cevap
# uretimine ayrilan ogrenme kapasitesi sulanir.
trainer = train_on_responses_only(
    trainer,
    instruction_part = "<|im_start|>user\n",
    response_part    = "<|im_start|>assistant\n",
)

# %% [6] Eğitimi başlat
# T4'te ~315 adım, kısa dizilerle (max 512 token) tahmini 25-40 dk sürer.
#
# GECE KOPMA SENARYOSU: Oturum eğitim bitmeden koparsa, sabah bu hücreleri
# tekrar çalıştırdığında aşağıdaki kontrol CKPT_DIR (Drive) içinde en son
# checkpoint'i bulur ve eğitim KALDIĞI ADIMDAN devam eder - sıfırdan başlamaz.
last_checkpoint = None
if os.path.isdir(CKPT_DIR):
    ckpts = [d for d in os.listdir(CKPT_DIR) if d.startswith("checkpoint-")]
    if ckpts:
        last_checkpoint = os.path.join(
            CKPT_DIR, sorted(ckpts, key=lambda x: int(x.split("-")[1]))[-1]
        )
        print(f"Var olan checkpoint bulundu, oradan devam edilecek: {last_checkpoint}")

trainer_stats = trainer.train(resume_from_checkpoint=last_checkpoint)

# %% [7] Hızlı duman testi - GGUF'a çevirmeden önce gerçek senaryolarla dene
FastLanguageModel.for_inference(model)

SYSPROMPT_SENSOR = (
    "Sen sensor asistanisin. Sana sensor verisi gelir, "
    "Turkce 1-2 cumle cevap ver. "
    "Sayilari oldugu gibi kullan, uydurma. "
    "Sensorler: BME280 (sicaklik/nem/basinc), "
    "MPU6500 (ivme/gyro), QMC5883L (manyetik/heading)."
)

test_cases = [
    # dashboard.cpp'nin gercekte uretecegi formatla BIREBIR (dogru [tag]/birim/etiket)
    "Soru: sicaklik kac derece\nVeri:\nSensor verisi:\n"
    "- sicaklik: 34.20 °C [yuksek]\n- nem: 51.30 % [normal]\n- basinc: 1016.80 hPa [normal]\n\nCevap:",

    "Soru: ivme olcumleri normal mi\nVeri:\nSensor verisi:\n"
    "- X ivmesi: 0.04 g [normal]\n- Y ivmesi: -0.02 g [normal]\n- Z ivmesi: 0.35 g [dusuk]\n\nCevap:",

    "Soru: hava kalitesi nasil, CO2 seviyesi kac\nVeri:\n"
    "Bu olcum icin sensor yok. Mevcut sensorler: BME280 (sicaklik/nem/basinc), "
    "MPU6500 (ivme/gyro), QMC5883L (manyetik/heading).\n\nCevap:",
]

for prompt in test_cases:
    messages = [{"role": "system", "content": SYSPROMPT_SENSOR},
                {"role": "user",   "content": prompt}]
    inputs = tokenizer.apply_chat_template(
        messages, tokenize=True, add_generation_prompt=True,
        enable_thinking=False, return_tensors="pt",
    ).to("cuda")
    out = model.generate(input_ids=inputs, max_new_tokens=120,
                         temperature=0.2, do_sample=True)
    print("---")
    print(tokenizer.decode(out[0][inputs.shape[1]:], skip_special_tokens=True))

# %% [8] LoRA adaptörünü kaydet -> doğrudan Drive (FINAL_DIR)
# NEDEN: Bu, eğitimin "asıl ürünü". Yerel diske kaydedip oturum kapanırsa
# (ör. uyandığında otomatik zaman aşımı tetiklenmişse) kaybolur.
lora_dir = f"{FINAL_DIR}/qwen3-1.7b-sensor-v4-lora"
model.save_pretrained(lora_dir)
tokenizer.save_pretrained(lora_dir)
print(f"LoRA adaptoru kaydedildi -> {lora_dir}")

# %% [9] GGUF'a çevir - start_server.sh DOGRUDAN bunu kullanabilir
# q4_k_m: start_server.sh'deki MODEL_FILE ile aynı kuantizasyon -> RPi4'te
# kanıtlanmış hız/kalite dengesi (~mevcut qwen3-1.7b.Q4_K_M_v2.gguf ile aynı sınıf)
#
# NEDEN ÖNCE YEREL DİSKE: GGUF dönüşümü llama.cpp'yi indirip derliyor ve
# ağır dosya I/O yapıyor - bunu doğrudan Drive (FUSE bağlı, yavaş) üzerinde
# yapmak çok yavaşlar, hatta zaman zaman hata verir. Bu yüzden önce yerel
# diske ("/content/...") çeviriyoruz, sonra SADECE bitmiş .gguf dosyasını
# Drive'a kopyalıyoruz - hem hızlı hem güvenli.
local_gguf_dir = "qwen3-1.7b-sensor-v4"
model.save_pretrained_gguf(
    local_gguf_dir,
    tokenizer,
    quantization_method="q4_k_m",
)

import shutil, glob
# Unsloth, verilen isme "_gguf" suffix'i ekliyor (örn. "qwen3-1.7b-sensor-v4_gguf/...")
# Her ikisini de kontrol et - gelecekteki sürümlerde davranış değişebilir.
gguf_files = glob.glob(f"{local_gguf_dir}_gguf/*.gguf") or glob.glob(f"{local_gguf_dir}/*.gguf")
for f in gguf_files:
    dst = f"{FINAL_DIR}/{os.path.basename(f)}"
    shutil.copy(f, dst)
    print(f"Drive'a kopyalandi -> {dst}")

print()
print("Uyandiginda kontrol et: Drive > muhtas_finetune > final_model/")
print("RPi'ye/Mac'e tasimak icin: .gguf dosyasini models/ klasorune kopyala,")
print("start_server.sh icindeki MODEL_FILE'i bu yeni dosyanin adiyla guncelle.")
