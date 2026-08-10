"""
HA fan-out drift sweeper (L4 / F2).

주기적으로 ha_group * collection 의 jsonl hash 를 멤버들 사이에서 비교 →
drift 발견 시 alert_log 에 1건 기록 + (optional) auto-resync.

설계:
- scope=service 또는 (scope=system + active_standby) 컬렉션만 비교 (ha_lookup.should_propagate)
- 첫 멤버 (master 우선, 없으면 deployment id 작은 쪽) 를 기준으로 hash 비교
- 비교 자체는 agent proxy GET 으로 현재 jsonl 읽음 — drift 없으면 alert 안 냄
- drift 있을 시:
    * 표준 알람 발화 (alarm_standardization.md) — 클래스 config_out_of_sync/CIMS-CFG-001,
      객체 mo_instance='cims/ha/g<group_id>/<collection>' (alarm_sweeper.transition 코어)
    * auto_resync=True 면 master records 로 다른 멤버에 PUT (배포)
    * drift 해소 시 Cleared(close)
- 구 포맷(type='config_drift::g<id>::<coll>', code/alarm_id 없음)의 미해소 open 은
  스윕에서 발견 즉시 이행 종결(close)하고, 조건이 지속이면 같은 스윕에서 표준 알람으로
  재발행한다 — 구 행이 영구 미해소로 남는 것을 막는 1회성 이행 경로.

호출자: oam_app.py main loop (interval default 300s).
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
                # 미배포(pending) 배포는 config 가 없어 drift 비교 시 false drift 를 유발한다
                # (중복/고아 배포 사례). 실제 배포된 것만 비교.
                if (d.get('status') or '').lower() == 'pending':
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


_UNKNOWN_STREAK: dict = {}    # akey -> 연속 '판정 불가'(all_ok=False) 횟수
_UNKNOWN_LIMIT = 3            # 이 횟수 연속이면 열린 알람을 판정불가 사유로 종료

# HA fan-out drift 알람 정의 — 표준화 §3.5: 클래스는 조건(config_out_of_sync), 객체는
# source.mo_instance. agent 계열 노드파일 드리프트(CIMS-CFG-001, <host>/<module>/config)와
# 같은 조건 클래스이며 객체만 다르다 (cims/ha/g<gid>/<collection>).
_DRIFT_RULE = {
    'type': 'config_out_of_sync', 'code': 'CIMS-CFG-001', 'perceived_severity': 'warning',
    'event_type': 'processingError', 'probable_cause': 'configurationOrCustomizationError',
    'mo_class': 'software', 'metric': 'HA fan-out 정합',
    'effect': 'HA 멤버 간 런타임 컬렉션 불일치 — 절체 시 동작 상이 위험',
    'recommended_action': 'auto_resync 또는 콘솔에서 해당 컬렉션 재배포로 멤버 정렬',
}

_OLD_PREFIX = 'config_drift::'    # 구 포맷 akey(type) — 이행 종결 대상


def _mo_instance(gid, coll: str) -> str:
    return f"cims/ha/g{gid}/{coll}"


def _akey(gid, coll: str) -> str:
    return f"{_DRIFT_RULE['code']}@{_mo_instance(gid, coll)}"


def _drift_prefix_keys(state_keys) -> list:
    return [k for k in state_keys
            if k.startswith(f"{_DRIFT_RULE['code']}@cims/ha/") or k.startswith(_OLD_PREFIX)]


def _reseed_if_empty(open_state: dict, service_log_dir: str) -> None:
    """in-memory 열림상태가 비어 있으면 alert_log 에서 재도출.

    open_state 는 OAM 프로세스 메모리에만 있고 기동 시 1회 복원된다. 그 복원이
    실패했거나 프로세스가 교체되면 "이미 열려 있는 알람"을 없다고 보고 open 을
    중복 발행한다 — close 는 1건만 뒤따르므로 앞선 open 이 영구 미해소로 남는다
    (2026-07-07 사례). 스윕마다 비었을 때만 도므로 정상 경로에서는 비용이 없다.
    구 포맷 키(config_drift::…)도 함께 재도출해 이행 종결이 걸리게 한다."""
    if open_state or not service_log_dir:
        return
    try:
        st = alert_log.compute_open_state(service_log_dir, days=90)
        for k in _drift_prefix_keys(st.keys()):
            open_state[k] = st[k]
    except Exception:
        pass


def _close(open_state: dict, service_log_dir: str, akey: str, message: str) -> None:
    """표준 close 발화 — akey 에서 mo 를 복원해 transition 코어로 종결."""
    from services import alarm_sweeper
    mo = akey.split('@', 1)[1]
    alarm_sweeper.transition(open_state, service_log_dir, _DRIFT_RULE, mo, 'oam',
                             False, '', message)


def emit_drift_alerts(config, scan_results: list[dict], service_log_dir: str,
                      open_state: dict) -> dict:
    """scan 결과를 표준 알람으로 반영 (alarm_sweeper.transition 코어).

    open_state: { akey(code@mo_instance): alarm_id } — 호출 전후 mutate 되는 상태 dict.
    """
    from services import alarm_sweeper
    now = datetime.now().isoformat(timespec='seconds')
    counts = {'opened': 0, 'closed': 0, 'still_open': 0}
    if not service_log_dir:
        return counts
    # 관측 대상이 하나도 없으면 아무 판정도 하지 않는다. HA 절체 직후 standby OAM 은
    # 자기 file_store(deployments/ha_groups)가 비어 있어 scan 이 공집합이 되는데,
    # 그대로 진행하면 아래 orphan reap 이 universe 를 공집합으로 보고 열려 있던
    # 알람을 전부 오종결한다. (ha_design.md §12 — OAM 스토어는 노드 간 미복제)
    if not scan_results:
        return counts
    _reseed_if_empty(open_state, service_log_dir)
    # 구 포맷 이행 종결 — 표준 필드 없는 open 행은 ack/색 매핑이 안 되므로 legacy
    # 포맷 그대로 close 를 페어링해 닫는다. 조건 지속이면 아래 평가가 같은 스윕에서
    # 표준 알람으로 다시 연다.
    for typ in [k for k in list(open_state) if k.startswith(_OLD_PREFIX)]:
        open_state.pop(typ, None)
        alert_log.record_event(service_log_dir, {
            'ts': now, 'type': typ, 'severity': 'warning', 'action': 'close',
            'message': f"알람 표준 포맷 이행 — 지속 조건은 표준 알람(CIMS-CFG-001)으로 재발행",
        })
        counts['closed'] += 1
    # 고아 reap — 이번 스윕의 비교 대상(모든 row, all_ok 무관)에 더 이상 없는 open 키는
    # 그룹 삭제/컬렉션 제거/배포 재구성으로 재평가가 불가능해진 알람 → close 발행.
    # (all_ok=False row 도 universe 에 포함되므로 proxy 일시 실패로 오닫힘 없음.)
    universe = {_akey(r['ha_group_id'], r['collection']) for r in scan_results}
    for akey in [k for k in list(open_state) if k not in universe]:
        _close(open_state, service_log_dir, akey,
               f"HA drift 대상 소멸 (그룹/컬렉션 제거) — {akey.split('@', 1)[1]}")
        counts['closed'] += 1
    for r in scan_results:
        gid = r['ha_group_id']
        coll = r['collection']
        akey = _akey(gid, coll)
        mo = _mo_instance(gid, coll)
        if not r.get('all_ok'):
            # proxy 실패는 drift 판정 보류. 다만 무기한 보류하면 이미 열린 알람이
            # 영원히 닫히지 않는다 (universe 에는 남아 orphan reap 도 안 걸림).
            # 연속 _UNKNOWN_LIMIT 회면 '판정 불가' 사유로 종료한다.
            n = _UNKNOWN_STREAK[akey] = _UNKNOWN_STREAK.get(akey, 0) + 1
            if akey in open_state and n >= _UNKNOWN_LIMIT:
                _close(open_state, service_log_dir, akey,
                       (f"HA drift 판정 불가 {n}회 연속 (멤버 컬렉션 조회 실패) — "
                        f"알람 종료. group '{r.get('ha_group_name')}' / {coll}"))
                counts['closed'] += 1
            continue
        _UNKNOWN_STREAK.pop(akey, None)     # 정상 판정 → 스트릭 리셋
        was = akey in open_state
        if r['drift']:
            counts['still_open'] += 1 if was else 0
            if not was:
                alarm_sweeper.transition(
                    open_state, service_log_dir, _DRIFT_RULE, mo, 'oam', True,
                    (f"HA fan-out drift — group '{r.get('ha_group_name')}' "
                     f"collection '{coll}': "
                     + ", ".join(f"d{m['deployment_id']}={m['hash'] or 'err'}"
                                 for m in r['members'])),
                    '')
                counts['opened'] += 1
        else:
            if was:
                _close(open_state, service_log_dir, akey,
                       f"HA drift 해소 — group '{r.get('ha_group_name')}' / {coll}")
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
