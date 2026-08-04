#!/usr/bin/env python3
"""OAM 녹취 슬롯 트랙 변환 E2E — 동시 발언·전이중 private call 재생 경로 검증.

실제 AMR-WB 를 인코딩해 CMP raw RTP 포맷으로 패킷화한 뒤, recording.py 의
_build_segments/_transcode_segment_file 을 그대로 태워 산출물을 확인한다.

실행: python3 tests/oam_recording_slot_test.py   (레포 루트에서, 번들 ffmpeg 사용)

검증 항목:
  [1] 동시 발언 3슬롯 — 슬롯 트랙 파싱·믹스 변환·슬롯 단독 변환·파형 피크
  [2] 구 녹취(flat 키, tracks[] 이전) — 합성 경로로 종전과 동일 결과
  [3] 전이중 private call — 세그먼트 1개 안의 멤버 2슬롯이 모두 재생 가능
"""
import os
import sys
import json
import struct
import shutil
import subprocess
import tempfile

REPO = '/home/cims/work/cims'
sys.path.insert(0, os.path.join(REPO, 'ems/core/oam/src'))

FF = os.path.join(REPO, 'ems/core/oam/vendor/bin/ffmpeg')
FFPROBE = os.path.join(REPO, 'ems/core/oam/vendor/bin/ffprobe')

import handlers.recording as rec  # noqa: E402

_AMRWB_FS = [17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0]

PASS = FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok   {msg}")
    else:
        FAIL += 1
        print(f"  FAIL {msg}")


def make_awb(path, freq, seconds):
    """사인파 → AMR-WB storage 파일"""
    subprocess.run([FF, '-y', '-hide_banner', '-loglevel', 'error',
                    '-f', 'lavfi', '-i', f'sine=frequency={freq}:duration={seconds}:sample_rate=16000',
                    '-c:a', 'libvo_amrwbenc', '-b:a', '23850', '-ar', '16000', '-ac', '1',
                    '-f', 'amr', path], check=True)


def awb_to_raw_rtp(awb_path, raw_path, pt, ssrc=0x1234):
    """AMR-WB storage → CMP raw RTP 덤프 [u32 len][i64 usec][RTP pkt]"""
    with open(awb_path, 'rb') as f:
        data = f.read()
    assert data.startswith(b'#!AMR-WB\n'), 'AMR-WB storage header 없음'
    i = len(b'#!AMR-WB\n')
    seq = 0
    ts = 0
    usec = 1_700_000_000_000_000
    n = 0
    with open(raw_path, 'wb') as out:
        while i < len(data):
            toc = data[i]
            ft = (toc >> 3) & 0x0F
            fs = _AMRWB_FS[ft]
            if fs == 0 or i + 1 + fs > len(data):
                break
            frame = data[i + 1:i + 1 + fs]
            i += 1 + fs
            # RTP octet-aligned payload: CMR(0xF0) + ToC(F=0) + frame
            payload = bytes([0xF0, toc & 0x7F]) + frame
            hdr = struct.pack('>BBHII', 0x80, pt & 0x7F, seq & 0xFFFF, ts, ssrc)
            pkt = hdr + payload
            out.write(struct.pack('<I', len(pkt)))
            out.write(struct.pack('<q', usec))
            out.write(pkt)
            seq += 1
            ts += 320          # 20ms @ 16kHz
            usec += 20000
            n += 1
    return n


def duration_of(path):
    r = subprocess.run([FFPROBE, '-v', 'error', '-show_entries', 'format=duration',
                        '-of', 'csv=p=0', path], capture_output=True)
    try:
        return float(r.stdout.decode().strip())
    except ValueError:
        return 0.0


def audio_streams(path):
    r = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'a',
                        '-show_entries', 'stream=codec_name', '-of', 'csv=p=0', path],
                       capture_output=True)
    return [x for x in r.stdout.decode().split() if x]


def build_recording(tmp, tracks_meta, duration_ms):
    """seg/000/ 에 raw 파일 + segments.jsonl(tracks[]) 을 만든다"""
    segdir = os.path.join(tmp, 'seg', '000')
    os.makedirs(segdir, exist_ok=True)
    tracks = []
    for t in tracks_meta:
        fname = f"seg_0001_{t['prefix']}.rtp"
        awb = os.path.join(tmp, f"{t['prefix']}.awb")
        make_awb(awb, t['freq'], t['sec'])
        npkt = awb_to_raw_rtp(awb, os.path.join(segdir, fname), t['pt'])
        assert npkt > 0, f"{t['prefix']} 패킷 0개"
        tracks.append({
            'prefix': t['prefix'], 'kind': 'audio', 'slot': t['slot'],
            'file': f"seg/000/{fname}", 'pt': t['pt'], 'codec': 'AMR-WB/16000',
            'speakers': t['speakers'],
        })
    seg = {
        'seq': 1, 'type': 'ptt', 'speaker_id': tracks_meta[0]['speakers'][0]['id'],
        'priority': 5,
        'start_time': '2026-08-04T14:26:40.000000',
        'end_time': '2026-08-04T14:28:14.000000',
        'duration_ms': duration_ms,
        # flat 호환 키
        'audio_file': tracks[0]['file'], 'audio_pt': tracks[0]['pt'],
        'audio_codec': 'AMR-WB/16000',
        'tracks': tracks, 'has_video': False,
    }
    for t in tracks[1:]:
        seg[f"{t['prefix']}_file"] = t['file']
        seg[f"speaker_id_{t['prefix']}"] = t['speakers'][0]['id']
    with open(os.path.join(tmp, 'segments.jsonl'), 'w') as f:
        f.write(json.dumps(seg, ensure_ascii=False) + '\n')
    return seg


# ══════════════════════════════════════════════════════════════

def test_multitalker_mix_and_slots():
    print("\n[1] 동시 발언 3슬롯 — 믹스 + 슬롯 단독 + 파형")
    tmp = tempfile.mkdtemp(prefix='cims_rec_multi_')
    try:
        build_recording(tmp, [
            {'prefix': 'audio',  'slot': 0, 'pt': 96, 'freq': 400, 'sec': 3,
             'speakers': [{'id': '01011112222', 'offset_ms': 0, 'dur_ms': 3000}]},
            {'prefix': 'audio1', 'slot': 1, 'pt': 99, 'freq': 800, 'sec': 2,
             'speakers': [{'id': '01033334444', 'offset_ms': 500, 'dur_ms': 2000}]},
            {'prefix': 'audio2', 'slot': 2, 'pt': 96, 'freq': 1200, 'sec': 1,
             'speakers': [{'id': '01055556666', 'offset_ms': 1500, 'dur_ms': 1000}]},
        ], 3000)

        segs = rec._build_segments(tmp)
        check(len(segs) == 1, "세그먼트 1개")
        s = segs[0]
        check(len(s['tracks']) == 3, f"슬롯 트랙 3개 파싱 (실제 {len(s['tracks'])})")
        check(s['talker_count'] == 3, f"talker_count=3 (실제 {s['talker_count']})")
        check(s['speaker_ids'] == ['01011112222', '01033334444', '01055556666'],
              f"화자 등장순서 {s['speaker_ids']}")
        check(s['max_concurrent'] == 3, f"최대 동시 발언 3명 (실제 {s['max_concurrent']})")

        # 믹스 변환
        rec._transcode_segment_file(tmp, s, None)
        mix = rec._converted_path_mp4(tmp, 1)
        check(os.path.exists(mix), "믹스 mp4 생성")
        check(os.path.getsize(mix) > 1000, f"믹스 mp4 크기 {os.path.getsize(mix)}B")
        check(len(audio_streams(mix)) == 1, "믹스는 단일 오디오 스트림(amix 합성)")
        d = duration_of(mix)
        check(2.8 < d < 3.4, f"믹스 길이 ≈ 최장 화자 3초 (실제 {d:.2f}s)")

        peaks_p = rec._peaks_path(tmp, 1)
        check(os.path.exists(peaks_p), "믹스 파형 피크 JSON 생성")
        pk = json.load(open(peaks_p))
        check(pk['buckets'] > 100, f"피크 버킷 {pk.get('buckets')}개")
        check(max(pk['peaks']) > 20, f"피크 진폭 유효 (max={max(pk['peaks'])})")

        # 슬롯 단독 변환 — 이 경로가 없으면 2·3번째 화자는 영원히 재생 불가였다
        for slot, sec in ((0, 3), (1, 2), (2, 1)):
            rec._transcode_segment_file(tmp, s, slot)
            p = rec._converted_path_mp4(tmp, 1, slot)
            check(os.path.exists(p), f"슬롯 {slot} 단독 mp4 생성")
            ds = duration_of(p)
            check(abs(ds - sec) < 0.5, f"슬롯 {slot} 길이 ≈ {sec}s (실제 {ds:.2f}s)")
            check(os.path.exists(rec._peaks_path(tmp, 1, slot)), f"슬롯 {slot} 파형 생성")

        # 상태 판정
        st = rec._build_segments(tmp)[0]
        check(st['status'] == 'ready', f"세그먼트 상태 ready (실제 {st['status']})")
        for t in st['tracks']:
            check(t['status'] == 'ready', f"슬롯 {t['slot']} 상태 ready")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_legacy_flat_recording():
    print("\n[2] 구 녹취(flat 키, tracks[] 없음) — 합성 경로")
    tmp = tempfile.mkdtemp(prefix='cims_rec_legacy_')
    try:
        segdir = os.path.join(tmp, 'seg', '000')
        os.makedirs(segdir, exist_ok=True)
        awb = os.path.join(tmp, 'a.awb')
        make_awb(awb, 500, 2)
        awb_to_raw_rtp(awb, os.path.join(segdir, 'seg_0001_audio.rtp'), 96)
        seg = {'seq': 1, 'type': 'ptt', 'speaker_id': '01011112222',
               'start_time': '2026-08-04T10:00:00.000000',
               'end_time': '2026-08-04T10:00:02.000000', 'duration_ms': 2000,
               'audio_file': 'seg/000/seg_0001_audio.rtp', 'audio_pt': 96,
               'audio_codec': 'AMR-WB/16000', 'has_video': False}
        with open(os.path.join(tmp, 'segments.jsonl'), 'w') as f:
            f.write(json.dumps(seg) + '\n')

        segs = rec._build_segments(tmp)
        s = segs[0]
        check(len(s['tracks']) == 1, "flat 키에서 트랙 1개 합성")
        check(s['tracks'][0]['slot'] == 0, "슬롯 0 부여")
        check(s['speaker_ids'] == ['01011112222'], "대표 화자 귀속")
        check(s['tracks'][0]['pt'] == 96, "flat audio_pt 승계")

        rec._transcode_segment_file(tmp, s, None)
        mix = rec._converted_path_mp4(tmp, 1)
        check(os.path.exists(mix), "구 녹취도 종전 경로(seg_0001.mp4)로 변환")
        d = duration_of(mix)
        check(abs(d - 2) < 0.5, f"길이 ≈ 2s (실제 {d:.2f}s)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_fullduplex_private_call():
    print("\n[3] 전이중 private call — 세그먼트 1개, 멤버 2슬롯")
    tmp = tempfile.mkdtemp(prefix='cims_rec_duplex_')
    try:
        build_recording(tmp, [
            {'prefix': 'audio',  'slot': 0, 'pt': 96, 'freq': 350, 'sec': 3,
             'speakers': [{'id': '01011112222', 'offset_ms': 0, 'dur_ms': 3000}]},
            {'prefix': 'audio1', 'slot': 1, 'pt': 96, 'freq': 700, 'sec': 3,
             'speakers': [{'id': '01055556666', 'offset_ms': 0, 'dur_ms': 3000}]},
        ], 3000)
        s = rec._build_segments(tmp)[0]
        check(s['talker_count'] == 2, "양측 트랙 인식")
        check(s['max_concurrent'] == 2, "전이중 = 동시 2명")

        rec._transcode_segment_file(tmp, s, None)
        mix = rec._converted_path_mp4(tmp, 1)
        check(os.path.exists(mix), "통화 믹스본 생성 — 종전엔 발신자만 들렸다")
        rec._transcode_segment_file(tmp, s, 1)
        check(os.path.exists(rec._converted_path_mp4(tmp, 1, 1)), "상대측 단독본 생성")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    rec.init(service_log_dir='/tmp', ffmpeg_bin=FF, transcode_workers=2)
    print("=== OAM 녹취 슬롯 트랙 변환 E2E ===")
    test_multitalker_mix_and_slots()
    test_legacy_flat_recording()
    test_fullduplex_private_call()
    print(f"\n결과: pass={PASS} fail={FAIL}")
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
