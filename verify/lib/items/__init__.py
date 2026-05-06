"""auto-import — items/ 하위의 모든 stage/카테고리/항목 모듈을 재귀 import.

각 디렉토리에 `__init__.py` 가 있으면 패키지로 인식되며 자동 스캔 대상.
파일 1개 = 검증 항목 1개 원칙 (또는 카테고리별 helper 묶음).
새 항목 추가 시 적합한 stage{N}/ 아래 파일 하나 생성하면 끝 — 명시적 import 불필요.

import 가 끝나면 registry.validate_registry() 로 group/parent 무결성 검증.
"""
import importlib
import pkgutil
from pathlib import Path

from ..registry import validate_registry as _validate_registry


def _autoimport_recursive(pkg_name: str, pkg_path: str) -> None:
    """{pkg_name} 아래 모든 .py 모듈을 재귀적으로 import.
    `__` 로 시작하는 파일은 제외 (`__init__.py`).
    `_` 로 시작하는 파일은 helper 로 import (예: `_helpers.py`, `_legacy.py`).
    """
    for _finder, name, ispkg in pkgutil.iter_modules([pkg_path]):
        if name.startswith("__"):
            continue
        full = f"{pkg_name}.{name}"
        if ispkg:
            sub = importlib.import_module(full)
            _autoimport_recursive(full, str(Path(sub.__file__).parent))
        else:
            importlib.import_module(full)


_autoimport_recursive(__name__, str(Path(__file__).parent))

# 무결성 검증 — issue 발견 시 ImportError 로 즉시 차단 (조용한 실패 방지)
_issues = _validate_registry()
if _issues:
    raise ImportError(
        "verify.lib registry validation failed:\n  - "
        + "\n  - ".join(_issues)
    )
