#!/usr/bin/env python3
"""DB `agent_deployment` + `agent_job` + `agent_metric` → file_store 마이그레이션.

사용:
  CIMS_CSC_CONFIG=/path/to/csc.json python3 csc/scripts/migrate_deployments_jobs_metrics_db_to_file.py

옵션:
  --rename-legacy   완료 후 DB 테이블을 `*_legacy` 로 rename
  --dry-run         읽기만 하고 파일을 쓰지 않음

idempotent. id 보존. .seq 자동 시드. agent_metric 은 jsonl 시계열.
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


def _migrate_deployments(config, dry_run):
    domain = file_store.domain_dir(config, 'deployments')
    print(f"[info] deployments dir: {domain}")
    rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM agent_deployment ORDER BY id")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] agent_deployment 테이블 없음: {e}")
                return 0
    finally:
        conn.close()
    print(f"[info] agent_deployment rows: {len(rows)}")
    max_id = 0
    for r in rows:
        did = int(r['id'])
        sf = r.get('service_functions') or ''
        sf_list = [x.strip() for x in str(sf).split(',') if x.strip()]
        obj = {
            'id': did,
            'agent_id': r.get('agent_id'),
            'package_id': r.get('package_id'),
            'instance_id': r.get('instance_id'),
            'process_name': r.get('process_name'),
            'service_functions': sf_list,
            'install_path': r.get('install_path'),
            'status': r.get('status'),
            'note': r.get('note'),
            'config': _safe_json(r.get('config_json')),
            'config_applied_at': _to_iso(r.get('config_applied_at')),
            'deployed_at': _to_iso(r.get('deployed_at')),
            'last_job_id': r.get('last_job_id'),
            'create_time': _to_iso(r.get('create_time')),
            'update_time': _to_iso(r.get('update_time')),
        }
        if dry_run:
            print(f"[dry] deploy id={did} agent={obj['agent_id']} pkg={obj['package_id']}")
        else:
            file_store.save(domain, did, obj)
            print(f"[ok]  deploy id={did} agent={obj['agent_id']} pkg={obj['package_id']} status={obj['status']}")
        max_id = max(max_id, did)
    if not dry_run and max_id:
        file_store.seed_seq(domain, max_id)
        print(f"[info] deployments/.seq seeded to {max_id}")
    return len(rows)


def _migrate_jobs(config, dry_run):
    domain = file_store.domain_dir(config, 'jobs')
    print(f"[info] jobs dir: {domain}")
    rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT * FROM agent_job ORDER BY id")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] agent_job 테이블 없음: {e}")
                return 0
    finally:
        conn.close()
    print(f"[info] agent_job rows: {len(rows)}")
    max_id = 0
    for r in rows:
        jid = int(r['id'])
        obj = {
            'id': jid,
            'agent_id': r.get('agent_id'),
            'job_type': r.get('job_type'),
            'params': _safe_json(r.get('params')) if r.get('params') else {},
            'status': r.get('status'),
            'result_code': r.get('result_code'),
            'result_stdout': r.get('result_stdout'),
            'result_stderr': r.get('result_stderr'),
            'dispatched_at': _to_iso(r.get('dispatched_at')),
            'completed_at': _to_iso(r.get('completed_at')),
            'create_time': _to_iso(r.get('create_time')),
            'update_time': _to_iso(r.get('update_time')),
        }
        if dry_run:
            print(f"[dry] job id={jid} agent={obj['agent_id']} type={obj['job_type']} status={obj['status']}")
        else:
            file_store.save(domain, jid, obj)
        max_id = max(max_id, jid)
    if not dry_run:
        if max_id:
            file_store.seed_seq(domain, max_id)
            print(f"[info] jobs/.seq seeded to {max_id}")
        print(f"[ok] {len(rows)} job(s) migrated")
    return len(rows)


def _migrate_metrics(config, dry_run):
    domain_root = file_store.domain_dir(config, 'metrics')
    print(f"[info] metrics dir: {domain_root}")
    rows = []
    conn = _connect(config)
    try:
        with conn.cursor() as cur:
            try:
                cur.execute("SELECT agent_id, ts, cpu_pct, mem_pct, disk_pct, "
                            "load_avg, processes_json FROM agent_metric ORDER BY agent_id, ts")
                rows = cur.fetchall()
            except pymysql.err.ProgrammingError as e:
                print(f"[warn] agent_metric 테이블 없음: {e}")
                return 0
    finally:
        conn.close()
    print(f"[info] agent_metric rows: {len(rows)}")
    from datetime import datetime as _dt
    grouped: dict = {}  # (agent_id, day) -> count
    for r in rows:
        ts = r.get('ts')
        if not ts:
            continue
        ts_iso = _to_iso(ts)
        agent_id = int(r['agent_id'])
        procs = _safe_json(r.get('processes_json')) or []
        record = {
            'ts': ts_iso,
            'agent_id': agent_id,
            'cpu_pct': r.get('cpu_pct'),
            'mem_pct': r.get('mem_pct'),
            'disk_pct': r.get('disk_pct'),
            'load_avg': r.get('load_avg'),
            'processes': procs,
        }
        if not dry_run:
            try:
                dt = ts if hasattr(ts, 'year') else _dt.fromisoformat(ts_iso)
            except Exception:
                continue
            file_store.jsonl_append(domain_root, str(agent_id), record, dt=dt)
        key = (agent_id, ts_iso[:10] if ts_iso else '')
        grouped[key] = grouped.get(key, 0) + 1
    for (aid, day), cnt in sorted(grouped.items()):
        print(f"[{'dry' if dry_run else 'ok'}] metric agent={aid} day={day} count={cnt}")
    return len(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rename-legacy', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    config = _load_config()
    print(f"[info] runtime store: {file_store.runtime_root(config)}")
    n_dep = _migrate_deployments(config, args.dry_run)
    n_job = _migrate_jobs(config, args.dry_run)
    n_met = _migrate_metrics(config, args.dry_run)
    if args.rename_legacy and not args.dry_run and (n_dep or n_job or n_met):
        conn = _connect(config)
        try:
            with conn.cursor() as cur:
                # FK 순서: agent_deployment 가 agent_job.last_job_id 를 참조하지 않으니 임의 순서 OK
                if n_dep:
                    cur.execute("RENAME TABLE agent_deployment TO agent_deployment_legacy")
                    print("[info] agent_deployment → agent_deployment_legacy")
                if n_job:
                    cur.execute("RENAME TABLE agent_job TO agent_job_legacy")
                    print("[info] agent_job → agent_job_legacy")
                if n_met:
                    cur.execute("RENAME TABLE agent_metric TO agent_metric_legacy")
                    print("[info] agent_metric → agent_metric_legacy")
        except Exception as e:
            print(f"[error] rename 실패: {e}")
        finally:
            conn.close()
    print(f"[done] deployments={n_dep} jobs={n_job} metrics={n_met}")


if __name__ == '__main__':
    main()
