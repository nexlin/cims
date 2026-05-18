"""
HA fan-out sync 트랜잭션 store.

csc 가 컬렉션 변경 (또는 모듈 config 변경) 1건을 발생시키면 sync 트랜잭션 1건이
열린다. 트랜잭션은 그룹 멤버 각각에 대한 expected ack 슬롯을 들고, agent 가
sync_config (또는 update_config) job 완료 시 csc 의 ack endpoint 를 호출해서
슬롯을 채운다.

도메인: `csp_sync_txn` (file_store)

스키마:
{
  "id": int,
  "collection": str,         # "csp_listener" / "config" (흐름 A 의 경우)
  "op": str,                 # "create"|"update"|"delete"|"put_config"
  "actor": str,              # "console" / user id
  "members": [
    {
      "agent_id":      int,
      "deployment_id": int (optional),
      "job_id":        int (csc 가 enqueue 한 job),
      "status":        "pending"|"ack"|"nack"|"timeout",
      "error":         str,
      "ack_at":        iso8601
    }, ...
  ],
  "status":      "pending"|"partial"|"success"|"failed"|"timeout",
  "create_time": iso,
  "update_time": iso,
  "completed_at": iso | null,
  "ttl_sec":     int,
  "note":        str
}

규칙:
- 모든 멤버 status=ack  → status=success, completed_at set
- 1+ nack + 나머지 ack/timeout → status=failed
- 1+ pending + create_time + ttl 초과 → 별도 sweeper 가 timeout 처리
"""
from __future__ import annotations

from datetime import datetime, timedelta
from typing import Optional

from services import file_store


_DOMAIN = 'csp_sync_txn'
DEFAULT_TTL_SEC = 60


def _dir(config):
    return file_store.domain_dir(config, _DOMAIN)


def _now_iso() -> str:
    return datetime.now().isoformat(timespec='seconds')


def create(config, *,
           collection: str,
           op: str,
           members: list[dict],
           actor: str = "console",
           ttl_sec: int = DEFAULT_TTL_SEC,
           note: str = "") -> dict:
    """sync 트랜잭션 1건 생성. 호출자가 members 의 각 agent_id 에 대해 이미
    job 을 enqueue 한 후 job_id 를 채워서 넘겨야 함.

    members[i] = { agent_id, deployment_id?, job_id }
    """
    d = _dir(config)
    sid = file_store.next_id(d)
    now = _now_iso()
    member_rows = []
    for m in members:
        member_rows.append({
            'agent_id':      m.get('agent_id'),
            'deployment_id': m.get('deployment_id'),
            'job_id':        m.get('job_id'),
            'status':        'pending',
            'error':         None,
            'ack_at':        None,
        })
    obj = {
        'id':           sid,
        'collection':   collection,
        'op':           op,
        'actor':        actor,
        'members':      member_rows,
        'status':       'pending' if member_rows else 'success',
        'create_time':  now,
        'update_time':  now,
        'completed_at': now if not member_rows else None,
        'ttl_sec':      int(ttl_sec),
        'note':         note,
    }
    file_store.save(d, sid, obj)
    return obj


def get(config, sid: int) -> Optional[dict]:
    return file_store.by_id(_dir(config), sid)


def list_recent(config, limit: int = 50) -> list[dict]:
    rows = file_store.load_all(_dir(config))
    rows.sort(key=lambda r: r.get('id', 0), reverse=True)
    return rows[:limit]


def _derive_status(members: list[dict]) -> str:
    if not members:
        return 'success'
    counts = {'pending': 0, 'ack': 0, 'nack': 0, 'timeout': 0}
    for m in members:
        s = m.get('status') or 'pending'
        counts[s] = counts.get(s, 0) + 1
    if counts['pending'] > 0:
        # 일부 ack 있으면 partial, 아니면 pending
        return 'partial' if counts['ack'] > 0 else 'pending'
    if counts['nack'] > 0 or counts['timeout'] > 0:
        return 'failed'
    return 'success'


def ack(config, sid: int, agent_id: int, *,
        status: str = 'ack',
        error: Optional[str] = None) -> Optional[dict]:
    """멤버 슬롯 1개 갱신. status ∈ {ack, nack}.

    호출 후 트랜잭션 전체 status 가 갱신되고 success/failed 시 completed_at 채움.
    sid 없거나 멤버 매칭 실패 시 None.
    """
    txn = get(config, sid)
    if not txn:
        return None
    members = txn.get('members') or []
    hit = False
    for m in members:
        if m.get('agent_id') == agent_id and m.get('status') == 'pending':
            m['status'] = status
            m['error']  = error
            m['ack_at'] = _now_iso()
            hit = True
            break
    if not hit:
        # 멤버가 없거나 이미 종료된 슬롯 — idempotent 처리, 변경 없이 현재 상태 반환
        return txn
    overall = _derive_status(members)
    txn['status'] = overall
    txn['update_time'] = _now_iso()
    if overall in ('success', 'failed') and not txn.get('completed_at'):
        txn['completed_at'] = txn['update_time']
    file_store.save(_dir(config), sid, txn)
    return txn


def sweep_timeouts(config) -> int:
    """create_time + ttl_sec 초과한 pending 멤버를 timeout 으로 마크.

    Returns: timeout 처리된 트랜잭션 수 (멤버 수가 아님).
    """
    rows = file_store.load_all(_dir(config))
    now = datetime.now()
    touched = 0
    for txn in rows:
        if txn.get('status') in ('success', 'failed'):
            continue
        ttl = int(txn.get('ttl_sec') or DEFAULT_TTL_SEC)
        try:
            ct = datetime.fromisoformat(txn.get('create_time'))
        except Exception:
            continue
        if now - ct < timedelta(seconds=ttl):
            continue
        changed = False
        for m in txn.get('members') or []:
            if m.get('status') == 'pending':
                m['status'] = 'timeout'
                m['ack_at'] = _now_iso()
                changed = True
        if changed:
            txn['status'] = _derive_status(txn.get('members') or [])
            txn['update_time'] = _now_iso()
            if txn['status'] in ('success', 'failed') and not txn.get('completed_at'):
                txn['completed_at'] = txn['update_time']
            file_store.save(_dir(config), txn['id'], txn)
            touched += 1
    return touched
