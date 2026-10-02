#!/usr/bin/env python3
"""oam_recording_video_timing_test.py — 녹취 영상 재생 속도 = RTP 시각으로 잰 실제 프레임 속도 (recording.md «영상 재생 속도»).

raw RTP 에서 뽑은 H.264 에는 프레임 시각이 없어, 변환기가 SPS VUI timing(없으면 25 fps)이나 고정값(15 fps)을 쓰면
실제와 다른 단말의 영상이 빨리 흘러 음성보다 먼저 끝나고 나머지가 검은 화면이 됐다. 시험은 «영상 안에는 25 fps 라고
적혀 있지만 실제로는 12.5 fps 로 온» 녹취(VoLTE)와 «실제 7.5 fps» 녹취(PTT·MCVideo — 종전 15 고정)를 만들어,
변환본의 영상이 녹취 시간만큼 이어지는지 본다.

  ① _rtp_video_fps — 프레임(서로 다른 RTP timestamp) 수 / 90 kHz 시간 폭, 32비트 wrap, 프레임 2장 미만 = 15
  ② VoLTE 양쪽 영상 — 영상이 끝까지 움직인다(검은 화면이 끝에 1초 넘게 남지 않는다)
  ③ PTT 영상(슬롯 1개, 재인코딩 없는 mux) — 영상 스트림 길이 ≈ 녹취 길이
  ④ 속도 표식 없는 옛 영상 변환본은 재생 요청 때 다시 만들고, 표식이 있거나 음성만인 변환본은 그대로 쓴다
  ⑤ 영상만 있는 슬롯(MCVideo 송출 구간)도 «영상 있음»
  ⑥ 음성 길이 = AMR-WB 프레임 수 × 20 ms(ffprobe 어림 아님) — 결과 끝에 소리·영상 없는 꼬리가 없다
  ⑦ 영상이 음성보다 조금 먼저 끝나면 마지막 장면 유지, 1초 넘게 먼저 끝나면 1초 뒤 검은 바탕

실행: python3 tests/oam_recording_video_timing_test.py   (번들 ffmpeg — 없으면 CIMS_FFMPEG=<ffmpeg 경로>)
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.join(REPO, 'ems/core/oam/src'))
# 번들 ffmpeg(빌드가 받아 둔다) — 없으면 OAM 과 같은 환경변수 CIMS_FFMPEG
FF = os.path.join(REPO, 'ems/core/oam/vendor/bin/ffmpeg')
if not os.path.exists(FF) and os.environ.get('CIMS_FFMPEG'):
    FF = os.environ['CIMS_FFMPEG']
FFPROBE = os.path.join(os.path.dirname(FF), 'ffprobe')

import handlers.recording as rec  # noqa: E402

PASS = FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok   {msg}")
    else:
        FAIL += 1
        print(f"  FAIL {msg}")


def write_rtp(out, pkt, usec):
    """CMP raw RTP 덤프 한 줄 [u32 len][i64 usec][RTP pkt]"""
    out.write(struct.pack('<I', len(pkt)))
    out.write(struct.pack('<q', usec))
    out.write(pkt)


def make_audio_rtp(raw_path, seconds, freq=440, pt=96):
    """사인파 → AMR-WB → raw RTP (20 ms 프레임)"""
    fs_tab = [17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0]
    awb = raw_path + '.awb'
    subprocess.run([FF, '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                    '-i', f'sine=frequency={freq}:duration={seconds}:sample_rate=16000',
                    '-c:a', 'libvo_amrwbenc', '-b:a', '23850', '-ar', '16000', '-ac', '1', '-f', 'amr', awb], check=True)
    data = open(awb, 'rb').read()
    i, seq, ts, usec = len(b'#!AMR-WB\n'), 0, 0, 1_700_000_000_000_000
    with open(raw_path, 'wb') as out:
        while i < len(data):
            toc = data[i]
            fs = fs_tab[(toc >> 3) & 0x0F]
            if fs == 0 or i + 1 + fs > len(data):
                break
            frame = data[i + 1:i + 1 + fs]
            i += 1 + fs
            write_rtp(out, struct.pack('>BBHII', 0x80, pt, seq & 0xFFFF, ts, 0x1111) + bytes([0xF0, toc & 0x7F]) + frame, usec)
            seq += 1; ts += 320; usec += 20000
    os.remove(awb)


def make_video_rtp(raw_path, frames, claimed_fps, real_fps, pt=97, ts0=0):
    """H.264(영상 안 VUI = claimed_fps) 를 real_fps 간격의 RTP 시각으로 싣는다 — 단일 NAL / FU-A(RFC 6184)."""
    h264 = raw_path + '.h264'
    subprocess.run([FF, '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                    '-i', f'testsrc2=size=320x240:rate={claimed_fps}', '-frames:v', str(frames),
                    '-c:v', 'libx264', '-profile:v', 'baseline', '-bf', '0', '-g', str(int(claimed_fps * 2)),
                    '-x264-params', 'slices=1', '-f', 'h264', h264], check=True)
    data = open(h264, 'rb').read()
    nals = [n for n in re.split(b'\x00\x00\x00\x01|\x00\x00\x01', data) if n]
    step = 90000.0 / real_fps
    seq, frame_no, usec, pending = 0, 0, 1_700_000_000_000_000, []
    with open(raw_path, 'wb') as out:
        for nal in nals:
            pending.append(nal)
            if (nal[0] & 0x1F) not in (1, 5):          # SPS/PPS/SEI 는 다음 프레임과 같은 시각
                continue
            ts = (ts0 + int(round(frame_no * step))) & 0xFFFFFFFF
            for k, n in enumerate(pending):
                last_nal = k == len(pending) - 1
                if len(n) <= 1200:
                    chunks = [(n, True)]
                else:
                    ind, typ, body = (n[0] & 0xE0) | 28, n[0] & 0x1F, n[1:]
                    parts = [body[j:j + 1200] for j in range(0, len(body), 1200)]
                    chunks = [(bytes([ind, (0x80 if j == 0 else 0) | (0x40 if j == len(parts) - 1 else 0) | typ]) + p,
                               j == len(parts) - 1) for j, p in enumerate(parts)]
                for payload, end in chunks:
                    m = 0x80 if (last_nal and end) else 0
                    write_rtp(out, struct.pack('>BBHII', 0x80, m | pt, seq & 0xFFFF, ts, 0x2222) + payload, usec)
                    seq += 1
            pending = []
            frame_no += 1
            usec += int(1_000_000 / real_fps)
    os.remove(h264)


def last_motion_and_dur(mp4):
    """(영상이 마지막으로 바뀐 시각, 파일 길이) — 그 뒤는 정지·검은 화면"""
    r = subprocess.run([FF, '-hide_banner', '-i', mp4, '-an', '-vf', "select='gt(scene,0.001)',showinfo", '-f', 'null', '-'],
                       capture_output=True, text=True)
    ts = [float(x) for x in re.findall(r'pts_time:([0-9.]+)', r.stderr)]
    d = subprocess.run([FFPROBE, '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', mp4],
                       capture_output=True, text=True).stdout.strip()
    return (max(ts) if ts else 0.0), float(d or 0)


def stream_dur(mp4, kind):
    d = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', kind, '-show_entries', 'stream=duration',
                        '-of', 'csv=p=0', mp4], capture_output=True, text=True).stdout.strip()
    return float(d or 0)


def black_starts(mp4):
    r = subprocess.run([FF, '-hide_banner', '-i', mp4, '-an', '-vf', 'blackdetect=d=0.1:pix_th=0.10', '-f', 'null', '-'],
                       capture_output=True, text=True)
    return [float(x) for x in re.findall(r'black_start:([0-9.]+)', r.stderr)]


def volte_seg():
    return {'seq': 1, 'type': 'voip', 'audio_pt_a': 96, 'audio_codec_a': 'AMR-WB/16000',
            'audio_pt_b': 96, 'audio_codec_b': 'AMR-WB/16000',
            '_audio_a': 'seg_0001_a.rtp', '_audio_b': 'seg_0001_b.rtp', '_audio': '',
            '_video_a': 'seg_0001_va.rtp', '_video_b': 'seg_0001_vb.rtp', '_video': '', '_tracks': []}


def video_stream_dur(mp4):
    d = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'v', '-show_entries', 'stream=duration',
                        '-of', 'csv=p=0', mp4], capture_output=True, text=True).stdout.strip()
    return float(d or 0)


def test_fps_measure(tmp):
    print("① _rtp_video_fps")
    p = os.path.join(tmp, 'fps.rtp')
    make_video_rtp(p, 40, 25, 12.5)
    f = rec._rtp_video_fps(p)
    check(abs(f - 12.5) < 0.05, f"12.5 fps 로 실린 영상 → {f:.2f} fps")
    w = os.path.join(tmp, 'wrap.rtp')
    make_video_rtp(w, 40, 25, 15, ts0=0xFFFFFFFF - 90000)   # 1초 뒤 32비트 wrap
    f = rec._rtp_video_fps(w)
    check(abs(f - 15) < 0.05, f"RTP timestamp 가 32비트를 넘어가도 → {f:.2f} fps")
    one = os.path.join(tmp, 'one.rtp')
    make_video_rtp(one, 1, 25, 15)
    check(rec._rtp_video_fps(one) == rec._VIDEO_FPS_FALLBACK, "프레임 1장 → 기본 15 fps")


def test_volte(tmp):
    print("② VoLTE 양쪽 영상 — 영상 안 25 fps, 실제 12.5 fps, 10초")
    d = os.path.join(tmp, 'volte')
    os.makedirs(d)
    make_audio_rtp(os.path.join(d, 'seg_0001_a.rtp'), 10, 440)
    make_audio_rtp(os.path.join(d, 'seg_0001_b.rtp'), 10, 660)
    make_video_rtp(os.path.join(d, 'seg_0001_va.rtp'), 125, 25, 12.5)
    make_video_rtp(os.path.join(d, 'seg_0001_vb.rtp'), 125, 25, 12.5)
    rec._transcode_segment_file(d, volte_seg())
    mp4 = rec._converted_path_mp4(d, 1)
    check(os.path.exists(mp4), "변환본이 만들어진다")
    motion, dur = last_motion_and_dur(mp4)
    check(motion >= dur - 0.3, f"영상이 끝까지 움직인다 — 마지막 변화 {motion:.2f}s / 길이 {dur:.2f}s")
    check(os.path.exists(rec._video_timing_marker(mp4)), "영상 변환본에 속도 표식이 붙는다")
    ad = stream_dur(mp4, 'a')
    check(abs(dur - ad) < 0.1, f"파일 길이 {dur:.2f}s = 음성 길이 {ad:.2f}s (끝에 빈 꼬리 없음)")
    check(not black_starts(mp4), f"검은 화면 구간 없음 — 영상이 조금 먼저 끝나도 마지막 장면 유지 {black_starts(mp4)}")


def test_amr_exact(tmp):
    print("⑥ 음성 길이 = AMR-WB 프레임 수 × 20 ms")
    d = os.path.join(tmp, 'amr')
    os.makedirs(d)
    raw = os.path.join(d, 'a.rtp')
    make_audio_rtp(raw, 10, 440)
    awb = os.path.join(d, 'a.awb')
    rec._strip_rtp_to_amrwb(raw, awb, audio_pt=96)
    frames = 0
    with open(raw, 'rb') as f:
        while True:
            h = f.read(12)
            if len(h) < 12:
                break
            f.read(struct.unpack('<I', h[:4])[0])
            frames += 1
    exact = float(rec._audio_duration(awb))
    probe = float(subprocess.run([FFPROBE, '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', awb],
                                 capture_output=True, text=True).stdout.strip() or 0)
    check(abs(exact - frames * 0.02) < 0.001, f"{frames}프레임 → {exact:.3f}s (ffprobe 어림 {probe:.3f}s)")
    check(rec._amrwb_storage_seconds(raw) is None, "AMR-WB storage 가 아니면 None(ffprobe 로 넘어간다)")


def test_volte_long_gap(tmp):
    print("⑦ VoLTE — 영상 5초, 음성 10초 (통화 중 영상을 끈 경우)")
    d = os.path.join(tmp, 'gap')
    os.makedirs(d)
    make_audio_rtp(os.path.join(d, 'seg_0001_a.rtp'), 10, 440)
    make_audio_rtp(os.path.join(d, 'seg_0001_b.rtp'), 10, 660)
    make_video_rtp(os.path.join(d, 'seg_0001_va.rtp'), 63, 25, 12.5)
    make_video_rtp(os.path.join(d, 'seg_0001_vb.rtp'), 63, 25, 12.5)
    rec._transcode_segment_file(d, volte_seg())
    mp4 = rec._converted_path_mp4(d, 1)
    bs = black_starts(mp4)
    check(len(bs) == 1 and abs(bs[0] - (5.04 + rec._VIDEO_HOLD_SEC)) < 0.3,
          f"마지막 장면을 {rec._VIDEO_HOLD_SEC:.0f}초 유지한 뒤 검은 바탕 — 검정 시작 {bs}")


def test_ptt(tmp):
    print("③ PTT 영상(슬롯 1개) — 실제 7.5 fps, 10초 (종전 15 fps 고정)")
    d = os.path.join(tmp, 'ptt')
    os.makedirs(os.path.join(d, 'seg', '000'))
    make_audio_rtp(os.path.join(d, 'seg/000/seg_0001_audio.rtp'), 10, 500)
    make_video_rtp(os.path.join(d, 'seg/000/seg_0001_video.rtp'), 75, 15, 7.5)
    seg = {'seq': 1, 'type': 'ptt', 'duration_ms': 10000, 'has_video': True,
           '_tracks': [{'prefix': 'audio', 'kind': 'audio', 'slot': 0, 'file': 'seg/000/seg_0001_audio.rtp',
                        'pt': 96, 'codec': 'AMR-WB/16000'},
                       {'prefix': 'video', 'kind': 'video', 'slot': 0, 'file': 'seg/000/seg_0001_video.rtp'}]}
    rec._transcode_segment_file(d, seg)
    mp4 = rec._converted_path_mp4(d, 1)
    check(os.path.exists(mp4), "변환본이 만들어진다")
    vd = video_stream_dur(mp4)
    check(abs(vd - 10.0) < 0.6, f"영상 스트림 길이 {vd:.1f}s ≈ 녹취 10s")
    check(os.path.exists(rec._video_timing_marker(mp4)), "영상 변환본에 속도 표식이 붙는다")


def test_stale_cache(tmp):
    print("④ 옛 영상 변환본 다시 만들기")
    d = os.path.join(tmp, 'cache')
    os.makedirs(d)
    open(os.path.join(d, 'seg_0001_a.rtp'), 'wb').write(b'\x00' * 32)
    mp4 = rec._converted_path_mp4(d, 1)
    open(mp4, 'wb').write(b'\x00' * 1024)
    calls = []
    orig, orig_exec = rec._transcode_segment_file, rec._transcode_executor
    rec._transcode_segment_file = lambda r, s, slot=None: calls.append((r, s.get('seq'), slot))
    rec._transcode_executor = None
    try:
        vseg = {'seq': 1, 'audio_file': 'seg_0001_a.rtp', 'has_video': True}
        st = rec._ensure_segment_ready(d, dict(vseg))
        check(st == 'transcoding' and len(calls) == 1, f"표식 없는 영상 변환본 → 다시 변환({st})")
        rec._transcoding_locks.clear()
        rec._mark_video_timing(mp4)
        st = rec._ensure_segment_ready(d, dict(vseg))
        check(st == 'ready' and len(calls) == 1, f"표식 있는 영상 변환본 → 그대로({st})")
        os.remove(rec._video_timing_marker(mp4))
        st = rec._ensure_segment_ready(d, {'seq': 1, 'audio_file': 'seg_0001_a.rtp', 'has_video': False})
        check(st == 'ready' and len(calls) == 1, f"음성만인 변환본은 표식 없이도 그대로({st})")
    finally:
        rec._transcode_segment_file, rec._transcode_executor = orig, orig_exec
        import time
        time.sleep(0.2)                                     # 가로챈 변환 스레드가 끝나게


def test_video_only_slot():
    print("⑤ 영상만 있는 슬롯")
    seg = {'tracks': [{'kind': 'video', 'slot': 2}, {'kind': 'audio', 'slot': 0, 'has_video': False}]}
    check(rec._slot_has_video(seg, 2), "영상 트랙만 있는 슬롯 → 영상 있음")
    check(not rec._slot_has_video(seg, 0), "음성 트랙만 있는 슬롯 → 영상 없음")


def main():
    if not os.path.exists(FF):
        print(f"SKIP: 번들 ffmpeg 없음 ({FF})")
        return 0
    rec.init(recordings_dir='/tmp', ffmpeg_bin=FF, transcode_workers=1)
    tmp = tempfile.mkdtemp(prefix='rec-vt-')
    try:
        test_fps_measure(tmp)
        test_volte(tmp)
        test_ptt(tmp)
        test_stale_cache(tmp)
        test_video_only_slot()
        test_amr_exact(tmp)
        test_volte_long_gap(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n결과: PASS {PASS} / FAIL {FAIL}")
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
