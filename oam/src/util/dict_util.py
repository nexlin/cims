from typing import Any


class DictUtil:

    @staticmethod
    def normalize(obj: Any):
        if isinstance(obj, dict):
            return (
                "dict",
                tuple(
                    sorted(
                        ((k, DictUtil.normalize(v)) for k, v in obj.items()),
                        key=lambda x: repr(x[0]),  # 키도 안전하게 문자열 기준 정렬
                    )
                ),
            )

        if isinstance(obj, (list, tuple, set)):
            normalized_items = [DictUtil.normalize(v) for v in obj]
            return (
                "list",
                tuple(
                    sorted(
                        normalized_items,
                        key=lambda x: repr(x),  # 여기서 에러 나던 부분 수정
                    )
                ),
            )

        return obj

    @staticmethod
    def deep_equal_ignore_order(a: Any, b: Any) -> bool:
        return DictUtil.normalize(a) == DictUtil.normalize(b)