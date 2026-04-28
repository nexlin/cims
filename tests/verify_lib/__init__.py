"""CIMS 검증 라이브러리.

cims.sh 의 검증 로직을 분리·체계화한 Python 모듈. 항목 단위 등록·실행·메타 노출.

주요 인터페이스:
- registry.verify_item: 검증 항목 등록 데코레이터
- registry.get_items: 등록 항목 조회 (phase / preset 필터)
- runner.run_items: 선택 항목 실행
- context.VerifyContext: 항목 함수에 전달되는 실행 컨텍스트

CLI 진입점은 tests/cims_verify.py.
"""

from .registry import verify_item, get_items, get_item, ItemMeta, ItemResult, ItemStatus  # noqa: F401
from .context import VerifyContext                                                        # noqa: F401
