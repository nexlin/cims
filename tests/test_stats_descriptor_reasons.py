"""VoLTE 통계 표의 사유 열이 종료 사유를 남김없이 덮는지 — 디스크립터 선언 검사 (오프라인).

  python3 tests/test_stats_descriptor_reasons.py

**왜 이 시험이 있나.** 종료 사유는 `flow_logger` 의 enum 7종이 정본인데, 표의 열은
`service_descriptors_seed/cims.json` 에 손으로 적는다. 둘이 어긋나면 **그 사유로 끝난 호가
표 어디에도 안 나타난다** — 열이 없으니 빈칸조차 안 생겨서 아무 데서도 경고가 나지 않는다.
실제로 `rejected`(거절)와 `no_answer`(무응답) 두 사유가 열 없이 오래 빠져 있었다.

응답코드 툴팁(`statusFrom`/`statusLabels`)도 함께 본다. 렌더러는 열마다 선언된 코드를
**그 칸의 숫자를 예산 삼아** 거둬 가므로(`dataSourceSpec.ts` pickDetail), 같은 코드가 두 열에
선언되면 한 호가 두 번 세어져 `칸의 숫자 = 상세의 합` 불변식이 깨진다.
코드→사유 분류는 CSP `csp/CallDir.h::_ReasonOfStatus` 가 정본이다.
"""
from __future__ import annotations

import json
import os
import re
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

SEED = os.path.join(_REPO, "ems", "core", "oam", "src", "services",
                    "service_descriptors_seed", "cims.json")
FLOW_LOGGER = os.path.join(_REPO, "ems", "core", "oam", "src", "services", "flow_logger.py")

# 사유 열이 아닌 것 — 사유 축이 아니라 집계·비율 축이다.
_NOT_REASON = "normal"          # `정상종료` 열이 이미 센다

# 집계가 파생한 사유 축 — 원천(`end_reason`) 어휘에는 없다. `unknown` 은 "사유도 응답코드도
# 없어 특정할 수 없는 실패" 를 담는 칸이라 CSP 가 적을 수 있는 값이 아니다(sip_statistics.md
# §2.3). enum 대조에서 빼되, **열이 있는지는 따로 지킨다**.
_DERIVED = {'unknown'}

# 응답코드 → 종료 사유. csp/CallDir.h::_ReasonOfStatus 와 같은 규칙이어야 한다.
def _reason_of_status(st: int) -> str:
    if st in (486, 600):
        return "busy"
    if st in (408, 480):
        return "no_answer"
    if st in (488, 606):
        return "error"
    if 500 <= st < 600:
        return "error"
    if st >= 400:
        return "rejected"
    return "error"


def _descriptor(sid: str) -> dict:
    with open(SEED, encoding="utf-8") as f:
        root = json.load(f)

    def walk(o):
        if isinstance(o, dict):
            if o.get("id") == sid:
                yield o
            for v in o.values():
                yield from walk(v)
        elif isinstance(o, list):
            for v in o:
                yield from walk(v)

    got = list(walk(root))
    assert len(got) == 1, f"{sid} 선언이 {len(got)}개다 (1개여야 한다)"
    return got[0]


def _reason_enum() -> list:
    """종료 사유 정본 — flow_logger 의 응답 스키마 enum 을 그대로 읽는다.

    여기서 하드코딩하면 정본이 늘어났을 때 이 시험이 조용히 통과한다.
    """
    with open(FLOW_LOGGER, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"'enum':\s*\[('normal'[^\]]*)\]", src)
    assert m, "flow_logger 에서 end_reason enum 을 못 찾았다 — 정본 위치가 바뀌었나"
    return re.findall(r"'([a-z_]+)'", m.group(1))


class VolteReasonColumns(unittest.TestCase):

    def setUp(self):
        self.cols = _descriptor("cims.svc.volte")["map"]["matrix"]["columns"]
        self.reason_cols = {
            c["path"].split(".")[-1]: c
            for c in self.cols if c.get("path", "").startswith("volte.reasons.")
        }

    def test_모든_종료_사유에_열이_있다(self):
        want = {r for r in _reason_enum() if r != _NOT_REASON}
        self.assertEqual(want, set(self.reason_cols) - _DERIVED,
                         "사유 열과 종료 사유 enum 이 어긋난다 — 빠진 사유로 끝난 호는 표에 안 나온다")

    def test_파생_축에도_열이_있다(self):
        """집계가 만드는 축은 원천 enum 에 없다 — 그래도 표에 자리가 있어야 한다.

        `unknown` 은 CSP 가 적는 값이 아니라 **집계가 만든 칸**이다(사유도 응답코드도 없어
        무엇 때문에 실패했는지 특정할 수 없는 호). 열이 없으면 그 호들이 표 어디에도 안
        나타나고, 시도 수와 사유 합이 어긋난 채로 남는다."""
        self.assertTrue(_DERIVED <= set(self.reason_cols),
                        f"파생 사유 축에 열이 없다: {_DERIVED - set(self.reason_cols)}")

    def test_정상종료_열이_따로_있다(self):
        keys = {c["key"] for c in self.cols}
        self.assertIn("completed", keys, "`정상종료` 열이 없으면 normal 사유를 셀 곳이 없다")

    def test_응답코드가_열끼리_겹치지_않는다(self):
        seen = {}
        for reason, col in self.reason_cols.items():
            for code in (col.get("statusLabels") or {}):
                self.assertNotIn(
                    code, seen,
                    f"응답코드 {code} 가 `{seen.get(code)}` 와 `{reason}` 두 열에 선언됐다 — "
                    "한 호가 두 번 세어져 칸의 숫자와 상세의 합이 어긋난다")
                seen[code] = reason

    def test_응답코드가_제_사유_열에_있다(self):
        for reason, col in self.reason_cols.items():
            for code in (col.get("statusLabels") or {}):
                self.assertEqual(
                    _reason_of_status(int(code)), reason,
                    f"{code} 는 `{_reason_of_status(int(code))}` 인데 `{reason}` 열에 선언됐다 "
                    "(정본: csp/CallDir.h::_ReasonOfStatus)")

    def test_코드를_선언한_열은_미기록_라벨도_갖는다(self):
        for reason, col in self.reason_cols.items():
            if col.get("statusFrom"):
                self.assertTrue(
                    col.get("detailUnknownLabel"),
                    f"`{reason}` 열에 남는 건수를 담을 라벨이 없다 — 칸의 숫자보다 상세의 합이 작아진다")

    def test_툴팁을_붙인_열은_statusFrom_경로가_맞다(self):
        for reason, col in self.reason_cols.items():
            if col.get("statusLabels"):
                self.assertEqual("volte.statuses", col.get("statusFrom"),
                                 f"`{reason}` 열의 응답코드 출처가 volte.statuses 가 아니다")


class ErrorsDescriptorStaysRegistered(unittest.TestCase):
    """`cims.svc.volte.errors` 는 기본 화면에 안 놓지만 소스 등록은 남긴다.

    운영자가 `[✎ 편집]` 으로 직접 붙일 수 있는 소스다 — 지우면 그 선택지가 사라진다.
    """

    def test_선언이_살아_있다(self):
        d = _descriptor("cims.svc.volte.errors")
        self.assertIn("matrix", d.get("shapes", []))
        self.assertTrue(d["map"]["matrix"].get("cellLabels"), "응답코드 라벨이 비었다")


if __name__ == "__main__":
    unittest.main(verbosity=2)
