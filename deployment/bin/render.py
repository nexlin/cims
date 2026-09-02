#!/usr/bin/env python3
"""
deployment/bin/render.py — env.yaml + scenario.yaml → 노드별 설정 bundle 생성.

USAGE
  ./render.py --env <env_dir> --scenario <scenario_name> [--out <bundle_dir>] [--check-only]

  예) ./render.py --env prod-multi-host --scenario volte-ptt
       → ./bundle/prod-multi-host__volte-ptt/{ctrl-a,ctrl-b,media-a,media-b}/

OUTPUT (per CSP node)
  <node>/csp.json                       csp 모듈의 csp/config/csp.json
  <node>/config/local_nodes.jsonl       csp 모듈의 CSP/config/*.jsonl 9종
  <node>/config/remote_nodes.jsonl
  <node>/config/access_services.jsonl
  <node>/config/routes.jsonl
  <node>/config/route_sets.jsonl
  <node>/config/routing_policies.jsonl
  <node>/config/rules.jsonl
  <node>/config/rule_sets.jsonl
  <node>/config/acl_policies.jsonl
  <node>/user/<sip_id>.json             csp 모듈의 csp/user/<sip_id>.json

OUTPUT (per CMP node)
  <node>/cmp.json                       cmp 모듈의 cmp/config/cmp.json
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:
    sys.stderr.write("PyYAML 필요: pip install pyyaml\n")
    sys.exit(2)


# ─────────────────────────────────────────────────────────────
# 유틸
# ─────────────────────────────────────────────────────────────

class RenderError(Exception):
    """env/scenario 검증 실패 또는 자동 유도 불가."""


def _load_yaml(path: Path) -> dict:
    if not path.exists():
        raise RenderError(f"파일 없음: {path}")
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _write_json(path: Path, obj: Any, *, sort_keys: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=4, sort_keys=sort_keys)
        f.write("\n")


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False))
            f.write("\n")


# ─────────────────────────────────────────────────────────────
# 입력 검증
# ─────────────────────────────────────────────────────────────

def _validate_env(env: dict) -> None:
    nets = {n["id"] for n in env.get("networks", [])}
    node_ids: set[str] = set()
    agent_seen: dict[int, str] = {}

    for node in env.get("nodes", []):
        nid = node.get("id")
        if not nid:
            raise RenderError("node.id 누락")
        if nid in node_ids:
            raise RenderError(f"node.id 중복: {nid}")
        node_ids.add(nid)

        aid = node.get("agent_id")
        if aid is not None:
            if aid in agent_seen:
                raise RenderError(f"agent_id={aid} 가 노드 '{agent_seen[aid]}' 와 '{nid}' 에 동시 매핑")
            agent_seen[aid] = nid

        for nic in node.get("nics", []) or []:
            if nic.get("net") not in nets:
                raise RenderError(f"node '{nid}' nic '{nic.get('iface')}' 가 알 수 없는 net={nic.get('net')} 참조")

    for hg in env.get("ha_groups", []):
        for m in hg.get("members", []) or []:
            if m.get("node") not in node_ids:
                raise RenderError(f"ha_group '{hg.get('name')}' 멤버 '{m.get('node')}' 미등록")
        for vip in hg.get("vips", []) or []:
            if vip.get("net") not in nets:
                raise RenderError(f"vip slot={vip.get('slot')} net={vip.get('net')} 미등록")


def _validate_scenario(scn: dict, env: dict) -> None:
    env_name = env.get("name")
    if scn.get("env") not in (None, env_name):
        raise RenderError(f"scenario.env={scn.get('env')} ≠ env.name={env_name}")

    hg_ids = {hg["id"] for hg in env.get("ha_groups", [])}
    hg_names = {hg.get("name"): hg["id"] for hg in env.get("ha_groups", [])}

    for dep in scn.get("deployments", []) or []:
        ref = dep.get("ha_group")
        if ref in hg_ids:
            continue
        if ref in hg_names:
            dep["ha_group"] = hg_names[ref]  # name → id 정규화
            continue
        raise RenderError(f"deployments.ha_group={ref} 가 env 에 없음")


# ─────────────────────────────────────────────────────────────
# 인덱스
# ─────────────────────────────────────────────────────────────

class Index:
    """env + scenario 의 조회 인덱스 — 노드 → nic 별 IP, ha_group → 멤버 노드, etc."""

    def __init__(self, env: dict, scn: dict):
        self.env = env
        self.scn = scn
        self.networks = {n["id"]: n for n in env.get("networks", [])}
        self.nodes = {n["id"]: n for n in env.get("nodes", [])}
        self.ha_groups = {hg["id"]: hg for hg in env.get("ha_groups", [])}

        self.deployments: dict[int, list[dict]] = {}     # ha_group_id → packages
        for dep in scn.get("deployments", []) or []:
            self.deployments.setdefault(dep["ha_group"], []).extend(dep.get("packages", []) or [])

    def node_ip(self, node_id: str, net: str) -> str | None:
        node = self.nodes.get(node_id)
        if not node:
            return None
        for nic in node.get("nics", []) or []:
            if nic.get("net") == net:
                return nic.get("ip")
        return None

    def service_ip(self, node_id: str) -> str | None:
        """svc net 우선, 없으면 loopback, 그래도 없으면 첫 NIC. single-host/multi-host 공통."""
        for candidate in ("svc", "loopback"):
            ip = self.node_ip(node_id, candidate)
            if ip:
                return ip
        node = self.nodes.get(node_id) or {}
        for nic in node.get("nics") or []:
            if nic.get("ip"):
                return nic["ip"]
        return None

    def service_net(self) -> str:
        """첫 멤버의 service NIC net 이름. VIP net 매칭 등에 사용."""
        for candidate in ("svc", "loopback"):
            if any(candidate == nic.get("net")
                   for n in self.nodes.values() for nic in (n.get("nics") or [])):
                return candidate
        # 첫 노드 첫 NIC
        for n in self.nodes.values():
            for nic in n.get("nics") or []:
                return nic.get("net", "svc")
        return "svc"

    def ha_for_package(self, pkg_name: str) -> int | None:
        for hg_id, pkgs in self.deployments.items():
            if any(p.get("name") == pkg_name for p in pkgs):
                return hg_id
        return None

    def ha_members(self, hg_id: int) -> list[dict]:
        hg = self.ha_groups.get(hg_id)
        return (hg or {}).get("members", []) or []

    def vip(self, hg_id: int, net: str) -> str | None:
        hg = self.ha_groups.get(hg_id)
        if not hg:
            return None
        for vip in hg.get("vips", []) or []:
            if vip.get("net") == net:
                return vip.get("ip")
        return None


# ─────────────────────────────────────────────────────────────
# Layer 생성 — CSP
# ─────────────────────────────────────────────────────────────

CSP_LISTEN_PORTS = {
    "UDP": ("csp-main-udp", 5060, "UDP"),
    "TCP": ("csp-main-tcp", 25061, "TCP"),
    "TLS": ("csp-main-tls", 5061, "TLS"),
}


def _build_local_nodes(idx: Index, scn_csp: dict) -> list[dict]:
    cfg = scn_csp.get("local_nodes") or {}
    rows: list[dict] = []

    if cfg.get("auto"):
        hg_id = idx.ha_for_package("csp")
        if hg_id is None:
            raise RenderError("local_nodes.auto=true 인데 csp 패키지가 어느 ha_group 에도 매핑되지 않음")
        hg = idx.ha_groups[hg_id]
        mode = hg.get("mode")
        # AS — VIP bind. AA — 멤버별 svc IP bind (멀티 row).
        snet = idx.service_net()
        if mode == "active_standby":
            vip_ip = idx.vip(hg_id, snet)
            if not vip_ip:
                raise RenderError(f"ha_group '{hg.get('name')}' 의 {snet} VIP 미정의")
            for _, (lid, port, proto) in CSP_LISTEN_PORTS.items():
                # csp 는 single is_primary 만 허용 (CspLocalNodeMap) — UDP 만 primary.
                # TCP/TLS 는 같은 VIP 의 보조 transport.
                row = {
                    "id": lid, "name": lid, "edge": "access",
                    "bind_ip": vip_ip, "bind_port": port, "protocol": proto,
                    "thread_count": 2, "enabled": True,
                    "is_primary": (proto == "UDP"),
                    "tags": [],
                    "note": f"{idx.env.get('name')} {idx.scn.get('name')} — VIP {proto}",
                }
                if proto == "TLS":
                    row["tls_cert_path"] = "cert/csp.pem"
                rows.append(row)
        elif mode == "all_active":
            first_node = idx.ha_members(hg_id)[0]["node"]
            for m in idx.ha_members(hg_id):
                node_id = m["node"]
                svc_ip = idx.service_ip(node_id)
                if not svc_ip:
                    raise RenderError(f"node '{node_id}' 가 svc net 에 NIC 없음")
                for _, (lid_base, port, proto) in CSP_LISTEN_PORTS.items():
                    lid = f"{lid_base}-{node_id}"
                    # AA: 첫 멤버의 UDP 만 is_primary (single primary 보장)
                    is_primary = (node_id == first_node and proto == "UDP")
                    row = {
                        "id": lid, "name": lid, "edge": "access",
                        "bind_ip": svc_ip, "bind_port": port, "protocol": proto,
                        "thread_count": 2, "enabled": True, "is_primary": is_primary,
                        "tags": [], "note": f"AA member {node_id} {proto}",
                    }
                    if proto == "TLS":
                        row["tls_cert_path"] = "cert/csp.pem"
                    rows.append(row)
        elif mode == "standalone":
            m = idx.ha_members(hg_id)[0]
            svc_ip = idx.service_ip(m["node"]) or "0.0.0.0"
            for _, (lid, port, proto) in CSP_LISTEN_PORTS.items():
                row = {
                    "id": lid, "name": lid, "edge": "access",
                    "bind_ip": svc_ip, "bind_port": port, "protocol": proto,
                    "thread_count": 2, "enabled": True,
                    "is_primary": (proto == "UDP"),     # single primary
                    "tags": [],
                    "note": f"standalone {proto}",
                }
                if proto == "TLS":
                    row["tls_cert_path"] = "cert/csp.pem"
                rows.append(row)
        else:
            raise RenderError(f"알 수 없는 ha_group mode: {mode}")

    for ovr in cfg.get("overrides") or []:
        # override 는 그대로 append (id 충돌 시 검증에서 잡힘)
        rows.append({
            "id": ovr["id"], "name": ovr.get("name", ovr["id"]),
            "edge": ovr.get("edge", "access"),
            "bind_ip": ovr["bind_ip"], "bind_port": ovr["bind_port"],
            "protocol": ovr.get("transport", ovr.get("protocol", "UDP")).upper(),
            "thread_count": ovr.get("thread_count", 2),
            "enabled": ovr.get("enabled", True),
            "is_primary": ovr.get("is_primary", False),
            "tags": ovr.get("tags", []),
            "note": ovr.get("note", "override"),
        })

    # 중복 id 검사
    ids: set[str] = set()
    for r in rows:
        if r["id"] in ids:
            raise RenderError(f"local_nodes 에 중복 id={r['id']}")
        ids.add(r["id"])
    return rows


def _build_remote_nodes(idx: Index, scn_csp: dict) -> list[dict]:
    cfg = scn_csp.get("remote_nodes") or {}
    rows: list[dict] = []

    if cfg.get("auto_cmp"):
        hg_id = idx.ha_for_package("cmp")
        if hg_id is None:
            sys.stderr.write("[warn] remote_nodes.auto_cmp=true 인데 cmp 패키지 배포 ha_group 없음 — skip\n")
        else:
            for m in idx.ha_members(hg_id):
                node_id = m["node"]
                svc_ip = idx.service_ip(node_id)
                if not svc_ip:
                    raise RenderError(f"cmp 멤버 '{node_id}' 의 svc IP 없음")
                # csp 의 RemoteNodeMap 스키마 (CspRemoteNodeMap.cpp:34-45):
                # id/name/ip/port/protocol/remote_domain/srv_lookup/dns_fallback/tls_verify/enabled/tags/note
                rows.append({
                    "id": f"cmp-{node_id}", "name": f"cmp-{node_id}",
                    "ip": svc_ip, "port": 9000, "protocol": "UDP",
                    "remote_domain": "", "srv_lookup": False,
                    "dns_fallback": True, "tls_verify": False,
                    "enabled": True, "tags": ["cmp"],
                    "note": f"auto cmp member {node_id}",
                })

    for ex in cfg.get("extra") or []:
        rows.append({
            "id": ex.get("id", ex.get("name", "")),
            "name": ex.get("name", ex["id"]),
            "ip": ex.get("ip", ex.get("host", "")),
            "port": ex.get("port", 5060),
            "protocol": ex.get("protocol", ex.get("transport", "UDP")).upper(),
            "remote_domain": ex.get("remote_domain", ""),
            "srv_lookup": bool(ex.get("srv_lookup", False)),
            "dns_fallback": bool(ex.get("dns_fallback", True)),
            "tls_verify": bool(ex.get("tls_verify", False)),
            "enabled": bool(ex.get("enabled", True)),
            "tags": list(ex.get("tags", [])),
            "note": ex.get("purpose", ex.get("note", "")),
        })

    return rows


def _build_access_services(scn_csp: dict, local_node_names: set[str]) -> list[dict]:
    """access_services.jsonl — csp 의 진짜 schema (CspServiceMap.cpp:24-67).

    필수: id (uuid 또는 int>0) / name / kind (volte|ptt 만) / domain.
    listeners 결정: allowed_local_node_refs[] (string name 배열).
    """
    rows: list[dict] = []
    for i, svc in enumerate(scn_csp.get("access_services") or [], start=1):
        kind = svc.get("kind", "volte")
        if kind not in ("volte", "ptt"):
            raise RenderError(f"access_service '{svc.get('name')}' kind='{kind}' 미지원 (volte|ptt 만)")
        # listener_ids 는 옛 호환 — 새 키는 allowed_local_node_refs (CspServiceMap.cpp).
        # 옛 키 사용 시 stderr 에 deprecation warn (한 번 출력 의도지만 svc 마다
        # 보내고 사용자가 빠르게 발견 가능하게 함 — 마이그레이션 후 사라짐).
        if "listener_ids" in svc and "allowed_local_node_refs" not in svc:
            sys.stderr.write(
                f"[deprecated] access_service '{svc.get('name')}' 의 listener_ids 는 "
                f"allowed_local_node_refs 로 rename (csp 진짜 schema). "
                f"render 는 호환 유지하지만 yaml 갱신 권장.\n"
            )
        refs = svc.get("allowed_local_node_refs") or svc.get("listener_ids") or []
        missing = [r for r in refs if r not in local_node_names]
        if missing:
            raise RenderError(
                f"access_service '{svc.get('name')}' allowed_local_node_refs 미존재: {missing}"
                f" (local_nodes={sorted(local_node_names)})"
            )
        rows.append({
            "id": svc.get("id", i),
            "name": svc["name"],
            "kind": kind,
            "domain": svc["domain"],
            "auth_realm": svc.get("auth_realm", svc["domain"]),
            "server_identity_uri": svc.get("server_identity_uri", ""),
            "inbound_policy": svc.get("inbound_policy", "any"),
            "priority": int(svc.get("priority", 100)),
            "enabled": bool(svc.get("enabled", True)),
            "allowed_local_node_refs": list(refs),
            "note": svc.get("note", ""),
        })
    return rows


def _build_routes(scn_csp: dict, local_ids: set[str], remote_ids: set[str]) -> list[dict]:
    """routes.jsonl — (local_node_ref, remote_node_ref) pair 가 SOT (csp/CspRouteMap.cpp).

    yaml 표현:
      routes:
        - name: trunk-pbx
          local_node_ref: csp-main-udp
          remote_node_ref: ibcf-trunk
          [outbound_proxy_ip, outbound_proxy_port, register_to_remote, ...]
    """
    rows: list[dict] = []
    pairs: set[tuple[str, str]] = set()
    for r in scn_csp.get("routes") or []:
        name = r.get("name") or r.get("id")
        ln = r.get("local_node_ref") or r.get("local")
        rn = r.get("remote_node_ref") or r.get("remote")
        if not name or not ln or not rn:
            raise RenderError(f"route 에 name/local_node_ref/remote_node_ref 필수: {r}")
        if ln not in local_ids:
            raise RenderError(f"route '{name}' local_node_ref='{ln}' 미존재 (local={sorted(local_ids)})")
        if rn not in remote_ids:
            raise RenderError(f"route '{name}' remote_node_ref='{rn}' 미존재 (remote={sorted(remote_ids)})")
        key = (ln, rn)
        if key in pairs:
            raise RenderError(f"route 중복 pair (local='{ln}', remote='{rn}')")
        pairs.add(key)
        rows.append({
            "id": r.get("id", name),
            "name": name,
            "local_node_ref": ln,
            "remote_node_ref": rn,
            "outbound_proxy_ip": r.get("outbound_proxy_ip", ""),
            "outbound_proxy_port": int(r.get("outbound_proxy_port", 0)),
            "register_to_remote": bool(r.get("register_to_remote", False)),
            "register_expires": int(r.get("register_expires", 3600)),
            "auth_user": r.get("auth_user", ""),
            "auth_password": r.get("auth_password", ""),
            "auth_realm": r.get("auth_realm", ""),
            "max_concurrent_calls": int(r.get("max_concurrent_calls", 0)),
            "cps_limit": int(r.get("cps_limit", 0)),
            "enabled": bool(r.get("enabled", True)),
            "tags": list(r.get("tags", [])),
            "note": r.get("note", ""),
        })
    return rows


def _build_route_sets(scn_csp: dict, route_names: set[str]) -> list[dict]:
    """route_sets.jsonl — members[].route_ref 가 routes.name 매칭 (CspRouteSetMap.cpp)."""
    rows: list[dict] = []
    for rs in scn_csp.get("route_sets") or []:
        name = rs.get("name") or rs.get("id")
        if not name:
            raise RenderError(f"route_set 에 name 필수: {rs}")
        members = []
        for m in rs.get("members") or []:
            if isinstance(m, str):
                m = {"route_ref": m}
            ref = m.get("route_ref")
            if ref not in route_names:
                raise RenderError(f"route_set '{name}' members.route_ref='{ref}' 미존재")
            members.append({
                "route_ref": ref,
                "priority": int(m.get("priority", 100)),
                "weight": int(m.get("weight", 1)),
            })
        rows.append({
            "id": rs.get("id", name),
            "name": name,
            "distribution_policy": rs.get("distribution_policy", "failover"),
            "health_check_mode": rs.get("health_check_mode", "options_ping"),
            "health_check_interval_sec": int(rs.get("health_check_interval_sec", 30)),
            "health_check_dead_threshold": int(rs.get("health_check_dead_threshold", 3)),
            "health_check_recovery_probes": int(rs.get("health_check_recovery_probes", 1)),
            "fallback_policy": rs.get("fallback_policy", "reject"),
            "enabled": bool(rs.get("enabled", True)),
            "members": members,
            "tags": list(rs.get("tags", [])),
            "note": rs.get("note", ""),
        })
    return rows


def _build_routing_policies(scn_csp: dict, route_set_names: set[str], rule_set_names: set[str]) -> list[dict]:
    """routing_policies.jsonl — match_rule_set_ref / target_ref (CspRoutingPolicyEngine.cpp)."""
    rows = []
    for p in scn_csp.get("routing_policies") or []:
        name = p.get("name") or p.get("id")
        if not name:
            raise RenderError(f"routing_policy 에 name 필수: {p}")
        mref = p.get("match_rule_set_ref", "")
        if mref and mref not in rule_set_names:
            raise RenderError(f"routing_policy '{name}' match_rule_set_ref='{mref}' 미존재")
        tref = p.get("target_ref")
        target_type = p.get("target_type", "route_set")
        if target_type == "route_set" and tref and tref not in route_set_names:
            raise RenderError(f"routing_policy '{name}' target_ref='{tref}' (route_set) 미존재")
        rows.append({
            "id": p.get("id", name),
            "name": name,
            "priority": int(p.get("priority", 100)),
            "match_rule_set_ref": mref,
            "target_type": target_type,
            "target_ref": tref or "",
            "transform_rule_set_refs": list(p.get("transform_rule_set_refs", [])),
            "fail_action": p.get("fail_action", "next_policy"),
            "enabled": bool(p.get("enabled", True)),
        })
    return rows


_RULE_FIELDS = {
    "from_uri_host", "from_uri_user", "to_uri_host", "to_uri_user",
    "req_uri_host", "req_uri_user", "src_ip", "dst_ip", "user_agent",
    "method", "p_asserted_identity", "via_host",
}
_RULE_OPS = {"exists", "not_exists", "eq", "ne", "prefix", "suffix",
             "contains", "regex", "in_cidr", "in_list"}


def _build_rules(scn_csp: dict) -> list[dict]:
    """rules.jsonl — name/field/op/value (CspRuleEvaluator.cpp:85-92).

    yaml 표현:
      rules:
        - name: allow-mgmt-net
          field: src_ip
          op: in_cidr
          value: "10.0.0.0/24"
    """
    rows = []
    for r in scn_csp.get("rules") or []:
        name = r.get("name") or r.get("id")
        field = r.get("field")
        op = r.get("op")
        value = r.get("value", "")
        if not name or not field or not op:
            raise RenderError(f"rule 에 name/field/op 필수: {r}")
        if field not in _RULE_FIELDS:
            raise RenderError(f"rule '{name}' field='{field}' 미지원 (지원: {sorted(_RULE_FIELDS)})")
        if op not in _RULE_OPS:
            raise RenderError(f"rule '{name}' op='{op}' 미지원 (지원: {sorted(_RULE_OPS)})")
        rows.append({
            "id": r.get("id", name),
            "name": name,
            "field": field,
            "op": op,
            "value": str(value),
            "enabled": bool(r.get("enabled", True)),
        })
    return rows


def _build_rule_sets(scn_csp: dict, rule_names: set[str]) -> list[dict]:
    """rule_sets.jsonl — members[].rule_ref/negate (CspRuleEvaluator.cpp:103-116)."""
    rows = []
    for rs in scn_csp.get("rule_sets") or []:
        name = rs.get("name") or rs.get("id")
        if not name:
            raise RenderError(f"rule_set 에 name 필수: {rs}")
        members = []
        for m in rs.get("members") or []:
            if isinstance(m, str):
                m = {"rule_ref": m}
            ref = m.get("rule_ref")
            if ref not in rule_names:
                raise RenderError(f"rule_set '{name}' members.rule_ref='{ref}' 미존재")
            members.append({
                "rule_ref": ref,
                "negate": bool(m.get("negate", False)),
            })
        rows.append({
            "id": rs.get("id", name),
            "name": name,
            "combinator": rs.get("combinator", "AND"),
            "members": members,
            "enabled": bool(rs.get("enabled", True)),
        })
    return rows


def _build_acl_policies(scn_csp: dict, rule_set_names: set[str]) -> list[dict]:
    """acl_policies.jsonl — match_rule_set_ref/scope/scope_ref/action (CspAclPolicyEngine.cpp)."""
    rows = []
    for p in scn_csp.get("acl_policies") or []:
        name = p.get("name") or p.get("id")
        mref = p.get("match_rule_set_ref")
        if not name or not mref:
            raise RenderError(f"acl_policy 에 name/match_rule_set_ref 필수: {p}")
        if mref not in rule_set_names:
            raise RenderError(f"acl_policy '{name}' match_rule_set_ref='{mref}' 미존재")
        rows.append({
            "id": p.get("id", name),
            "name": name,
            "priority": int(p.get("priority", 100)),
            "match_rule_set_ref": mref,
            "scope": p.get("scope", "global"),
            "scope_ref": p.get("scope_ref", ""),
            "action": p.get("action", "deny"),
            "enabled": bool(p.get("enabled", True)),
        })
    return rows


# ─────────────────────────────────────────────────────────────
# csp.json (Setup.*) 빌드
# ─────────────────────────────────────────────────────────────

def _build_csp_json(idx: Index, node_id: str, scn: dict) -> OrderedDict:
    setup_in = (scn.get("csp_config") or {}).get("setup") or {}
    sip = setup_in.get("sip") or {}
    media = setup_in.get("media_server") or {}
    log = setup_in.get("log") or {}
    svc = scn.get("services") or {}

    # MediaServer.Host = cmp ha_group 의 첫 멤버 svc IP (auto)
    cmp_hg = idx.ha_for_package("cmp")
    media_host = ""
    if cmp_hg is not None and media.get("auto", True):
        members = idx.ha_members(cmp_hg)
        if members:
            media_host = idx.node_ip(members[0]["node"], "svc") or ""

    # 자기 노드 svc IP (LocalIp)
    self_svc_ip = idx.service_ip(node_id) or "0.0.0.0"

    # Audit.HaVip — HaRole=auto 에서 감시할 VIP. active_standby 이면 CSP 시그널링 VIP(로컬 소유=active).
    #   all_active/단일노드는 CSP 전용 VIP 가 없어 빈 값(=auto 는 active). 명시 audit.ha_vip 설정이 우선.
    audit_ha_vip = (media.get("audit") or {}).get("ha_vip")
    if audit_ha_vip is None:
        audit_ha_vip = ""
        try:
            _csp_hg = idx.ha_for_package("csp")
            if _csp_hg is not None and idx.ha_groups[_csp_hg].get("mode") == "active_standby":
                audit_ha_vip = idx.vip(_csp_hg, idx.service_net()) or ""
        except Exception:
            audit_ha_vip = ""

    # Database — env.database 가 null 이면 placeholder (file fallback 진입)
    db = idx.env.get("database") or {
        "host": "127.0.0.1", "port": 3306, "user": "cims", "password": "cims1234", "dbname": "cims",
    }

    out = OrderedDict()
    out["Setup"] = OrderedDict([
        # SIP bind (LocalIp/UdpPort/TcpPort/TlsPort/CertFile) 는 local_nodes.jsonl 가 SoT.
        # csp.json 의 Setup.Sip 은 scalar tick/timeout 만 유지 — 변경 시 재기동 필요.
        ("Sip", OrderedDict([
            ("UdpThreadCount",     sip.get("udp_thread_count", 2)),
            ("StackExecutePeriod", sip.get("stack_execute_period", 20)),
            ("MinRegisterTimeout", sip.get("min_register_timeout", 60)),
            ("UserTimeout",        sip.get("user_timeout", 3600)),
            ("SendOptionsPeriod",  sip.get("send_options_period", 0)),
            ("CallPickupId",       sip.get("call_pickup_id", "**")),
            ("StaleCallTimeout",   sip.get("stale_call_timeout", 300)),
            ("TcpThreadCount",     sip.get("tcp_thread_count", 2)),
            ("TcpRecvTimeout",     sip.get("tcp_recv_timeout", 600)),
            ("TlsAcceptTimeout",   sip.get("tls_accept_timeout", 10)),
        ])),
        ("Roles", OrderedDict([
            ("CSCF",   bool(svc.get("cscf",   True))),
            ("TAS",    bool(svc.get("tas",    True))),
            ("PTT_AS", bool(svc.get("ptt_as", True))),
            ("IBCF",   bool(svc.get("ibcf",   False))),
        ])),
        ("MediaServer", OrderedDict([
            ("Enable",      bool(media.get("enable", True))),
            ("Host",        media.get("host", media_host)),
            ("ControlPort", media.get("control_port", 9000)),
            ("LocalPort",   media.get("local_port", 9001)),
            ("LocalIp",     media.get("local_ip", self_svc_ip)),
            # 세션 재조정(audit 수준2) — CSP↔CMP 자원 정합 (ha_design.md §5.6 / cmp_media_api.md §5.3).
            #   HaRole=auto: HaVip 설정 시 VIP 로컬 소유로 active/standby 동적 판정(hot-standby 승격을
            #   별도 훅 없이 audit cycle 에 반영), 미설정(단일노드/all_active)이면 active 취급.
            ("Audit", OrderedDict([
                ("Enable",         bool((media.get("audit") or {}).get("enable", True))),
                ("GraceSec",       int((media.get("audit") or {}).get("grace_sec", 30))),
                ("MaxPerCycle",    int((media.get("audit") or {}).get("max_per_cycle", 20))),
                ("ZombieTeardown", bool((media.get("audit") or {}).get("zombie_teardown", False))),
                ("HaRole",         (media.get("audit") or {}).get("ha_role", "auto")),
                ("HaVip",          audit_ha_vip),
            ])),
        ])),
        ("Log", OrderedDict([
            ("Folder",  log.get("folder", "log")),
            ("MaxSize", log.get("max_size_mb", 10) * 1_000_000),
            ("Level", OrderedDict([
                ("Debug",   bool((log.get("level") or {}).get("debug", True))),
                ("Info",    bool((log.get("level") or {}).get("info", True))),
                ("Network", bool((log.get("level") or {}).get("network", True))),
                ("Sql",     bool((log.get("level") or {}).get("sql", False))),
            ])),
        ])),
        ("Database", OrderedDict([
            ("Host",     db.get("host", "127.0.0.1")),
            ("Port",     db.get("port", 3306)),
            ("User",     db.get("user", "cims")),
            ("Password", db.get("password", "cims1234")),
            ("DbName",   db.get("dbname", "cims")),
        ])),
        ("DataFolder", OrderedDict([("User", "user"), ("Group", "group"), ("DispatchGroup", "dispatch_group")])),
    ])

    # ServiceLogging — env.service_logging 또는 setup.service_logging override
    sl_env = idx.env.get("service_logging") or {}
    sl_scn = setup_in.get("service_logging") or {}
    sl_dir = sl_scn.get("dir") or sl_env.get("dir") or "/var/log/cims/service_log"
    sl_enable = sl_scn.get("enable") or sl_env.get("enable") or ["sip", "cmp", "csc"]
    sl_recording = sl_scn.get("recording", sl_env.get("recording", True))
    out["Setup"]["ServiceLogging"] = OrderedDict([
        ("Dir", sl_dir),
        ("Enable", list(sl_enable)),
        ("Recording", bool(sl_recording)),
    ])

    # Monitor — host 의 admin/모니터링 endpoint
    mon = setup_in.get("monitor") or {}
    out["Setup"]["Monitor"] = OrderedDict([
        ("Port", mon.get("port", 16000)),
        ("ClientIpList", mon.get("client_ip_list", [])),
    ])

    # Security — SIP User-Agent 블랙리스트
    sec = setup_in.get("security") or {}
    out["Setup"]["Security"] = OrderedDict([
        ("DenySipUserAgentList", sec.get("deny_sip_user_agents", ["friendly-scanner", "sundayddr"])),
    ])
    return out


# ─────────────────────────────────────────────────────────────
# cmp.json 빌드
# ─────────────────────────────────────────────────────────────

def _build_cmp_json(idx: Index, node_id: str, scn: dict) -> OrderedDict:
    cmp_cfg = scn.get("cmp_config") or {}
    ovr = cmp_cfg.get("overrides") or {}
    svc_ip = idx.service_ip(node_id) or "0.0.0.0"

    csp_vip = ""
    csp_hg = idx.ha_for_package("csp")
    if csp_hg is not None:
        csp_vip = idx.vip(csp_hg, idx.service_net()) or ""
        if not csp_vip:
            members = idx.ha_members(csp_hg)
            if members:
                csp_vip = idx.service_ip(members[0]["node"]) or ""

    out = OrderedDict([
        ("ServerIp",           svc_ip),
        ("ServerPort",         ovr.get("server_port", 9000)),
        ("CspPort",            ovr.get("csp_port", 9001)),
        ("RtpStartPort",       ovr.get("rtp_start_port", 50000)),
        ("RtpPoolSize",        ovr.get("rtp_pool_size", 20)),
        ("PttRtpStartPort",    ovr.get("ptt_rtp_start_port", 52000)),
        ("PttRtpPoolSize",     ovr.get("ptt_rtp_pool_size", 10)),
        ("PttFloorStartPort",  ovr.get("ptt_floor_start_port", 54000)),
        ("PttVideoStartPort",  ovr.get("ptt_video_start_port", 56000)),
        ("RtpWorkerCount",     ovr.get("rtp_worker_count", 4)),
        ("EnableDtmfPtt",      bool(ovr.get("enable_dtmf_ptt", True))),
        ("DtmfPushDigit",      ovr.get("dtmf_push_digit", "*")),
        ("DtmfReleaseDigit",   ovr.get("dtmf_release_digit", "#")),
        ("SessionTimeout",     ovr.get("session_timeout", 600)),
        ("LogLevel",           ovr.get("log_level", "INFO")),
        ("LogDir",             ovr.get("log_dir", "log")),
        ("LogMaxSizeMB",       ovr.get("log_max_size_mb", 10)),
        ("LogMaxFiles",        ovr.get("log_max_files", 5)),
        ("SystemId",           ovr.get("system_id", f"cmp_{node_id}")),
        ("RtpIp",              svc_ip),
        ("CspIp",              csp_vip),
    ])

    # ServiceLogging — env.service_logging 또는 cmp_config.overrides.service_logging
    sl_env = idx.env.get("service_logging") or {}
    sl_ovr = ovr.get("service_logging") or {}
    out["ServiceLogging"] = OrderedDict([
        ("Dir",        sl_ovr.get("dir", sl_env.get("dir", "/var/log/cims/service_log"))),
        ("Enable",     list(sl_ovr.get("enable", sl_env.get("enable_for_cmp", ["csp"])))),
        ("MediaTypes", list(sl_ovr.get("media_types", ["floor", "dtmf"]))),
        ("Flow", OrderedDict([
            ("Floor", bool((sl_ovr.get("flow") or {}).get("floor", True))),
            ("Dtmf",  bool((sl_ovr.get("flow") or {}).get("dtmf", True))),
            ("Rtcp",  bool((sl_ovr.get("flow") or {}).get("rtcp", False))),
        ])),
    ])
    return out


# ─────────────────────────────────────────────────────────────
# user seed
# ─────────────────────────────────────────────────────────────

def _build_user(u: dict) -> tuple[str, dict]:
    sip_id = u["sip_id"]
    auth_id = u.get("auth_id", "")
    domain = u.get("domain", "")
    # 어제 LIVE 결과: auth_id 는 IMSI@DOMAIN 풀폼으로 저장 (VoLTE 모드에서 cspsim 가 풀폼 명시 필요)
    full_auth = auth_id if "@" in auth_id else f"{auth_id}@{domain}"
    body = OrderedDict([
        ("auth_id",     full_auth),
        ("passwd",      u.get("passwd", "123456")),
        ("org_id",      u.get("org_id", "")),
        ("dnd",         "false"),
        ("forward_id",  ""),
        ("reject_id",   []),
        ("create_time", "2026-03-19 00:00:00.000"),
        ("update_time", "2026-03-19 00:00:00.000"),
        ("imsi",        auth_id.split("@")[0] if "@" in auth_id else auth_id),
        ("service_ref", u.get("service_ref", "")),
    ])
    return sip_id, body


def _enrich_users_with_bindings(scn: dict) -> list[dict]:
    users = list((scn.get("subscribers") or {}).get("users") or [])
    bind: dict[str, str] = {}
    for b in (scn.get("subscribers") or {}).get("volte_bindings") or []:
        bind[b["user"]] = b["service"]
    for b in (scn.get("subscribers") or {}).get("ptt_bindings") or []:
        bind[b["user"]] = b["service"]
    out = []
    for u in users:
        u = dict(u)
        if not u.get("service_ref") and u["sip_id"] in bind:
            u["service_ref"] = bind[u["sip_id"]]
        out.append(u)
    return out


# ─────────────────────────────────────────────────────────────
# 렌더링 entry
# ─────────────────────────────────────────────────────────────

def render(env_dir: Path, scenario_name: str, out_dir: Path, *, check_only: bool = False) -> None:
    env = _load_yaml(env_dir / "env.yaml")
    scn = _load_yaml(env_dir / "scenarios" / f"{scenario_name}.yaml")

    _validate_env(env)
    _validate_scenario(scn, env)

    idx = Index(env, scn)
    scn_csp = scn.get("csp_config") or {}

    # CSP layer 산출 (검증 동시 수행)
    local_nodes = _build_local_nodes(idx, scn_csp)
    local_names = {r["name"] for r in local_nodes}
    remote_nodes = _build_remote_nodes(idx, scn_csp)
    remote_names = {r["name"] for r in remote_nodes}
    access_services = _build_access_services(scn_csp, local_names)
    routes = _build_routes(scn_csp, local_names, remote_names)
    route_names = {r["name"] for r in routes}
    rules = _build_rules(scn_csp)
    rule_names = {r["name"] for r in rules}
    rule_sets = _build_rule_sets(scn_csp, rule_names)
    rule_set_names = {r["name"] for r in rule_sets}
    route_sets = _build_route_sets(scn_csp, route_names)
    route_set_names = {r["name"] for r in route_sets}
    routing_policies = _build_routing_policies(scn_csp, route_set_names, rule_set_names)
    acl_policies = _build_acl_policies(scn_csp, rule_set_names)

    users = _enrich_users_with_bindings(scn)

    if check_only:
        print(f"[ok] env={env.get('name')} scenario={scn.get('name')} — 검증 통과")
        print(f"     local_nodes={len(local_nodes)} remote_nodes={len(remote_nodes)} "
              f"access_services={len(access_services)} routes={len(routes)} rules={len(rules)} "
              f"users={len(users)}")
        return

    # 노드별 산출
    csp_hg = idx.ha_for_package("csp")
    csp_nodes = [m["node"] for m in idx.ha_members(csp_hg)] if csp_hg is not None else []
    cmp_hg = idx.ha_for_package("cmp")
    cmp_nodes = [m["node"] for m in idx.ha_members(cmp_hg)] if cmp_hg is not None else []

    out_dir.mkdir(parents=True, exist_ok=True)

    for node_id in csp_nodes:
        node_dir = out_dir / node_id
        _write_json(node_dir / "csp.json", _build_csp_json(idx, node_id, scn))
        cfg_dir = node_dir / "config"
        _write_jsonl(cfg_dir / "local_nodes.jsonl", local_nodes)
        _write_jsonl(cfg_dir / "remote_nodes.jsonl", remote_nodes)
        _write_jsonl(cfg_dir / "access_services.jsonl", access_services)
        _write_jsonl(cfg_dir / "routes.jsonl", routes)
        _write_jsonl(cfg_dir / "route_sets.jsonl", route_sets)
        _write_jsonl(cfg_dir / "routing_policies.jsonl", routing_policies)
        _write_jsonl(cfg_dir / "rules.jsonl", rules)
        _write_jsonl(cfg_dir / "rule_sets.jsonl", rule_sets)
        _write_jsonl(cfg_dir / "acl_policies.jsonl", acl_policies)
        if users:
            user_dir = node_dir / "user"
            for u in users:
                sip_id, body = _build_user(u)
                _write_json(user_dir / f"{sip_id}.json", body)
                # IMSI 별칭 — cspsim 가 VoLTE 모드에서 -user 에 IMSI 를 넘기는 경우
                # csp 는 <from_user>.json 으로 lookup 하므로 별칭 파일이 없으면 매칭 실패.
                imsi = body.get("imsi", "")
                if imsi and imsi != sip_id:
                    _write_json(user_dir / f"{imsi}.json", body)

    for node_id in cmp_nodes:
        node_dir = out_dir / node_id
        _write_json(node_dir / "cmp.json", _build_cmp_json(idx, node_id, scn))

    # manifest
    manifest = OrderedDict([
        ("env", env.get("name")),
        ("scenario", scn.get("name")),
        ("csp_nodes", csp_nodes),
        ("cmp_nodes", cmp_nodes),
        ("counts", OrderedDict([
            ("local_nodes",      len(local_nodes)),
            ("remote_nodes",     len(remote_nodes)),
            ("access_services",  len(access_services)),
            ("routes",           len(routes)),
            ("route_sets",       len(route_sets)),
            ("routing_policies", len(routing_policies)),
            ("rules",            len(rules)),
            ("rule_sets",        len(rule_sets)),
            ("acl_policies",     len(acl_policies)),
            ("users",            len(users)),
        ])),
    ])
    _write_json(out_dir / "manifest.json", manifest)
    print(f"[ok] bundle 생성: {out_dir}")
    print(f"     csp_nodes={csp_nodes} cmp_nodes={cmp_nodes}")
    print(f"     {dict(manifest['counts'])}")


# ─────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────

def _install_dst_for(env: dict, node: str, base: Path, version: str,
                      relpath: Path) -> Path | None:
    """bundle 의 상대경로 → install dir 의 절대경로 매핑 (apply.py 와 동일 로직).

    single-host/multi-host 공통 — base 아래 csp/ cmp/ 모듈 디렉토리 직접 매핑.
    (env/version 인자는 apply.py 의 동명 함수와 시그니처 정합 목적으로 유지.)
    """
    parts = relpath.parts
    if not parts:
        return None
    if parts[0] == "csp.json":
        return base / "csp" / "config" / "csp.json"
    if parts[0] == "cmp.json":
        return base / "cmp" / "config" / "cmp.json"
    if parts[0] == "config":
        return base / "csp" / "config" / Path(*parts[1:])
    if parts[0] == "user":
        return base / "csp" / "user" / Path(*parts[1:])
    return None


def _semantic_lines(path: Path) -> list[str]:
    """JSON/JSONL 파일을 의미적 비교 가능한 라인 시퀀스로 변환 (sort_keys)."""
    if not path.exists():
        return []
    text = path.read_text(encoding="utf-8")
    name = path.name
    if name.endswith(".jsonl"):
        out = []
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                out.append(json.dumps(obj, ensure_ascii=False, sort_keys=True))
            except json.JSONDecodeError:
                out.append(line)
        return sorted(out)   # jsonl 은 라인 순서 무관
    if name.endswith(".json"):
        try:
            obj = json.loads(text)
            return json.dumps(obj, ensure_ascii=False, indent=2, sort_keys=True).splitlines()
        except json.JSONDecodeError:
            return text.splitlines()
    return text.splitlines()


def _run_diff(env_dir: Path, scenario_name: str, base: Path | None, version: str) -> int:
    """render 결과와 install dir 의 현재 파일을 의미적 diff."""
    import difflib
    import tempfile

    env = _load_yaml(env_dir / "env.yaml")
    scn = _load_yaml(env_dir / "scenarios" / f"{scenario_name}.yaml")

    # base 결정 (single-host/multi-host 공통)
    if base is None:
        base = Path("/home/nex/work/cims/build/dist")

    with tempfile.TemporaryDirectory(prefix="render-diff-") as tmp:
        tmp_out = Path(tmp)
        render(env_dir, scenario_name, tmp_out)

        changed = 0
        unchanged = 0
        new_files = 0
        for node_dir in sorted(tmp_out.iterdir()):
            if not node_dir.is_dir():
                continue
            node = node_dir.name
            for bundle_file in sorted(node_dir.rglob("*")):
                if not bundle_file.is_file():
                    continue
                rel = bundle_file.relative_to(node_dir)
                live = _install_dst_for(env, node, base, version, rel)
                if live is None:
                    continue
                new = _semantic_lines(bundle_file)
                cur = _semantic_lines(live)
                if new == cur:
                    unchanged += 1
                    continue
                changed += 1 if cur else 0
                new_files += 0 if cur else 1
                short_dst = str(live)
                try:
                    short_dst = str(live.relative_to(base.parent))
                except ValueError:
                    pass
                print(f"\n──── {node}/{rel}  →  {short_dst}")
                if not cur:
                    print("  (신규 파일 — install dir 에 없음)")
                    continue
                diff = list(difflib.unified_diff(cur, new, fromfile="LIVE", tofile="RENDER", lineterm=""))
                for line in diff[:60]:   # 최대 60 라인
                    print(f"  {line}")
                if len(diff) > 60:
                    print(f"  ... (잘림, 총 {len(diff)} 라인)")

        print(f"\n[diff] changed={changed} new={new_files} unchanged={unchanged}")
        return 0 if (changed == 0 and new_files == 0) else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", required=True, help="환경 디렉토리 이름 (deployment/ 아래)")
    p.add_argument("--scenario", required=True, help="시나리오 이름 (scenarios/<n>.yaml)")
    p.add_argument("--out", help="bundle 출력 디렉토리 (기본 ./bundle/<env>__<scn>/)")
    p.add_argument("--check-only", action="store_true", help="파일 생성 없이 검증만 수행")
    p.add_argument("--diff", action="store_true", help="install dir 의 현재 파일과 의미적 diff (apply 전 미리보기)")
    p.add_argument("--base", help="install base 경로 (--diff 시 사용)")
    p.add_argument("--version", default="0.0.1", help="패키지 버전 (--diff 시 사용)")
    p.add_argument("--root", help="deployment/ 의 부모 (기본 자동 탐지)")
    args = p.parse_args()

    here = Path(__file__).resolve().parent
    root = Path(args.root) if args.root else here.parent
    env_dir = root / args.env
    out_dir = Path(args.out) if args.out else (Path.cwd() / "bundle" / f"{args.env}__{args.scenario}")

    try:
        if args.diff:
            return _run_diff(env_dir, args.scenario,
                              Path(args.base) if args.base else None, args.version)
        render(env_dir, args.scenario, out_dir, check_only=args.check_only)
    except RenderError as e:
        sys.stderr.write(f"[error] {e}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
