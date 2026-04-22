#!/usr/bin/env python3
"""
CSP SIP config v2 → v3 마이그레이션.

원본 (v2, 4 collection):
  <install_path>/config/listeners.jsonl
  <install_path>/config/trunks.jsonl
  <install_path>/config/routes.jsonl        (구 match+target 스키마)
  <install_path>/config/acl.jsonl
  <install_path>/config/services.jsonl

결과 (v3, 9 collection):
  <install_path>/config/local_nodes.jsonl
  <install_path>/config/remote_nodes.jsonl
  <install_path>/config/routes.jsonl        (신 LN,RN pair 스키마)
  <install_path>/config/route_sets.jsonl
  <install_path>/config/rules.jsonl
  <install_path>/config/rule_sets.jsonl
  <install_path>/config/routing_policies.jsonl
  <install_path>/config/acl_policies.jsonl
  <install_path>/config/access_services.jsonl

변환 규칙 (sip_service_model.md §7-1):
  - services kind=voip/ptt → access_services 1:1
  - services kind=ibcf 1건 + 그 service 의 trunks N건 → route_sets 1건 (members=N Routes)
  - listeners 1건 → local_nodes 1건 (service 필드 제거, edge 자동 추론)
  - trunks 1건 → remote_nodes 1 + routes 1 + 포함 RouteSet.members 에 추가
  - 구 routes 1건 (match+target) → rules + rule_set + routing_policy 분할
  - 구 acl 1건 → rules + rule_set + acl_policy 분할

실행:
  python3 migrate_csp_sip_config_v3.py <install_path>            # dry-run + 변환
  python3 migrate_csp_sip_config_v3.py <install_path> --apply    # 실제 파일 쓰기

구 파일은 --apply 시 *.v2.bak 로 rename. 신 파일은 기존 것이 있으면 덮어쓰기 전 중지.
"""

import argparse
import json
import os
import sys
import uuid
from pathlib import Path
from typing import Any


def gen_uuid() -> str:
    """16 hex UUID (existing scheme)."""
    return uuid.uuid4().hex


def read_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"  [skip] {path.name} bad line: {e}", file=sys.stderr)
    return out


def write_jsonl(path: Path, rows: list[dict]):
    with path.open('w') as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')


# ──────────────────────────────────────────────────────────────
# 변환 함수들

def _infer_edge(listener: dict) -> str:
    """기존 listener 의 service 필드로 edge 추측."""
    svc = str(listener.get('service', '')).lower()
    if svc in ('ibcf', 'peering'):
        return 'peering'
    if svc in ('console', 'system'):
        return 'mgmt'
    return 'access'


def convert_listeners(v2: list[dict]) -> list[dict]:
    out = []
    for ln in v2:
        r = {
            'id':           ln.get('id') or gen_uuid(),
            'name':         ln.get('name') or f"ln-{len(out)}",
            'enabled':      ln.get('enabled', True),
            'edge':         _infer_edge(ln),
            'bind_ip':      ln.get('bind_ip', '0.0.0.0'),
            'bind_port':    ln.get('bind_port', 5060),
            'protocol':     ln.get('protocol', 'UDP'),
            'tls_cert_path': ln.get('tls_cert_path', ''),
            'tls_key_path':  ln.get('tls_key_path', ''),
            'tls_ca_path':   ln.get('tls_ca_path', ''),
            'tls_verify_peer': ln.get('tls_verify_peer', False),
            'max_connections': ln.get('max_connections', 0),
            'tags':         [t for t in [ln.get('service')] if t],
            'note':         ln.get('note', ''),
        }
        out.append(r)
    return out


def convert_services_and_trunks(v2_services: list[dict], v2_trunks: list[dict]) -> tuple[
    list[dict], list[dict], list[dict], list[dict]
]:
    """services + trunks → access_services, remote_nodes, routes, route_sets

    service.kind:
      - voip/volte → access_services (kind=voip)
      - ptt/mcptt  → access_services (kind=ptt)
      - ibcf       → route_set (cluster)

    각 trunk 는 remote_node + route 로 분리, route 는 해당 service 의 RouteSet.members 에 편입.
    Route 의 local_node_ref 는 추론 불가 — 빈 값 또는 첫 peering LN 으로 설정 (운영자가 수정).
    """
    access_services = []
    remote_nodes = []
    routes = []
    route_sets = []

    svc_kind_map: dict[int, str] = {}   # service.id → kind
    svc_name_map: dict[int, str] = {}   # service.id → v3 name

    for s in v2_services:
        sid = s.get('id') or 0
        kind_raw = str(s.get('kind', '')).lower()
        if kind_raw in ('voip', 'volte'):
            kind = 'voip'
        elif kind_raw in ('ptt', 'mcptt'):
            kind = 'ptt'
        elif kind_raw == 'ibcf':
            kind = 'ibcf'
        else:
            kind = kind_raw or 'voip'

        svc_kind_map[sid] = kind
        name = s.get('name') or f"svc-{sid}"
        svc_name_map[sid] = name

        if kind in ('voip', 'ptt'):
            access_services.append({
                'id':       s.get('uuid') or gen_uuid(),
                'name':     name,
                'enabled':  s.get('enabled', True),
                'kind':     kind,
                'domain':   s.get('domain', ''),
                'auth_realm': s.get('auth_realm', ''),
                'inbound_policy': s.get('inbound_policy', 'any'),
                'allowed_local_node_refs': [],  # v2 의 listeners[] (int id) → 매핑 불가: 운영자 수동
                'priority': s.get('priority', 100),
                'tags':     ['migrated-v2'],
                'note':     f"v2 service_id={sid}",
            })
        elif kind == 'ibcf':
            # 이 service 를 RouteSet 로 치환 (members 는 trunks 루프에서 채움)
            route_sets.append({
                'id':       s.get('uuid') or gen_uuid(),
                'name':     name,
                'enabled':  s.get('enabled', True),
                'distribution_policy': 'failover',
                'members':  [],
                'health_check_mode': 'options_ping',
                'health_check_interval_sec': 30,
                'health_check_dead_threshold': 3,
                'health_check_recovery_probes': 1,
                'fallback_policy': 'reject',
                'tags':     ['migrated-v2', 'peering'],
                'note':     f"v2 ibcf service_id={sid}",
            })

    # trunks → remote_nodes + routes, 각 route 를 해당 service 의 route_set.members 에 add
    for t in v2_trunks:
        tid = t.get('id') or gen_uuid()
        name = t.get('name') or f"trunk-{tid[:6]}"
        svc_id = t.get('service_id') or 0
        rn = {
            'id':            tid,
            'name':          f"rn-{name}",
            'enabled':       t.get('enabled', True),
            'ip':            t.get('remote_ip', ''),
            'port':          t.get('remote_port', 5060),
            'protocol':      t.get('protocol', 'UDP'),
            'remote_domain': t.get('remote_domain', ''),
            'srv_lookup':    t.get('srv_lookup', False),
            'dns_fallback':  t.get('dns_fallback', True),
            'tls_verify':    False,
            'tags':          ['migrated-v2'],
            'note':          f"v2 trunk_id={tid}",
        }
        remote_nodes.append(rn)
        r = {
            'id':               gen_uuid(),
            'name':             f"r-{name}",
            'enabled':          t.get('enabled', True),
            'local_node_ref':   '',   # 운영자가 채워야 함 (어느 LN 으로 이 피어에 발신할지)
            'remote_node_ref':  rn['name'],
            'outbound_proxy_ip':   t.get('outbound_proxy_ip', ''),
            'outbound_proxy_port': t.get('outbound_proxy_port', 0),
            'register_to_remote':  t.get('register_to_remote', False),
            'register_expires':    t.get('register_expires', 3600),
            'auth_user':        t.get('auth_user', ''),
            'auth_password':    t.get('auth_password', ''),
            'auth_realm':       t.get('auth_realm', ''),
            'max_concurrent_calls': t.get('max_concurrent_calls', 0),
            'cps_limit':        t.get('cps_limit', 0),
            'tags':             ['migrated-v2'],
            'note':             f"v2 trunk_id={tid} (local_node_ref 를 수동 지정 필요)",
        }
        routes.append(r)

        # service=ibcf 인 경우 그 RouteSet.members 에 편입
        svc_name = svc_name_map.get(svc_id)
        if svc_name:
            for rs in route_sets:
                if rs['name'] == svc_name:
                    rs['members'].append({
                        'route_ref': r['name'],
                        'priority':  t.get('failover_priority', 100),
                        'weight':    1,
                    })
                    break

    return access_services, remote_nodes, routes, route_sets


def convert_acl(v2_acl: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    """구 acl 1건 → rule + rule_set + acl_policy"""
    rules = []
    rule_sets = []
    acl_policies = []
    for i, a in enumerate(v2_acl):
        match_type = a.get('match_type', 'ip')   # ip | cidr | ua_regex
        value = a.get('value', '')
        kind = a.get('kind', 'deny')              # allow | deny
        scope = a.get('scope', 'global')
        name = a.get('name') or f"acl-{i+1}"

        # field + op 매핑
        if match_type == 'ip':
            field, op = 'src_ip', 'eq'
        elif match_type == 'cidr':
            field, op = 'src_ip', 'in_cidr'
        elif match_type == 'ua_regex':
            field, op = 'user_agent', 'regex'
        else:
            field, op = 'src_ip', 'eq'

        rule = {
            'id':      gen_uuid(),
            'name':    f"rule-{name}",
            'enabled': a.get('enabled', True),
            'field':   field,
            'op':      op,
            'value':   value,
            'tags':    ['acl', 'migrated-v2'],
            'note':    f"from v2 acl id={a.get('id')}",
        }
        rules.append(rule)
        rset = {
            'id':         gen_uuid(),
            'name':       f"rs-{name}",
            'enabled':    True,
            'combinator': 'AND',
            'members':    [{'rule_ref': rule['name'], 'negate': False}],
            'tags':       ['acl', 'migrated-v2'],
            'note':       '',
        }
        rule_sets.append(rset)
        acl_policies.append({
            'id':                 gen_uuid(),
            'name':               f"acl-{name}",
            'enabled':            a.get('enabled', True),
            'priority':           a.get('priority', 100),
            'match_rule_set_ref': rset['name'],
            'scope':              scope,
            'scope_ref':          '',
            'action':             kind,  # allow | deny
            'tags':               ['migrated-v2'],
            'note':               f"from v2 acl id={a.get('id')}",
        })
    return rules, rule_sets, acl_policies


def convert_old_routes(v2_old_routes: list[dict], trunk_name_by_id: dict[Any, str]) -> tuple[
    list[dict], list[dict], list[dict]
]:
    """구 routes.jsonl (match_json+target) → rules + rule_sets + routing_policies"""
    rules = []
    rule_sets = []
    routing_policies = []

    for i, ro in enumerate(v2_old_routes):
        name = ro.get('name') or f"route-{i+1}"
        priority = ro.get('priority', 100)
        target_mode = ro.get('target_mode', 'trunk')
        target_ref = ro.get('target_ref', '')
        fail_action = ro.get('fail_action', 'next_rule')
        if fail_action == 'next_rule':
            fail_action = 'next_policy'

        match_json_s = ro.get('match_json') or '[]'
        try:
            conds = json.loads(match_json_s)
            if not isinstance(conds, list):
                conds = []
        except json.JSONDecodeError:
            conds = []

        rule_refs = []
        for j, c in enumerate(conds):
            field_raw = c.get('field', 'req_uri_user')
            # field 이름 매핑 (v2 가 다양하게 썼을 수 있으므로 best-effort)
            field_map = {
                'req_uri_user': 'req_uri_user', 'req_uri_host': 'req_uri_host',
                'to_uri_user':  'to_uri_user',  'to_uri_host':  'to_uri_host',
                'from_uri_user':'from_uri_user','from_uri_host':'from_uri_host',
                'src_ip':       'src_ip',       'dst_ip':       'dst_ip',
                'method':       'method',       'user_agent':   'user_agent',
            }
            field = field_map.get(field_raw, field_raw)
            op = c.get('op', 'eq')
            op_map = {
                'eq': 'eq', 'ne': 'ne', 'prefix': 'prefix', 'suffix': 'suffix',
                'contains': 'contains', 'regex': 'regex',
                'cidr': 'in_cidr', 'in_cidr': 'in_cidr', 'in_list': 'in_list',
            }
            op = op_map.get(op, 'eq')
            rule = {
                'id':      gen_uuid(),
                'name':    f"rule-{name}-{j+1}",
                'enabled': True,
                'field':   field,
                'op':      op,
                'value':   str(c.get('value', '')),
                'tags':    ['routing', 'migrated-v2'],
                'note':    f"from v2 route '{name}' cond[{j}]",
            }
            rules.append(rule)
            rule_refs.append(rule['name'])

        rset = {
            'id':         gen_uuid(),
            'name':       f"rs-{name}",
            'enabled':    True,
            'combinator': 'AND',
            'members':    [{'rule_ref': r, 'negate': False} for r in rule_refs],
            'tags':       ['routing', 'migrated-v2'],
            'note':       '',
        }
        rule_sets.append(rset)

        # target 매핑
        if target_mode == 'reject':
            tgt_type = 'reject'
            tgt_ref = ''
        elif target_mode == 'trunk':
            # 기존 trunk name (또는 id) → 새 RouteSet 이름은 알 수 없음.
            # 운영자가 수동으로 route_set 으로 묶어야 함. 일단 access_service 가 아닌 route_set 으로 두고 ref 비움.
            tgt_type = 'route_set'
            tgt_ref = ''
        elif target_mode == 'service':
            # service_id → ibcf 이면 route_set, voip/ptt 이면 access_service.
            # 이 시점에선 알 수 없으므로 일단 route_set 으로 기록, 운영자 확인 필요.
            tgt_type = 'access_service'
            tgt_ref = str(target_ref)
        else:
            tgt_type = 'route_set'
            tgt_ref = str(target_ref) if target_ref else ''

        routing_policies.append({
            'id':                     gen_uuid(),
            'name':                   f"rp-{name}",
            'enabled':                ro.get('enabled', True),
            'priority':               priority,
            'match_rule_set_ref':     rset['name'],
            'target_type':            tgt_type,
            'target_ref':             tgt_ref,
            'transform_rule_set_refs':[],
            'fail_action':            fail_action,
            'tags':                   ['migrated-v2'],
            'note':                   f"from v2 route '{name}' (target_mode={target_mode})",
        })

    return rules, rule_sets, routing_policies


# ──────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('install_path', help='CSP install_path (contains config/)')
    ap.add_argument('--apply', action='store_true', help='실제 파일 쓰기')
    args = ap.parse_args()

    root = Path(args.install_path)
    cfg = root / 'config'
    if not cfg.is_dir():
        print(f"❌ {cfg} 가 존재하지 않습니다", file=sys.stderr)
        sys.exit(1)

    v2_listeners = read_jsonl(cfg / 'listeners.jsonl')
    v2_trunks    = read_jsonl(cfg / 'trunks.jsonl')
    v2_old_routes= read_jsonl(cfg / 'routes.jsonl')    # v2 스키마 (match+target)
    v2_acl       = read_jsonl(cfg / 'acl.jsonl')
    v2_services  = read_jsonl(cfg / 'services.jsonl')

    print(f"입력: listeners={len(v2_listeners)} trunks={len(v2_trunks)} "
          f"routes(v2)={len(v2_old_routes)} acl={len(v2_acl)} services={len(v2_services)}")

    # 구 routes 의 스키마가 신 스키마 ((LN,RN) pair) 와 다르므로 판별:
    #  - 만약 routes.jsonl 의 첫 레코드에 'local_node_ref'/'remote_node_ref' 가 있으면 이미 v3 → 건너뜀.
    if v2_old_routes and 'local_node_ref' in v2_old_routes[0]:
        print("⚠ routes.jsonl 이 이미 v3 형식 — 그대로 유지")
        v2_old_routes_is_v2 = False
    else:
        v2_old_routes_is_v2 = True

    # 변환
    local_nodes = convert_listeners(v2_listeners)
    access_services, remote_nodes, routes_new, route_sets = convert_services_and_trunks(
        v2_services, v2_trunks)
    rules_a, rule_sets_a, acl_policies = convert_acl(v2_acl)
    if v2_old_routes_is_v2:
        trunk_name_by_id = {t.get('id'): t.get('name') for t in v2_trunks}
        rules_b, rule_sets_b, routing_policies = convert_old_routes(v2_old_routes, trunk_name_by_id)
    else:
        rules_b, rule_sets_b, routing_policies = [], [], []
        # routes 는 이미 v3 이므로 그대로
        routes_new = v2_old_routes

    all_rules = rules_a + rules_b
    all_rule_sets = rule_sets_a + rule_sets_b

    print(f"출력: local_nodes={len(local_nodes)} remote_nodes={len(remote_nodes)} "
          f"routes={len(routes_new)} route_sets={len(route_sets)}")
    print(f"       rules={len(all_rules)} rule_sets={len(all_rule_sets)} "
          f"routing_policies={len(routing_policies)} acl_policies={len(acl_policies)} "
          f"access_services={len(access_services)}")

    out_map = {
        'local_nodes.jsonl':       local_nodes,
        'remote_nodes.jsonl':      remote_nodes,
        'routes.jsonl':            routes_new,
        'route_sets.jsonl':        route_sets,
        'rules.jsonl':             all_rules,
        'rule_sets.jsonl':         all_rule_sets,
        'routing_policies.jsonl':  routing_policies,
        'acl_policies.jsonl':      acl_policies,
        'access_services.jsonl':   access_services,
    }

    if not args.apply:
        print("\n[dry-run] 아무 파일도 쓰지 않음. 실제 실행: --apply")
        return

    # 기존 구 파일 백업
    v2_files = ['listeners.jsonl', 'trunks.jsonl', 'acl.jsonl', 'services.jsonl']
    if v2_old_routes_is_v2:
        v2_files.append('routes.jsonl')
    for fn in v2_files:
        p = cfg / fn
        if p.exists():
            bak = p.with_suffix('.jsonl.v2.bak')
            p.rename(bak)
            print(f"  backed up: {fn} → {bak.name}")

    # 신 파일 쓰기
    for fn, rows in out_map.items():
        p = cfg / fn
        write_jsonl(p, rows)
        print(f"  wrote: {fn} ({len(rows)} records)")

    print("\n✓ 마이그레이션 완료. 운영자 확인 필요 항목:")
    print("  - access_services.allowed_local_node_refs[]  (원본 listeners[] int id → name 미매핑)")
    print("  - routes.local_node_ref                       (어느 LN 으로 피어에 발신할지)")
    print("  - routing_policies.target_ref                 (target_mode=trunk/service 의 매핑)")


if __name__ == '__main__':
    main()
