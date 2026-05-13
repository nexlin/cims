#!/usr/bin/env python3
"""DB CSP 런타임 설정 9 테이블 → file_store 마이그레이션 (Phase 5).

대상 도메인 / 파일:
  csp_listener         → {CimsRuntimeDir}/csp_listener/<id>.json
  sip_trunk            → {CimsRuntimeDir}/sip_trunk/<id>.json
  routing_rule (+match+transform)
                       → {CimsRuntimeDir}/routing_rule/<id>.json (match/transform 임베드)
  routing_access_list  → {CimsRuntimeDir}/routing_access_list/<id>.json
  sip_service (+sip_service_listener)
                       → {CimsRuntimeDir}/sip_service/<id>.json (listeners 임베드)
  csp_config_audit     → JSONL 시계열 (마이그 시 raw 보존 안 함 — 운영 이력만)

사용:
  CIMS_CSC_CONFIG=... python3 csc/scripts/migrate_csp_runtime_db_to_file.py
"""
import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, '..', 'src')))

import pymysql  # type: ignore
import pymysql.cursors  # type: ignore

from services import file_store  # type: ignore


def _load_config() -> dict:
    p = os.environ.get('CIMS_CSC_CONFIG')
    if not p or not os.path.isfile(p):
        print(f"[error] CIMS_CSC_CONFIG 미설정: {p}", file=sys.stderr); sys.exit(1)
    with open(p, 'r', encoding='utf-8') as f:
        return json.load(f)


def _connect(config):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'), port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'), password=db.get('Password', ''),
        database=db.get('Db', 'cims'), charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor, autocommit=True,
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


def _migrate_table(config, table_name, domain, dry_run):
    rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute(f"SELECT * FROM {table_name} ORDER BY id")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] {table_name} 테이블 없음: {e}")
                return 0
    finally:
        conn.close()
    print(f"[info] {table_name} rows: {len(rows)} → domain={domain}")
    d = file_store.domain_dir(config, domain)
    max_id = 0
    for r in rows:
        rid = int(r['id'])
        # datetime 필드 ISO 정규화
        obj = {k: (_to_iso(v) if hasattr(v, 'isoformat') else v) for k, v in r.items()}
        if not dry_run:
            file_store.save(d, rid, obj)
        max_id = max(max_id, rid)
    if not dry_run and max_id:
        file_store.seed_seq(d, max_id)
        print(f"  → {domain}/.seq seeded to {max_id}")
    return len(rows)


def _migrate_routes(config, dry_run):
    """rule + match + transform 을 합쳐 1 파일."""
    rules = []
    matches_by_rule: dict = {}
    transforms_by_rule: dict = {}
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM routing_rule ORDER BY id")
                rules = cur.fetchall()
                cur.execute("SELECT * FROM routing_rule_match")
                for m in cur.fetchall():
                    matches_by_rule.setdefault(int(m['rule_id']), []).append(m)
                cur.execute("SELECT * FROM routing_rule_transform")
                for t in cur.fetchall():
                    transforms_by_rule.setdefault(int(t['rule_id']), []).append(t)
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] routing_rule* 없음: {e}")
                return 0
    finally:
        conn.close()
    d = file_store.domain_dir(config, 'routing_rule')
    print(f"[info] routing_rule rows: {len(rules)}")
    max_id = 0
    for r in rules:
        rid = int(r['id'])
        ms = sorted(matches_by_rule.get(rid, []), key=lambda x: x.get('seq', 0))
        ts = sorted(transforms_by_rule.get(rid, []), key=lambda x: x.get('seq', 0))
        obj = {k: (_to_iso(v) if hasattr(v, 'isoformat') else v) for k, v in r.items()}
        obj['target_json'] = _safe_json(r.get('target_json'))
        obj['match'] = [
            {'field': m['field'], 'op': m['op'], 'value': m['value'],
             'invert': bool(m.get('invert')), 'seq': m.get('seq', 0)}
            for m in ms
        ]
        obj['transform'] = [
            {'action': t['action'], 'target': t.get('target'),
             'value': t.get('value'), 'seq': t.get('seq', 0)}
            for t in ts
        ]
        if not dry_run:
            file_store.save(d, rid, obj)
        max_id = max(max_id, rid)
    if not dry_run and max_id:
        file_store.seed_seq(d, max_id)
        print(f"  → routing_rule/.seq seeded to {max_id}")
    return len(rules)


def _migrate_services(config, dry_run):
    services = []
    listeners_by_svc: dict = {}
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM sip_service ORDER BY id")
                services = cur.fetchall()
                cur.execute("SELECT * FROM sip_service_listener")
                for r in cur.fetchall():
                    listeners_by_svc.setdefault(int(r['service_id']), []).append(int(r['listener_id']))
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] sip_service* 없음: {e}")
                return 0
    finally:
        conn.close()
    d = file_store.domain_dir(config, 'sip_service')
    print(f"[info] sip_service rows: {len(services)}")
    max_id = 0
    for s in services:
        sid = int(s['id'])
        obj = {k: (_to_iso(v) if hasattr(v, 'isoformat') else v) for k, v in s.items()}
        obj['listeners'] = listeners_by_svc.get(sid, [])
        if not dry_run:
            file_store.save(d, sid, obj)
        max_id = max(max_id, sid)
    if not dry_run and max_id:
        file_store.seed_seq(d, max_id)
        print(f"  → sip_service/.seq seeded to {max_id}")
    return len(services)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rename-legacy', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    config = _load_config()
    print(f"[info] runtime: {file_store.runtime_root(config)}")

    n_lst = _migrate_table(config, 'csp_listener', 'csp_listener', args.dry_run)
    n_trk = _migrate_table(config, 'sip_trunk', 'sip_trunk', args.dry_run)
    n_acc = _migrate_table(config, 'routing_access_list', 'routing_access_list', args.dry_run)
    n_rt  = _migrate_routes(config, args.dry_run)
    n_svc = _migrate_services(config, args.dry_run)

    if args.rename_legacy and not args.dry_run:
        conn = _connect(config)
        try:
            with conn.cursor() as cur:
                for tbl in ('csp_config_audit',
                            'sip_service_listener', 'sip_service',
                            'routing_rule_transform', 'routing_rule_match', 'routing_rule',
                            'routing_access_list', 'sip_trunk', 'csp_listener'):
                    try:
                        cur.execute(f"RENAME TABLE {tbl} TO {tbl}_legacy")
                        print(f"  rename: {tbl} → {tbl}_legacy")
                    except Exception as e:
                        print(f"  skip rename {tbl}: {e}")
        finally:
            conn.close()
    print(f"[done] listener={n_lst} trunk={n_trk} access={n_acc} route={n_rt} service={n_svc}")


if __name__ == '__main__':
    main()
