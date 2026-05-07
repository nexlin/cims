"""verify.lib 단위 테스트 — registry / runner / presets / context.

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
_REPO_ROOT = os.path.dirname(_THIS_DIR)
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)


class TestRegistry(unittest.TestCase):
    """@verify_item 데코레이터 + get_items 필터 + group/parent."""

    def setUp(self) -> None:
        from verify.lib import registry
        self.registry = registry
        # 글로벌 registry 보존을 위해 snapshot
        self._snapshot = dict(registry._REGISTRY)

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]

    def test_register_and_lookup(self) -> None:
        @self.registry.verify_item(
            id="TEST-001", stage=1, category="유닛",
            name="test register", presets=["test-preset"],
        )
        def _fn(ctx: Any) -> bool:
            return True
        rec = self.registry.get_item("TEST-001")
        self.assertIsNotNone(rec)
        meta, fn = rec
        self.assertEqual(meta.id, "TEST-001")
        self.assertEqual(meta.stage, 1)
        self.assertEqual(meta.category, "유닛")
        self.assertEqual(meta.presets, ["test-preset"])
        self.assertFalse(meta.is_group)
        self.assertIsNone(meta.parent)

    def test_invalid_stage_rejected(self) -> None:
        with self.assertRaises(ValueError):
            @self.registry.verify_item(
                id="TEST-INVALID-STAGE", stage=99, category="유닛", name="invalid",
            )
            def _fn(ctx: Any) -> bool: return True

    def test_duplicate_id_rejected(self) -> None:
        @self.registry.verify_item(
            id="TEST-DUP", stage=1, category="유닛", name="first",
        )
        def _fn1(ctx: Any) -> bool: return True
        with self.assertRaises(ValueError):
            @self.registry.verify_item(
                id="TEST-DUP", stage=1, category="유닛", name="second",
            )
            def _fn2(ctx: Any) -> bool: return True

    def test_group_and_parent_metadata(self) -> None:
        @self.registry.verify_item(
            id="TEST-GROUP", stage=5, category="유닛",
            name="group", is_group=True,
        )
        def _g(ctx: Any) -> Any: return None
        @self.registry.verify_item(
            id="TEST-GROUP-CHILD-A", stage=5, category="유닛",
            name="child A", parent="TEST-GROUP",
        )
        def _a(ctx: Any) -> bool: return True
        kids = self.registry.get_children("TEST-GROUP")
        self.assertEqual([k.id for k in kids], ["TEST-GROUP-CHILD-A"])
        groups = self.registry.get_groups(stage=5)
        self.assertIn("TEST-GROUP", [g.id for g in groups])

    def test_group_with_parent_rejected(self) -> None:
        # is_group=True 와 parent= 동시 지정은 데코레이터가 거부
        with self.assertRaises(ValueError):
            @self.registry.verify_item(
                id="TEST-BAD", stage=5, category="유닛",
                name="bad", is_group=True, parent="OTHER",
            )
            def _fn(ctx: Any) -> bool: return True

    def test_filter_by_stage(self) -> None:
        @self.registry.verify_item(
            id="TEST-S1-A", stage=1, category="유닛", name="A",
        )
        def _a(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-S1-B", stage=1, category="유닛", name="B",
        )
        def _b(ctx: Any) -> bool: return True
        items = self.registry.get_items(stage=1)
        ids = [m.id for m in items]
        self.assertIn("TEST-S1-A", ids)
        self.assertIn("TEST-S1-B", ids)

    def test_filter_include_children(self) -> None:
        @self.registry.verify_item(
            id="TEST-G2", stage=5, category="유닛", name="grp", is_group=True,
        )
        def _g(ctx: Any) -> Any: return None
        @self.registry.verify_item(
            id="TEST-G2-C", stage=5, category="유닛", name="child", parent="TEST-G2",
        )
        def _c(ctx: Any) -> bool: return True
        # include_children=False 면 자식 제외
        ids = [m.id for m in self.registry.get_items(stage=5, include_children=False)]
        self.assertIn("TEST-G2", ids)
        self.assertNotIn("TEST-G2-C", ids)
        # include_children=True 면 자식 포함
        ids = [m.id for m in self.registry.get_items(stage=5, include_children=True)]
        self.assertIn("TEST-G2-C", ids)

    def test_execution_order_sort(self) -> None:
        """execution_order — 명시 항목이 ID alphabetical 보다 우선."""
        @self.registry.verify_item(
            id="TEST-EO-Z", stage=5, category="유닛", name="z (order=10)",
            execution_order=10,
        )
        def _z(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-EO-A", stage=5, category="유닛", name="a (no order)",
        )
        def _a(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-EO-M", stage=5, category="유닛", name="m (order=20)",
            execution_order=20,
        )
        def _m(ctx: Any) -> bool: return True
        ids = [m.id for m in self.registry.get_items(stage=5)]
        # Z(order=10), M(order=20) 가 명시 → 먼저, A 는 alphabetical fallback
        z_idx = ids.index("TEST-EO-Z")
        m_idx = ids.index("TEST-EO-M")
        a_idx = ids.index("TEST-EO-A")
        self.assertLess(z_idx, m_idx)
        self.assertLess(m_idx, a_idx)

    def test_execution_order_in_get_children(self) -> None:
        """execution_order — 그룹 자식 정렬에도 반영."""
        @self.registry.verify_item(
            id="TEST-EOG", stage=5, category="유닛", name="grp", is_group=True,
        )
        def _g(ctx: Any) -> Any: return None
        @self.registry.verify_item(
            id="TEST-EOG-Z", stage=5, category="유닛", name="z child",
            parent="TEST-EOG", execution_order=1,
        )
        def _z(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-EOG-A", stage=5, category="유닛", name="a child",
            parent="TEST-EOG", execution_order=2,
        )
        def _a(ctx: Any) -> bool: return True
        kids = self.registry.get_children("TEST-EOG")
        # alphabetical 이면 A 먼저지만 execution_order=1 인 Z 가 먼저
        self.assertEqual([k.id for k in kids], ["TEST-EOG-Z", "TEST-EOG-A"])

    def test_filter_by_preset(self) -> None:
        @self.registry.verify_item(
            id="TEST-PRESET-A", stage=1, category="유닛", name="A",
            presets=["my-preset"],
        )
        def _a(ctx: Any) -> bool: return True
        @self.registry.verify_item(
            id="TEST-PRESET-B", stage=1, category="유닛", name="B",
            presets=["other-preset"],
        )
        def _b(ctx: Any) -> bool: return True
        items = self.registry.get_items(preset="my-preset")
        ids = [m.id for m in items]
        self.assertIn("TEST-PRESET-A", ids)
        self.assertNotIn("TEST-PRESET-B", ids)


class TestExpandLeaves(unittest.TestCase):
    """expand_to_leaves / selected_groups — 그룹 → 자식 펼치기."""

    def setUp(self) -> None:
        from verify.lib import registry
        self.registry = registry
        self._snapshot = dict(registry._REGISTRY)
        @registry.verify_item(id="TG-A", stage=5, category="u", name="grp",
                              is_group=True)
        def _g(ctx: Any) -> Any: return None
        @registry.verify_item(id="TG-A-1", stage=5, category="u", name="c1", parent="TG-A")
        def _c1(ctx: Any) -> bool: return True
        @registry.verify_item(id="TG-A-2", stage=5, category="u", name="c2", parent="TG-A")
        def _c2(ctx: Any) -> bool: return True
        @registry.verify_item(id="TG-FLAT", stage=5, category="u", name="flat")
        def _f(ctx: Any) -> bool: return True

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]

    def test_expand_group_to_children(self) -> None:
        leaves = self.registry.expand_to_leaves(["TG-A", "TG-FLAT"])
        # 그룹 자체는 빠지고 자식 + 평면만 남음
        self.assertIn("TG-A-1", leaves)
        self.assertIn("TG-A-2", leaves)
        self.assertIn("TG-FLAT", leaves)
        self.assertNotIn("TG-A", leaves)

    def test_selected_groups(self) -> None:
        groups = self.registry.selected_groups(["TG-A", "TG-FLAT", "TG-A-1"])
        self.assertEqual(groups, ["TG-A"])


class TestRunner(unittest.TestCase):
    """runner.run_items — 의존성/SKIP/group 합산."""

    def setUp(self) -> None:
        from verify.lib import registry, runner
        from verify.lib.context import VerifyContext
        self.registry = registry
        self.runner = runner
        self.VerifyContext = VerifyContext
        self._snapshot = dict(registry._REGISTRY)

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]

    def _ctx(self) -> Any:
        return self.VerifyContext(
            repo_root="/tmp", dist_dir="/tmp",
            report_path="/tmp/_test_report.md", stage=1, ts="20990101_000000",
        )

    def test_run_simple_pass(self) -> None:
        @self.registry.verify_item(
            id="TEST-SIMPLE", stage=1, category="유닛", name="simple",
        )
        def _fn(ctx: Any) -> bool:
            return True
        results = self.runner.run_items(self._ctx(), ["TEST-SIMPLE"])
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "PASS")
        self.assertEqual(results[0].id, "TEST-SIMPLE")

    def test_run_simple_fail(self) -> None:
        @self.registry.verify_item(
            id="TEST-FAIL", stage=1, category="유닛", name="fail",
        )
        def _fn(ctx: Any) -> bool: return False
        results = self.runner.run_items(self._ctx(), ["TEST-FAIL"])
        self.assertEqual(results[0].status, "FAIL")

    def test_dependency_skip_when_dep_fails(self) -> None:
        @self.registry.verify_item(
            id="TEST-DEP-A", stage=1, category="유닛", name="A (fails)",
        )
        def _a(ctx: Any) -> bool: return False
        @self.registry.verify_item(
            id="TEST-DEP-B", stage=1, category="유닛", name="B (depends)",
            depends_on=["TEST-DEP-A"],
        )
        def _b(ctx: Any) -> bool: return True
        results = self.runner.run_items(self._ctx(), ["TEST-DEP-A", "TEST-DEP-B"])
        statuses = {r.id: r.status for r in results}
        self.assertEqual(statuses["TEST-DEP-A"], "FAIL")
        self.assertEqual(statuses["TEST-DEP-B"], "SKIP")

    def test_execution_order_runner(self) -> None:
        """runner — 의존성 없을 때 stage 안에서 execution_order 순서로 실행."""
        order: list = []
        @self.registry.verify_item(
            id="TEST-ORD-LATE", stage=5, category="유닛", name="late",
            execution_order=20,
        )
        def _late(ctx: Any) -> bool:
            order.append("LATE"); return True
        @self.registry.verify_item(
            id="TEST-ORD-EARLY", stage=5, category="유닛", name="early",
            execution_order=10,
        )
        def _early(ctx: Any) -> bool:
            order.append("EARLY"); return True
        # 입력이 alphabetical 역순이어도 execution_order 로 정렬됨
        self.runner.run_items(self._ctx(), ["TEST-ORD-LATE", "TEST-ORD-EARLY"])
        self.assertEqual(order, ["EARLY", "LATE"])

    def test_dependency_topo_sort(self) -> None:
        order: list = []
        @self.registry.verify_item(
            id="TEST-T-A", stage=1, category="유닛", name="A",
        )
        def _a(ctx: Any) -> bool:
            order.append("A"); return True
        @self.registry.verify_item(
            id="TEST-T-B", stage=1, category="유닛", name="B",
            depends_on=["TEST-T-A"],
        )
        def _b(ctx: Any) -> bool:
            order.append("B"); return True
        # 입력 순서를 의도적으로 뒤집어도 위상 정렬로 A → B 보장
        self.runner.run_items(self._ctx(), ["TEST-T-B", "TEST-T-A"])
        self.assertEqual(order, ["A", "B"])

    def test_exception_becomes_fail(self) -> None:
        @self.registry.verify_item(
            id="TEST-EXC", stage=1, category="유닛", name="exception",
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

    def test_stage_gate_blocks_later_stages(self) -> None:
        """stage N FAIL → stage>N 의 leaf 자동 BLOCKED."""
        ran: list = []
        @self.registry.verify_item(
            id="GATE-S2-FAIL", stage=2, category="유닛", name="s2 fail",
        )
        def _s2(ctx: Any) -> bool:
            ran.append("S2"); return False
        @self.registry.verify_item(
            id="GATE-S3-A", stage=3, category="유닛", name="s3 a",
        )
        def _s3a(ctx: Any) -> bool:
            ran.append("S3-A"); return True
        @self.registry.verify_item(
            id="GATE-S5-A", stage=5, category="유닛", name="s5 a",
        )
        def _s5a(ctx: Any) -> bool:
            ran.append("S5-A"); return True
        results = self.runner.run_items(
            self._ctx(),
            ["GATE-S2-FAIL", "GATE-S3-A", "GATE-S5-A"],
        )
        statuses = {r.id: r.status for r in results}
        # S2 FAIL → S3, S5 모두 BLOCKED, 실행 자체 스킵
        self.assertEqual(statuses["GATE-S2-FAIL"], "FAIL")
        self.assertEqual(statuses["GATE-S3-A"], "BLOCKED")
        self.assertEqual(statuses["GATE-S5-A"], "BLOCKED")
        self.assertEqual(ran, ["S2"])    # S3/S5 함수 호출 자체가 안 됨

    def test_stage_gate_does_not_block_same_stage(self) -> None:
        """동일 stage 내 FAIL 은 후속 sibling 을 BLOCK 하지 않음 (계속 실행)."""
        ran: list = []
        @self.registry.verify_item(
            id="GATE-SAME-A", stage=2, category="유닛", name="a fails",
        )
        def _a(ctx: Any) -> bool:
            ran.append("A"); return False
        @self.registry.verify_item(
            id="GATE-SAME-B", stage=2, category="유닛", name="b succeeds",
        )
        def _b(ctx: Any) -> bool:
            ran.append("B"); return True
        results = self.runner.run_items(
            self._ctx(),
            ["GATE-SAME-A", "GATE-SAME-B"],
        )
        statuses = {r.id: r.status for r in results}
        self.assertEqual(statuses["GATE-SAME-A"], "FAIL")
        self.assertEqual(statuses["GATE-SAME-B"], "PASS")
        self.assertEqual(ran, ["A", "B"])

    def test_stage_gate_disabled(self) -> None:
        """stage_gate=False 면 후속 stage 도 그대로 실행."""
        ran: list = []
        @self.registry.verify_item(
            id="GATE-OFF-S2", stage=2, category="유닛", name="s2 fail",
        )
        def _s2(ctx: Any) -> bool:
            ran.append("S2"); return False
        @self.registry.verify_item(
            id="GATE-OFF-S3", stage=3, category="유닛", name="s3 ok",
        )
        def _s3(ctx: Any) -> bool:
            ran.append("S3"); return True
        results = self.runner.run_items(
            self._ctx(),
            ["GATE-OFF-S2", "GATE-OFF-S3"],
            stage_gate=False,
        )
        statuses = {r.id: r.status for r in results}
        self.assertEqual(statuses["GATE-OFF-S2"], "FAIL")
        self.assertEqual(statuses["GATE-OFF-S3"], "PASS")
        self.assertEqual(ran, ["S2", "S3"])

    def test_inject_fail_forces_fail(self) -> None:
        """ctx.opts['inject_fail'] 에 포함된 ID 는 함수 호출 없이 FAIL 반환."""
        called: list = []
        @self.registry.verify_item(
            id="TEST-INJECT-A", stage=1, category="유닛", name="injected",
        )
        def _a(ctx: Any) -> bool:
            called.append("A"); return True
        @self.registry.verify_item(
            id="TEST-INJECT-B", stage=1, category="유닛", name="real",
        )
        def _b(ctx: Any) -> bool:
            called.append("B"); return True

        ctx = self.VerifyContext(
            repo_root="/tmp", dist_dir="/tmp",
            report_path="/tmp/_test_report.md", stage=1, ts="20990101_000000",
            opts={"inject_fail": {"TEST-INJECT-A"}},
        )
        results = self.runner.run_items(
            ctx, ["TEST-INJECT-A", "TEST-INJECT-B"], stage_gate=False,
        )
        statuses = {r.id: r.status for r in results}
        self.assertEqual(statuses["TEST-INJECT-A"], "FAIL")
        self.assertEqual(statuses["TEST-INJECT-B"], "PASS")
        # 강제 FAIL 항목 함수는 호출 안 됨
        self.assertEqual(called, ["B"])
        # detail 에 주입 표시
        a_detail = next(r.detail for r in results if r.id == "TEST-INJECT-A")
        self.assertIn("--inject-fail", a_detail)

    def test_stage_gate_emits_marker(self) -> None:
        """stage gate 차단 발생 시 [VERIFY] stage-blocked 마커 emit."""
        @self.registry.verify_item(
            id="GATE-MK-S2", stage=2, category="유닛", name="s2 fail",
        )
        def _s2(ctx: Any) -> bool: return False
        @self.registry.verify_item(
            id="GATE-MK-S3", stage=3, category="유닛", name="s3",
        )
        def _s3(ctx: Any) -> bool: return True

        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.runner.run_items(self._ctx(), ["GATE-MK-S2", "GATE-MK-S3"])
        out = buf.getvalue()
        self.assertIn("[VERIFY] item-end: GATE-MK-S3 status=BLOCKED", out)
        self.assertIn("[VERIFY] stage-blocked: stage=3 reason=stage2-FAIL count=1", out)

    def test_group_worst_status_aggregation(self) -> None:
        """그룹 입력 시 자식 worst-status 로 합산된 ItemResult 1건 반환."""
        @self.registry.verify_item(id="TG-WST", stage=5, category="u",
                                    name="grp", is_group=True)
        def _g(ctx: Any) -> Any: return None
        @self.registry.verify_item(id="TG-WST-OK", stage=5, category="u",
                                    name="ok", parent="TG-WST")
        def _ok(ctx: Any) -> bool: return True
        @self.registry.verify_item(id="TG-WST-NG", stage=5, category="u",
                                    name="ng", parent="TG-WST")
        def _ng(ctx: Any) -> bool: return False
        results = self.runner.run_items(self._ctx(), ["TG-WST"])
        # 그룹 자체 1건 (자식들은 children 으로 들어감)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].id, "TG-WST")
        self.assertEqual(results[0].status, "FAIL")    # worst = FAIL
        # children 에는 자식 결과 들어 있어야 함
        ids = {c.id for c in results[0].children}
        self.assertEqual(ids, {"TG-WST-OK", "TG-WST-NG"})


class TestPresets(unittest.TestCase):
    """presets — list / resolve / dynamic."""

    def setUp(self) -> None:
        from verify.lib import presets, registry
        self.presets = presets
        self.registry = registry
        self._snapshot = dict(registry._REGISTRY)
        @self.registry.verify_item(
            id="TEST-PR-X", stage=1, category="유닛", name="X",
            presets=["test-pr-static"],
        )
        def _x(ctx: Any) -> bool: return True

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]
        self.presets._PRESETS.pop("test-pr-static", None)
        self.presets._PRESETS.pop("test-pr-dyn", None)

    def test_register_and_resolve_static(self) -> None:
        self.presets.register_preset("test-pr-static", ["TEST-PR-X"])
        items = self.presets.resolve_preset("test-pr-static")
        self.assertEqual(items, ["TEST-PR-X"])

    def test_resolve_dynamic(self) -> None:
        self.presets.register_preset(
            "test-pr-dyn",
            lambda: [m.id for m in self.registry.get_items(stage=1)],
        )
        items = self.presets.resolve_preset("test-pr-dyn")
        self.assertIn("TEST-PR-X", items)

    def test_resolve_unknown_returns_empty(self) -> None:
        self.assertEqual(self.presets.resolve_preset("nonexistent"), [])


class TestExistingRegistry(unittest.TestCase):
    """기본 verify.lib import 후 등록된 항목들이 정상 메타를 가지는지."""

    def test_stage6_items_registered(self) -> None:
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        ids = {m.id for m in registry.get_items(stage=6)}
        self.assertIn("S6-ENTRY-CHECK", ids)
        self.assertIn("S6-SEED", ids)
        self.assertIn("S6-SCN-VOLTE-VOICE", ids)
        self.assertIn("S6-SUMMARY", ids)

    def test_stage3_items_registered(self) -> None:
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        ids = {m.id for m in registry.get_items(stage=3)}
        self.assertIn("S3-RESET", ids)
        self.assertIn("S3-START", ids)
        self.assertIn("S3-HEALTH", ids)
        self.assertIn("S3-SEED", ids)
        self.assertIn("S3-SCN-VOIP-SMOKE", ids)
        self.assertIn("S3-SCN-PTT-SMOKE", ids)

    def test_stage5_groups_registered(self) -> None:
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        # S5 의 5개 그룹
        groups = registry.get_groups(stage=5)
        gids = {g.id for g in groups}
        self.assertEqual(gids, {
            "S5-CSC-DEPLOY", "S5-CSC-VERIFY", "S5-CSC-RUN",
            "S5-MODULES-DEPLOY", "S5-MODULES-RUN",
        })
        # 자식 — 13개
        kids: list = []
        for g in groups:
            kids.extend(registry.get_children(g.id))
        self.assertEqual(len(kids), 13)

    def test_stage5_execution_order(self) -> None:
        """S5 항목 실행 순서: RESET → CSC-DEPLOY(*) → CSC-VERIFY(*) →
        CSC-RUN(*) → MODULES-DEPLOY(*) → MODULES-RUN(*) → FINALIZE.
        """
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        ids = [m.id for m in registry.get_items(stage=5, include_children=True)]
        # 핵심 mile-stone 의 인덱스 — 부분 순서 검증
        idx = lambda x: ids.index(x)
        self.assertLess(idx("S5-RESET"), idx("S5-CSC-DEPLOY"))
        self.assertLess(idx("S5-CSC-DEPLOY"), idx("S5-CSC-DEPLOY-AGENT-ENROLL"))
        self.assertLess(idx("S5-CSC-DEPLOY-AGENT-ENROLL"), idx("S5-CSC-DEPLOY-PKG-UPLOAD"))
        self.assertLess(idx("S5-CSC-DEPLOY-PKG-UPLOAD"), idx("S5-CSC-DEPLOY-INSTALL"))
        self.assertLess(idx("S5-CSC-DEPLOY-INSTALL"), idx("S5-CSC-VERIFY"))
        self.assertLess(idx("S5-CSC-VERIFY"), idx("S5-CSC-RUN"))
        self.assertLess(idx("S5-CSC-RUN"), idx("S5-MODULES-DEPLOY"))
        self.assertLess(idx("S5-MODULES-DEPLOY-AUTH"), idx("S5-MODULES-DEPLOY-PKG-UPLOAD"))
        self.assertLess(idx("S5-MODULES-DEPLOY-PKG-UPLOAD"), idx("S5-MODULES-DEPLOY-AGENT-ENROLL"))
        self.assertLess(idx("S5-MODULES-DEPLOY-AGENT-ENROLL"), idx("S5-MODULES-DEPLOY-INSTALL"))
        self.assertLess(idx("S5-MODULES-DEPLOY"), idx("S5-MODULES-RUN"))
        self.assertLess(idx("S5-MODULES-RUN"), idx("S5-FINALIZE"))

    def test_stage1_items_registered(self) -> None:
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        ids = {m.id for m in registry.get_items(stage=1)}
        self.assertIn("S1-PY-SYNTAX", ids)
        self.assertIn("S1-FRONTEND-LINT", ids)
        self.assertIn("S1-FRONTEND-TYPECHECK", ids)
        self.assertIn("S1-CPP-FORMAT", ids)
        self.assertIn("S1-UNIT-VERIFY-LIB", ids)

    def test_stage4_items_registered(self) -> None:
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        rec = registry.get_item("S4-PKG-BUILD")
        self.assertIsNotNone(rec)
        self.assertEqual(rec[0].stage, 4)
        rec = registry.get_item("S4-PKG-MANIFEST")
        self.assertIsNotNone(rec)
        self.assertEqual(rec[0].stage, 4)

    def test_validate_registry_clean(self) -> None:
        """무결성 검증 — 그룹/자식 stage/parent 일관성."""
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        issues = registry.validate_registry()
        self.assertEqual(issues, [], f"validation issues: {issues}")

    def test_presets_loaded(self) -> None:
        from verify.lib import presets
        from verify.lib import items                            # noqa: F401
        names = {p["name"] for p in presets.list_presets()}
        for required in (
            "stage1-full", "stage2-full", "stage3-full", "stage3-quick",
            "stage4-full", "stage5-full", "stage6-full",
            "stage6-volte", "stage6-ptt",
            "pipeline-full", "pre-package", "post-deploy",
        ):
            self.assertIn(required, names, f"missing preset: {required}")


class TestRunnerMarkers(unittest.TestCase):
    """[VERIFY] item-start / item-end / child-result / group-end 마커.

    backend (verification.py _parse_items_progress) 가 파싱하므로 형식 회귀 안전망.
    """

    def setUp(self) -> None:
        from verify.lib import registry, runner, context
        self.registry = registry
        self.runner = runner
        self.context = context
        self._snapshot = dict(registry._REGISTRY)

    def tearDown(self) -> None:
        added = [k for k in self.registry._REGISTRY if k not in self._snapshot]
        for k in added:
            del self.registry._REGISTRY[k]

    def _make_ctx(self, tmpdir):
        return self.context.VerifyContext.create(
            repo_root=_REPO_ROOT, stage=1,
            report_dir=tmpdir,
        )

    def test_runner_emits_item_markers(self) -> None:
        from verify.lib.registry import ItemResult, ItemStatus
        @self.registry.verify_item(id="MARKER-A", stage=1, category="유닛", name="alpha")
        def _a(ctx) -> ItemResult:
            return ItemResult(id="MARKER-A", name="alpha", status=ItemStatus.PASS)
        @self.registry.verify_item(id="MARKER-B", stage=1, category="유닛", name="beta")
        def _b(ctx) -> ItemResult:
            return ItemResult(id="MARKER-B", name="beta", status=ItemStatus.PASS)

        import io, tempfile
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            ctx = self._make_ctx(td)
            try:
                with redirect_stdout(buf):
                    self.runner.run_items(ctx, ["MARKER-A", "MARKER-B"])
            finally:
                ctx.report_close()
        out = buf.getvalue()
        self.assertIn("[VERIFY] run-start: total=2", out)
        # stage=1 추가됨 (B4 backend 파서와 일관)
        self.assertIn("[VERIFY] item-start: MARKER-A stage=1 idx=1/2 name=alpha", out)
        self.assertIn("[VERIFY] item-end: MARKER-A status=PASS", out)
        self.assertIn("[VERIFY] item-start: MARKER-B stage=1 idx=2/2 name=beta", out)
        self.assertIn("[VERIFY] item-end: MARKER-B status=PASS", out)
        self.assertIn("[VERIFY] run-end: total=2 pass=2", out)

    def test_runner_emits_group_end_marker(self) -> None:
        """is_group + 자식 → group-end 마커 emit + 자식별 child-result."""
        from verify.lib.registry import ItemResult, ItemStatus
        @self.registry.verify_item(id="MK-GRP", stage=5, category="유닛",
                                    name="grp", is_group=True)
        def _g(ctx) -> Any: return None
        @self.registry.verify_item(id="MK-GRP-A", stage=5, category="유닛",
                                    name="alpha", parent="MK-GRP")
        def _a(ctx) -> ItemResult:
            return ItemResult(id="MK-GRP-A", name="alpha",
                              status=ItemStatus.PASS, elapsed_ms=10)
        @self.registry.verify_item(id="MK-GRP-B", stage=5, category="유닛",
                                    name="beta", parent="MK-GRP")
        def _b(ctx) -> ItemResult:
            return ItemResult(id="MK-GRP-B", name="beta",
                              status=ItemStatus.FAIL, elapsed_ms=20)

        import io, tempfile
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            ctx = self._make_ctx(td)
            try:
                with redirect_stdout(buf):
                    self.runner.run_items(ctx, ["MK-GRP"])
            finally:
                ctx.report_close()
        out = buf.getvalue()
        # 자식 child-result 마커
        self.assertIn("[VERIFY] child-result: MK-GRP.MK-GRP-A status=PASS", out)
        self.assertIn("[VERIFY] child-result: MK-GRP.MK-GRP-B status=FAIL", out)
        # group-end 마커 — 부모 status 는 worst=FAIL
        self.assertIn("[VERIFY] group-end: MK-GRP status=FAIL child_count=2", out)


class TestParseItemsProgress(unittest.TestCase):
    """csc/src/handlers/verification.py _parse_items_progress 의 stdout 파싱.

    runner.py 의 [VERIFY] item-start/item-end/child-result/group-end/
    stage-blocked 마커 정규식 검증.
    """

    def setUp(self) -> None:
        repo_root = _REPO_ROOT
        csc_src = os.path.join(repo_root, "csc", "src")
        if csc_src not in sys.path:
            sys.path.insert(0, csc_src)
        # httpsrv 가 없으면 stub 으로 대체
        if "httpsrv" not in sys.modules:
            import types
            ha = types.ModuleType("httpsrv")
            hh = types.ModuleType("httpsrv.handler")
            class _HA: pass
            class _HR:
                def __init__(self, status=200, body=None):
                    self.status, self.body = status, body
            hh.HandlerArgs = _HA
            hh.HandlerResult = _HR
            sys.modules["httpsrv"] = ha
            sys.modules["httpsrv.handler"] = hh
        import importlib
        try:
            self.verification = importlib.import_module("handlers.verification")
        except Exception as e:
            self.skipTest(f"handlers.verification import 실패: {e}")
        self._tmpfiles: list = []

    def tearDown(self) -> None:
        for p in self._tmpfiles:
            try: os.remove(p)
            except OSError: pass

    def _write_log(self, lines: list) -> str:
        import tempfile
        fd, path = tempfile.mkstemp(prefix="verify_log_", suffix=".log")
        os.close(fd)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        self._tmpfiles.append(path)
        return path

    def test_parse_basic_two_items(self) -> None:
        log = self._write_log([
            "[VERIFY] run-start: total=2 ids=A,B",
            "[VERIFY] item-start: A stage=1 idx=1/2 name=alpha",
            "[VERIFY] item-end: A status=PASS elapsed_ms=320",
            "[VERIFY] item-start: B stage=1 idx=2/2 name=beta",
            "[VERIFY] item-end: B status=FAIL elapsed_ms=100",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertEqual(p["total"], 2)
        self.assertEqual(p["completed"], 2)
        self.assertEqual(p["selected"], ["A", "B"])
        self.assertEqual(len(p["items"]), 2)
        self.assertEqual(p["items"][0]["status"], "PASS")
        self.assertEqual(p["items"][0]["stage"], 1)
        self.assertEqual(p["items"][0]["elapsed_ms"], 320)
        self.assertEqual(p["items"][1]["status"], "FAIL")
        self.assertIsNone(p["current"])

    def test_parse_running_state(self) -> None:
        """item-end 가 아직 안 온 항목은 status=RUNNING + current 로 표시."""
        log = self._write_log([
            "[VERIFY] run-start: total=2 ids=A,B",
            "[VERIFY] item-start: A stage=1 idx=1/2 name=alpha",
            "[VERIFY] item-end: A status=PASS elapsed_ms=10",
            "[VERIFY] item-start: B stage=1 idx=2/2 name=beta",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertEqual(p["completed"], 1)
        self.assertEqual(p["current"], "B")
        self.assertEqual(p["items"][1]["status"], "RUNNING")

    def test_parse_children(self) -> None:
        log = self._write_log([
            "[VERIFY] run-start: total=1 ids=S5-CSC-DEPLOY",
            "[VERIFY] item-start: S5-CSC-DEPLOY-AGENT-ENROLL stage=5 idx=1/3 name=enroll",
            "[VERIFY] child-result: S5-CSC-DEPLOY.S5-CSC-DEPLOY-AGENT-ENROLL status=PASS elapsed_ms=320 name=enroll",
            "[VERIFY] item-end: S5-CSC-DEPLOY-AGENT-ENROLL status=PASS elapsed_ms=320",
            "[VERIFY] group-end: S5-CSC-DEPLOY status=PASS child_count=1",
        ])
        p = self.verification._parse_items_progress(log)
        # 부모 그룹은 child-result/group-end 로 만들어짐
        parent = next((it for it in p["items"] if it["id"] == "S5-CSC-DEPLOY"), None)
        self.assertIsNotNone(parent)
        self.assertEqual(parent["status"], "PASS")
        self.assertEqual(len(parent["children"]), 1)
        self.assertEqual(parent["children"][0]["id"], "S5-CSC-DEPLOY-AGENT-ENROLL")

    def test_parse_run_end_summary(self) -> None:
        """run-end 마커의 pass/fail/skip/blocked 카운트 추출."""
        log = self._write_log([
            "[VERIFY] run-start: total=3 ids=A,B,C",
            "[VERIFY] item-start: A stage=1 idx=1/3 name=a",
            "[VERIFY] item-end: A status=PASS elapsed_ms=10",
            "[VERIFY] item-start: B stage=1 idx=2/3 name=b",
            "[VERIFY] item-end: B status=FAIL elapsed_ms=20",
            "[VERIFY] item-start: C stage=1 idx=3/3 name=c",
            "[VERIFY] item-end: C status=SKIP elapsed_ms=0",
            "[VERIFY] run-end: total=3 pass=1 fail=1 skip=1 blocked=0",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertIsNotNone(p["summary"])
        self.assertEqual(p["summary"], {"pass": 1, "fail": 1, "skip": 1, "blocked": 0})

    def test_parse_strips_ansi(self) -> None:
        log = self._write_log([
            "\x1b[32m[VERIFY] run-start: total=1 ids=A\x1b[0m",
            "\x1b[36m[VERIFY] item-start: A stage=1 idx=1/1 name=test\x1b[0m",
            "[VERIFY] item-end: A status=PASS elapsed_ms=5",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertEqual(p["total"], 1)
        self.assertEqual(p["items"][0]["status"], "PASS")

    def test_parse_empty_log(self) -> None:
        p = self.verification._parse_items_progress("/nonexistent/path")
        self.assertEqual(p["total"], 0)
        self.assertEqual(p["items"], [])

    def test_pkg_manifest_immutability_check(self) -> None:
        """common.pkg_manifest — write_marker / immutability_check 라운드트립."""
        import tempfile
        import json as _json
        from verify.lib.common import pkg_manifest as pm

        with tempfile.TemporaryDirectory() as td:
            # manifest 부재 → FAIL
            ok, cur, dep, msg = pm.immutability_check(td)
            self.assertFalse(ok)
            self.assertIsNone(cur)
            self.assertIn("manifest.json 없음", msg)

            # manifest 만 있고 marker 없으면 FAIL
            os.makedirs(os.path.join(td, "packages"), exist_ok=True)
            mp = pm.manifest_path(td)
            with open(mp, "w") as f:
                _json.dump({"packages": [{"name": "a.tar.gz", "sha256": "x"}]}, f)
            ok, cur, dep, msg = pm.immutability_check(td)
            self.assertFalse(ok)
            self.assertIsNotNone(cur)
            self.assertIsNone(dep)
            self.assertIn("marker", msg.lower())

            # marker 작성 후 → PASS
            sha1 = pm.write_marker(td)
            self.assertEqual(len(sha1), 64)
            ok, cur, dep, msg = pm.immutability_check(td)
            self.assertTrue(ok)
            self.assertEqual(cur, dep)

            # manifest 변경 → FAIL (deploy 후 재패키지화 시뮬레이션)
            with open(mp, "w") as f:
                _json.dump({"packages": [{"name": "a.tar.gz", "sha256": "y-changed"}]}, f)
            ok, cur, dep, msg = pm.immutability_check(td)
            self.assertFalse(ok)
            self.assertNotEqual(cur, dep)
            self.assertIn("불일치", msg)

    def test_parse_stage_blocked_marker(self) -> None:
        """[VERIFY] stage-blocked 마커 파싱 → progress.stage_gate."""
        log = self._write_log([
            "[VERIFY] run-start: total=3 ids=A,B,C",
            "[VERIFY] item-start: A stage=2 idx=1/3 name=a",
            "[VERIFY] item-end: A status=FAIL elapsed_ms=5",
            "[VERIFY] item-start: B stage=3 idx=2/3 name=b",
            "[VERIFY] item-end: B status=BLOCKED elapsed_ms=0",
            "[VERIFY] item-start: C stage=5 idx=3/3 name=c",
            "[VERIFY] item-end: C status=BLOCKED elapsed_ms=0",
            "[VERIFY] stage-blocked: stage=3 reason=stage2-FAIL count=1",
            "[VERIFY] stage-blocked: stage=5 reason=stage2-FAIL count=1",
            "[VERIFY] run-end: total=3 pass=0 fail=1 skip=0 blocked=2",
        ])
        p = self.verification._parse_items_progress(log)
        sg = p.get("stage_gate")
        self.assertIsNotNone(sg)
        self.assertEqual(sg["first_failed"], 2)
        self.assertEqual(sg["blocked_stages"], {3: 1, 5: 1})


class TestStage5NativeSteps(unittest.TestCase):
    """S5 native Python step 구현 — _native_steps.step_01_cleanup."""

    def setUp(self) -> None:
        from verify.lib.items.stage5 import _native_steps
        from verify.lib.context import VerifyContext
        from verify.lib import shell as _shell
        from verify.lib.registry import ItemStatus
        self._native = _native_steps
        self._VerifyContext = VerifyContext
        self._shell = _shell
        self._ItemStatus = ItemStatus
        self._orig_run_cims_sh = _shell.run_cims_sh

    def tearDown(self) -> None:
        self._shell.run_cims_sh = self._orig_run_cims_sh

    def _make_ctx(self):
        return self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)

    def test_step_01_pass(self) -> None:
        captured = []
        def fake_run(repo_root, *args, **kw):
            captured.append((repo_root, args, kw))
            return (0, "[INFO] cleanup ok\n", "")
        self._shell.run_cims_sh = fake_run

        ctx = self._make_ctx()
        result = self._native.step_01_cleanup(ctx)

        self.assertEqual(result.id, "S5-RESET")
        self.assertEqual(result.status, self._ItemStatus.PASS)
        self.assertIn("rc=0", result.detail)
        # cmd_reset --all --keep-processes 호출 확인
        self.assertEqual(len(captured), 1)
        self.assertEqual(captured[0][1][:3], ("reset", "--all", "--keep-processes"))

    def test_step_01_fail(self) -> None:
        def fake_run(repo_root, *args, **kw):
            return (1, "", "[ERR] cleanup failed: db connection refused")
        self._shell.run_cims_sh = fake_run

        ctx = self._make_ctx()
        result = self._native.step_01_cleanup(ctx)

        self.assertEqual(result.status, self._ItemStatus.FAIL)
        self.assertIn("rc=1", result.detail)
        self.assertIn("db connection refused", result.detail)

    def test_step_01_cached_no_double_run(self) -> None:
        call_count = [0]
        def fake_run(repo_root, *args, **kw):
            call_count[0] += 1
            return (0, "", "")
        self._shell.run_cims_sh = fake_run

        ctx = self._make_ctx()
        r1 = self._native.step_01_cleanup(ctx)
        r2 = self._native.step_01_cleanup(ctx)
        # 두 번째 호출은 cache 사용 — fake_run 1회만 호출
        self.assertEqual(call_count[0], 1)
        self.assertIs(r1, r2)
        self.assertTrue(self._native.already_ran(ctx, 1))


class TestStage5AgentEnrollSteps(unittest.TestCase):
    """S5 native — step 05/06/07 (agent enroll chain)."""

    def setUp(self) -> None:
        from verify.lib.items.stage5 import _native_steps
        from verify.lib.context import VerifyContext
        from verify.lib.common import csc_http
        from verify.lib.registry import ItemStatus
        self._native = _native_steps
        self._VerifyContext = VerifyContext
        self._csc_http = csc_http
        self._ItemStatus = ItemStatus
        # 원본 함수 보관
        self._orig = {
            "admin_login":  csc_http.admin_login,
            "post_json":    csc_http.post_json,
            "delete":       csc_http.delete,
            "find_by_name": csc_http.find_agent_id_by_name,
        }

    def tearDown(self) -> None:
        for k, v in self._orig.items():
            if k == "admin_login":  self._csc_http.admin_login = v
            elif k == "post_json":  self._csc_http.post_json = v
            elif k == "delete":     self._csc_http.delete = v
            elif k == "find_by_name": self._csc_http.find_agent_id_by_name = v

    def _ctx(self):
        return self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)

    # ── step 05 ──
    def test_step_05_login_pass(self) -> None:
        called = []
        def fake_login(base, lid, pw, timeout=5):
            called.append((base, lid, pw))
            return "JWT-ABC123"
        self._csc_http.admin_login = fake_login
        ctx = self._ctx()
        r = self._native.step_05_admin_login(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "tok"), "JWT-ABC123")
        # 두 번째 호출은 cache (admin_login 추가 호출 X)
        self._native.step_05_admin_login(ctx)
        self.assertEqual(len(called), 1)

    def test_step_05_login_fail(self) -> None:
        def fake_login(base, lid, pw, timeout=5):
            return ""
        self._csc_http.admin_login = fake_login
        ctx = self._ctx()
        r = self._native.step_05_admin_login(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("admin login 실패", r.detail)
        self.assertIsNone(self._native._get(ctx, "tok"))

    # ── step 06 ──
    def test_step_06_skips_without_tok(self) -> None:
        ctx = self._ctx()
        r = self._native.step_06_agent_register(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("step 05", r.detail)

    def test_step_06_register_pass(self) -> None:
        calls = []
        def fake_post(url, payload, token=None, timeout=10):
            calls.append((url, payload))
            if url.endswith("/agents"):
                return (201, {"id": 42, "enrollment_token": "ENR-XYZ"})
            if url.endswith("/approve"):
                return (200, {"ok": True})
            return (500, {})
        self._csc_http.post_json = fake_post
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        r = self._native.step_06_agent_register(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "aid_csc"), 42)
        self.assertEqual(self._native._get(ctx, "enroll_tok_csc"), "ENR-XYZ")
        # POST /agents 1회 + approve 1회
        self.assertEqual(len(calls), 2)
        self.assertTrue(calls[1][0].endswith("/agents/42/approve"))

    def test_step_06_register_409_recreate(self) -> None:
        attempts = {"post": 0, "delete": 0}
        def fake_post(url, payload, token=None, timeout=10):
            if url.endswith("/agents"):
                attempts["post"] += 1
                if attempts["post"] == 1:
                    return (409, {"error": "exists"})
                return (201, {"id": 99, "enrollment_token": "RE-ENR"})
            if url.endswith("/approve"):
                return (200, {"ok": True})
            return (500, {})
        def fake_find(base, tok, name):
            return 99
        def fake_delete(url, token=None, timeout=10):
            attempts["delete"] += 1
            return 204
        self._csc_http.post_json = fake_post
        self._csc_http.find_agent_id_by_name = fake_find
        self._csc_http.delete = fake_delete

        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        r = self._native.step_06_agent_register(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(attempts["post"], 2)        # 첫 409 + 재생성
        self.assertEqual(attempts["delete"], 1)
        self.assertEqual(self._native._get(ctx, "aid_csc"), 99)

    def test_step_06_register_http_error_after_409(self) -> None:
        def fake_post(url, payload, token=None, timeout=10):
            return (500, {"error": "internal"})
        self._csc_http.post_json = fake_post
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        r = self._native.step_06_agent_register(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("status=500", r.detail)

    # ── step 07 ──
    def test_step_07_skips_without_aid(self) -> None:
        ctx = self._ctx()
        r = self._native.step_07_testagent_spawn(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_07_fails_when_agent_py_missing(self) -> None:
        ctx = self._ctx()
        self._native._set(ctx, "aid_csc", 1)
        self._native._set(ctx, "enroll_tok_csc", "TOK")
        # ctx.dist_dir 의 build/dist/agent/cims_agent.py 가 없으면 FAIL
        # (실제 환경에 있을 수도 있으므로 임시 dist 로 override)
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ctx.dist_dir = td
            r = self._native.step_07_testagent_spawn(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("cims_agent.py 없음", r.detail)

    # ── 합성 함수 ──
    def test_steps_05_06_07_composite_skip_chain(self) -> None:
        """step 05 FAIL → 06/07 모두 SKIP, worst=FAIL."""
        def fake_login(base, lid, pw, timeout=5):
            return ""    # login 실패
        self._csc_http.admin_login = fake_login
        ctx = self._ctx()
        result = self._native.steps_05_06_07_agent_enroll(ctx)
        self.assertEqual(result.id, "S5-CSC-DEPLOY-AGENT-ENROLL")
        self.assertEqual(result.status, self._ItemStatus.FAIL)
        # 자식 3개 — 1 FAIL + 2 SKIP
        self.assertEqual(len(result.children), 3)
        statuses = [c.status for c in result.children]
        self.assertEqual(statuses[0], self._ItemStatus.FAIL)
        self.assertEqual(statuses[1], self._ItemStatus.SKIP)
        self.assertEqual(statuses[2], self._ItemStatus.SKIP)


class TestStage5CscDeploySteps(unittest.TestCase):
    """S5 native — step 08 (package upload), 09 (deployment), 10 (install poll)."""

    def setUp(self) -> None:
        from verify.lib.items.stage5 import _native_steps
        from verify.lib.context import VerifyContext
        from verify.lib.common import csc_http
        from verify.lib.common import db as _db
        from verify.lib.registry import ItemStatus
        self._native = _native_steps
        self._VerifyContext = VerifyContext
        self._csc_http = csc_http
        self._db = _db
        self._ItemStatus = ItemStatus
        self._orig = {
            "post_multipart": csc_http.post_multipart,
            "post_json":      csc_http.post_json,
            "csp_db_config":  _db.csp_db_config,
            "connect":        _db.connect,
        }

    def tearDown(self) -> None:
        self._csc_http.post_multipart = self._orig["post_multipart"]
        self._csc_http.post_json      = self._orig["post_json"]
        self._db.csp_db_config        = self._orig["csp_db_config"]
        self._db.connect              = self._orig["connect"]

    def _ctx_with_dist(self, dist_dir):
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        ctx.dist_dir = dist_dir
        return ctx

    # ── step 08 ──
    def test_step_08_skips_without_tok(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ctx = self._ctx_with_dist(td)
            r = self._native.step_08_package_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("admin login", r.detail)

    def test_step_08_fail_when_tarball_missing(self) -> None:
        import tempfile
        # tok 은 있지만 packages/ 가 비어있음
        captured = []
        def fake_post_multipart(url, **kw):
            captured.append(url)
            return (200, {"id": 1})
        self._csc_http.post_multipart = fake_post_multipart
        with tempfile.TemporaryDirectory() as td:
            os.makedirs(os.path.join(td, "packages"), exist_ok=True)
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok", "JWT")
            r = self._native.step_08_package_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("tarball 없음", r.detail)
        # 업로드는 호출 안 됨
        self.assertEqual(captured, [])

    def test_step_08_pass_with_tarballs(self) -> None:
        import tempfile
        captured: list = []
        def fake_post_multipart(url, *, file_path, file_field="file",
                                filename=None, form_fields=None,
                                token=None, timeout=60):
            captured.append((url, file_path, form_fields, token))
            # csc 가 먼저 / console 다음 — 파일명으로 구분해 다른 id 반환
            base = os.path.basename(file_path)
            pid = 1 if base.startswith("csc-") else 2
            return (201, {"id": pid})
        self._csc_http.post_multipart = fake_post_multipart

        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            # csc-1.0.0.tar.gz, csc-1.10.0.tar.gz (natural sort: 1.10 > 1.0)
            for fn in ("csc-1.0.0.tar.gz", "csc-1.10.0.tar.gz",
                       "console-2.5.0.tar.gz"):
                with open(os.path.join(pkg_dir, fn), "w") as f:
                    f.write("dummy")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok", "JWT")
            r = self._native.step_08_package_upload(ctx)

        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "pkg_id_csc"), 1)
        self.assertEqual(self._native._get(ctx, "pkg_id_console"), 2)
        # csc 는 1.10.0 (natural sort) 이 선택됐는지 검증
        csc_call = next(c for c in captured if "csc-" in c[1])
        self.assertIn("csc-1.10.0.tar.gz", csc_call[1])
        # form_fields 에 force=true
        self.assertEqual(csc_call[2], {"force": "true"})
        self.assertEqual(csc_call[3], "JWT")

    def test_step_08_fail_on_http_error(self) -> None:
        import tempfile
        def fake_post_multipart(url, **kw):
            return (500, {"error": "internal"})
        self._csc_http.post_multipart = fake_post_multipart
        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            with open(os.path.join(pkg_dir, "csc-1.0.0.tar.gz"), "w") as f:
                f.write("d")
            with open(os.path.join(pkg_dir, "console-1.0.0.tar.gz"), "w") as f:
                f.write("d")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok", "JWT")
            r = self._native.step_08_package_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("status=500", r.detail)

    # ── step 09 ──
    def test_step_09_skips_without_tok_or_aid(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        # tok 있지만 aid 없음
        self._native._set(ctx, "tok", "JWT")
        r = self._native.step_09_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_09_fail_without_pkg_ids(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "aid_csc", 7)
        # pkg_id_csc / pkg_id_console 미설정 → SKIP 노트 + FAIL
        r = self._native.step_09_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("step 08", r.detail)

    def test_step_09_pass_with_overlay(self) -> None:
        captured: list = []
        def fake_post_json(url, payload, token=None, timeout=15):
            captured.append((url, payload))
            # csc → did=11, console → did=22
            did = 11 if payload["process_name"] == "CSC" else 22
            return (201, {"id": did})
        self._csc_http.post_json = fake_post_json

        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "aid_csc", 7)
        self._native._set(ctx, "pkg_id_csc", 1)
        self._native._set(ctx, "pkg_id_console", 2)
        r = self._native.step_09_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "dep_id_csc"), 11)
        self.assertEqual(self._native._get(ctx, "dep_id_console"), 22)
        # config overlay 검증 — csc:Server.Port=4445, console:Port=8081
        csc_payload = next(p for u, p in captured if p["process_name"] == "CSC")
        self.assertEqual(csc_payload["config"], {"Server.Port": 4445})
        cons_payload = next(p for u, p in captured if p["process_name"] == "CONSOLE")
        self.assertEqual(cons_payload["config"], {"Port": 8081})

    # ── step 10 ──
    def test_step_10_skips_without_tok(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        r = self._native.step_10_install_poll(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_10_fail_without_dep_ids(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok", "JWT")
        r = self._native.step_10_install_poll(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("step 09", r.detail)

    def test_step_10_pass_when_all_succeed(self) -> None:
        # agent_deployment.status enum: pending|deploying|running|stopped|failed|removed
        # PASS 조건: 폴링 종료 시 모든 deployment 가 running 또는 stopped.
        def fake_post_json(url, payload, token=None, timeout=10):
            return (202, {"job_id": 1})
        self._csc_http.post_json = fake_post_json

        self._db.csp_db_config = lambda dist: {"Host": "x", "User": "x",
                                                "Password": "x", "DbName": "x"}

        class FakeCursor:
            def __init__(self, results): self._r = results
            def execute(self, sql, params=None):
                self._params = params
            def fetchone(self):
                return ("running",)    # install 후 정상 done 상태

        class FakeConn:
            def cursor(self): return FakeCursor([])
            def close(self): pass

        self._db.connect = lambda cfg: FakeConn()

        # time.sleep 을 빠르게 만들기
        import time as _t
        orig_sleep = _t.sleep
        _t.sleep = lambda s: None
        try:
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
            self._native._set(ctx, "tok", "JWT")
            self._native._set(ctx, "dep_id_csc", 11)
            self._native._set(ctx, "dep_id_console", 22)
            r = self._native.step_10_install_poll(ctx)
        finally:
            _t.sleep = orig_sleep

        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "all_install_done_csc"))
        self.assertIn("running", r.detail)

    def test_step_10_fail_on_timeout(self) -> None:
        def fake_post_json(url, payload, token=None, timeout=10):
            return (202, {})
        self._csc_http.post_json = fake_post_json
        self._db.csp_db_config = lambda dist: {"Host": "x", "User": "x",
                                                "Password": "x", "DbName": "x"}

        class FakeCursor:
            def execute(self, sql, params=None): pass
            def fetchone(self): return ("deploying",)    # 영원히 deploying
        class FakeConn:
            def cursor(self): return FakeCursor()
            def close(self): pass
        self._db.connect = lambda cfg: FakeConn()

        import time as _t
        orig_sleep = _t.sleep
        _t.sleep = lambda s: None
        try:
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
            self._native._set(ctx, "tok", "JWT")
            self._native._set(ctx, "dep_id_csc", 11)
            r = self._native.step_10_install_poll(ctx)
        finally:
            _t.sleep = orig_sleep

        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("all_done=False", r.detail)
        self.assertFalse(self._native._get(ctx, "all_install_done_csc"))

    # ── 합성 함수 (steps_09_10) ──
    def test_steps_09_10_composite(self) -> None:
        """step 09 SKIP (no aid) → step 10 도 dep 없어 FAIL → composite=FAIL."""
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok", "JWT")
        # aid_csc 미설정 → step 09 SKIP, step 10 도 dep 없어 FAIL
        result = self._native.steps_09_10_install(ctx)
        self.assertEqual(result.id, "S5-CSC-DEPLOY-INSTALL")
        self.assertEqual(result.status, self._ItemStatus.FAIL)
        self.assertEqual(len(result.children), 2)


class TestStage5CscVerifySteps(unittest.TestCase):
    """S5 native — step 11 (file verify), 12 (overlay verify)."""

    def setUp(self) -> None:
        from verify.lib.items.stage5 import _native_steps
        from verify.lib.context import VerifyContext
        from verify.lib.registry import ItemStatus
        self._native = _native_steps
        self._VerifyContext = VerifyContext
        self._ItemStatus = ItemStatus

    def _ctx_with_dist(self, dist_dir):
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        ctx.dist_dir = dist_dir
        return ctx

    # ── step 11 ──
    def test_step_11_pass_when_all_files_exist(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            for name in ("csc", "console"):
                base = os.path.join(td, "csc-server", name)
                os.makedirs(os.path.join(base, "config"))
                with open(os.path.join(base, "meta.json"), "w") as f:
                    f.write('{"name": "' + name + '"}')
            ctx = self._ctx_with_dist(td)
            r = self._native.step_11_verify_files(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("csc: meta.json + config/ 존재", r.detail)
        self.assertIn("console: meta.json + config/ 존재", r.detail)

    def test_step_11_fail_when_meta_missing(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            # csc 만 정상, console 의 meta.json 누락
            os.makedirs(os.path.join(td, "csc-server", "csc", "config"))
            with open(os.path.join(td, "csc-server", "csc", "meta.json"), "w") as f:
                f.write("{}")
            os.makedirs(os.path.join(td, "csc-server", "console", "config"))
            # console/meta.json 만들지 않음
            ctx = self._ctx_with_dist(td)
            r = self._native.step_11_verify_files(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("console: 누락 meta.json", r.detail)

    def test_step_11_fail_when_config_dir_missing(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            for name in ("csc", "console"):
                base = os.path.join(td, "csc-server", name)
                os.makedirs(base)
                with open(os.path.join(base, "meta.json"), "w") as f:
                    f.write("{}")
                # config/ 디렉토리 만들지 않음
            ctx = self._ctx_with_dist(td)
            r = self._native.step_11_verify_files(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("누락 config/", r.detail)

    def test_step_11_cached(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ctx = self._ctx_with_dist(td)
            r1 = self._native.step_11_verify_files(ctx)
            r2 = self._native.step_11_verify_files(ctx)
        self.assertIs(r1, r2)

    # ── step 12 ──
    def test_step_12_pass_with_flat_key(self) -> None:
        import tempfile
        import json as _json
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "csc-server", "csc")
            os.makedirs(base)
            with open(os.path.join(base, "config.json"), "w") as f:
                _json.dump({"Server.Port": 4445, "other": 1}, f)
            ctx = self._ctx_with_dist(td)
            r = self._native.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("Server.Port=4445", r.detail)

    def test_step_12_pass_with_nested_key(self) -> None:
        import tempfile
        import json as _json
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "csc-server", "csc")
            os.makedirs(base)
            with open(os.path.join(base, "config.json"), "w") as f:
                _json.dump({"Server": {"Port": 4445}}, f)
            ctx = self._ctx_with_dist(td)
            r = self._native.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)

    def test_step_12_fail_wrong_port(self) -> None:
        import tempfile
        import json as _json
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "csc-server", "csc")
            os.makedirs(base)
            with open(os.path.join(base, "config.json"), "w") as f:
                _json.dump({"Server.Port": 4444}, f)
            ctx = self._ctx_with_dist(td)
            r = self._native.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("실제=4444", r.detail)

    def test_step_12_fail_missing_config(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ctx = self._ctx_with_dist(td)
            r = self._native.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("실제=None", r.detail)

    def test_step_12_fail_malformed_json(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "csc-server", "csc")
            os.makedirs(base)
            with open(os.path.join(base, "config.json"), "w") as f:
                f.write("not-json{")
            ctx = self._ctx_with_dist(td)
            r = self._native.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)


class TestStage5CscRunSteps(unittest.TestCase):
    """S5 native — step 13 (csc start), 14 (csc health), 15 (console start)."""

    def setUp(self) -> None:
        from verify.lib.items.stage5 import _native_steps
        from verify.lib.context import VerifyContext
        from verify.lib.common import csc_http
        from verify.lib.common import db as _db
        from verify.lib import shell as _shell
        from verify.lib.registry import ItemStatus
        self._native = _native_steps
        self._VerifyContext = VerifyContext
        self._csc_http = csc_http
        self._db = _db
        self._shell = _shell
        self._ItemStatus = ItemStatus
        self._orig = {
            "post_json":     csc_http.post_json,
            "csp_db_config": _db.csp_db_config,
            "connect":       _db.connect,
            "port_listening": _shell.port_listening,
        }
        # time.sleep 빠르게
        import time as _t
        self._orig_sleep = _t.sleep
        _t.sleep = lambda s: None

    def tearDown(self) -> None:
        self._csc_http.post_json   = self._orig["post_json"]
        self._db.csp_db_config     = self._orig["csp_db_config"]
        self._db.connect           = self._orig["connect"]
        self._shell.port_listening = self._orig["port_listening"]
        import time as _t
        _t.sleep = self._orig_sleep

    def _ctx(self):
        return self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)

    # ── step 13 ──
    def test_step_13_skips_without_tok_or_did(self) -> None:
        ctx = self._ctx()
        r = self._native.step_13_csc_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_13_pass_when_listening(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        # 첫 polling 시도에서 즉시 LISTEN
        self._shell.port_listening = lambda port, proto="tcp": port == 4445
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        r = self._native.step_13_csc_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "csc_start_ok"))

    def test_step_13_fail_on_listen_timeout(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        self._shell.port_listening = lambda port, proto="tcp": False
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        r = self._native.step_13_csc_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertFalse(self._native._get(ctx, "csc_start_ok"))

    def test_step_13_fail_on_post_status_error(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (500, {})
        self._shell.port_listening = lambda port, proto="tcp": True
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        r = self._native.step_13_csc_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("status=500", r.detail)

    # ── step 14 ──
    def test_step_14_skips_when_csc_not_started(self) -> None:
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        # csc_start_ok 미설정 (False)
        r = self._native.step_14_csc_health(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_14_pass_when_health_succeeds(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {"job_id": 99})
        self._db.csp_db_config = lambda d: {"Host":"x","User":"x","Password":"x","DbName":"x"}
        class FakeCursor:
            def execute(self, sql, params=None): pass
            def fetchone(self):
                # status=succeeded, rc=0, stdout 안 'tcp:4445=open'
                return ("succeeded", 0, "tcp:4445=open\n")
        class FakeConn:
            def cursor(self): return FakeCursor()
            def close(self): pass
        self._db.connect = lambda cfg: FakeConn()

        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        self._native._set(ctx, "csc_start_ok", True)
        r = self._native.step_14_csc_health(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "csc_health_ok"))

    def test_step_14_fail_when_stdout_missing_tcp_open(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {"job_id": 99})
        self._db.csp_db_config = lambda d: {"Host":"x","User":"x","Password":"x","DbName":"x"}
        class FakeCursor:
            def execute(self, sql, params=None): pass
            def fetchone(self):
                return ("succeeded", 0, "")    # stdout 비어있음
        class FakeConn:
            def cursor(self): return FakeCursor()
            def close(self): pass
        self._db.connect = lambda cfg: FakeConn()

        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        self._native._set(ctx, "csc_start_ok", True)
        r = self._native.step_14_csc_health(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertFalse(self._native._get(ctx, "csc_health_ok"))

    def test_step_14_fail_when_post_missing_job_id(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (200, {})
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        self._native._set(ctx, "csc_start_ok", True)
        r = self._native.step_14_csc_health(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("job_id 누락", r.detail)

    # ── step 15 ──
    def test_step_15_pass_when_listening(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        self._shell.port_listening = lambda port, proto="tcp": port == 8081
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_console", 22)
        r = self._native.step_15_console_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "console_start_ok"))

    def test_step_15_fail_on_listen_timeout(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        self._shell.port_listening = lambda port, proto="tcp": False
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_console", 22)
        r = self._native.step_15_console_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)


class TestRunStore(unittest.TestCase):
    """verify.lib.run_store — 파일 기반 회차 이력 store.

    write_run / get_run / list_runs / delete_run / stats. tempdir 만 사용,
    DB 의존 X.
    """

    def setUp(self) -> None:
        from verify.lib import run_store
        import tempfile
        self._rs = run_store
        self._td = tempfile.mkdtemp(prefix="run_store_test_")

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self._td, ignore_errors=True)

    def _make_record(self, scope="stage5", verdict="PASS", elapsed_ms=1000):
        return {
            "id": 0,
            "started_at": "2026-05-07T12:00:00.000",
            "finished_at": "2026-05-07T12:00:01.000",
            "elapsed_ms": elapsed_ms,
            "trigger": "user",
            "scope": scope,
            "selected_ids": ["S5-RESET"],
            "verdict": verdict,
            "totals": {"total": 14, "pass": 12, "fail": 0, "skip": 2, "blocked": 0},
            "pkg_manifest_hash": "abc123",
            "git_branch": "main", "git_sha": "abc1234", "host": "test-host",
            "ens_ip": "", "report_path": "/tmp/x", "job_id": "uuid-1", "note": "",
            "items": [
                {"id": "S5-RESET", "stage": 5, "parent_id": None, "is_group": False,
                 "name": "reset", "status": "PASS", "elapsed_ms": 100, "detail": "", "idx": 1},
            ],
        }

    def test_write_and_get_roundtrip(self) -> None:
        rec = self._make_record()
        rid = self._rs.write_run(self._td, rec)
        self.assertGreater(rid, 0)
        # 파일 존재
        path = self._rs._path_for_id(self._td, rid)
        self.assertTrue(os.path.isfile(path))
        # get_run 으로 조회
        loaded = self._rs.get_run(self._td, rid)
        self.assertIsNotNone(loaded)
        self.assertEqual(loaded["id"], rid)
        self.assertEqual(loaded["scope"], "stage5")
        self.assertEqual(loaded["verdict"], "PASS")
        self.assertEqual(len(loaded["items"]), 1)

    def test_unique_ids_on_collision(self) -> None:
        # 동일 ms 에 두 번 write — id 가 +1 증가
        rid1 = self._rs.write_run(self._td, self._make_record())
        rid2 = self._rs.write_run(self._td, self._make_record())
        self.assertNotEqual(rid1, rid2)

    def test_list_runs_filters(self) -> None:
        # 5 회차: 3 stage5 (2 PASS + 1 FAIL) + 2 stage1 (PASS)
        for verdict in ("PASS", "PASS", "FAIL"):
            self._rs.write_run(self._td, self._make_record(scope="stage5", verdict=verdict))
        for _ in range(2):
            self._rs.write_run(self._td, self._make_record(scope="stage1"))
        # 전체
        total, rows = self._rs.list_runs(self._td)
        self.assertEqual(total, 5)
        self.assertEqual(len(rows), 5)
        # items 는 list 응답에 없어야 함
        self.assertNotIn("items", rows[0])
        # stage 필터
        total, rows = self._rs.list_runs(self._td, stage=5)
        self.assertEqual(total, 3)
        # verdict 필터
        total, rows = self._rs.list_runs(self._td, verdict="FAIL")
        self.assertEqual(total, 1)
        # scope + verdict 동시
        total, rows = self._rs.list_runs(self._td, stage=5, verdict="PASS")
        self.assertEqual(total, 2)

    def test_list_runs_paging_and_order(self) -> None:
        ids = [self._rs.write_run(self._td, self._make_record()) for _ in range(7)]
        # limit=3 → 최신 3개 (DESC)
        total, rows = self._rs.list_runs(self._td, limit=3)
        self.assertEqual(total, 7)
        self.assertEqual([r["id"] for r in rows], sorted(ids, reverse=True)[:3])
        # offset=3, limit=2 → 4번째~5번째 (DESC)
        total, rows = self._rs.list_runs(self._td, limit=2, offset=3)
        self.assertEqual([r["id"] for r in rows], sorted(ids, reverse=True)[3:5])

    def test_delete_run(self) -> None:
        rid = self._rs.write_run(self._td, self._make_record())
        self.assertTrue(self._rs.delete_run(self._td, rid))
        self.assertIsNone(self._rs.get_run(self._td, rid))
        # 없는 id
        self.assertFalse(self._rs.delete_run(self._td, 999999999))

    def test_purge_older_than_basic(self) -> None:
        """오래된 회차 삭제 + keep_min 보장 + 빈 디렉토리 정리."""
        import time as _t
        # 5 회차: 3 개는 100일 전, 2 개는 최근 (방금)
        old_ms = int((_t.time() - 100 * 86400) * 1000)
        for offset in range(3):
            rec = self._make_record()
            rec["id"] = 0    # write_run 가 새 id 할당하지만, 명시적 path 작성
            # write_run 은 _next_id 사용 → 항상 현재 시간 기반.
            # 오래된 회차를 시뮬하려면 직접 파일 생성.
            old_id = old_ms + offset
            path = self._rs._path_for_id(self._td, old_id)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            rec["id"] = old_id
            with open(path, "w") as f:
                import json as _json
                _json.dump(rec, f)
        # 최근 2 개
        recent_ids = [self._rs.write_run(self._td, self._make_record())
                      for _ in range(2)]

        # 90일 retention, keep_min=0
        summary = self._rs.purge_older_than(self._td, days=90, keep_min=0)
        self.assertEqual(len(summary["deleted"]), 3)
        self.assertEqual(summary["kept"], 2)
        self.assertGreater(summary["freed_bytes"], 0)

        # 최근 2개는 살아남음
        for rid in recent_ids:
            self.assertIsNotNone(self._rs.get_run(self._td, rid))

    def test_purge_keep_min_protects_recent(self) -> None:
        """keep_min 으로 오래된 회차도 최근 N 개는 보존."""
        import time as _t
        old_ms = int((_t.time() - 100 * 86400) * 1000)
        # 5 회차 모두 100일 전 (기본은 모두 삭제 대상)
        for offset in range(5):
            rec = self._make_record()
            rid = old_ms + offset
            rec["id"] = rid
            path = self._rs._path_for_id(self._td, rid)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                import json as _json
                _json.dump(rec, f)

        # keep_min=2 → 최근 2개 (높은 id) 는 무조건 보존
        summary = self._rs.purge_older_than(self._td, days=90, keep_min=2)
        self.assertEqual(len(summary["deleted"]), 3)
        self.assertEqual(summary["kept"], 2)

    def test_purge_zero_days_deletes_all(self) -> None:
        """days=0 이면 모든 회차 삭제 (단 keep_min 적용)."""
        ids = [self._rs.write_run(self._td, self._make_record()) for _ in range(3)]
        summary = self._rs.purge_older_than(self._td, days=0, keep_min=0)
        self.assertEqual(len(summary["deleted"]), 3)
        # 빈 디렉토리도 정리됐는지
        self.assertGreater(len(summary["removed_dirs"]), 0)

    def test_stats_shape(self) -> None:
        for v, e in (("PASS", 1000), ("PASS", 2000), ("FAIL", 5000)):
            self._rs.write_run(self._td, self._make_record(scope="stage5",
                                                            verdict=v, elapsed_ms=e))
        for v, e in (("PASS", 500), ("PASS", 600)):
            self._rs.write_run(self._td, self._make_record(scope="stage1",
                                                            verdict=v, elapsed_ms=e))
        st = self._rs.stats(self._td, days=30)
        self.assertEqual(st["overall"]["runs"], 5)
        self.assertEqual(st["overall"]["pass"], 4)
        self.assertEqual(st["overall"]["fail"], 1)
        self.assertEqual(st["overall"]["success_rate"], 80.0)
        # by_scope — 정렬 (alphabetical)
        scopes = [s["scope"] for s in st["by_scope"]]
        self.assertEqual(scopes, ["stage1", "stage5"])
        s5 = next(s for s in st["by_scope"] if s["scope"] == "stage5")
        self.assertEqual(s5["runs"], 3)
        self.assertEqual(s5["pass"], 2)
        # timeline — ASC (오래된 → 최신)
        self.assertEqual(len(st["timeline"]), 5)
        ts = [t["id"] for t in st["timeline"]]
        self.assertEqual(ts, sorted(ts))    # ASC


class TestStage5ModulesSteps(unittest.TestCase):
    """S5 native — step 16~22 (modules + finalize)."""

    def setUp(self) -> None:
        from verify.lib.items.stage5 import _native_steps
        from verify.lib.context import VerifyContext
        from verify.lib.common import csc_http
        from verify.lib.common import db as _db
        from verify.lib.common import pkg_manifest as _pkgm
        from verify.lib import shell as _shell
        from verify.lib.registry import ItemStatus
        self._native = _native_steps
        self._VerifyContext = VerifyContext
        self._csc_http = csc_http
        self._db = _db
        self._pkgm = _pkgm
        self._shell = _shell
        self._ItemStatus = ItemStatus
        self._orig = {
            "admin_login":     csc_http.admin_login,
            "post_json":       csc_http.post_json,
            "post_multipart":  csc_http.post_multipart,
            "delete":          csc_http.delete,
            "find_by_name":    csc_http.find_agent_id_by_name,
            "csp_db_config":   _db.csp_db_config,
            "connect":         _db.connect,
            "port_listening":  _shell.port_listening,
            "write_marker":    _pkgm.write_marker,
        }
        import time as _t
        self._orig_sleep = _t.sleep
        _t.sleep = lambda s: None

    def tearDown(self) -> None:
        self._csc_http.admin_login           = self._orig["admin_login"]
        self._csc_http.post_json             = self._orig["post_json"]
        self._csc_http.post_multipart        = self._orig["post_multipart"]
        self._csc_http.delete                = self._orig["delete"]
        self._csc_http.find_agent_id_by_name = self._orig["find_by_name"]
        self._db.csp_db_config               = self._orig["csp_db_config"]
        self._db.connect                     = self._orig["connect"]
        self._shell.port_listening           = self._orig["port_listening"]
        self._pkgm.write_marker              = self._orig["write_marker"]
        import time as _t
        _t.sleep = self._orig_sleep

    def _ctx_with_dist(self, dist):
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        ctx.dist_dir = dist
        return ctx

    # ── step 16 ──
    def test_step_16_skips_when_csc_not_started(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        r = self._native.step_16_modules_auth(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_16_pass(self) -> None:
        called = []
        def fake_login(base, lid, pw, timeout=5):
            called.append(base)
            return "JWT2-XYZ"
        self._csc_http.admin_login = fake_login
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "csc_start_ok", True)
        r = self._native.step_16_modules_auth(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        # 배포본 csc(4445) 로 호출됐는지
        self.assertIn("4445", called[0])
        self.assertEqual(self._native._get(ctx, "tok2"), "JWT2-XYZ")

    def test_step_16_fail_login(self) -> None:
        self._csc_http.admin_login = lambda *a, **k: ""
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "csc_start_ok", True)
        r = self._native.step_16_modules_auth(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)

    # ── step 17 ──
    def test_step_17_pass_with_3_modules(self) -> None:
        import tempfile
        captured: list = []
        def fake_post_multipart(url, *, file_path, file_field="file",
                                filename=None, form_fields=None,
                                token=None, timeout=60):
            captured.append(file_path)
            base = os.path.basename(file_path)
            if base.startswith("csp-"): pid = 11
            elif base.startswith("cmp-"): pid = 12
            elif base.startswith("cspsim-"): pid = 13
            else: pid = 99
            return (201, {"id": pid})
        self._csc_http.post_multipart = fake_post_multipart

        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            for fn in ("csp-1.0.0.tar.gz", "cmp-1.0.0.tar.gz", "cspsim-1.0.0.tar.gz"):
                with open(os.path.join(pkg_dir, fn), "w") as f: f.write("d")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok2", "JWT2")
            r = self._native.step_17_modules_pkg_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "pkg2_id_csp"), 11)
        self.assertEqual(self._native._get(ctx, "pkg2_id_cmp"), 12)
        # sim 의 tarball prefix 는 cspsim
        self.assertEqual(self._native._get(ctx, "pkg2_id_sim"), 13)

    def test_step_17_fail_sim_tarball_missing(self) -> None:
        import tempfile
        self._csc_http.post_multipart = lambda u, **k: (201, {"id": 1})
        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            # cspsim tarball 만 누락
            for fn in ("csp-1.0.0.tar.gz", "cmp-1.0.0.tar.gz"):
                with open(os.path.join(pkg_dir, fn), "w") as f: f.write("d")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok2", "JWT2")
            r = self._native.step_17_modules_pkg_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("sim: tarball 없음", r.detail)
        self.assertIn("cspsim-*.tar.gz", r.detail)

    # ── step 18 ──
    def test_step_18_skips_without_tok2(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        r = self._native.step_18_modules_agent_enroll(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_18_pass(self) -> None:
        # 3 모듈 모두 register + spawn + online 성공 시뮬
        post_called: list = []
        def fake_post_json(url, payload, token=None, timeout=10):
            post_called.append(url)
            if url.endswith("/agents"):
                # name 별 다른 id
                name = payload["name"]
                aid = 100 if "csp" in name else 200 if "cmp" in name else 300
                return (201, {"id": aid, "enrollment_token": f"ENR-{aid}"})
            if url.endswith("/approve"):
                return (200, {})
            return (404, {})
        self._csc_http.post_json = fake_post_json
        self._csc_http.find_agent_id_by_name = lambda *a, **k: None
        self._csc_http.delete = lambda *a, **k: 204

        # spawn 모킹 — 실제 Popen 안 함
        spawned: list = []
        def fake_spawn(ctx, m, base, aname, enroll_tok):
            spawned.append((m, aname))
            return (1000 + (1 if m == "csp" else 2 if m == "cmp" else 3), "")
        self._native._spawn_one_module_agent = fake_spawn

        # _all_modules_online 즉시 True
        orig_all = self._native._all_modules_online
        self._native._all_modules_online = lambda dist: True

        try:
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
            self._native._set(ctx, "tok2", "JWT2")
            r = self._native.step_18_modules_agent_enroll(ctx)
        finally:
            self._native._all_modules_online = orig_all
            # spawn 복구는 monkey patch 였으므로 module attr 제거
            try: delattr(self._native, "_spawn_one_module_agent")
            except AttributeError: pass

        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "aid_csp"), 100)
        self.assertEqual(self._native._get(ctx, "aid_cmp"), 200)
        self.assertEqual(self._native._get(ctx, "aid_sim"), 300)
        self.assertEqual(self._native._get(ctx, "ta_pid_csp"), 1001)
        self.assertEqual(len(spawned), 3)

    # ── step 19 ──
    def test_step_19_pass(self) -> None:
        captured: list = []
        def fake_post_json(url, payload, token=None, timeout=15):
            captured.append(payload)
            mod = payload["process_name"]
            did = 11 if mod == "CSP" else 12 if mod == "CMP" else 13
            return (201, {"id": did})
        self._csc_http.post_json = fake_post_json

        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        for m, aid, pid in [("csp", 100, 11), ("cmp", 200, 12), ("sim", 300, 13)]:
            self._native._set(ctx, f"aid_{m}", aid)
            self._native._set(ctx, f"pkg2_id_{m}", pid)
        r = self._native.step_19_modules_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "dep2_id_csp"), 11)
        self.assertEqual(self._native._get(ctx, "dep2_id_sim"), 13)
        # sim 의 process_name 은 CSPSIM
        sim_payload = next(p for p in captured if p["process_name"] == "CSPSIM")
        self.assertIsNotNone(sim_payload)

    def test_step_19_fail_missing_pkg_for_one_module(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=15: (201, {"id": 1})
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        # csp/cmp 만 ready, sim 의 pkg_id 누락
        for m in ("csp", "cmp"):
            self._native._set(ctx, f"aid_{m}", 100)
            self._native._set(ctx, f"pkg2_id_{m}", 1)
        self._native._set(ctx, "aid_sim", 300)
        # pkg2_id_sim 미설정
        r = self._native.step_19_modules_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("sim:", r.detail)

    # ── step 20 ──
    def test_step_20_pass_when_all_succeed(self) -> None:
        # PASS 조건: 폴링 종료 시 모든 deployment status ∈ {running, stopped}.
        # sim 은 install-only 라 stopped, csp/cmp 는 install 후 stopped 또는 running.
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        self._db.csp_db_config = lambda d: {"Host": "x", "User": "x",
                                             "Password": "x", "DbName": "x"}
        class FakeCursor:
            def execute(self, sql, params=None): pass
            def fetchone(self): return ("stopped",)
        class FakeConn:
            def cursor(self): return FakeCursor()
            def close(self): pass
        self._db.connect = lambda cfg: FakeConn()

        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        for m, did in [("csp", 11), ("cmp", 12), ("sim", 13)]:
            self._native._set(ctx, f"dep2_id_{m}", did)
        r = self._native.step_20_modules_install_poll(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "all_install_done_modules"))

    # ── step 21 ──
    def test_step_21_pass(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        # 5060/udp + 9000/udp 모두 LISTEN
        def fake_listen(port, proto="tcp"):
            return port in (5060, 9000) and proto == "udp"
        self._shell.port_listening = fake_listen
        marker_called = [0]
        def fake_marker(dist):
            marker_called[0] += 1
            return "abc123def456abc123def456abc123def456abc123def456abc123def456ab"
        self._pkgm.write_marker = fake_marker

        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        for m, did in [("csp", 11), ("cmp", 12)]:
            self._native._set(ctx, f"dep2_id_{m}", did)
        r = self._native.step_21_modules_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "modules_start_ok"))
        # immutability marker 기록됐는지
        self.assertEqual(marker_called[0], 1)
        self.assertIn(".deployed-manifest.json", r.detail)

    def test_step_21_fail_when_cmp_not_listening(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        # csp 는 LISTEN, cmp 는 timeout
        self._shell.port_listening = lambda port, proto="tcp": port == 5060
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        for m, did in [("csp", 11), ("cmp", 12)]:
            self._native._set(ctx, f"dep2_id_{m}", did)
        r = self._native.step_21_modules_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertFalse(self._native._get(ctx, "modules_start_ok"))

    # ── step 22 ──
    def test_step_22_default_keep_running(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        # ctx.stop_after = False (default)
        r = self._native.step_22_finalize(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("기동 유지", r.detail)

    def test_step_22_stop_after_kills_agents(self) -> None:
        # stop_after=True 시 stop jobs + kill
        post_calls: list = []
        def fake_post_json(url, payload, token=None, timeout=10):
            post_calls.append((url, payload))
            return (202, {})
        self._csc_http.post_json = fake_post_json

        # os.kill 모킹
        import os as _os
        kill_calls: list = []
        orig_kill = _os.kill
        _os.kill = lambda pid, sig: kill_calls.append((pid, sig))

        try:
            ctx = self._VerifyContext.create(
                repo_root=_REPO_ROOT, stage=5, opts={"stop_after": True},
            )
            # 모든 deployment + Test-agent pid 설정
            self._native._set(ctx, "tok", "JWT")
            self._native._set(ctx, "tok2", "JWT2")
            self._native._set(ctx, "dep_id_csc", 1)
            self._native._set(ctx, "dep_id_console", 2)
            self._native._set(ctx, "dep2_id_csp", 11)
            self._native._set(ctx, "dep2_id_cmp", 12)
            self._native._set(ctx, "ta_pid_csc", 1000)
            self._native._set(ctx, "ta_pid_csp", 1001)
            self._native._set(ctx, "ta_pid_cmp", 1002)
            self._native._set(ctx, "ta_pid_sim", 1003)
            r = self._native.step_22_finalize(ctx)
        finally:
            _os.kill = orig_kill

        self.assertEqual(r.status, self._ItemStatus.PASS)
        # csc/console (TB-CSC) + csp/cmp (배포본 csc) = 4 stop 발행
        self.assertEqual(len(post_calls), 4)
        # 4 Test-agent kill
        self.assertEqual(len(kill_calls), 4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
