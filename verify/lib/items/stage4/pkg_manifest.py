"""S4-PKG-MANIFEST — 패키지 hash + timestamp 기록 (S6 매칭 보증).

build/dist/packages/manifest.json 에 5개 tarball 의 SHA256 + 크기 + 타임스탬프 기록.
S6 통합 검증은 이 manifest 의 hash 와 실행 중인 모듈의 패키지 hash 를 매칭.
빌드를 새로 했으면 S4 부터 다시 — immutability gate.
"""
from __future__ import annotations

import hashlib
import json
import os
from datetime import datetime, timezone
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


@verify_item(
    id="S4-PKG-MANIFEST",
    stage=4, category="패키지",
    name="패키지 manifest 기록 (sha256 + ts → packages/manifest.json)",
    depends_on=["S4-PKG-BUILD"],
    presets=["stage4-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write"], timeout_s=30,
)
def pkg_manifest(ctx: VerifyContext) -> ItemResult:
    pkg_dir = os.path.join(ctx.dist_dir, "packages")
    tarballs = sorted(glob(os.path.join(pkg_dir, "*.tar.gz")))
    if not tarballs:
        return ItemResult(
            id="S4-PKG-MANIFEST", name="패키지 manifest",
            status=ItemStatus.FAIL,
            detail=f"{pkg_dir}/*.tar.gz 없음 — S4-PKG-BUILD 선행 필요", stage=4,
        )

    entries = []
    for t in tarballs:
        entries.append({
            "name":   os.path.basename(t),
            "size":   os.path.getsize(t),
            "sha256": _sha256_file(t),
            "mtime":  datetime.fromtimestamp(
                os.path.getmtime(t), tz=timezone.utc
            ).isoformat(),
        })

    manifest = {
        "ts":  datetime.now(timezone.utc).astimezone().isoformat(),
        "git": {"branch": ctx.git_branch, "sha": ctx.git_sha},
        "host": _hostname(),
        "ens_ip": ctx.ens_ip or "",
        "packages": entries,
    }
    out_path = os.path.join(pkg_dir, "manifest.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    # ctx.state 에 hash 캐시 (S6 가 매칭 시 사용 가능)
    ctx.state["pkg_manifest"] = manifest

    ctx.w("## S4-PKG-MANIFEST — manifest 기록")
    ctx.w(f"- 산출: `{out_path}`")
    ctx.w(f"- ts: {manifest['ts']}")
    ctx.w(f"- git: {ctx.git_branch} @ {ctx.git_sha}")
    ctx.w("```")
    for e in entries:
        ctx.w(f"  {e['name']:32}  {e['sha256'][:12]}…  {e['size']:>10,} bytes")
    ctx.w("```")
    ctx.w()

    detail = f"{len(entries)} packages, manifest={os.path.basename(out_path)}"
    return ItemResult(
        id="S4-PKG-MANIFEST", name="패키지 manifest",
        status=ItemStatus.PASS, detail=detail, stage=4,
    )


def _hostname() -> str:
    import socket
    try: return socket.gethostname()
    except Exception: return "?"
