#!/usr/bin/env python3
"""oam_recording_mcvideo_test.py — MCVideo 그룹 호 녹취가 PTT 세션 경로로 재생되는지 (recording.md §3.3.1).

  ① 세그먼트 type mcvideo 는 슬롯 트랙 변환(_transcode_ptt_multi)으로 간다 — VoIP 경로로 가면 원본 없음으로 실패한다.
  ② 오디오 없이 영상만 있는 송출 구간(mcvideo.md D12 — 송출 중 무전)도 상태가 raw 이고 변환 대상이다.
  ③ 세션 서비스 = session.json type (없으면 세그먼트 type, 그래도 없으면 ptt).

실행:  python3 tests/oam_recording_mcvideo_test.py   (ffmpeg 불필요 — 변환 함수는 가로챈다)
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ems', 'core', 'oam', 'src'))
from handlers import recording  # noqa: E402

_pass = _fail = 0


def check(name, cond, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print(f"  PASS  {name}")
    else:
        _fail += 1
        print(f"  FAIL  {name}{('  — ' + detail) if detail else ''}")


def main():
    d = tempfile.mkdtemp(prefix="rec-mcv-")
    try:
        os.makedirs(os.path.join(d, "seg", "000"))
        vfile = "seg/000/seg_0001_video.rtp"
        with open(os.path.join(d, vfile), "wb") as f:
            f.write(b"\x80" * 64)
        seg = {"seq": 1, "type": "mcvideo", "speaker_id": "+82500000002", "duration_ms": 4000,
               "tracks": [{"prefix": "video", "kind": "video", "slot": 0, "file": vfile,
                           "speakers": [{"id": "+82500000002", "offset_ms": 0, "dur_ms": 4000}]}],
               "has_video": True}
        tracks = recording._seg_tracks(seg)
        check("mcvideo 세그먼트의 tracks[] 를 그대로 읽는다", len(tracks) == 1 and tracks[0]["kind"] == "video")
        # 구 녹취형 flat 키(tracks[] 없음)도 세션 경로로 합성
        flat = {"seq": 2, "type": "mcvideo", "speaker_id": "+82500000003", "duration_ms": 1000,
                "audio_file": "seg/000/seg_0002_audio.rtp", "video_file": "seg/000/seg_0002_video.rtp"}
        ft = recording._seg_tracks(flat)
        check("flat 키 mcvideo 세그먼트 → 슬롯 트랙 합성(오디오·영상 슬롯 0)",
              sorted((t["kind"], t["slot"]) for t in ft) == [("audio", 0), ("video", 0)], str(ft))
        st = recording._segment_status(d, dict(seg, _tracks=tracks))
        check("영상만 있는 송출 구간 — 상태 raw(변환 대상)", st == "raw", st)

        called = {}

        def fake_multi(rec_dir, s, slot, out_path, tmp_out):
            called["multi"] = s.get("type")
            return True

        orig = recording._transcode_ptt_multi
        recording._transcode_ptt_multi = fake_multi
        try:
            recording._transcode_segment_file(d, dict(seg, _tracks=tracks))
        finally:
            recording._transcode_ptt_multi = orig
        check("mcvideo 세그먼트 → 슬롯 트랙 변환 경로", called.get("multi") == "mcvideo", str(called))

        with open(os.path.join(d, "session.json"), "w") as f:
            json.dump({"type": "mcvideo"}, f)
        check("세션 서비스 = session.json type", recording._session_service(d, []) == "mcvideo")
        os.remove(os.path.join(d, "session.json"))
        check("session.json 이 없으면 세그먼트 type", recording._session_service(d, [seg]) == "mcvideo")
        check("둘 다 없으면 ptt", recording._session_service(d, [{"seq": 1}]) == "ptt")
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print(f"\n결과: PASS {_pass} / FAIL {_fail}")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
