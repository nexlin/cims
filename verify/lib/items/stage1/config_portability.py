"""S1-CONFIG-PORTABILITY — 배포되는 설정에 **빌드 머신 절대경로**가 없는지.

실측 사고: `ems/core/oam/config/oam.json` 에 개발 머신 경로
(`/home/<user>/work/cims/build/dist/ext_mnt/runtime`)가 커밋된 채 패키지로 나갔다.
부트스트랩 노드는 설치 스크립트가 그 값을 덮어써서 드러나지 않았지만, **콘솔로 설치된
모듈**은 패키지 기본값을 그대로 써서 OAM 이 그 경로에 `makedirs` 하다 `PermissionError`
로 죽었다 → /health 무응답 → role FAULT → 절체 실패 + 콘솔 소멸.

경로는 노드마다 다르므로 패키지에 절대경로를 박으면 안 된다. 빈 값으로 두고 런타임이
노드 로컬로 해석하거나, 설치·배포설정이 명시하게 한다.
"""
from __future__ import annotations

import json
import os
import re

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-CONFIG-PORTABILITY"
_NAME = "배포 설정 이식성 (빌드 머신 절대경로 금지)"

# 패키지에 실려 나가는 설정 파일 (repo_root 상대)
#   소스뿐 아니라 **build/dist** 도 본다: `cims.sh pkg` 는 보통 auto-sync 로 소스를 dist 에
#   반영하지만 `--no-sync` 면 옛 dist 가 그대로 tar 된다. 그때는 소스만 검사하면 통과하면서
#   나쁜 패키지가 나간다(실측 사고의 재발 경로).
_TARGETS = [
    "ems/core/oam/config",
    "ems/core/oam",            # pkg.json
    "csc/bin/csc_pihttp/config",
    "csp", "cmp", "cspsim", "agent",
    # 빌드 산출물 (있을 때만)
    "build/dist/oam/config",
    "build/dist/oam-svc/config",
    "build/dist/agent",
    "build/dist/csp", "build/dist/cmp",
]

# 개발/빌드 머신을 가리키는 패턴 — 노드에는 존재하지 않는 경로
_BAD = re.compile(r"/home/[^/\"]+/(work|cims|dev|src|projects?)/|/Users/[^/\"]+/")

# 값이 아니라 설명인 키는 제외 (문서용 주석)
_SKIP_KEYS = ("_comment", "_runtime_comment", "description", "help", "desc", "label")


def _walk(node, path=""):
    """(경로, 문자열값) 산출 — 주석성 키는 건너뛴다."""
    if isinstance(node, dict):
        for k, v in node.items():
            if any(k == s or k.endswith(s) for s in _SKIP_KEYS):
                continue
            yield from _walk(v, f"{path}.{k}" if path else str(k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _walk(v, f"{path}[{i}]")
    elif isinstance(node, str):
        yield path, node


@verify_item(
    id=_ID, stage=1, category="정적", name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=30,
    execution_order=12,
)
def config_portability(ctx: VerifyContext) -> ItemResult:
    hits: list = []
    scanned = 0
    for rel in _TARGETS:
        base = os.path.join(ctx.repo_root, rel)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not name.endswith(".json"):
                continue
            fp = os.path.join(base, name)
            try:
                with open(fp) as f:
                    doc = json.load(f)
            except Exception:
                continue                      # 파싱 불가는 다른 항목 소관
            scanned += 1
            for key, val in _walk(doc):
                if _BAD.search(val):
                    hits.append(f"{os.path.relpath(fp, ctx.repo_root)}: {key} = {val}")

    if hits:
        ctx.w(f"### {_ID} — FAIL")
        ctx.w("배포 설정에 빌드 머신 절대경로가 있습니다 — 다른 노드에서는 존재하지 않아")
        ctx.w("모듈이 기동에 실패합니다. 빈 값으로 두고 런타임/설치가 해석하게 하세요.")
        for h in hits:
            ctx.w(f"- {h}")
        ctx.w()
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.FAIL, stage=1,
                          detail=f"{len(hits)}건: " + "; ".join(hits[:3]))

    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.PASS, stage=1,
                      detail=f"설정 {scanned}개 — 빌드 머신 경로 없음")
