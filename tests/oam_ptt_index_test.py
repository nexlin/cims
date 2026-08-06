#!/usr/bin/env python3
"""oam_ptt_index_test.py — PTT 세션 인덱스(services.ptt_index) 검증.

핵심 주장 두 가지를 건다.
  ① **인덱스 = 스캔.** 인덱스로 읽은 세션 목록이 녹취를 직접 훑은 결과와 같아야 한다.
     인덱스는 파생물이므로 다르면 인덱스가 틀린 것이다.
  ② **세션이 기록 단위다.** 같은 시간대의 두 세션은 두 줄, 시간을 넘긴 한 세션은 한 줄.

실행:  python3 tests/oam_ptt_index_test.py
       (실서버 불필요 — tmp 에 녹취 트리를 만들어 돌린다)
"""

import json
import os
import shutil
import sys
import tempfile
from datetime import datetime, timedelta

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'ems', 'core', 'oam', 'src'))
from services import ptt_index  # noqa: E402

_pass = _fail = 0


def check(name, cond, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print(f"  PASS  {name}")
    else:
        _fail += 1
        print(f"  FAIL  {name}{('  — ' + detail) if detail else ''}")


# ── 녹취 트리 만들기 ──────────────────────────────────────────

def seg(seq, start, end, speaker, dur_ms, slot=0):
    return {"seq": seq, "type": "ptt", "speaker_id": speaker,
            "start_time": start, "end_time": end, "duration_ms": dur_ms,
            "tracks": [{"prefix": "audio", "kind": "audio", "slot": slot,
                        "file": f"seg/000/seg_{seq:04d}_audio.rtp", "pt": 96,
                        "codec": "AMR-WB/16000",
                        "speakers": [{"id": speaker, "offset_ms": 0, "dur_ms": dur_ms}]}]}


def write_session(root, gkey, window, ses_key, segs, session_json=None):
    """{root}/ptt/{gkey}/{Y}/{M}/{D}/{H}/{ses_key}/ 에 세션 산출물을 만든다."""
    y, m, d, h = window[0:4], window[4:6], window[6:8], window[8:10]
    d_dir = os.path.join(root, "ptt", gkey, y, m, d, h)
    if ses_key:
        d_dir = os.path.join(d_dir, ses_key)
    os.makedirs(os.path.join(d_dir, "seg", "000"), exist_ok=True)
    with open(os.path.join(d_dir, "segments.jsonl"), "w") as f:
        for s in segs:
            f.write(json.dumps(s) + "\n")
    if session_json is not None:
        with open(os.path.join(d_dir, "session.json"), "w") as f:
            json.dump(session_json, f)
    return d_dir


def write_group(root, gkey, desc):
    base = os.path.join(root, "ptt", gkey)
    os.makedirs(base, exist_ok=True)
    with open(os.path.join(base, "group.json"), "w") as f:
        json.dump(desc, f)


def build_tree(root, day):
    """그룹 1개에 세션 4건:
         ① 13시 두 건 (같은 시간대 — 종전 구조라면 한 줄로 뭉쳤다)
         ② 13→14 시 경계를 넘긴 한 건
         ③ 12시 구 녹취 (세션 디렉터리 없음)
    """
    y, m, d = day[0:4], day[4:6], day[6:8]
    write_group(root, "1", {
        "id": 1, "mcptt_group_id": "g001", "name": "음성그룹1",
        "group_type": "prearranged", "floor_control": "on", "floor_policy": "single",
        "max_talkers": 1, "video_enabled": False, "member_count": 3,
        "members": [{"user_id": "+82500000001"}, {"user_id": "+82500000002"},
                    {"user_id": "+82500000003"}],
    })

    sj = lambda ini, ts: {                                        # noqa: E731
        "id": 1, "mcptt_group_id": "g001", "name": "음성그룹1",
        "group_type": "prearranged", "floor_control": "on", "floor_policy": "single",
        "max_talkers": 1, "state": "ended", "sesid": f"g001::csp::{ts}::1",
        "initiator": ini, "call_id": f"call-{ts}", "start_time": f"{y}-{m}-{d}T13:00:00",
        "end_time": f"{y}-{m}-{d}T13:00:30",
    }

    # ① 같은 13시 버킷에 두 세션
    k1 = f"S{day}131000000000_1"
    write_session(root, "1", day + "13", k1,
                  [seg(1, f"{y}-{m}-{d}T13:10:00", f"{y}-{m}-{d}T13:10:05", "+82500000001", 5000),
                   seg(2, f"{y}-{m}-{d}T13:10:06", f"{y}-{m}-{d}T13:10:12", "+82500000002", 6000)],
                  sj("+82500000001", f"{day}131000000000"))
    k2 = f"S{day}134000000000_1"
    write_session(root, "1", day + "13", k2,
                  [seg(1, f"{y}-{m}-{d}T13:40:00", f"{y}-{m}-{d}T13:40:04", "+82500000003", 4000)],
                  sj("+82500000003", f"{day}134000000000"))

    # ② 13→14 경계를 넘긴 세션 — seq 는 세션 단위 단조증가라 버킷을 넘어도 이어진다
    k3 = f"S{day}135800000000_1"
    write_session(root, "1", day + "13", k3,
                  [seg(1, f"{y}-{m}-{d}T13:58:00", f"{y}-{m}-{d}T13:59:00", "+82500000001", 60000)],
                  sj("+82500000001", f"{day}135800000000"))
    write_session(root, "1", day + "14", k3,
                  [seg(2, f"{y}-{m}-{d}T14:00:00", f"{y}-{m}-{d}T14:00:30", "+82500000002", 30000)])

    # ③ 구 녹취 — 세션 디렉터리 없이 버킷에 직접
    write_session(root, "1", day + "12", None,
                  [seg(1, f"{y}-{m}-{d}T12:05:00", f"{y}-{m}-{d}T12:05:09", "+82500000001", 9000)])
    return k1, k2, k3


def main():
    root = tempfile.mkdtemp(prefix="ptt-index-test-")
    try:
        day = (datetime.now() - timedelta(days=2)).strftime("%Y%m%d")   # '지난 날짜' 경로
        k1, k2, k3 = build_tree(root, day)
        ptt_index.init(root, enabled=True)

        print("\n[1] 세션이 기록 단위인가")
        rows = ptt_index.scan_day(day)
        keys = [r["key"] for r in rows]
        check("세션 4건 (같은 시간대 2건이 각각, 경계 통과 1건, 구 녹취 1건)",
              len(rows) == 4, f"got {len(rows)}: {keys}")
        check("같은 13시 버킷의 두 세션이 각각 한 줄",
              k1 in keys and k2 in keys, str(keys))
        r3 = next((r for r in rows if r["key"] == k3), None)
        check("시간을 넘긴 세션은 한 줄이고 버킷이 둘",
              r3 is not None and r3["windows"] == [day + "13", day + "14"],
              str(r3 and r3["windows"]))
        check("경계 통과 세션의 지표가 두 버킷 합계",
              r3 is not None and r3["segments"] == 2 and r3["speech_ms"] == 90000,
              str(r3 and (r3["segments"], r3["speech_ms"])))
        legacy = next((r for r in rows if r["key"] == day + "12"), None)
        check("구 녹취는 버킷 자체가 세션 1건 (세션키 = YYYYMMDDHH)",
              legacy is not None and legacy["legacy"] is True)

        print("\n[2] 인덱스 = 스캔 (인덱스는 파생물이다)")
        scanned = ptt_index.scan_day(day)
        first = ptt_index.day(day)          # 파일 생성
        path = os.path.join(root, "ptt", "index", f"{day}.jsonl")
        check("인덱스 파일 생성", os.path.exists(path))
        second = ptt_index.day(day)         # 파일에서 읽기 (캐시 비우고)
        ptt_index.init(root, enabled=True)  # 캐시 초기화 — 파일 경로를 강제
        third = ptt_index.day(day)
        check("최초 조회 결과 == 스캔", first == scanned)
        check("재조회(캐시) 결과 == 스캔", second == scanned)
        check("재기동 후 파일에서 읽은 결과 == 스캔", third == scanned,
              "인덱스 파일 왕복에서 값이 변형됐다")

        print("\n[3] 그룹 요약 — 전 버킷 glob 없이")
        check("그룹 키 목록에 index 디렉터리가 섞이지 않는다",
              ptt_index.group_keys() == ["1"], str(ptt_index.group_keys()))
        check("last_window = 가장 최근 버킷",
              ptt_index.last_window("1") == day + "14", ptt_index.last_window("1"))
        gd = ptt_index.group_descriptor("1")
        check("group.json 에서 분류·이름", gd["kind"] == "group" and gd["name"] == "음성그룹1")

        print("\n[4] 재생성 — 인덱스를 지워도 복구된다")
        os.remove(path)
        again = ptt_index.day(day)
        check("파일 삭제 후 재조회 = 스캔", again == scanned)
        check("파일 재생성", os.path.exists(path))

        print("\n[5] 기간 조회")
        rng = ptt_index.range_days(day, day)
        check("range_days(단일 일자) == day()", rng == scanned)
        check("빈 날짜는 빈 목록", ptt_index.day("19990101") == [])

    finally:
        shutil.rmtree(root, ignore_errors=True)

    print(f"\n{'=' * 52}\n  PASS {_pass} / FAIL {_fail}\n{'=' * 52}")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
