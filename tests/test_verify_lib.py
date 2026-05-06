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

    runner.py 의 [VERIFY] item-start/item-end/child-result/group-end 마커 +
    cims.sh _verify_phase2 의 [VERIFY] step-start/step-end 마커 정규식 검증.
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
