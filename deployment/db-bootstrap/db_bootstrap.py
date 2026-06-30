#!/usr/bin/env python3
"""
CIMS DB 부트스트랩 — 최초 DB/계정/스키마 생성기 (대화식).

cims-bootstrap(운영평면 인스톨러)와 짝을 이루는 DB 초기 생성 도구. 실행 과정에서
DB 접속 정보를 입력받아: ① 데이터베이스 생성 ② 앱 계정 생성/권한 부여
③ 통합 스키마(cims_schema.sql) 적용 ④ (선택) 미사용 테이블 정리 를 수행한다.

설계:
  - air-gapped(private) 환경 대응 — 시스템 mysql 클라이언트 불필요. 동봉/저장소
    vendored pymysql(순수 파이썬) 사용.
  - 멱등: 스키마는 CREATE ... IF NOT EXISTS, 계정/DB 도 존재 시 건너뜀 → 재실행 안전.
  - 비대화식(--yes + 플래그/환경변수)도 지원 (CI/자동화).

대화식:   python3 db_bootstrap.py
비대화식: python3 db_bootstrap.py --yes \
            --host 127.0.0.1 --port 3306 \
            --admin-user root --admin-pass '***' \
            --db cims --app-user cims --app-pass '***' --grant-host '%'

환경변수(플래그 미지정 시): CIMS_DB_HOST/PORT, CIMS_DB_ADMIN_USER/ADMIN_PASS,
  CIMS_DB_NAME, CIMS_DB_APP_USER/APP_PASS, CIMS_DB_GRANT_HOST.
"""
import argparse
import getpass
import os
import re
import sys


# ── vendored pymysql 로드 (air-gapped) ──────────────────────────────
def _load_pymysql():
    try:
        import pymysql  # noqa
        import pymysql.cursors  # noqa
        return pymysql
    except ImportError:
        here = os.path.dirname(os.path.abspath(__file__))
        cands = [
            os.path.join(here, 'vendor'),                 # 동봉(패키지)
            os.path.join(here, '..', '..', 'oam', 'vendor'),
            os.path.join(here, '..', '..', 'csc', 'vendor'),
            '/opt/cims-agent/oam/vendor',
        ]
        for c in cands:
            if os.path.isdir(os.path.join(c, 'pymysql')):
                sys.path.insert(0, os.path.abspath(c))
                import pymysql  # noqa
                import pymysql.cursors  # noqa
                return pymysql
        sys.exit("ERROR: pymysql 모듈을 찾을 수 없습니다 (vendor/ 동봉 또는 pip install pymysql)")


def _locate(name, override):
    """스키마/마이그레이션 .sql 파일 탐색."""
    here = os.path.dirname(os.path.abspath(__file__))
    cands = []
    if override:
        cands.append(override)
    cands += [
        os.path.join(here, name),                          # 동봉
        os.path.join(here, '..', '..', 'sql', name),       # 저장소
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def _split_sql(text):
    """주석/빈 줄 제거 후 ';' 단위로 statement 분리.
    (스키마에 ';' 는 statement 종결자로만 쓰임 — ENUM 의 ',' 는 영향 없음.)"""
    lines = []
    for ln in text.splitlines():
        s = ln.strip()
        if not s or s.startswith('--'):
            continue
        lines.append(ln)
    body = '\n'.join(lines)
    return [st.strip() for st in body.split(';') if st.strip()]


def _prompt(label, default='', secret=False, noninteractive=False):
    if noninteractive:
        return default
    if secret:
        v = getpass.getpass(f"{label}: ")
        return v if v else default
    suffix = f" [{default}]" if default else ""
    v = input(f"{label}{suffix}: ").strip()
    return v if v else default


def main():
    ap = argparse.ArgumentParser(description="CIMS DB 부트스트랩 (대화식)")
    ap.add_argument('--host'); ap.add_argument('--port', type=int)
    ap.add_argument('--admin-user'); ap.add_argument('--admin-pass')
    ap.add_argument('--db'); ap.add_argument('--app-user'); ap.add_argument('--app-pass')
    ap.add_argument('--grant-host')
    ap.add_argument('--schema', help='cims_schema.sql 경로 (기본: 동봉/저장소 자동탐색)')
    ap.add_argument('--cleanup', action='store_true', help='기존 DB 의 미사용 테이블 정리도 적용')
    ap.add_argument('--yes', '-y', action='store_true', help='비대화식 — 프롬프트 없이 플래그/환경변수/기본값 사용')
    args = ap.parse_args()
    ni = args.yes

    E = os.environ.get
    host  = args.host       or _prompt("DB 호스트", E('CIMS_DB_HOST', '127.0.0.1'), noninteractive=ni)
    port  = args.port       or int(_prompt("DB 포트", E('CIMS_DB_PORT', '3306'), noninteractive=ni) or 3306)
    admin = args.admin_user or _prompt("관리자(생성권한) 계정", E('CIMS_DB_ADMIN_USER', 'root'), noninteractive=ni)
    apass = args.admin_pass if args.admin_pass is not None else \
            _prompt("관리자 비밀번호", E('CIMS_DB_ADMIN_PASS', ''), secret=True, noninteractive=ni)
    dbname = args.db        or _prompt("생성할 DB 이름", E('CIMS_DB_NAME', 'cims'), noninteractive=ni)
    appusr = args.app_user  or _prompt("앱(서비스) 계정", E('CIMS_DB_APP_USER', 'cims'), noninteractive=ni)
    apppw  = args.app_pass if args.app_pass is not None else \
             _prompt("앱 계정 비밀번호", E('CIMS_DB_APP_PASS', ''), secret=True, noninteractive=ni)
    ghost  = args.grant_host or _prompt("앱 계정 접속 허용 host (% = 모든 호스트)",
                                        E('CIMS_DB_GRANT_HOST', '%'), noninteractive=ni)

    if not apppw and not ni:
        # 확인 입력
        apppw2 = _prompt("앱 계정 비밀번호 (확인)", '', secret=True)
        if apppw != apppw2:
            sys.exit("ERROR: 앱 계정 비밀번호가 일치하지 않습니다")

    schema_path = _locate('cims_schema.sql', args.schema)
    if not schema_path:
        sys.exit("ERROR: cims_schema.sql 을 찾을 수 없습니다 (--schema 로 지정)")

    print("\n── 설정 확인 ─────────────────────────────")
    print(f"  대상       : {admin}@{host}:{port}")
    print(f"  DB 이름    : {dbname}")
    print(f"  앱 계정    : {appusr}@{ghost}")
    print(f"  스키마     : {schema_path}")
    print(f"  미사용정리 : {'예' if args.cleanup else '아니오'}")
    print("──────────────────────────────────────────")
    if not ni:
        if input("진행할까요? [y/N]: ").strip().lower() not in ('y', 'yes'):
            sys.exit("중단했습니다.")

    pymysql = _load_pymysql()

    # (1) 관리자로 접속 — DB 미선택.
    try:
        conn = pymysql.connect(host=host, port=port, user=admin, password=apass,
                               charset='utf8mb4', autocommit=True)
    except Exception as e:
        sys.exit(f"ERROR: 관리자 접속 실패 — {e}")

    with conn.cursor() as cur:
        # (2) DB 생성
        cur.execute(f"CREATE DATABASE IF NOT EXISTS `{dbname}` "
                    f"CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci")
        print(f"✓ database `{dbname}` ready")

        # (3) 앱 계정 + 권한 (멱등). 비밀번호 지정 시 갱신.
        if appusr:
            cur.execute(f"CREATE USER IF NOT EXISTS '{appusr}'@'{ghost}' IDENTIFIED BY %s", (apppw,))
            if apppw:
                cur.execute(f"ALTER USER '{appusr}'@'{ghost}' IDENTIFIED BY %s", (apppw,))
            cur.execute(f"GRANT ALL PRIVILEGES ON `{dbname}`.* TO '{appusr}'@'{ghost}'")
            cur.execute("FLUSH PRIVILEGES")
            print(f"✓ user '{appusr}'@'{ghost}' granted on `{dbname}`")

        # (4) 스키마 적용
        cur.execute(f"USE `{dbname}`")
        with open(schema_path, encoding='utf-8') as f:
            stmts = _split_sql(f.read())
        applied = 0
        for st in stmts:
            # 스키마 파일의 CREATE DATABASE/USE 는 위에서 처리했으므로 건너뜀.
            if re.match(r'(?i)^\s*(CREATE\s+DATABASE|USE)\b', st):
                continue
            cur.execute(st)
            applied += 1
        print(f"✓ schema applied ({applied} statements) ← {os.path.basename(schema_path)}")

        # (5) (선택) 미사용 테이블 정리
        if args.cleanup:
            cpath = _locate('migrate_drop_unused_tables.sql', None)
            if not cpath:
                print("⚠ migrate_drop_unused_tables.sql 미발견 — 정리 건너뜀")
            else:
                cur.execute("SET FOREIGN_KEY_CHECKS=0")
                with open(cpath, encoding='utf-8') as f:
                    for st in _split_sql(f.read()):
                        if re.match(r'(?i)^\s*(USE|SET)\b', st):
                            continue
                        cur.execute(st)
                cur.execute("SET FOREIGN_KEY_CHECKS=1")
                print(f"✓ cleanup applied ← {os.path.basename(cpath)}")

        # 요약
        cur.execute("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=%s", (dbname,))
        n = cur.fetchone()[0]
        print(f"\n완료 — `{dbname}` 테이블 {n}개.")

    conn.close()


if __name__ == '__main__':
    main()
