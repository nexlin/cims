#!/usr/bin/env python3
"""계측기 워커 동봉 미디어 샘플 생성기 — test_instrument.md §4 미디어 평면.

원음은 전부 16-bit linear PCM · 16 kHz · mono 마스터(`pcm/<id>.wav`)로 만들어 두고, 거기서 코덱별 파일을 뽑는다.
  <id>.pcmu / <id>.pcma   G.711 raw(8 kHz 재표본, 160 B = 20 ms 프레임)
  <id>.g722               G.722 raw(RFC 3551 §4.5.2 — 160 B = 20 ms)
  <id>.amrwb              AMR-WB 23.85 kbps, DTX 없음 — raw 61 B 프레임(ToC + 60 B) 연속
  <id>_dtx.amrwb          AMR-WB 23.85 kbps, DTX(VAD/SCR TS 26.193·26.194) — RFC 4867 §5 저장 형식("#!AMR-WB\\n" +
                          ToC 크기 가변 프레임: SPEECH 61 B · SID 6 B · NO_DATA 1 B). 워커는 NO_DATA 프레임에서 패킷을 내지 않는다.

종류
  ① 신호음  — ITU-T E.180 Supplement 2(한국) · 전기통신설비의 기술기준에 관한 규칙: 발신음 350+440 연속 · 호출음 440+480 1/2 s ·
             화중음 480+620 0.5/0.5 s · 혼잡음 480+620 0.3/0.2 s · 통화중대기음 350+440 0.25/0.25/0.25/3.25 s · 1 kHz 기준음
  ② 안내음  — TTS 한국어(연결 중·보류·통화중대기·통화중·무응답·없는 번호) + 합성 보류 음악
  ③ 통화 음성 — TTS 한국어 대화 문장을 발화/무음 패턴에 맞춰 배치
      speech_act40_kr  한쪽 화자 on/off — ITU-T P.59 표 1(hangover 포함 측정): talk-spurt 지수분포 평균 1.004 s ·
                       pause 0.2 s + 지수분포(평균 1.587 s) → 활동률 38.5 %. AMR-WB VAD 가 클린 음성에서 보는 40 %
                       (3GPP TR 26.976 §29.2, 채널 활동률 51 %)와 같은 급 — DTX 성능 시험의 기준 원천
      speech_act50_kr  같은 talk-spurt 분포, pause 평균 1.004 s → 활동률 50 % (3GPP 시스템 용량 평가 관례 VAF 50 %)
      speech_act100_kr 문장 연속(200 ms 미만 간격만) — 활동률 100 %, DTX 이득 없는 상한(코덱 시험용 P.50 성격)
      conv_p59_a/b     P.59 §3 상태 전이 모델(단독 발화 Tst=-0.854·ln x · 동시 발화 Tdt=-0.226·ln x · 상호 무음 Tms=-0.456·ln x,
                       P1/P2/P3 = 40/50/50 %, 200 ms 미만 pause 규칙)로 만든 두 화자 대화 — 발신자에 a, 착신자에 b 를 주면
                       한 통화 안에서 교대·동시 발화·상호 무음이 맞물린다
  레벨 — 음성은 ITU-T P.56 활성 음성 레벨 -26 dBov(TR 26.976 특성화 시험 입력 레벨), 신호음 -16 dBov(≈ -13 dBm0), 음악 -20 dBov

실행(산출물은 이 디렉터리에 커밋 — 빌드 의존성 없음. 다시 만들 때만):
  python3 -m venv --without-pip venv && curl -sSO https://bootstrap.pypa.io/get-pip.py && venv/bin/python get-pip.py
  venv/bin/python -m pip install numpy edge-tts
  FFMPEG=/path/to/ffmpeg CONV=build/bin/cims-sample-conv venv/bin/python gen_samples.py [--tts-cache DIR] [--only tones,ann,speech]
코덱 변환·측정은 레포의 변환기 `cims-sample-conv`(tester/sampleconv — 컨트롤러가 콘솔 등록 때 쓰는 것과 같은 경로)가 한다.
ffmpeg 는 TTS(edge-tts, 온라인 — ko-KR-SunHiNeural / ko-KR-InJoonNeural)의 mp3 를 WAV 로 푸는 데만 쓴다. 문장 WAV 는 --tts-cache 에 두고 재사용한다.
"""
import argparse
import asyncio
import json
import math
import os
import random
import struct
import subprocess
import sys
import wave

import numpy as np

FS = 16000
FRAME = 320                    # 20 ms @ 16 kHz
HERE = os.path.dirname(os.path.abspath(__file__))
PCM_DIR = os.path.join(HERE, "pcm")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
CONV = os.environ.get("CONV", os.path.join(HERE, "..", "..", "..", "build", "bin", "cims-sample-conv"))
VOICE_A = "ko-KR-SunHiNeural"
VOICE_B = "ko-KR-InJoonNeural"

# ---------------------------------------------------------------- WAV / 레벨 --------------------------------------------------

def write_wav(path, x):
    x = np.clip(np.round(x), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes(x.tobytes())


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == FS, path
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)


def pad_frames(x):
    n = (-len(x)) % FRAME
    return np.concatenate([x, np.zeros(n)]) if n else x


def _iir1(x, g):
    """y[n] = g·y[n-1] + (1-g)·x[n] (P.56 포락선 필터)."""
    y = np.empty_like(x); acc = 0.0
    for i in range(len(x)):
        acc = g * acc + (1.0 - g) * x[i]; y[i] = acc
    return y


def p56_active_level(x, fs=FS):
    """ITU-T P.56 method B — 활성 음성 레벨(dBov, 0 dBov = 32768 RMS)과 활동률(%). 시간상수 30 ms · hangover 200 ms · margin 15.9 dB."""
    if len(x) == 0 or not np.any(x):
        return -100.0, 0.0
    g = math.exp(-1.0 / (fs * 0.03))
    q = _iir1(_iir1(np.abs(x), g), g)
    sq = float(np.sum(x * x)); n = len(x)
    hang = int(0.2 * fs)
    idx = np.arange(n)
    levels = []   # (L_j, C_j, a_j)
    for j in range(16):
        c = 32768.0 * 2.0 ** (-j)
        act = q > c
        last = np.where(act, idx, -10 ** 9)
        last = np.maximum.accumulate(last)
        a = int(np.count_nonzero((idx - last) < hang))
        if a == 0:
            levels.append((None, 20 * math.log10(c / 32768.0), 0)); continue
        L = 10 * math.log10(sq / a / (32768.0 ** 2))
        levels.append((L, 20 * math.log10(c / 32768.0), a))
    M = 15.9
    prev = None
    for L, C, a in reversed(levels):   # 문턱을 낮은 쪽(j=15)부터 올린다 — (L-C) 는 단조 감소
        if L is None: break
        d = L - C
        if d <= M:   # (L-C) 가 M 아래로 떨어지는 첫 지점 — 바로 앞(낮은 문턱) 지점과 선형 보간
            if prev is None:
                return L, 100.0 * a / n
            Lp, Cp, ap = prev; dp = Lp - Cp
            t = (dp - M) / (dp - d) if dp != d else 0.0
            return Lp + t * (L - Lp), 100.0 * (ap + t * (a - ap)) / n
        prev = (L, C, a)
    L, C, a = prev
    return L, 100.0 * a / n


def normalize(x, target_dbov):
    lvl, _ = p56_active_level(x)
    y = x * 10 ** ((target_dbov - lvl) / 20.0)
    peak = np.max(np.abs(y))
    if peak > 32000:
        y *= 32000.0 / peak
    return y


def rms_dbov(x):
    r = math.sqrt(float(np.mean(x * x))) if len(x) else 0.0
    return 20 * math.log10(r / 32768.0) if r > 0 else -100.0

# ---------------------------------------------------------------- ① 신호음 -------------------------------------------------------

def tone(freqs, cadence, total_s, level_dbov=-16.0):
    """freqs 합성 사인, cadence = [(on_s, off_s), …] 반복. RMS 가 level_dbov 가 되게."""
    n = int(round(total_s * FS)); t = np.arange(n) / FS
    sig = sum(np.sin(2 * math.pi * f * t) for f in freqs)
    env = np.zeros(n); pos = 0
    while pos < n:
        for on, off in cadence:
            a = int(round(on * FS)); env[pos:pos + a] = 1.0; pos += a + int(round(off * FS))
            if pos >= n: break
    # on 구간 RMS 기준 레벨(단속음도 켜진 동안 같은 크기), 5 ms 램프로 클릭 억제
    ramp = int(0.005 * FS)
    if ramp:
        edges = np.flatnonzero(np.diff(env))
        for e in edges:
            r = np.linspace(0, 1, ramp)
            if env[e + 1] > env[e]: env[e + 1:e + 1 + ramp] = r[:len(env[e + 1:e + 1 + ramp])]
            else: env[e + 1 - ramp:e + 1] = r[::-1][:len(env[e + 1 - ramp:e + 1])]
    x = sig * env
    on_rms = math.sqrt(float(np.mean((sig ** 2)[env > 0.99]))) if np.any(env > 0.99) else 1.0
    return pad_frames(x * (32768.0 * 10 ** (level_dbov / 20.0) / on_rms))


TONES = {
    # E.180 Sup.2 (Korea, Rep. of) · 전기통신설비의 기술기준에 관한 규칙 — 주파수(Hz) · 단속(on/off s)
    "dial_kr":         ((350, 440), [(4.0, 0.0)], 4.0,  "발신음 350+440 Hz 연속"),
    "ringback_kr":     ((440, 480), [(1.0, 2.0)], 3.0,  "호출음(링백) 440+480 Hz 1 s on / 2 s off"),
    "busy_kr":         ((480, 620), [(0.5, 0.5)], 2.0,  "화중음 480+620 Hz 0.5 s on / 0.5 s off"),
    "congestion_kr":   ((480, 620), [(0.3, 0.2)], 2.0,  "혼잡음(중계선 화중음) 480+620 Hz 0.3 s on / 0.2 s off"),
    "call_waiting_kr": ((350, 440), [(0.25, 0.25), (0.25, 3.25)], 4.0, "통화중대기음 350+440 Hz 0.25/0.25/0.25/3.25 s"),
    "tone_1k":         ((1000,),    [(1.0, 0.0)], 1.0,  "1 kHz 기준음 1 s — 경로·레벨 확인"),
}


def moh_simple(total_s=24.0, level_dbov=-20.0):
    """합성 보류 음악 — 120 bpm 아르페지오(C–Am–F–G), 감쇠 배음 3개. 저작권 없는 결정적 신호."""
    n = int(total_s * FS); x = np.zeros(n)
    chords = [(261.63, 329.63, 392.00, 523.25), (220.00, 261.63, 329.63, 440.00),
              (174.61, 220.00, 261.63, 349.23), (196.00, 246.94, 293.66, 392.00)]
    step = int(0.25 * FS); pos = 0; k = 0
    while pos + step <= n:
        f = chords[(k // 8) % 4][k % 4] * (2 if (k % 8) >= 4 and (k % 2) else 1)
        t = np.arange(step * 2) / FS
        note = (np.sin(2 * math.pi * f * t) + 0.5 * np.sin(4 * math.pi * f * t) + 0.25 * np.sin(6 * math.pi * f * t)) * np.exp(-t * 6)
        end = min(n, pos + len(note)); x[pos:end] += note[:end - pos]
        pos += step; k += 1
    return pad_frames(x * (32768.0 * 10 ** (level_dbov / 20.0) / math.sqrt(float(np.mean(x * x)))))

# ---------------------------------------------------------------- ② TTS ------------------------------------------------------------

ANNOUNCEMENTS = {
    "ann_connecting":     (VOICE_A, "연결 중입니다. 잠시만 기다려 주십시오.",                       "통화 연결 안내"),
    "ann_hold":           (VOICE_A, "통화가 보류되었습니다. 잠시만 기다려 주십시오.",                 "보류 안내"),
    "ann_call_waiting":   (VOICE_A, "다른 전화가 걸려 왔습니다. 잠시 후 다시 연결됩니다.",             "통화중대기 안내"),
    "ann_busy":           (VOICE_A, "지금 거신 전화는 통화 중이오니 잠시 후 다시 걸어 주십시오.",       "통화 중 안내"),
    "ann_no_answer":      (VOICE_A, "고객이 전화를 받을 수 없습니다. 잠시 후 다시 걸어 주십시오.",       "무응답 안내"),
    "ann_invalid_number": (VOICE_A, "지금 거신 번호는 없는 번호입니다. 다시 확인하시고 걸어 주십시오.",  "없는 번호 안내"),
    "ann_forwarded":      (VOICE_A, "전화가 다른 번호로 연결됩니다. 잠시만 기다려 주십시오.",             "착신전환 안내"),
}

PHRASES_A = [
    "네.", "여보세요?", "네, 알겠습니다.", "아, 그렇군요.", "네, 맞습니다.", "감사합니다. 수고하세요.",
    "지금 어디쯤이세요?", "잠시만요, 확인해 보겠습니다.", "괜찮습니다. 천천히 말씀하세요.",
    "오늘 오후 세 시에 현장에서 뵙겠습니다.", "자료는 조금 전에 메일로 보내 드렸습니다.",
    "그 부분은 제가 다시 확인하고 연락드리겠습니다.", "회의 시간이 바뀌어서 미리 알려 드리려고 전화했습니다.",
    "네, 그렇게 진행하시면 될 것 같습니다.", "혹시 어제 보내 드린 일정표는 확인하셨어요?",
    "장비 점검은 내일 오전 중으로 마무리하겠습니다.", "지금 통화 괜찮으세요? 급한 건은 아닙니다.",
    "알겠습니다. 그럼 이따 다시 연락드리겠습니다.", "저도 방금 그 보고서를 봤는데, 수치가 조금 이상하더라고요.",
    "서버 재시작은 점검 창에 맞춰서 하는 게 좋겠습니다.",
]
PHRASES_B = [
    "네, 말씀하세요.", "여보세요.", "아, 네.", "음, 그렇네요.", "네, 감사합니다.", "수고하셨습니다. 들어가세요.",
    "지금 사무실로 들어가는 길입니다.", "잠깐만요, 메모 좀 하겠습니다.", "괜찮습니다. 지금 통화 가능합니다.",
    "그 건은 오후에 팀장님하고 이야기해 보겠습니다.", "메일은 아직 못 봤는데, 지금 바로 확인하겠습니다.",
    "네, 그럼 세 시에 현장에서 뵙겠습니다.", "일정이 바뀌면 저한테도 문자로 한 번 더 보내 주세요.",
    "알겠습니다. 그렇게 정리해서 공유하겠습니다.", "어제 일정표는 봤는데, 화요일 항목이 비어 있더라고요.",
    "점검 결과는 내일 아침 회의 때 같이 보시죠.", "네, 급한 건 아니면 이따 오후에 다시 전화 주세요.",
    "맞습니다. 그 방향이 맞는 것 같습니다.", "보고서 수치는 집계 기준이 달라서 그런 것 같습니다. 다시 뽑아 보겠습니다.",
    "재시작은 오늘 밤 점검 창에 넣어 두겠습니다.",
]


def tts_wav(cache, voice, text):
    """edge-tts → mp3 → 16 kHz mono s16 WAV(앞뒤 무음 제거). 캐시 키 = voice + 문장."""
    import hashlib
    key = hashlib.sha1((voice + "|" + text).encode()).hexdigest()[:16]
    wav = os.path.join(cache, f"{key}.wav")
    if not os.path.exists(wav):
        import edge_tts
        mp3 = wav[:-4] + ".mp3"
        asyncio.run(edge_tts.Communicate(text, voice).save(mp3))
        run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-i", mp3, "-ac", "1", "-ar", str(FS), "-sample_fmt", "s16", wav])
        os.remove(mp3)
    x = read_wav(wav)
    # 앞뒤 무음 제거(-45 dBov 문턱, 20 ms 여유)
    thr = 32768.0 * 10 ** (-45 / 20.0)
    nz = np.flatnonzero(np.abs(x) > thr)
    if len(nz):
        x = x[max(0, nz[0] - FRAME): min(len(x), nz[-1] + FRAME)]
    return x


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}\n{r.stderr}")
    return r

# ---------------------------------------------------------------- ③ 발화 패턴 ---------------------------------------------------

def fill_spurt(rng, pool, target_s, gap_s=0.1):
    """target_s 에 가깝게 문장(잘라 쓰지 않는다)을 골라 잇는다. 반환 = (신호, 실제 길이 s)."""
    parts = []; total = 0.0
    while True:
        remain = target_s - total
        cands = [p for p in pool if p[1] <= remain + 0.25]
        if not cands:
            if not parts: cands = [min(pool, key=lambda p: p[1])]
            else: break
        # 남은 길이에 가까운 것 우선(가중 무작위)
        w = np.array([1.0 / (0.15 + abs(remain - p[1])) for p in cands]); w /= w.sum()
        p = cands[rng.choice(len(cands), p=w)]
        if parts: parts.append(np.zeros(int(gap_s * FS))); total += gap_s
        parts.append(p[0]); total += p[1]
        if total >= target_s - 0.15: break
    x = np.concatenate(parts)
    return x, len(x) / FS


def one_party(rng, pool, total_s, talk_mean, pause_mean, continuous=False):
    """P.59 §2.2 — talk-spurt ~ Exp(talk_mean), pause = 0.2 + Exp(pause_mean - 0.2). continuous = 문장 연속(간격 0.1 s)."""
    out = []; t = 0.0; spurts = []; pauses = []
    while t < total_s:
        if continuous:
            x, d = fill_spurt(rng, pool, total_s - t)
            out.append(x); spurts.append(d); t += d
            if t < total_s: g = 0.1; out.append(np.zeros(int(g * FS))); t += g
            continue
        T = rng.exponential(talk_mean)
        x, d = fill_spurt(rng, pool, T)
        out.append(x); spurts.append(d); t += d
        P = 0.2 + rng.exponential(max(pause_mean - 0.2, 0.01))
        P = min(P, max(total_s - t, 0)); pauses.append(P)
        out.append(np.zeros(int(P * FS))); t += P
    x = np.concatenate(out)[: int(total_s * FS)]
    return pad_frames(x), spurts, pauses


def two_party_p59(rng, pool_a, pool_b, total_s):
    """P.59 §3 상태 전이 — ST_A / ST_B / DT / MS. ST→DT P1=40 % · DT→(ST_A|ST_B) P2=50 % · MS→(ST_A|ST_B) P3=50 %.
    200 ms 미만 pause: 50 % 단독 발화 / 50 % 상호 무음을 pause 가 200 ms 를 넘을 때까지 다시 고른다."""
    n = int(total_s * FS); a = np.zeros(n); b = np.zeros(n)
    t = 0.0; state = "ST_A" if rng.random() < 0.5 else "ST_B"; events = []
    def place(track, pool, at, dur):
        x, d = fill_spurt(rng, pool, dur)
        i = int(at * FS); j = min(n, i + len(x)); track[i:j] += x[: j - i]
        return d
    while t < total_s:
        if state in ("ST_A", "ST_B"):
            T = -0.854 * math.log(rng.random())
            d = place(a if state == "ST_A" else b, pool_a if state == "ST_A" else pool_b, t, T)
            events.append((state, t, d)); t += d
            state = "DT" if rng.random() < 0.40 else "MS"
        elif state == "DT":
            T = -0.226 * math.log(rng.random())
            da = place(a, pool_a, t, T); db = place(b, pool_b, t, T)
            d = max(da, db); events.append(("DT", t, d)); t += d
            state = "ST_A" if rng.random() < 0.5 else "ST_B"
        else:
            T = -0.456 * math.log(rng.random())
            while T < 0.2 and rng.random() < 0.5:
                T += -0.456 * math.log(rng.random())
            events.append(("MS", t, T)); t += T
            state = "ST_A" if rng.random() < 0.5 else "ST_B"
    return pad_frames(a[:n]), pad_frames(b[:n]), events


def spurt_stats(x, hang_s=0.2, thr_dbov=-45.0):
    """마스터에서 잰 on/off 통계 — 20 ms 프레임 에너지 문턱 + hangover(P.59 표 1 과 같은 '측정' 관점)."""
    fr = x[: len(x) // FRAME * FRAME].reshape(-1, FRAME)
    e = 10 * np.log10(np.mean(fr * fr, axis=1) / 32768.0 ** 2 + 1e-12)
    act = e > thr_dbov
    hang = int(hang_s / 0.02); last = -10 ** 9; on = np.zeros(len(act), bool)
    for i, v in enumerate(act):
        if v: last = i
        on[i] = (i - last) < hang
    spurts = []; pauses = []; cur = on[0]; ln = 0
    for v in on:
        if v == cur: ln += 1
        else: (spurts if cur else pauses).append(ln * 0.02); cur = v; ln = 1
    (spurts if cur else pauses).append(ln * 0.02)
    return {"activity_pct": round(100.0 * on.mean(), 1),
            "talkspurt_mean_s": round(float(np.mean(spurts)), 3) if spurts else 0.0,
            "pause_mean_s": round(float(np.mean(pauses)), 3) if pauses else 0.0,
            "talkspurts": len(spurts)}

# ---------------------------------------------------------------- 코덱 변환 -----------------------------------------------------

def convert(id_, codecs, dtx):
    """마스터 pcm/<id>.wav → 코덱 파일 — cims-sample-conv 한 번. 반환 = (files, extra: dtx 집계 등 변환기 JSON 의 나머지)."""
    wav = os.path.join(PCM_DIR, id_ + ".wav")
    cmd = [CONV, "--in", wav, "--out-dir", HERE, "--id", id_, "--codecs", ",".join(codecs)] + ([] if dtx else ["--no-dtx"])
    out = json.loads(run(cmd).stdout)
    if "error" in out: sys.exit(f"cims-sample-conv: {out['error']}")
    master = os.path.join(HERE, id_ + ".wav")   # 변환기가 out-dir 에 다시 쓴 마스터(16 kHz 원본과 동일) — 마스터 자리는 pcm/ 하나
    if os.path.exists(master): os.remove(master)
    extra = {k: out[k] for k in ("amr-wb-dtx", "dtx_frames", "dtx_channel_activity_pct") if k in out}
    return out["files"], extra

# ---------------------------------------------------------------- main ---------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tts-cache", default=os.path.join(HERE, ".tts-cache"))
    ap.add_argument("--only", default="tones,ann,speech")
    ap.add_argument("--seed", type=int, default=20260921)
    args = ap.parse_args()
    only = set(args.only.split(","))
    os.makedirs(PCM_DIR, exist_ok=True); os.makedirs(args.tts_cache, exist_ok=True)
    catalog_path = os.path.join(HERE, "samples.json")
    catalog = json.load(open(catalog_path)) if os.path.exists(catalog_path) else {}

    def emit(id_, x, kind, desc, codecs, dtx=False, stats=None):
        write_wav(os.path.join(PCM_DIR, id_ + ".wav"), x)
        files, extra = convert(id_, codecs, dtx)
        lvl, act = p56_active_level(x)
        rec = {"kind": kind, "description": desc, "master": f"pcm/{id_}.wav", "duration_s": round(len(x) / FS, 2),
               "p56_active_level_dbov": round(lvl, 1), "p56_activity_pct": round(act, 1), "rms_dbov": round(rms_dbov(x), 1), "files": files}
        if stats: rec.update(stats)
        rec.update(extra); catalog[id_] = rec
        print(f"  {id_:22s} {rec['duration_s']:6.2f} s  level {rec['p56_active_level_dbov']:6.1f} dBov  act {rec['p56_activity_pct']:5.1f} %"
              + (f"  DTX ch.act {extra['dtx_channel_activity_pct']} %" if extra else ""))

    if "tones" in only:
        print("① 신호음")
        for id_, (freqs, cad, total, desc) in TONES.items():
            emit(id_, tone(freqs, cad, total), "tone", desc, ["pcmu", "pcma", "g722", "amr-wb"])
        emit("moh_simple", moh_simple(), "music", "합성 보류 음악(아르페지오 C–Am–F–G, 120 bpm) -20 dBov", ["pcmu", "pcma", "g722", "amr-wb"])

    if "ann" in only:
        print("② 안내음")
        for id_, (voice, text, desc) in ANNOUNCEMENTS.items():
            x = tts_wav(args.tts_cache, voice, text)
            x = np.concatenate([np.zeros(int(0.3 * FS)), normalize(x, -26.0), np.zeros(int(0.5 * FS))])
            emit(id_, pad_frames(x), "announcement", f"{desc} — \"{text}\" ({voice})", ["pcmu", "pcma", "g722", "amr-wb"])

    if "speech" in only:
        print("③ 통화 음성")
        rng = np.random.default_rng(args.seed)
        pool_a = [(normalize(tts_wav(args.tts_cache, VOICE_A, s), -26.0), 0.0) for s in PHRASES_A]
        pool_b = [(normalize(tts_wav(args.tts_cache, VOICE_B, s), -26.0), 0.0) for s in PHRASES_B]
        pool_a = [(x, len(x) / FS) for x, _ in pool_a]; pool_b = [(x, len(x) / FS) for x, _ in pool_b]
        print(f"  문장 풀 A {len(pool_a)}개 {min(d for _, d in pool_a):.2f}~{max(d for _, d in pool_a):.2f} s · B {len(pool_b)}개 {min(d for _, d in pool_b):.2f}~{max(d for _, d in pool_b):.2f} s")
        codecs = ["pcmu", "g722", "amr-wb"]
        for id_, talk, pause, cont, desc in [
            ("speech_act40_kr", 1.004, 1.587, False, "한쪽 화자 on/off — P.59 표 1 (talk-spurt 1.004 s · pause 1.587 s → 활동률 38.5 %). DTX 기준 원천"),
            ("speech_act50_kr", 1.004, 1.004, False, "한쪽 화자 on/off — 활동률 50 % (3GPP 용량 평가 VAF 50 %)"),
            ("speech_act100_kr", 0, 0, True, "문장 연속(간격 < 200 ms) — 활동률 100 %, DTX 이득 없는 상한"),
        ]:
            best = None
            for k in range(60):   # 30 s 실현이 목표 활동률에 가장 가까운 seed 를 고른다(짧은 파일의 표본 오차 보정)
                r = np.random.default_rng(args.seed + k)
                x, sp, pa = one_party(r, pool_a, 30.0, talk, pause, cont)
                st = spurt_stats(x); target = 100.0 if cont else 100.0 * talk / (talk + pause)
                err = abs(st["activity_pct"] - target) + (0 if cont else 5 * abs(st["talkspurt_mean_s"] - talk))
                if best is None or err < best[0]: best = (err, x, st)
            emit(id_, normalize(best[1], -26.0), "speech", desc, codecs, dtx=True, stats={"pattern": best[2], "target_activity_pct": round(100.0 if cont else 100.0 * talk / (talk + pause), 1)})
        best = None
        for k in range(60):
            r = np.random.default_rng(args.seed + 1000 + k)
            a, b, ev = two_party_p59(r, pool_a, pool_b, 45.0)
            sa, sb = spurt_stats(a), spurt_stats(b)
            err = abs(sa["activity_pct"] - 38.53) + abs(sb["activity_pct"] - 38.53) + 5 * (abs(sa["talkspurt_mean_s"] - 1.004) + abs(sb["talkspurt_mean_s"] - 1.004))
            if best is None or err < best[0]: best = (err, a, b, ev, sa, sb)
        _, a, b, ev, sa, sb = best
        a, b = normalize(a, -26.0), normalize(b, -26.0)   # 파일 전체 P.56 활성 레벨 -26 dBov
        dt = sum(d for s, _, d in ev if s == "DT"); ms = sum(d for s, _, d in ev if s == "MS")
        both = {"double_talk_pct": round(100.0 * dt / 45.0, 1), "mutual_silence_pct": round(100.0 * ms / 45.0, 1)}
        emit("conv_p59_a", a, "speech", "P.59 §3 두 화자 대화 — A 측(여, 발신자용). conv_p59_b 와 같은 시간축", codecs, dtx=True, stats={"pattern": sa, **both, "pair": "conv_p59_b"})
        emit("conv_p59_b", b, "speech", "P.59 §3 두 화자 대화 — B 측(남, 착신자용). conv_p59_a 와 같은 시간축", codecs, dtx=True, stats={"pattern": sb, **both, "pair": "conv_p59_a"})

    json.dump(catalog, open(catalog_path, "w"), ensure_ascii=False, indent=1)
    print("catalog:", catalog_path)


if __name__ == "__main__":
    main()
