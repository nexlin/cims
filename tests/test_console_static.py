"""
콘솔 정적 디렉토리 해석 단위 테스트 (handlers.console_static.resolve_console_static_dir).

콘솔 번들은 oam 패키지 동봉본 **하나**다 — 서비스 모듈 패키지(oam-svc·oam-cims-tester)는
콘솔을 동봉하지 않으므로 해석 후보에 다른 모듈의 설치 트리가 있어서는 안 된다
(test_instrument.md §7 base 확장 ②, oam_base_service_split D1).

Covers:
  - Console.StaticDir(절대/상대) 가 있으면 그것 — 없는 경로면 ''(서빙 비활성, 폴백 없음)
  - 동봉본 <root>/console/dist 우선, 없으면 개발 트리 형제 <root>/../console/dist
  - 설치 트리에 oam-svc 동봉 콘솔이 있어도 **후보가 아니다**
  - 아무 것도 없으면 ''
"""
import os
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from handlers.console_static import resolve_console_static_dir  # noqa: E402


class TestResolveConsoleStaticDir(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        # 설치 레이아웃: <modules>/oam/<ver>/oam  (component_root)
        self.modules = os.path.join(self._td.name, "modules")
        self.root = os.path.join(self.modules, "oam", "0.2.0", "oam")
        os.makedirs(self.root)

    def tearDown(self):
        self._td.cleanup()

    def _mk(self, *parts):
        p = os.path.join(*parts)
        os.makedirs(p, exist_ok=True)
        return os.path.normpath(p)

    def test_static_dir_config_wins_and_no_fallback_when_missing(self):
        bundled = self._mk(self.root, "console", "dist")
        explicit = self._mk(self._td.name, "elsewhere", "dist")
        self.assertEqual(resolve_console_static_dir({"Console": {"StaticDir": explicit}}, self.root), explicit)
        # 상대 경로는 component_root 기준
        self.assertEqual(resolve_console_static_dir({"Console": {"StaticDir": "console/dist"}}, self.root), bundled)
        # 명시했는데 없으면 폴백하지 않는다(설정 오류가 조용히 다른 번들로 가려지지 않게)
        self.assertEqual(resolve_console_static_dir({"Console": {"StaticDir": "/nonexistent/x"}}, self.root), "")

    def test_bundled_before_flat_sibling(self):
        flat = self._mk(self.root, "..", "console", "dist")
        self.assertEqual(resolve_console_static_dir({}, self.root), flat)
        bundled = self._mk(self.root, "console", "dist")
        self.assertEqual(resolve_console_static_dir({}, self.root), bundled)

    def test_service_module_console_is_not_a_candidate(self):
        # 과거 규칙(oam-svc 동봉 콘솔 우선)의 트리를 만들어 둔다 — 지금은 후보가 아니어야 한다.
        self._mk(self.modules, "oam-svc", "0.2.106", "oam-svc", "console", "dist")
        self._mk(self.modules, "oam-cims-tester", "0.1.0", "oam-cims-tester", "console", "dist")
        self.assertEqual(resolve_console_static_dir({}, self.root), "")
        bundled = self._mk(self.root, "console", "dist")
        self.assertEqual(resolve_console_static_dir({}, self.root), bundled)

    def test_nothing_found(self):
        self.assertEqual(resolve_console_static_dir({}, self.root), "")


if __name__ == "__main__":
    unittest.main()
