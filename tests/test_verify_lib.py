"""verify_lib 단위 테스트 — registry / runner / presets / context.

외부 의존성 없이 동작 (cims.sh 호출 안 함). standalone 으로 실행:
  python3 -m tests.test_verify_lib
또는 pytest:
  pytest tests/test_verify_lib.py
"""
from __future__ import annotations

import os
import sys
import unittest
from typing import Any

# tests/ 를 sys.path 에 추가 (단독 실행 대비)
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)


class TestRegistry(unittest.TestCase):
    """@verify_item 데코레이터 + get_items 필터 동작."""

    def setUp(self) -> None:
        from verify_lib import registry
        self.registry = registry
        # 글로벌 registry 보존을 위해 snapshot
        self._snapshot = dict(registry._REGISTRY)

    def tearDown(self) -> None:
        # 테스트로 추가된 항목 제거
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]

    def test_register_and_lookup(self) -> None:
        @self.registry.verify_item(
            id="TEST-001", phase=99, category="유닛",
            name="test register", presets=["test-preset"],
        )
        def _fn(ctx: Any) -> bool:
            return True
        rec = self.registry.get_item("TEST-001")
        self.assertIsNotNone(rec)
        meta, fn = rec
        self.assertEqual(meta.id, "TEST-001")
        self.assertEqual(meta.phase, 99)
        self.assertEqual(meta.category, "유닛")
        self.assertEqual(meta.presets, ["test-preset"])

    def test_duplicate_id_rejected(self) -> None:
        @self.registry.verify_item(
            id="TEST-DUP", phase=99, category="유닛", name="first",
        )
        def _fn1(ctx: Any) -> bool: return True
        with self.assertRaises(ValueError):
            @self.registry.verify_item(
                id="TEST-DUP", phase=99, category="유닛", name="second",
            )
            def _fn2(ctx: Any) -> bool: return True

    def test_filter_by_phase(self) -> None:
        @self.registry.verify_item(
            id="TEST-P99-A", phase=99, category="유닛", name="A",
        )
        def _a(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-P99-B", phase=99, category="유닛", name="B",
        )
        def _b(ctx: Any) -> bool: return True
        items = self.registry.get_items(phase=99)
        ids = [m.id for m in items]
        self.assertIn("TEST-P99-A", ids)
        self.assertIn("TEST-P99-B", ids)

    def test_filter_by_preset(self) -> None:
        @self.registry.verify_item(
            id="TEST-PRESET-A", phase=99, category="유닛", name="A",
            presets=["my-preset"],
        )
        def _a(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-PRESET-B", phase=99, category="유닛", name="B",
            presets=["other-preset"],
        )
        def _b(ctx: Any) -> bool: return True
        items = self.registry.get_items(preset="my-preset")
        ids = [m.id for m in items]
        self.assertIn("TEST-PRESET-A", ids)
        self.assertNotIn("TEST-PRESET-B", ids)


class TestRunner(unittest.TestCase):
    """runner.run_items — 의존성/SKIP/ItemResult 변환."""

    def setUp(self) -> None:
        from verify_lib import registry, runner
        from verify_lib.context import VerifyContext
        self.registry = registry
        self.runner = runner
        self.VerifyContext = VerifyContext
        self._snapshot = dict(registry._REGISTRY)

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]

    def _ctx(self) -> Any:
        # 임시 디렉토리 ts 만 사용 — report 작성은 안 함
        ctx = self.VerifyContext(
            repo_root="/tmp", dist_dir="/tmp",
            report_path="/tmp/_test_report.md", phase=99, ts="20990101_000000",
        )
        return ctx

    def test_run_simple_pass(self) -> None:
        @self.registry.verify_item(
            id="TEST-SIMPLE", phase=99, category="유닛", name="simple",
        )
        def _fn(ctx: Any) -> bool:
            return True
        results = self.runner.run_items(self._ctx(), ["TEST-SIMPLE"])
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "PASS")
        self.assertEqual(results[0].id, "TEST-SIMPLE")

    def test_run_simple_fail(self) -> None:
        @self.registry.verify_item(
            id="TEST-FAIL", phase=99, category="유닛", name="fail",
        )
        def _fn(ctx: Any) -> bool: return False
        results = self.runner.run_items(self._ctx(), ["TEST-FAIL"])
        self.assertEqual(results[0].status, "FAIL")

    def test_dependency_skip_when_dep_fails(self) -> None:
        @self.registry.verify_item(
            id="TEST-DEP-A", phase=99, category="유닛", name="A (fails)",
        )
        def _a(ctx: Any) -> bool: return False
        @self.registry.verify_item(
            id="TEST-DEP-B", phase=99, category="유닛", name="B (depends)",
            depends_on=["TEST-DEP-A"],
        )
        def _b(ctx: Any) -> bool: return True
        results = self.runner.run_items(self._ctx(), ["TEST-DEP-A", "TEST-DEP-B"])
        statuses = {r.id: r.status for r in results}
        self.assertEqual(statuses["TEST-DEP-A"], "FAIL")
        self.assertEqual(statuses["TEST-DEP-B"], "SKIP")

    def test_dependency_topo_sort(self) -> None:
        order: list = []
        @self.registry.verify_item(
            id="TEST-T-A", phase=99, category="유닛", name="A",
        )
        def _a(ctx: Any) -> bool:
            order.append("A"); return True
        @self.registry.verify_item(
            id="TEST-T-B", phase=99, category="유닛", name="B",
            depends_on=["TEST-T-A"],
        )
        def _b(ctx: Any) -> bool:
            order.append("B"); return True
        # 입력 순서를 의도적으로 뒤집어도 위상 정렬로 A → B 보장
        self.runner.run_items(self._ctx(), ["TEST-T-B", "TEST-T-A"])
        self.assertEqual(order, ["A", "B"])

    def test_exception_becomes_fail(self) -> None:
        @self.registry.verify_item(
            id="TEST-EXC", phase=99, category="유닛", name="exception",
        )
        def _fn(ctx: Any) -> Any:
            raise RuntimeError("boom")
        results = self.runner.run_items(self._ctx(), ["TEST-EXC"])
        self.assertEqual(results[0].status, "FAIL")
        self.assertIn("RuntimeError", results[0].detail)
        self.assertIn("boom", results[0].detail)

    def test_unknown_item_id(self) -> None:
        with self.assertRaises(ValueError):
            self.runner.run_items(self._ctx(), ["NOT-REGISTERED-XYZ"])


class TestPresets(unittest.TestCase):
    """presets — list / resolve / dynamic."""

    def setUp(self) -> None:
        from verify_lib import presets, registry
        self.presets = presets
        self.registry = registry
        self._snapshot = dict(registry._REGISTRY)
        # 테스트용 항목 등록 (phase 99)
        @self.registry.verify_item(
            id="TEST-PR-X", phase=99, category="유닛", name="X",
            presets=["test-pr-static"],
        )
        def _x(ctx: Any) -> bool: return True

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]
        self.presets._PRESETS.pop("test-pr-static", None)

    def test_register_and_resolve_static(self) -> None:
        self.presets.register_preset("test-pr-static", ["TEST-PR-X"])
        items = self.presets.resolve_preset("test-pr-static")
        self.assertEqual(items, ["TEST-PR-X"])

    def test_resolve_dynamic(self) -> None:
        # 등록된 phase99-full 같은 동적 프리셋 시뮬레이션
        self.presets.register_preset(
            "test-pr-dyn",
            lambda: [m.id for m in self.registry.get_items(phase=99)],
        )
        items = self.presets.resolve_preset("test-pr-dyn")
        self.assertIn("TEST-PR-X", items)

    def test_resolve_unknown_returns_empty(self) -> None:
        self.assertEqual(self.presets.resolve_preset("nonexistent"), [])


class TestExistingRegistry(unittest.TestCase):
    """기본 verify_lib import 후 등록된 항목들이 정상 메타를 가지는지."""

    def test_phase3_items_registered(self) -> None:
        from verify_lib import registry
        # __init__.py 가 phase3/phase1/phase2/modules 모두 import
        from verify_lib import items                            # noqa: F401
        ids = {m.id for m in registry.get_items(phase=3)}
        self.assertIn("P3-ENTRY-CHECK", ids)
        self.assertIn("P3-SEED", ids)
        self.assertIn("P3-SCN-VOLTE-VOICE", ids)
        self.assertIn("P3-SUMMARY", ids)

    def test_phase1_items_registered(self) -> None:
        from verify_lib import registry
        from verify_lib import items                            # noqa: F401
        ids = {m.id for m in registry.get_items(phase=1)}
        self.assertIn("P1-PREFLIGHT", ids)
        self.assertIn("P1-START", ids)
        self.assertIn("P1-HEALTH", ids)
        self.assertIn("MODULE-CMP", ids)
        self.assertIn("MODULE-CSC", ids)

    def test_phase2_run_all_registered(self) -> None:
        from verify_lib import registry
        from verify_lib import items                            # noqa: F401
        rec = registry.get_item("P2-RUN-ALL")
        self.assertIsNotNone(rec)
        self.assertEqual(rec[0].phase, 2)

    def test_presets_loaded(self) -> None:
        from verify_lib import presets
        names = {p["name"] for p in presets.list_presets()}
        # Step 4 까지 정의된 주요 프리셋 모두 존재
        for required in ("phase1-full", "phase1-main", "phase1-modules",
                         "phase1-quick", "phase2-full", "phase3-full",
                         "phase3-volte", "phase3-ptt"):
            self.assertIn(required, names, f"missing preset: {required}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
