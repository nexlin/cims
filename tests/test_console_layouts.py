"""
콘솔 레이아웃 영속 API 단위 테스트 (handlers.console 직접 호출 — 서버 미기동).

Covers:
  - GET  미저장 → 200 {found: false} (프론트 seed fallback 흐름. 404 금지)
  - PUT  widgets[] 통째 보존 — placement 의 x/y/w/h·config·title 확장 필드 유실 없음
  - PUT  top-level 화이트리스트 — id/title/widgets/gap/seedVersion 만. seedVersion 은
         "이 배치가 어느 seed 세대 기준인가" 각인이라 유실되면 콘솔이 개편 안내를 매번
         다시 띄운다(console_platform.md §3.4).
  - PUT  잘못된 seedVersion(bool/문자열)은 무시, widgets[] 없으면 400
  - DELETE → 삭제 후 GET 이 다시 미저장 sentinel

각 테스트는 tmpdir 로 CimsRuntimeDir 격리. sys.path 는 ems/core/oam/{src,vendor}.
"""
import asyncio
import os
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

# 같은 프로세스에서 다른 테스트가 csc/src 의 services 를 먼저 import 했을 수 있다
# — oam 쪽 모듈로 재해석되도록 관련 모듈 캐시를 비운다.
for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))


class TestConsoleLayouts(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.config = {"CimsRuntimeDir": self._td.name}
        # 관리 store 는 단일 writer — write 는 소유권 리스를 요구한다(oam_ha.md §4.4).
        # 실기동(oam_app)이 bind 전에 잡는 자리와 같게 잡아 실제 write 경로를 통과시킨다.
        from services import file_store, lease
        self._lease = lease
        st = lease.acquire(file_store.runtime_root(self.config))
        if not st.get('active'):
            self.skipTest(f"store 리스 획득 불가({st.get('reason')}) — flock 미지원 tmpdir")

    def tearDown(self):
        self._lease.release()
        self._td.cleanup()

    def _call(self, method, path, body=None):
        from handlers.console import handle_console
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method=method, full_path=path,
                         client_ip="127.0.0.1", client_port=0, body=body)
        return asyncio.run(handle_console(ha, {"config": self.config}))

    def _get(self, lid):
        return self._call("GET", f"/api/v1/console/layouts/{lid}")

    def _put(self, lid, body):
        return self._call("PUT", f"/api/v1/console/layouts/{lid}", body)

    def test_get_unsaved_returns_sentinel(self):
        res = self._get("dashboard")
        self.assertEqual(res.status, 200)
        self.assertFalse(res.body.get("found"))
        self.assertNotIn("widgets", res.body)

    def test_put_preserves_placement_extras(self):
        widgets = [
            {"widgetId": "cims.stat.subscribers", "x": 0, "y": 49, "w": 7, "h": 6},
            {"widgetId": "shape.time-bar", "x": 0, "y": 71, "w": 48, "h": 15,
             "config": {"source": "cims.svc.volte"}, "title": "VoLTE 추이"},
        ]
        res = self._put("dashboard", {"title": "대시보드", "widgets": widgets,
                                      "gap": 8, "seedVersion": 2})
        self.assertEqual(res.status, 200)
        self.assertEqual(res.body["widgets"], widgets)      # 필드 필터 없이 통째 보존
        self.assertEqual(res.body["gap"], 8)
        self.assertEqual(res.body["seedVersion"], 2)

        got = self._get("dashboard")                        # 재조회에도 남아 있어야 한다
        self.assertEqual(got.body["seedVersion"], 2)
        self.assertEqual(got.body["widgets"][1]["config"], {"source": "cims.svc.volte"})
        self.assertEqual(got.body["widgets"][1]["title"], "VoLTE 추이")

    def test_put_drops_unknown_toplevel_and_bad_seed_version(self):
        res = self._put("stats.volte", {"widgets": [{"widgetId": "shape.kpi"}],
                                        "seedVersion": True, "bogus": "x"})
        self.assertEqual(res.status, 200)
        self.assertNotIn("seedVersion", res.body)           # bool 은 세대 번호가 아니다
        self.assertNotIn("bogus", res.body)

        res = self._put("stats.ptt", {"widgets": [{"widgetId": "shape.kpi"}],
                                      "seedVersion": "2"})
        self.assertNotIn("seedVersion", res.body)

    def test_put_requires_widgets(self):
        res = self._put("stats.sip", {"title": "SIP"})
        self.assertEqual(res.status, 400)

    def test_delete_resets_to_seed(self):
        self._put("service.status", {"widgets": [{"widgetId": "cims.svc-trend"}], "seedVersion": 2})
        res = self._call("DELETE", "/api/v1/console/layouts/service.status")
        self.assertEqual(res.status, 200)
        self.assertTrue(res.body["deleted"])
        self.assertFalse(self._get("service.status").body.get("found"))

    def test_list_layouts(self):
        self._put("dashboard", {"widgets": [{"widgetId": "cims.stat.rtp-ptt"}], "seedVersion": 2})
        res = self._call("GET", "/api/v1/console/layouts")
        self.assertEqual(res.status, 200)
        ids = [row["id"] for row in res.body["layouts"]]
        self.assertIn("dashboard", ids)


if __name__ == "__main__":
    unittest.main()
