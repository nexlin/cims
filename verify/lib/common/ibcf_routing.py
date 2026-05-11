"""IBCF routing 6종 jsonl 시드 helper — ISP 인스턴스 전용.

docs/design/features/sip_service_model.md §5-2 "표준" 예시를 기반으로
ISP (127.0.0.5:5060) 가 mock 외부 peer 로 호를 라우팅하는 최소 정책 세트를
생성한다. 시드 대상:

- local_nodes.jsonl      : 수신 LN (ISP peering edge)
- remote_nodes.jsonl     : mock peer (cspsim peer 모드로 띄울 UAS)
- routes.jsonl           : (LN, RN) pair
- route_sets.jsonl       : 단일 route 의 failover 집합
- rules.jsonl + rule_sets.jsonl  : Request-URI 도메인 매칭
- routing_policies.jsonl : RuleSet match → RouteSet target

local_nodes 는 ISP 가 이미 Setup.Sip.LocalIp/UdpPort 로 bind 중이므로
override 의미보단 routing_policies/route 의 local_node_ref 참조용 entry.
"""
from __future__ import annotations

import json
import os
import uuid


IBCF_PEER_DOMAIN = "trunk.peer.test"


def seed_ibcf_routing(cfg_dir: str,
                      isp_local_ip: str,
                      isp_local_port: int,
                      peer_ip: str,
                      peer_port: int,
                      peer_domain: str = IBCF_PEER_DOMAIN,
                      tag: str = "verify-ibcf-seed") -> int:
    """{cfg_dir}/<6개 jsonl> 작성. 시드된 collection 수 반환 (성공 시 6).

    참조 무결성: name 기반 (id 는 uuid 자동 생성). routes 가 local_nodes /
    remote_nodes 를 name 으로 참조, route_sets 가 routes 를 name 으로 참조,
    routing_policies 가 rule_sets + route_sets 를 name 으로 참조.
    """
    os.makedirs(cfg_dir, exist_ok=True)

    def _uid() -> str:
        return uuid.uuid4().hex

    ln_name = "lb-ibcf-peering"
    rn_name = "rn-mock-peer"
    route_name = "r-mock-peer"
    rs_name = "rs-mock-peer"
    rule_name = "rule-ibcf-peer-domain"
    ruleset_name = "rs-ibcf-outbound"
    policy_name = "rp-ibcf-out"

    local_nodes = [{
        "id": _uid(), "name": ln_name, "edge": "peering",
        "bind_ip": isp_local_ip, "bind_port": isp_local_port,
        "protocol": "UDP", "enabled": True, "is_primary": True,
        "tags": [tag, "ibcf"], "note": "ISP peering listener",
    }]

    remote_nodes = [{
        "id": _uid(), "name": rn_name,
        "ip": peer_ip, "port": peer_port, "protocol": "UDP",
        "remote_domain": peer_domain,
        "srv_lookup": False, "dns_fallback": False, "tls_verify": False,
        "enabled": True, "tags": [tag], "note": "mock external SIP peer",
    }]

    routes = [{
        "id": _uid(), "name": route_name,
        "local_node_ref": ln_name, "remote_node_ref": rn_name,
        "register_to_remote": False,
        "enabled": True, "tags": [tag], "note": "ISP → mock peer trunk",
    }]

    route_sets = [{
        "id": _uid(), "name": rs_name,
        "distribution_policy": "failover",
        "members": [{"route_ref": route_name, "priority": 100, "weight": 1}],
        # 헬스체크 OPTIONS ping 이 실패해도 첫 INVITE 는 보낸다. 1차 검증에서는
        # health_check_mode=none 으로 두어 routing 결정만 검증.
        "health_check_mode": "none",
        "fallback_policy": "reject",
        "enabled": True, "tags": [tag],
    }]

    rules = [{
        "id": _uid(), "name": rule_name,
        # cspsim caller 가 보낸 Request-URI 는 "sip:9000@trunk.peer.test@127.0.0.5".
        # psip CSipUri::ParseUser 가 첫 '@' 로 분리하므로:
        #   user = "9000"
        #   host = "trunk.peer.test@127.0.0.5"  (`@` 가 host 에 그대로 포함됨)
        # 따라서 host contains 로 외부 도메인을 식별. cspsim 의 URI 정리 (proper
        # contact/route 송출) 는 별도 라운드.
        "field": "req_uri_host", "op": "contains", "value": peer_domain,
        "enabled": True, "tags": [tag, "routing", "ibcf"],
    }]

    rule_sets = [{
        "id": _uid(), "name": ruleset_name,
        "combinator": "AND",
        "members": [{"rule_ref": rule_name, "negate": False}],
        "enabled": True, "tags": [tag],
    }]

    routing_policies = [{
        "id": _uid(), "name": policy_name,
        "priority": 50,
        "match_rule_set_ref": ruleset_name,
        "target_type": "route_set", "target_ref": rs_name,
        "transform_rule_set_refs": [],
        "fail_action": "reject",
        "enabled": True, "tags": [tag],
    }]

    collections = {
        "local_nodes.jsonl":      local_nodes,
        "remote_nodes.jsonl":     remote_nodes,
        "routes.jsonl":           routes,
        "route_sets.jsonl":       route_sets,
        "rules.jsonl":            rules,
        "rule_sets.jsonl":        rule_sets,
        "routing_policies.jsonl": routing_policies,
    }
    written = 0
    for fname, records in collections.items():
        path = os.path.join(cfg_dir, fname)
        with open(path, "w") as f:
            for rec in records:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        written += 1
    return written
