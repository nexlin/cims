#!/usr/bin/env python3
"""서비스 안내음성·신호음·보류 음악 기본 세트(`sys:`) 생성기 — docs/design/features/announcements.md §7.2.

마스터 = `pcm/<id>.wav`(16-bit PCM 16 kHz mono — 계측기 tester/worker/samples/pcm 에서 **복사해 독립**시킨 것. 계측기 샘플을 바꿔도
서비스 음원은 바뀌지 않는다). 변환기 `cims-sample-conv`(tester/sampleconv — 콘솔 등록 경로와 같은 변환기)가 코덱 4종을 만든다:
  sys/<id>.pcmu / .pcma   G.711(8 kHz 재표본, 160 B = 20 ms)        sys/<id>.g722   G.722 64 kbit/s(160 B = 20 ms)
  sys/<id>.amrwb          AMR-WB 23.85 kbps raw 61 B 프레임 — **DTX 끔**(안내·신호음·MOH 는 무음 구간도 프레임을 낸다: 단말 jitter buffer·NAT 바인딩 유지)
+ 카탈로그 `sys/catalog.jsonl`(행 = 음원 하나 — id·kind·description·duration_ms·loop·files·sha256). CMP 가 기동 때 읽어 전량 메모리에 상주시킨다.

실행(레포 루트, 산출물은 커밋 — 빌드 의존성 없음. 마스터를 바꿨을 때만):
  CONV=build/bin/cims-sample-conv python3 media/announcements/gen_announcements.py
"""
import hashlib
import json
import os
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
PCM_DIR = os.path.join(HERE, "pcm")
OUT_DIR = os.path.join(HERE, "sys")
CONV = os.environ.get("CONV", os.path.join(HERE, "..", "..", "build", "bin", "cims-sample-conv"))
CODECS = ["pcmu", "pcma", "g722", "amr-wb"]
FILE_EXT = {"pcmu": "pcmu", "pcma": "pcma", "g722": "g722", "amr-wb": "amrwb"}

# id → (kind, loop, description). loop = 카탈로그 힌트(신호음·MOH 는 주기 반복 전제).
SET = [
    ("dial_kr",            "tone", True,  "발신음 350+440 Hz 연속 (E.180 Sup.2 KR)"),
    ("ringback_kr",        "tone", True,  "호출음(링백) 440+480 Hz 1 s on / 2 s off"),
    ("busy_kr",            "tone", True,  "화중음 480+620 Hz 0.5 s on / 0.5 s off"),
    ("congestion_kr",      "tone", True,  "혼잡음(중계선 화중음) 480+620 Hz 0.3 s on / 0.2 s off"),
    ("call_waiting_kr",    "tone", True,  "통화중대기음 350+440 Hz 0.25/0.25/0.25/3.25 s"),
    ("ann_connecting",     "announcement", False, "통화 연결 안내 — \"연결 중입니다. 잠시만 기다려 주십시오.\""),
    ("ann_hold",           "announcement", False, "보류 안내 — \"통화가 보류되었습니다. 잠시만 기다려 주십시오.\""),
    ("ann_call_waiting",   "announcement", False, "통화중대기 안내 — \"다른 전화가 걸려 왔습니다. 잠시 후 다시 연결됩니다.\""),
    ("ann_busy",           "announcement", False, "통화 중 안내 — \"지금 거신 전화는 통화 중이오니 잠시 후 다시 걸어 주십시오.\""),
    ("ann_no_answer",      "announcement", False, "무응답 안내 — \"고객이 전화를 받을 수 없습니다. 잠시 후 다시 걸어 주십시오.\""),
    ("ann_invalid_number", "announcement", False, "없는 번호 안내 — \"지금 거신 번호는 없는 번호입니다. 다시 확인하시고 걸어 주십시오.\""),
    ("moh_simple",         "music", True,  "합성 보류 음악(아르페지오 C–Am–F–G, 120 bpm) -20 dBov"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def duration_ms(wav):
    with wave.open(wav, "rb") as w:
        return int(round(w.getnframes() * 1000.0 / w.getframerate()))


def main():
    if not os.path.isfile(CONV):
        sys.exit(f"converter not found: {CONV} (build first, or CONV=…)")
    os.makedirs(OUT_DIR, exist_ok=True)
    rows = []
    for id_, kind, loop, desc in SET:
        wav = os.path.join(PCM_DIR, f"{id_}.wav")
        if not os.path.isfile(wav):
            sys.exit(f"master missing: {wav}")
        # --no-dtx: 안내·신호음·MOH 는 연속 송출. --master-only 가 아니므로 <id>.wav 사본도 out-dir 에 생기는데 sys/ 에는 두지 않는다.
        r = subprocess.run([CONV, "--in", wav, "--out-dir", OUT_DIR, "--id", id_, "--codecs", ",".join(CODECS), "--no-dtx"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"convert failed {id_}: {r.stderr or r.stdout}")
        try:
            meas = json.loads(r.stdout.strip().splitlines()[-1])
        except Exception:
            meas = {}
        if "error" in meas:
            sys.exit(f"convert error {id_}: {meas['error']}")
        dup = os.path.join(OUT_DIR, f"{id_}.wav")
        if os.path.isfile(dup):
            os.remove(dup)
        files = {c: f"sys/{id_}.{FILE_EXT[c]}" for c in CODECS}
        for c in CODECS:
            if not os.path.isfile(os.path.join(HERE, files[c])):
                sys.exit(f"missing output {files[c]}")
        rows.append({
            "id": f"sys:{id_}", "kind": kind, "description": desc,
            "duration_ms": duration_ms(wav), "loop": loop,
            "files": files,
            "sha256": {c: sha256(os.path.join(HERE, files[c])) for c in CODECS},
            "level_dbov": meas.get("p56_active_level_dbov"),
        })
        print(f"{id_:20s} {kind:12s} {rows[-1]['duration_ms']:6d} ms  level {meas.get('p56_active_level_dbov')}")
    with open(os.path.join(OUT_DIR, "catalog.jsonl"), "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    print(f"catalog: {len(rows)} rows → {os.path.join(OUT_DIR, 'catalog.jsonl')}")


if __name__ == "__main__":
    main()
