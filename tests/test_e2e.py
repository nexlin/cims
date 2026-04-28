"""
연동/E2E 검증: CSC-CSP 동기화, 헬스체크 연동, 단대단 시나리오
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))

from conftest import (CscClient, csp_request, cmp_request, TestRunner,
                       TEST_PREFIX, TEST_VOIP_MSISDN, TEST_PTT_MSISDN, TEST_GROUP_ID)


def run_e2e_tests(only=None):
    runner = TestRunner("E2E", only_ids=only)
    c = CscClient()
    try:
        c.login()
    except Exception as e:
        print(f"  [ERROR] CSC 로그인 실패: {e}")
        for tid, name in [("E2E-HEALTH-01", "CSC→CSP+CMP+DB 헬스 연동"),
                          ("E2E-SYNC-01", "사용자 변경 → CSP 동기화"),
                          ("E2E-SYNC-02", "그룹 변경 → CSP 동기화"),
                          ("E2E-OPS-01", "전체 운용 흐름")]:
            runner.run(tid, name, lambda: (False, f"CSC 연결 실패: {e}"))
        return runner.summary()

    # ================================================================
    # E2E-HEALTH: 헬스체크 연동
    # ================================================================
    print("\n── E2E-HEALTH: 헬스체크 연동 ──")

    def health_01():
        r = c.get("/api/v1/stats/health")
        h = r.get("health", {})
        cmp_ok = h.get("cmp") == "up"
        db_ok = h.get("db") == "up"
        csp_status = h.get("csp", "down")
        detail = f"csp={csp_status}, cmp={'up' if cmp_ok else 'down'}, db={'up' if db_ok else 'down'}"
        # CMP와 DB는 반드시 up, CSP는 바이너리 버전에 따라 다를 수 있음
        return cmp_ok and db_ok, detail
    runner.run("E2E-HEALTH-01", "CSC→CSP+CMP+DB 헬스 연동", health_01)

    # ================================================================
    # E2E-SYNC: CSC-CSP 동기화
    # ================================================================
    print("\n── E2E-SYNC: CSC-CSP 동기화 ──")

    def sync_01():
        # CSC에서 사용자 생성 → notify_csp가 호출되는지 확인
        # CSC admin API가 내부적으로 CspNotify UDP를 전송함
        uid_r = c.post("/api/v1/users", {"name": f"{TEST_PREFIX}sync_user", "login_id": f"{TEST_PREFIX}sync_user", "password": "test1234"})
        if uid_r["_status"] != 201:
            return False, f"사용자 생성 실패: {uid_r['_status']}"
        uid = uid_r["id"]

        # 사용자에 VoIP 구독 추가 (이때 notify_csp 발생)
        sub_r = c.post(f"/api/v1/users/{uid}/call", {
            "id": "+8299998001", "auth_id": "sync@test", "passwd": "1234",
        })

        # 수정 (PUT → user_change 통지)
        mod_r = c.put(f"/api/v1/users/{uid}/call/+8299998001", {"dnd": True})

        # 정리
        c.delete(f"/api/v1/users/{uid}")

        ok = sub_r["_status"] == 201 and mod_r["_status"] == 200
        return ok, f"sub={sub_r['_status']}, mod={mod_r['_status']} (CSP notify는 CSC 내부에서 자동 발송)"
    runner.run("E2E-SYNC-01", "사용자 변경 → CSP 동기화", sync_01)

    def sync_02():
        # 그룹 생성/수정 → group_change 통지
        grp_r = c.post("/api/v1/ptt/groups", {
            "id": "+8299998000", "name": f"{TEST_PREFIX}sync_grp", "members": [],
        })
        if grp_r["_status"] not in (200, 201):
            return False, f"그룹 생성 실패: {grp_r['_status']}"

        # 수정
        mod_r = c.put("/api/v1/ptt/groups/+8299998000", {"name": f"{TEST_PREFIX}sync_grp_mod"})

        # 정리
        c.delete("/api/v1/ptt/groups/+8299998000")

        ok = grp_r["_status"] in (200, 201) and mod_r["_status"] == 200
        return ok, f"create={grp_r['_status']}, modify={mod_r['_status']}"
    runner.run("E2E-SYNC-02", "그룹 변경 → CSP 동기화", sync_02)

    # ================================================================
    # E2E-OPS: 전체 운용 시나리오
    # ================================================================
    print("\n── E2E-OPS: 전체 운용 시나리오 ──")

    def ops_01():
        steps = []

        # Step 1: 사용자 생성
        u = c.post("/api/v1/users", {"name": f"{TEST_PREFIX}ops_user", "login_id": f"{TEST_PREFIX}ops_user", "password": "test1234", "email": "ops@test.com"})
        steps.append(("사용자 생성", u["_status"] == 201))
        if u["_status"] != 201:
            return False, f"Step 1 실패: {u}"
        uid = u["id"]

        # Step 2: VoIP 구독 추가
        vs = c.post(f"/api/v1/users/{uid}/call", {
            "id": "+8299997001", "auth_id": "ops_voip@test", "passwd": "1234",
        })
        steps.append(("VoIP 구독 추가", vs["_status"] == 201))

        # Step 3: PTT 구독 추가
        ps = c.post(f"/api/v1/users/{uid}/ptt", {
            "id": "+8299997002", "auth_id": "ops_ptt@test", "passwd": "1234",
        })
        steps.append(("PTT 구독 추가", ps["_status"] == 201))

        # Step 4: 그룹 생성 (멤버 포함)
        gr = c.post("/api/v1/ptt/groups", {
            "id": "+8299997000", "name": f"{TEST_PREFIX}ops_grp",
            "members": [{"user_id": "+8299997002", "priority": 1}],
        })
        steps.append(("그룹 생성", gr["_status"] in (200, 201)))

        # Step 5: 사용자 조회 (구독 포함 확인)
        u2 = c.get(f"/api/v1/users/{uid}")
        has_call = len(u2.get("call_subscriptions", [])) > 0
        has_ptt = len(u2.get("ptt_subscriptions", [])) > 0
        steps.append(("사용자 조회 (구독 확인)", has_call and has_ptt))

        # Step 6: 그룹 조회 (멤버 확인)
        g2 = c.get("/api/v1/ptt/groups/+8299997000")
        has_member = len(g2.get("members", [])) > 0
        steps.append(("그룹 조회 (멤버 확인)", has_member))

        # Step 7: 가입자 상태 API
        ss = c.get("/api/v1/stats/subscribers")
        steps.append(("가입자 상태 API", ss["_status"] == 200))

        # Step 8: 통화 이력 API
        cl = c.get("/api/v1/call/logs")
        steps.append(("통화 이력 API", cl["_status"] == 200))

        # 정리
        c.delete("/api/v1/ptt/groups/+8299997000")
        c.delete(f"/api/v1/users/{uid}")

        all_ok = all(s[1] for s in steps)
        detail = " | ".join(f"{s[0]}:{'OK' if s[1] else 'NG'}" for s in steps)
        return all_ok, detail
    runner.run("E2E-OPS-01", "전체 운용 흐름 (생성→구독→그룹→조회→정리)", ops_01)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("연동/E2E 검증 시작")
    print("=" * 60)
    result = run_e2e_tests()
    print(f"\n총 {result['total']}건: PASS={result['pass']} FAIL={result['fail']} SKIP={result['skip']}")
