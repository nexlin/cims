"""
검증 환경 초기화: 서비스 종료 → DB/로그/녹취 정리 → 서비스 재시작
"""
import subprocess, time, os, sys

CIMS_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CIMS_SH = os.path.join(CIMS_ROOT, "cims.sh")
DIST_DIR = os.path.join(CIMS_ROOT, "build", "dist")


def run(cmd, desc=""):
    if desc:
        print(f"  {desc}...", end=" ", flush=True)
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30, cwd=CIMS_ROOT)
    if desc:
        print("OK" if r.returncode == 0 else f"WARN({r.returncode})")
    return r


def clean_and_restart():
    print("\n" + "=" * 60)
    print("  검증 환경 초기화")
    print("=" * 60)

    # 1. 전체 서비스 종료
    print("\n[1/4] 서비스 종료")
    run(f"{CIMS_SH} stop all", "전체 서비스 종료")
    time.sleep(2)

    # 2. DB 정리 (통화이력, 참여자 테이블 초기화)
    print("\n[2/4] DB 정리")
    import pymysql
    try:
        conn = pymysql.connect(
            host="127.0.0.1", port=3306, user="cims", password="cims1234",
            database="cims", charset="utf8mb4", autocommit=True,
            cursorclass=pymysql.cursors.DictCursor,
        )
        with conn.cursor() as cur:
            for tbl in [
                "voip_call_participants", "ptt_call_participants",
                "voip_call_logs", "ptt_call_logs",
            ]:
                cur.execute(f"DELETE FROM {tbl}")
                print(f"    {tbl} 초기화 ({cur.rowcount}건 삭제)")

            # VoIP 구독: 빈 비밀번호를 '123456'으로, auth_id 패턴 수정
            cur.execute("UPDATE voip_subscriptions SET passwd='123456' WHERE passwd='' OR passwd IS NULL")
            if cur.rowcount > 0:
                print(f"    VoIP passwd 수정: {cur.rowcount}건")
            # auth_id에 @가 없는 행 수정 (참조 패턴: 450033100000NNN@domain)
            cur.execute("SELECT id, auth_id FROM voip_subscriptions WHERE auth_id NOT LIKE '%%@%%'")
            for row in cur.fetchall():
                msisdn = row['id']
                # 동일 시리즈의 정상 행에서 auth_id 패턴을 참조
                # +821357007002 → 450033100000002, +821357007003 → 450033100000003
                # 마지막 숫자를 교체
                cur.execute(
                    "SELECT auth_id FROM voip_subscriptions WHERE id != %s AND auth_id LIKE '%%@%%' LIMIT 1",
                    (msisdn,)
                )
                ref = cur.fetchone()
                if ref:
                    ref_auth = ref['auth_id']
                    # 참조 auth_id의 마지막 숫자를 현재 msisdn의 마지막 숫자로 교체
                    at_pos = ref_auth.index('@')
                    prefix = ref_auth[:at_pos]
                    suffix = ref_auth[at_pos:]
                    # prefix 끝의 숫자를 msisdn 끝 숫자로 교체
                    last = msisdn[-1]
                    auth_id = prefix[:-1] + last + suffix
                else:
                    auth_id = msisdn + "@ims.mnc033.mcc450.3gppnetwork.org"
                cur.execute("UPDATE voip_subscriptions SET auth_id=%s WHERE id=%s", (auth_id, msisdn))
                print(f"    VoIP auth_id 수정: {msisdn} → {auth_id}")

            # 녹취 테이블 존재 확인 및 생성
            cur.execute("SHOW TABLES LIKE 'recordings'")
            if not cur.fetchone():
                print("    recordings/recording_segments 테이블 생성...")
                sql_path = os.path.join(CIMS_ROOT, "sql", "migrate_recordings.sql")
                if os.path.exists(sql_path):
                    with open(sql_path) as sf:
                        stmts = sf.read().split(';')
                        for stmt in stmts:
                            stmt = stmt.strip()
                            if stmt:
                                cur.execute(stmt)
                    print("    녹취 테이블 생성 완료")
            else:
                # 녹취 테이블 초기화
                cur.execute("DELETE FROM recording_segments")
                print(f"    recording_segments 초기화 ({cur.rowcount}건 삭제)")
                cur.execute("DELETE FROM recordings")
                print(f"    recordings 초기화 ({cur.rowcount}건 삭제)")

            # register_time / logout_time 초기화
            cur.execute("UPDATE voip_subscriptions SET register_time=NULL, logout_time=NULL")
            print(f"    voip_subscriptions register/logout 초기화")
            cur.execute("UPDATE ptt_subscriptions SET register_time=NULL, logout_time=NULL")
            print(f"    ptt_subscriptions register/logout 초기화")
        conn.close()
        print("  DB 정리 완료")
    except Exception as e:
        print(f"  DB 정리 실패: {e}")

    # 3. 파일 정리 (로그, 녹취, 메시지 로그)
    print("\n[3/4] 파일 정리")
    dirs_to_clean = [
        os.path.join(DIST_DIR, "log"),
        os.path.join(DIST_DIR, "csc", "log"),
        os.path.join(DIST_DIR, "ext_mnt", "recordings"),
        os.path.join(DIST_DIR, "ext_mnt", "call_log"),
    ]
    for d in dirs_to_clean:
        if os.path.isdir(d):
            run(f"find {d} -type f -name '*.log' -o -name '*.rtp' -o -name '*.jsonl' -o -name '*.csv' | xargs rm -f 2>/dev/null",
                f"{os.path.basename(d)} 정리")
        # 로그 파일 재생성 (빈 파일)
        if d == os.path.join(DIST_DIR, "log"):
            for f in ["csp.log", "cmp.log", "csc.log", "cwrtc.log"]:
                open(os.path.join(d, f), "w").close()

    # 4. 서비스 재시작
    print("\n[4/4] 서비스 시작")
    run(f"{CIMS_SH} start cmp", "CMP 시작")
    time.sleep(1)
    run(f"{CIMS_SH} start csp", "CSP 시작")
    time.sleep(1)
    run(f"{CIMS_SH} start csc", "CSC 시작")
    time.sleep(2)

    # 상태 확인
    print("\n  서비스 상태:")
    r = run(f"{CIMS_SH} status")
    for line in r.stdout.split("\n"):
        if "●" in line:
            print(f"  {line.strip()}")

    print("\n  초기화 완료\n")


if __name__ == "__main__":
    clean_and_restart()
