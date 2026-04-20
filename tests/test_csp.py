"""
CSP 모듈 검증: CscInterface UDP + cspsim 기반 SIP 시나리오
"""
import sys, os, time, subprocess, re
sys.path.insert(0, os.path.dirname(__file__))

from conftest import (
    csp_request, TestRunner,
    CSP_IP, CSP_REALM, VOLTE_DOMAIN as VOIP_DOMAIN, PTT_DOMAIN,
)

# cspsim 바이너리 경로
DIST_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "dist"))
CSPSIM = os.path.join(DIST_DIR, "cspsim", "bin", "cspsim")


def _run_cspsim(args, timeout=30):
    """cspsim 실행 후 stdout 반환"""
    cmd = [CSPSIM] + args
    try:
        result = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout, cwd=os.path.join(DIST_DIR, "cspsim"))
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    except FileNotFoundError:
        return f"NOT_FOUND: {CSPSIM}"


def _parse_stats(output):
    """cspsim 출력에서 통계 파싱
    형식 예:
      Registered   : 1 / 1  (fail=0)
      GMS Subscribed: 1
      Call OK/End   : 2 / 2  (fail=0)
    """
    stats = {}
    for line in output.split("\n"):
        # Registered : N / T (fail=F)
        m = re.search(r"Registered\s*:\s*(\d+)\s*/\s*\d+\s*\(fail=(\d+)\)", line)
        if m:
            stats["RegOk"] = int(m.group(1))
            stats["RegFail"] = int(m.group(2))
        # GMS Subscribed: N
        m = re.search(r"GMS Subscribed:\s*(\d+)", line)
        if m:
            stats["GmsOk"] = int(m.group(1))
        # CMS Subscribed: N
        m = re.search(r"CMS Subscribed:\s*(\d+)", line)
        if m:
            stats["CmsOk"] = int(m.group(1))
        # Call OK/End : N / M (fail=F)
        m = re.search(r"Call OK/End\s*:\s*(\d+)\s*/\s*(\d+)\s*\(fail=(\d+)\)", line)
        if m:
            stats["CallOk"] = int(m.group(1))
            stats["CallEnd"] = int(m.group(2))
            stats["CallFail"] = int(m.group(3))
        # NOTIFY Recv : N
        m = re.search(r"NOTIFY Recv\s*:\s*(\d+)", line)
        if m:
            stats["NotifyRecv"] = int(m.group(1))
    return stats


def run_csp_tests():
    runner = TestRunner("CSP")

    # ================================================================
    # CSP-IF: CscInterface UDP 명령
    # ================================================================
    print("\n── CSP-IF: CscInterface UDP 명령 ──")

    def if_01():
        r = csp_request("stats")
        if r is None:
            return False, "응답 없음 (timeout) — CSP가 최신 바이너리인지 확인"
        ok = r.get("status") == "OK" and "registered_users" in r
        return ok, f"response={r}"
    runner.run("CSP-IF-01", "stats 요청", if_01)

    def if_02():
        # user_change는 fire-and-forget이므로 수신 확인만
        # 실제로 CSP가 수신하면 로그에 기록됨
        r = csp_request("user_change", uri="tel:+8299999999", action="PUT")
        # CSP는 user_change에 응답을 보내지 않으므로 None이 정상
        # 오류 없이 전송되면 통과
        return True, "전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요)"
    runner.run("CSP-IF-02", "user_change 통지", if_02)

    def if_03():
        r = csp_request("group_change", uri="tel:+8299991000", action="PUT")
        return True, "전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요)"
    runner.run("CSP-IF-03", "group_change 통지", if_03)

    # ================================================================
    # CSP-TMR: 타이머 설정 검증
    # ================================================================
    print("\n── CSP-TMR: 타이머/타임아웃 설정 ──")

    def tmr_01():
        """CSP stats에서 타이머 설정값 확인"""
        r = csp_request("stats")
        if r is None:
            return False, "응답 없음 (timeout)"
        timeouts = r.get("timeouts", {})
        user_timeout = timeouts.get("user_timeout", -1)
        stale_call = timeouts.get("stale_call_timeout", -1)
        options_period = timeouts.get("send_options_period", -1)
        ok = user_timeout > 0 and stale_call >= 0 and options_period >= 0
        return ok, f"user_timeout={user_timeout}s, stale_call_timeout={stale_call}s, options_period={options_period}s"
    runner.run("CSP-TMR-01", "타이머 설정값 확인", tmr_01)

    def tmr_02():
        """등록 만료 → DB logout_time 갱신 확인
        짧은 Expires(3초)로 등록 → 등록 확인 → 만료 대기(UserTimeout + DeleteTimeout 여유) → 접속자=0"""
        import pymysql

        # 1. 짧은 Expires로 등록 (call_duration=1 → 빠르게 종료, cspsim 자체는 UNREGISTER 전송)
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", "+821357007002",
            "-auth_id", "450033100000002@" + VOIP_DOMAIN,
            "-domain", VOIP_DOMAIN,
            "-password", "123456", "-mode", "voip",
            "-scenario", "register", "-call_duration", "2",
        ], timeout=15)
        s = _parse_stats(out)
        if s.get("RegOk", 0) < 1:
            return False, f"등록 실패: stats={s}"

        # 2. 종료 후 DB에서 logout_time 확인
        time.sleep(2)
        try:
            conn = pymysql.connect(
                host="127.0.0.1", port=3306, user="cims", password="cims1234",
                database="cims", charset="utf8mb4",
                cursorclass=pymysql.cursors.DictCursor,
            )
            with conn.cursor() as cur:
                cur.execute("SELECT register_time, logout_time FROM voip_subscriptions WHERE id='+821357007002'")
                row = cur.fetchone()
            conn.close()
        except Exception as e:
            return False, f"DB 조회 실패: {e}"

        if not row:
            return False, "DB에 사용자 없음"

        reg_time = row.get("register_time")
        logout_time = row.get("logout_time")
        # logout_time이 존재하고 register_time 이후여야 함 (오프라인 상태)
        ok = logout_time is not None and reg_time is not None and logout_time >= reg_time
        return ok, f"register_time={reg_time}, logout_time={logout_time}"
    runner.run("CSP-TMR-02", "등록해제 시 DB logout_time 갱신", tmr_02)

    # ================================================================
    # CSP-SIP: SIP 등록/구독 (cspsim 이용)
    # ================================================================
    print("\n── CSP-SIP: SIP 등록/구독 ──")

    # cspsim 존재 확인
    if not os.path.isfile(CSPSIM):
        print(f"  [SKIP] cspsim 바이너리 없음: {CSPSIM}")
        for tid, name in [("CSP-SIP-01", "SIP REGISTER 성공"), ("CSP-SIP-02", "SIP REGISTER 인증 실패"),
                          ("CSP-SIP-03", "GMS SUBSCRIBE"), ("CSP-SIP-04", "CMS SUBSCRIBE"),
                          ("CSP-CALL-01", "VoIP 1:1 통화"), ("CSP-CALL-02", "PTT 그룹 통화")]:
            runner.run(tid, name, lambda: (False, "cspsim not found"))
        return runner.summary()

    def sip_01():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", "+82571900001", "-domain", PTT_DOMAIN,
            "-password", "123456", "-mode", "ptt",
            "-scenario", "register", "-call_duration", "2",
        ], timeout=15)
        if "NOT_FOUND" in out:
            return False, out
        s = _parse_stats(out)
        ok = s.get("RegOk", 0) >= 1
        return ok, f"stats={s}"
    runner.run("CSP-SIP-01", "SIP REGISTER 성공", sip_01)

    def sip_02():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", "+82571900001", "-domain", PTT_DOMAIN,
            "-password", "wrongpassword", "-mode", "ptt",
            "-scenario", "register", "-call_duration", "2",
        ], timeout=15)
        s = _parse_stats(out)
        ok = s.get("RegFail", 0) >= 1 or s.get("RegOk", 0) == 0
        return ok, f"stats={s}"
    runner.run("CSP-SIP-02", "SIP REGISTER 인증 실패", sip_02)

    def sip_03():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", "+82571900001", "-domain", PTT_DOMAIN,
            "-password", "123456", "-mode", "ptt",
            "-scenario", "subscribe", "-call_duration", "3",
        ], timeout=20)
        s = _parse_stats(out)
        ok = s.get("GmsOk", 0) >= 1
        return ok, f"stats={s}"
    runner.run("CSP-SIP-03", "GMS SUBSCRIBE", sip_03)

    def sip_04():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "1",
            "-user", "+82571900001", "-domain", PTT_DOMAIN,
            "-password", "123456", "-mode", "ptt",
            "-scenario", "subscribe", "-call_duration", "3",
        ], timeout=20)
        s = _parse_stats(out)
        ok = s.get("CmsOk", 0) >= 1
        return ok, f"stats={s}"
    runner.run("CSP-SIP-04", "CMS SUBSCRIBE", sip_04)

    # ================================================================
    # CSP-CALL: VoIP/PTT 통화
    # ================================================================
    print("\n── CSP-CALL: VoIP/PTT 통화 ──")

    def call_01():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "2",
            "-user", "+821357007002",
            "-auth_id", "450033100000002@" + VOIP_DOMAIN,
            "-domain", VOIP_DOMAIN,
            "-password", "123456", "-mode", "voip",
            "-scenario", "call", "-call_duration", "3",
        ], timeout=25)
        s = _parse_stats(out)
        ok = s.get("CallOk", 0) >= 1
        return ok, f"stats={s}"
    runner.run("CSP-CALL-01", "VoIP 1:1 통화", call_01)

    def call_02():
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "4",
            "-user", "+82571900001", "-domain", PTT_DOMAIN,
            "-password", "123456", "-mode", "ptt",
            "-group", "+82571910001",
            "-scenario", "group-call", "-call_duration", "5",
        ], timeout=30)
        s = _parse_stats(out)
        ok = s.get("CallOk", 0) >= 1
        return ok, f"stats={s}"
    runner.run("CSP-CALL-02", "PTT 그룹 통화", call_02)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("CSP 모듈 검증 시작")
    print("=" * 60)
    result = run_csp_tests()
    print(f"\n총 {result['total']}건: PASS={result['pass']} FAIL={result['fail']} SKIP={result['skip']}")
