#!/usr/bin/env python3
"""MCPTT emergency/imminent floor-tier 라이브 검증 도구 (Phase 1 CMP 선점).

CSP 가 긴급 개시 시 CMP 로 보내는 PTT_FLOOR_TIER 와 동일한 명령을 직접 주입한다.
정상 PTT 그룹콜(cspsim 변경 불필요) 위에서 한 멤버의 floor tier 를 emergency 로 올린 뒤
그 멤버가 floor REQUEST(PTT push) 하면, 하위 tier 점유자를 선점(REVOKE→GRANT)하는지
floor.jsonl("reason":"emergency_preempt")·오디오로 확인한다.

사용:
  python3 mcptt_floor_tier_test.py --cmp 10.0.1.48 --port 9000 \
      --group g001 --session +821012345678 --tier emergency
  (취소: --tier normal)

session = CMP 멤버 식별자 = 그 멤버의 PTT MSISDN(PTT_JOIN 의 session_id).
"""
import argparse
import json
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmp", required=True, help="CMP control IP (예: media01 내부망)")
    ap.add_argument("--port", type=int, default=9000, help="CMP control UDP 포트 (기본 9000)")
    ap.add_argument("--group", required=True, help="mcptt_group_id (예: g001)")
    ap.add_argument("--session", required=True, help="멤버 session_id = PTT MSISDN")
    ap.add_argument("--tier", default="emergency",
                    choices=["emergency", "imminent", "normal"], help="설정할 floor tier")
    ap.add_argument("--timeout", type=float, default=2.0)
    args = ap.parse_args()

    pkt = {
        "hdr": {
            "ver": 2,
            "trans_id": int(time.time()) % 100000,
            "node": "script",
            "cmd": "PTT_FLOOR_TIER",
            "type": "request",
            "service": "mcptt",
        },
        "payload": {
            "group_id": args.group,
            "session_id": args.session,
            "tier": args.tier,
        },
    }
    data = json.dumps(pkt).encode()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(args.timeout)
    s.sendto(data, (args.cmp, args.port))
    print(f"→ {args.cmp}:{args.port}  {json.dumps(pkt['payload'], ensure_ascii=False)}")
    try:
        resp, _ = s.recvfrom(4096)
        print(f"← {resp.decode(errors='replace')}")
    except socket.timeout:
        print("← (응답 없음 — CMP 도달/포트/방화벽 확인)", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
