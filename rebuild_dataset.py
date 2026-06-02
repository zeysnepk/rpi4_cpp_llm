#!/usr/bin/env python3
"""
Dataset Yeniden Insa: Eski teknik/negatif ornekleri cikar, yenileri ekle
=========================================================================
1. Eski dataset'ten ESKI teknik ornekleri cikarir (hatali bilgi iceriyordu)
2. Yeni teknik + yeni negatif ornekleri ekler
3. Tekrarlari temizler, format dogrular
4. Train/val ayirir

Kullanim:
  python rebuild_dataset.py \
      --old dataset_v2.jsonl \
      --new-tech yeni_teknik.jsonl \
      --new-neg yeni_negatif.jsonl

Cikti: dataset_v3_train.jsonl + dataset_v3_val.jsonl
"""

import json
import sys
import random
import argparse
from collections import Counter

random.seed(42)


def load_jsonl(path):
    """JSONL dosyasini yukle, bozuk satirlari atla. Gemini ciktisindaki
    'onceki sorulari tekrarlama' gibi metin satirlarini da temizle."""
    data = []
    skipped = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            # JSON degilse atla (Gemini bazen aciklama satiri ekliyor)
            if not line.startswith("{"):
                skipped += 1
                continue
            try:
                d = json.loads(line)
                # Format kontrol
                if "messages" in d and len(d["messages"]) == 3:
                    data.append(d)
                else:
                    skipped += 1
            except json.JSONDecodeError:
                skipped += 1
    print(f"  {path}: {len(data)} gecerli, {skipped} atlandi")
    return data


def is_old_technical(d):
    """Eski teknik ornek mi? (system prompt'ta 'teknik' geciyorsa)"""
    sys_p = d["messages"][0]["content"].lower()
    return "teknik asistanisin" in sys_p


def categorize(d):
    """Ornegi kategorize et (rapor icin)."""
    sys_p = d["messages"][0]["content"].lower()
    user = d["messages"][1]["content"].lower()
    ans = d["messages"][2]["content"].lower()

    if "teknik" in sys_p:
        return "teknik"
    if ("veri yok" in user or "veremiyorum" in ans or "danismalisin" in ans
            or "sensorum yok" in ans or "yardimci olamam" in ans
            or "tavsiye veremem" in ans):
        return "negatif"
    if "sohbet" in sys_p or "samimi" in sys_p:
        return "sohbet"
    if "[kritik]" in user or "[uyari]" in user:
        return "uyari_kritik"
    return "sensor_normal"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True, help="Eski dataset (dataset_v2.jsonl)")
    ap.add_argument("--new-tech", help="Yeni teknik ornekler")
    ap.add_argument("--new-neg", help="Yeni negatif ornekler")
    ap.add_argument("--out-prefix", default="dataset_v3", help="Cikti dosya oneki")
    ap.add_argument("--val-ratio", type=float, default=0.05)
    args = ap.parse_args()

    print("=== 1. Eski dataset yukleniyor ===")
    old = load_jsonl(args.old)

    print("\n=== 2. Eski teknik ornekler cikariliyor ===")
    kept = [d for d in old if not is_old_technical(d)]
    removed = len(old) - len(kept)
    print(f"  {removed} eski teknik ornek cikarildi")
    print(f"  {len(kept)} ornek kaldi")

    print("\n=== 3. Yeni ornekler ekleniyor ===")
    new_total = 0
    if args.new_tech:
        new_tech = load_jsonl(args.new_tech)
        kept.extend(new_tech)
        new_total += len(new_tech)
        print(f"  +{len(new_tech)} yeni teknik")
    if args.new_neg:
        new_neg = load_jsonl(args.new_neg)
        kept.extend(new_neg)
        new_total += len(new_neg)
        print(f"  +{len(new_neg)} yeni negatif")

    print("\n=== 4. Tekrarlar temizleniyor ===")
    seen = set()
    clean = []
    dup = 0
    for d in kept:
        # system + user birlikte anahtar
        key = d["messages"][0]["content"] + "|||" + d["messages"][1]["content"]
        if key in seen:
            dup += 1
            continue
        # Bos cevap kontrol
        if not d["messages"][2]["content"].strip():
            dup += 1
            continue
        seen.add(key)
        clean.append(d)
    print(f"  {dup} tekrar/bos atildi")
    print(f"  {len(clean)} temiz ornek")

    print("\n=== 5. Train/val ayriliyor ===")
    random.shuffle(clean)
    val_size = int(len(clean) * args.val_ratio)
    val = clean[:val_size]
    train = clean[val_size:]

    train_path = f"{args.out_prefix}_train.jsonl"
    val_path = f"{args.out_prefix}_val.jsonl"

    with open(train_path, "w", encoding="utf-8") as f:
        for d in train:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
    with open(val_path, "w", encoding="utf-8") as f:
        for d in val:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")

    print(f"  Train: {len(train)} -> {train_path}")
    print(f"  Val:   {len(val)} -> {val_path}")

    print("\n=== 6. Kategori dagilimi (train) ===")
    cats = Counter(categorize(d) for d in train)
    for c, n in cats.most_common():
        print(f"  {c:16s}: {n}")


if __name__ == "__main__":
    main()