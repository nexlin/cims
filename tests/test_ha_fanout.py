"""
HA fan-out core 단위 테스트.

HA 그룹 멤버 사이에서 설정·컬렉션을 같게 유지하는 기능이다. 절체로 넘어간 쪽이 다른
설정으로 뜨는 것을 막는 게 목적.

Covers:
  - services.ha_lookup — ha_group ↔ package ↔ deployment 매핑, scope+mode → 전파 판정
    (scope=service 는 그룹 공통값이라 항상 전파, scope=system 은 노드 고유값이라 A/S 만)
  - services.sync_txn — 여러 멤버에 나간 job 을 한 트랜잭션으로 묶어 per-member
    ack/nack 추적 + status 도출

전파 모델은 **자동 push 에서 관측 후 교정으로 바뀌었다**: 쓸 때마다 밀어넣던
`sync_dispatch` 는 없어지고(절체 직후 못 받은 멤버에서 조용히 유실됐다), 지금은
명시 동기화(`POST /deployments/{id}/sync`, op=group_sync) · 자동 정합
(`reconcile_group_package`, op=auto_sync) · 주기 비교(`drift_sweeper`)가 대신한다.
sync_txn 은 그 세 경로가 공통으로 쓰는 현역이다.

각 테스트는 tmpdir 로 CimsRuntimeDir 격리. 서버 본체는 안 띄움 — 순수 함수 호출만.
"""
import os
import sys
import tempfile
import unittest

# fan-out 정본은 **oam** 이다 — csc/src/services/ha_lookup 에도 같은 함수가 남아
# 있지만 csc 는 collection_dir(경로 해석)만 쓰고 나머지는 죽은 코드다.
# 다른 테스트가 csc 쪽 services 를 먼저 import 했을 수 있으므로 캐시를 비운다.
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))


class _FsCase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.config = {"CimsRuntimeDir": self._td.name}
        # 관리 store 는 단일 writer — write 는 소유권 리스를 요구한다(oam_ha.md §4.4).
        from services import file_store, lease
        self._lease = lease
        st = lease.acquire(file_store.runtime_root(self.config))
        if not st.get('active'):
            self.skipTest(f"store 리스 획득 불가({st.get('reason')}) — flock 미지원 tmpdir")

    def tearDown(self):
        self._lease.release()
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

    services / handlers 를 import 하면 httpsrv 가 sys.modules 에 real 모듈로 적재됨. test_verify_lib 가 'httpsrv' not in sys.modules 일 때만
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
