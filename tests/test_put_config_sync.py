"""
deployments/{id}/config sync_keys 선택 전파 (R2) 단위 테스트.

Covers (handlers.agents 직접 호출 — 서버 미기동):
  - _put_deployment_config: sync_keys 부재(레거시 통짜 전파) / 부분집합 merge /
    빈 배열(피어 무변경) / sync_checked 영속(ha_group.config_sync) / standalone /
    AA 3멤버 fan-out / per-target job config 실체화
  - _get_deployment_config: ha block (그룹 멤버) / null (standalone)

각 테스트는 tmpdir 로 CimsRuntimeDir 격리. sys.path 는 ems/core/oam/{src,vendor}
(csc→oam 분리 이후 배포 admin 핸들러는 oam 소유 — test_ha_fanout.py 의 csc/src 와 다름).
"""
import asyncio
import os
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

# 같은 프로세스에서 다른 테스트(예: test_ha_fanout)가 csc/src 의 services 를 먼저
# import 했을 수 있다 — oam 쪽 모듈로 재해석되도록 관련 모듈 캐시를 비운다.
for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))


TEMPLATE = {
    "version": 1,
    "sections": [
        {"key": "svc", "title": "Service", "scope": "service",
         "fields": [
             {"key": "Db.Host", "type": "string", "default": "127.0.0.1"},
             {"key": "Db.Port", "type": "int", "default": 3306},
         ]},
        {"key": "sys", "title": "System", "scope": "system",
         "fields": [
             {"key": "SystemId", "type": "string", "default": ""},
             {"key": "LocalIp", "type": "string", "default": ""},
         ]},
    ],
}


class _FsCase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.config = {"CimsRuntimeDir": self._td.name}

    def tearDown(self):
        self._td.cleanup()

    # ── file_store seed helpers ─────────────────────────────────────

    def _seed_pkg(self, pid, name, version="0.1.0", template=None):
        from services import file_store
        d = file_store.domain_dir(self.config, "packages")
        file_store.save(d, pid, {"id": pid, "name": name, "version": version,
                                 "config_template": template or TEMPLATE})

    def _seed_agent(self, aid, name):
        from services import file_store
        d = file_store.domain_dir(self.config, "agents")
        file_store.save(d, aid, {"id": aid, "name": name})

    def _seed_deployment(self, did, agent_id, package_id, config=None):
        from services import file_store
        d = file_store.domain_dir(self.config, "deployments")
        row = {"id": did, "agent_id": agent_id, "package_id": package_id,
               "install_path": f"/opt/cims/dep{did}"}
        if config is not None:
            row["config"] = config
        file_store.save(d, did, row)

    def _seed_ha_group(self, gid, name, mode, member_agent_ids):
        from services import file_store
        d = file_store.domain_dir(self.config, "ha_groups")
        members = [{"agent_id": aid,
                    "role": ("master" if i == 0 else "backup"),
                    "priority": 100 - i}
                   for i, aid in enumerate(member_agent_ids)]
        file_store.save(d, gid, {"id": gid, "name": name, "mode": mode,
                                 "members": members})

    # ── handler call / read helpers ─────────────────────────────────

    def _put(self, did, body):
        from handlers.agents import _put_deployment_config
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="PUT", full_path=f"/api/v1/deployments/{did}/config",
                         client_ip="127.0.0.1", client_port=0, body=body)
        return asyncio.run(_put_deployment_config(ha, did, self.config))

    def _get(self, did):
        from handlers.agents import _get_deployment_config
        return asyncio.run(_get_deployment_config(did, self.config))

    def _dep(self, did):
        from services import file_store
        return file_store.by_id(file_store.domain_dir(self.config, "deployments"), did)

    def _jobs(self):
        from services import file_store
        return file_store.load_all(file_store.domain_dir(self.config, "jobs"))

    def _group(self, gid):
        from services import file_store
        return file_store.by_id(file_store.domain_dir(self.config, "ha_groups"), gid)

    def _seed_as_pair(self):
        """표준 시나리오 — AS 그룹(1: control) 멤버 agent 10/11, csp dep 5/6.
        피어(dep 6)는 고유 overlay {SystemId: B, Db.Host: 10.0.0.1} 보유."""
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "ctrl-a")
        self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(1, "control", "active_standby", [10, 11])
        self._seed_deployment(5, 10, 1, config={"SystemId": "A"})
        self._seed_deployment(6, 11, 1, config={"SystemId": "B", "Db.Host": "10.0.0.1"})


class TestPutConfigSync(_FsCase):

    def test_legacy_no_sync_keys_full_propagate(self):
        """(a) sync_keys 부재 = 레거시 — 피어 overlay 를 values 로 통짜 교체 (회귀 고정)."""
        self._seed_as_pair()
        r = self._put(5, {"config": {"Db.Host": "10.9.9.9", "SystemId": "A"}})
        self.assertEqual(r.status, 200)
        self.assertTrue(r.body["propagated"])
        self.assertIsNone(r.body["sync_keys_applied"])
        self.assertEqual(self._dep(5)["config"], {"Db.Host": "10.9.9.9", "SystemId": "A"})
        # 통짜 — 피어 고유 SystemId=B 소실 (레거시 계약)
        self.assertEqual(self._dep(6)["config"], {"Db.Host": "10.9.9.9", "SystemId": "A"})
        self.assertEqual(len(self._jobs()), 2)

    def test_sync_keys_subset_merges_peer(self):
        """(b) sync_keys 부분집합 — 피어는 해당 키만 merge, 고유 키 보존.
        job config 는 per-target 실체화 (피어 job 에 피어의 SystemId)."""
        self._seed_as_pair()
        r = self._put(5, {"config": {"Db.Host": "10.9.9.9", "SystemId": "A"},
                          "sync_keys": ["Db.Host"],
                          "sync_checked": ["Db.Host", "Db.Port"]})
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["sync_keys_applied"], ["Db.Host"])
        self.assertEqual(self._dep(5)["config"], {"Db.Host": "10.9.9.9", "SystemId": "A"})
        self.assertEqual(self._dep(6)["config"], {"Db.Host": "10.9.9.9", "SystemId": "B"})
        jobs = {j["params"]["deployment_id"]: j for j in self._jobs()}
        self.assertEqual(set(jobs), {5, 6})
        # per-target 실체화 — template default(Db.Port) + 각자의 overlay
        self.assertEqual(jobs[5]["params"]["config"]["SystemId"], "A")
        self.assertEqual(jobs[6]["params"]["config"]["SystemId"], "B")
        self.assertEqual(jobs[6]["params"]["config"]["Db.Host"], "10.9.9.9")
        self.assertEqual(jobs[6]["params"]["config"]["Db.Port"], 3306)

    def test_empty_sync_keys_peer_untouched(self):
        """(c) sync_keys=[] — 피어 무변경, job 은 요청 dep 1건만, sync_txn 없음."""
        self._seed_as_pair()
        r = self._put(5, {"config": {"Db.Host": "10.9.9.9", "SystemId": "A"},
                          "sync_keys": []})
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["sync_keys_applied"], [])
        self.assertFalse(r.body["propagated"])
        self.assertIsNone(r.body["sync_id"])
        self.assertEqual(self._dep(6)["config"], {"SystemId": "B", "Db.Host": "10.0.0.1"})
        jobs = self._jobs()
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["params"]["deployment_id"], 5)

    def test_sync_checked_persisted_and_exposed(self):
        """(d) sync_checked → ha_group.config_sync[pkg] 영속 + GET ha.sync_keys 반영."""
        self._seed_as_pair()
        self._put(5, {"config": {"SystemId": "A"},
                      "sync_keys": [], "sync_checked": ["Db.Host", "Db.Port"]})
        g = self._group(1)
        self.assertEqual(g["config_sync"], {"csp": ["Db.Host", "Db.Port"]})
        view = self._get(5)
        self.assertEqual(view.status, 200)
        ha = view.body["ha"]
        self.assertEqual(ha["group_id"], 1)
        self.assertEqual(ha["mode"], "active_standby")
        self.assertEqual(ha["sync_keys"], ["Db.Host", "Db.Port"])
        self.assertEqual({m["deployment_id"] for m in ha["members"]}, {5, 6})
        self.assertEqual({m["agent_name"] for m in ha["members"]}, {"ctrl-a", "ctrl-b"})

    def test_standalone_no_group(self):
        """(e) standalone — 전파/영속 없음, GET ha=null."""
        self._seed_pkg(1, "csp")
        self._seed_agent(20, "solo")
        self._seed_deployment(7, 20, 1)
        r = self._put(7, {"config": {"Db.Host": "10.1.1.1"},
                          "sync_keys": [], "sync_checked": ["Db.Host"]})
        self.assertEqual(r.status, 200)
        self.assertIsNone(r.body["ha_group_id"])
        self.assertEqual(self._dep(7)["config"], {"Db.Host": "10.1.1.1"})
        view = self._get(7)
        self.assertIsNone(view.body["ha"])

    def test_all_active_three_members(self):
        """(f) AA 3멤버 — 피어 2명 모두 sync_keys merge."""
        self._seed_pkg(2, "cmp", template=TEMPLATE)
        for aid, name in ((30, "media-a"), (31, "media-b"), (32, "media-c")):
            self._seed_agent(aid, name)
        self._seed_ha_group(2, "media", "all_active", [30, 31, 32])
        self._seed_deployment(8, 30, 2, config={"SystemId": "M1"})
        self._seed_deployment(9, 31, 2, config={"SystemId": "M2"})
        self._seed_deployment(10, 32, 2, config={"SystemId": "M3"})
        r = self._put(8, {"config": {"Db.Host": "10.2.2.2", "SystemId": "M1"},
                          "sync_keys": ["Db.Host"]})
        self.assertEqual(r.status, 200)
        self.assertTrue(r.body["propagated"])
        self.assertEqual(self._dep(9)["config"], {"SystemId": "M2", "Db.Host": "10.2.2.2"})
        self.assertEqual(self._dep(10)["config"], {"SystemId": "M3", "Db.Host": "10.2.2.2"})
        self.assertEqual(len(self._jobs()), 3)

    def test_sync_keys_invalid_type_400(self):
        """sync_keys 가 list 아님 → 400."""
        self._seed_as_pair()
        r = self._put(5, {"config": {"SystemId": "A"}, "sync_keys": "Db.Host"})
        self.assertEqual(r.status, 400)


if __name__ == "__main__":
    unittest.main()
