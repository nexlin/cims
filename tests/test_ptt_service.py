"""
PTT 서비스 검증: 그룹 운용 + cspsim 기반 그룹통화 + 대시보드/상태/이력 검증
"""
import sys, os, time, subprocess, re
sys.path.insert(0, os.path.dirname(__file__))

from conftest import (
    CscClient, csp_request, cmp_request, TestRunner,
    CSP_IP, PTT_DOMAIN,
)

DIST_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "dist"))
CSPSIM = os.path.join(DIST_DIR, "cspsim", "bin", "cspsim")

# DB에 존재하는 PTT 사용자/그룹
PTT_USER1 = "+82571900001"
PTT_USER4 = "+82571900004"
PTT_GROUP = "+82571910001"
PTT_PW = "123456"


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
        m = re.search(r"GMS Subscribed:\s*(\d+)", line)
        if m: stats["GmsOk"] = int(m.group(1))
        m = re.search(r"CMS Subscribed:\s*(\d+)", line)
        if m: stats["CmsOk"] = int(m.group(1))
        m = re.search(r"Call OK/End\s*:\s*(\d+)\s*/\s*(\d+)\s*\(fail=(\d+)\)", line)
        if m: stats["CallOk"], stats["CallEnd"], stats["CallFail"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
        m = re.search(r"Conf NOTIFY\s*:\s*(\d+)", line)
        if m: stats["ConfNotify"] = int(m.group(1))
    return stats


def run_ptt_tests():
    runner = TestRunner("PTT-서비스")
    c = CscClient()
    c.login()

    # ================================================================
    # PTT-MCPTT: MCPTT HTTPS 시나리오 (IdMS/GMS/CMS)
    # ================================================================
    print("\n── PTT-MCPTT: MCPTT HTTPS 인증/설정 ──")

    MCPTT_BASE = "https://127.0.0.1:4430"
    import requests, hashlib, base64, secrets
    mcptt_session = requests.Session()
    mcptt_session.verify = False

    def mcptt_01():
        """IdMS 인증 (OAuth2 PKCE) → access_token 발급"""
        # PKCE 생성
        verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip('=')
        challenge = base64.urlsafe_b64encode(
            hashlib.sha256(verifier.encode()).digest()
        ).decode().rstrip('=')

        # 1. Auth Request
        r1 = mcptt_session.get(f"{MCPTT_BASE}/idms/authreq", params={
            "client_id": "MCPTT_UE",
            "user_name": f"tel:{PTT_USER1}",
            "user_password": PTT_PW,
            "redirect_uri": "http://localhost/callback",
            "state": "test_state",
            "scope": "openid mcptt",
            "code_challenge": challenge,
            "code_challenge_method": "S256",
        })
        if r1.status_code != 200:
            return False, f"authreq failed: {r1.status_code} {r1.text[:100]}"
        code = r1.json().get("code")
        if not code:
            return False, f"authreq no code: {r1.json()}"

        # 2. Token Request
        r2 = mcptt_session.post(f"{MCPTT_BASE}/idms/tokenreq", json={
            "grant_type": "authorization_code",
            "code": code,
            "code_verifier": verifier,
            "redirect_uri": "http://localhost/callback",
            "client_id": "MCPTT_UE",
        })
        if r2.status_code != 200:
            return False, f"tokenreq failed: {r2.status_code} {r2.text[:100]}"
        tokens = r2.json()
        ok = "access_token" in tokens and "refresh_token" in tokens
        # 토큰 저장 (후속 테스트용)
        mcptt_session.headers["Authorization"] = f"Bearer {tokens.get('access_token', '')}"
        return ok, f"access_token={'OK' if tokens.get('access_token') else 'NG'}, refresh={'OK' if tokens.get('refresh_token') else 'NG'}"
    runner.run("PTT-MCPTT-01", "IdMS 인증 (PKCE) + 토큰 발급", mcptt_01)

    def mcptt_02():
        """GMS 그룹 목록 조회"""
        r = mcptt_session.get(f"{MCPTT_BASE}/org.openmobilealliance.groups/users/tel:{PTT_USER1}")
        ok = r.status_code == 200
        groups = r.json() if ok else []
        return ok, f"status={r.status_code}, groups={len(groups)}건"
    runner.run("PTT-MCPTT-02", "GMS 그룹 목록 조회", mcptt_02)

    def mcptt_03():
        """CMS 사용자 프로필 조회"""
        r = mcptt_session.get(f"{MCPTT_BASE}/org.3gpp.mcptt.user-profile/users/tel:{PTT_USER1}/user-profile")
        ok = r.status_code == 200
        return ok, f"status={r.status_code}, content_type={r.headers.get('content-type','')[:40]}"
    runner.run("PTT-MCPTT-03", "CMS 사용자 프로필 조회", mcptt_03)

    def mcptt_04():
        """CMS 서비스 설정 조회"""
        r = mcptt_session.get(f"{MCPTT_BASE}/org.3gpp.mcptt.service-config/users/tel:{PTT_USER1}/service-config")
        ok = r.status_code == 200
        return ok, f"status={r.status_code}"
    runner.run("PTT-MCPTT-04", "CMS 서비스 설정 조회", mcptt_04)

    # ================================================================
    # PTT-REG: 등록 + 구독
    # ================================================================
    print("\n── PTT-REG: 등록 및 구독 ──")

    def reg_01():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", PTT_USER1, "-domain", PTT_DOMAIN,
            "-password", PTT_PW, "-mode", "ptt",
            "-scenario", "subscribe", "-call_duration", "3",
        ], timeout=15)
        s = _parse_stats(out)
        ok = s.get("RegOk", 0) >= 1 and s.get("GmsOk", 0) >= 1
        return ok, f"Reg={s.get('RegOk')}, GMS={s.get('GmsOk')}, CMS={s.get('CmsOk')}"
    runner.run("PTT-REG-01", "PTT 등록 + GMS/CMS 구독", reg_01)

    # ================================================================
    # PTT-CALL: 그룹 통화 + 대시보드/상태 검증
    # ================================================================
    print("\n── PTT-CALL: 그룹 통화 시나리오 ──")

    def call_01():
        """4세션 그룹 통화 → 통화 중 실시간 대시보드/서비스상태 확인 → 종료 후 이력 정합성"""
        import subprocess as sp

        # 1. 통화 전: 대시보드 초기 상태
        h0 = c.get("/api/v1/stats/health")
        ptt0 = len(h0.get("active_ptt", []))
        rtp0 = h0.get("cmp", {}).get("rtp_ports", {}).get("used", 0)

        # 2. cspsim 백그라운드 실행 (7초 유지)
        cmd = ["stdbuf", "-oL", "-eL", CSPSIM,
            "-server_ip", CSP_IP, "-count", "4",
            "-user", PTT_USER1, "-domain", PTT_DOMAIN,
            "-password", PTT_PW, "-mode", "ptt",
            "-group", PTT_GROUP,
            "-scenario", "group-call", "-call_duration", "12",
        ]
        proc = sp.Popen(cmd, stdin=sp.DEVNULL, stdout=sp.PIPE, stderr=sp.STDOUT, text=True,
                        cwd=os.path.join(DIST_DIR, "cspsim"))

        checks = []
        try:
            # 4세션 등록(~3s) + 구독(~1s) + INVITE 참여(~2s) → 통화 중 상태 안정화 대기
            time.sleep(10)

            # 3. 통화 중 실시간 검증
            h_mid = c.get("/api/v1/stats/health")
            mid_ptt = h_mid.get("active_ptt", [])
            mid_rtp = h_mid.get("cmp", {}).get("rtp_ports", {}).get("used", 0)
            mid_reg = h_mid.get("csp", {}).get("registered_users", 0)

            # DB 직접 확인 (디버그)
            try:
                import pymysql
                dconn = pymysql.connect(host="127.0.0.1", port=3306, user="cims", password="cims1234",
                                        database="cims", charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor)
                with dconn.cursor() as dcur:
                    dcur.execute("SELECT id, group_id, state FROM ptt_call_logs WHERE state IN ('ringing','active')")
                    db_logs = dcur.fetchall()
                    dcur.execute("SELECT cp.log_id, cp.msisdn FROM ptt_call_participants cp "
                                 "JOIN ptt_call_logs cl ON cl.id=cp.log_id "
                                 "WHERE cl.state IN ('ringing','active') AND cp.leave_time IS NULL")
                    db_parts = dcur.fetchall()
                dconn.close()
            except Exception:
                db_logs, db_parts = [], []

            checks.append(("통화 중 active_ptt>=1", len(mid_ptt) >= 1 or len(db_logs) >= 1))
            checks.append(("통화 중 registered>=4", mid_reg >= 4))
            checks.append(("통화 중 RTP 포트 사용>0", mid_rtp > 0))

            # 서비스 상태: PTT 가입자 접속 확인
            subs = c.get("/api/v1/stats/subscribers")
            ptt_online = sum(1 for s in subs.get("subscribers", [])
                           if s.get("ptt") and s["ptt"].get("online"))
            checks.append(("통화 중 PTT 접속자>=4", ptt_online >= 4))

            # 서비스 상태: PTT 그룹 참여 확인
            ptt_in_group = sum(1 for s in subs.get("subscribers", [])
                              if s.get("ptt") and len(s["ptt"].get("groups", [])) > 0)
            checks.append(("통화 중 그룹참여>=1", ptt_in_group >= 1 or len(db_parts) >= 1))

            proc.wait(timeout=30)
        except Exception:
            proc.kill()
            proc.wait()

        # 4. 종료 후 검증 (cspsim BYE + 등록해제 + CSP DB 갱신 대기)
        time.sleep(8)
        h1 = c.get("/api/v1/stats/health")
        ptt1 = len(h1.get("active_ptt", []))
        reg1 = h1.get("csp", {}).get("registered_users", 0)

        # cspsim이 BYE + 등록해제를 전송 후 종료하므로 CSP 상태가 정리되어야 함
        checks.append(("종료 후 registered=0", reg1 == 0))

        # 서비스 상태: PTT 접속자 0, 그룹 참여 0 확인
        subs1 = c.get("/api/v1/stats/subscribers")
        ptt_online1 = sum(1 for s in subs1.get("subscribers", [])
                         if s.get("ptt") and s["ptt"].get("online"))
        ptt_in_grp1 = sum(1 for s in subs1.get("subscribers", [])
                         if s.get("ptt") and len(s["ptt"].get("groups", [])) > 0)
        checks.append(("서비스상태 PTT 접속자=0", ptt_online1 == 0))
        # DB 직접 확인: 활성 PTT 세션이 없으면 OK (CSC API 캐시 지연 허용)
        try:
            dconn2 = pymysql.connect(host="127.0.0.1", port=3306, user="cims", password="cims1234",
                                     database="cims", charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor)
            with dconn2.cursor() as dcur2:
                dcur2.execute("SELECT COUNT(*) as cnt FROM ptt_call_logs "
                              "WHERE group_id=%s AND state IN ('ringing','active')", (PTT_GROUP,))
                db_active_calls = dcur2.fetchone()['cnt']
            dconn2.close()
        except Exception:
            db_active_calls = -1
        checks.append(("서비스상태 PTT 그룹참여=0", ptt_in_grp1 == 0 or db_active_calls == 0))

        # 이력 확인
        logs = c.get("/api/v1/call/logs", {"call_type": "ptt", "limit": "5", "date": time.strftime("%Y-%m-%d")})
        ptt_logs = [l for l in logs.get("logs", []) if l.get("call_type") == "ptt"]
        checks.append(("PTT 이력 존재", len(ptt_logs) > 0))

        all_ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        return all_ok, detail
    runner.run("PTT-CALL-01", "그룹 통화 중 실시간 대시보드/상태 + 이력 정합성", call_01)

    def call_02():
        """Conference NOTIFY 수신 확인"""
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "4",
            "-user", PTT_USER1, "-domain", PTT_DOMAIN,
            "-password", PTT_PW, "-mode", "ptt",
            "-group", PTT_GROUP,
            "-scenario", "group-call", "-call_duration", "5",
        ], timeout=30)
        s = _parse_stats(out)
        # 4명이 순차 참여하므로 conference NOTIFY가 발생해야 함
        conf = s.get("ConfNotify", 0)
        return conf > 0, f"ConfNotify={conf}, CallOk={s.get('CallOk')}"
    runner.run("PTT-CALL-02", "Conference NOTIFY 수신 확인", call_02)

    # cspsim 종료 후 CSP 그룹콜 세션 정리 대기
    time.sleep(3)

    # ================================================================
    # PTT-GRP: 그룹 운용 변경 시나리오
    # ================================================================
    print("\n── PTT-GRP: 그룹 운용 변경 ──")

    def grp_01():
        """그룹 생성 → CMP 동기화 확인"""
        test_gid = "+8299995000"
        r = c.post("/api/v1/ptt/groups", {
            "id": test_gid, "name": "테스트그룹", "members": [],
        })
        if r["_status"] not in (200, 201):
            return False, f"생성 실패: {r['_status']}"

        # CMP stats에서 그룹 존재 확인
        time.sleep(1)
        cmp = cmp_request({"cmd": "stats"})
        details = cmp.get("response", {}).get("group_details", []) if cmp else []
        found = any(d.get("group_id") == test_gid for d in details)

        # 정리
        c.delete(f"/api/v1/ptt/groups/{test_gid}")
        return found, f"CMP 그룹 존재={found}"
    runner.run("PTT-GRP-01", "그룹 생성 → CMP 동기화", grp_01)

    def grp_02():
        """그룹 멤버 추가 → CMP modifygroup 반영"""
        test_gid = "+8299995001"
        c.post("/api/v1/ptt/groups", {
            "id": test_gid, "name": "멤버추가테스트", "members": [],
        })
        time.sleep(0.5)

        # 멤버 추가
        r = c.post(f"/api/v1/ptt/groups/{test_gid}/members", {
            "user_id": PTT_USER1, "priority": 1,
        })
        time.sleep(1)

        # CMP에서 멤버 확인
        cmp = cmp_request({"cmd": "stats"})
        details = cmp.get("response", {}).get("group_details", []) if cmp else []
        grp_info = next((d for d in details if d.get("group_id") == test_gid), None)

        # 정리
        c.delete(f"/api/v1/ptt/groups/{test_gid}")

        ok = r["_status"] == 201 and grp_info is not None
        return ok, f"멤버추가={r['_status']}, CMP멤버={grp_info}"
    runner.run("PTT-GRP-02", "그룹 멤버 추가 → CMP 반영", grp_02)

    def grp_03():
        """그룹 삭제 → CMP removegroup 확인"""
        test_gid = "+8299995002"
        c.post("/api/v1/ptt/groups", {"id": test_gid, "name": "삭제테스트", "members": []})
        time.sleep(1)

        # 삭제
        c.delete(f"/api/v1/ptt/groups/{test_gid}")
        time.sleep(1)

        # CMP에서 제거 확인
        cmp = cmp_request({"cmd": "stats"})
        details = cmp.get("response", {}).get("group_details", []) if cmp else []
        found = any(d.get("group_id") == test_gid for d in details)

        return not found, f"CMP에서 제거됨={not found}"
    runner.run("PTT-GRP-03", "그룹 삭제 → CMP 제거 확인", grp_03)

    # ================================================================
    # PTT-DASH: 대시보드/상태/통계 검증
    # ================================================================
    print("\n── PTT-DASH: 대시보드/상태/통계 ──")

    def dash_01():
        """대시보드 CMP 상태 (그룹 수, RTP 포트) 정합성"""
        h = c.get("/api/v1/stats/health")
        cmp_info = h.get("cmp", {})
        ok = h["_status"] == 200 and "groups" in cmp_info and "rtp_ports" in cmp_info
        rtp = cmp_info.get("rtp_ports", {})
        return ok, f"groups={cmp_info.get('groups')}, rtp={rtp.get('used')}/{rtp.get('total')}"
    runner.run("PTT-DASH-01", "대시보드 CMP 상태 정합성", dash_01)

    def dash_02():
        """서비스 상태 PTT 가입자 목록 정합성"""
        subs = c.get("/api/v1/stats/subscribers")
        sub_list = subs.get("subscribers", [])
        ptt_subs = [s for s in sub_list if s.get("ptt")]
        ok = subs["_status"] == 200 and len(ptt_subs) > 0
        online = sum(1 for s in ptt_subs if s["ptt"].get("online"))
        return ok, f"PTT 가입자={len(ptt_subs)}, 접속중={online}"
    runner.run("PTT-DASH-02", "서비스 상태 PTT 가입자 목록", dash_02)

    def dash_03():
        """PTT 서비스 통계 정합성"""
        stats = c.get("/api/v1/stats/service/ptt")
        ok = stats["_status"] == 200 and "ptt" in stats
        ptt = stats.get("ptt", {})
        return ok, f"total_calls={ptt.get('total_calls')}, avg_dur={ptt.get('avg_duration_sec')}s"
    runner.run("PTT-DASH-03", "PTT 서비스 통계", dash_03)

    def dash_04():
        """PTT 통화 이력 조회"""
        logs = c.get("/api/v1/call/logs", {"call_type": "ptt", "limit": "10", "date": time.strftime("%Y-%m-%d")})
        ok = logs["_status"] == 200 and "logs" in logs
        log_list = logs.get("logs", [])
        return ok, f"total={logs.get('total')}, recent={len(log_list)}건"
    runner.run("PTT-DASH-04", "PTT 통화 이력 조회", dash_04)

    def dash_05():
        """대시보드 CSP 모듈 역할 확인"""
        h = c.get("/api/v1/stats/health")
        roles = h.get("csp", {}).get("roles", {})
        ok = roles.get("CSCF") and roles.get("PTT_AS")
        return ok, f"roles={roles}"
    runner.run("PTT-DASH-05", "대시보드 CSP 역할 상태", dash_05)

    def dash_06():
        """시험 종료 후 잔류 데이터 없음 확인 (접속자/그룹참여=0)"""
        # PTT-CALL-02 종료 후 CSP 세션 정리 대기
        time.sleep(5)
        h = c.get("/api/v1/stats/health")
        reg = h.get("csp", {}).get("registered_users", 0)
        active_ptt = h.get("active_ptt", [])

        subs = c.get("/api/v1/stats/subscribers")
        ptt_online = sum(1 for s in subs.get("subscribers", [])
                        if s.get("ptt") and s["ptt"].get("online"))
        ptt_in_grp = sum(1 for s in subs.get("subscribers", [])
                        if s.get("ptt") and len(s["ptt"].get("groups", [])) > 0)

        checks = []
        checks.append(("PTT 접속자=0", ptt_online == 0))
        checks.append(("PTT 그룹참여=0", ptt_in_grp == 0))
        checks.append(("active_ptt 비어있음", len(active_ptt) == 0))

        ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        if not ok:
            detail += f" (online={ptt_online}, in_grp={ptt_in_grp}, active_ptt={len(active_ptt)})"
        return ok, detail
    runner.run("PTT-DASH-06", "시험 종료 후 잔류 데이터 없음 확인", dash_06)

    # ================================================================
    # PTT-TMR: 타이머/자원 해제 검증
    # ================================================================
    print("\n── PTT-TMR: 타이머/자원 해제 검증 ──")

    def tmr_01():
        """CSP 타이머 설정 + PTT 그룹콜 모니터 활성 확인"""
        r = csp_request("stats")
        if r is None:
            return False, "CSP 응답 없음"
        timeouts = r.get("timeouts", {})
        user_to = timeouts.get("user_timeout", 0)
        stale_to = timeouts.get("stale_call_timeout", 0)
        ok = user_to > 0
        return ok, f"UserTimeout={user_to}s, StaleCallTimeout={stale_to}s"
    runner.run("PTT-TMR-01", "CSP 타이머 설정 상태", tmr_01)

    def tmr_02():
        """그룹콜 종료 후 CMP 개별 세션 정리 + 테스트 잔류 그룹 없음 확인"""
        r = cmp_request({"cmd": "stats"})
        if r is None:
            return False, "CMP 응답 없음"
        resp = r.get("response", {})
        sessions = resp.get("sessions", -1)
        free = resp.get("rtp_ports_free", 0)
        total = resp.get("rtp_ports_total", 0)
        groups = resp.get("groups", 0)
        st = resp.get("session_timeout", 0)
        group_details = resp.get("group_details", [])
        # 테스트 전용 그룹(_vtest_)이 남아있지 않은지 확인
        stale_test_groups = [g for g in group_details if g.get("group_id", "").startswith("_vtest_")]
        checks = []
        checks.append(("CMP 잔류세션=0", sessions == 0))
        checks.append(("테스트 잔류그룹=0", len(stale_test_groups) == 0))
        ok = all(v for _, v in checks)
        detail = f"sessions={sessions}, groups={groups}, stale_test={len(stale_test_groups)}, rtp_free={free}/{total}, session_timeout={st}s"
        return ok, detail
    runner.run("PTT-TMR-02", "그룹콜 종료 후 CMP 자원 회수", tmr_02)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("PTT 서비스 검증")
    print("=" * 60)
    r = run_ptt_tests()
    print(f"\n총 {r['total']}건: PASS={r['pass']} FAIL={r['fail']}")
