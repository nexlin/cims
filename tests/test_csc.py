"""
CSC 모듈 검증: REST API 전체 엔드포인트 테스트
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

from conftest import CscClient, TestRunner, TEST_PREFIX, TEST_VOIP_MSISDN, TEST_PTT_MSISDN, TEST_GROUP_ID


def run_csc_tests():
    runner = TestRunner("CSC")
    c = CscClient()

    # 테스트용 상태 보관
    state = {}

    # ================================================================
    # CSC-AUTH: 인증 API
    # ================================================================
    print("\n── CSC-AUTH: 인증 API ──")

    def auth_01():
        r = c.login()
        return r["_status"] == 200 and "token" in r, f"status={r['_status']}"
    runner.run("CSC-AUTH-01", "로그인 성공", auth_01)

    def auth_02():
        c2 = CscClient()
        r = c2.post("/api/v1/auth/login", {"login_id": "admin", "password": "wrong_password_xyz"})
        return r["_status"] == 401, f"status={r['_status']}"
    runner.run("CSC-AUTH-02", "로그인 실패 (잘못된 비밀번호)", auth_02)

    def auth_03():
        r = c.get("/api/v1/auth/me")
        return r["_status"] == 200 and "login_id" in r, f"status={r['_status']}, keys={list(r.keys())}"
    runner.run("CSC-AUTH-03", "세션 조회 (me)", auth_03)

    def auth_04():
        # 테스트용 사용자 생성 후 비밀번호 변경
        c2 = CscClient()
        reg = c2.post("/api/v1/auth/register", {
            "name": f"{TEST_PREFIX}pwuser", "login_id": f"{TEST_PREFIX}pwuser", "password": "old1234"
        })
        if reg["_status"] not in (200, 201):
            # 이미 존재하면 로그인
            c2.login(f"{TEST_PREFIX}pwuser", "old1234")
        else:
            c2.token = reg.get("token")
        r = c2.put("/api/v1/auth/password", {"old_password": "old1234", "new_password": "new1234"})
        # 새 비밀번호로 로그인 확인
        c3 = CscClient()
        r2 = c3.post("/api/v1/auth/login", {"login_id": f"{TEST_PREFIX}pwuser", "password": "new1234"})
        # 원래대로 복원
        c3.token = r2.get("token")
        c3.put("/api/v1/auth/password", {"old_password": "new1234", "new_password": "old1234"})
        return r["_status"] == 200 and r2["_status"] == 200, f"change={r['_status']}, relogin={r2['_status']}"
    runner.run("CSC-AUTH-04", "비밀번호 변경", auth_04)

    # ================================================================
    # CSC-USER: 가입자 관리
    # ================================================================
    print("\n── CSC-USER: 가입자 관리 ──")

    def user_01():
        r = c.post("/api/v1/users", {"name": f"{TEST_PREFIX}user1", "login_id": f"{TEST_PREFIX}user1", "password": "test1234", "email": "test@test.com"})
        if r["_status"] == 201:
            state["user_id"] = r["id"]
        return r["_status"] == 201 and r.get("id", 0) > 0, f"status={r['_status']}, id={r.get('id')}"
    runner.run("CSC-USER-01", "가입자 생성", user_01)

    def user_02():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.get(f"/api/v1/users/{uid}")
        return r["_status"] == 200 and r.get("name", "").startswith(TEST_PREFIX), f"status={r['_status']}, name={r.get('name')}"
    runner.run("CSC-USER-02", "가입자 조회", user_02)

    def user_03():
        r = c.get("/api/v1/users")
        return r["_status"] == 200 and isinstance(r.get("users"), list), f"status={r['_status']}, count={len(r.get('users', []))}"
    runner.run("CSC-USER-03", "가입자 목록 조회", user_03)

    def user_04():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.put(f"/api/v1/users/{uid}", {"name": f"{TEST_PREFIX}user1_mod"})
        r2 = c.get(f"/api/v1/users/{uid}")
        return r["_status"] == 200 and r2.get("name") == f"{TEST_PREFIX}user1_mod", f"status={r['_status']}, name={r2.get('name')}"
    runner.run("CSC-USER-04", "가입자 수정", user_04)

    # ================================================================
    # CSC-VSUB: VoIP 구독 관리
    # ================================================================
    print("\n── CSC-VSUB: VoIP 구독 관리 ──")

    def vsub_01():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.post(f"/api/v1/users/{uid}/call", {
            "id": TEST_VOIP_MSISDN,
            "auth_id": f"test@ims.test",
            "passwd": "1234",
        })
        return r["_status"] == 201, f"status={r['_status']}"
    runner.run("CSC-VSUB-01", "VoIP 구독 추가", vsub_01)

    def vsub_02():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.get(f"/api/v1/users/{uid}/call")
        subs = r.get("subscriptions", [])
        return r["_status"] == 200 and len(subs) > 0, f"status={r['_status']}, count={len(subs)}"
    runner.run("CSC-VSUB-02", "VoIP 구독 조회", vsub_02)

    def vsub_03():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.put(f"/api/v1/users/{uid}/call/{TEST_VOIP_MSISDN}", {"dnd": True})
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("CSC-VSUB-03", "VoIP 구독 수정 (DND)", vsub_03)

    def vsub_04():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.delete(f"/api/v1/users/{uid}/call/{TEST_VOIP_MSISDN}")
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("CSC-VSUB-04", "VoIP 구독 삭제", vsub_04)

    # ================================================================
    # CSC-PSUB: PTT 구독 관리
    # ================================================================
    print("\n── CSC-PSUB: PTT 구독 관리 ──")

    def psub_01():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.post(f"/api/v1/users/{uid}/ptt", {
            "id": TEST_PTT_MSISDN,
            "auth_id": f"test@ptt.test",
            "passwd": "1234",
        })
        return r["_status"] == 201, f"status={r['_status']}"
    runner.run("CSC-PSUB-01", "PTT 구독 추가", psub_01)

    def psub_02():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.get(f"/api/v1/users/{uid}/ptt")
        subs = r.get("subscriptions", [])
        return r["_status"] == 200 and len(subs) > 0, f"status={r['_status']}, count={len(subs)}"
    runner.run("CSC-PSUB-02", "PTT 구독 조회", psub_02)

    def psub_03():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.put(f"/api/v1/users/{uid}/ptt/{TEST_PTT_MSISDN}", {"dnd": True})
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("CSC-PSUB-03", "PTT 구독 수정 (DND)", psub_03)

    def psub_04():
        uid = state.get("user_id")
        if not uid:
            return False, "선행 테스트 실패"
        r = c.delete(f"/api/v1/users/{uid}/ptt/{TEST_PTT_MSISDN}")
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("CSC-PSUB-04", "PTT 구독 삭제", psub_04)

    # ================================================================
    # CSC-GRP: PTT 그룹 관리
    # ================================================================
    print("\n── CSC-GRP: PTT 그룹 관리 ──")

    def grp_01():
        r = c.post("/api/v1/ptt/groups", {
            "id": TEST_GROUP_ID,
            "name": f"{TEST_PREFIX}group1",
            "members": [],
        })
        return r["_status"] in (200, 201), f"status={r['_status']}"
    runner.run("CSC-GRP-01", "PTT 그룹 생성", grp_01)

    def grp_02():
        r = c.get(f"/api/v1/ptt/groups/{TEST_GROUP_ID}")
        return r["_status"] == 200 and "name" in r, f"status={r['_status']}"
    runner.run("CSC-GRP-02", "PTT 그룹 조회", grp_02)

    def grp_03():
        r = c.put(f"/api/v1/ptt/groups/{TEST_GROUP_ID}", {"name": f"{TEST_PREFIX}group1_mod"})
        return r["_status"] == 200, f"status={r['_status']}"
    runner.run("CSC-GRP-03", "PTT 그룹 수정", grp_03)

    def grp_04():
        r = c.delete(f"/api/v1/ptt/groups/{TEST_GROUP_ID}")
        r2 = c.get(f"/api/v1/ptt/groups/{TEST_GROUP_ID}")
        return r["_status"] == 200 and r2["_status"] == 404, f"delete={r['_status']}, verify={r2['_status']}"
    runner.run("CSC-GRP-04", "PTT 그룹 삭제", grp_04)

    # ================================================================
    # CSC-STAT: 통계/헬스
    # ================================================================
    print("\n── CSC-STAT: 통계/헬스 ──")

    def stat_01():
        r = c.get("/api/v1/stats/health")
        h = r.get("health", {})
        return r["_status"] == 200 and "db" in h, f"status={r['_status']}, health={h}"
    runner.run("CSC-STAT-01", "헬스체크", stat_01)

    def stat_02():
        r = c.get("/api/v1/stats/subscribers")
        return r["_status"] == 200 and "subscribers" in r, f"status={r['_status']}"
    runner.run("CSC-STAT-02", "가입자 상태 조회", stat_02)

    def stat_03():
        r = c.get("/api/v1/stats/service/summary")
        return r["_status"] == 200 and "granularity" in r, f"status={r['_status']}, keys={list(r.keys())}"
    runner.run("CSC-STAT-03", "서비스 통계 조회", stat_03)

    # ================================================================
    # 정리: 테스트 데이터 삭제
    # ================================================================
    uid = state.get("user_id")
    if uid:
        c.delete(f"/api/v1/users/{uid}")

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("CSC 모듈 검증 시작")
    print("=" * 60)
    result = run_csc_tests()
    print(f"\n총 {result['total']}건: PASS={result['pass']} FAIL={result['fail']} SKIP={result['skip']}")
