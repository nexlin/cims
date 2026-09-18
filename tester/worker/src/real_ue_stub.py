#!/usr/bin/env python3
"""cimsue-cli drive 프로토콜 스텁 — tester_real_ue_test 가 RealUeProcess 를 시험할 때 실스택 대신 띄운다.
stdin 한 줄 = 명령(공백 토큰), stdout 한 줄 = JSON 이벤트. 명령마다 result 하나 + 상태 이벤트(reg·call·stats·floor)."""
import json
import sys
import time

CALL = 0


def out(d):
    sys.stdout.write(json.dumps(d) + "\n")
    sys.stdout.flush()


def result(op, ok=True, call=-1, code=0, reason=""):
    out({"event": "result", "op": op, "ok": ok, "call": call, "code": code, "reason": reason})


def main():
    global CALL
    argv = sys.argv[1:]
    if "--fail-start" in argv:
        out({"event": "exit", "error": "engine start failed: stub"})
        return 3
    out({"event": "ready", "version": "stub", "aor": "sip:stub@test"})
    stats = {"rx_pkts": 250, "tx_pkts": 250, "rx_loss": 1, "rx_bytes": 8000, "jitter_us": 1500, "stats_valid": True}
    for line in sys.stdin:
        tk = line.split()
        if not tk:
            continue
        op = tk[0]
        if op == "quit":
            result(op)
            break
        if op == "register":
            result(op)
            out({"event": "reg", "state": "registering", "code": 0, "reason": "", "rrd_ms": -1})
            out({"event": "reg", "state": "registered", "code": 200, "reason": "OK", "expires": 3600, "rrd_ms": 12})
        elif op == "unregister":
            result(op)
            out({"event": "reg", "state": "unregistered", "code": 200, "reason": "OK", "rrd_ms": -1})
        elif op == "dial":
            CALL += 1
            result(op, call=CALL)
            out({"event": "call", "call": CALL, "dir": "out", "state": "outgoing", "code": 100, "reason": "Trying", "media": False, "mcptt": False, "video": False, "by_us": False, "group": ""})
            time.sleep(0.05)
            out({"event": "call", "call": CALL, "dir": "out", "state": "active", "code": 200, "reason": "OK", "media": True, "mcptt": False, "video": False, "by_us": False, "group": "", "srd_ms": 48})
            out(dict({"event": "stats", "call": CALL}, **stats))
        elif op == "hangup":
            cid = int(tk[1]) if len(tk) > 1 else -1
            result(op, call=cid)
            out(dict({"event": "call", "call": cid, "dir": "out", "state": "disconnected", "code": 200, "reason": "Normal", "media": False, "mcptt": False,
                      "video": False, "by_us": True, "group": "", "sdd_ms": 9}, **stats))
        elif op == "floor_request":
            cid = int(tk[1]) if len(tk) > 1 else -1
            result(op, call=cid)
            out({"event": "floor", "call": cid, "kind": "granted", "subtype": 1, "t_us": int(time.time() * 1e6), "cause": -1, "queue_position": -1, "duration": 30})
        elif op == "affiliate":
            result(op)
            out({"event": "request", "method": "PUBLISH", "op": "affiliate", "on": tk[2] != "off" if len(tk) > 2 else True, "code": 200, "reason": "OK", "ms": 7, "token": 1})
        elif op == "stats":
            out(dict({"event": "stats", "call": CALL}, **stats))
            result(op)
        elif op == "crash":
            result(op)
            return 9
        else:
            result(op, ok=False, reason="unknown command")
    out({"event": "exit"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
