#!/usr/bin/env python3
"""DB `cims_instance` + `cims_agent` → file_store('instances', 'agents') 마이그레이션.

사용:
  CIMS_CSC_CONFIG=/path/to/csc.json python3 csc/scripts/migrate_agents_db_to_file.py

옵션:
  --rename-legacy   완료 후 DB 테이블을 `*_legacy` 로 rename
  --dry-run         읽기만 하고 파일을 쓰지 않음

idempotent. id 보존. .seq 자동 시드.
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


def _to_iso(v):
    if v is None: return None
    if hasattr(v, 'isoformat'): return v.isoformat()
    return v


def _safe_json(raw):
    if raw is None: return None
    if isinstance(raw, (dict, list)): return raw
    try: return json.loads(raw)
    except Exception: return None


def _migrate_instances(config, dry_run: bool):
    domain = file_store.domain_dir(config, 'instances')
    print(f"[info] instances dir: {domain}")
    rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM cims_instance ORDER BY id")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] cims_instance 테이블 없음: {e}")
                return 0
    finally:
        conn.close()
    print(f"[info] cims_instance rows: {len(rows)}")
    max_id = 0
    for r in rows:
        iid = int(r['id'])
        obj = {
            'id': iid,
            'name': r.get('name'),
            'role': r.get('role'),
            'description': r.get('description'),
            'host': r.get('host'),
            'csp_notify_port': r.get('csp_notify_port'),
            'cmp_control_port': r.get('cmp_control_port'),
            'cmp_rtp_port_start': r.get('cmp_rtp_port_start'),
            'enabled': bool(r.get('enabled', 1)),
            'last_seen': _to_iso(r.get('last_seen')),
            'last_health': r.get('last_health'),
            'note': r.get('note'),
            'etag': r.get('etag'),
            'agent_id': r.get('agent_id'),
            'create_time': _to_iso(r.get('create_time')),
            'update_time': _to_iso(r.get('update_time')),
        }
        if dry_run:
            print(f"[dry] instance id={iid} name={obj['name']}")
        else:
            file_store.save(domain, iid, obj)
            print(f"[ok]  instance id={iid} name={obj['name']}")
        max_id = max(max_id, iid)
    if not dry_run and max_id:
        file_store.seed_seq(domain, max_id)
        print(f"[info] instances/.seq seeded to {max_id}")
    return len(rows)


def _migrate_agents(config, dry_run: bool):
    domain = file_store.domain_dir(config, 'agents')
    print(f"[info] agents dir: {domain}")
    rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM cims_agent ORDER BY id")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] cims_agent 테이블 없음: {e}")
                return 0
    finally:
        conn.close()
    print(f"[info] cims_agent rows: {len(rows)}")
    max_id = 0
    for r in rows:
        aid = int(r['id'])
        obj = {
            'id': aid,
            'agent_token': r.get('agent_token'),
            'enrollment_token': r.get('enrollment_token'),
            'name': r.get('name'),
            'hostname': r.get('hostname'),
            'ip_address': r.get('ip_address'),
            'os_info': r.get('os_info'),
            'cpu_cores': r.get('cpu_cores'),
            'memory_mb': r.get('memory_mb'),
            'disk_gb': r.get('disk_gb'),
            'agent_version': r.get('agent_version'),
            'status': r.get('status'),
            'last_heartbeat': _to_iso(r.get('last_heartbeat')),
            'last_metric': _to_iso(r.get('last_metric')),
            'enrolled_at': _to_iso(r.get('enrolled_at')),
            'approved_at': _to_iso(r.get('approved_at')),
            'note': r.get('note'),
            'create_time': _to_iso(r.get('create_time')),
            'update_time': _to_iso(r.get('update_time')),
            # mTLS / sync_port / IP rows / interfaces
            'mtls_enabled': r.get('mtls_enabled'),
            'cert_issued_at': _to_iso(r.get('cert_issued_at')),
            'cert_expires_at': _to_iso(r.get('cert_expires_at')),
            'cert_rotate_pending': r.get('cert_rotate_pending'),
            'sync_port': r.get('sync_port'),
            'interfaces': _safe_json(r.get('interfaces_json')),
            'service_ip_rows': _safe_json(r.get('service_ip_rows_json')),
        }
        if dry_run:
            print(f"[dry] agent id={aid} name={obj['name']} status={obj['status']}")
        else:
            file_store.save(domain, aid, obj)
            print(f"[ok]  agent id={aid} name={obj['name']} status={obj['status']}")
        max_id = max(max_id, aid)
    if not dry_run and max_id:
        file_store.seed_seq(domain, max_id)
        print(f"[info] agents/.seq seeded to {max_id}")
    return len(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rename-legacy', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    config = _load_config()
    print(f"[info] runtime store: {file_store.runtime_root(config)}")
    n_inst = _migrate_instances(config, args.dry_run)
    n_agent = _migrate_agents(config, args.dry_run)
    if args.rename_legacy and not args.dry_run and (n_inst or n_agent):
        conn = _connect(config)
        try:
            with conn.cursor() as cur:
                if n_inst:
                    cur.execute("RENAME TABLE cims_instance TO cims_instance_legacy")
                    print("[info] cims_instance → cims_instance_legacy")
                if n_agent:
                    cur.execute("RENAME TABLE cims_agent TO cims_agent_legacy")
                    print("[info] cims_agent → cims_agent_legacy")
        except Exception as e:
            print(f"[error] rename 실패: {e}")
        finally:
            conn.close()
    print(f"[done] instances={n_inst} agents={n_agent}")


if __name__ == '__main__':
    main()
