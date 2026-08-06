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

    def test_stage6_new_scenarios_registered(self) -> None:
        """S6 신규 + depth 시나리오 6개 등록 + SUMMARY depends_on 매칭."""
        from verify.lib import registry
        from verify.lib import items                            # noqa: F401
        ids = {m.id for m in registry.get_items(stage=6)}
        for new_id in (
            "S6-SCN-SUBSCRIBE", "S6-SCN-CERT-ROTATE", "S6-SCN-DB-SYNC",
            "S6-L7-SUBSCRIBE-NOTIFY", "S6-CMP-GROUP-SYNC", "S6-MCPTT-FLOOR-GRANT",
        ):
            self.assertIn(new_id, ids)
        # SUMMARY 가 새 시나리오 + depth 항목들에도 의존
        rec = registry.get_item("S6-SUMMARY")
        self.assertIsNotNone(rec)
        deps = set(rec[0].depends_on)
        for dep_id in (
            "S6-SCN-SUBSCRIBE", "S6-SCN-CERT-ROTATE", "S6-SCN-DB-SYNC",
            "S6-L7-SUBSCRIBE-NOTIFY", "S6-CMP-GROUP-SYNC", "S6-MCPTT-FLOOR-GRANT",
        ):
            self.assertIn(dep_id, deps)

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


class TestStage6NewScenarios(unittest.TestCase):
    """S6-SCN-SUBSCRIBE / -CERT-ROTATE / -DB-SYNC unit tests (mock 기반)."""

    def setUp(self) -> None:
        from verify.lib.context import VerifyContext
        from verify.lib.registry import ItemStatus
        self._VerifyContext = VerifyContext
        self._ItemStatus = ItemStatus

    def _ctx(self, dist_dir=None):
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=6)
        if dist_dir:
            ctx.dist_dir = dist_dir
        return ctx

    # ── S6-SCN-SUBSCRIBE ──
    def test_scn_subscribe_skip_without_ptt_state(self) -> None:
        from verify.lib.items.stage6 import scn_subscribe
        ctx = self._ctx()
        # PTT_USER 등 미설정 (S6-SEED 안 돌렸을 때)
        r = scn_subscribe.scn_subscribe(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("PTT 가입자", r.detail)

    def test_scn_subscribe_pass_when_complete(self) -> None:
        from verify.lib.items.stage6 import scn_subscribe
        # scn_subscribe 모듈이 `from ...common.cspsim import run_cspsim` 했으므로
        # 모듈 자체의 reference 를 patch 해야 효과 있음.
        orig = scn_subscribe.run_cspsim
        orig_cnt = scn_subscribe._count_notify_lines
        try:
            scn_subscribe.run_cspsim = lambda repo, args, timeout=120, tail_lines=100: (
                0, "[Scenario] Sending GMS/CMS SUBSCRIBE...\n"
                   "[Scenario] Subscriptions complete\n",
            )
            scn_subscribe._count_notify_lines = lambda dist, since: 2
            ctx = self._ctx()
            ctx.state.update({"PTT_USER": "u", "PTT_DOM": "d", "PTT_PWD": "p"})
            r = scn_subscribe.scn_subscribe(ctx)
        finally:
            scn_subscribe.run_cspsim = orig
            scn_subscribe._count_notify_lines = orig_cnt
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("subscribe-complete=True", r.detail)

    def test_entry_check_required_ports_target_prod(self) -> None:
        """target=prod → _required_ports csc/console 4420/80 + 시그널링/미디어 인스턴스."""
        from verify.lib.items.stage6 import entry_check
        ctx = self._VerifyContext.create(
            repo_root=_REPO_ROOT, stage=6, opts={"target": "prod"},
        )
        ports = entry_check._required_ports(ctx)
        # entry: (port, proto, host, label)
        triples = {(p, proto) for p, proto, _h, _l in ports}
        self.assertIn((4420, "tcp"), triples)
        self.assertIn((80,   "tcp"), triples)
        # csp/cmp + psp/pmp 는 두 환경 동일 (포트는 같고 host 만 다름)
        self.assertIn((5060, "udp"), triples)
        self.assertIn((9000, "udp"), triples)
        # P1 토폴로지 — 시그널링 2 인스턴스 (csp 127.0.0.1, psp 127.0.0.3) 모두 등재
        sip_hosts = {h for p, proto, h, _l in ports if p == 5060}
        self.assertIn("127.0.0.1", sip_hosts)
        self.assertIn("127.0.0.3", sip_hosts)
        media_hosts = {h for p, proto, h, _l in ports if p == 9000}
        self.assertIn("127.0.0.1", media_hosts)
        self.assertIn("127.0.0.3", media_hosts)

    def test_entry_check_required_ports_target_verify_default(self) -> None:
        from verify.lib.items.stage6 import entry_check
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=6)
        ports = entry_check._required_ports(ctx)
        triples = {(p, proto) for p, proto, _h, _l in ports}
        self.assertIn((4445, "tcp"), triples)
        self.assertIn((8081, "tcp"), triples)

    def test_scn_db_sync_deployed_base_target_prod(self) -> None:
        from verify.lib.items.stage6 import scn_db_sync
        ctx = self._VerifyContext.create(
            repo_root=_REPO_ROOT, stage=6, opts={"target": "prod"},
        )
        self.assertEqual(scn_db_sync._deployed_csc_base(ctx),
                         "https://127.0.0.1:4420")

    def test_scn_subscribe_pass_when_notify_missing_but_marker_present(self) -> None:
        """NOTIFY 라인 0 이어도 SUBSCRIBE 마커 있으면 PASS — msg_log 비활성 환경 대응."""
        from verify.lib.items.stage6 import scn_subscribe
        orig = scn_subscribe.run_cspsim
        orig_cnt = scn_subscribe._count_notify_lines
        try:
            scn_subscribe.run_cspsim = lambda repo, args, timeout=120, tail_lines=100: (
                0, "[Scenario] Subscriptions complete\n",
            )
            scn_subscribe._count_notify_lines = lambda dist, since: 0
            ctx = self._ctx()
            ctx.state.update({"PTT_USER": "u", "PTT_DOM": "d", "PTT_PWD": "p"})
            r = scn_subscribe.scn_subscribe(ctx)
        finally:
            scn_subscribe.run_cspsim = orig
            scn_subscribe._count_notify_lines = orig_cnt
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("msg_log 가 비활성", r.detail)

    def test_scn_subscribe_fail_when_marker_missing(self) -> None:
        """SUBSCRIBE / Subscriptions 마커 둘 다 없으면 FAIL (cspsim 시나리오 미진행)."""
        from verify.lib.items.stage6 import scn_subscribe
        orig = scn_subscribe.run_cspsim
        orig_cnt = scn_subscribe._count_notify_lines
        try:
            scn_subscribe.run_cspsim = lambda repo, args, timeout=120, tail_lines=100: (
                0, "[Sim] register only — no scenario marker\n",
            )
            scn_subscribe._count_notify_lines = lambda dist, since: 0
            ctx = self._ctx()
            ctx.state.update({"PTT_USER": "u", "PTT_DOM": "d", "PTT_PWD": "p"})
            r = scn_subscribe.scn_subscribe(ctx)
        finally:
            scn_subscribe.run_cspsim = orig
            scn_subscribe._count_notify_lines = orig_cnt
        self.assertEqual(r.status, self._ItemStatus.FAIL)

    # ── S6-SCN-CERT-ROTATE ──
    def test_scn_cert_rotate_skip_when_mtls_off(self) -> None:
        from verify.lib.items.stage6 import scn_cert_rotate
        import tempfile, json as _json
        with tempfile.TemporaryDirectory() as td:
            cfg = os.path.join(td, "csc", "config")
            os.makedirs(cfg)
            with open(os.path.join(cfg, "csc-tb.json"), "w") as f:
                _json.dump({"Agent": {"MtlsEnabled": False}}, f)
            ctx = self._ctx(dist_dir=td)
            r = scn_cert_rotate.scn_cert_rotate(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("MtlsEnabled=false", r.detail)

    def test_scn_cert_rotate_skip_when_csc_tb_missing(self) -> None:
        from verify.lib.items.stage6 import scn_cert_rotate
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ctx = self._ctx(dist_dir=td)
            r = scn_cert_rotate.scn_cert_rotate(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("csc-tb.json", r.detail)

    def test_scn_cert_rotate_skip_when_db_unavailable(self) -> None:
        from verify.lib.items.stage6 import scn_cert_rotate
        from verify.lib.common import db as _db
        import tempfile, json as _json
        # mTLS=true 인데 DB 접속 실패 → SKIP
        orig_cfg = _db.csp_db_config
        try:
            _db.csp_db_config = lambda d: {}    # 빈 config
            with tempfile.TemporaryDirectory() as td:
                cfg = os.path.join(td, "csc", "config")
                os.makedirs(cfg)
                with open(os.path.join(cfg, "csc-tb.json"), "w") as f:
                    _json.dump({"Agent": {"MtlsEnabled": True}}, f)
                ctx = self._ctx(dist_dir=td)
                r = scn_cert_rotate.scn_cert_rotate(ctx)
        finally:
            _db.csp_db_config = orig_cfg
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("DB", r.detail)

    # ── S6-SCN-DB-SYNC ──
    def test_scn_db_sync_skip_when_csc_login_fails(self) -> None:
        from verify.lib.items.stage6 import scn_db_sync
        from verify.lib.common import csc_http
        orig = csc_http.admin_login
        try:
            csc_http.admin_login = lambda *a, **k: ""    # login 실패
            ctx = self._ctx()
            r = scn_db_sync.scn_db_sync(ctx)
        finally:
            csc_http.admin_login = orig
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("login", r.detail.lower())

    def test_scn_db_sync_pass_when_csp_log_has_notify(self) -> None:
        """admin login OK + 그룹 추가 + csp 로그에 GROUP_CHANGED 라인 → PASS."""
        from verify.lib.items.stage6 import scn_db_sync
        from verify.lib.common import csc_http
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            log_dir = os.path.join(td, "volte-sip-server", "csp", "log")
            os.makedirs(log_dir)
            log_path = os.path.join(log_dir, "csp_2026-05.log")
            # 사전: 빈 로그 (offset=0)
            with open(log_path, "w") as f:
                f.write("")

            orig_login = csc_http.admin_login
            orig_post = csc_http.post_json
            orig_delete = csc_http.delete
            try:
                csc_http.admin_login = lambda *a, **k: "JWT"
                # POST /ptt/groups → 201 + 잠시 후 csp 로그에 GROUP_CHANGED 라인 추가
                def fake_post(url, payload, **kw):
                    # 그룹 생성 시 csp 가 로그 라인 작성하는 것 시뮬
                    with open(log_path, "a") as f:
                        f.write("[INFO] GROUP_CHANGED uri=tel:test\n")
                    return (201, {"id": payload.get("group_id")})
                csc_http.post_json = fake_post
                csc_http.delete = lambda *a, **k: 204

                # time.sleep 빠르게
                import time as _t
                orig_sleep = _t.sleep
                _t.sleep = lambda s: None
                try:
                    ctx = self._ctx(dist_dir=td)
                    r = scn_db_sync.scn_db_sync(ctx)
                finally:
                    _t.sleep = orig_sleep
            finally:
                csc_http.admin_login = orig_login
                csc_http.post_json = orig_post
                csc_http.delete = orig_delete

        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("notify 라인: 1", r.detail)

    def test_scn_db_sync_fail_when_csp_log_silent(self) -> None:
        """admin login OK + 그룹 추가 + csp 로그에 새 라인 없음 → FAIL."""
        from verify.lib.items.stage6 import scn_db_sync
        from verify.lib.common import csc_http
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            log_dir = os.path.join(td, "volte-sip-server", "csp", "log")
            os.makedirs(log_dir)
            with open(os.path.join(log_dir, "csp_2026-05.log"), "w") as f:
                f.write("")
            orig_login = csc_http.admin_login
            orig_post = csc_http.post_json
            orig_delete = csc_http.delete
            try:
                csc_http.admin_login = lambda *a, **k: "JWT"
                csc_http.post_json = lambda *a, **k: (201, {"id": "x"})
                csc_http.delete = lambda *a, **k: 204
                import time as _t
                orig_sleep = _t.sleep
                _t.sleep = lambda s: None
                try:
                    ctx = self._ctx(dist_dir=td)
                    r = scn_db_sync.scn_db_sync(ctx)
                finally:
                    _t.sleep = orig_sleep
            finally:
                csc_http.admin_login = orig_login
                csc_http.post_json = orig_post
                csc_http.delete = orig_delete
        self.assertEqual(r.status, self._ItemStatus.FAIL)

    # ── S6-L7-SUBSCRIBE-NOTIFY ──
    def test_scn_l7_pass_when_xcap_diff_present(self) -> None:
        """NOTIFY body 에 <xcap-diff> 포함 → PASS."""
        from verify.lib.items.stage6 import scn_l7_subscribe_notify as mod
        notify_msg = (
            "NOTIFY sip:user@host SIP/2.0\r\n"
            "Event: presence\r\n"
            "Content-Type: application/xcap-diff+xml\r\n"
            "Content-Length: 80\r\n\r\n"
            "<?xml version=\"1.0\"?><xcap-diff>...</xcap-diff>"
        )
        orig = mod.iter_sip_msgs
        try:
            mod.iter_sip_msgs = (
                lambda dist_dir, *, since=0.0, method=None: iter([{"msg": notify_msg}])
            )
            ctx = self._ctx()
            r = mod.scn_l7_subscribe_notify(ctx)
        finally:
            mod.iter_sip_msgs = orig
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("xcap-diff", r.detail)

    def test_scn_l7_skip_when_no_notify(self) -> None:
        """iter_sip_msgs 빈 list → SKIP (msg_log 비활성)."""
        from verify.lib.items.stage6 import scn_l7_subscribe_notify as mod
        orig = mod.iter_sip_msgs
        try:
            mod.iter_sip_msgs = (
                lambda dist_dir, *, since=0.0, method=None: iter([])
            )
            ctx = self._ctx()
            r = mod.scn_l7_subscribe_notify(ctx)
        finally:
            mod.iter_sip_msgs = orig
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_scn_l7_fail_when_body_malformed(self) -> None:
        """NOTIFY body 가 unknown namespace + 비-XML → FAIL."""
        from verify.lib.items.stage6 import scn_l7_subscribe_notify as mod
        notify_msg = (
            "NOTIFY sip:u@h SIP/2.0\r\n"
            "Event: presence\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "this is not xml at all"
        )
        orig = mod.iter_sip_msgs
        try:
            mod.iter_sip_msgs = (
                lambda dist_dir, *, since=0.0, method=None: iter([{"msg": notify_msg}])
            )
            ctx = self._ctx()
            r = mod.scn_l7_subscribe_notify(ctx)
        finally:
            mod.iter_sip_msgs = orig
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("malformed", r.detail)

    # ── S6-CMP-GROUP-SYNC ──
    def test_scn_cmp_group_sync_pass_when_stats_has_gid(self) -> None:
        """admin OK + CMP STATS 응답 group_details 에 gid 포함 → PASS + cleanup."""
        from verify.lib.items.stage6 import scn_cmp_group_sync as mod
        from verify.lib.common import csc_http
        captured_gid: list = []
        delete_called: list = []

        def fake_post(url, payload, **kw):
            captured_gid.append(payload.get("id"))
            return (201, {"id": payload.get("id")})

        def fake_delete(url, **kw):
            delete_called.append(url)
            return 204

        def fake_cmp_request(payload, ip="127.0.0.1", port=9000, timeout=1.0):
            sesid = payload.get("sesid", "")
            if "precheck" in sesid:
                return {"response": {"groups": 0, "group_details": []}}
            gid = captured_gid[0] if captured_gid else "x"
            return {"response": {"groups": 1,
                                 "group_details": [{"group_id": gid, "members": 0}]}}

        orig_login = csc_http.admin_login
        orig_post = csc_http.post_json
        orig_delete = csc_http.delete
        orig_cmp = mod.cmp_request
        import time as _t
        orig_sleep = _t.sleep
        try:
            csc_http.admin_login = lambda *a, **k: "JWT"
            csc_http.post_json = fake_post
            csc_http.delete = fake_delete
            mod.cmp_request = fake_cmp_request
            _t.sleep = lambda s: None
            ctx = self._ctx()
            r = mod.scn_cmp_group_sync(ctx)
        finally:
            csc_http.admin_login = orig_login
            csc_http.post_json = orig_post
            csc_http.delete = orig_delete
            mod.cmp_request = orig_cmp
            _t.sleep = orig_sleep
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(len(delete_called), 1, "cleanup DELETE 호출 1회")

    def test_scn_cmp_group_sync_fail_when_stats_silent(self) -> None:
        """admin OK + CMP STATS 가 항상 빈 group_details → FAIL (5s 폴링 미발견)."""
        from verify.lib.items.stage6 import scn_cmp_group_sync as mod
        from verify.lib.common import csc_http
        orig_login = csc_http.admin_login
        orig_post = csc_http.post_json
        orig_delete = csc_http.delete
        orig_cmp = mod.cmp_request
        import time as _t
        orig_sleep = _t.sleep
        try:
            csc_http.admin_login = lambda *a, **k: "JWT"
            csc_http.post_json = lambda *a, **k: (201, {"id": "x"})
            csc_http.delete = lambda *a, **k: 204
            mod.cmp_request = lambda *a, **k: {
                "response": {"groups": 0, "group_details": []}
            }
            _t.sleep = lambda s: None
            ctx = self._ctx()
            r = mod.scn_cmp_group_sync(ctx)
        finally:
            csc_http.admin_login = orig_login
            csc_http.post_json = orig_post
            csc_http.delete = orig_delete
            mod.cmp_request = orig_cmp
            _t.sleep = orig_sleep
        self.assertEqual(r.status, self._ItemStatus.FAIL)

    def test_scn_cmp_group_sync_skip_when_cmp_unreachable(self) -> None:
        """CMP precheck timeout (None) → SKIP."""
        from verify.lib.items.stage6 import scn_cmp_group_sync as mod
        from verify.lib.common import csc_http
        orig_login = csc_http.admin_login
        orig_cmp = mod.cmp_request
        try:
            csc_http.admin_login = lambda *a, **k: "JWT"
            mod.cmp_request = lambda *a, **k: None
            ctx = self._ctx()
            r = mod.scn_cmp_group_sync(ctx)
        finally:
            csc_http.admin_login = orig_login
            mod.cmp_request = orig_cmp
        self.assertEqual(r.status, self._ItemStatus.SKIP)
        self.assertIn("STATS", r.detail)

    # ── S6-MCPTT-FLOOR-GRANT ──
    def test_scn_floor_pass_via_flow_jsonl(self) -> None:
        """flow.jsonl 에 GRANT/TAKEN/IDLE 모두 등장 → PASS."""
        from verify.lib.items.stage6 import scn_mcptt_floor_grant as mod
        flow = [
            {"method": "FLOOR_REQUEST", "proto": "MCPTT"},
            {"method": "FLOOR_GRANT",   "proto": "MCPTT"},
            {"method": "FLOOR_TAKEN",   "proto": "MCPTT"},
            {"method": "FLOOR_IDLE",    "proto": "MCPTT"},
            {"method": "FLOOR_GRANT",   "proto": "MCPTT"},
            {"method": "FLOOR_TAKEN",   "proto": "MCPTT"},
            {"method": "FLOOR_IDLE",    "proto": "MCPTT"},
        ]
        orig = mod.iter_flow_lines
        try:
            mod.iter_flow_lines = (
                lambda dist_dir, *, node=None, proto=None, since=0.0: iter(flow)
            )
            ctx = self._ctx()
            r = mod.scn_mcptt_floor_grant(ctx)
        finally:
            mod.iter_flow_lines = orig
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("GRANT=2", r.detail)

    def test_scn_floor_pass_via_cspsim_fallback(self) -> None:
        """flow 0건이지만 cspsim TAIL 마커 → PASS (fallback)."""
        from verify.lib.items.stage6 import scn_mcptt_floor_grant as mod
        orig = mod.iter_flow_lines
        try:
            mod.iter_flow_lines = (
                lambda dist_dir, *, node=None, proto=None, since=0.0: iter([])
            )
            ctx = self._ctx()
            ctx.state["S6_PTT_VOICE_TAIL"] = (
                "[Scenario] Member 1: PTT Request (floor)\n"
                "[Scenario] Floor rotation complete\n"
            )
            r = mod.scn_mcptt_floor_grant(ctx)
        finally:
            mod.iter_flow_lines = orig
        self.assertEqual(r.status, self._ItemStatus.PASS)

    def test_scn_floor_skip_when_no_signal(self) -> None:
        """flow 0건 + cspsim 마커 없음 → SKIP."""
        from verify.lib.items.stage6 import scn_mcptt_floor_grant as mod
        orig = mod.iter_flow_lines
        try:
            mod.iter_flow_lines = (
                lambda dist_dir, *, node=None, proto=None, since=0.0: iter([])
            )
            ctx = self._ctx()
            r = mod.scn_mcptt_floor_grant(ctx)
        finally:
            mod.iter_flow_lines = orig
        self.assertEqual(r.status, self._ItemStatus.SKIP)


class TestCscConfig(unittest.TestCase):
    """verify.lib.common.csc_config — Agent.MtlsEnabled 토글 helper."""

    def test_set_mtls_returns_false_when_csc_tb_missing(self) -> None:
        from verify.lib.common import csc_config
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ok = csc_config.set_mtls_enabled(td, True)
        self.assertFalse(ok, "csc-tb.json 없으면 False")

    def test_set_mtls_toggles_false_to_true(self) -> None:
        from verify.lib.common import csc_config
        import tempfile, json as _json
        with tempfile.TemporaryDirectory() as td:
            cfg = os.path.join(td, "mgmt-server", "csc", "csc", "config")
            os.makedirs(cfg)
            path = os.path.join(cfg, "csc-tb.json")
            with open(path, "w") as f:
                _json.dump({"Agent": {"MtlsEnabled": False}}, f)
            ok = csc_config.set_mtls_enabled(td, True)
            self.assertTrue(ok)
            self.assertTrue(csc_config.get_mtls_enabled(td))

    def test_set_mtls_creates_agent_section_if_missing(self) -> None:
        from verify.lib.common import csc_config
        import tempfile, json as _json
        with tempfile.TemporaryDirectory() as td:
            cfg = os.path.join(td, "mgmt-server", "csc", "csc", "config")
            os.makedirs(cfg)
            path = os.path.join(cfg, "csc-tb.json")
            with open(path, "w") as f:
                _json.dump({"OtherSection": {}}, f)
            ok = csc_config.set_mtls_enabled(td, True)
            self.assertTrue(ok)
            with open(path) as f:
                data = _json.load(f)
            self.assertEqual(data["Agent"]["MtlsEnabled"], True)
            self.assertIn("OtherSection", data, "기존 섹션 보존")


class TestCspNotify(unittest.TestCase):
    """verify.lib.common.csp_notify — CSP UDP 4421 notify helper."""

    def test_notify_csp_event_sends_json(self) -> None:
        from verify.lib.common import csp_notify
        captured: list = []

        class _FakeSock:
            def __init__(self, *a, **k): pass
            def settimeout(self, t): pass
            def sendto(self, data, addr):
                captured.append((data, addr))
            def close(self): pass

        import socket as _socket
        orig = _socket.socket
        try:
            _socket.socket = lambda *a, **k: _FakeSock()
            ok = csp_notify.notify_csp_event(
                "GROUP_CHANGED", "tel:test-grp", "PUT",
                ip="127.0.0.1", port=4421,
            )
        finally:
            _socket.socket = orig

        self.assertTrue(ok)
        self.assertEqual(len(captured), 1)
        data, addr = captured[0]
        self.assertEqual(addr, ("127.0.0.1", 4421))
        import json as _json
        payload = _json.loads(data.decode())
        self.assertEqual(payload["event"], "GROUP_CHANGED")
        self.assertEqual(payload["uri"], "tel:test-grp")
        self.assertEqual(payload["action"], "PUT")
        self.assertEqual(payload["service"], "console")

    def test_notify_csp_event_returns_false_on_socket_error(self) -> None:
        from verify.lib.common import csp_notify

        class _BrokenSock:
            def __init__(self, *a, **k): pass
            def settimeout(self, t): pass
            def sendto(self, data, addr): raise OSError("network unreachable")
            def close(self): pass

        import socket as _socket
        orig = _socket.socket
        try:
            _socket.socket = lambda *a, **k: _BrokenSock()
            ok = csp_notify.notify_csp_event("GROUP_CHANGED")
        finally:
            _socket.socket = orig
        self.assertFalse(ok)


class TestWebhook(unittest.TestCase):
    """verify.lib.webhook — env 설정 + payload 빌드 + filter."""

    def setUp(self) -> None:
        from verify.lib import webhook
        self._wh = webhook
        # env 보존 (다른 테스트 영향 X)
        self._orig_env = {
            k: os.environ.get(k) for k in (
                "CIMS_VERIFY_WEBHOOK_URL",
                "CIMS_VERIFY_WEBHOOK_FILTER",
                "CIMS_VERIFY_WEBHOOK_TIMEOUT",
            )
        }

    def tearDown(self) -> None:
        for k, v in self._orig_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def _record(self, verdict="PASS"):
        return {
            "id": 1234567890,
            "verdict": verdict,
            "scope": "stage5",
            "totals": {"total": 7, "pass": 6, "fail": 1, "skip": 0, "blocked": 0},
            "elapsed_ms": 23456,
            "started_at": "2026-05-07T12:00:00.000",
            "finished_at": "2026-05-07T12:00:23.456",
            "git_branch": "feature/x",
            "git_sha": "abc1234",
            "host": "test-host",
            "trigger": "cli",
            "report_path": "/tmp/x",
            "pkg_manifest_hash": "deadbeef",
        }

    def test_publish_no_url_returns_none(self) -> None:
        os.environ.pop("CIMS_VERIFY_WEBHOOK_URL", None)
        self.assertIsNone(self._wh.publish(self._record()))

    def test_publish_dry_run_returns_payload(self) -> None:
        os.environ["CIMS_VERIFY_WEBHOOK_URL"] = "https://example.invalid/hook"
        os.environ.pop("CIMS_VERIFY_WEBHOOK_FILTER", None)
        payload = self._wh.publish(self._record("FAIL"), dry_run=True)
        self.assertIsNotNone(payload)
        self.assertEqual(payload["run_id"], 1234567890)
        self.assertEqual(payload["verdict"], "FAIL")
        self.assertEqual(payload["scope"], "stage5")
        self.assertEqual(payload["totals"]["fail"], 1)
        self.assertEqual(payload["host"], "test-host")
        # write_run 후 record 의 모든 키가 payload 에 포함
        self.assertIn("pkg_manifest_hash", payload)
        self.assertEqual(payload["trigger"], "cli")

    def test_publish_filter_blocks_unmatched_verdict(self) -> None:
        os.environ["CIMS_VERIFY_WEBHOOK_URL"] = "https://example.invalid/hook"
        os.environ["CIMS_VERIFY_WEBHOOK_FILTER"] = "FAIL"
        # PASS 는 filter 에서 제외 → None
        self.assertIsNone(self._wh.publish(self._record("PASS"), dry_run=True))
        # FAIL 은 통과
        self.assertIsNotNone(self._wh.publish(self._record("FAIL"), dry_run=True))

    def test_publish_filter_allows_multiple_verdicts(self) -> None:
        os.environ["CIMS_VERIFY_WEBHOOK_URL"] = "https://example.invalid/hook"
        os.environ["CIMS_VERIFY_WEBHOOK_FILTER"] = "FAIL,UNKNOWN"
        self.assertIsNone(self._wh.publish(self._record("PASS"), dry_run=True))
        self.assertIsNotNone(self._wh.publish(self._record("FAIL"), dry_run=True))
        self.assertIsNotNone(self._wh.publish(self._record("UNKNOWN"), dry_run=True))

    def test_publish_real_http_failure_returns_none_no_raise(self) -> None:
        # 존재하지 않는 host — 예외는 잡히고 None 반환 (raise 하지 않음).
        os.environ["CIMS_VERIFY_WEBHOOK_URL"] = "http://127.0.0.1:1/no-such-port"
        os.environ["CIMS_VERIFY_WEBHOOK_TIMEOUT"] = "1"
        result = self._wh.publish(self._record(), dry_run=False)
        self.assertIsNone(result)


class TestVerificationHandlers(unittest.TestCase):
    """csc/src/handlers/verification.py 의 회차/통계 handler 단위 테스트.

    handle_verification 라우팅 + _record_run / _list_runs / _runs_stats /
    _get_run / _delete_run 모두 cover. run_store 는 실제 사용 (tempdir),
    pkg_manifest_hash / git_meta 는 monkey patch.
    """

    def setUp(self) -> None:
        # httpsrv stub (TestParseItemsProgress 의 패턴 그대로)
        repo_root = _REPO_ROOT
        csc_src = os.path.join(repo_root, "csc", "src")
        if csc_src not in sys.path:
            sys.path.insert(0, csc_src)
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
            self.v = importlib.import_module("handlers.verification")
        except Exception as e:
            self.skipTest(f"handlers.verification import 실패: {e}")
        # 격리 tempdir 을 _SCRIPT_DIR 로 가짜 — verify_runs/ 그 안에 떨어지게.
        import tempfile
        self._td = tempfile.mkdtemp(prefix="verify_handler_test_")
        self._orig_script_dir = self.v._SCRIPT_DIR
        self.v._SCRIPT_DIR = self._td
        # _run_store — init() 가 import 했어야 하지만 unit test 에선 직접 주입.
        self._orig_rs = self.v._run_store
        from verify.lib import run_store as _rs
        self.v._run_store = _rs
        # _detect_git_meta / _resolve_pkg_manifest_hash 는 실 환경 의존이라 stub.
        self._orig_git = self.v._detect_git_meta
        self._orig_pkg = self.v._resolve_pkg_manifest_hash
        self.v._detect_git_meta = lambda: ("test-branch", "abc1234", "test-host")
        self.v._resolve_pkg_manifest_hash = lambda: "deadbeef" * 8
        self._tmpfiles: list = []

    def tearDown(self) -> None:
        import shutil
        self.v._SCRIPT_DIR = self._orig_script_dir
        self.v._run_store = self._orig_rs
        self.v._detect_git_meta = self._orig_git
        self.v._resolve_pkg_manifest_hash = self._orig_pkg
        shutil.rmtree(self._td, ignore_errors=True)
        for p in self._tmpfiles:
            try: os.remove(p)
            except OSError: pass

    def _make_log(self, lines: list) -> str:
        import tempfile
        fd, path = tempfile.mkstemp(prefix="job_log_", suffix=".log")
        os.close(fd)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        self._tmpfiles.append(path)
        return path

    def _make_handler_args(self, query_params=None):
        ha = self.v.HandlerArgs()
        ha.query_params = query_params or {}
        ha.method = "GET"
        ha.full_path = "/api/v1/verification/runs"
        ha.body = {}
        return ha

    def _async_run(self, coro):
        """비동기 핸들러 실행 — 격리된 loop, 자동 close."""
        import asyncio
        loop = asyncio.new_event_loop()
        try:
            return loop.run_until_complete(coro)
        finally:
            loop.close()

    # ── _record_run ──
    def test_record_run_writes_file(self) -> None:
        log = self._make_log([
            "[VERIFY] run-start: total=2 ids=A,B",
            "[VERIFY] item-start: A stage=1 idx=1/2 name=alpha",
            "[VERIFY] item-end: A status=PASS elapsed_ms=100",
            "[VERIFY] item-start: B stage=1 idx=2/2 name=beta",
            "[VERIFY] item-end: B status=PASS elapsed_ms=50",
            "[VERIFY] run-end: total=2 pass=2 fail=0 skip=0 blocked=0",
        ])
        job = {
            "log_path": log, "started_at": 1000.0, "ended_at": 1001.5,
            "verdict": "PASS", "scope": "stage1", "selected_ids": ["A", "B"],
            "trigger_type": "user", "report_path": "/tmp/x", "job_id": "job-uuid-1",
        }
        self.v._record_run(job)
        self.assertGreater(job.get("run_id", 0), 0)
        # 실제 파일 작성 확인
        from verify.lib import run_store
        rec = run_store.get_run(self._td, job["run_id"])
        self.assertIsNotNone(rec)
        self.assertEqual(rec["verdict"], "PASS")
        self.assertEqual(rec["scope"], "stage1")
        self.assertEqual(rec["totals"]["pass"], 2)
        self.assertEqual(rec["git_branch"], "test-branch")
        self.assertEqual(rec["git_sha"], "abc1234")
        self.assertEqual(rec["host"], "test-host")
        self.assertEqual(len(rec["pkg_manifest_hash"]), 64)
        # 항목 평탄화 확인
        self.assertEqual(len(rec["items"]), 2)
        self.assertEqual(rec["items"][0]["id"], "A")
        self.assertEqual(rec["items"][0]["status"], "PASS")
        self.assertEqual(rec["items"][1]["idx"], 2)
        # elapsed_ms = (ended - started) * 1000 = 1500
        self.assertEqual(rec["elapsed_ms"], 1500)

    def test_record_run_with_children_flattens_with_parent_id(self) -> None:
        log = self._make_log([
            "[VERIFY] run-start: total=1 ids=PG",
            "[VERIFY] item-start: PG-CHILD-A stage=5 idx=1/2 name=child-a",
            "[VERIFY] child-result: PG.PG-CHILD-A status=PASS elapsed_ms=100 name=child-a",
            "[VERIFY] item-end: PG-CHILD-A status=PASS elapsed_ms=100",
            "[VERIFY] item-start: PG-CHILD-B stage=5 idx=2/2 name=child-b",
            "[VERIFY] child-result: PG.PG-CHILD-B status=FAIL elapsed_ms=200 name=child-b",
            "[VERIFY] item-end: PG-CHILD-B status=FAIL elapsed_ms=200",
            "[VERIFY] group-end: PG status=FAIL child_count=2",
            "[VERIFY] run-end: total=1 pass=0 fail=1 skip=0 blocked=0",
        ])
        job = {
            "log_path": log, "started_at": 1000.0, "ended_at": 1001.0,
            "verdict": "FAIL", "scope": "stage5",
            "selected_ids": ["PG"], "trigger_type": "user",
        }
        self.v._record_run(job)
        from verify.lib import run_store
        rec = run_store.get_run(self._td, job["run_id"])
        self.assertEqual(rec["verdict"], "FAIL")
        # 부모 PG + 자식 2개 = 3 entries (평탄화)
        self.assertEqual(len(rec["items"]), 3)
        parent = rec["items"][0]
        self.assertEqual(parent["id"], "PG")
        self.assertTrue(parent["is_group"])
        self.assertIsNone(parent["parent_id"])
        # 자식
        children = [it for it in rec["items"] if it["parent_id"] == "PG"]
        self.assertEqual(len(children), 2)
        self.assertEqual({c["id"] for c in children}, {"PG-CHILD-A", "PG-CHILD-B"})

    def test_record_run_silently_no_op_without_run_store(self) -> None:
        # _run_store None 이면 raise 안 하고 무시 (CIMS 정책)
        orig = self.v._run_store
        try:
            self.v._run_store = None
            job = {"log_path": "/dev/null", "verdict": "PASS"}
            self.v._record_run(job)
            self.assertNotIn("run_id", job)
        finally:
            self.v._run_store = orig

    # ── _list_runs ──
    def test_list_runs_empty(self) -> None:
        ha = self._make_handler_args()
        r = self._async_run(self.v._list_runs(ha))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["total"], 0)
        self.assertEqual(r.body["runs"], [])

    def test_list_runs_returns_recorded(self) -> None:
        # 2 회차 기록 후 list
        for verdict in ("PASS", "FAIL"):
            log = self._make_log([
                "[VERIFY] run-start: total=1 ids=X",
                "[VERIFY] item-start: X stage=1 idx=1/1 name=x",
                f"[VERIFY] item-end: X status={verdict} elapsed_ms=10",
                f"[VERIFY] run-end: total=1 pass={1 if verdict=='PASS' else 0} "
                f"fail={1 if verdict=='FAIL' else 0} skip=0 blocked=0",
            ])
            self.v._record_run({
                "log_path": log, "started_at": 1000.0, "ended_at": 1001.0,
                "verdict": verdict, "scope": f"stage{1 if verdict=='PASS' else 2}",
                "selected_ids": ["X"], "trigger_type": "cli",
            })
        # 전체 list
        r = self._async_run(self.v._list_runs(self._make_handler_args()))
        self.assertEqual(r.body["total"], 2)
        # verdict 필터
        r = self._async_run(
            self.v._list_runs(self._make_handler_args({"verdict": "PASS"})),
        )
        self.assertEqual(r.body["total"], 1)
        self.assertEqual(r.body["runs"][0]["verdict"], "PASS")
        # stage 필터 (scope=stage1)
        r = self._async_run(
            self.v._list_runs(self._make_handler_args({"stage": "1"})),
        )
        self.assertEqual(r.body["total"], 1)

    # ── _runs_stats ──
    def test_runs_stats_aggregates(self) -> None:
        # 3 회차 (2 PASS + 1 FAIL), 모두 stage1 scope
        for verdict, ms in (("PASS", 100), ("PASS", 200), ("FAIL", 500)):
            log = self._make_log([
                "[VERIFY] run-start: total=1 ids=X",
                "[VERIFY] item-start: X stage=1 idx=1/1 name=x",
                f"[VERIFY] item-end: X status={verdict} elapsed_ms={ms}",
                f"[VERIFY] run-end: total=1 pass={1 if verdict=='PASS' else 0} "
                f"fail={1 if verdict=='FAIL' else 0} skip=0 blocked=0",
            ])
            self.v._record_run({
                "log_path": log,
                "started_at": 1000.0, "ended_at": 1000.0 + ms / 1000.0,
                "verdict": verdict, "scope": "stage1",
                "selected_ids": ["X"], "trigger_type": "user",
            })
        r = self._async_run(
            self.v._runs_stats(self._make_handler_args({"days": "30"})),
        )
        self.assertEqual(r.status, 200)
        ov = r.body["overall"]
        self.assertEqual(ov["runs"], 3)
        self.assertEqual(ov["pass"], 2)
        self.assertEqual(ov["fail"], 1)
        self.assertEqual(ov["success_rate"], round(100.0 * 2 / 3, 1))
        # by_scope
        self.assertEqual(len(r.body["by_scope"]), 1)
        self.assertEqual(r.body["by_scope"][0]["scope"], "stage1")
        # timeline ASC (오래된 → 최신)
        tl = r.body["timeline"]
        self.assertEqual(len(tl), 3)
        for i in range(len(tl) - 1):
            self.assertLessEqual(tl[i]["id"], tl[i + 1]["id"])

    # ── _get_run / _delete_run ──
    def test_record_run_fires_webhook_when_configured(self) -> None:
        """CIMS_VERIFY_WEBHOOK_URL 설정 시 _record_run 후 webhook.publish 호출."""
        log = self._make_log([
            "[VERIFY] run-start: total=1 ids=X",
            "[VERIFY] item-start: X stage=1 idx=1/1 name=x",
            "[VERIFY] item-end: X status=PASS elapsed_ms=10",
            "[VERIFY] run-end: total=1 pass=1 fail=0 skip=0 blocked=0",
        ])
        # webhook.publish monkey patch — payload 캡처
        from verify.lib import webhook as _wh
        captured: list = []
        orig = _wh.publish
        try:
            _wh.publish = lambda rec, **kw: captured.append(rec) or rec
            self.v._record_run({
                "log_path": log, "started_at": 1000.0, "ended_at": 1001.0,
                "verdict": "PASS", "scope": "stage1",
                "selected_ids": ["X"], "trigger_type": "user",
            })
        finally:
            _wh.publish = orig
        self.assertEqual(len(captured), 1)
        self.assertEqual(captured[0]["verdict"], "PASS")
        self.assertGreater(captured[0]["id"], 0)
        # record 의 id 가 write_run 후 갱신됐는지
        self.assertEqual(captured[0]["scope"], "stage1")

    def test_get_and_delete_run(self) -> None:
        log = self._make_log([
            "[VERIFY] run-start: total=1 ids=X",
            "[VERIFY] item-start: X stage=1 idx=1/1 name=x",
            "[VERIFY] item-end: X status=PASS elapsed_ms=10",
            "[VERIFY] run-end: total=1 pass=1 fail=0 skip=0 blocked=0",
        ])
        job = {
            "log_path": log, "started_at": 1000.0, "ended_at": 1001.0,
            "verdict": "PASS", "scope": "stage1",
            "selected_ids": ["X"], "trigger_type": "user",
        }
        self.v._record_run(job)
        rid = job["run_id"]

        # get
        r = self._async_run(self.v._get_run(rid))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["id"], rid)
        self.assertEqual(r.body["verdict"], "PASS")
        # 없는 id → 404
        r = self._async_run(self.v._get_run(999999999))
        self.assertEqual(r.status, 404)
        # delete
        r = self._async_run(self.v._delete_run(rid))
        self.assertEqual(r.status, 200)
        self.assertTrue(r.body["deleted"])
        # 다시 get → 404
        r = self._async_run(self.v._get_run(rid))
        self.assertEqual(r.status, 404)


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
            # csc / console / cspsim — 파일명으로 구분해 다른 id 반환
            base = os.path.basename(file_path)
            if base.startswith("csc-"):       pid = 1
            elif base.startswith("console-"): pid = 2
            else:                              pid = 3   # cspsim
            return (201, {"id": pid})
        self._csc_http.post_multipart = fake_post_multipart

        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            # csc-1.0.0.tar.gz, csc-1.10.0.tar.gz (natural sort: 1.10 > 1.0)
            for fn in ("csc-1.0.0.tar.gz", "csc-1.10.0.tar.gz",
                       "console-2.5.0.tar.gz", "cspsim-0.0.1.tar.gz"):
                with open(os.path.join(pkg_dir, fn), "w") as f:
                    f.write("dummy")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok", "JWT")
            r = self._native.step_08_package_upload(ctx)

        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "pkg_id_csc"), 1)
        self.assertEqual(self._native._get(ctx, "pkg_id_console"), 2)
        self.assertEqual(self._native._get(ctx, "pkg_id_sim"), 3)
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
            # csc → did=11, console → did=22, sim → did=33
            pname = payload["process_name"]
            did = {"CSC": 11, "CONSOLE": 22, "CSPSIM": 33}[pname]
            return (201, {"id": did})
        self._csc_http.post_json = fake_post_json

        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "aid_csc", 7)
        self._native._set(ctx, "pkg_id_csc", 1)
        self._native._set(ctx, "pkg_id_console", 2)
        self._native._set(ctx, "pkg_id_sim", 3)
        r = self._native.step_09_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "dep_id_csc"), 11)
        self.assertEqual(self._native._get(ctx, "dep_id_console"), 22)
        self.assertEqual(self._native._get(ctx, "dep_id_sim"), 33)
        # config overlay 검증 — csc:Server.Port=4445, console:Port=8081, sim: 없음
        csc_payload = next(p for u, p in captured if p["process_name"] == "CSC")
        self.assertEqual(csc_payload["config"], {"Server.Port": 4445})
        cons_payload = next(p for u, p in captured if p["process_name"] == "CONSOLE")
        self.assertEqual(cons_payload["config"], {"Port": 8081})
        sim_payload = next(p for u, p in captured if p["process_name"] == "CSPSIM")
        self.assertEqual(sim_payload["config"], {})

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
                base = os.path.join(td, "mgmt-server", name)
                os.makedirs(os.path.join(base, "config"))
                with open(os.path.join(base, "meta.json"), "w") as f:
                    f.write('{"name": "' + name + '"}')
            # sim 은 cspsim tarball 구조상 config/ 없음 — meta.json 만
            sim_base = os.path.join(td, "mgmt-server", "sim")
            os.makedirs(sim_base)
            with open(os.path.join(sim_base, "meta.json"), "w") as f:
                f.write('{"name": "sim"}')
            ctx = self._ctx_with_dist(td)
            r = self._native.step_11_verify_files(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("csc: meta.json + config/ 존재", r.detail)
        self.assertIn("console: meta.json + config/ 존재", r.detail)
        self.assertIn("sim: meta.json 존재", r.detail)

    def test_step_11_fail_when_meta_missing(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            # csc 만 정상, console 의 meta.json 누락
            os.makedirs(os.path.join(td, "mgmt-server", "csc", "config"))
            with open(os.path.join(td, "mgmt-server", "csc", "meta.json"), "w") as f:
                f.write("{}")
            os.makedirs(os.path.join(td, "mgmt-server", "console", "config"))
            # console/meta.json 만들지 않음
            ctx = self._ctx_with_dist(td)
            r = self._native.step_11_verify_files(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("console: 누락 meta.json", r.detail)

    def test_step_11_fail_when_config_dir_missing(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            for name in ("csc", "console"):
                base = os.path.join(td, "mgmt-server", name)
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
    def test_target_helpers_verify_default(self) -> None:
        """opts 비어있으면 verify default — _ports {csc:4445, console:8081}."""
        from verify.lib.items.stage5 import _native_steps as ns
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self.assertEqual(ns._target(ctx), "verify")
        self.assertEqual(ns._ports(ctx), {"csc": 4445, "console": 8081})
        self.assertEqual(ns._deployed_csc_base(ctx), "https://127.0.0.1:4445")

    def test_target_helpers_prod(self) -> None:
        """opts.target='prod' — _ports {csc:4420, console:80}."""
        from verify.lib.items.stage5 import _native_steps as ns
        ctx = self._VerifyContext.create(
            repo_root=_REPO_ROOT, stage=5, opts={"target": "prod"},
        )
        self.assertEqual(ns._target(ctx), "prod")
        self.assertEqual(ns._ports(ctx), {"csc": 4420, "console": 80})
        self.assertEqual(ns._deployed_csc_base(ctx), "https://127.0.0.1:4420")

    def test_target_helpers_unknown_falls_back_to_verify(self) -> None:
        """알 수 없는 target → verify default."""
        from verify.lib.items.stage5 import _native_steps as ns
        ctx = self._VerifyContext.create(
            repo_root=_REPO_ROOT, stage=5, opts={"target": "staging"},
        )
        # _target 자체는 "staging" 반환하지만 _ports 는 verify default fallback
        self.assertEqual(ns._target(ctx), "staging")
        self.assertEqual(ns._ports(ctx), {"csc": 4445, "console": 8081})

    def test_csc_overlay_uses_ports(self) -> None:
        """_csc_overlay 가 명시 ports dict 사용."""
        from verify.lib.items.stage5 import _native_steps as ns
        verify_ports = {"csc": 4445, "console": 8081}
        prod_ports = {"csc": 4420, "console": 80}
        self.assertEqual(ns._csc_overlay("csc", verify_ports), {"Server.Port": 4445})
        self.assertEqual(ns._csc_overlay("csc", prod_ports), {"Server.Port": 4420})
        self.assertEqual(ns._csc_overlay("console", verify_ports), {"Port": 8081})
        self.assertEqual(ns._csc_overlay("console", prod_ports), {"Port": 80})
        self.assertEqual(ns._csc_overlay("unknown", verify_ports), {})

    def test_step_12_target_prod_uses_4420(self) -> None:
        """target=prod 시 _csc_overlay 가 csc=4420 기대값으로."""
        from verify.lib.items.stage5 import _native_steps
        import tempfile, json as _json
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "mgmt-server", "csc")
            os.makedirs(base)
            # 4420 으로 overlay 된 config 가 있다 가정
            with open(os.path.join(base, "config.json"), "w") as f:
                _json.dump({"Server.Port": 4420}, f)
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5,
                                              opts={"target": "prod"})
            ctx.dist_dir = td
            r = _native_steps.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertIn("Server.Port=4420", r.detail)

    def test_step_12_target_prod_fail_when_4445(self) -> None:
        """target=prod 인데 config 가 verify 값(4445) 면 FAIL."""
        from verify.lib.items.stage5 import _native_steps
        import tempfile, json as _json
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "mgmt-server", "csc")
            os.makedirs(base)
            with open(os.path.join(base, "config.json"), "w") as f:
                _json.dump({"Server.Port": 4445}, f)
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5,
                                              opts={"target": "prod"})
            ctx.dist_dir = td
            r = _native_steps.step_12_verify_overlay(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("기대=4420", r.detail)

    def test_step_12_pass_with_flat_key(self) -> None:
        import tempfile
        import json as _json
        with tempfile.TemporaryDirectory() as td:
            base = os.path.join(td, "mgmt-server", "csc")
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
            base = os.path.join(td, "mgmt-server", "csc")
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
            base = os.path.join(td, "mgmt-server", "csc")
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
            base = os.path.join(td, "mgmt-server", "csc")
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
        self._shell.port_listening = lambda port, proto="tcp", host="": port == 4445
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        r = self._native.step_13_csc_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "csc_start_ok"))

    def test_step_13_fail_on_listen_timeout(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        self._shell.port_listening = lambda port, proto="tcp", host="": False
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_csc", 11)
        r = self._native.step_13_csc_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertFalse(self._native._get(ctx, "csc_start_ok"))

    def test_step_13_fail_on_post_status_error(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (500, {})
        self._shell.port_listening = lambda port, proto="tcp", host="": True
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
        self._shell.port_listening = lambda port, proto="tcp", host="": port == 8081
        ctx = self._ctx()
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_console", 22)
        r = self._native.step_15_console_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "console_start_ok"))

    def test_step_15_target_prod_uses_port_80(self) -> None:
        """target=prod 시 step_15 가 console port 80 LISTEN 검증."""
        captured_ports: list = []
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        def fake_listen(port, proto="tcp", host=""):
            captured_ports.append(port)
            return port == 80
        self._shell.port_listening = fake_listen
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5,
                                          opts={"target": "prod"})
        self._native._set(ctx, "tok", "JWT")
        self._native._set(ctx, "dep_id_console", 22)
        r = self._native.step_15_console_start(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(captured_ports[0], 80)
        self.assertIn("port 80", r.detail)

    def test_step_15_fail_on_listen_timeout(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        self._shell.port_listening = lambda port, proto="tcp", host="": False
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

    def test_delete_run_single(self) -> None:
        """delete_run — 단건 삭제 동작 + 없는 id 처리."""
        rid = self._rs.write_run(self._td, self._make_record())
        # 단건 삭제
        self.assertTrue(self._rs.delete_run(self._td, rid))
        # 다시 삭제 시도 → False
        self.assertFalse(self._rs.delete_run(self._td, rid))
        # 다른 회차는 영향 X
        rid2 = self._rs.write_run(self._td, self._make_record())
        self.assertIsNotNone(self._rs.get_run(self._td, rid2))

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
        # step_21 의 csp↔cmp wait 를 unit test 에서 비활성 — sleep 이 0 으로
        # mock 됐는데 wait deadline 까지 busy loop 도는 것 방지 (150s).
        self._orig_cmp_wait = os.environ.get("CIMS_VERIFY_CMP_WAIT_S")
        os.environ["CIMS_VERIFY_CMP_WAIT_S"] = "0"

    def tearDown(self) -> None:
        if self._orig_cmp_wait is None:
            os.environ.pop("CIMS_VERIFY_CMP_WAIT_S", None)
        else:
            os.environ["CIMS_VERIFY_CMP_WAIT_S"] = self._orig_cmp_wait
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
    def test_step_17_pass_with_modules(self) -> None:
        # P2 토폴로지: 6 service-server (csp/psp/isp + cmp/pmp/imp) tarball 업로드 PASS.
        # sim 은 mgmt-server agent 가 step_08 에서 처리 — step_17 대상 아님.
        import tempfile
        captured: list = []
        def fake_post_multipart(url, *, file_path, file_field="file",
                                filename=None, form_fields=None,
                                token=None, timeout=60):
            captured.append(file_path)
            base = os.path.basename(file_path)
            if   base.startswith("csp-"): pid = 11
            elif base.startswith("psp-"): pid = 14
            elif base.startswith("isp-"): pid = 16
            elif base.startswith("cmp-"): pid = 12
            elif base.startswith("pmp-"): pid = 15
            elif base.startswith("imp-"): pid = 17
            else: pid = 99
            return (201, {"id": pid})
        self._csc_http.post_multipart = fake_post_multipart

        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            for fn in ("csp-1.0.0.tar.gz", "psp-1.0.0.tar.gz", "isp-1.0.0.tar.gz",
                       "cmp-1.0.0.tar.gz", "pmp-1.0.0.tar.gz", "imp-1.0.0.tar.gz"):
                with open(os.path.join(pkg_dir, fn), "w") as f: f.write("d")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok2", "JWT2")
            r = self._native.step_17_modules_pkg_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "pkg2_id_csp"), 11)
        self.assertEqual(self._native._get(ctx, "pkg2_id_psp"), 14)
        self.assertEqual(self._native._get(ctx, "pkg2_id_isp"), 16)
        self.assertEqual(self._native._get(ctx, "pkg2_id_cmp"), 12)
        self.assertEqual(self._native._get(ctx, "pkg2_id_pmp"), 15)
        self.assertEqual(self._native._get(ctx, "pkg2_id_imp"), 17)

    def test_step_17_fail_when_pmp_tarball_missing(self) -> None:
        import tempfile
        self._csc_http.post_multipart = lambda u, **k: (201, {"id": 1})
        with tempfile.TemporaryDirectory() as td:
            pkg_dir = os.path.join(td, "packages")
            os.makedirs(pkg_dir)
            # pmp tarball 만 누락 — 나머지 3 모듈은 존재
            for fn in ("csp-1.0.0.tar.gz", "psp-1.0.0.tar.gz",
                       "cmp-1.0.0.tar.gz"):
                with open(os.path.join(pkg_dir, fn), "w") as f: f.write("d")
            ctx = self._ctx_with_dist(td)
            self._native._set(ctx, "tok2", "JWT2")
            r = self._native.step_17_modules_pkg_upload(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("ptt-media-server", r.detail)
        self.assertIn("pmp-*.tar.gz", r.detail)

    # ── step 18 ──
    def test_step_18_skips_without_tok2(self) -> None:
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        r = self._native.step_18_modules_agent_enroll(ctx)
        self.assertEqual(r.status, self._ItemStatus.SKIP)

    def test_step_18_pass(self) -> None:
        # P2 토폴로지 (2026-05-11): 4 unique agent (volte-sip/media, ptt-sip/media) +
        # 6 instances (csp/psp/isp/cmp/pmp/imp). 같은 agent_name 의 변종은 aid/pid 공유.
        post_called: list = []
        _AID_BY_AGENT = {
            "volte-sip-server":   100,   # csp + isp 공유
            "ptt-sip-server":     110,   # psp
            "volte-media-server": 200,   # cmp + imp 공유
            "ptt-media-server":   210,   # pmp
        }
        def fake_post_json(url, payload, token=None, timeout=10):
            post_called.append(url)
            if url.endswith("/agents"):
                name = payload["name"]
                aid = _AID_BY_AGENT.get(name, 999)
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
            return (1000 + _AID_BY_AGENT.get(aname, 0), "")
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
        # 같은 agent 의 변종은 같은 aid 공유 (csp=isp=100, cmp=imp=200)
        self.assertEqual(self._native._get(ctx, "aid_csp"), 100)
        self.assertEqual(self._native._get(ctx, "aid_isp"), 100)
        self.assertEqual(self._native._get(ctx, "aid_psp"), 110)
        self.assertEqual(self._native._get(ctx, "aid_cmp"), 200)
        self.assertEqual(self._native._get(ctx, "aid_imp"), 200)
        self.assertEqual(self._native._get(ctx, "aid_pmp"), 210)
        self.assertEqual(self._native._get(ctx, "ta_pid_csp"), 1100)
        self.assertEqual(self._native._get(ctx, "ta_pid_isp"), 1100)
        # spawn 은 unique agent 당 1회 — 4번
        self.assertEqual(len(spawned), 4)

    # ── step 19 ──
    def test_step_19_pass(self) -> None:
        # P2 토폴로지: 6 service-server deployment (CSP/PSP/ISP/CMP/PMP/IMP) 생성 PASS.
        captured: list = []
        _PMAP = {"CSP": 11, "PSP": 14, "ISP": 16,
                 "CMP": 12, "PMP": 15, "IMP": 17}
        def fake_post_json(url, payload, token=None, timeout=15):
            captured.append(payload)
            return (201, {"id": _PMAP[payload["process_name"]]})
        self._csc_http.post_json = fake_post_json

        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        for m, aid, pid in [("csp", 100, 11), ("psp", 110, 14), ("isp", 120, 16),
                             ("cmp", 200, 12), ("pmp", 210, 15), ("imp", 220, 17)]:
            self._native._set(ctx, f"aid_{m}", aid)
            self._native._set(ctx, f"pkg2_id_{m}", pid)
        r = self._native.step_19_modules_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertEqual(self._native._get(ctx, "dep2_id_csp"), 11)
        self.assertEqual(self._native._get(ctx, "dep2_id_psp"), 14)
        self.assertEqual(self._native._get(ctx, "dep2_id_isp"), 16)
        self.assertEqual(self._native._get(ctx, "dep2_id_pmp"), 15)
        self.assertEqual(self._native._get(ctx, "dep2_id_imp"), 17)
        # PSP/PMP 는 config_overlay (Roles/LocalIp) 가 payload 에 포함
        psp_payload = next(p for p in captured if p["process_name"] == "PSP")
        self.assertIn("config", psp_payload)
        self.assertEqual(psp_payload["config"].get("Setup.Roles.PTT_AS"), True)
        self.assertEqual(psp_payload["config"].get("Setup.Sip.LocalIp"), "127.0.0.3")
        pmp_payload = next(p for p in captured if p["process_name"] == "PMP")
        self.assertEqual(pmp_payload["config"].get("RtpIp"), "127.0.0.3")
        # ISP 는 IBCF role 단독 (CSCF/TAS/PTT_AS=false), local_ip 127.0.0.5
        isp_payload = next(p for p in captured if p["process_name"] == "ISP")
        self.assertEqual(isp_payload["config"].get("Setup.Roles.IBCF"), True)
        self.assertEqual(isp_payload["config"].get("Setup.Roles.CSCF"), False)
        self.assertEqual(isp_payload["config"].get("Setup.Roles.TAS"), False)
        self.assertEqual(isp_payload["config"].get("Setup.Roles.PTT_AS"), False)
        self.assertEqual(isp_payload["config"].get("Setup.Sip.LocalIp"), "127.0.0.5")
        imp_payload = next(p for p in captured if p["process_name"] == "IMP")
        self.assertEqual(imp_payload["config"].get("RtpIp"), "127.0.0.5")
        # install_path 는 server level (agent_name 까지) — tarball 안 변종
        # 디렉토리 (csp/psp/isp/) 가 그 안에 풀리며 _install_path 자체는 leaf 미포함.
        # P2 토폴로지: ISP 는 volte-sip-server, IMP 는 volte-media-server 와 공존.
        csp_payload = next(p for p in captured if p["process_name"] == "CSP")
        self.assertTrue(csp_payload["install_path"].endswith("/volte-sip-server"))
        psp_payload2 = next(p for p in captured if p["process_name"] == "PSP")
        self.assertTrue(psp_payload2["install_path"].endswith("/ptt-sip-server"))
        isp_payload2 = next(p for p in captured if p["process_name"] == "ISP")
        self.assertTrue(isp_payload2["install_path"].endswith("/volte-sip-server"))
        imp_payload2 = next(p for p in captured if p["process_name"] == "IMP")
        self.assertTrue(imp_payload2["install_path"].endswith("/volte-media-server"))

    def test_step_19_fail_missing_pkg_for_one_module(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=15: (201, {"id": 1})
        ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
        self._native._set(ctx, "tok2", "JWT2")
        # csp/cmp 만 ready, pmp 의 pkg_id 누락
        for m in ("csp", "cmp"):
            self._native._set(ctx, f"aid_{m}", 100)
            self._native._set(ctx, f"pkg2_id_{m}", 1)
        self._native._set(ctx, "aid_pmp", 210)
        # pkg2_id_pmp 미설정
        r = self._native.step_19_modules_deployment_create(ctx)
        self.assertEqual(r.status, self._ItemStatus.FAIL)
        self.assertIn("ptt-media-server", r.detail)

    # ── step 20 ──
    def test_step_20_pass_when_all_succeed(self) -> None:
        # PASS 조건: 폴링 종료 시 모든 service-server deployment status ∈ {running, stopped}.
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
        for m, did in [("csp", 11), ("psp", 14), ("isp", 16),
                        ("cmp", 12), ("pmp", 15), ("imp", 17)]:
            self._native._set(ctx, f"dep2_id_{m}", did)
        r = self._native.step_20_modules_install_poll(ctx)
        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "all_install_done_modules"))

    # ── step 21 ──
    def test_step_21_pass(self) -> None:
        # P2: csp/psp/isp + cmp/pmp/imp 6 인스턴스 모두 LISTEN. CMP_WAIT_S=0 으로
        # 시그널링↔미디어 connection wait 자체 비활성 (단위 테스트 — 실제 csp_*.log 없음).
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        def fake_listen(port, proto="tcp", host=""):
            return port in (5060, 9000) and proto == "udp"
        self._shell.port_listening = fake_listen
        marker_called = [0]
        def fake_marker(dist):
            marker_called[0] += 1
            return "abc123def456abc123def456abc123def456abc123def456abc123def456ab"
        self._pkgm.write_marker = fake_marker

        import os as _os
        prev_wait = _os.environ.get("CIMS_VERIFY_CMP_WAIT_S")
        _os.environ["CIMS_VERIFY_CMP_WAIT_S"] = "0"
        try:
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
            self._native._set(ctx, "tok2", "JWT2")
            for m, did in [("csp", 11), ("psp", 14), ("isp", 16),
                            ("cmp", 12), ("pmp", 15), ("imp", 17)]:
                self._native._set(ctx, f"dep2_id_{m}", did)
            r = self._native.step_21_modules_start(ctx)
        finally:
            if prev_wait is None:
                _os.environ.pop("CIMS_VERIFY_CMP_WAIT_S", None)
            else:
                _os.environ["CIMS_VERIFY_CMP_WAIT_S"] = prev_wait

        self.assertEqual(r.status, self._ItemStatus.PASS)
        self.assertTrue(self._native._get(ctx, "modules_start_ok"))
        # immutability marker 기록됐는지
        self.assertEqual(marker_called[0], 1)
        self.assertIn(".deployed-manifest.json", r.detail)

    def test_step_21_fail_when_cmp_not_listening(self) -> None:
        self._csc_http.post_json = lambda u, p, token=None, timeout=10: (202, {})
        # 시그널링 (5060) 은 LISTEN, 미디어 (9000) 는 timeout
        self._shell.port_listening = lambda port, proto="tcp", host="": port == 5060
        import os as _os
        prev_wait = _os.environ.get("CIMS_VERIFY_CMP_WAIT_S")
        _os.environ["CIMS_VERIFY_CMP_WAIT_S"] = "0"
        try:
            ctx = self._VerifyContext.create(repo_root=_REPO_ROOT, stage=5)
            self._native._set(ctx, "tok2", "JWT2")
            for m, did in [("csp", 11), ("psp", 14), ("cmp", 12), ("pmp", 15)]:
                self._native._set(ctx, f"dep2_id_{m}", did)
            r = self._native.step_21_modules_start(ctx)
        finally:
            if prev_wait is None:
                _os.environ.pop("CIMS_VERIFY_CMP_WAIT_S", None)
            else:
                _os.environ["CIMS_VERIFY_CMP_WAIT_S"] = prev_wait
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
            # mgmt-server 배포 3 모듈 (csc/console/sim)
            self._native._set(ctx, "dep_id_csc", 1)
            self._native._set(ctx, "dep_id_console", 2)
            self._native._set(ctx, "dep_id_sim", 3)
            # 4 service-server deployment
            self._native._set(ctx, "dep2_id_csp", 11)
            self._native._set(ctx, "dep2_id_psp", 14)
            self._native._set(ctx, "dep2_id_cmp", 12)
            self._native._set(ctx, "dep2_id_pmp", 15)
            # Test-agent pid: mgmt-server 1 + service-server 4 = 5
            self._native._set(ctx, "ta_pid_csc", 1000)
            self._native._set(ctx, "ta_pid_csp", 1001)
            self._native._set(ctx, "ta_pid_psp", 1011)
            self._native._set(ctx, "ta_pid_cmp", 1002)
            self._native._set(ctx, "ta_pid_pmp", 1012)
            r = self._native.step_22_finalize(ctx)
        finally:
            _os.kill = orig_kill

        self.assertEqual(r.status, self._ItemStatus.PASS)
        # mgmt-server: csc/console/sim (3) + service-server: csp/psp/cmp/pmp (4) = 7 stop 발행
        self.assertEqual(len(post_calls), 7)
        # 5 Test-agent kill (mgmt-server + 4 service-server)
        self.assertEqual(len(kill_calls), 5)


class TestPickStartSubscriber(unittest.TestCase):
    """cspsim 은 시작 가입자 비밀번호 하나로 -count 명을 만든다 —
    시작 가입자는 '번호 연속 + 비밀번호 동일' 구간에서 골라야 한다."""

    def setUp(self) -> None:
        from verify.lib.common.subscribers import pick_start_subscriber
        self.pick = pick_start_subscriber

    def test_skips_accounts_with_odd_password(self) -> None:
        # 앞 2개만 계정별 비밀번호(실서버에서 발견된 데이터 흔들림) → 균일 구간부터 시작
        rows = [("+821300000001", "45033821300000001"),
                ("+821300000002", "45033821300000002"),
                ("+821300000003", "123456"),
                ("+821300000004", "123456")]
        self.assertEqual(self.pick(rows, 2)[0], "+821300000003")

    def test_count_one_takes_first(self) -> None:
        rows = [("+821300000001", "A"), ("+821300000002", "B")]
        self.assertEqual(self.pick(rows, 1)[0], "+821300000001")

    def test_requires_consecutive_numbers(self) -> None:
        # 비밀번호는 같지만 번호가 끊기면 구간이 아니다 (cspsim 은 +1 씩 올린다)
        rows = [("+821300000001", "123456"), ("+821300000003", "123456")]
        self.assertEqual(self.pick(rows, 2)[0], "+821300000001")   # 폴백 = 첫 행

    def test_no_run_falls_back_to_first(self) -> None:
        rows = [("+821300000001", "A"), ("+821300000002", "B")]
        self.assertEqual(self.pick(rows, 2)[0], "+821300000001")

    def test_empty_rows(self) -> None:
        self.assertEqual(self.pick([], 3), ())

    def test_ignores_blank_password_run(self) -> None:
        rows = [("+821300000001", ""), ("+821300000002", ""),
                ("+821300000003", "123456"), ("+821300000004", "123456")]
        self.assertEqual(self.pick(rows, 2)[0], "+821300000003")


class TestServiceLogRoots(unittest.TestCase):
    """녹취/flow 카운터는 설정된 ServiceLogDir 을 봐야 한다 —
    기본 경로(<dist>/ext_mnt/service_log)만 보면 경로를 옮긴 환경에서 '파일 없음' 오판."""

    def setUp(self) -> None:
        import tempfile
        from verify.lib.common.service_log import service_log_roots
        self.roots = service_log_roots
        self.tmp = tempfile.mkdtemp(prefix="cims_svclog_")

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write_cfg(self, rel_parts, payload) -> None:
        import json
        p = os.path.join(self.tmp, *rel_parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            json.dump(payload, f)

    def test_reads_cmp_configured_dir(self) -> None:
        ext = os.path.join(self.tmp, "elsewhere")
        os.makedirs(ext)
        self._write_cfg(("cmp", "config", "cmp.json"),
                        {"ServiceLogging": {"Dir": ext}})
        self.assertIn(ext, self.roots(self.tmp))

    def test_reads_csp_setup_wrapper(self) -> None:
        ext = os.path.join(self.tmp, "csp_log")
        os.makedirs(ext)
        self._write_cfg(("csp", "config", "csp.json"),
                        {"Setup": {"ServiceLogging": {"Dir": ext}}})
        self.assertIn(ext, self.roots(self.tmp))

    def test_default_path_included(self) -> None:
        default = os.path.join(self.tmp, "ext_mnt", "service_log")
        os.makedirs(default)
        self.assertIn(default, self.roots(self.tmp))

    def test_missing_dirs_excluded(self) -> None:
        # 설정에만 있고 실제로 없는 경로는 제외 (glob 대상이 아니다)
        self._write_cfg(("cmp", "config", "cmp.json"),
                        {"ServiceLogging": {"Dir": "/nonexistent/cims_log"}})
        self.assertEqual(self.roots(self.tmp), [])

    def test_no_config_no_crash(self) -> None:
        self.assertEqual(self.roots(self.tmp), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
