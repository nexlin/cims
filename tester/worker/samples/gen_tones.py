#!/usr/bin/env python3
"""계측기 워커 동봉 미디어 샘플 생성 — G.711 raw(8 kHz, 160 B = 20 ms 프레임). test_instrument.md §4 미디어 평면.

  ringback_kr.{pcmu,pcma}  한국 링백 톤 — 440+480 Hz, 1 s on / 2 s off (3 s 한 주기)
  tone_1k.{pcmu,pcma}      1 kHz 연속 톤 1 s — 경로·레벨 확인용

산출물은 같은 디렉터리에 커밋한다(빌드 의존성 없음). 다시 만들 때만 실행: `python3 gen_tones.py`
AMR-WB 샘플(raw 61 B 프레임, 23.85 kbps)은 인코더가 필요하므로 동봉하지 않는다 — 운영자가 SampleDir 에 넣는다.
"""
import math
import os

RATE = 8000


def _ulaw(x: int) -> int:
    x = max(-32635, min(32635, x))
    sign = 0x80 if x < 0 else 0
    x = abs(x) + 0x84
    exp = 7
    mask = 0x4000
    while exp > 0 and not (x & mask):
        exp -= 1
        mask >>= 1
    return ~(sign | (exp << 4) | ((x >> (exp + 3)) & 0x0F)) & 0xFF


def _alaw(x: int) -> int:
    # 레포 기준 구현(cspsim/G711.cpp linear2alaw — Sun g711.c)과 같은 결정 경계: 음수는 크기를 -x-8 로 잡는다.
    #   -7..-1 은 그 식이 음수가 되므로 0 으로 막는다(기준 구현은 이 구간에서 엉뚱한 부호를 낸다).
    mask = 0xD5 if x >= 0 else 0x55
    v = x if x >= 0 else max(0, -x - 8)
    seg = next((i for i, end in enumerate((0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF)) if v <= end), 8)
    if seg >= 8:
        return 0x7F ^ mask
    q = (v >> 4) & 0x0F if seg < 2 else (v >> (seg + 3)) & 0x0F
    return ((seg << 4) | q) ^ mask


def _pcm(freqs, on_s, off_s, amp=6000):
    out = []
    for n in range(int(on_s * RATE)):
        out.append(int(sum(amp * math.sin(2 * math.pi * f * n / RATE) for f in freqs)))
    out += [0] * int(off_s * RATE)
    return out[:len(out) - len(out) % 160]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, pcm in (('ringback_kr', _pcm((440, 480), 1.0, 2.0)), ('tone_1k', _pcm((1000,), 1.0, 0.0, amp=8000))):
        for ext, enc in (('pcmu', _ulaw), ('pcma', _alaw)):
            with open(os.path.join(here, f'{name}.{ext}'), 'wb') as f:
                f.write(bytes(enc(s) for s in pcm))
            print(f'{name}.{ext}: {len(pcm)} B ({len(pcm) // 160} frames)')


if __name__ == '__main__':
    main()
