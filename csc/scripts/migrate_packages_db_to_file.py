#!/usr/bin/env python3
"""DB `cims_package` → file_store('packages') 마이그레이션.

사용:
  CIMS_CSC_CONFIG=/path/to/csc.json python3 csc/scripts/migrate_packages_db_to_file.py

옵션:
  --rename-legacy   완료 후 DB 테이블을 `cims_package_legacy` 로 rename (안전한 1단계 DROP 준비)
  --dry-run         읽기만 하고 파일을 쓰지 않음

idempotent: 동일 (name, version) 가 file_store 에 이미 있으면 id 보존.
"""
import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_CSC_SRC = os.path.normpath(os.path.join(_HERE, '..', 'src'))
sys.path.insert(0, _CSC_SRC)

import pymysql  # type: ignore
import pymysql.cursors  # type: ignore

from services import file_store  # type: ignore


def _load_config() -> dict:
    cfg_path = os.environ.get('CIMS_CSC_CONFIG')
    if not cfg_path or not os.path.isfile(cfg_path):
        print(f"[error] CIMS_CSC_CONFIG 환경변수 미설정 또는 파일 없음: {cfg_path}", file=sys.stderr)
        sys.exit(1)
    with open(cfg_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def _connect(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rename-legacy', action='store_true',
                    help='완료 후 cims_package → cims_package_legacy 로 rename')
    ap.add_argument('--dry-run', action='store_true', help='읽기만 (파일 쓰지 않음)')
    args = ap.parse_args()

    config = _load_config()
    domain = file_store.domain_dir(config, 'packages')
    print(f"[info] runtime store: {file_store.runtime_root(config)}")
    print(f"[info] domain dir   : {domain}")

    conn = _connect(config)
    rows = []
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM cims_package ORDER BY id")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] cims_package 테이블 없음 (이미 rename/DROP?): {e}")
                rows = []
    finally:
        conn.close()

    print(f"[info] DB rows: {len(rows)}")

    migrated = 0
    max_id = 0
    for r in rows:
        pid = int(r['id'])
        name = r.get('name') or ''
        version = r.get('version') or ''
        if not name or not version:
            print(f"[skip] id={pid}: name/version 비어있음")
            continue

        # JSON 컬럼 정상화
        def _normalize_json(raw):
            if raw is None: return None
            if isinstance(raw, (dict, list)): return raw
            try: return json.loads(raw)
            except Exception: return None

        ua = r.get('uploaded_at')
        if hasattr(ua, 'isoformat'):
            ua = ua.isoformat()

        obj = {
            'id': pid,
            'name': name,
            'version': version,
            'file_path': r.get('file_path'),
            'file_size': r.get('file_size'),
            'sha256': r.get('sha256'),
            'description': r.get('description'),
            'uploaded_by': r.get('uploaded_by'),
            'uploaded_at': ua,
            'meta': _normalize_json(r.get('meta_json')),
            'config_template': _normalize_json(r.get('config_template_json')),
        }
        key = f"{name}__{version}"
        if args.dry_run:
            print(f"[dry] would write {key}.json (id={pid})")
        else:
            file_store.save(domain, key, obj)
            print(f"[ok]  wrote     {key}.json (id={pid})")
            migrated += 1
        if pid > max_id:
            max_id = pid

    if not args.dry_run and max_id > 0:
        file_store.seed_seq(domain, max_id)
        print(f"[info] .seq seeded to {max_id} (next_id → {max_id + 1})")

    if args.rename_legacy and not args.dry_run and rows:
        conn = _connect(config)
        try:
            with conn.cursor() as cur:
                cur.execute("RENAME TABLE cims_package TO cims_package_legacy")
            print("[info] DB table renamed: cims_package → cims_package_legacy")
        except Exception as e:
            print(f"[error] rename 실패: {e}")
        finally:
            conn.close()

    print(f"[done] migrated={migrated}/{len(rows)}")


if __name__ == '__main__':
    main()
