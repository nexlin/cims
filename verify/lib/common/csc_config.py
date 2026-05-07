"""배포본 csc-tb.json 토글 helper.

verify 단계에서 csc 설정을 일부 변경할 때 사용. 대표 케이스: `--enable-mtls`
옵션 시 `Agent.MtlsEnabled=true` 로 토글 → 신규 enroll agent 가 mTLS cert
발급받음.

배포본 csc-tb.json 위치: `<dist>/csc-server/csc/csc/config/csc-tb.json`.
target=verify 와 target=prod 모두 동일 path.

활성화는 idempotent — csc 가 이미 LISTEN 중이어도 다음 enroll 시점에 새 값
적용. 기존 X-Agent-Token agent 는 영향 없음 (per-agent 모델, 메모리에 보존).
"""
from __future__ import annotations

import json
import os
from typing import Optional


def _csc_tb_json_path(dist_dir: str) -> str:
    return os.path.join(dist_dir, "csc-server", "csc", "csc", "config",
                        "csc-tb.json")


def set_mtls_enabled(dist_dir: str, enabled: bool) -> bool:
    """배포본 csc-tb.json 의 `Agent.MtlsEnabled` 를 토글.

    return: 변경 성공 시 True. 파일 없거나 JSON parse/IO 실패 시 False.
    """
    path = _csc_tb_json_path(dist_dir)
    if not os.path.isfile(path):
        return False
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return False
    agent = data.get("Agent")
    if not isinstance(agent, dict):
        agent = {}
        data["Agent"] = agent
    agent["MtlsEnabled"] = bool(enabled)
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
        return True
    except OSError:
        return False


def get_mtls_enabled(dist_dir: str) -> Optional[bool]:
    """현재 `Agent.MtlsEnabled` 값. 파일 없거나 parse 실패 시 None."""
    path = _csc_tb_json_path(dist_dir)
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return None
    return bool((data.get("Agent") or {}).get("MtlsEnabled", False))
