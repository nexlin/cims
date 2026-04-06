"""
CMP 모듈 검증: UDP 제어 명령, 그룹 관리, RTP 포트
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))

from conftest import cmp_request, TestRunner


def run_cmp_tests():
    runner = TestRunner("CMP")

    # ================================================================
    # CMP-CMD: 제어 명령
    # ================================================================
    print("\n── CMP-CMD: 제어 명령 ──")

    def cmd_01():
        r = cmp_request({"cmd": "alive"})
        if r is None:
            return False, "응답 없음 (timeout)"
        return True, f"response={r}"
    runner.run("CMP-CMD-01", "alive 헬스체크", cmd_01)

    def cmd_02():
        r = cmp_request({"cmd": "stats"})
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", {})
        has_fields = "sessions" in resp and "rtp_ports_total" in resp
        return resp.get("status") == "OK" and has_fields, f"response={resp}"
    runner.run("CMP-CMD-02", "stats 상태 조회", cmd_02)

    def cmd_03():
        sid = f"_vtest_s_{int(time.time())}"
        r = cmp_request({
            "cmd": "add",
            "session_id": sid,
            "remote_ip": "0.0.0.0",
            "remote_port": 0,
            "remote_video_port": 0,
            "peer_index": -1,
        })
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", {})
        if isinstance(resp, str):
            return False, f"문자열 응답 (포트 풀 소진?): {resp}"
        ok = resp.get("status") == "OK" and resp.get("local_port", 0) > 0
        # 정리
        cmp_request({"cmd": "remove", "session_id": sid})
        return ok, f"local_port={resp.get('local_port')}"
    runner.run("CMP-CMD-03", "add 세션 생성", cmd_03)

    def cmd_04():
        sid = f"_vtest_s_{int(time.time())}"
        # 생성
        cmp_request({
            "cmd": "add", "session_id": sid,
            "remote_ip": "0.0.0.0", "remote_port": 0,
            "remote_video_port": 0, "peer_index": -1,
        })
        # 삭제
        r = cmp_request({"cmd": "remove", "session_id": sid})
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", "")
        return resp == "OK" or (isinstance(resp, dict) and resp.get("status") == "OK"), f"response={resp}"
    runner.run("CMP-CMD-04", "remove 세션 삭제", cmd_04)

    # ================================================================
    # CMP-GRP: 그룹 명령
    # ================================================================
    print("\n── CMP-GRP: 그룹 명령 ──")

    ts = int(time.time())
    gid = f"_vtest_g_{ts}"
    sid1 = f"_vtest_gs1_{ts}"
    sid2 = f"_vtest_gs2_{ts}"

    def grp_01():
        r = cmp_request({
            "cmd": "addgroup",
            "group_id": gid,
            "members": f"{sid1}:1,{sid2}:2",
        })
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", {})
        ok = isinstance(resp, dict) and resp.get("status") == "OK"
        return ok, f"response={resp}"
    runner.run("CMP-GRP-01", "addgroup 그룹 생성", grp_01)

    def grp_02():
        sid3 = f"_vtest_gs3_{ts}"
        r = cmp_request({
            "cmd": "joingroup",
            "group_id": gid,
            "session_id": sid3,
            "user_ip": "127.0.0.1",
            "user_port": 30000,
            "user_video_port": 0,
        })
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", "")
        ok = resp == "OK" or (isinstance(resp, dict) and resp.get("status") == "OK")
        # 정리
        cmp_request({"cmd": "leavegroup", "group_id": gid, "session_id": sid3})
        return ok, f"response={resp}"
    runner.run("CMP-GRP-02", "joingroup 멤버 참여", grp_02)

    def grp_03():
        sid4 = f"_vtest_gs4_{ts}"
        cmp_request({
            "cmd": "joingroup", "group_id": gid, "session_id": sid4,
            "user_ip": "127.0.0.1", "user_port": 30010, "user_video_port": 0,
        })
        r = cmp_request({"cmd": "leavegroup", "group_id": gid, "session_id": sid4})
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", "")
        ok = resp == "OK" or (isinstance(resp, dict) and resp.get("status") == "OK")
        return ok, f"response={resp}"
    runner.run("CMP-GRP-03", "leavegroup 멤버 탈퇴", grp_03)

    def grp_04():
        r = cmp_request({"cmd": "removegroup", "group_id": gid})
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", "")
        ok = resp == "OK" or (isinstance(resp, dict) and resp.get("status") == "OK")
        return ok, f"response={resp}"
    runner.run("CMP-GRP-04", "removegroup 그룹 삭제", grp_04)

    # ================================================================
    # CMP-RTP: RTP 포트 관리
    # ================================================================
    print("\n── CMP-RTP: RTP 포트 관리 ──")

    def rtp_01():
        # 초기 상태
        r0 = cmp_request({"cmd": "stats"})
        if r0 is None:
            return False, "stats 응답 없음"
        used0 = r0.get("response", {}).get("rtp_ports_used", 0)

        # 2개 세션 추가
        s1 = f"_vtest_rtp1_{ts}"
        s2 = f"_vtest_rtp2_{ts}"
        cmp_request({"cmd": "add", "session_id": s1, "remote_ip": "0.0.0.0", "remote_port": 0, "remote_video_port": 0, "peer_index": -1})
        cmp_request({"cmd": "add", "session_id": s2, "remote_ip": "0.0.0.0", "remote_port": 0, "remote_video_port": 0, "peer_index": -1})

        r1 = cmp_request({"cmd": "stats"})
        used1 = r1.get("response", {}).get("rtp_ports_used", 0)

        # 삭제
        cmp_request({"cmd": "remove", "session_id": s1})
        cmp_request({"cmd": "remove", "session_id": s2})

        r2 = cmp_request({"cmd": "stats"})
        used2 = r2.get("response", {}).get("rtp_ports_used", 0)

        added = used1 - used0
        freed = used1 - used2
        ok = added == 2 and freed == 2
        return ok, f"before={used0}, after_add={used1}(+{added}), after_remove={used2}(-{freed})"
    runner.run("CMP-RTP-01", "포트 할당/해제 정합성", rtp_01)

    # ================================================================
    # CMP-FLOOR: 플로어 제어
    # ================================================================
    print("\n── CMP-FLOOR: 플로어 제어 ──")

    def floor_01():
        # 그룹 생성 → stats에서 group_details 확인
        fg = f"_vtest_floor_{ts}"
        fs1 = f"_vtest_fls1_{ts}"
        fs2 = f"_vtest_fls2_{ts}"
        cmp_request({
            "cmd": "addgroup", "group_id": fg,
            "members": f"{fs1}:1,{fs2}:2",
        })
        r = cmp_request({"cmd": "stats"})
        # 정리
        cmp_request({"cmd": "removegroup", "group_id": fg})

        if r is None:
            return False, "stats 응답 없음"
        resp = r.get("response", {})
        details = resp.get("group_details", [])
        found = any(d.get("group_id") == fg for d in details)
        return found, f"group_details count={len(details)}, found={found}"
    runner.run("CMP-FLOOR-01", "그룹 플로어 상태 확인", floor_01)

    # ================================================================
    # CMP-TMR: 타이머/타임아웃 설정
    # ================================================================
    print("\n── CMP-TMR: 타이머/타임아웃 설정 ──")

    def tmr_01():
        """CMP stats에서 SessionTimeout 설정값 확인"""
        r = cmp_request({"cmd": "stats"})
        if r is None:
            return False, "응답 없음 (timeout)"
        resp = r.get("response", {})
        timeout_val = resp.get("session_timeout", -1)
        ok = timeout_val > 0
        return ok, f"session_timeout={timeout_val}s"
    runner.run("CMP-TMR-01", "세션 타임아웃 설정값 확인", tmr_01)

    def tmr_02():
        """세션 생성 → touchActivity 동작 확인 (stats에서 세션 존재 확인 후 정리)"""
        sid = f"_vtest_tmr_{int(time.time())}"
        # 세션 생성
        r = cmp_request({
            "cmd": "add", "session_id": sid,
            "remote_ip": "0.0.0.0", "remote_port": 0,
            "remote_video_port": 0, "peer_index": -1,
        })
        if r is None:
            return False, "세션 생성 실패"

        # stats에서 세션 수 확인
        r2 = cmp_request({"cmd": "stats"})
        sessions_before = r2.get("response", {}).get("sessions", 0) if r2 else 0

        # 정리
        cmp_request({"cmd": "remove", "session_id": sid})

        r3 = cmp_request({"cmd": "stats"})
        sessions_after = r3.get("response", {}).get("sessions", 0) if r3 else 0

        ok = sessions_before > sessions_after
        return ok, f"생성 후 sessions={sessions_before}, 삭제 후 sessions={sessions_after}"
    runner.run("CMP-TMR-02", "세션 생성/삭제 후 리소스 정합성", tmr_02)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("CMP 모듈 검증 시작")
    print("=" * 60)
    result = run_cmp_tests()
    print(f"\n총 {result['total']}건: PASS={result['pass']} FAIL={result['fail']} SKIP={result['skip']}")
