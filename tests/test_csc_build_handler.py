"""csc/src/handlers/build.py 단위 테스트.

- 라우팅 매칭 (run / pkg / jobs / manifest / packages / packages/<m>)
- 인증 게이트 (require_admin) — 401/403
- 모듈 화이트리스트 422
- 동시 실행 가드 409
- manifest 부재 시 packages fallback / manifest 200
- 패키지 다운로드 404 / 200 + 헤더
"""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import sys
import tempfile
import types
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _ensure_csc_src_on_path() -> None:
    csc_src = os.path.join(_REPO_ROOT, "csc", "src")
    if csc_src not in sys.path:
        sys.path.insert(0, csc_src)
    if "httpsrv" not in sys.modules:
        ha_mod = types.ModuleType("httpsrv")
        hh_mod = types.ModuleType("httpsrv.handler")

        class _HA:
            def __init__(self, **kw):
                self.method = kw.get("method", "GET")
                self.full_path = kw.get("full_path", "/")
                self.headers = kw.get("headers", {})
                self.query_params = kw.get("query_params", {})
                self.body = kw.get("body")

        class _HR:
            def __init__(self, status=200, body=None, headers=None, media_type=None):
                self.status = status
                self.body = body
                self.headers = headers or {}
                self.media_type = media_type

        hh_mod.HandlerArgs = _HA
        hh_mod.HandlerResult = _HR
        sys.modules["httpsrv"] = ha_mod
        sys.modules["httpsrv.handler"] = hh_mod


def _async_run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


class TestBuildHandler(unittest.TestCase):
    def setUp(self) -> None:
        _ensure_csc_src_on_path()
        import importlib
        try:
            self.b = importlib.import_module("handlers.build")
        except Exception as e:
            self.skipTest(f"handlers.build import 실패: {e}")

        self._td = tempfile.mkdtemp(prefix="build_handler_test_")
        # cims.sh 가 있어야 _start_build 가 진행되므로 dummy 생성
        with open(os.path.join(self._td, "cims.sh"), "w") as f:
            f.write("#!/bin/sh\necho ok\n")
        os.chmod(os.path.join(self._td, "cims.sh"), 0o755)
        os.makedirs(os.path.join(self._td, "build", "dist", "packages"), exist_ok=True)
        self.b.init(self._td)

        # require_admin 통과 stub
        self._orig_require_admin = self.b._auth.require_admin
        self.b._auth.require_admin = lambda ha: ({"role": "admin"}, None)

        # _start_job 은 실제 subprocess 띄우지 않게 stub
        self._orig_start_job = self.b._start_job
        self._next_job_id = 0
        async def _fake_start_job(kind, argv, timeout, label=""):
            self._next_job_id += 1
            jid = f"abcdef{self._next_job_id:06d}"
            self.b._JOBS[jid] = {
                "job_id": jid, "kind": kind, "label": label, "argv": argv,
                "started_at": 1000.0, "ended_at": None, "log_path": os.path.join(self._td, f"{jid}.log"),
                "returncode": None, "done": False, "verdict": None,
                "_proc": None, "_log_file": None, "_timeout": timeout,
            }
            # 빈 로그 파일 생성
            with open(self.b._JOBS[jid]["log_path"], "w") as f:
                f.write("")
            return jid
        self.b._start_job = _fake_start_job
        self.b._JOBS.clear()

    def tearDown(self) -> None:
        self.b._auth.require_admin = self._orig_require_admin
        self.b._start_job = self._orig_start_job
        self.b._JOBS.clear()
        shutil.rmtree(self._td, ignore_errors=True)

    # ── 인증 ────────────────────────────────────────────────
    def test_require_admin_blocks_unauth(self) -> None:
        from httpsrv.handler import HandlerResult
        # 원래 require_admin 으로 복귀해서 토큰 없이 호출
        self.b._auth.require_admin = self._orig_require_admin
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/manifest", headers={},
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertIsInstance(r, HandlerResult)
        self.assertEqual(r.status, 401)

    # ── 라우팅 ──────────────────────────────────────────────
    def test_unknown_path_returns_404(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/no-such",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 404)

    # ── /run ────────────────────────────────────────────────
    def test_post_run_starts_job(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/run", body={},
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 202)
        self.assertEqual(r.body["kind"], "build")
        self.assertIn(r.body["job_id"], self.b._JOBS)

    def test_concurrent_request_returns_409(self) -> None:
        # 첫 요청으로 job 시작
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/run", body={},
        )
        r1 = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r1.status, 202)
        # 두 번째 요청 — 409
        r2 = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r2.status, 409)
        self.assertIn("진행 중", r2.body.get("error", ""))

    def test_pkg_blocked_by_running_build(self) -> None:
        ha_run = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/run", body={},
        )
        _async_run(self.b.handle_build(ha_run, {}))
        ha_pkg = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/pkg", body={"module": "csp"},
        )
        r = _async_run(self.b.handle_build(ha_pkg, {}))
        self.assertEqual(r.status, 409)

    # ── /pkg 화이트리스트 ───────────────────────────────────
    def test_pkg_invalid_module_returns_422(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/pkg",
            body={"module": "not-a-module"},
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 422)
        self.assertIn("invalid module", r.body["error"])

    def test_pkg_valid_module_starts_job(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/pkg",
            body={"module": "csp"},
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 202)
        self.assertEqual(r.body["module"], "csp")
        argv = self.b._JOBS[r.body["job_id"]]["argv"]
        self.assertEqual(argv[-3:], ["pkg", "csp", "--no-bump"])

    # ── /jobs/<id> ──────────────────────────────────────────
    def test_jobs_status_returns_state(self) -> None:
        ha_run = sys.modules["httpsrv.handler"].HandlerArgs(
            method="POST", full_path="/api/v1/build/run", body={},
        )
        r = _async_run(self.b.handle_build(ha_run, {}))
        jid = r.body["job_id"]
        # status 조회
        ha_get = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path=f"/api/v1/build/jobs/{jid}",
        )
        rs = _async_run(self.b.handle_build(ha_get, {}))
        self.assertEqual(rs.status, 200)
        self.assertEqual(rs.body["job_id"], jid)
        self.assertFalse(rs.body["done"])

    def test_jobs_status_unknown_id_404(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/jobs/deadbeefdead",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 404)

    # ── /manifest ───────────────────────────────────────────
    def test_manifest_missing_returns_404(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/manifest",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 404)

    def test_manifest_present_returns_body_with_self_sha(self) -> None:
        mf = {
            "ts": "2026-05-07T12:40:35", "git": {"branch": "f", "sha": "1234567"},
            "host": "h", "packages": [{"name": "csp-0.0.1.tar.gz", "size": 100, "sha256": "ab", "mtime": ""}],
        }
        with open(os.path.join(self._td, "build/dist/packages/manifest.json"), "w") as f:
            json.dump(mf, f)
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/manifest",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["host"], "h")
        self.assertEqual(len(r.body["_self_sha256"]), 64)

    # ── /packages ───────────────────────────────────────────
    def test_packages_no_manifest_falls_back_to_scan(self) -> None:
        # tarball 만 떨궈두고 manifest 없음
        for fn in ("csp-0.0.1.tar.gz", "cmp-0.0.1.tar.gz"):
            with open(os.path.join(self._td, "build/dist/packages", fn), "w") as f:
                f.write("dummy")
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/packages",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 200)
        self.assertFalse(r.body["manifest_present"])
        names = {p["name"] for p in r.body["packages"]}
        self.assertEqual(names, {"csp-0.0.1.tar.gz", "cmp-0.0.1.tar.gz"})

    # ── /packages/<m> 다운로드 ──────────────────────────────
    def test_download_missing_module_returns_404(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/packages/csp",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 404)

    def test_download_invalid_module_returns_422(self) -> None:
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/packages/badmodule",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        # 라우터의 정규식은 [a-z]+ 만 매칭 — badmodule 은 매칭되지만 화이트리스트에서 422.
        self.assertEqual(r.status, 422)

    def test_download_returns_binary_with_attachment_header(self) -> None:
        path = os.path.join(self._td, "build/dist/packages/csp-1.2.3.tar.gz")
        payload = b"\x1f\x8b\x08binary"
        with open(path, "wb") as f:
            f.write(payload)
        ha = sys.modules["httpsrv.handler"].HandlerArgs(
            method="GET", full_path="/api/v1/build/packages/csp",
        )
        r = _async_run(self.b.handle_build(ha, {}))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, payload)
        self.assertEqual(r.headers["Content-Type"], "application/octet-stream")
        self.assertIn("attachment", r.headers["Content-Disposition"])
        self.assertIn("csp-1.2.3.tar.gz", r.headers["Content-Disposition"])
        self.assertEqual(r.headers["X-Package-Module"], "csp")


if __name__ == "__main__":
    unittest.main()
