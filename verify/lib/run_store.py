"""검증 회차 이력 — 파일 기반 store.

옛 verification_run + verification_run_item DB 테이블을 대체. 회차 1개 = JSON
파일 1개 (`verify_runs/YYYY/MM/<id>.json`). 통계/이력 모두 파일 시스템 스캔으로
처리해 DB 의존을 제거.

** 파일 레이아웃 **
  verify_runs/
    YYYY/MM/
      <id>.json          # 회차 1개. id = int (밀리초 timestamp)

** id 규약 **
  - 정수 (millisecond Unix timestamp). 충돌 시 +1 증가.
  - JSON 의 "id" 필드와 파일명 basename(no ext) 일치.
  - frontend 는 number 로 다룸 — backward compat 유지.

** record 스키마 ** (DB row 와 동등)
  {
    "id": int, "started_at": iso, "finished_at": iso, "elapsed_ms": int,
    "trigger": "user|cli|ci",
    "scope": str, "selected_ids": [str], "verdict": "PASS|FAIL|UNKNOWN",
    "totals": {total, pass, fail, skip, blocked},
    "pkg_manifest_hash": str, "git_branch": str, "git_sha": str, "host": str,
    "report_path": str, "job_id": str, "ens_ip": str, "note": str,
    "items": [
      {"id": str, "stage": int, "parent_id": str|null, "is_group": bool,
       "name": str, "status": str, "elapsed_ms": int, "detail": str, "idx": int}
    ]
  }
"""
from __future__ import annotations

import json
import os
import re
import time
from datetime import datetime, timedelta, timezone
from typing import Iterable, Optional


_RUN_DIR_NAME = "verify_runs"
_ID_RE = re.compile(r"^(\d+)\.json$")


# ─────────────────────────────────────────────────────────────
# 경로 헬퍼
# ─────────────────────────────────────────────────────────────
def runs_root(repo_root: str) -> str:
    return os.path.join(repo_root, _RUN_DIR_NAME)


def _path_for_id(repo_root: str, run_id: int) -> str:
    """id 의 표준 경로. id timestamp 의 연/월 디렉토리 사용."""
    dt = datetime.fromtimestamp(run_id / 1000.0)
    return os.path.join(
        runs_root(repo_root), f"{dt.year:04d}", f"{dt.month:02d}",
        f"{run_id}.json",
    )


def _next_id(repo_root: str) -> int:
    """현재 ms timestamp. 동일 ms 충돌 시 +1 (스레드/멀티잡 방어)."""
    cand = int(time.time() * 1000)
    while os.path.exists(_path_for_id(repo_root, cand)):
        cand += 1
    return cand


# ─────────────────────────────────────────────────────────────
# 쓰기
# ─────────────────────────────────────────────────────────────
def write_run(repo_root: str, record: dict) -> int:
    """record 를 새 id 로 저장. 반환값은 할당된 id. record 안 "id" 도 갱신."""
    rid = _next_id(repo_root)
    record["id"] = rid
    path = _path_for_id(repo_root, rid)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(record, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)
    return rid


def delete_run(repo_root: str, run_id: int) -> bool:
    """파일 삭제. 존재하지 않으면 False. 빈 디렉토리 정리는 안 함."""
    path = _path_for_id(repo_root, run_id)
    if not os.path.isfile(path):
        # legacy: 다른 위치 가능성 (수동 이동 등) — 전체 스캔으로 한 번 더 시도
        for p in _iter_run_files(repo_root):
            base = os.path.basename(p)
            if base == f"{run_id}.json":
                os.remove(p)
                return True
        return False
    os.remove(path)
    return True


# ─────────────────────────────────────────────────────────────
# 읽기
# ─────────────────────────────────────────────────────────────
def _iter_run_files(repo_root: str) -> Iterable[str]:
    """모든 회차 파일 경로 (정렬 X). 빠른 스캔용."""
    root = runs_root(repo_root)
    if not os.path.isdir(root):
        return
    for ye in sorted(os.listdir(root), reverse=True):
        ye_dir = os.path.join(root, ye)
        if not os.path.isdir(ye_dir):
            continue
        for mo in sorted(os.listdir(ye_dir), reverse=True):
            mo_dir = os.path.join(ye_dir, mo)
            if not os.path.isdir(mo_dir):
                continue
            for f in os.listdir(mo_dir):
                if _ID_RE.match(f):
                    yield os.path.join(mo_dir, f)


def _load_one(path: str) -> Optional[dict]:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def get_run(repo_root: str, run_id: int) -> Optional[dict]:
    """단일 회차. 표준 경로 우선, 없으면 전체 스캔 fallback."""
    p = _path_for_id(repo_root, run_id)
    if os.path.isfile(p):
        return _load_one(p)
    for x in _iter_run_files(repo_root):
        if os.path.basename(x) == f"{run_id}.json":
            return _load_one(x)
    return None


def list_runs(
    repo_root: str,
    *,
    stage: Optional[int] = None,
    scope: Optional[str] = None,
    verdict: Optional[str] = None,
    limit: int = 50,
    offset: int = 0,
    since_ts: Optional[float] = None,
) -> tuple:
    """필터링 + 페이징된 list. 정렬: id DESC (최신 우선).

    반환: (total_after_filter, sliced_records).
    각 record 는 file 의 메타 부분만 (items 제외) — list 페이지용 경량 응답.
    """
    matched: list = []
    want_scope = None
    if stage is not None:
        want_scope = f"stage{int(stage)}"
    if scope:
        want_scope = scope[:64]

    for path in _iter_run_files(repo_root):
        # 파일명에서 id 즉시 필터 (since_ts cheap path)
        m = _ID_RE.match(os.path.basename(path))
        if not m:
            continue
        rid = int(m.group(1))
        if since_ts is not None and (rid / 1000.0) < since_ts:
            continue
        rec = _load_one(path)
        if rec is None:
            continue
        if want_scope and rec.get("scope") != want_scope:
            continue
        if verdict and rec.get("verdict") != verdict:
            continue
        # items 는 list 응답에서 제외 — get_run 시에만 포함
        rec_lite = {k: v for k, v in rec.items() if k != "items"}
        matched.append(rec_lite)

    matched.sort(key=lambda r: r.get("id", 0), reverse=True)
    total = len(matched)
    sliced = matched[offset:offset + limit] if limit else matched[offset:]
    return (total, sliced)


# ─────────────────────────────────────────────────────────────
# 통계
# ─────────────────────────────────────────────────────────────
def _pct(n: int, d: int) -> float:
    return round(100.0 * n / d, 1) if d else 0.0


def _quantile(vals: list, q: float) -> int:
    if not vals:
        return 0
    s = sorted(vals)
    idx = max(0, min(len(s) - 1, int(round((len(s) - 1) * q))))
    return int(s[idx])


def stats(repo_root: str, *, days: int = 30, limit: int = 200) -> dict:
    """최근 N 일 회차 요약 통계 — overall + by_scope + timeline."""
    days = max(1, min(int(days), 365))
    limit = max(10, min(int(limit), 1000))
    since_ts = time.time() - days * 86400.0

    rows: list = []
    for path in _iter_run_files(repo_root):
        m = _ID_RE.match(os.path.basename(path))
        if not m:
            continue
        rid = int(m.group(1))
        if (rid / 1000.0) < since_ts:
            continue
        rec = _load_one(path)
        if rec is None:
            continue
        rows.append(rec)

    rows.sort(key=lambda r: r.get("id", 0), reverse=True)
    rows = rows[:limit]

    elapsed_list = [int(r.get("elapsed_ms") or 0) for r in rows]
    n_pass = sum(1 for r in rows if r.get("verdict") == "PASS")
    n_fail = sum(1 for r in rows if r.get("verdict") == "FAIL")
    n_unknown = sum(1 for r in rows if r.get("verdict") not in ("PASS", "FAIL"))

    overall = {
        "runs":              len(rows),
        "pass":              n_pass,
        "fail":              n_fail,
        "unknown":           n_unknown,
        "success_rate":      _pct(n_pass, n_pass + n_fail),
        "avg_elapsed_ms":    int(sum(elapsed_list) / len(rows)) if rows else 0,
        "median_elapsed_ms": _quantile(elapsed_list, 0.5),
        "p95_elapsed_ms":    _quantile(elapsed_list, 0.95),
    }

    by_scope_map: dict = {}
    for r in rows:
        sc = r.get("scope") or "unknown"
        d = by_scope_map.setdefault(sc, {
            "scope": sc, "runs": 0, "pass": 0, "fail": 0, "_elapsed": [],
        })
        d["runs"] += 1
        v = r.get("verdict")
        if v == "PASS":   d["pass"] += 1
        elif v == "FAIL": d["fail"] += 1
        d["_elapsed"].append(int(r.get("elapsed_ms") or 0))
    by_scope = []
    for sc, d in sorted(by_scope_map.items()):
        e = d.pop("_elapsed")
        d["success_rate"]   = _pct(d["pass"], d["pass"] + d["fail"])
        d["avg_elapsed_ms"] = int(sum(e) / len(e)) if e else 0
        by_scope.append(d)

    # 시계열: 시간 순 (오래된 → 최신) 으로 정렬
    timeline = []
    for r in reversed(rows):
        totals = r.get("totals") or {}
        timeline.append({
            "id":         r.get("id"),
            "started_at": r.get("started_at"),
            "scope":      r.get("scope") or "",
            "verdict":    r.get("verdict") or "UNKNOWN",
            "elapsed_ms": int(r.get("elapsed_ms") or 0),
            "pass":       int(totals.get("pass", 0) or 0),
            "fail":       int(totals.get("fail", 0) or 0),
            "skip":       int(totals.get("skip", 0) or 0),
            "blocked":    int(totals.get("blocked", 0) or 0),
            "total":      int(totals.get("total", 0) or 0),
        })

    since_iso = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()
    return {
        "window":  {"days": days, "since_iso": since_iso, "limit": limit},
        "overall": overall,
        "by_scope": by_scope,
        "timeline": timeline,
    }
