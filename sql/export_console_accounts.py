#!/usr/bin/env python3
"""
export_console_accounts.py  (2026-06-15)

기존 배포의 DB users 테이블에 콘솔 로그인 계정(role IN admin/manager/operator/
monitor)이 있다면, migrate_users_person_only.sql 로 컬럼을 제거하기 전에 OAM
file_store 도메인 console_accounts 로 내보낸다.

  - role='user' (가입자) 는 콘솔 로그인 대상이 아니므로 제외한다.
  - 이미 console_accounts 에 같은 login_id 가 있으면 건너뛴다(덮어쓰지 않음).
  - password 는 DB 의 SHA-256 해시를 그대로 password_sha256 으로 옮긴다.

사용:
  python3 export_console_accounts.py \
      --host 127.0.0.1 --port 3306 --user cims --password '***' --db cims \
      --runtime-dir /path/to/oam/runtime          # console_accounts 디렉터리의 부모
  (--runtime-dir 미지정 시 oam.json 의 CimsRuntimeDir 사용: --oam-config 로 지정)
"""
import argparse
import json
import os
import sys
from datetime import datetime


def _load_pymysql():
    try:
        import pymysql  # noqa
        return pymysql
    except ImportError:
        here = os.path.dirname(os.path.abspath(__file__))
        for cand in (os.path.join(here, '..', 'oam', 'vendor'),
                     os.path.join(here, '..', 'csc', 'vendor')):
            if os.path.isdir(os.path.join(cand, 'pymysql')):
                sys.path.insert(0, os.path.abspath(cand))
                import pymysql  # noqa
                return pymysql
        raise


def _runtime_dir(args):
    if args.runtime_dir:
        return args.runtime_dir
    if args.oam_config and os.path.isfile(args.oam_config):
        cfg = json.load(open(args.oam_config))
        rt = cfg.get('CimsRuntimeDir')
        if rt:
            return rt
    raise SystemExit("runtime 디렉터리를 결정할 수 없습니다 — --runtime-dir 또는 --oam-config 지정")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=3306)
    ap.add_argument('--user', default='cims')
    ap.add_argument('--password', default='')
    ap.add_argument('--db', default='cims')
    ap.add_argument('--runtime-dir', default='')
    ap.add_argument('--oam-config', default='')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    pymysql = _load_pymysql()
    dest = os.path.join(_runtime_dir(args), 'console_accounts')

    conn = pymysql.connect(host=args.host, port=args.port, user=args.user,
                           password=args.password, database=args.db,
                           charset='utf8mb4', cursorclass=pymysql.cursors.DictCursor)
    with conn.cursor() as cur:
        # role 컬럼이 이미 제거됐으면 내보낼 것이 없음.
        cur.execute("SELECT COUNT(*) AS c FROM information_schema.columns "
                    "WHERE table_schema=%s AND table_name='users' AND column_name='role'", (args.db,))
        if cur.fetchone()['c'] == 0:
            print("users.role 컬럼 없음 — 이미 person 전용. 내보낼 콘솔 계정 없음.")
            return
        cur.execute("SELECT id, name, login_id, password, role, email "
                    "FROM users WHERE role <> 'user' AND login_id <> ''")
        rows = cur.fetchall()

    if not rows:
        print("DB users 에 콘솔 로그인 계정 없음 — 이관 불필요.")
        return

    print(f"콘솔 계정 {len(rows)}건 발견 → {dest}")
    if not args.dry_run:
        os.makedirs(dest, exist_ok=True)
    now = datetime.now().isoformat(timespec='seconds')
    moved = skipped = 0
    for r in rows:
        lid = r['login_id']
        path = os.path.join(dest, lid.replace('/', '_') + '.json')
        if os.path.exists(path):
            print(f"  skip (이미 존재): {lid}")
            skipped += 1
            continue
        rec = {'login_id': lid, 'name': r.get('name') or lid, 'role': r['role'],
               'email': r.get('email') or '', 'password_sha256': r.get('password') or '',
               'create_time': now, 'update_time': now}
        print(f"  export: {lid} ({r['role']})")
        if not args.dry_run:
            tmp = path + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(rec, f, ensure_ascii=False, indent=2)
            os.replace(tmp, path)
        moved += 1
    print(f"완료 — 이관 {moved}, 건너뜀 {skipped}{' (dry-run)' if args.dry_run else ''}")


if __name__ == '__main__':
    main()
