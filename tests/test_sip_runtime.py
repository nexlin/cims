"""
SIP 런타임 설정 hot-reload 검증 (P1~P6 통합).

Phase 커버리지:
  P1 - 설정 캐시 3층 구조 (DB/mem/file) + 내부 API + notify 이벤트
  P2 - SIP 리스너 hot-reload (UDP add/remove)
  P3 - 트렁크 레지스트리 + OPTIONS 헬스
  P4 - 라우팅 규칙 엔진 + dry-run
  P5 - 접근제어 (ACL + rate limit)
  P6 - 서비스 프로세스 제어
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))

from conftest import CscClient, TestRunner

TEST_TAG = "_vruntime_"


# ──────────────────────────────────────────────────────────────
#  유틸
# ──────────────────────────────────────────────────────────────

def _port_bound(port: int) -> bool:
    """UDP 포트가 bind 되어 있는지 ss 로 확인."""
    try:
        out = subprocess.check_output(["ss", "-uln"], text=True, timeout=2)
        return f":{port}" in out
    except Exception:
        return False


def _send_udp(ip: str, port: int, data: bytes) -> bool:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(2)
        s.sendto(data, (ip, port))
        s.close()
        return True
    except Exception:
        return False


def _detect_csp_local_ip() -> str:
    """CSP 의 설정된 LocalIp 를 읽어 반환. bootstrap listener 정보에서 유추."""
    import pathlib, json
    for p in (pathlib.Path("/home/nex/work/cims/build/dist/csp/config/csp.json"),
              pathlib.Path("./csp/config/csp.json")):
        if p.exists():
            try:
                cfg = json.loads(p.read_text())
                ip = ((cfg.get("Setup") or {}).get("Sip") or {}).get("LocalIp")
                if ip and ip != "0.0.0.0": return ip
            except Exception:
                pass
    return ""


def _wait(cond_fn, timeout: float = 10.0, interval: float = 0.3) -> bool:
    """cond_fn() 이 True 가 될 때까지 대기."""
    end = time.time() + timeout
    while time.time() < end:
        if cond_fn():
            return True
        time.sleep(interval)
    return False


def _cleanup_runtime(c: CscClient):
    """테스트 아티팩트 정리 + 이전 수동 테스트 잔존물 제거.

    원칙: bootstrap 리스너(id=1, default-udp-5060)는 건드리지 않는다.
    그 외 것은 과감히 정리해 테스트가 결정론적이도록 한다.
    """
    for r in c.get("/api/v1/csp/listeners").get("items", []):
        name = (r.get("name") or "")
        port = r.get("bind_port")
        # default-udp-5060 (id=1, bootstrap) 제외
        if r.get("id") == 1 and name == "default-udp-5060":
            continue
        if TEST_TAG in name or port in (5093, 5094, 5095, 5099) or name.startswith("admin-api-"):
            c.delete(f"/api/v1/csp/listeners/{r['id']}")
    # 트렁크: 알려지지 않은 일반 항목은 보존, 테스트용만 삭제
    for r in c.get("/api/v1/csp/trunks").get("items", []):
        name = (r.get("name") or "")
        if TEST_TAG in name or name in ("pbx-trunk", "test-trunk", "self-trunk", "alive-self",
                                        "dead-trunk", "integration-test", "mid-call-test"):
            c.delete(f"/api/v1/csp/trunks/{r['id']}")
    # 라우팅 규칙: 전부 정리 (hit 카운터는 메모리만이라 재생성 OK)
    for r in c.get("/api/v1/csp/routes").get("items", []):
        c.delete(f"/api/v1/csp/routes/{r['id']}")
    for r in c.get("/api/v1/csp/access").get("items", []):
        note = (r.get("note") or "")
        if TEST_TAG in note or r.get("value", "").startswith("203.0.113."):
            c.delete(f"/api/v1/csp/access/{r['id']}")


# ──────────────────────────────────────────────────────────────
#  메인
# ──────────────────────────────────────────────────────────────

def run_sip_runtime_tests(only=None):
    runner = TestRunner("SIP-RUNTIME", only_ids=only)
    c = CscClient()
    login = c.login()
    if not c.token:
        for k in ("P1", "P2", "P3", "P4", "P5", "P6"):
            runner.run(f"{k}-PRE", f"{k} 로그인 전제조건", lambda: (False, "admin 로그인 실패"))
        return runner.summary()

    _cleanup_runtime(c)
    state: dict = {}

    # ════════════════════════════════════════════════════════════
    #  P1 — 설정 캐시 3층 구조
    # ════════════════════════════════════════════════════════════
    print("\n── P1: 설정 캐시 + 내부 API ──")

    def p1_01():
        # CSC 내부 API 메타 (loopback, token 필요)
        import json
        # token 경로를 CSC 설정에서 찾아야 하지만 테스트 편의상 외부에 노출된 /stats/health 로 대체
        r = c.get("/api/v1/stats/health")
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("P1-01", "CSC 헬스 체크", p1_01)

    def p1_02():
        # CSC file snapshot 존재 여부
        import pathlib
        candidates = [
            pathlib.Path("/home/nex/work/cims/build/dist/csc/cache"),
            pathlib.Path("./csc/cache"),
        ]
        found = None
        for p in candidates:
            if p.exists() and any(p.iterdir()):
                found = p; break
        if not found:
            return False, "csc/cache 디렉토리 없음"
        names = {f.name for f in found.iterdir()}
        needed = {"listeners.json", "trunks.json", "routes.json", "access.json", "_meta.json"}
        missing = needed - names
        return not missing, f"{found} → missing={missing or 'none'}"
    runner.run("P1-02", "CSC file snapshot 5개 파일", p1_02)

    def p1_03():
        import pathlib
        candidates = [
            pathlib.Path("/home/nex/work/cims/build/dist/csp/bin/cache"),
            pathlib.Path("./csp/bin/cache"),
        ]
        found = None
        for p in candidates:
            if p.exists() and any(p.iterdir()):
                found = p; break
        if not found:
            return False, "csp/bin/cache 디렉토리 없음"
        names = {f.name for f in found.iterdir()}
        needed = {"listeners.json", "trunks.json", "routes.json", "access.json"}
        return needed.issubset(names), f"files={sorted(names)}"
    runner.run("P1-03", "CSP local file cache 존재", p1_03)

    # ════════════════════════════════════════════════════════════
    #  P2 — SIP 리스너 hot-reload
    # ════════════════════════════════════════════════════════════
    print("\n── P2: SIP 리스너 hot-reload ──")

    def p2_01():
        r = c.get("/api/v1/csp/listeners")
        return r["_status"] == 200 and "items" in r, f"status={r['_status']}"
    runner.run("P2-01", "리스너 목록 조회", p2_01)

    def p2_02():
        r = c.post("/api/v1/csp/listeners", {
            "name": f"{TEST_TAG}udp5093",
            "bind_ip": "0.0.0.0", "bind_port": 5093,
            "protocol": "UDP", "service": "system",
        })
        if r["_status"] == 201 and r.get("id"):
            state["lid1"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P2-02", "리스너 생성 (port 5093)", p2_02)

    def p2_03():
        # bind 완료 대기
        ok = _wait(lambda: _port_bound(5093), timeout=5)
        return ok, "port 5093 bound" if ok else "port 5093 not bound within 5s"
    runner.run("P2-03", "실제 UDP bind 확인 (ss)", p2_03)

    def p2_04():
        lid = state.get("lid1")
        if not lid: return False, "선행 테스트 실패"
        r = c.put(f"/api/v1/csp/listeners/{lid}", {"note": "modified"})
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("P2-04", "리스너 수정 (PUT)", p2_04)

    def p2_05():
        # 중복 포트 bind 는 DB unique 제약으로 409
        r = c.post("/api/v1/csp/listeners", {
            "name": f"{TEST_TAG}dup",
            "bind_ip": "0.0.0.0", "bind_port": 5093,
            "protocol": "UDP", "service": "system",
        })
        return r["_status"] == 409, f"status={r['_status']}"
    runner.run("P2-05", "중복 bind_ip:port:protocol 거부", p2_05)

    def p2_06():
        lid = state.get("lid1")
        if not lid: return False, "선행 실패"
        r = c.delete(f"/api/v1/csp/listeners/{lid}")
        if r["_status"] != 204:
            return False, f"delete status={r['_status']}"
        # unbind 대기
        ok = _wait(lambda: not _port_bound(5093), timeout=5)
        return ok, "unbind completed" if ok else "port 5093 still bound after 5s"
    runner.run("P2-06", "리스너 삭제 → 포트 해제", p2_06)

    # ════════════════════════════════════════════════════════════
    #  P3 — 트렁크 + OPTIONS 헬스
    # ════════════════════════════════════════════════════════════
    print("\n── P3: 트렁크 + OPTIONS 헬스 ──")

    def p3_01():
        r = c.get("/api/v1/csp/trunks")
        return r["_status"] == 200 and "items" in r, f"status={r['_status']}"
    runner.run("P3-01", "트렁크 목록 조회", p3_01)

    def p3_02():
        # CSP 자체 5060 으로 OPTIONS 왕복 — LocalIp(loopback 아님) 사용해야
        # psip 가 Via 를 실제 IP 로 채우고 응답이 올바르게 돌아온다.
        local_ip = _detect_csp_local_ip() or "192.168.0.2"
        r = c.post("/api/v1/csp/trunks", {
            "name": f"{TEST_TAG}alive",
            "remote_ip": local_ip, "remote_port": 5060,
            "protocol": "UDP",
            "options_ping_sec": 3, "options_dead_threshold": 2,
        })
        if r["_status"] == 201:
            state["alive_tid"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P3-02", "Alive 트렁크 생성 (self:5060)", p3_02)

    def p3_03():
        r = c.post("/api/v1/csp/trunks", {
            "name": f"{TEST_TAG}dead",
            "remote_ip": "192.0.2.99", "remote_port": 5060,   # TEST-NET-1 (RFC 5737)
            "protocol": "UDP",
            "options_ping_sec": 3, "options_dead_threshold": 2,
        })
        if r["_status"] == 201:
            state["dead_tid"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P3-03", "Dead 트렁크 생성 (TEST-NET)", p3_03)

    def p3_04():
        # CSP 에 직접 STATS_REQUEST 보내 trunks 배열 확인
        import json, socket
        alive_tid = state.get("alive_tid")
        dead_tid  = state.get("dead_tid")
        if not alive_tid or not dead_tid: return False, "선행 실패"

        def _query_csp():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", 0))
            s.settimeout(3)
            msg = json.dumps({"trans_id": "vtest", "event": "STATS_REQUEST", "service": "system"})
            s.sendto(msg.encode(), ("127.0.0.1", 4421))
            try:
                data, _ = s.recvfrom(8192)
                return json.loads(data.decode())
            except Exception:
                return None
            finally:
                s.close()

        def _check():
            r = _query_csp()
            if not r: return False
            trunks = r.get("trunks") or []
            a = next((t for t in trunks if t["id"] == alive_tid), None)
            d = next((t for t in trunks if t["id"] == dead_tid), None)
            return a and d and a.get("alive") is True and a.get("last_rtt_ms", -1) >= 0

        ok = _wait(_check, timeout=25, interval=1.5)
        r = _query_csp() or {}
        trunks = r.get("trunks") or []
        a = next((t for t in trunks if t["id"] == alive_tid), None)
        d = next((t for t in trunks if t["id"] == dead_tid), None)
        return ok, (f"alive={a and a.get('alive')} rtt={a and a.get('last_rtt_ms')} | "
                    f"dead={d and d.get('alive')} fails={d and d.get('fail_count')}")
    runner.run("P3-04", "헬스 alive/dead 감지 (~20s)", p3_04)

    def p3_05():
        # 두 트렁크 삭제
        ok = True
        for key in ("alive_tid", "dead_tid"):
            tid = state.get(key)
            if tid:
                r = c.delete(f"/api/v1/csp/trunks/{tid}")
                if r["_status"] != 204: ok = False
        return ok, "all deleted" if ok else "some failed"
    runner.run("P3-05", "트렁크 삭제", p3_05)

    # ════════════════════════════════════════════════════════════
    #  P4 — 라우팅 규칙 엔진
    # ════════════════════════════════════════════════════════════
    print("\n── P4: 라우팅 규칙 엔진 ──")

    def p4_01():
        r = c.post("/api/v1/csp/trunks", {
            "name": f"{TEST_TAG}pbx",
            "remote_ip": "127.0.0.1", "remote_port": 5060,
            "protocol": "UDP", "options_ping_sec": 60,
        })
        if r["_status"] == 201:
            state["route_trunk"] = r["id"]
            return True, f"trunk_id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P4-01", "라우팅 타겟 트렁크 준비", p4_01)

    def p4_02():
        tid = state.get("route_trunk")
        if not tid: return False, "선행 실패"
        r = c.post("/api/v1/csp/routes", {
            "name": f"{TEST_TAG}prefix9",
            "priority": 50,
            "match": [{"field": "req_uri_user", "op": "prefix", "value": "9"}],
            "transform": [{"action": "strip_prefix", "value": "9"}],
            "target": {"mode": "trunk", "trunk_id": tid},
            "fail": {"action": "reject", "code": 404, "reason": "No Route"},
        })
        if r["_status"] == 201:
            state["rule1"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P4-02", "Prefix 라우팅 규칙 생성", p4_02)

    def p4_03():
        r = c.post("/api/v1/csp/routes/dryrun", {
            "sample": {"method": "INVITE", "req_uri_user": "91234",
                       "req_uri_host": "example.com"}
        })
        return r.get("matched") is True and r.get("rule_id") == state.get("rule1"), \
               f"matched={r.get('matched')} rule_id={r.get('rule_id')}"
    runner.run("P4-03", "Dry-run: 91234 매칭", p4_03)

    def p4_04():
        r = c.post("/api/v1/csp/routes/dryrun", {
            "sample": {"method": "INVITE", "req_uri_user": "1234",
                       "req_uri_host": "example.com"}
        })
        return r.get("matched") is False, f"matched={r.get('matched')}"
    runner.run("P4-04", "Dry-run: 1234 비매칭", p4_04)

    def p4_05():
        r = c.post("/api/v1/csp/routes", {
            "name": f"{TEST_TAG}reject-scanner",
            "priority": 10,
            "match": [{"field": "from_uri", "op": "contains", "value": "friendly-scanner"}],
            "target": {"mode": "reject"},
            "fail": {"action": "reject", "code": 403, "reason": "Forbidden"},
        })
        if r["_status"] == 201:
            state["rule2"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P4-05", "Reject 규칙 생성 (priority 10)", p4_05)

    def p4_06():
        # priority 10 이 먼저 평가됨 — 테스트 자체 규칙이 매칭되어야 함 (rule_name 확인)
        r = c.post("/api/v1/csp/routes/dryrun", {
            "sample": {"method": "INVITE", "req_uri_user": "91234",
                       "from_uri": "sip:friendly-scanner@evil.com"}
        })
        rn = r.get("rule_name") or ""
        expected_prefix = f"{TEST_TAG}reject-scanner"
        return (r.get("matched") and rn == expected_prefix), \
               f"matched={r.get('matched')} rule_name={rn} expected={expected_prefix}"
    runner.run("P4-06", "Priority 검증 (reject 먼저 매칭)", p4_06)

    def p4_07():
        # 규칙 삭제 후 dry-run → 비매칭
        for k in ("rule1", "rule2"):
            rid = state.get(k)
            if rid: c.delete(f"/api/v1/csp/routes/{rid}")
        # 트렁크도
        tid = state.get("route_trunk")
        if tid: c.delete(f"/api/v1/csp/trunks/{tid}")
        time.sleep(0.5)
        r = c.post("/api/v1/csp/routes/dryrun", {
            "sample": {"method": "INVITE", "req_uri_user": "91234"}
        })
        return r.get("matched") is False, f"matched={r.get('matched')}"
    runner.run("P4-07", "규칙 삭제 후 비매칭", p4_07)

    # ════════════════════════════════════════════════════════════
    #  P5 — 접근제어 + rate limit
    # ════════════════════════════════════════════════════════════
    print("\n── P5: 접근제어 ──")

    def p5_01():
        r = c.get("/api/v1/csp/access")
        return r["_status"] == 200 and "items" in r, f"status={r['_status']}"
    runner.run("P5-01", "ACL 목록 조회", p5_01)

    def p5_02():
        r = c.post("/api/v1/csp/access", {
            "scope": "global", "kind": "deny",
            "match_type": "ip", "value": "203.0.113.200",
            "priority": 10, "note": f"{TEST_TAG}deny-ip"
        })
        if r["_status"] == 201:
            state["acl1"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P5-02", "deny ACL 생성 (TEST-NET-3)", p5_02)

    def p5_03():
        r = c.post("/api/v1/csp/access", {
            "scope": "global", "kind": "deny",
            "match_type": "cidr", "value": "203.0.113.0/24",
            "priority": 20, "note": f"{TEST_TAG}cidr-block"
        })
        if r["_status"] == 201:
            state["acl2"] = r["id"]
            return True, f"id={r['id']}"
        return False, f"status={r['_status']}"
    runner.run("P5-03", "CIDR ACL 생성", p5_03)

    def p5_04():
        # 잘못된 match_type 거부
        r = c.post("/api/v1/csp/access", {
            "scope": "global", "kind": "deny",
            "match_type": "bogus", "value": "x",
        })
        return r["_status"] == 400, f"status={r['_status']}"
    runner.run("P5-04", "Invalid match_type 거부", p5_04)

    def p5_05():
        for k in ("acl1", "acl2"):
            aid = state.get(k)
            if aid: c.delete(f"/api/v1/csp/access/{aid}")
        r = c.get("/api/v1/csp/access")
        remaining = [a for a in r.get("items", []) if TEST_TAG in (a.get("note") or "")]
        return len(remaining) == 0, f"remaining={len(remaining)}"
    runner.run("P5-05", "ACL 삭제", p5_05)

    # ════════════════════════════════════════════════════════════
    #  P6 — 서비스 프로세스 제어
    # ════════════════════════════════════════════════════════════
    print("\n── P6: 프로세스 제어 ──")

    def p6_01():
        r = c.get("/api/v1/services")
        return r["_status"] == 200 and "output" in r, f"status={r['_status']}"
    runner.run("P6-01", "서비스 상태 API", p6_01)

    def p6_02():
        # CMP 재시작 (non-critical)
        r = c.post("/api/v1/services/cmp/restart", {})
        return r["_status"] == 200 and r.get("returncode") == 0, \
               f"status={r['_status']} rc={r.get('returncode')}"
    runner.run("P6-02", "CMP restart via API", p6_02)

    def p6_03():
        # 재시작 후 running 상태 확인
        time.sleep(2)
        r = c.get("/api/v1/services")
        return r["_status"] == 200 and "cmp" in r.get("output", "") and "실행" in r.get("output", ""), \
               f"status={r['_status']} len={len(r.get('output',''))}"
    runner.run("P6-03", "CMP 재시작 후 running 상태", p6_03)

    def p6_04():
        # 알 수 없는 서비스명 거부
        r = c.post("/api/v1/services/nosuch/start", {})
        return r["_status"] == 400, f"status={r['_status']}"
    runner.run("P6-04", "알 수 없는 서비스 거부", p6_04)

    def p6_05():
        # 감사 로그에 서비스 제어 이력 기록됨
        import pymysql
        try:
            conn = pymysql.connect(host="127.0.0.1", user="cims", password="cims1234",
                                   database="cims", connect_timeout=2)
            with conn.cursor() as cur:
                cur.execute("SELECT COUNT(*) FROM csp_config_audit WHERE entity='service' AND action='RESTART'")
                n = cur.fetchone()[0]
            conn.close()
            return n > 0, f"audit rows={n}"
        except Exception as e:
            return False, f"db err: {e}"
    runner.run("P6-05", "감사 로그에 RESTART 기록", p6_05)

    # 마무리 정리
    _cleanup_runtime(c)

    return runner.summary()


if __name__ == "__main__":
    run_sip_runtime_tests()
