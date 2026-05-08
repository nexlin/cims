"""검증 회차 LIVE store — 실행 중·최근 종료 회차의 stdout 스트림 + 메타 보관.

run_store 와 분리: run_store 는 종료 회차의 최종 record 를 보관 (영구).
live_store 는 실행 중 회차의 진행 상태 + stdout 을 파일에 기록해 어디서든
부착해서 볼 수 있게 한다 (UI 페이지 전환·CLI 직접 실행 모두 가시화).

** 파일 레이아웃 **
  verify_runs/live/<live_id>/
    meta.json    # 진행 상태 스냅샷 (start_live → update_live → finalize_live)
    stdout.log   # 회차 stdout (verify 마커 포함). 진행 중에 append.

** live_id 규약 **
  - uuid hex 12자 (job_id 와 동일 스킴). 영구 run_store 의 정수 id 와 분리.
  - 종료 시 run_store.write_run() 으로 정수 run_id 가 별도 부여되고
    finalize_live(meta, run_id=...) 로 backref 만 유지.

** TTL **
  - done=true 회차는 ended_at 으로부터 _DONE_TTL_S 후 list_active 호출 시 GC.
  - done=false 회차는 pid 가 alive 가 아니면 verdict='ABORTED' 로 강제 종료.
"""
from __future__ import annotations

import json
import os
import time
import uuid
from typing import Optional


_LIVE_SUBDIR = "live"
# done 상태로 전이된 후 list_active 결과에서 사라지기까지 보존 시간 (초).
# 이 동안에는 콘솔에서 "최근 회차" 로 조회 가능.
_DONE_TTL_S = 3600          # 1h
_MAX_ACTIVE_RETURN = 30     # /active 응답 상한


# ─────────────────────────────────────────────────────────────
# 경로 헬퍼
# ─────────────────────────────────────────────────────────────
def live_root(repo_root: str) -> str:
    return os.path.join(repo_root, "verify_runs", _LIVE_SUBDIR)


def _live_dir(repo_root: str, live_id: str) -> str:
    return os.path.join(live_root(repo_root), live_id)


def meta_path(live_dir: str) -> str:
    return os.path.join(live_dir, "meta.json")


def stdout_path(live_dir: str) -> str:
    return os.path.join(live_dir, "stdout.log")


# ─────────────────────────────────────────────────────────────
# 시작 / 업데이트 / 종료
# ─────────────────────────────────────────────────────────────
def new_live_id() -> str:
    return uuid.uuid4().hex[:12]


def start_live(
    repo_root: str,
    *,
    live_id: Optional[str] = None,
    source: str,                # 'cli' | 'backend'
    scope: str,
    selected_ids: Optional[list] = None,
    argv: Optional[list] = None,
    label: str = "",
    trigger_type: str = "user",
    pid: Optional[int] = None,
    host: str = "",
) -> tuple:
    """live 디렉터리 생성 + meta.json 초기 기록. 반환=(live_id, dir, stdout_path)."""
    lid = live_id or new_live_id()
    d = _live_dir(repo_root, lid)
    os.makedirs(d, exist_ok=True)
    meta = {
        "id":            lid,
        "source":        source,
        "scope":         scope,
        "selected_ids":  list(selected_ids or []),
        "argv":          list(argv or []),
        "label":         label,
        "trigger_type":  trigger_type,
        "pid":           int(pid) if pid is not None else None,
        "host":          host,
        "started_at":    time.time(),
        "ended_at":      None,
        "done":          False,
        "verdict":       None,
        "returncode":    None,
        "run_id":        None,           # finalize 후 run_store id 백레퍼런스
        "stdout_path":   stdout_path(d),
    }
    _atomic_write(meta_path(d), meta)
    # stdout.log 빈 파일 미리 생성 — tail/read 가 NotFound 회피
    with open(stdout_path(d), "a", encoding="utf-8"):
        pass
    return lid, d, stdout_path(d)


def update_live(live_dir: str, **fields) -> None:
    """meta.json 의 일부 필드를 atomic merge 갱신."""
    p = meta_path(live_dir)
    try:
        with open(p, "r", encoding="utf-8") as f:
            meta = json.load(f)
    except Exception:
        return
    meta.update(fields)
    _atomic_write(p, meta)


def finalize_live(
    live_dir: str,
    *,
    verdict: Optional[str],
    returncode: Optional[int],
    run_id: Optional[int] = None,
) -> None:
    update_live(
        live_dir,
        done=True,
        ended_at=time.time(),
        verdict=verdict,
        returncode=returncode,
        run_id=run_id,
    )


# ─────────────────────────────────────────────────────────────
# 조회 / GC
# ─────────────────────────────────────────────────────────────
def list_active(repo_root: str, *,
                max_count: int = _MAX_ACTIVE_RETURN,
                done_ttl_s: int = _DONE_TTL_S) -> list:
    """live/<id>/meta.json 스캔. 비정상 종료 감지 + TTL 지난 done 항목 GC.

    반환=meta dict 리스트 (started_at desc, max_count 제한).
    """
    root = live_root(repo_root)
    if not os.path.isdir(root):
        return []
    out: list = []
    now = time.time()
    for name in os.listdir(root):
        d = os.path.join(root, name)
        if not os.path.isdir(d):
            continue
        meta = _read_meta_safe(d)
        if not meta:
            continue
        # 비정상 종료 감지: done=false + pid 죽음 → verdict=ABORTED 로 강제 마감
        if not meta.get("done"):
            pid = meta.get("pid")
            if pid and not _pid_alive(int(pid)):
                meta["done"]       = True
                meta["ended_at"]   = meta.get("ended_at") or now
                meta["verdict"]    = "ABORTED"
                meta["returncode"] = -1
                _atomic_write(meta_path(d), meta)
        # done + TTL 초과 → GC (디렉터리째 제거)
        if meta.get("done") and meta.get("ended_at") \
                and (now - float(meta["ended_at"])) > done_ttl_s:
            _rmtree_safe(d)
            continue
        out.append(meta)
    out.sort(key=lambda m: float(m.get("started_at") or 0), reverse=True)
    return out[:max_count]


def read_live(repo_root: str, live_id: str,
              *, tail_lines: int = 200) -> Optional[dict]:
    """단건 live 메타 + stdout tail. 없으면 None."""
    d = _live_dir(repo_root, live_id)
    meta = _read_meta_safe(d)
    if not meta:
        return None
    sp = meta.get("stdout_path") or stdout_path(d)
    tail = ""
    try:
        with open(sp, "rb") as f:
            data = f.read().decode("utf-8", errors="replace")
            tail = "\n".join(data.splitlines()[-tail_lines:])
    except Exception:
        pass
    meta["stdout_tail"] = tail
    return meta


def remove_live(repo_root: str, live_id: str) -> bool:
    d = _live_dir(repo_root, live_id)
    if not os.path.isdir(d):
        return False
    _rmtree_safe(d)
    return True


# ─────────────────────────────────────────────────────────────
# 내부
# ─────────────────────────────────────────────────────────────
def _atomic_write(path: str, obj: dict) -> None:
    tmp = path + ".tmp"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def _read_meta_safe(live_dir: str) -> Optional[dict]:
    p = meta_path(live_dir)
    if not os.path.isfile(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # 권한이 없어도 존재는 함
        return True
    except Exception:
        return False


def _rmtree_safe(path: str) -> None:
    import shutil
    try:
        shutil.rmtree(path, ignore_errors=True)
    except Exception:
        pass
