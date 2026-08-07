"""관리 store 소유권 리스 (epoch fence) — oam_ha.md §4.4

관리평면(OAM)은 `safety.class=shared_writer` 다: 공유 파일 write·스케줄러·VIP 무관
background 작업을 모두 가지므로 **VIP 이관만으로는 정합이 성립하지 않는다**. 이 모듈은
"이 프로세스가 관리 store 의 유일한 writer 인가" 를 판정하고, 아니면 write 를 거부한다.

store 는 **양 노드가 상시 마운트하는 공유 스토리지**(NAS)에 있다. 즉 파일시스템 계층이
동시 접근을 막아주지 않으므로 **이 모듈이 단일 writer 를 만드는 유일한 장치**다. 방어는
2층이다:

  1. **mount guard** — 공유 store 가 마운트되지 않은 채 뜨면 마운트 포인트 하부 로컬
     디스크에 두 번째 store 를 만들므로 OAM 이 기동을 거부한다.
     (§4.3, oam_app._assert_runtime_mount)
  2. **리스(이 모듈)** — 두 노드의 OAM(또는 같은 노드의 두 프로세스) 중 하나만 write 한다.

설계 제약:

- **시각 비교를 하지 않는다.** ha_service_model.md §15 가 크로스노드 시각 비교를 금지하고,
  실제로 노드 간 30초 시계 오차가 절체 포렌식을 무효화한 사례가 있다. 리스는 TTL 이 아니라
  **flock(커널 배타 잠금) + 단조 epoch** 으로 정의한다.
- **잠금이 실제로 동작하는지 자기검증한다.** 공유 스토리지에서 advisory lock 이 no-op 인
  구성(NFSv3 에 lockd 없음, `nolock` 마운트, 일부 CIFS)이면 flock 은 **항상 성공**하고
  펜싱이 조용히 사라진다 — 파일시스템 층이 없는 NAS 구성에서는 곧 데이터 손상이다.
  그래서 획득 시 **두 번째 fd 로 배타 잠금을 시도**해 반드시 실패하는지 확인하고,
  실패하지 않으면 소유권을 인정하지 않는다(read-only 로 남는다).
- **read-only 강등이 기본 실패 모드**다. 소유권이 없으면 프로세스를 죽이지 않고 write 만
  막는다 — 조회는 계속 되어야 운영자가 원인을 볼 수 있다.
"""
from __future__ import annotations

import fcntl
import json
import os
import socket
import time

_OWNER_FILE = '.owner.json'
_LOCK_FILE = '.owner.lock'

_STATE: dict = {
    'active': False,        # 리스 보유 여부
    'reason': 'not_acquired',
    'epoch': 0,
    'node_id': '',
    'path': '',
    'lost_at': None,
}
_LOCK_FH = None             # 프로세스 수명 동안 유지하는 flock 핸들 (GC 되면 잠금 해제)
_VERIFY_TTL = 1.0           # owner 레코드 재확인 최소 간격(초) — write 폭주 시 stat 절약
_LAST_VERIFY = [0.0, True]  # [ts, ok]


class LeaseLostError(RuntimeError):
    """관리 store 소유권이 없거나 상실됨 — write 거부. HTTP 409(not_lease_owner)로 매핑."""


def _node_id() -> str:
    return os.environ.get('CIMS_NODE_ID') or socket.gethostname() or 'unknown'


def _boot_id() -> str:
    try:
        with open('/proc/sys/kernel/random/boot_id') as f:
            return f.read().strip()
    except Exception:
        return ''


def state() -> dict:
    """현재 리스 상태 스냅샷 (콘솔·health 응답용)."""
    return dict(_STATE)


def is_active() -> bool:
    return bool(_STATE['active'])


def _read_owner(root: str) -> dict:
    try:
        with open(os.path.join(root, _OWNER_FILE)) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def _write_owner(root: str, rec: dict) -> None:
    p = os.path.join(root, _OWNER_FILE)
    tmp = p + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(rec, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, p)


def _locking_enforced(path: str) -> bool:
    """이 경로의 flock 이 실제로 배타적인가 — 두 번째 fd 로 확인.

    같은 프로세스라도 flock 은 **open file description 단위**라, 이미 잠긴 파일을 새로
    open 해서 `LOCK_EX|LOCK_NB` 하면 반드시 `BlockingIOError` 여야 한다. 성공하면 그
    파일시스템에서 잠금이 무의미하다는 뜻이므로 펜싱이 성립하지 않는다.
    """
    try:
        probe = open(path, 'a+')
    except Exception:
        return False                    # 열 수 없으면 판정 불가 → 보수적으로 실패
    try:
        fcntl.flock(probe.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return True                     # 기대한 결과 — 잠금이 강제된다
    else:
        fcntl.flock(probe.fileno(), fcntl.LOCK_UN)
        return False
    finally:
        probe.close()


def acquire(root: str) -> dict:
    """store 루트의 소유권 획득 시도. 결과 상태 dict 반환 (예외 없음).

    성공 조건: `<root>/.owner.lock` 에 배타 flock 획득. 성공 시 `.owner.json` 의 epoch 를
    +1 해 기록한다(단조 — 누가 나중에 잡았는지 시각 없이 판정). 실패는 다른 writer 존재를
    뜻하므로 read-only 로 강등한다."""
    global _LOCK_FH
    _STATE['path'] = root
    _STATE['node_id'] = _node_id()
    try:
        os.makedirs(root, exist_ok=True)
        fh = open(os.path.join(root, _LOCK_FILE), 'a+')
        try:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fh.close()
            _STATE.update(active=False, reason='locked_by_other_writer')
            return state()
        if not _locking_enforced(os.path.join(root, _LOCK_FILE)):
            # 잠금이 no-op 인 파일시스템 — 소유권을 주장하면 두 노드가 동시에 write 한다.
            fh.close()
            _STATE.update(active=False, reason='locking_not_enforced')
            return state()
        prev = _read_owner(root)
        epoch = int(prev.get('epoch') or 0) + 1
        rec = {
            'node_id': _node_id(),
            'epoch': epoch,
            'boot_id': _boot_id(),
            'pid': os.getpid(),
            # 사람이 읽는 용도 — 판정에는 쓰지 않는다(시각 비교 금지).
            'acquired_at': time.strftime('%Y-%m-%dT%H:%M:%S'),
            'prev_node_id': prev.get('node_id') or None,
        }
        _write_owner(root, rec)
        _LOCK_FH = fh                      # 프로세스 수명 동안 보유
        _STATE.update(active=True, reason='ok', epoch=epoch, lost_at=None)
        _LAST_VERIFY[0], _LAST_VERIFY[1] = time.time(), True
        return state()
    except Exception as e:
        _STATE.update(active=False, reason=f'acquire_failed:{e}')
        return state()


def _lose(reason: str) -> None:
    if _STATE['active']:
        _STATE['lost_at'] = time.strftime('%Y-%m-%dT%H:%M:%S')
    _STATE.update(active=False, reason=reason)


def verify(force: bool = False) -> bool:
    """소유권 유지 확인 (epoch fence). 잃었으면 False 로 강등한다.

    다른 노드가 (강제 dual-primary 등으로) 같은 store 를 열고 epoch 를 올렸다면 우리
    레코드가 덮여 있으므로 그것을 감지한다 — **먼저 있던 쪽이 물러난다**. 손상 방지가
    목적이며 가용성을 보장하지는 않는다(양쪽 read-only 수렴 가능, §13)."""
    if not _STATE['active']:
        return False
    now = time.time()
    if not force and (now - _LAST_VERIFY[0]) < _VERIFY_TTL:
        return _LAST_VERIFY[1]
    ok = True
    rec = _read_owner(_STATE['path'])
    if not rec:
        ok = False
        reason = 'owner_record_missing'
    elif rec.get('node_id') != _STATE['node_id'] or int(rec.get('epoch') or 0) != _STATE['epoch']:
        ok = False
        reason = (f"epoch_fenced(owner={rec.get('node_id')}#{rec.get('epoch')} "
                  f"self={_STATE['node_id']}#{_STATE['epoch']})")
    if not ok:
        _lose(reason)
    _LAST_VERIFY[0], _LAST_VERIFY[1] = now, ok
    return ok


def assert_writable() -> None:
    """write 진입점 가드 — 소유권이 없으면 LeaseLostError."""
    if not _STATE['active']:
        raise LeaseLostError(f"not_lease_owner: {_STATE['reason']}")
    if not verify():
        raise LeaseLostError(f"not_lease_owner: {_STATE['reason']}")


def release() -> None:
    """정상 종료 시 잠금 해제 (레코드는 남긴다 — 다음 소유자가 epoch 를 이어받도록)."""
    global _LOCK_FH
    if _LOCK_FH is not None:
        try:
            fcntl.flock(_LOCK_FH.fileno(), fcntl.LOCK_UN)
            _LOCK_FH.close()
        except Exception:
            pass
        _LOCK_FH = None
    _STATE.update(active=False, reason='released')
