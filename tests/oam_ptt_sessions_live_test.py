#!/usr/bin/env python3
"""oam_ptt_sessions_live_test.py — PTT 세션 이력 조회의 진행중 세션 병합 규약 검증.

주장: **진행중 세션은 조회 범위와 무관하게 전량 얹는다.** 어제 시작해 아직 열린 세션 안에서
오늘 통화가 일어나면, 오늘 보기에도 그 세션이 진행중으로 있어야 한다(recording.md 'PTT 세션
이력' 진행중 구역). 시작일이 범위 밖이라 빼면 그 통화는 어느 날짜 보기에도 나오지 않는다.

실행:  python3 tests/oam_ptt_sessions_live_test.py   (실서버·녹취 트리 불필요 — 읽기 모델을 대체한다)
"""

import os
import sys
from datetime import datetime, timedelta

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'ems', 'core', 'oam', 'vendor'))
sys.path.insert(0, os.path.join(_HERE, '..', 'ems', 'core', 'oam', 'src'))
from services import flow_logger, ptt_index  # noqa: E402

_pass = _fail = 0


def check(name, cond, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print(f"  PASS  {name}")
    else:
        _fail += 1
        print(f"  FAIL  {name}  {detail}")


def row(key, gkey, start, state="ended"):
    return {"key": key, "group_key": gkey, "start": start, "state": state, "kind": "group"}


def main():
    today = datetime.now()
    yday = today - timedelta(days=1)
    t8 = today.strftime("%Y%m%d")
    y8 = yday.strftime("%Y%m%d")
    live_key = f"S{y8}210005874407_1"            # 어제 21:00 시작, 아직 열림
    ended_today = f"S{t8}102243599182_1"         # 오늘 시작·종료
    ended_yday = f"S{y8}100849006079_1"          # 어제 시작·종료

    index = {
        t8: [row(ended_today, "2", f"{today:%Y-%m-%d}T10:22:43")],
        y8: [row(ended_yday, "1", f"{yday:%Y-%m-%d}T10:08:49"),
             # 어제 23:59 스위퍼가 'ended' 로 굳혀 둔 같은 세션 — 실시간 쪽으로 갈아 끼워야 한다
             row(live_key, "1", f"{yday:%Y-%m-%d}T21:00:05")],
    }
    live_rows = [row(live_key, "1", f"{yday:%Y-%m-%d}T21:00:05", state="active")]

    ptt_index.day = lambda d, force=False: list(index.get(d, []))

    def range_days(a, b):
        out = []
        d0, d1 = datetime.strptime(a, "%Y%m%d"), datetime.strptime(b, "%Y%m%d")
        for i in range((d1 - d0).days + 1):
            out.extend(index.get((d0 + timedelta(days=i)).strftime("%Y%m%d"), []))
        return out
    ptt_index.range_days = range_days
    ptt_index.live = lambda: [dict(r) for r in live_rows]

    print("[1] 오늘 보기 — 어제 시작한 진행중 세션이 있어야 한다")
    rows = flow_logger._ptt_index_rows(date=today.strftime("%Y-%m-%d"))
    keys = {(r["group_key"], r["key"]): r for r in rows}
    check("오늘 종료 세션 포함", ("2", ended_today) in keys)
    check("어제 시작 진행중 세션 포함", ("1", live_key) in keys, str(sorted(keys)))
    check("진행중 상태로 얹힘", keys.get(("1", live_key), {}).get("state") == "active")
    check("어제 종료 세션은 오늘 보기에 없음", ("1", ended_yday) not in keys)

    print("[2] 어제 보기 — 인덱스의 'ended' 사본이 실시간 행으로 대체돼 한 줄만 있어야 한다")
    rows = flow_logger._ptt_index_rows(date=yday.strftime("%Y-%m-%d"))
    same = [r for r in rows if (r["group_key"], r["key"]) == ("1", live_key)]
    check("같은 세션 1행", len(same) == 1, f"{len(same)}행")
    check("그 행은 active", same and same[0]["state"] == "active")
    check("어제 종료 세션 포함", any(r["key"] == ended_yday for r in rows))

    print("[3] 기간(from/to) 보기 — 범위가 오늘 하루여도 진행중은 얹힌다")
    rows = flow_logger._ptt_index_rows(from_date=today.strftime("%Y-%m-%d"),
                                       to_date=today.strftime("%Y-%m-%d"))
    check("진행중 포함", any(r["key"] == live_key and r["state"] == "active" for r in rows))

    print("[4] 진행중이 없으면 인덱스 그대로")
    ptt_index.live = lambda: []
    rows = flow_logger._ptt_index_rows(date=yday.strftime("%Y-%m-%d"))
    check("어제 2행(ended)", len(rows) == 2 and all(r["state"] == "ended" for r in rows), str(rows))

    print(f"\n결과: PASS {_pass} / FAIL {_fail}")
    return 0 if _fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
