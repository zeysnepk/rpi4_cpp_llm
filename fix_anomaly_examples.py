#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dataset format-uyumsuzlugu duzeltme: [uyari]/[kritik] -> gercek runtime formati
================================================================================
SORUN: dataset_v3'teki 697 ornek (~%17), Analyzer::analyze() / format_for_llm()
(src/analyzer.cpp, src/tool_dispatcher.cpp) tarafindan ASLA uretilemeyecek bir
format kullaniyor:
  - Etiketler: [uyari]/[kritik]  -- runtime sadece [normal]/[yuksek]/[dusuk] uretir
  - Birimler:  µT (manyetometre)  -- runtime "G" (Gauss) uretir
  - Etiket adlari: "ivme"/"manyetik"/"X ivme" -- runtime "X ivmesi"/"X manyetik" uretir
  - QMC5883L icin config'de threshold YOK -- runtime asla qmc okumalarina [tag] eklemiyor,
    ama bu ornekler ekliyor
  - Cevap metinleri de bu hayali semaya gore yazilmis ("KRITIK UYARI! ... parasut acin"
    gibi asiri dramatik, PCB uzerindeki ortam sensorlerine uygun olmayan ifadeler iceriyor)

COZUM: bu 697 ornegi cikar, yerine runtime'in GERCEKTEN uretebilecegi formatla
(dogru etiket: dusuk/yuksek/normal, dogru birim/etiket adi, olculu ton) elle
yazilmis ~45 yeni anomali/normal ornegi ekle. Boylece model "[yuksek]/[dusuk]"
etiketlerini production'da gercekten karsilasacagi sekilde ogrenir.

Cikti: dataset_v4_train.jsonl + dataset_v4_val.jsonl
"""

import json
import random

random.seed(42)

SYSPROMPT_SENSOR = (
    "Sen sensor asistanisin. Sana sensor verisi gelir, "
    "Turkce 1-2 cumle cevap ver. "
    "Sayilari oldugu gibi kullan, uydurma. "
    "Sensorler: BME280 (sicaklik/nem/basinc), "
    "MPU6500 (ivme/gyro), QMC5883L (manyetik/heading)."
)

# ============================================================
# RUNTIME MIRROR - analyzer.cpp + tool_dispatcher.cpp ile BIREBIR
# ============================================================
THRESHOLDS = {
    ("bme280", "temperature_c"): (15, 32),
    ("bme280", "humidity_pct"):  (25, 75),
    ("bme280", "pressure_hpa"):  (980, 1040),
    ("mpu6050", "accel_g.x"):    (-0.5, 0.5),
    ("mpu6050", "accel_g.y"):    (-0.5, 0.5),
    ("mpu6050", "accel_g.z"):    (0.80, 1.20),
    ("mpu6050", "gyro_dps.x"):   (-10.0, 10.0),
    ("mpu6050", "gyro_dps.y"):   (-10.0, 10.0),
    ("mpu6050", "gyro_dps.z"):   (-10.0, 10.0),
    ("mpu6050", "temp_c"):       (15, 60),
}

LABELS = {
    ("bme280", "temperature_c"): "sicaklik",
    ("bme280", "humidity_pct"):  "nem",
    ("bme280", "pressure_hpa"):  "basinc",
    ("mpu6050", "accel_g.x"):    "X ivmesi",
    ("mpu6050", "accel_g.y"):    "Y ivmesi",
    ("mpu6050", "accel_g.z"):    "Z ivmesi",
    ("mpu6050", "gyro_dps.x"):   "X gyro",
    ("mpu6050", "gyro_dps.y"):   "Y gyro",
    ("mpu6050", "gyro_dps.z"):   "Z gyro",
    ("mpu6050", "temp_c"):       "MPU sicakligi",
}


def unit_for(metric):
    if metric in ("temperature_c", "temp_c"): return "°C"
    if metric == "humidity_pct": return "%"
    if metric == "pressure_hpa": return "hPa"
    if metric.startswith("accel_g"): return "g"
    if metric.startswith("gyro_dps"): return "°/s"
    return ""


def status_of(sensor, metric, value):
    th = THRESHOLDS.get((sensor, metric))
    if not th:
        return None
    tmin, tmax = th
    if value < tmin: return "dusuk"
    if value > tmax: return "yuksek"
    return "normal"


def fmt_block(sensor, metric_values):
    """tool_dispatcher.cpp::format_for_llm (get_current, tek sensor) ile ayni cikti."""
    lines = ["Sensor verisi:"]
    for metric, value in metric_values:
        label = LABELS[(sensor, metric)]
        unit = unit_for(metric)
        st = status_of(sensor, metric, value)
        line = f"- {label}: {value:.2f} {unit}"
        if st:
            line += f" [{st}]"
        lines.append(line)
    return "\n".join(lines)


def build_user(question, sensor, metric_values):
    """dashboard.cpp::user_for_llm (tool_result yolu) ile BIREBIR ayni: tool_text_for_llm
    sonunda '\\n' var, sonra '+\"\\n\"' ekleniyor -> Cevap: oncesinde bos satir olusuyor."""
    block = fmt_block(sensor, metric_values)
    return f"Soru: {question}\nVeri:\n{block}\n\nCevap:"


# ============================================================
# YENI ORNEKLER -  format dogru, ton olculu (dramatize yok)
# her ornek: (soru, sensor, [(metric, deger), ...], cevap)
# ============================================================
RAW_EXAMPLES = [
    # ---------------- BME280 ----------------
    ("ofis bayagi sicak galiba, durum nasil", "bme280",
     [("temperature_c", 34.80), ("humidity_pct", 48.20), ("pressure_hpa", 1015.40)],
     "Evet, sicaklik 34.80°C ile esik degerin uzerinde, ortam beklenenden sicak. "
     "Nem ve basinc normal araliginda, havalandirma iyi gelebilir."),

    ("sicaklik normal mi", "bme280",
     [("temperature_c", 33.20), ("humidity_pct", 52.00), ("pressure_hpa", 1018.10)],
     "Sicaklik 33.20°C ile normal araligin biraz uzerinde olculdu, dikkat etmekte fayda var. "
     "Nem ve basinc ise normal seviyede."),

    ("burasi neden bu kadar soguk", "bme280",
     [("temperature_c", 12.40), ("humidity_pct", 38.50), ("pressure_hpa", 1021.00)],
     "Sicaklik 12.40°C ile beklenen alt sinirin altinda kalmis, ortam soguk. "
     "Nem ve basinc tarafinda bir sorun yok."),

    ("ortam sicakligi uygun mu", "bme280",
     [("temperature_c", 13.80), ("humidity_pct", 44.00), ("pressure_hpa", 1019.50)],
     "Hayir, sicaklik 13.80°C ile alt esigin altinda, biraz dusuk kalmis. "
     "Diger degerler normal gorunuyor."),

    ("nem orani yuksek mi", "bme280",
     [("temperature_c", 26.50), ("humidity_pct", 82.00), ("pressure_hpa", 1014.20)],
     "Evet, nem orani %82.00 ile esigin uzerinde, ortam oldukca rutubetli. "
     "Sicaklik ve basinc normal seyrediyor."),

    ("ortam cok rutubetli mi", "bme280",
     [("temperature_c", 24.10), ("humidity_pct", 78.50), ("pressure_hpa", 1016.80)],
     "Nem %78.50 ile normal araligin uzerinde, evet rutubet yuksek sayilir. "
     "Sicaklik ve basinc tarafinda anomali yok."),

    ("hava cok kuru hissettiriyor, oyle mi", "bme280",
     [("temperature_c", 23.40), ("humidity_pct", 18.00), ("pressure_hpa", 1017.30)],
     "Haklisin, nem orani %18.00 ile alt esigin altinda kalmis, hava kuru. "
     "Sicaklik ve basinc normal seviyede."),

    ("nem seviyesi dusuk mu", "bme280",
     [("temperature_c", 22.90), ("humidity_pct", 21.50), ("pressure_hpa", 1013.60)],
     "Evet, nem %21.50 ile esigin biraz altinda, kuru bir ortam soz konusu. "
     "Sicaklik ve basinc normal."),

    ("basinc degisti mi", "bme280",
     [("temperature_c", 22.00), ("humidity_pct", 47.00), ("pressure_hpa", 1045.00)],
     "Basinc 1045.00 hPa ile ust esigin uzerine cikmis, normalden yuksek. "
     "Sicaklik ve nem ise normal araliginda."),

    ("hava basinci normal mi", "bme280",
     [("temperature_c", 21.50), ("humidity_pct", 50.30), ("pressure_hpa", 1042.50)],
     "Hayir, basinc 1042.50 hPa ile esigin az ustunde olcumlendi. "
     "Sicaklik ve nem tarafinda bir sapma yok."),

    ("firtina geliyor olabilir mi, basinc ne durumda", "bme280",
     [("temperature_c", 19.80), ("humidity_pct", 61.00), ("pressure_hpa", 965.00)],
     "Basinc 965.00 hPa ile alt esigin epey altinda, belirgin bir dusus var. "
     "Sicaklik ve nem normal seviyede, sadece basinci takip etmekte fayda var."),

    ("basinc dusuk mu", "bme280",
     [("temperature_c", 20.50), ("humidity_pct", 55.00), ("pressure_hpa", 972.00)],
     "Evet, basinc 972.00 hPa ile esigin altinda kalmis. "
     "Sicaklik ve nem normal goruluyor."),

    ("her sey yolunda mi", "bme280",
     [("temperature_c", 33.60), ("humidity_pct", 79.40), ("pressure_hpa", 1017.00)],
     "Tam olarak degil: sicaklik 33.60°C ve nem %79.40 ile ikisi de esik uzerinde, "
     "ortam hem sicak hem rutubetli. Basinc ise normal."),

    ("anomali var mi", "bme280",
     [("temperature_c", 13.10), ("humidity_pct", 49.80), ("pressure_hpa", 974.50)],
     "Evet, sicaklik 13.10°C ve basinc 974.50 hPa ikisi de alt esigin altinda kalmis. "
     "Nem tarafinda bir sorun yok."),

    ("genel durum nasil", "bme280",
     [("temperature_c", 24.30), ("humidity_pct", 46.70), ("pressure_hpa", 1016.20)],
     "Sicaklik, nem ve basinc uc deger de normal araliginda, ortamda dikkat ceken bir sey yok."),

    ("su an her sey normal mi", "bme280",
     [("temperature_c", 22.80), ("humidity_pct", 51.90), ("pressure_hpa", 1013.00)],
     "Evet, sicaklik 22.80°C, nem %51.90 ve basinc 1013.00 hPa ile uc olcum de normal seviyede."),

    ("sicaklik kac, sinirda mi", "bme280",
     [("temperature_c", 32.40), ("humidity_pct", 60.10), ("pressure_hpa", 1011.50)],
     "Sicaklik 32.40°C ile esik degerin hemen ustune cikmis, sinirda sayilir. "
     "Nem ve basinc normal."),

    ("nem cok mu dustu", "bme280",
     [("temperature_c", 25.60), ("humidity_pct", 24.10), ("pressure_hpa", 1020.30)],
     "Evet, nem %24.10 ile esigin az altina inmis, hafifce kuru bir ortam var. "
     "Sicaklik ve basinc normal araliginda."),

    ("dikkat etmem gereken bir sey var mi", "bme280",
     [("temperature_c", 22.20), ("humidity_pct", 44.50), ("pressure_hpa", 1038.90)],
     "Su an icin tek dikkat ceken nokta basincin 1038.90 hPa ile ust esige yakin olmasi, "
     "henuz esigi asmamis ama izlemekte fayda var. Sicaklik ve nem normal."),

    ("ortam konforlu mu", "bme280",
     [("temperature_c", 27.10), ("humidity_pct", 39.80), ("pressure_hpa", 1009.40)],
     "Evet, sicaklik 27.10°C, nem %39.80 ve basinc 1009.40 hPa ile uc deger de "
     "konfor araliginda, ortam rahat sayilir."),

    # ---------------- MPU6050 ----------------
    ("cihaz dusuyor mu, ivme garip", "mpu6050",
     [("accel_g.x", 0.05), ("accel_g.y", -0.03), ("accel_g.z", 0.30)],
     "Z eksenindeki ivme 0.30g ile beklenen yercekimi araliginin (0.80-1.20g) "
     "altinda, bu da cihazin serbest dususe yakin bir durumda olabilecegini gosteriyor. "
     "X ve Y ekseni normal."),

    ("ani bir darbe oldu mu", "mpu6050",
     [("accel_g.x", 0.12), ("accel_g.y", -0.08), ("accel_g.z", 1.65)],
     "Evet, Z ekseni ivmesi 1.65g ile esigin belirgin sekilde uzerinde, "
     "ani bir darbe veya sicrama yasanmis olabilir. X ve Y ekseni normal seviyede."),

    ("yatay eksende hareket var mi", "mpu6050",
     [("accel_g.x", 0.82), ("accel_g.y", 0.04), ("accel_g.z", 1.05)],
     "Evet, X ekseni ivmesi 0.82g ile statik araligin uzerinde, yatayda belirgin "
     "bir hareket veya egim soz konusu. Y ve Z ekseni normal."),

    ("Y ekseninde sapma var mi", "mpu6050",
     [("accel_g.x", -0.10), ("accel_g.y", -0.74), ("accel_g.z", 1.02)],
     "Evet, Y ekseni ivmesi -0.74g ile statik esigin disina cikmis, bir egim ya da "
     "yanal hareket var gibi gorunuyor. X ve Z ekseni normal."),

    ("ivme olcumleri nasil", "mpu6050",
     [("accel_g.x", 0.02), ("accel_g.y", -0.04), ("accel_g.z", 0.99)],
     "Uc eksendeki ivme degerleri de normal araliginda; cihaz sakin ve sabit duruyor."),

    ("doniyor muyuz", "mpu6050",
     [("gyro_dps.x", -2.10), ("gyro_dps.y", 1.40), ("gyro_dps.z", 46.50)],
     "Evet, Z ekseni acisal hizi 46.50°/s ile esigin epey uzerinde, belirgin bir "
     "donus hareketi var. X ve Y ekseni normal seviyede."),

    ("titresim var mi", "mpu6050",
     [("gyro_dps.x", -27.30), ("gyro_dps.y", 3.20), ("gyro_dps.z", -1.80)],
     "Evet, X ekseni acisal hizi -27.30°/s ile statik esigin disina cikmis, bu da "
     "titresim ya da ani bir donus hareketine isaret ediyor. Y ve Z ekseni normal."),

    ("acisal hiz tarafinda bir sapma var mi", "mpu6050",
     [("gyro_dps.x", 4.10), ("gyro_dps.y", 19.80), ("gyro_dps.z", -3.50)],
     "Evet, Y ekseni 19.80°/s ile esigin uzerinde, o eksende bir donus hareketi var. "
     "X ve Z ekseni normal araliginda."),

    ("acisal hiz olcumleri normal mi", "mpu6050",
     [("gyro_dps.x", 1.20), ("gyro_dps.y", -0.90), ("gyro_dps.z", 2.40)],
     "Evet, uc eksendeki acisal hiz degerleri de normal araliginda, donus hareketi yok."),

    ("MPU isinmis mi", "mpu6050",
     [("accel_g.x", 0.01), ("accel_g.y", 0.02), ("temp_c", 64.50)],
     "Evet, MPU sicakligi 64.50°C ile esik degerin uzerinde, sensor beklenenden sicak "
     "calisiyor. Ivme tarafinda bir anomali yok."),

    ("MPU sensoru soguk mu", "mpu6050",
     [("accel_g.z", 1.01), ("temp_c", 11.20)],
     "Evet, MPU ic sicakligi 11.20°C ile alt esigin altinda kalmis, sensor sogukta "
     "calisiyor. Z ekseni ivmesi normal."),

    ("sarsinti mi oldu", "mpu6050",
     [("accel_g.z", 1.85), ("gyro_dps.z", 38.20), ("gyro_dps.x", 2.10)],
     "Evet, hem Z ekseni ivmesi (1.85g) hem de Z ekseni acisal hizi (38.20°/s) "
     "esik degerlerin uzerinde; ani bir sarsinti veya darbe yasanmis olabilir."),

    ("hareket sensoru ne diyor", "mpu6050",
     [("accel_g.x", -0.03), ("accel_g.y", 0.01), ("accel_g.z", 1.00),
      ("gyro_dps.x", 0.50), ("gyro_dps.y", -0.30), ("gyro_dps.z", 0.80)],
     "Ivme ve acisal hiz degerlerinin tamami normal araliginda; cihaz sabit ve "
     "hareketsiz durumda."),

    ("X ekseni ivmesi dusuk mu kaldi", "mpu6050",
     [("accel_g.x", -0.65), ("accel_g.y", 0.06), ("accel_g.z", 0.97)],
     "Evet, X ekseni ivmesi -0.65g ile statik esigin altina inmis, beklenenden "
     "daha negatif bir deger. Y ve Z ekseni normal."),

    ("Z ekseninde anormal bir donus var mi", "mpu6050",
     [("gyro_dps.x", 0.40), ("gyro_dps.y", -1.10), ("gyro_dps.z", -14.60)],
     "Evet, Z ekseni acisal hizi -14.60°/s ile esigin altina sarkmis, ters yonde "
     "belirgin bir donus var. X ve Y ekseni normal."),

    ("carpisma oldu mu", "mpu6050",
     [("accel_g.x", 0.30), ("accel_g.y", -0.45), ("accel_g.z", 2.10)],
     "Z ekseni ivmesi 2.10g ile esigin oldukca uzerinde, sert bir darbe ya da "
     "carpisma yasanmis olabilir. X ekseni normal, Y ekseni ise sinira yakin."),

    ("dongu mu yapiyoruz", "mpu6050",
     [("gyro_dps.x", 12.40), ("gyro_dps.y", -88.30), ("gyro_dps.z", 5.10)],
     "Evet, Y ekseni acisal hizi -88.30°/s ile esigin cok uzerinde, hizli ve "
     "surekli bir donus hareketi soz konusu. X ekseni de hafifce esigi asmis, "
     "Z ekseni ise normal."),

    ("sicaklik tarafinda sorun var mi mpu icin", "mpu6050",
     [("accel_g.z", 1.10), ("temp_c", 58.40)],
     "MPU sicakligi 58.40°C ile henuz esigin altinda ama ust sinira yaklasmis, "
     "izlemekte fayda var. Z ekseni ivmesi normal goruluyor."),

    ("mpu280 anomali var mi", "mpu6050",
     [("accel_g.x", 0.04), ("accel_g.y", -0.02), ("accel_g.z", 1.03),
      ("gyro_dps.x", -1.50), ("gyro_dps.y", 0.90), ("gyro_dps.z", 0.40)],
     "Hayir, ivme ve acisal hiz degerlerinin tamami normal araliginda, anomali yok."),

    ("aniden egildik mi", "mpu6050",
     [("accel_g.x", 0.58), ("accel_g.y", 0.49), ("accel_g.z", 0.88)],
     "Evet, X ekseni ivmesi 0.58g ile statik esigin uzerine cikmis, bir egilme veya "
     "yatay yonelme degisikligi olmus olabilir. Y ve Z ekseni normal araliginda."),

    ("durum ciddi mi, ivme cok yuksek gorunuyor", "mpu6050",
     [("accel_g.x", 0.95), ("accel_g.y", 0.88), ("accel_g.z", 1.92)],
     "Evet, uc eksende de ivme degerleri esik araliklarinin disinda; cihaz oldukca "
     "sert bir hareket veya darbe yasamis gorunuyor, kontrol etmekte fayda var."),

    ("gyro degerleri kotu mu", "mpu6050",
     [("gyro_dps.x", -3.20), ("gyro_dps.y", 2.10), ("gyro_dps.z", 1.40)],
     "Hayir, uc eksendeki acisal hiz degerleri de normal araliginda, kotu bir durum yok."),
]


def build_examples():
    out = []
    for question, sensor, metric_values, answer in RAW_EXAMPLES:
        user = build_user(question, sensor, metric_values)
        out.append({
            "messages": [
                {"role": "system",  "content": SYSPROMPT_SENSOR},
                {"role": "user",    "content": user},
                {"role": "assistant", "content": answer},
            ]
        })
    return out


# ============================================================
# ANA ISLEM: 697 fantom-format ornegi cikar + yenileri ekle
# ============================================================
def load_jsonl(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def is_phantom_format(d):
    user = d["messages"][1]["content"]
    return "[uyari]" in user or "[kritik]" in user


def main():
    train = load_jsonl("dataset_v3_train.jsonl")
    val   = load_jsonl("dataset_v3_val.jsonl")
    combined = train + val

    removed = [d for d in combined if is_phantom_format(d)]
    kept    = [d for d in combined if not is_phantom_format(d)]
    print(f"Toplam ornek: {len(combined)}")
    print(f"Cikarilan ([uyari]/[kritik] fantom-format): {len(removed)}")
    print(f"Kalan: {len(kept)}")

    fresh = build_examples()
    print(f"Eklenen yeni (runtime-uyumlu) ornek: {len(fresh)}")

    final = kept + fresh
    random.shuffle(final)

    val_ratio = 0.05
    val_size = int(len(final) * val_ratio)
    new_val = final[:val_size]
    new_train = final[val_size:]

    with open("dataset_v4_train.jsonl", "w", encoding="utf-8") as f:
        for d in new_train:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
    with open("dataset_v4_val.jsonl", "w", encoding="utf-8") as f:
        for d in new_val:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")

    print(f"\nYazildi: dataset_v4_train.jsonl ({len(new_train)}), "
          f"dataset_v4_val.jsonl ({len(new_val)})")


if __name__ == "__main__":
    main()
