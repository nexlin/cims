"""
HA fan-out dispatcher — csp 런타임 컬렉션 변경을 그룹 멤버들에 전파.

흐름:
  1. ha_lookup.fanout_targets_for_collection() 으로 멤버 deployment 들 찾음
  2. 각 deployment 의 agent 에 sync_config job 1건씩 enqueue
  3. sync_txn.create() 로 트랜잭션 생성 (멤버별 expected ack 슬롯)
  4. sync_id 를 호출자에 반환 → API 응답에 포함되어 Console UI 가 polling

대상 0건 (단일 호스트 / agent 없음) 이면 NO-OP — sync_id=None. notify_config_change
는 호출자가 별도로 보냄 (단일 호스트 호환).
"""
from __future__ import annotations

from typing import Optional

from services import file_store, ha_lookup, sync_txn


# entity 이름 (csp_runtime 가 쓰는 짧은 약어) → agent pull URL 의 collection 토큰
_ENTITY_TO_COLLECTION = {
    "listener": "csp_listener",
    "trunk":    "sip_trunk",
    "route":    "routing_rule",
    "access":   "routing_access_list",
    "service":  "sip_service",
}


def collection_for_entity(entity: str) -> Optional[str]:
    return _ENTITY_TO_COLLECTION.get(entity)


def enqueue_collection_sync(config: dict, *,
                            entity: str,
                            op: str,
                            row_id: int,
                            actor: str = "console",
                            ttl_sec: int = 60) -> Optional[int]:
    """런타임 컬렉션 1건 변경 직후 호출. 멤버들에 sync_config job 일괄 enqueue +
    sync_txn 1건 생성 → sync_id 반환.

    대상 멤버 0건 (csp deployment 없음 / ha_group 미정의) → None 반환 (호출자는
    NO-OP 로 해석).

    Args:
      entity:  "listener"|"trunk"|"route"|"access"|"service"
      op:      "CREATE"|"UPDATE"|"DELETE"
      row_id:  변경된 row 의 id
    """
    collection = collection_for_entity(entity)
    if not collection:
        return None
    targets = ha_lookup.fanout_targets_for_collection(config, collection)
    if not targets:
        return None

    # 멤버별 sync_config job 생성 — 그 후 sync_txn 에 job_id 들 묶음.
    from handlers.agents import _job_create
    members: list[dict] = []
    for t in targets:
        params = {
            "collection":     collection,
            "op":             op,
            "row_id":         row_id,
            "install_path":   t.get("install_path"),
            "deployment_id":  t.get("deployment_id"),
            "ha_group_id":    t.get("ha_group_id"),
            # sync_id 는 _job_create 후에 채워 넣음 — sync_txn.create 시점에 알아냄
        }
        jid = _job_create(config, t["agent_id"], "sync_config", params)
        members.append({
            "agent_id":      t["agent_id"],
            "deployment_id": t.get("deployment_id"),
            "job_id":        jid,
        })

    txn = sync_txn.create(config,
                          collection=collection,
                          op=op,
                          members=members,
                          actor=actor,
                          ttl_sec=ttl_sec,
                          note=f"{entity}#{row_id}")

    # 각 job 의 params 에 sync_id 를 backfill — agent 가 ack 시 sync_id 알도록.
    from handlers.agents import _job_load, _job_update
    for m in members:
        j = _job_load(config, m["job_id"])
        if not j:
            continue
        p = j.get("params") or {}
        p["sync_id"] = txn["id"]
        _job_update(config, m["job_id"], {"params": p})

    return txn["id"]
