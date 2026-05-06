"""패키지 manifest immutability 헬퍼.

S4-PKG-MANIFEST 가 산출한 `build/dist/packages/manifest.json` 의 sha256 을
deploy 시점에 marker 로 저장 → S6-ENTRY-CHECK 가 매칭 검증.

목적: S5 배포 이후 사용자가 패키지를 다시 빌드/패키지화 (S2/S4 재실행) 했을 때
S6 가 "지금 LISTEN 중인 모듈은 옛 패키지" 라는 mismatch 를 감지해 자동 FAIL.
사용자는 다시 S5 부터 진행해야 한다.

API:
  manifest_path(dist_dir) -> str
  marker_path(dist_dir)   -> str
  read_manifest_sha(dist_dir) -> str | None        # 현재 manifest.json 의 sha256
  read_marker(dist_dir)       -> dict | None       # {"manifest_sha": str, "ts": str}
  write_marker(dist_dir, ts)  -> str               # marker 작성, manifest sha 반환
  immutability_check(dist_dir) -> tuple(ok: bool, current_sha, deployed_sha, detail)
"""
from __future__ import annotations

import hashlib
import json
import os
from datetime import datetime, timezone
from typing import Optional


def manifest_path(dist_dir: str) -> str:
    return os.path.join(dist_dir, "packages", "manifest.json")


def marker_path(dist_dir: str) -> str:
    return os.path.join(dist_dir, ".deployed-manifest.json")


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest_sha(dist_dir: str) -> Optional[str]:
    p = manifest_path(dist_dir)
    if not os.path.isfile(p):
        return None
    try:
        return _sha256_file(p)
    except OSError:
        return None


def read_marker(dist_dir: str) -> Optional[dict]:
    p = marker_path(dist_dir)
    if not os.path.isfile(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def write_marker(dist_dir: str, ts: Optional[str] = None) -> Optional[str]:
    """현재 packages/manifest.json 의 sha256 을 .deployed-manifest.json 에 기록.

    manifest 가 없으면 None 반환 (marker 작성 안 함).
    """
    sha = read_manifest_sha(dist_dir)
    if sha is None:
        return None
    payload = {
        "manifest_sha": sha,
        "ts":           ts or datetime.now(timezone.utc).astimezone().isoformat(),
    }
    p = marker_path(dist_dir)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    return sha


def immutability_check(dist_dir: str) -> tuple:
    """현재 manifest sha 와 deploy marker 의 sha 비교.

    Returns: (ok: bool, current_sha: str|None, deployed_sha: str|None, detail: str)
      ok=True   — 매칭 (또는 marker 도 없고 manifest 도 없는 초기 상태? — False)
      ok=False  — 불일치 또는 marker/manifest 결측
    """
    cur = read_manifest_sha(dist_dir)
    marker = read_marker(dist_dir)
    if cur is None:
        return (False, None, marker.get("manifest_sha") if marker else None,
                "packages/manifest.json 없음 — S4-PKG-MANIFEST 선행 필요")
    if marker is None:
        return (False, cur, None,
                ".deployed-manifest.json 없음 — S5 배포 미수행 (또는 옛 배포 marker 부재)")
    dep = marker.get("manifest_sha", "")
    if cur != dep:
        return (
            False, cur, dep,
            f"manifest 불일치: 현재={cur[:12]}… 배포시={dep[:12]}… → "
            "패키지가 재빌드됐으므로 S5 부터 재배포 필요",
        )
    return (True, cur, dep, f"manifest 일치: {cur[:12]}… (배포 시각: {marker.get('ts','')})")
