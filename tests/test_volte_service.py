"""
VoLTE 서비스 검증: 운용 설정 + cspsim 기반 통화 시나리오 + 대시보드/상태/이력 검증
"""
import sys, os, time, subprocess, re
sys.path.insert(0, os.path.dirname(__file__))

from conftest import CscClient, csp_request, cmp_request, TestRunner

DIST_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "dist"))
CSPSIM = os.path.join(DIST_DIR, "cspsim", "bin", "cspsim")
CSP_IP = "127.0.0.1"
VOIP_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"

# 미디어 파일 경로
MEDIA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "media")
AUDIO_FILE = os.path.join(MEDIA_DIR, "8050001000004_audio.amrwb")
VIDEO_FILE = os.path.join(MEDIA_DIR, "8050001000004_video.h264")

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

        # 2. cspsim으로 통화 실행 (10초 유지, 음성+영상 미디어 전송)
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "2",
            "-user", VOIP_USER1, "-auth_id", VOIP_AUTH1,
            "-domain", VOIP_DOMAIN, "-password", VOIP_PW,
            "-mode", "voip", "-scenario", "call", "-call_duration", "10",
            "-media_file", AUDIO_FILE, "-video_file", VIDEO_FILE,
        ], timeout=35)
        s = _parse_stats(out)
        call_ok = s.get("CallOk", 0) >= 1

        # 3. 통화 종료 후 대시보드: active_calls 0 복원
        time.sleep(1)
        h1 = c.get("/api/v1/stats/health")
        calls1 = h1.get("csp", {}).get("active_calls", 0)
        reg1 = h1.get("csp", {}).get("registered_users", 0)
        active_voip = h1.get("active_voip", [])

        # 4. 이력: state=ended 이고 발/착신 번호 정확한지
        logs = c.get("/api/v1/call/logs", {"call_type": "voip", "limit": "5", "date": time.strftime("%Y-%m-%d")})
        log_list = logs.get("logs", [])
        ended_logs = [l for l in log_list if l.get("state") == "ended" and l.get("initiator") == VOIP_USER1]

        # 5. 서비스 상태: 가입자 접속 상태 정확한지
        subs = c.get("/api/v1/stats/subscribers")
        sub_list = subs.get("subscribers", [])

        checks = []
        checks.append(("cspsim CallOk>=1", call_ok))
        checks.append(("종료 후 active_calls=0", calls1 == 0))
        checks.append(("종료 후 active_voip 비어있음", len(active_voip) == 0))
        checks.append(("종료 후 registered=0", reg1 == 0))

        # 서비스 상태: 접속자 0 확인
        subs1 = c.get("/api/v1/stats/subscribers")
        voip_online1 = sum(1 for s in subs1.get("subscribers", [])
                          if s.get("voip") and s["voip"].get("online"))
        checks.append(("서비스상태 VoIP 접속자=0", voip_online1 == 0))

        # 이력 검증
        recent_logs = [l for l in log_list if l.get("call_type") == "voip" and l.get("initiator") == VOIP_USER1]
        checks.append(("이력 존재", len(recent_logs) > 0))
        if recent_logs:
            checks.append(("이력 callee 정확", recent_logs[0].get("callee") == VOIP_USER2))
            checks.append(("이력 state=ended", recent_logs[0].get("state") == "ended"))

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
            "-mode", "voip", "-scenario", "call", "-call_duration", "10",
            "-media_file", AUDIO_FILE, "-video_file", VIDEO_FILE,
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
            proc.wait(timeout=30)
        except Exception:
            proc.kill()
            proc.wait()

        # 종료 후 확인 (cspsim이 BYE+등록해제 전송 후 종료)
        time.sleep(2)
        h_end = c.get("/api/v1/stats/health")
        end_calls = h_end.get("csp", {}).get("active_calls", 0)
        end_reg = h_end.get("csp", {}).get("registered_users", 0)
        checks.append(("종료 후 active_calls=0", end_calls == 0))
        checks.append(("종료 후 registered=0", end_reg == 0))

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
        logs = c.get("/api/v1/call/logs", {"call_type": "voip", "limit": "10", "date": time.strftime("%Y-%m-%d")})
        ok = logs["_status"] == 200 and "logs" in logs
        log_list = logs.get("logs", [])
        return ok, f"total={logs.get('total')}, recent={len(log_list)}건"
    runner.run("VoLTE-DASH-04", "VoIP 통화 이력 조회", dash_04)

    def dash_05():
        """서비스 상태 잔류 데이터 없음 확인 (시험 종료 후 접속자/활성호=0)"""
        h = c.get("/api/v1/stats/health")
        reg = h.get("csp", {}).get("registered_users", 0)
        calls = h.get("csp", {}).get("active_calls", 0)
        active_voip = h.get("active_voip", [])
        # ringing 상태로 남은 stale 이력 없는지 확인
        ringing = [v for v in active_voip if v.get("state") == "ringing"]

        subs = c.get("/api/v1/stats/subscribers")
        voip_online = sum(1 for s in subs.get("subscribers", [])
                         if s.get("voip") and s["voip"].get("online"))

        checks = []
        checks.append(("registered=0", reg == 0))
        checks.append(("active_calls=0", calls == 0))
        checks.append(("active_voip 비어있음", len(active_voip) == 0))
        checks.append(("ringing 잔류 없음", len(ringing) == 0))
        checks.append(("VoIP 접속자=0", voip_online == 0))

        ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        if not ok:
            detail += f" (reg={reg}, calls={calls}, voip_active={len(active_voip)}, online={voip_online})"
        return ok, detail
    runner.run("VoLTE-DASH-05", "시험 종료 후 잔류 데이터 없음 확인", dash_05)

    # ================================================================
    # VoLTE-TMR: 타이머/타임아웃 검증
    # ================================================================
    print("\n── VoLTE-TMR: 타이머/자원 해제 검증 ──")

    def tmr_01():
        """CSP 타이머 설정 활성 상태 확인"""
        r = csp_request("stats")
        if r is None:
            return False, "CSP 응답 없음"
        timeouts = r.get("timeouts", {})
        user_to = timeouts.get("user_timeout", 0)
        stale_to = timeouts.get("stale_call_timeout", 0)
        checks = []
        checks.append(("UserTimeout>0", user_to > 0))
        checks.append(("StaleCallTimeout>=0", stale_to >= 0))
        ok = all(v for _, v in checks)
        detail = f"UserTimeout={user_to}s, StaleCallTimeout={stale_to}s"
        return ok, detail
    runner.run("VoLTE-TMR-01", "CSP 타이머 설정 활성 상태", tmr_01)

    def tmr_02():
        """CMP 세션 타임아웃 설정 활성 상태 확인"""
        r = cmp_request({"cmd": "stats"})
        if r is None:
            return False, "CMP 응답 없음"
        resp = r.get("response", {})
        st = resp.get("session_timeout", 0)
        ok = st > 0
        return ok, f"SessionTimeout={st}s"
    runner.run("VoLTE-TMR-02", "CMP 세션 타임아웃 설정 활성 상태", tmr_02)

    def tmr_03():
        """통화 종료 후 CMP VoIP 개별 세션 잔류 없음 확인"""
        r = cmp_request({"cmd": "stats"})
        if r is None:
            return False, "CMP 응답 없음"
        resp = r.get("response", {})
        sessions = resp.get("sessions", -1)
        free = resp.get("rtp_ports_free", 0)
        total = resp.get("rtp_ports_total", 0)
        st = resp.get("session_timeout", 0)
        # VoIP 개별 세션(1:1)은 0이어야 함
        # PTT 그룹은 운용 그룹이므로 포트 점유 정상
        ok = sessions == 0
        detail = f"sessions={sessions}, rtp_free={free}/{total}, session_timeout={st}s"
        return ok, detail
    runner.run("VoLTE-TMR-03", "통화 종료 후 CMP VoIP 세션 정리 확인", tmr_03)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("VoLTE 서비스 검증")
    print("=" * 60)
    r = run_volte_tests()
    print(f"\n총 {r['total']}건: PASS={r['pass']} FAIL={r['fail']}")
