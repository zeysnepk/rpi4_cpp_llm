#!/usr/bin/env python3
"""
Tool Calling Dataset v5
=======================
Qwen3 tool calling formatında training dataset üretir.
Her sensör örneği: [system] → [user] → [assistant: tool_call] → [tool: sonuç] → [assistant: cevap]
No-tool örnekleri:  [system] → [user] → [assistant: direkt cevap]
"""
import json, random
random.seed(42)

# ── Sabitler ──────────────────────────────────────────────────────────────────

SYSPROMPT = (
    "Sen Muhtas adli bir sensor asistanisin. "
    "PCB uzerinde su sensorler var: "
    "BME280 (sicaklik/nem/basinc), MPU6500 (ivme/jiroskop), QMC5883L (manyetik/heading). "
    "Sensor verisi gerektiginde ilgili tool'u cagir, sonra kisa Turkce cevap ver. "
    "Bu sensorlerde OLMAYAN olcumler (CO2, GPS, ses, hava kalitesi, UV vb.) icin "
    "tool cagirma; 'bu olcum icin sensorum yok' diyerek nazikce reddet. "
    "Sayilari uydurma, sadece tool sonucundaki degerleri kullan."
)

THRESHOLDS = {
    "bme280.temperature_c": (15.0, 32.0),
    "bme280.humidity_pct":  (25.0, 75.0),
    "bme280.pressure_hpa":  (980.0, 1040.0),
    "mpu6050.accel_g.x":   (-0.5, 0.5),
    "mpu6050.accel_g.y":   (-0.5, 0.5),
    "mpu6050.accel_g.z":   (0.80, 1.20),
    "mpu6050.gyro_dps.x":  (-10.0, 10.0),
    "mpu6050.gyro_dps.y":  (-10.0, 10.0),
    "mpu6050.gyro_dps.z":  (-10.0, 10.0),
    "mpu6050.temp_c":      (15.0, 60.0),
}

UNITS = {
    "temperature_c": "°C", "temp_c": "°C", "humidity_pct": "%",
    "pressure_hpa": "hPa",
    "accel_g.x": "g", "accel_g.y": "g", "accel_g.z": "g",
    "gyro_dps.x": "°/s", "gyro_dps.y": "°/s", "gyro_dps.z": "°/s",
    "heading_deg": "°", "mag_g.x": "G", "mag_g.y": "G", "mag_g.z": "G",
}

LABELS = {
    "temperature_c": "sicaklik", "humidity_pct": "nem", "pressure_hpa": "basinc",
    "accel_g.x": "X ivmesi", "accel_g.y": "Y ivmesi", "accel_g.z": "Z ivmesi",
    "gyro_dps.x": "X gyro", "gyro_dps.y": "Y gyro", "gyro_dps.z": "Z gyro",
    "temp_c": "MPU sicakligi",
    "heading_deg": "yon (heading)", "mag_g.x": "X manyetik",
    "mag_g.y": "Y manyetik", "mag_g.z": "Z manyetik",
}

# ── Yardımcılar ───────────────────────────────────────────────────────────────

def status(sensor, metric, val):
    key = f"{sensor}.{metric}"
    if key not in THRESHOLDS:
        return "normal"
    lo, hi = THRESHOLDS[key]
    if val < lo: return "dusuk"
    if val > hi: return "yuksek"
    return "normal"

def fmt_line(sensor, metric, val):
    lbl  = LABELS.get(metric, metric)
    unit = UNITS.get(metric, "")
    st   = status(sensor, metric, val)
    return f"- {lbl}: {val:.2f} {unit} [{st}]"

def rn(lo, hi):   return round(random.uniform(lo * 1.05, hi * 0.95), 2)
def rhi(hi):      return round(hi + random.uniform(0.5, hi * 0.12 + 0.5), 2)
def rlo(lo):      return round(lo - random.uniform(0.5, abs(lo) * 0.12 + 0.5), 2)
def pick(lo, hi, case):
    return {"n": rn(lo,hi), "h": rhi(hi), "l": rlo(lo)}[case]

def tool_call(name, args, cid="c0"):
    return {"role": "assistant", "content": "", "tool_calls": [{
        "id": cid, "type": "function",
        "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)}
    }]}

def tool_result(content, cid="c0"):
    return {"role": "tool", "tool_call_id": cid, "content": content}

def ex(user, tc_msg, tr_msg, answer):
    return {"messages": [
        {"role": "system",    "content": SYSPROMPT},
        {"role": "user",      "content": user},
        tc_msg, tr_msg,
        {"role": "assistant", "content": answer}
    ]}

def ex_notool(user, answer):
    return {"messages": [
        {"role": "system",    "content": SYSPROMPT},
        {"role": "user",      "content": user},
        {"role": "assistant", "content": answer}
    ]}

# ── Soru havuzları ────────────────────────────────────────────────────────────

TEMP_Q = [
    "sıcaklık kaç", "kaç derece", "oda sıcaklığı ne", "sıcak mı",
    "hava sıcaklığı", "ortam sıcaklığı kaç", "sıcaklık değeri ne",
    "şu anki sıcaklık", "sıcaklık ölçümü", "içerisi kaç derece",
    "sıcaklık nasıl", "BME280 sıcaklık",
]
HUM_Q = [
    "nem kaç", "nem oranı", "ortam nemi", "hava nemi kaç",
    "nem yüzdesi ne", "nemli mi", "nem durumu nasıl",
    "nem ölçümü", "bağıl nem kaç",
]
PRES_Q = [
    "basınç kaç", "hava basıncı ne", "atmosfer basıncı",
    "basınç değeri", "hPa kaç", "basınç ölçümü",
]
BME_ALL_Q = [
    "ortam verileri", "çevre ölçümleri", "BME280 verileri",
    "sıcaklık nem basınç", "ortam koşulları", "tüm BME değerleri",
]
MPU_Q = [
    "ivme kaç", "ivme ölçümü", "hareket var mı", "titreşim nasıl",
    "jiroskop değeri", "ivmeölçer ne diyor", "gyro kaç",
    "cihaz hareket ediyor mu", "MPU verileri", "ivme ve gyro",
]
QMC_Q = [
    "manyetik alan kaç", "heading ne", "pusula yönü kaç",
    "manyetometre değeri", "yön ne", "QMC sensörü ne diyor",
    "manyetik ölçüm",
]
HIST_Q = [
    ("sıcaklık artıyor mu",          "bme280", "temperature_c"),
    ("sıcaklık trendi nasıl",        "bme280", "temperature_c"),
    ("nem son dakikada nasıl",        "bme280", "humidity_pct"),
    ("basınç düşüyor mu",             "bme280", "pressure_hpa"),
    ("ivme son 60 saniyede nasıldı",  "mpu6050", "accel_g.z"),
    ("gyro stabil mi",                "mpu6050", "gyro_dps.z"),
]

# ── Cevap üreticileri ─────────────────────────────────────────────────────────

def ans_single(sensor, metric, val):
    st   = status(sensor, metric, val)
    unit = UNITS.get(metric, "")
    lbl  = LABELS.get(metric, metric)
    if st == "normal":
        return random.choice([
            f"{lbl.capitalize()} {val:.2f}{unit}, normal araliginda.",
            f"{lbl.capitalize()} su an {val:.2f}{unit}, beklenen deger.",
        ])
    word = "yukseldi" if st == "yuksek" else "dustu"
    return f"{lbl.capitalize()} {val:.2f}{unit}, esik degerinin {'ustunde' if st=='yuksek' else 'altinda'}."

def ans_bme_all(t, h, p):
    ts = status("bme280", "temperature_c", t)
    hs = status("bme280", "humidity_pct",  h)
    ps = status("bme280", "pressure_hpa",  p)
    issues = [(m, s) for m, s in [("sicaklik", ts), ("nem", hs), ("basinc", ps)] if s != "normal"]
    if not issues:
        return f"Sicaklik {t:.2f}°C, nem %{h:.2f}, basinc {p:.2f} hPa — hepsi normal."
    desc = ", ".join(f"{m} {s}" for m, s in issues)
    return f"Sicaklik {t:.2f}°C, nem %{h:.2f}, basinc {p:.2f} hPa. Dikkat: {desc}."

def ans_mpu(ax, ay, az, gx, gy, gz, tc):
    issues = []
    for m, v in [("accel_g.x",ax),("accel_g.y",ay),("accel_g.z",az),
                 ("gyro_dps.x",gx),("gyro_dps.y",gy),("gyro_dps.z",gz)]:
        if status("mpu6050", m, v) != "normal":
            issues.append(LABELS[m])
    if not issues:
        return f"Z ivmesi {az:.2f} g, tum eksenler normal."
    return f"Z ivmesi {az:.2f} g. Esik disi: {', '.join(issues[:2])}."

def ans_hist(sensor, metric, avg, trend, change):
    unit = UNITS.get(metric, "")
    lbl  = LABELS.get(metric, metric)
    if trend == "artiyor":   tdesc = f"artiyor (+{change:.2f}{unit})"
    elif trend == "azaliyor": tdesc = f"azaliyor (-{change:.2f}{unit})"
    else:                     tdesc = "stabil"
    return f"Son 60 sn {lbl} ortalamasi {avg:.2f}{unit}, {tdesc}."

# ── Örnek üreticileri ─────────────────────────────────────────────────────────

def gen_bme():
    out = []
    cases = [
        ("n","n","n"), ("h","n","n"), ("l","n","n"),
        ("n","h","n"), ("n","l","n"), ("n","n","h"), ("n","n","l"),
        ("h","h","n"), ("l","l","n"),
    ]
    repeat = 30   # her case için yaklaşık bu kadar örnek
    for tc, hc, pc in cases:
        for _ in range(repeat):
            t = pick(15,32,tc); h = pick(25,75,hc); p = pick(980,1040,pc)
            block = ("Sensor verisi:\n"
                     + fmt_line("bme280","temperature_c",t) + "\n"
                     + fmt_line("bme280","humidity_pct",  h) + "\n"
                     + fmt_line("bme280","pressure_hpa",  p) + "\n")
            tc_msg = tool_call("get_current", {"sensor": "bme280"})
            tr_msg = tool_result(block)

            # Sıcaklık sorusu
            out.append(ex(random.choice(TEMP_Q), tc_msg, tr_msg,
                          ans_single("bme280","temperature_c",t)))
            # Nem sorusu
            out.append(ex(random.choice(HUM_Q), tc_msg, tr_msg,
                          ans_single("bme280","humidity_pct",h)))
            # Basınç sorusu (daha az)
            if random.random() < 0.5:
                out.append(ex(random.choice(PRES_Q), tc_msg, tr_msg,
                              ans_single("bme280","pressure_hpa",p)))
            # Tümü sorusu
            if random.random() < 0.4:
                out.append(ex(random.choice(BME_ALL_Q), tc_msg, tr_msg,
                              ans_bme_all(t,h,p)))
    return out

def gen_mpu():
    out = []
    for _ in range(400):
        accase = random.choice(["n","n","n","h"])
        gccase = random.choice(["n","n","n","h"])
        ax = pick(-0.5, 0.5, accase); ay = pick(-0.5, 0.5, accase)
        az = pick(0.80, 1.20, "n" if accase=="n" else "l")
        gx = pick(-10,10,gccase); gy = pick(-10,10,gccase); gz = pick(-10,10,gccase)
        tc = rn(15,60)
        block = ("Sensor verisi:\n"
                 + fmt_line("mpu6050","accel_g.x",ax) + "\n"
                 + fmt_line("mpu6050","accel_g.y",ay) + "\n"
                 + fmt_line("mpu6050","accel_g.z",az) + "\n"
                 + fmt_line("mpu6050","gyro_dps.x",gx) + "\n"
                 + fmt_line("mpu6050","gyro_dps.y",gy) + "\n"
                 + fmt_line("mpu6050","gyro_dps.z",gz) + "\n"
                 + fmt_line("mpu6050","temp_c",tc)     + "\n")
        out.append(ex(
            random.choice(MPU_Q),
            tool_call("get_current", {"sensor": "mpu6050"}),
            tool_result(block),
            ans_mpu(ax,ay,az,gx,gy,gz,tc)
        ))
    return out

def gen_qmc():
    out = []
    for _ in range(200):
        h = round(random.uniform(0,359), 1)
        mx = round(random.uniform(-0.5,0.5),3)
        my = round(random.uniform(-0.5,0.5),3)
        mz = round(random.uniform(-0.3,0.3),3)
        block = ("Sensor verisi:\n"
                 f"- yon (heading): {h:.1f} °\n"
                 f"- X manyetik: {mx:.3f} G\n"
                 f"- Y manyetik: {my:.3f} G\n"
                 f"- Z manyetik: {mz:.3f} G\n")
        out.append(ex(
            random.choice(QMC_Q),
            tool_call("get_current", {"sensor": "qmc5883l"}),
            tool_result(block),
            f"Yon {h:.1f}°, manyetik alan verileri normal."
        ))
    return out

def gen_history():
    out = []
    for _ in range(200):
        q, sensor, metric = random.choice(HIST_Q)
        lo, hi = THRESHOLDS.get(f"{sensor}.{metric}", (0, 100))
        avg    = rn(lo, hi)
        trend  = random.choice(["artiyor","artiyor","azaliyor","stabil"])
        change = round(random.uniform(0.1, 2.0), 2)
        unit   = UNITS.get(metric,"")
        sign   = "+" if trend=="artiyor" else "-"
        block  = (f"Istatistik (son 60 sn) - {sensor}/{metric}:\n"
                  f"- Ortalama: {avg:.2f} {unit}\n"
                  f"- Min: {avg-change:.2f} {unit}\n"
                  f"- Max: {avg+change:.2f} {unit}\n"
                  f"- Trend: {trend} ({sign}{change:.2f} {unit})\n"
                  f"- Durum: {status(sensor, metric, avg)}\n")
        out.append(ex(
            q,
            tool_call("get_history_stats", {"sensor": sensor, "metric": metric, "seconds": 60}),
            tool_result(block),
            ans_hist(sensor, metric, avg, trend, change)
        ))
    return out

def gen_no_tool():
    raw = [
        # Teknik
        ("BME280 nedir",
         "BME280, sicaklik, nem ve basinc olcen bir ortam sensorudur. I2C/SPI protokoluyle haberlesir."),
        ("MPU6500 nedir",
         "MPU6500, 3 eksenli ivmeolcer ve jiroskop icerir. I2C uzerinden okunur."),
        ("QMC5883L nedir",
         "QMC5883L, 3 eksenli manyetometre ve pusula sensorudur."),
        ("hangi sensörler var",
         "BME280 (sicaklik/nem/basinc), MPU6500 (ivme/jiroskop) ve QMC5883L (manyetik/heading) aktif."),
        ("I2C nedir",
         "I2C, iki kablo (SDA/SCL) ile birden fazla sensoru ayni hatta baglayan bir iletisim protokoludur."),
        ("MPU6050 ile MPU6500 farkı",
         "MPU6500 daha dusuk gurultulu ivmeolcer kullanir; her ikisi de ayni I2C adresi ve register yapisiyla calisir."),
        ("heading ne demek",
         "Heading, QMC5883L'den alinan manyetik baslik acidir; kuzey 0°, dogu 90°."),
        ("sampling rate ne",
         "Sampling rate, sensorun saniyede kac olcum yaptigi frekanstur. BME280/MPU6050 varsayilan 5 Hz."),
        # Olmayan sensör
        ("CO2 miktarı ne",
         "CO2 olcumu icin sensorum yok. Mevcut sensorler: BME280, MPU6500, QMC5883L."),
        ("hava kalitesi nasıl",
         "Hava kalitesi sensorum yok. BME280 ile sicaklik, nem ve basinc olcebilirim."),
        ("GPS konumum nerede",
         "GPS sensorum yok, konum verisi sunamam."),
        ("bugün hava nasıl olacak",
         "Hava tahmini yapamam; sadece anlik sensor verilerini okuyabilirim."),
        ("UV indeksi kaç",
         "UV sensorum yok. BME280, MPU6500 ve QMC5883L disinda sensor bulunmuyor."),
        ("ses seviyesi kaç",
         "Ses/desibel olcen sensorum yok."),
        ("ışık şiddeti ne",
         "Isik sensorum yok. Mevcut sensorler ortam, hareket ve manyetik olcumu yapiyor."),
        ("nabzım kaç",
         "Nabiz veya saglik olcum sensorum yok."),
        ("toz oranı ne",
         "Partikul / toz sensorum yok."),
        # Sohbet
        ("merhaba",
         "Merhaba! Sensor verileri hakkinda sorularinda yardimci olabilirim."),
        ("nasılsın",
         "Iyiyim, sensorler de aktif! Bir sey sormak ister misin?"),
        ("teşekkürler", "Rica ederim!"),
        ("sağ ol", "Ne demek, her zaman!"),
        # Alakasız
        ("bana tarif anlat",
         "Ben sensor verilerini yorumlayan bir asistanim, yemek tarifi konusunda yardimci olamam."),
        ("borsa nasıl",
         "Borsa verileri benim kapsamimda degil. Sensor sorularinda yardimci olabilirim."),
        ("moralim çok bozuk",
         "Uzgunum, umarim gecer. Sensor verileri hakkinda merak ettigin bir sey var mi?"),
        ("şiir yaz",
         "Siir yazmak uzmanlık alanım degil; sensor sorularinda yardımcı olabilirim."),
    ]
    out = []
    repeats = {
        "BME280 nedir": 8, "MPU6500 nedir": 6, "QMC5883L nedir": 5,
        "hangi sensörler var": 10, "I2C nedir": 5,
        "CO2 miktarı ne": 10, "hava kalitesi nasıl": 10,
        "GPS konumum nerede": 8, "bugün hava nasıl olacak": 8,
        "UV indeksi kaç": 6, "ses seviyesi kaç": 6,
        "merhaba": 6, "nasılsın": 4, "teşekkürler": 4,
        "bana tarif anlat": 8, "borsa nasıl": 6, "moralim çok bozuk": 6,
    }
    for q, a in raw:
        n = repeats.get(q, 4)
        for _ in range(n):
            out.append(ex_notool(q, a))
    return out

# ── Ana akış ──────────────────────────────────────────────────────────────────

def main():
    print("Dataset v5 olusturuluyor (tool calling formati)...")

    bme  = gen_bme()
    mpu  = gen_mpu()
    qmc  = gen_qmc()
    hist = gen_history()
    nt   = gen_no_tool()

    print(f"  BME280 ornekleri : {len(bme)}")
    print(f"  MPU6050 ornekleri: {len(mpu)}")
    print(f"  QMC5883L         : {len(qmc)}")
    print(f"  History stats    : {len(hist)}")
    print(f"  No-tool          : {len(nt)}")

    all_data = bme + mpu + qmc + hist + nt
    random.shuffle(all_data)

    val_n  = int(len(all_data) * 0.05)
    val    = all_data[:val_n]
    train  = all_data[val_n:]

    with open("dataset_v5_train.jsonl","w",encoding="utf-8") as f:
        for d in train: f.write(json.dumps(d, ensure_ascii=False) + "\n")
    with open("dataset_v5_val.jsonl","w",encoding="utf-8") as f:
        for d in val:   f.write(json.dumps(d, ensure_ascii=False) + "\n")

    print(f"\nToplam : {len(all_data)}")
    print(f"  Train : {len(train)}  -> dataset_v5_train.jsonl")
    print(f"  Val   : {len(val)}    -> dataset_v5_val.jsonl")

    # Hızlı format doğrulaması
    sample = train[0]
    roles  = [m["role"] for m in sample["messages"]]
    assert roles[0] == "system"
    assert roles[-1] == "assistant"
    has_tc = any("tool_calls" in m for m in sample["messages"])
    print(f"\nOrnek kontrol: roller={roles}, tool_call_var={has_tc} OK")

if __name__ == "__main__":
    main()
