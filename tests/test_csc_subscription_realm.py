"""csc/src/handlers/admin.py `_service_realm` — 가입 번호 H(A1) 결박용 (domain, realm) 해석 단위 테스트 (오프라인).

sip_access_security.md §4.1: ① 정본 `access_services` 컬렉션(csp 소유, runtime store) → ② csc.json
`Provisioning.Services.<kind>` 미러. 어느 쪽도 없으면 None(API 400). 종전에는 한 번도 초기화되지
않는 config_cache('service'=sip_service)만 봐서 passwd 가 있는 모든 번호 추가/변경이 400 이었다.

  python3 -m unittest tests.test_csc_subscription_realm
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import types
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_CSC_SRC = os.path.join(_REPO_ROOT, "csc", "src")


def _stub(name: str, **attrs):
    """스텁 모듈 보장 — 같은 프로세스에서 다른 시험(test_csc_dispatch_rbac)이 먼저 만든 스텁이 있으면
    빠진 속성만 채운다(한 unittest 실행에 두 모듈이 함께 돌 때)."""
    m = sys.modules.get(name)
    if m is None:
        m = types.ModuleType(name)
        sys.modules[name] = m
    for k, v in attrs.items():
        if not hasattr(m, k):
            setattr(m, k, v)
    return m


def _load_admin():
    """handlers/admin.py 를 파일 경로로 적재 — DB·MCPTT·AuC 의존은 스텁."""
    if _CSC_SRC not in sys.path:
        sys.path.insert(0, _CSC_SRC)
    for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
        if os.path.isdir(_v) and _v not in sys.path:
            sys.path.append(_v)
            break
    import services  # noqa: F401
    import handlers  # noqa: F401

    class _Log:
        def log_warning(self, *a, **k): pass
        def log_info(self, *a, **k): pass
        def log_error(self, *a, **k): pass

    _stub("services.mcptt", notify_csp=lambda *a, **k: None, refresh_group_members=lambda *a, **k: None,
          DEFAULT_USER_PROFILE={}, update_user_profile_cache=lambda *a, **k: None, SERVICE_CONFIG_DEFAULTS={},
          get_service_config=lambda *a, **k: {}, update_service_config_cache=lambda *a, **k: None,
          get_service_config_xml=lambda *a, **k: "", logger=_Log())
    _stub("services.auc", auc=types.SimpleNamespace())
    _stub("handlers.dispatch", dispatch_group_of_user=lambda *a, **k: None)
    spec = importlib.util.spec_from_file_location(
        "handlers_admin_under_test", os.path.join(_CSC_SRC, "handlers", "admin.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ServiceRealmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.a = _load_admin()
        cls.tmp = tempfile.mkdtemp(prefix="csc_realm_")

    def _config(self, runtime_dir: str | None = None, services: dict | None = None) -> dict:
        cfg = {"CimsRuntimeDir": runtime_dir or os.path.join(self.tmp, "no-such-runtime")}
        if services is not None:
            cfg["Provisioning"] = {"Services": services}
        return cfg

    def _write_access_services(self, runtime_dir: str, rows: list[dict]):
        # ha_lookup: 비표준 runtime_root → {runtime}/collections/<owner>/<name>
        d = os.path.join(runtime_dir, "collections", "csp", "access_services")
        os.makedirs(d, exist_ok=True)
        for i, r in enumerate(rows):
            with open(os.path.join(d, f"{i + 1}.json"), "w", encoding="utf-8") as f:
                json.dump(r, f)

    def test_kind_mapping(self):
        self.assertEqual(self.a._service_kind("call"), "volte")
        self.assertEqual(self.a._service_kind("ptt"), "ptt")

    def test_none_without_service_ref(self):
        cfg = self._config(services={"ptt": {"domain": "ptt.example"}})
        self.assertIsNone(self.a._service_realm(cfg, None, "ptt"))
        self.assertIsNone(self.a._service_realm(cfg, "", "ptt"))

    def test_fallback_to_provisioning_services(self):
        cfg = self._config(services={"volte": {"domain": "ims.mnc033.mcc450.3gppnetwork.org"},
                                     "ptt": {"domain": "ptt.mnc033.mcc450.3gppnetwork.org"}})
        self.assertEqual(self.a._service_realm(cfg, "mcptt", "ptt"),
                         ("ptt.mnc033.mcc450.3gppnetwork.org", "ptt.mnc033.mcc450.3gppnetwork.org"))
        self.assertEqual(self.a._service_realm(cfg, "volte", "volte"),
                         ("ims.mnc033.mcc450.3gppnetwork.org", "ims.mnc033.mcc450.3gppnetwork.org"))

    def test_fallback_auth_realm_override(self):
        cfg = self._config(services={"ptt": {"domain": "ptt.example", "auth_realm": "auth.example"}})
        self.assertEqual(self.a._service_realm(cfg, "mcptt", "ptt"), ("ptt.example", "auth.example"))

    def test_none_when_neither_source_has_it(self):
        cfg = self._config(services={"volte": {"domain": "ims.example"}})
        self.assertIsNone(self.a._service_realm(cfg, "mcptt", "ptt"))
        self.assertIsNone(self.a._service_realm({}, "mcptt", "ptt"))

    def test_access_services_collection_wins(self):
        rt = os.path.join(self.tmp, "rt1")
        self._write_access_services(rt, [
            {"name": "volte", "domain": "ims.sot", "auth_realm": ""},
            {"name": "mcptt", "domain": "ptt.sot", "auth_realm": "ptt.realm"},
        ])
        cfg = self._config(runtime_dir=rt, services={"ptt": {"domain": "ptt.mirror"}})
        self.assertEqual(self.a._service_realm(cfg, "mcptt", "ptt"), ("ptt.sot", "ptt.realm"))
        self.assertEqual(self.a._service_realm(cfg, "volte", "volte"), ("ims.sot", "ims.sot"))
        # 컬렉션에 없는 이름은 미러로
        self.assertEqual(self.a._service_realm(cfg, "other", "ptt"), ("ptt.mirror", "ptt.mirror"))

    def test_digest_ha1_binding(self):
        import hashlib
        h = self.a._digest_ha1("+82510001001", "ptt.example", "ptt.example", "1234")
        self.assertEqual(h, hashlib.md5(b"+82510001001@ptt.example:ptt.example:1234").hexdigest())


if __name__ == "__main__":
    unittest.main()
