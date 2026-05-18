"""
HA fan-out core (Phase: 흐름 B + 흐름 A 정상화) 단위 테스트.

Covers:
  - services.ha_lookup — ha_group ↔ package ↔ deployment 매핑
  - services.sync_txn — 트랜잭션 store CRUD + status 도출
  - services.sync_dispatch — 멤버 fan-out + job enqueue + txn 생성

각 테스트는 tmpdir 로 CimsRuntimeDir 격리. csc 서버 본체는 안 띄움 —
순수 함수 호출만.
"""
import os
import sys
import tempfile
import unittest

# csc/src/services 모듈을 직접 import 하기 위해 sys.path 셋업
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "csc", "src"))


class _FsCase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.config = {"CimsRuntimeDir": self._td.name}

    def tearDown(self):
        self._td.cleanup()

    def _seed_pkg(self, pid, name, version="0.1.0"):
        from services import file_store
        d = file_store.domain_dir(self.config, "packages")
        file_store.save(d, pid, {"id": pid, "name": name, "version": version})

    def _seed_agent(self, aid, name):
        from services import file_store
        d = file_store.domain_dir(self.config, "agents")
        file_store.save(d, aid, {"id": aid, "name": name})

    def _seed_deployment(self, did, agent_id, package_id, package_name=None,
                         install_path=None):
        from services import file_store
        d = file_store.domain_dir(self.config, "deployments")
        row = {"id": did, "agent_id": agent_id, "package_id": package_id,
               "install_path": install_path or f"/opt/cims/dep{did}"}
        if package_name:
            row["package_name"] = package_name
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


class HaLookupTests(_FsCase):
    def test_group_for_package_picks_member_deployment(self):
        from services import ha_lookup
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "ctrl-a"); self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(100, "Control", "active_standby", [10, 11])
        self._seed_deployment(500, 10, 1)
        self._seed_deployment(501, 11, 1)

        g = ha_lookup.ha_group_for_package(self.config, "csp")
        self.assertIsNotNone(g)
        self.assertEqual(g["id"], 100)
        self.assertEqual(g["mode"], "active_standby")

    def test_deployments_in_group_returns_both_members(self):
        from services import ha_lookup
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "ctrl-a"); self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(100, "Control", "active_standby", [10, 11])
        self._seed_deployment(500, 10, 1)
        self._seed_deployment(501, 11, 1)

        deps = ha_lookup.deployments_in_group_for_package(self.config, 100, "csp")
        self.assertEqual(len(deps), 2)
        self.assertEqual({d["agent_id"] for d in deps}, {10, 11})
        # package_name 이 enrich 되어야 함 (deployment row 에 없어도 packages 도메인 lookup)
        self.assertTrue(all(d.get("package_name") == "csp" for d in deps))

    def test_fanout_targets_for_collection_dedupes(self):
        from services import ha_lookup
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "ctrl-a"); self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(100, "Control", "active_standby", [10, 11])
        self._seed_deployment(500, 10, 1)
        self._seed_deployment(501, 11, 1)

        targets = ha_lookup.fanout_targets_for_collection(self.config, "csp_listener")
        self.assertEqual(len(targets), 2)
        agents = sorted(t["agent_id"] for t in targets)
        self.assertEqual(agents, [10, 11])

    def test_unknown_collection_returns_empty(self):
        from services import ha_lookup
        self.assertEqual(ha_lookup.fanout_targets_for_collection(self.config, "unknown_col"), [])

    def test_no_ha_group_returns_empty(self):
        from services import ha_lookup
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "x")
        self._seed_deployment(500, 10, 1)
        # ha_group 없음
        self.assertEqual(ha_lookup.fanout_targets_for_collection(self.config, "csp_listener"), [])


class SyncTxnTests(_FsCase):
    def test_create_pending_until_all_ack(self):
        from services import sync_txn
        txn = sync_txn.create(self.config, collection="csp_listener", op="CREATE",
                              members=[{"agent_id": 10, "job_id": 1},
                                       {"agent_id": 11, "job_id": 2}])
        self.assertEqual(txn["status"], "pending")
        # 1명 ack → partial
        t = sync_txn.ack(self.config, txn["id"], 10)
        self.assertEqual(t["status"], "partial")
        # 2명 ack → success
        t = sync_txn.ack(self.config, txn["id"], 11)
        self.assertEqual(t["status"], "success")
        self.assertIsNotNone(t["completed_at"])

    def test_nack_marks_failed(self):
        from services import sync_txn
        txn = sync_txn.create(self.config, collection="csp_listener", op="UPDATE",
                              members=[{"agent_id": 10, "job_id": 1},
                                       {"agent_id": 11, "job_id": 2}])
        sync_txn.ack(self.config, txn["id"], 10, status="ack")
        t = sync_txn.ack(self.config, txn["id"], 11, status="nack", error="signal failed")
        self.assertEqual(t["status"], "failed")
        # 멤버별 status 확인
        by_agent = {m["agent_id"]: m for m in t["members"]}
        self.assertEqual(by_agent[10]["status"], "ack")
        self.assertEqual(by_agent[11]["status"], "nack")
        self.assertEqual(by_agent[11]["error"], "signal failed")

    def test_ack_idempotent_on_already_settled_slot(self):
        from services import sync_txn
        txn = sync_txn.create(self.config, collection="csp_listener", op="CREATE",
                              members=[{"agent_id": 10, "job_id": 1}])
        sync_txn.ack(self.config, txn["id"], 10)
        # 두번째 ack — 상태 변경 없이 안전 반환
        t = sync_txn.ack(self.config, txn["id"], 10)
        self.assertEqual(t["status"], "success")

    def test_get_nonexistent_returns_none(self):
        from services import sync_txn
        self.assertIsNone(sync_txn.get(self.config, 99999))


class SyncDispatchTests(_FsCase):
    def test_enqueue_creates_jobs_and_txn(self):
        from services import sync_dispatch, sync_txn, file_store
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "ctrl-a"); self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(100, "Control", "active_standby", [10, 11])
        self._seed_deployment(500, 10, 1)
        self._seed_deployment(501, 11, 1)

        sid = sync_dispatch.enqueue_collection_sync(
            self.config, entity="listener", op="CREATE", row_id=42, actor="console")
        self.assertIsNotNone(sid)

        txn = sync_txn.get(self.config, sid)
        self.assertEqual(txn["collection"], "csp_listener")
        self.assertEqual(txn["op"], "CREATE")
        self.assertEqual(len(txn["members"]), 2)

        # 각 멤버 job 이 생성되고 params.sync_id 가 backfill 되었는지
        jobs_dir = file_store.domain_dir(self.config, "jobs")
        for m in txn["members"]:
            j = file_store.by_id(jobs_dir, m["job_id"])
            self.assertIsNotNone(j)
            self.assertEqual(j["job_type"], "sync_config")
            self.assertEqual(j["params"]["sync_id"], sid)
            self.assertEqual(j["params"]["collection"], "csp_listener")
            self.assertEqual(j["params"]["op"], "CREATE")
            self.assertEqual(j["params"]["row_id"], 42)

    def test_enqueue_returns_none_when_no_members(self):
        from services import sync_dispatch
        # ha_group 도 deployment 도 없음
        sid = sync_dispatch.enqueue_collection_sync(
            self.config, entity="listener", op="CREATE", row_id=42)
        self.assertIsNone(sid)

    def test_enqueue_unknown_entity_returns_none(self):
        from services import sync_dispatch
        sid = sync_dispatch.enqueue_collection_sync(
            self.config, entity="bogus", op="CREATE", row_id=1)
        self.assertIsNone(sid)


class ShouldPropagateTests(unittest.TestCase):
    """T4 의 scope+mode → propagate 결정 헬퍼."""

    def test_explicit_override_wins(self):
        from services import ha_lookup
        self.assertTrue (ha_lookup.should_propagate("system", "all_active", override=True))
        self.assertFalse(ha_lookup.should_propagate("service", "active_standby", override=False))

    def test_service_always_propagates(self):
        from services import ha_lookup
        for mode in (None, "active_standby", "all_active", "standalone"):
            self.assertTrue(ha_lookup.should_propagate("service", mode))
            self.assertTrue(ha_lookup.should_propagate(None, mode))  # 기본값=service

    def test_system_only_when_active_standby(self):
        from services import ha_lookup
        self.assertTrue (ha_lookup.should_propagate("system", "active_standby"))
        self.assertFalse(ha_lookup.should_propagate("system", "all_active"))
        self.assertFalse(ha_lookup.should_propagate("system", "standalone"))
        self.assertFalse(ha_lookup.should_propagate("system", None))


def tearDownModule():
    """test_verify_lib 의 httpsrv stub 등록 로직과 충돌 회피.

    csc/src 의 services / handlers 를 import 하면 httpsrv 가 sys.modules 에
    real 모듈로 적재됨. test_verify_lib 가 'httpsrv' not in sys.modules 일 때만
    stub 을 등록하므로, 우리가 남긴 real 모듈이 무인자 HandlerArgs 호출을 깨뜨림.
    여기서 정리해 두면 다른 테스트가 자기 stub 을 자유롭게 등록 가능.
    """
    for k in list(sys.modules):
        if k == "httpsrv" or k.startswith("httpsrv."):
            sys.modules.pop(k, None)
        if k == "services" or k.startswith("services.") \
                or k == "handlers" or k.startswith("handlers."):
            sys.modules.pop(k, None)


if __name__ == "__main__":
    unittest.main()
