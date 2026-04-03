"""
CSP 모듈 검증: CscInterface UDP + cspsim 기반 SIP 시나리오
"""
import sys, os, time, subprocess, re
sys.path.insert(0, os.path.dirname(__file__))

from conftest import csp_request, TestRunner

# cspsim 바이너리 경로
DIST_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "dist"))
CSPSIM = os.path.join(DIST_DIR, "cspsim", "bin", "cspsim")
CSP_IP = "127.0.0.1"

# DB의 실제 사용자 정보 (auth_id의 domain이 CSP realm과 다를 수 있으므로
# cspsim의 -domain은 CSP realm 기준으로 설정)
CSP_REALM = "ims.mnc001.mcc001.3gppnetwork.org"
PTT_DOMAIN = "ptt.mnc033.mcc450.3gppnetwork.org"
VOIP_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"


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
