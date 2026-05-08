"""csc-tb.json 토글 helper — `Agent.MtlsEnabled` per-agent mTLS 정책 스위치.

verify 단계에서 csc 설정을 일부 변경할 때 사용. 대표 케이스: `--enable-mtls`
옵션 시 mTLS 모드로 활성화 → 신규 enroll agent 가 mTLS cert 발급받음.

csc-tb.json 은 두 위치에 존재:
  1. **TB-CSC** (4419 LISTEN): `<dist>/csc/config/csc-tb.json`
     `cims.sh start csc` 로 기동되는 dev/검증환경 컨트롤 csc.
     `S6-SCN-CERT-ROTATE._read_mtls_enabled` 가 보는 정식 위치.
  2. **배포본 mgmt-server** (4445 LISTEN):
     `<dist>/mgmt-server/csc/csc/config/csc-tb.json`
     S5 가 install 한 운영 시뮬레이션용 csc.

per-agent 모델이라 두 위치 모두 토글하는 게 안전 (어떤 csc 가 enroll
하더라도 일관). 활성화 후 csc 재시작이 필요할 수 있음 — csc 가 시작 시점에
설정을 캐시하는 경우. (cims.sh restart csc / restart tb-csc).
"""
from __future__ import annotations

import json
import os
from typing import Optional


_CSC_TB_PATHS = (
    # TB-CSC — scn_cert_rotate._read_mtls_enabled 가 보는 정식 위치
    ("csc", "config", "csc-tb.json"),
    # 배포본 mgmt-server (S5 install)
    ("mgmt-server", "csc", "csc", "config", "csc-tb.json"),
)


def _existing_paths(dist_dir: str) -> list:
    return [os.path.join(dist_dir, *rel) for rel in _CSC_TB_PATHS
            if os.path.isfile(os.path.join(dist_dir, *rel))]


def set_mtls_enabled(dist_dir: str, enabled: bool) -> bool:
    """csc-tb.json 의 `Agent.MtlsEnabled` 토글 — TB-CSC + 배포본 csc 모두.

    return: 1개 이상 토글 성공 시 True. 양 path 모두 없거나 IO 실패 시 False.
    """
    paths = _existing_paths(dist_dir)
    if not paths:
        return False
    any_ok = False
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError):
            continue
        agent = data.get("Agent")
        if not isinstance(agent, dict):
            agent = {}
            data["Agent"] = agent
        agent["MtlsEnabled"] = bool(enabled)
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
                f.write("\n")
            any_ok = True
        except OSError:
            pass
    return any_ok


def get_mtls_enabled(dist_dir: str) -> Optional[bool]:
    """현재 `Agent.MtlsEnabled` 값 — TB-CSC 우선. 양쪽 다 없으면 None.

    값이 다르면 TB-CSC 의 값을 신뢰 (scn_cert_rotate 가 보는 정식 위치).
    """
    paths = _existing_paths(dist_dir)
    if not paths:
        return None
    try:
        with open(paths[0], "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return None
    return bool((data.get("Agent") or {}).get("MtlsEnabled", False))
