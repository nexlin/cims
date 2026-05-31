"""
HA fan-out drift sweeper (L4 / F2).

주기적으로 ha_group * collection 의 jsonl hash 를 멤버들 사이에서 비교 →
drift 발견 시 alert_log 에 1건 기록 + (optional) auto-resync.

설계:
- scope=service 또는 (scope=system + active_standby) 컬렉션만 비교 (ha_lookup.should_propagate)
- 첫 멤버 (master 우선, 없으면 deployment id 작은 쪽) 를 기준으로 hash 비교
- 비교 자체는 agent proxy GET 으로 현재 jsonl 읽음 — drift 없으면 alert 안 냄
- drift 있을 시:
    * alert_log 에 type='config_drift_{collection}' open 이벤트 (severity warning)
    * auto_resync=True 면 master records 로 다른 멤버에 PUT (배포)
    * drift 해소 시 alert close

호출자: csc_app.py main loop (interval default 300s).
"""
from __future__ import annotations

import hashlib
import json
from datetime import datetime
from typing import Optional

from services import file_store, ha_lookup, alert_log


_DEPLOY_DOMAIN = 'deployments'
_PKG_DOMAIN = 'packages'


def _records_hash(records) -> str:
    try:
        # 멤버/스윕 간 jsonl 레코드 ORDER 가 달라도 동일 내용이면 같은 hash 가 되도록
        # 각 레코드를 정규화(sort_keys) 직렬화 후 정렬 — 순서 차이로 인한 false drift
        # (config_drift 알람 flapping) 방지. sort_keys 만으로는 dict 키만 정렬돼 부족.
        norm = sorted(json.dumps(r, ensure_ascii=False, sort_keys=True) for r in (records or []))
        payload = json.dumps(norm, ensure_ascii=False).encode('utf-8')
        return hashlib.sha256(payload).hexdigest()[:12]
    except Exception:
        return ""


def _load_deployment(config, did: int) -> Optional[dict]:
    return file_store.by_id(file_store.domain_dir(config, _DEPLOY_DOMAIN), did)


def _load_package(config, pid: int) -> Optional[dict]:
    return file_store.by_id(file_store.domain_dir(config, _PKG_DOMAIN), pid)


def _agent_load(config, aid: int) -> Optional[dict]:
    return file_store.by_id(file_store.domain_dir(config, 'agents'), aid)


def _proxy_get_collection(agent: dict, install_path: str, collection: str,
                          config: dict) -> tuple:
    """agent proxy 로 컬렉션 GET. 옛 _agent_proxy_call 재사용은 import 순환 회피
    위해 인라인 구현."""
    import urllib.parse, urllib.request, urllib.error, ssl as _ssl, os as _os
    ip = agent.get('ip_address') or '127.0.0.1'
    port = agent.get('sync_port')
    if not port:
        return 0, None
    qs = urllib.parse.urlencode({'install_path': install_path, 'name': collection})
    url = f"https://{ip}:{port}/collection?{qs}"
    req = urllib.request.Request(url, method='GET',
        headers={'X-Agent-Token': agent.get('agent_token') or ''})
    ctx = _ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = _ssl.CERT_NONE
    if agent.get('mtls_enabled'):
        mtls_cfg = (config or {}).get('Agent') or {}
        mtls_dir = mtls_cfg.get('MtlsDir') or 'cert/agent_mtls'
        if not _os.path.isabs(mtls_dir):
            mtls_dir = _os.path.abspath(mtls_dir)
        ca_crt = _os.path.join(mtls_dir, 'ca.crt')
        c_crt  = _os.path.join(mtls_dir, 'csc_client.crt')
        c_key  = _os.path.join(mtls_dir, 'csc_client.key')
        if _os.path.isfile(ca_crt) and _os.path.isfile(c_crt) and _os.path.isfile(c_key):
            ctx.verify_mode = _ssl.CERT_REQUIRED
            ctx.load_verify_locations(ca_crt)
            ctx.load_cert_chain(certfile=c_crt, keyfile=c_key)
    try:
        with urllib.request.urlopen(req, timeout=10, context=ctx) as resp:
            body = json.loads(resp.read().decode('utf-8'))
            return resp.status, body
    except urllib.error.HTTPError as e:
        return e.code, None
    except Exception:
        return 0, None


def scan_group_collection(config, ha_group: dict, collection: str,
                          deployments: list[dict]) -> dict:
    """ha_group 의 collection 1개를 멤버들 사이에서 hash 비교.

    Returns:
      {
        ha_group_id, collection, members: [{deployment_id, agent_id, status, hash, count}],
        drift: bool, base_hash, status_summary: str
      }
    """
    members = []
    for d in deployments:
        ag = _agent_load(config, d.get('agent_id')) or {}
        st, body = _proxy_get_collection(ag, d.get('install_path'), collection, config)
        ok = (st == 200) and isinstance(body, dict)
        recs = (body or {}).get('records') or [] if ok else []
        members.append({
            'deployment_id': d.get('id'),
            'agent_id':      d.get('agent_id'),
            'status':        st,
            'ok':            ok,
            'count':         len(recs) if ok else None,
            'hash':          _records_hash(recs) if ok else '',
            'records':       recs if ok else None,
        })

    drift = False
    base_hash = ''
    base_ok = [m for m in members if m['ok']]
    if base_ok:
        base_hash = base_ok[0]['hash']
        for m in base_ok[1:]:
            if m['hash'] and m['hash'] != base_hash:
                drift = True
                break

    return {
        'ha_group_id':    ha_group.get('id'),
        'ha_group_name':  ha_group.get('name'),
        'ha_group_mode':  ha_group.get('mode'),
        'collection':     collection,
        'members':        members,
        'drift':          drift,
        'base_hash':      base_hash,
        'all_ok':         all(m['ok'] for m in members) if members else False,
    }


def _collections_for_package(config, package_name: str) -> list[tuple]:
    """package 의 config_template 에서 (collection_key, scope) pair 목록 반환."""
    pkg_dir = file_store.domain_dir(config, _PKG_DOMAIN)
    for p in file_store.load_all(pkg_dir):
        if p.get('name') != package_name:
            continue
        tmpl = p.get('config_template')
        if isinstance(tmpl, str):
            try: tmpl = json.loads(tmpl)
            except: tmpl = {}
        if not isinstance(tmpl, dict):
            continue
        out = []
        for c in tmpl.get('collections') or []:
            key   = c.get('key')
            scope = (c.get('scope') or 'service').lower()
            if key:
                out.append((key, scope))
        return out
    return []


def scan_all(config) -> list[dict]:
    """모든 ha_group * (propagate 대상 컬렉션) 을 sweep.

    Returns: list of scan results — drift 있는/없는 모든 row.
    """
    out: list[dict] = []
    seen = set()  # (ha_group_id, package, collection) dedupe
    for g in ha_lookup.ha_groups_all(config):
        gid = g.get('id')
        mode = g.get('mode')
        # 멤버 → 어떤 패키지 deploy 했는지 추출
        deploys_by_pkg: dict = {}
        for m in ha_lookup.members_of(g):
            aid = m.get('agent_id')
            for d in file_store.load_all(file_store.domain_dir(config, _DEPLOY_DOMAIN)):
                if d.get('agent_id') != aid:
                    continue
                pkg_name = d.get('package_name')
                if not pkg_name:
                    pkg = _load_package(config, d.get('package_id')) or {}
                    pkg_name = pkg.get('name')
                if pkg_name:
                    deploys_by_pkg.setdefault(pkg_name, []).append(d)

        for pkg_name, deps in deploys_by_pkg.items():
            if len(deps) < 2:
                continue  # 1명 이하면 비교 불필요
            cols = _collections_for_package(config, pkg_name)
            for key, scope in cols:
                if not ha_lookup.should_propagate(scope, mode, None):
                    continue
                tag = (gid, pkg_name, key)
                if tag in seen:
                    continue
                seen.add(tag)
                out.append(scan_group_collection(config, g, key, deps))
    return out


def emit_drift_alerts(config, scan_results: list[dict], service_log_dir: str,
                      open_state: dict) -> dict:
    """scan 결과를 alert_log 에 반영.

    open_state: { alert_type: True } — 호출 전후 mutate 되는 상태 dict
                 (csc_app.py 의 _alert_open 과 동일 의도).
    """
    now = datetime.now().isoformat(timespec='seconds')
    counts = {'opened': 0, 'closed': 0, 'still_open': 0}
    if not service_log_dir:
        return counts
    for r in scan_results:
        if not r.get('all_ok'):
            continue  # proxy 실패는 drift 판정 보류
        gid = r['ha_group_id']
        coll = r['collection']
        typ = f"config_drift::g{gid}::{coll}"
        was = typ in open_state
        if r['drift']:
            counts['still_open'] += 1 if was else 0
            if not was:
                open_state[typ] = True
                alert_log.record_event(service_log_dir, {
                    'ts': now,
                    'type': typ,
                    'severity': 'warning',
                    'action': 'open',
                    'message': (f"HA fan-out drift — group '{r.get('ha_group_name')}' "
                                f"collection '{coll}': "
                                + ", ".join(f"d{m['deployment_id']}={m['hash'] or 'err'}"
                                            for m in r['members']))
                })
                counts['opened'] += 1
        else:
            if was:
                open_state.pop(typ, None)
                alert_log.record_event(service_log_dir, {
                    'ts': now,
                    'type': typ,
                    'severity': 'warning',
                    'action': 'close',
                    'message': f"HA drift 해소 — group '{r.get('ha_group_name')}' / {coll}",
                })
                counts['closed'] += 1
    return counts


def auto_resync(config, scan_results: list[dict]) -> dict:
    """drift 있는 컬렉션에 대해 master 의 records 로 다른 멤버에 PUT.

    master 결정: ha_group.members 의 role=='master' agent_id 매칭. 없으면 첫 멤버.
    """
    summary = {'resynced': 0, 'errors': []}
    for r in scan_results:
        if not r['drift'] or not r['all_ok']:
            continue
        members = r['members']
        # master 우선 — ha_group.members 의 role 정보를 갖고 있지 않으므로
        # deployment_id 작은 쪽 (보통 ctrl-a) 을 fallback 으로 사용.
        members_sorted = sorted(members, key=lambda m: m['deployment_id'])
        master = members_sorted[0]
        if not master.get('records'):
            continue
        # 다른 멤버에 PUT — agent proxy
        import urllib.parse, urllib.request, urllib.error, ssl as _ssl, os as _os
        for m in members_sorted[1:]:
            if m['hash'] == master['hash']:
                continue
            agent = _agent_load(config, m['agent_id']) or {}
            ip = agent.get('ip_address') or '127.0.0.1'
            port = agent.get('sync_port')
            if not port:
                summary['errors'].append({'deployment_id': m['deployment_id'],
                                          'error': 'agent sync_port unknown'})
                continue
            dep = _load_deployment(config, m['deployment_id'])
            if not dep:
                continue
            qs = urllib.parse.urlencode({'install_path': dep.get('install_path'),
                                         'name': r['collection']})
            url = f"https://{ip}:{port}/collection?{qs}"
            body = json.dumps({'records': master['records']}).encode('utf-8')
            req = urllib.request.Request(url, method='PUT', data=body,
                headers={'X-Agent-Token': agent.get('agent_token') or '',
                         'Content-Type': 'application/json'})
            ctx = _ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = _ssl.CERT_NONE
            if agent.get('mtls_enabled'):
                mtls_cfg = (config or {}).get('Agent') or {}
                mtls_dir = mtls_cfg.get('MtlsDir') or 'cert/agent_mtls'
                if not _os.path.isabs(mtls_dir):
                    mtls_dir = _os.path.abspath(mtls_dir)
                ca_crt = _os.path.join(mtls_dir, 'ca.crt')
                c_crt  = _os.path.join(mtls_dir, 'csc_client.crt')
                c_key  = _os.path.join(mtls_dir, 'csc_client.key')
                if _os.path.isfile(ca_crt) and _os.path.isfile(c_crt) and _os.path.isfile(c_key):
                    ctx.verify_mode = _ssl.CERT_REQUIRED
                    ctx.load_verify_locations(ca_crt)
                    ctx.load_cert_chain(certfile=c_crt, keyfile=c_key)
            try:
                with urllib.request.urlopen(req, timeout=15, context=ctx) as resp:
                    if resp.status == 200:
                        summary['resynced'] += 1
            except Exception as e:
                summary['errors'].append({'deployment_id': m['deployment_id'],
                                          'error': str(e)})
    return summary
