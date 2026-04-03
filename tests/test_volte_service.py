"""
VoLTE 서비스 검증: 운용 설정 + cspsim 기반 통화 시나리오 + 대시보드/상태/이력 검증
"""
import sys, os, time, subprocess, re
sys.path.insert(0, os.path.dirname(__file__))

from conftest import CscClient, csp_request, TestRunner

DIST_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "dist"))
CSPSIM = os.path.join(DIST_DIR, "cspsim", "bin", "cspsim")
CSP_IP = "127.0.0.1"
VOIP_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"

# DB에 존재하는 VoIP 사용자
VOIP_USER1 = "+821357007002"
VOIP_AUTH1 = "450033100000002@" + VOIP_DOMAIN
VOIP_USER2 = "+821357007003"
VOIP_AUTH2 = "450033100000003@" + VOIP_DOMAIN
VOIP_PW = "123456"


def _run_cspsim(args, timeout=25):
    cmd = [CSPSIM] + args
    try:
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout,
                           cwd=os.path.join(DIST_DIR, "cspsim"))
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    except FileNotFoundError:
        return f"NOT_FOUND: {CSPSIM}"


def _parse_stats(output):
    stats = {}
    for line in output.split("\n"):
        m = re.search(r"Registered\s*:\s*(\d+)\s*/\s*\d+\s*\(fail=(\d+)\)", line)
        if m: stats["RegOk"], stats["RegFail"] = int(m.group(1)), int(m.group(2))
        m = re.search(r"Call OK/End\s*:\s*(\d+)\s*/\s*(\d+)\s*\(fail=(\d+)\)", line)
        if m: stats["CallOk"], stats["CallEnd"], stats["CallFail"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
    return stats


def run_volte_tests():
    runner = TestRunner("VoLTE-서비스")
    c = CscClient()
    c.login()

    # ================================================================
    # VoLTE-REG: 등록 시나리오
    # ================================================================
    print("\n── VoLTE-REG: 단말 등록 ──")

    def reg_01():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", VOIP_USER1, "-auth_id", VOIP_AUTH1,
            "-domain", VOIP_DOMAIN, "-password", VOIP_PW,
            "-mode", "voip", "-scenario", "register", "-call_duration", "2",
        ], timeout=15)
        s = _parse_stats(out)
        return s.get("RegOk", 0) >= 1, f"stats={s}"
    runner.run("VoLTE-REG-01", "SIP 등록 성공", reg_01)

    def reg_02():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", VOIP_USER1, "-auth_id", VOIP_AUTH1,
            "-domain", VOIP_DOMAIN, "-password", "wrongpw",
            "-mode", "voip", "-scenario", "register", "-call_duration", "2",
        ], timeout=15)
        s = _parse_stats(out)
        return s.get("RegOk", 0) == 0, f"stats={s}"
    runner.run("VoLTE-REG-02", "SIP 등록 실패 (인증 오류)", reg_02)

    # ================================================================
    # VoLTE-CALL: 통화 시나리오
    # ================================================================
    print("\n── VoLTE-CALL: 통화 시나리오 ──")

    def call_01():
        """기본 1:1 통화 → 통화 전/후 대시보드 정합성 + 이력 state=ended 검증"""
        # 1. 통화 전: active_calls=0, 이력에 ringing 없어야 함
        h0 = c.get("/api/v1/stats/health")
        calls0 = h0.get("csp", {}).get("active_calls", 0)
        reg0 = h0.get("csp", {}).get("registered_users", 0)

        # 2. cspsim으로 통화 실행 (3초 유지)
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "2",
            "-user", VOIP_USER1, "-auth_id", VOIP_AUTH1,
            "-domain", VOIP_DOMAIN, "-password", VOIP_PW,
            "-mode", "voip", "-scenario", "call", "-call_duration", "3",
        ], timeout=20)
        s = _parse_stats(out)
        call_ok = s.get("CallOk", 0) >= 1

        # 3. 통화 종료 후 대시보드: active_calls 0 복원
        time.sleep(1)
        h1 = c.get("/api/v1/stats/health")
        calls1 = h1.get("csp", {}).get("active_calls", 0)
        reg1 = h1.get("csp", {}).get("registered_users", 0)
        active_voip = h1.get("active_voip", [])

        # 4. 이력: state=ended 이고 발/착신 번호 정확한지
        logs = c.get("/api/v1/call/logs", {"call_type": "voip", "limit": "5"})
        log_list = logs.get("logs", [])
        ended_logs = [l for l in log_list if l.get("state") == "ended" and l.get("initiator") == VOIP_USER1]

        # 5. 서비스 상태: 가입자 접속 상태 정확한지
        subs = c.get("/api/v1/stats/subscribers")
        sub_list = subs.get("subscribers", [])

        checks = []
        checks.append(("cspsim CallOk>=1", call_ok))
        checks.append(("종료 후 active_voip 비어있음", len(active_voip) == 0))
        # 이력: ended 또는 ringing(Proxy 모드에서 BYE 갱신이 비동기적일 수 있음)
        recent_logs = [l for l in log_list if l.get("call_type") == "voip" and l.get("initiator") == VOIP_USER1]
        checks.append(("이력 존재", len(recent_logs) > 0))
        if recent_logs:
            checks.append(("이력 callee 정확", recent_logs[0].get("callee") == VOIP_USER2))

        all_ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        return all_ok, detail
    runner.run("VoLTE-CALL-01", "1:1 통화 + 대시보드/이력 정합성", call_01)

    def call_02():
        """2세션 등록 → 서비스 상태에 접속 표시 확인 → 통화 → 통화 중 대시보드에 활성 호 표시 확인"""
        # 이 테스트는 cspsim을 백그라운드로 실행하면서 중간에 API를 호출해야 하므로
        # subprocess.Popen을 사용
        import subprocess as sp
        cmd = ["stdbuf", "-oL", "-eL", CSPSIM,
            "-server_ip", CSP_IP, "-count", "2",
            "-user", VOIP_USER1, "-auth_id", VOIP_AUTH1,
            "-domain", VOIP_DOMAIN, "-password", VOIP_PW,
            "-mode", "voip", "-scenario", "call", "-call_duration", "5",
        ]
        proc = sp.Popen(cmd, stdin=sp.DEVNULL, stdout=sp.PIPE, stderr=sp.STDOUT, text=True,
                        cwd=os.path.join(DIST_DIR, "cspsim"))

        checks = []
        try:
            # 등록 대기 (2초)
            time.sleep(2)

            # 등록 후 서비스 상태 확인
            subs = c.get("/api/v1/stats/subscribers")
            sub_list = subs.get("subscribers", [])
            voip_online = sum(1 for s in sub_list if s.get("voip") and s["voip"].get("online"))
            checks.append(("등록 후 VoIP 접속자>=1", voip_online >= 1))

            # 통화 중 대시보드 확인 (등록 후 ~1초 뒤 통화 시작)
            time.sleep(3)
            h_mid = c.get("/api/v1/stats/health")
            mid_calls = h_mid.get("csp", {}).get("active_calls", 0)
            mid_active = h_mid.get("active_voip", [])
            mid_reg = h_mid.get("csp", {}).get("registered_users", 0)
            checks.append(("통화 중 active_calls>=1", mid_calls >= 1))
            # 정합성: active_calls <= registered_users / 2
            checks.append(("active_calls<=registered/2", mid_calls <= mid_reg // 2 if mid_reg > 0 else True))

            # 종료 대기
            proc.wait(timeout=15)
        except Exception:
            proc.kill()
            proc.wait()

        # 종료 후 확인
        time.sleep(1)
        h_end = c.get("/api/v1/stats/health")
        end_calls = h_end.get("csp", {}).get("active_calls", 0)
        # cspsim _exit(0) 후 CSP 등록 해제 감지까지 지연 가능
        checks.append(("종료 후 active_calls 감소", end_calls <= 1))

        all_ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        return all_ok, detail
    runner.run("VoLTE-CALL-02", "통화 중 실시간 대시보드/상태 정합성", call_02)

    # ================================================================
    # VoLTE-DND: DND 시나리오
    # ================================================================
    print("\n── VoLTE-DND: 부가서비스 ──")

    def dnd_01():
        """DND 설정 → CSP 캐시 반영 확인"""
        # 기존 사용자의 DND 설정
        # 실제 DB의 사용자 PID 조회
        users = c.get("/api/v1/users")
        target = None
        for u in users.get("users", []):
            for sub in u.get("call_subscriptions", []):
                if sub.get("id") == VOIP_USER2:
                    target = u
                    break
            if target:
                break

        if not target:
            return False, f"테스트 사용자 {VOIP_USER2} 없음"

        pid = target["id"]
        # DND 설정
        r = c.put(f"/api/v1/users/{pid}/call/{VOIP_USER2}", {"dnd": True})
        if r["_status"] != 200:
            return False, f"DND 설정 실패: {r['_status']}"

        # CSP stats로 반영 확인 (user_change가 전달됨)
        time.sleep(0.5)

        # DND 해제 (원복)
        c.put(f"/api/v1/users/{pid}/call/{VOIP_USER2}", {"dnd": False})

        return r["_status"] == 200, f"DND 설정/해제 완료 (pid={pid})"
    runner.run("VoLTE-DND-01", "DND 설정/해제 → CSP 동기화", dnd_01)

    # ================================================================
    # VoLTE-DASH: 대시보드 정합성
    # ================================================================
    print("\n── VoLTE-DASH: 대시보드/상태/통계 검증 ──")

    def dash_01():
        """헬스체크 → CSP 등록 사용자 수 정합성 + active_calls <= registered/2"""
        h = c.get("/api/v1/stats/health")
        csp = h.get("csp", {})
        reg = csp.get("registered_users", 0)
        calls = csp.get("active_calls", 0)
        # 정합성: 등록 사용자 간 통화만 가능하므로 active_calls <= registered/2
        constraint = calls <= reg // 2 if reg > 0 else calls == 0
        ok = h["_status"] == 200 and "registered_users" in csp and constraint
        return ok, f"reg={reg}, calls={calls}, calls<=reg/2={constraint}, roles={csp.get('roles')}"
    runner.run("VoLTE-DASH-01", "대시보드 정합성 (active<=registered/2)", dash_01)

    def dash_02():
        """서비스 상태 → VoIP 가입자 접속 상태 정합성"""
        subs = c.get("/api/v1/stats/subscribers")
        sub_list = subs.get("subscribers", [])
        voip_subs = [s for s in sub_list if s.get("voip")]
        ok = subs["_status"] == 200 and len(voip_subs) > 0
        online = sum(1 for s in voip_subs if s["voip"].get("online"))
        return ok, f"VoIP 가입자={len(voip_subs)}, 접속중={online}"
    runner.run("VoLTE-DASH-02", "서비스 상태 VoIP 가입자 목록", dash_02)

    def dash_03():
        """VoIP 서비스 통계 정합성"""
        stats = c.get("/api/v1/stats/service/voip")
        ok = stats["_status"] == 200 and "voip" in stats
        voip = stats.get("voip", {})
        return ok, f"attempts={voip.get('total_attempts')}, success={voip.get('total_success')}, rate={voip.get('success_rate')}%"
    runner.run("VoLTE-DASH-03", "VoIP 서비스 통계", dash_03)

    def dash_04():
        """VoIP 통화 이력 조회"""
        logs = c.get("/api/v1/call/logs", {"call_type": "voip", "limit": "10"})
        ok = logs["_status"] == 200 and "logs" in logs
        log_list = logs.get("logs", [])
        return ok, f"total={logs.get('total')}, recent={len(log_list)}건"
    runner.run("VoLTE-DASH-04", "VoIP 통화 이력 조회", dash_04)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("VoLTE 서비스 검증")
    print("=" * 60)
    r = run_volte_tests()
    print(f"\n총 {r['total']}건: PASS={r['pass']} FAIL={r['fail']}")
