#!/usr/bin/env python3
"""DB `ha_groups` + `ha_group_members` → file_store('ha_groups') 마이그레이션.

각 그룹은 members 배열을 임베드한 1 JSON 파일.

사용:
  CIMS_CSC_CONFIG=/path/to/csc.json python3 csc/scripts/migrate_ha_groups_db_to_file.py
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
        print(f"[error] CIMS_CSC_CONFIG 미설정: {cfg_path}", file=sys.stderr)
        sys.exit(1)
    with open(cfg_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def _connect(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'), port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'), password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4', cursorclass=pymysql.cursors.DictCursor, autocommit=True,
    )


def _safe_json(raw):
    if raw is None: return None
    if isinstance(raw, (dict, list)): return raw
    try: return json.loads(raw)
    except Exception: return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rename-legacy', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    config = _load_config()
    print(f"[info] runtime store: {file_store.runtime_root(config)}")
    domain = file_store.domain_dir(config, 'ha_groups')
    print(f"[info] ha_groups dir: {domain}")

    groups_rows = []
    members_rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM ha_groups ORDER BY id")
                groups_rows = cur.fetchall()
                cur.execute("SELECT group_id, agent_id, priority, role FROM ha_group_members")
                members_rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] ha_groups* 테이블 없음: {e}")
                return
    finally:
        conn.close()

    members_by_gid: dict = {}
    for m in members_rows:
        gid = int(m['group_id'])
        members_by_gid.setdefault(gid, []).append({
            'agent_id': int(m['agent_id']),
            'priority': int(m['priority'] or 0),
            'role': m.get('role') or 'backup',
        })

    print(f"[info] ha_groups rows: {len(groups_rows)} / ha_group_members rows: {len(members_rows)}")
    max_id = 0
    for r in groups_rows:
        gid = int(r['id'])
        members = members_by_gid.get(gid, [])
        members.sort(key=lambda m: -int(m['priority']))
        obj = {
            'id': gid,
            'name': r.get('name'),
            'mode': r.get('mode'),
            'vip': r.get('vip'),
            'vrid': r.get('vrid'),
            'vip_mask': r.get('vip_mask'),
            'auth_pass': r.get('auth_pass'),
            'note': r.get('note'),
            'vip_bindings': _safe_json(r.get('vip_bindings_json')) or [],
            'members': members,
        }
        if args.dry_run:
            print(f"[dry] ha_group id={gid} name={obj['name']} mode={obj['mode']} members={len(members)}")
        else:
            file_store.save(domain, gid, obj)
            print(f"[ok]  ha_group id={gid} name={obj['name']} mode={obj['mode']} members={len(members)}")
        max_id = max(max_id, gid)
    if not args.dry_run and max_id:
        file_store.seed_seq(domain, max_id)
        print(f"[info] ha_groups/.seq seeded to {max_id}")

    if args.rename_legacy and not args.dry_run and groups_rows:
        conn = _connect(config)
        try:
            with conn.cursor() as cur:
                cur.execute("RENAME TABLE ha_group_members TO ha_group_members_legacy")
                cur.execute("RENAME TABLE ha_groups TO ha_groups_legacy")
                print("[info] ha_group* → *_legacy")
        except Exception as e:
            print(f"[error] rename 실패: {e}")
        finally:
            conn.close()
    print(f"[done] groups={len(groups_rows)} members={len(members_rows)}")


if __name__ == '__main__':
    main()
