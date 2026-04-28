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


class TestRunnerMarkers(unittest.TestCase):
    """[VERIFY] item-start / item-end / child-result 마커 stdout 출력.

    backend (verification.py _parse_items_progress) 가 파싱하므로 형식 회귀 안전망.
    """

    def setUp(self) -> None:
        from verify_lib import registry, runner, context
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
            repo_root=os.path.dirname(_THIS_DIR), phase=99,
            report_dir=tmpdir,
        )

    def test_runner_emits_item_markers(self) -> None:
        """선택 항목 N개 실행 시 item-start N + item-end N 마커 출력."""
        from verify_lib.registry import ItemResult, ItemStatus
        @self.registry.verify_item(id="MARKER-A", phase=99, category="유닛", name="alpha")
        def _a(ctx) -> ItemResult:
            return ItemResult(id="MARKER-A", name="alpha", status=ItemStatus.PASS)
        @self.registry.verify_item(id="MARKER-B", phase=99, category="유닛", name="beta")
        def _b(ctx) -> ItemResult:
            return ItemResult(id="MARKER-B", name="beta", status=ItemStatus.PASS)

        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            ctx = self._make_ctx(td)
            try:
                with redirect_stdout(buf):
                    self.runner.run_items(ctx, ["MARKER-A", "MARKER-B"])
            finally:
                ctx.report_close()
        out = buf.getvalue()
        self.assertIn("[VERIFY] run-start: total=2", out)
        self.assertIn("[VERIFY] item-start: MARKER-A idx=1/2 name=alpha", out)
        self.assertIn("[VERIFY] item-end: MARKER-A status=PASS", out)
        self.assertIn("[VERIFY] item-start: MARKER-B idx=2/2 name=beta", out)
        self.assertIn("[VERIFY] item-end: MARKER-B status=PASS", out)
        self.assertIn("[VERIFY] run-end: total=2 pass=2", out)

    def test_runner_emits_child_markers(self) -> None:
        """ItemResult.children 이 있으면 부모 종료 전 child-result 마커 emit."""
        from verify_lib.registry import ItemResult, ItemStatus
        @self.registry.verify_item(id="MARKER-PARENT", phase=99, category="유닛", name="parent")
        def _p(ctx) -> ItemResult:
            return ItemResult(
                id="MARKER-PARENT", name="parent", status=ItemStatus.PASS,
                children=[
                    ItemResult(id="C-01", name="child1", status=ItemStatus.PASS, elapsed_ms=10),
                    ItemResult(id="C-02", name="child2", status=ItemStatus.FAIL, elapsed_ms=20),
                ],
            )

        import io, tempfile
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            ctx = self._make_ctx(td)
            try:
                with redirect_stdout(buf):
                    self.runner.run_items(ctx, ["MARKER-PARENT"])
            finally:
                ctx.report_close()
        out = buf.getvalue()
        self.assertIn("[VERIFY] child-result: MARKER-PARENT.C-01 status=PASS elapsed_ms=10 name=child1", out)
        self.assertIn("[VERIFY] child-result: MARKER-PARENT.C-02 status=FAIL elapsed_ms=20 name=child2", out)
        # 부모 종료 마커는 자식들 다음에 와야 함
        idx_child2 = out.index("child-result: MARKER-PARENT.C-02")
        idx_end = out.index("item-end: MARKER-PARENT")
        self.assertLess(idx_child2, idx_end, "child-result 가 item-end 보다 먼저 emit 되어야 함")

    def test_runner_emits_skip_markers_for_dep_fail(self) -> None:
        """의존 항목 FAIL 시 후속 항목 SKIP 도 마커 emit."""
        from verify_lib.registry import ItemResult, ItemStatus
        @self.registry.verify_item(id="MARKER-DEP", phase=99, category="유닛", name="dep")
        def _d(ctx) -> ItemResult:
            return ItemResult(id="MARKER-DEP", name="dep", status=ItemStatus.FAIL)
        @self.registry.verify_item(id="MARKER-USER", phase=99, category="유닛", name="user",
                                    depends_on=["MARKER-DEP"])
        def _u(ctx) -> ItemResult:
            return ItemResult(id="MARKER-USER", name="user", status=ItemStatus.PASS)

        import io, tempfile
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            ctx = self._make_ctx(td)
            try:
                with redirect_stdout(buf):
                    self.runner.run_items(ctx, ["MARKER-DEP", "MARKER-USER"])
            finally:
                ctx.report_close()
        out = buf.getvalue()
        self.assertIn("[VERIFY] item-end: MARKER-DEP status=FAIL", out)
        self.assertIn("[VERIFY] item-end: MARKER-USER status=SKIP", out)


class TestOnlyChildren(unittest.TestCase):
    """모듈 자식 항목 부분 실행 인프라 — TestRunner.only_ids,
    VerifyContext.only_children_for, cims_verify._parse_only_children."""

    def test_parse_only_children_kv_form(self) -> None:
        from cims_verify import _parse_only_children
        out = _parse_only_children(["MODULE-CSC=CSC-AUTH-01,CSC-USER-01"])
        self.assertEqual(out, {"MODULE-CSC": ["CSC-AUTH-01", "CSC-USER-01"]})

    def test_parse_only_children_json_form(self) -> None:
        from cims_verify import _parse_only_children
        out = _parse_only_children(['{"MODULE-CSC":["CSC-AUTH-01"],"MODULE-CMP":["CMP-CMD-01"]}'])
        self.assertEqual(out, {"MODULE-CSC": ["CSC-AUTH-01"], "MODULE-CMP": ["CMP-CMD-01"]})

    def test_parse_only_children_multiple_specs(self) -> None:
        from cims_verify import _parse_only_children
        out = _parse_only_children([
            "MODULE-CSC=CSC-AUTH-01",
            "MODULE-CSC=CSC-USER-01,CSC-USER-02",
            "MODULE-CMP=CMP-CMD-01",
        ])
        self.assertEqual(set(out.keys()), {"MODULE-CSC", "MODULE-CMP"})
        self.assertEqual(set(out["MODULE-CSC"]), {"CSC-AUTH-01", "CSC-USER-01", "CSC-USER-02"})
        self.assertEqual(out["MODULE-CMP"], ["CMP-CMD-01"])

    def test_parse_only_children_empty(self) -> None:
        from cims_verify import _parse_only_children
        self.assertEqual(_parse_only_children(None), {})
        self.assertEqual(_parse_only_children([]), {})

    def test_context_only_children_for(self) -> None:
        from verify_lib.context import VerifyContext
        ctx = VerifyContext.create(
            repo_root=os.path.dirname(_THIS_DIR), phase=1,
            opts={"only_children": {"MODULE-CSC": ["CSC-AUTH-01", "CSC-AUTH-02"]}},
        )
        try:
            self.assertEqual(ctx.only_children_for("MODULE-CSC"),
                             {"CSC-AUTH-01", "CSC-AUTH-02"})
            self.assertIsNone(ctx.only_children_for("MODULE-CMP"))  # 미지정
            self.assertIsNone(ctx.only_children_for("UNKNOWN"))
        finally:
            ctx.report_close()
            try: os.remove(ctx.report_path)
            except OSError: pass

    def test_test_runner_only_ids_skips_others(self) -> None:
        """tests/conftest.py 의 TestRunner — only_ids 지정 시 미포함 ID 는 SKIP."""
        from conftest import TestRunner
        runner = TestRunner("UNIT", only_ids={"X-01", "X-03"})
        runner.run("X-01", "선택", lambda: (True, "ok"))
        runner.run("X-02", "미선택", lambda: (True, "should be skipped"))
        runner.run("X-03", "선택2", lambda: (False, "fail"))
        s = runner.summary()
        by_id = {r["id"]: r for r in s["results"]}
        self.assertEqual(by_id["X-01"]["status"], "PASS")
        self.assertEqual(by_id["X-02"]["status"], "SKIP")
        self.assertEqual(by_id["X-03"]["status"], "FAIL")

    def test_test_runner_only_ids_none_runs_all(self) -> None:
        from conftest import TestRunner
        runner = TestRunner("UNIT", only_ids=None)
        runner.run("X-01", "all-1", lambda: (True, ""))
        runner.run("X-02", "all-2", lambda: (True, ""))
        s = runner.summary()
        self.assertEqual(s["pass"], 2)
        self.assertEqual(s["skip"], 0)


class TestParseItemsProgress(unittest.TestCase):
    """csc/src/handlers/verification.py _parse_items_progress 의 stdout 파싱.

    runner.py 의 [VERIFY] item-start/item-end/child-result 마커 +
    cims.sh _verify_phase2 의 [VERIFY] step-start/step-end 마커 정규식 검증.
    """

    def setUp(self) -> None:
        # csc handler 모듈 import — sys.path 추가 필요
        repo_root = os.path.dirname(_THIS_DIR)
        csc_src = os.path.join(repo_root, "csc", "src")
        if csc_src not in sys.path:
            sys.path.insert(0, csc_src)
        # httpsrv handler 인터페이스 (없으면 stub) — _parse_items_progress 만 사용하니
        # ImportError 시 직접 모듈 한정 import
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
            "[VERIFY] item-start: A idx=1/2 name=alpha",
            "[VERIFY] item-end: A status=PASS elapsed_ms=320",
            "[VERIFY] item-start: B idx=2/2 name=beta",
            "[VERIFY] item-end: B status=FAIL elapsed_ms=100",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertEqual(p["total"], 2)
        self.assertEqual(p["completed"], 2)
        self.assertEqual(p["selected"], ["A", "B"])
        self.assertEqual(len(p["items"]), 2)
        self.assertEqual(p["items"][0]["status"], "PASS")
        self.assertEqual(p["items"][0]["elapsed_ms"], 320)
        self.assertEqual(p["items"][1]["status"], "FAIL")
        self.assertIsNone(p["current"])

    def test_parse_running_state(self) -> None:
        """item-end 가 아직 안 온 항목은 status=RUNNING + current 로 표시."""
        log = self._write_log([
            "[VERIFY] run-start: total=2 ids=A,B",
            "[VERIFY] item-start: A idx=1/2 name=alpha",
            "[VERIFY] item-end: A status=PASS elapsed_ms=10",
            "[VERIFY] item-start: B idx=2/2 name=beta",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertEqual(p["completed"], 1)
        self.assertEqual(p["current"], "B")
        self.assertEqual(p["items"][1]["status"], "RUNNING")

    def test_parse_children(self) -> None:
        log = self._write_log([
            "[VERIFY] run-start: total=1 ids=MODULE-CSC",
            "[VERIFY] item-start: MODULE-CSC idx=1/1 name=CSC 단위 테스트",
            "[VERIFY] child-result: MODULE-CSC.CSC-AUTH-01 status=PASS elapsed_ms=320 name=로그인 성공",
            "[VERIFY] child-result: MODULE-CSC.CSC-AUTH-02 status=FAIL elapsed_ms=10 name=로그인 실패",
            "[VERIFY] item-end: MODULE-CSC status=FAIL elapsed_ms=12345",
        ])
        p = self.verification._parse_items_progress(log)
        parent = p["items"][0]
        self.assertEqual(parent["id"], "MODULE-CSC")
        self.assertEqual(parent["status"], "FAIL")
        self.assertEqual(len(parent["children"]), 2)
        self.assertEqual(parent["children"][0]["id"], "CSC-AUTH-01")
        self.assertEqual(parent["children"][0]["status"], "PASS")
        self.assertEqual(parent["children"][1]["status"], "FAIL")

    def test_parse_phase2_steps_to_children(self) -> None:
        """Phase 2 의 step-start/step-end 가 P2-RUN-ALL 의 children 으로 흡수."""
        log = self._write_log([
            "[VERIFY] item-start: P2-RUN-ALL idx=1/1 name=Phase2 22단계",
            "[VERIFY] step-start: 01 Cleanup",
            "[VERIFY] step-end: 01 status=PASS elapsed_ms=500",
            "[VERIFY] step-start: 02 Build",
            "[VERIFY] step-end: 02 status=PASS elapsed_ms=15000",
            "[VERIFY] step-start: 03 Configure",
            "[VERIFY] item-end: P2-RUN-ALL status=FAIL elapsed_ms=20000",
        ])
        p = self.verification._parse_items_progress(log)
        parent = p["items"][0]
        self.assertEqual(parent["id"], "P2-RUN-ALL")
        self.assertEqual(parent["status"], "FAIL")
        # 자식: 01 PASS, 02 PASS, 03 RUNNING (step-end 없음)
        ids = [c["id"] for c in parent["children"]]
        self.assertEqual(ids, ["P2-01", "P2-02", "P2-03"])
        self.assertEqual(parent["children"][0]["status"], "PASS")
        self.assertEqual(parent["children"][1]["elapsed_ms"], 15000)
        self.assertEqual(parent["children"][2]["status"], "RUNNING")

    def test_parse_strips_ansi(self) -> None:
        """ANSI 컬러 코드가 섞여 있어도 파싱 가능."""
        log = self._write_log([
            "\x1b[32m[VERIFY] run-start: total=1 ids=A\x1b[0m",
            "\x1b[36m[VERIFY] item-start: A idx=1/1 name=test\x1b[0m",
            "[VERIFY] item-end: A status=PASS elapsed_ms=5",
        ])
        p = self.verification._parse_items_progress(log)
        self.assertEqual(p["total"], 1)
        self.assertEqual(p["items"][0]["status"], "PASS")

    def test_parse_empty_log(self) -> None:
        p = self.verification._parse_items_progress("/nonexistent/path")
        self.assertEqual(p["total"], 0)
        self.assertEqual(p["items"], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
