"""
deployments 설정 저장(무전파) + HA 그룹 자동 동기화(R4) 단위 테스트.

Covers (handlers 직접 호출 — 서버 미기동):
  - _put_deployment_config: 항상 단일 deployment 저장 — HA 피어 무변경, job 1건.
    구 body 필드(sync_keys/sync_checked/propagate_to_ha_peers)는 무시.
  - _sync_deployment_config (POST /deployments/{id}/sync): 방향성 복사 —
    source overlay 값 merge / source 에 없는 키 제거(기본값 복귀) /
    유효 scope(field.scope ?? section.scope)=system 키 skipped /
    버전 가드(409) / 멤버십 가드(409) / target job + sync_txn(op=group_sync) /
    scope=system 컬렉션 skipped.
  - _get_deployment_config: ha block (그룹 멤버 + package_version) / null (standalone)
  - _effective_scope / _service_scope_keys / _service_scope_collections 헬퍼
  - ha_lookup.vip_observation / auto_sync_enabled: heartbeat interfaces[] 기반 실측
    ACTIVE 판정 (정확히 1명 보유·stale 제외·애매하면 None) + 스위치 기본 ON
  - reconcile_group_package: 자동 교정 — ACTIVE→STANDBY service 키 복사/기본값 복귀,
    스위치 OFF·판정 불가·버전 불일치 skip, sync_txn(op=auto_sync), 멱등(in_sync)
  - evaluate_group_package: 드리프트 판정(읽기 전용) — 교정이 바꿀 키와 정확히 일치,
    스위치 OFF 에서도 판정, 판정 불가는 unknown(+reason), password 값 마스킹
  - _put_group_pkg_config / _put_group_auto_sync (ha_groups): 그룹 공통 설정 저장
    (ON=전 멤버, OFF=target 필수) + 스위치 영속·ON 전환 즉시 정합

각 테스트는 tmpdir 로 CimsRuntimeDir 격리. sys.path 는 ems/core/oam/{src,vendor}
(csc→oam 분리 이후 배포 admin 핸들러는 oam 소유).
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


TEMPLATE = {
    "version": 1,
    "sections": [
        {"key": "svc", "title": "Service", "scope": "service",
         "fields": [
             {"key": "Db.Host", "type": "string", "default": "127.0.0.1"},
             {"key": "Db.Port", "type": "int", "default": 3306},
             # 필드 오버라이드 — service 섹션 안의 노드별 값 (예: csp media_server.LocalIp)
             {"key": "Media.LocalIp", "type": "string", "default": "", "scope": "system"},
         ]},
        {"key": "sys", "title": "System", "scope": "system",
         "fields": [
             {"key": "SystemId", "type": "string", "default": ""},
             {"key": "LocalIp", "type": "string", "default": ""},
             # 역방향 오버라이드 — system 섹션 안의 공통값 (예: csc _infra JwtSecret)
             {"key": "Tls.Port", "type": "int", "default": 5061, "scope": "service"},
         ]},
    ],
    "collections": [
        {"key": "routes", "title": "Routes", "scope": "service",
         "schema": {"fields": []}},
        {"key": "local_nodes", "title": "Local Nodes", "scope": "system",
         "schema": {"fields": []}},
    ],
}


class _FsCase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.config = {"CimsRuntimeDir": self._td.name}
        # 관리 store 는 단일 writer 다 — write 는 소유권 리스를 요구한다(oam_ha.md §4.4).
        # 실기동(oam_app)이 bind 전에 acquire 하는 것과 같은 자리에서 잡아, 테스트도
        # 실제 write 경로(assert_writable)를 그대로 통과하게 한다.
        from services import file_store, lease
        self._lease = lease
        st = lease.acquire(file_store.runtime_root(self.config))
        if not st.get('active'):
            self.skipTest(f"store 리스 획득 불가({st.get('reason')}) — flock 미지원 tmpdir")

    def tearDown(self):
        self._lease.release()
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

    def _sync(self, did, body):
        from handlers.agents import _sync_deployment_config
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="POST", full_path=f"/api/v1/deployments/{did}/sync",
                         client_ip="127.0.0.1", client_port=0, body=body)
        return asyncio.run(_sync_deployment_config(ha, did, self.config))

    def _get(self, did):
        from handlers.agents import _get_deployment_config
        return asyncio.run(_get_deployment_config(did, self.config))

    def _dep(self, did):
        from services import file_store
        return file_store.by_id(file_store.domain_dir(self.config, "deployments"), did)

    def _jobs(self):
        from services import file_store
        return file_store.load_all(file_store.domain_dir(self.config, "jobs"))

    def _txns(self):
        from services import file_store
        return file_store.load_all(file_store.domain_dir(self.config, "csp_sync_txn"))

    def _seed_as_pair(self):
        """표준 시나리오 — AS 그룹(1: control) 멤버 agent 10/11, csp dep 5/6.
        피어(dep 6)는 고유 overlay {SystemId: B, Db.Host: 10.0.0.1, Db.Port: 3307} 보유."""
        self._seed_pkg(1, "csp")
        self._seed_agent(10, "ctrl-a")
        self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(1, "control", "active_standby", [10, 11])
        self._seed_deployment(5, 10, 1, config={"SystemId": "A", "Db.Host": "10.9.9.9",
                                                "Tls.Port": 6061})
        self._seed_deployment(6, 11, 1, config={"SystemId": "B", "Db.Host": "10.0.0.1",
                                                "Db.Port": 3307})


class TestPutConfigNoPropagation(_FsCase):
    """PUT /deployments/{id}/config — 항상 단일 서버 저장, 전파 없음."""

    def test_put_saves_only_this_deployment(self):
        """온 키만 기존 overlay 에 병합 — 콘솔은 **바뀐 키만** 보낸다.
        전체 값을 되돌려 보내지 않으므로, 안 온 키(Tls.Port)는 보존되어야 한다.
        (전체 덮어쓰기면 화면에 마스킹돼 보이던 _infra 시크릿이 지워져 전면 401)"""
        self._seed_as_pair()
        r = self._put(5, {"config": {"Db.Host": "10.5.5.5", "SystemId": "A"}})
        self.assertEqual(r.status, 200)
        self.assertEqual(self._dep(5)["config"],
                         {"Db.Host": "10.5.5.5", "SystemId": "A", "Tls.Port": 6061})
        # 피어 무변경
        self.assertEqual(self._dep(6)["config"],
                         {"SystemId": "B", "Db.Host": "10.0.0.1", "Db.Port": 3307})
        jobs = self._jobs()
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["params"]["deployment_id"], 5)
        self.assertEqual(r.body["job_id"], jobs[0]["id"])
        # 실체화 — template default + 저장 overlay
        self.assertEqual(jobs[0]["params"]["config"]["Db.Port"], 3306)

    def test_put_null_removes_key(self):
        """병합의 반대편 — 값 null 은 **명시 삭제**(기본값 복귀). 안 보낸 키와 구분된다."""
        self._seed_as_pair()
        r = self._put(5, {"config": {"Tls.Port": None}})
        self.assertEqual(r.status, 200)
        self.assertEqual(self._dep(5)["config"], {"SystemId": "A", "Db.Host": "10.9.9.9"})

    def test_put_ignores_legacy_sync_body_fields(self):
        """구 R2 body(sync_keys/sync_checked/propagate_to_ha_peers)는 완전 무시 —
        어떤 값이 와도 피어·그룹 레코드에 쓰지 않는다."""
        self._seed_as_pair()
        r = self._put(5, {"config": {"Db.Host": "10.5.5.5"},
                          "sync_keys": ["Db.Host"],
                          "sync_checked": ["Db.Host", "Db.Port"],
                          "propagate_to_ha_peers": True})
        self.assertEqual(r.status, 200)
        self.assertNotIn("propagated", r.body)
        self.assertNotIn("sync_keys_applied", r.body)
        self.assertEqual(self._dep(6)["config"],
                         {"SystemId": "B", "Db.Host": "10.0.0.1", "Db.Port": 3307})
        self.assertNotIn("config_sync", self._group_rec(1))
        self.assertEqual(len(self._jobs()), 1)

    def test_put_queue_update_false(self):
        self._seed_as_pair()
        r = self._put(5, {"config": {"Db.Host": "10.5.5.5"}, "queue_update": False})
        self.assertEqual(r.status, 200)
        self.assertIsNone(r.body["job_id"])
        self.assertEqual(self._jobs(), [])

    def _group_rec(self, gid):
        from services import file_store
        return file_store.by_id(file_store.domain_dir(self.config, "ha_groups"), gid)


class TestGetConfigHaBlock(_FsCase):

    def test_ha_block_members_with_version(self):
        self._seed_as_pair()
        view = self._get(5)
        self.assertEqual(view.status, 200)
        ha = view.body["ha"]
        self.assertEqual(ha["group_id"], 1)
        self.assertEqual(ha["mode"], "active_standby")
        self.assertNotIn("sync_keys", ha)
        self.assertEqual({m["deployment_id"] for m in ha["members"]}, {5, 6})
        self.assertEqual({m["agent_name"] for m in ha["members"]}, {"ctrl-a", "ctrl-b"})
        self.assertEqual({m["package_version"] for m in ha["members"]}, {"0.1.0"})

    def test_standalone_ha_null(self):
        self._seed_pkg(1, "csp")
        self._seed_agent(20, "solo")
        self._seed_deployment(7, 20, 1)
        view = self._get(7)
        self.assertIsNone(view.body["ha"])


class TestGroupSync(_FsCase):
    """POST /deployments/{id}/sync — 명시적 방향성 동기화."""

    def test_directional_copy_service_keys_only(self):
        """source overlay 값 merge + source 에 없는 키 제거 + system 키 skipped.
        job 은 target 에만, sync_txn(op=group_sync) 생성 + sync_id backfill."""
        self._seed_as_pair()
        r = self._sync(5, {"targets": [6],
                           "keys": ["Db.Host", "Db.Port", "Tls.Port",
                                    "SystemId", "Media.LocalIp"]})
        self.assertEqual(r.status, 200)
        self.assertTrue(r.body["ok"])
        # 유효 scope 마스크: service 섹션(Db.*) + 필드 오버라이드 service(Tls.Port) 허용,
        # system 섹션(SystemId) + 필드 오버라이드 system(Media.LocalIp) 차단
        self.assertEqual(r.body["applied_keys"], ["Db.Host", "Tls.Port"])
        self.assertEqual(r.body["removed_keys"], ["Db.Port"])   # source 에 없음 → 기본값 복귀
        self.assertEqual(r.body["skipped_keys"], ["Media.LocalIp", "SystemId"])
        # source 무변경
        self.assertEqual(self._dep(5)["config"],
                         {"SystemId": "A", "Db.Host": "10.9.9.9", "Tls.Port": 6061})
        # target: Db.Host/Tls.Port 복사, Db.Port 제거, 고유 SystemId 보존
        self.assertEqual(self._dep(6)["config"],
                         {"SystemId": "B", "Db.Host": "10.9.9.9", "Tls.Port": 6061})
        # job 은 target(dep6)에만 — source 는 재기록 불필요
        jobs = self._jobs()
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["params"]["deployment_id"], 6)
        self.assertEqual(jobs[0]["params"]["config"]["SystemId"], "B")
        self.assertEqual(jobs[0]["params"]["config"]["Db.Host"], "10.9.9.9")
        self.assertEqual(jobs[0]["params"]["config"]["Db.Port"], 3306)   # 기본값 복귀
        # sync_txn + backfill
        self.assertIsNotNone(r.body["sync_id"])
        self.assertEqual(jobs[0]["params"]["sync_id"], r.body["sync_id"])
        txns = self._txns()
        self.assertEqual(len(txns), 1)
        self.assertEqual(txns[0]["op"], "group_sync")

    def test_version_mismatch_409(self):
        """롤링 업그레이드 혼재(버전 상이) — 409 로 차단."""
        self._seed_pkg(1, "csp", version="0.1.0")
        self._seed_pkg(3, "csp", version="0.2.0")
        self._seed_agent(10, "ctrl-a")
        self._seed_agent(11, "ctrl-b")
        self._seed_ha_group(1, "control", "active_standby", [10, 11])
        self._seed_deployment(5, 10, 1, config={"Db.Host": "10.9.9.9"})
        self._seed_deployment(6, 11, 3, config={})
        r = self._sync(5, {"targets": [6], "keys": ["Db.Host"]})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "version_mismatch")
        self.assertEqual(r.body["source_version"], "0.1.0")
        self.assertEqual(r.body["targets"],
                         [{"deployment_id": 6, "package_version": "0.2.0"}])
        self.assertEqual(self._dep(6)["config"], {})
        self.assertEqual(self._jobs(), [])

    def test_target_not_in_group_409(self):
        self._seed_as_pair()
        self._seed_agent(20, "outsider")
        self._seed_deployment(9, 20, 1, config={})   # 그룹 밖 같은 패키지
        r = self._sync(5, {"targets": [9], "keys": ["Db.Host"]})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "target_not_in_group")
        self.assertEqual(self._dep(9)["config"], {})

    def test_standalone_source_409(self):
        self._seed_pkg(1, "csp")
        self._seed_agent(20, "solo")
        self._seed_deployment(7, 20, 1)
        r = self._sync(7, {"targets": [7], "keys": ["Db.Host"]})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "not_in_ha_group")

    def test_self_only_target_400(self):
        self._seed_as_pair()
        r = self._sync(5, {"targets": [5], "keys": ["Db.Host"]})
        self.assertEqual(r.status, 400)
        self.assertEqual(r.body["error"], "no_valid_targets")

    def test_empty_request_400(self):
        self._seed_as_pair()
        r = self._sync(5, {"targets": [6]})
        self.assertEqual(r.status, 400)
        r = self._sync(5, {"targets": [], "keys": ["Db.Host"]})
        self.assertEqual(r.status, 400)

    def test_queue_update_false_no_jobs(self):
        self._seed_as_pair()
        r = self._sync(5, {"targets": [6], "keys": ["Db.Host"], "queue_update": False})
        self.assertEqual(r.status, 200)
        self.assertEqual(self._jobs(), [])
        self.assertIsNone(r.body["sync_id"])
        self.assertEqual(self._dep(6)["config"]["Db.Host"], "10.9.9.9")

    def test_system_scope_collection_skipped(self):
        """scope=system 컬렉션은 agent proxy 호출 없이 skipped 보고."""
        self._seed_as_pair()
        r = self._sync(5, {"targets": [6], "keys": ["Db.Host"],
                           "collections": ["local_nodes"]})
        self.assertEqual(r.status, 200)
        self.assertEqual(len(r.body["collections"]), 1)
        c = r.body["collections"][0]
        self.assertEqual(c["name"], "local_nodes")
        self.assertEqual(c["skipped"], "scope_not_service")

    def test_all_active_multi_target(self):
        """AA 3멤버 — source 1 → target 2 방향성 복사."""
        self._seed_pkg(2, "cmp", template=TEMPLATE)
        for aid, name in ((30, "media-a"), (31, "media-b"), (32, "media-c")):
            self._seed_agent(aid, name)
        self._seed_ha_group(2, "media", "all_active", [30, 31, 32])
        self._seed_deployment(8, 30, 2, config={"SystemId": "M1", "Db.Host": "10.2.2.2"})
        self._seed_deployment(9, 31, 2, config={"SystemId": "M2"})
        self._seed_deployment(10, 32, 2, config={"SystemId": "M3"})
        r = self._sync(8, {"targets": [9, 10], "keys": ["Db.Host"]})
        self.assertEqual(r.status, 200)
        self.assertEqual(self._dep(9)["config"], {"SystemId": "M2", "Db.Host": "10.2.2.2"})
        self.assertEqual(self._dep(10)["config"], {"SystemId": "M3", "Db.Host": "10.2.2.2"})
        self.assertEqual(len(self._jobs()), 2)


class _R4Case(_FsCase):
    """R4 공용 시더 — heartbeat 관측 가능한 agent + VIP 있는 AS 그룹."""

    VIP = "10.0.0.100"

    def _seed_agent_hb(self, aid, name, ips, hb_age_sec=0):
        """interfaces + last_heartbeat 를 가진 agent (heartbeat 관측 시뮬레이션)."""
        from services import file_store
        from datetime import datetime, timedelta
        d = file_store.domain_dir(self.config, "agents")
        file_store.save(d, aid, {
            "id": aid, "name": name,
            "interfaces": [{"name": "eth0", "ip": ip, "mask": 24} for ip in ips],
            "last_heartbeat": (datetime.now() - timedelta(seconds=hb_age_sec))
                              .isoformat(timespec="seconds"),
        })

    def _seed_as_group(self, gid=1, auto_sync=None, member_agent_ids=(10, 11)):
        from services import file_store
        d = file_store.domain_dir(self.config, "ha_groups")
        rec = {"id": gid, "name": "control", "mode": "active_standby",
               "vip": None,
               "vip_bindings": [{"bid": 1, "slot": "SIP", "ip": self.VIP}],
               "members": [{"agent_id": aid, "priority": 100 - i}
                           for i, aid in enumerate(member_agent_ids)]}
        if auto_sync is not None:
            rec["auto_sync"] = auto_sync
        file_store.save(d, gid, rec)
        return rec

    def _seed_r4_pair(self, active_agent=10, auto_sync=None):
        """agent10(VIP 보유=ACTIVE)·agent11(STANDBY), csp dep 5/6 — R4 표준 시나리오."""
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "ctrl-a",
                            ["10.0.0.11"] + ([self.VIP] if active_agent == 10 else []))
        self._seed_agent_hb(11, "ctrl-b",
                            ["10.0.0.12"] + ([self.VIP] if active_agent == 11 else []))
        g = self._seed_as_group(auto_sync=auto_sync)
        self._seed_deployment(5, 10, 1, config={"SystemId": "A", "Db.Host": "10.9.9.9",
                                                "Tls.Port": 6061})
        self._seed_deployment(6, 11, 1, config={"SystemId": "B", "Db.Host": "10.0.0.1",
                                                "Db.Port": 3307})
        return g


class TestVipObservation(_R4Case):
    """ha_lookup.vip_observation — heartbeat interfaces[] 기반 실측 ACTIVE 판정."""

    def test_single_holder_active(self):
        from services import ha_lookup
        g = self._seed_r4_pair(active_agent=10)
        obs = ha_lookup.vip_observation(self.config, g)
        self.assertEqual(obs["active_agent_id"], 10)
        self.assertEqual(obs["observed"], {10: True, 11: False})

    def test_two_holders_ambiguous(self):
        """절체 직후 관측 창 — 2명 보유로 보이면 판정 불가 (오방향 복사 방지)."""
        from services import ha_lookup
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12", self.VIP])
        g = self._seed_as_group()
        obs = ha_lookup.vip_observation(self.config, g)
        self.assertIsNone(obs["active_agent_id"])

    def test_stale_heartbeat_excluded(self):
        """다운된(heartbeat 끊긴) 옛 ACTIVE 는 판정에서 제외 — 새 보유자로 확정."""
        from services import ha_lookup
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP], hb_age_sec=600)  # stale
        self._seed_agent_hb(11, "b", ["10.0.0.12", self.VIP])
        g = self._seed_as_group()
        obs = ha_lookup.vip_observation(self.config, g)
        self.assertEqual(obs["active_agent_id"], 11)
        self.assertIsNone(obs["observed"][10])   # stale = 판정 불가

    def test_no_vip_defined(self):
        from services import ha_lookup
        self._seed_agent_hb(10, "a", ["10.0.0.11"])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        from services import file_store
        d = file_store.domain_dir(self.config, "ha_groups")
        g = {"id": 1, "name": "g", "mode": "active_standby", "vip": None,
             "members": [{"agent_id": 10, "priority": 100},
                         {"agent_id": 11, "priority": 90}]}
        file_store.save(d, 1, g)
        obs = ha_lookup.vip_observation(self.config, g)
        self.assertIsNone(obs["active_agent_id"])

    def test_auto_sync_enabled_defaults(self):
        from services import ha_lookup
        as_g = {"mode": "active_standby"}
        self.assertTrue(ha_lookup.auto_sync_enabled(as_g, "csp"))          # 부재 = ON
        self.assertFalse(ha_lookup.auto_sync_enabled(
            {"mode": "active_standby", "auto_sync": {"csp": False}}, "csp"))
        self.assertTrue(ha_lookup.auto_sync_enabled(
            {"mode": "active_standby", "auto_sync": {"cmp": False}}, "csp"))
        self.assertFalse(ha_lookup.auto_sync_enabled({"mode": "all_active"}, "csp"))


class TestReconcileGroupPackage(_R4Case):
    """reconcile_group_package — ACTIVE 기준 STANDBY 자동 교정."""

    def _reconcile(self, g, pkg="csp", **kw):
        from handlers.agents import reconcile_group_package
        kw.setdefault("include_collections", False)
        return reconcile_group_package(self.config, g, pkg, **kw)

    def test_happy_path_active_to_standby(self):
        g = self._seed_r4_pair(active_agent=10)
        r = self._reconcile(g)
        self.assertEqual(r["status"], "synced")
        self.assertEqual(r["active_agent_id"], 10)
        self.assertEqual(r["synced_keys"], ["Db.Host", "Tls.Port"])
        self.assertEqual(r["removed_keys"], ["Db.Port"])   # ACTIVE 에 없음 → 기본값 복귀
        # STANDBY(dep6): service 키는 ACTIVE 값, 고유 SystemId 보존
        self.assertEqual(self._dep(6)["config"],
                         {"SystemId": "B", "Db.Host": "10.9.9.9", "Tls.Port": 6061})
        # ACTIVE(dep5) 무변경
        self.assertEqual(self._dep(5)["config"],
                         {"SystemId": "A", "Db.Host": "10.9.9.9", "Tls.Port": 6061})
        jobs = self._jobs()
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["params"]["deployment_id"], 6)
        txns = self._txns()
        self.assertEqual(len(txns), 1)
        self.assertEqual(txns[0]["op"], "auto_sync")
        # 멱등 — 재실행 시 in_sync, 추가 job 없음
        r2 = self._reconcile(g)
        self.assertEqual(r2["status"], "in_sync")
        self.assertEqual(len(self._jobs()), 1)

    def test_direction_follows_active(self):
        """agent11 이 VIP 보유(절체 후) — 복사 방향이 11→10 으로 바뀐다."""
        g = self._seed_r4_pair(active_agent=11)
        r = self._reconcile(g)
        self.assertEqual(r["active_agent_id"], 11)
        # dep5(STANDBY 가 된 쪽) 가 dep6(ACTIVE) 값을 받고, ACTIVE overlay 에 없는
        # Tls.Port 는 제거(기본값 복귀). 고유 SystemId 는 보존.
        self.assertEqual(self._dep(5)["config"],
                         {"SystemId": "A", "Db.Host": "10.0.0.1", "Db.Port": 3307})

    def test_switch_off_skips(self):
        g = self._seed_r4_pair(auto_sync={"csp": False})
        r = self._reconcile(g)
        self.assertEqual(r["status"], "skipped")
        self.assertEqual(r["reason"], "switch_off")
        self.assertEqual(self._dep(6)["config"]["Db.Host"], "10.0.0.1")

    def test_active_unknown_skips(self):
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12", self.VIP])   # 2명 보유
        g = self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={"Db.Host": "x"})
        self._seed_deployment(6, 11, 1, config={})
        r = self._reconcile(g)
        self.assertEqual(r["reason"], "active_unknown")
        self.assertEqual(self._dep(6)["config"], {})

    def test_version_mismatch_deferred(self):
        self._seed_pkg(1, "csp", version="0.1.0")
        self._seed_pkg(3, "csp", version="0.2.0")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={"Db.Host": "x"})
        self._seed_deployment(6, 11, 3, config={})
        r = self._reconcile(g)
        self.assertEqual(r["reason"], "version_mismatch")
        self.assertEqual(r["deferred"],
                         [{"deployment_id": 6, "package_version": "0.2.0"}])
        self.assertEqual(self._dep(6)["config"], {})


class TestOverlaySchemaMask(_R4Case):
    """overlay = config_template 선언 키만 — write 프루닝 + 렌더 동치 증명 스윕."""

    # 주입 대상 모듈(shared_identity)로 두 경로를 한 픽스처에서 본다.
    TPL = {"version": 1, "sections": [
        {"key": "svc", "title": "S", "scope": "service", "fields": [
            {"key": "Db.Host", "type": "string", "default": "127.0.0.1"},
        ]},
        {"key": "_infra", "title": "Infra", "scope": "system", "fields": [
            {"key": "CimsRuntimeDir", "type": "path", "default": ""},
        ]}]}

    def _seed_shared_pkg(self, pid=1, name="oam"):
        from services import file_store
        file_store.save(file_store.domain_dir(self.config, "packages"), pid, {
            "id": pid, "name": name, "version": "0.1.0",
            "config_template": self.TPL, "meta": {"shared_identity": True}})

    def _put(self, did, cfg):
        from handlers.agents import _put_deployment_config
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="PUT", full_path=f"/api/v1/deployments/{did}/config",
                         client_ip="127.0.0.1", client_port=0,
                         body={"config": cfg, "queue_update": False})
        return asyncio.run(_put_deployment_config(ha, did, self.config))

    def _sweep(self, apply=True):
        from handlers.agents import sweep_overlay_schema
        return sweep_overlay_schema(self.config, apply=apply)

    def test_put_drops_untemplated_keys_and_reports(self):
        self._seed_shared_pkg()
        self._seed_agent(10, "a")
        self._seed_deployment(5, 10, 1, config={})
        r = self._put(5, {"Db.Host": "10.0.0.9", "Bogus.Key": "x"})
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body["pruned_keys"], ["Bogus.Key"])   # 조용히 버리지 않는다
        self.assertEqual(self._dep(5)["config"], {"Db.Host": "10.0.0.9"})

    def test_put_keeps_all_keys_when_package_has_no_template(self):
        """템플릿 없는 패키지는 검증 근거가 없다 — 판단하지 않고 그대로 저장."""
        from services import file_store
        file_store.save(file_store.domain_dir(self.config, "packages"), 1,
                        {"id": 1, "name": "x", "version": "0.1.0"})
        self._seed_agent(10, "a")
        self._seed_deployment(5, 10, 1, config={})
        r = self._put(5, {"Whatever": 1})
        self.assertEqual(r.body["pruned_keys"], [])
        self.assertEqual(self._dep(5)["config"], {"Whatever": 1})

    def test_create_prunes_bootstrap_snapshot(self):
        """부트스트랩이 실행 중 config.json 을 통째로 보내도 템플릿 키만 굳는다."""
        from handlers.agents import _create_deployment
        from httpsrv.handler import HandlerArgs
        self._seed_pkg(1, "csp", template=self.TPL)
        self._seed_agent(10, "a")
        ha = HandlerArgs(method="POST", full_path="/api/v1/deployments",
                         client_ip="127.0.0.1", client_port=0,
                         body={"agent_id": 10, "package_id": 1, "process_name": "csp",
                               "install_path": "/opt/x", "status": "running",
                               "config": {"Db.Host": "10.0.0.9",
                                          "CimsRuntimeDir": "/mnt/cims/runtime",
                                          "Runtime.Snapshot.Junk": "from-config-json"}})
        r = asyncio.run(_create_deployment(ha, self.config))
        self.assertEqual(r.status, 201)
        self.assertEqual(r.body["pruned_keys"], ["Runtime.Snapshot.Junk"])
        self.assertEqual(self._dep(r.body["id"])["config"],
                         {"Db.Host": "10.0.0.9", "CimsRuntimeDir": "/mnt/cims/runtime"})

    def test_sweep_removes_only_render_equivalent_keys(self):
        """렌더 결과가 같을 때만 정리 — 살아있는 키는 두고 경고."""
        self._seed_shared_pkg()
        self._seed_agent(10, "a")
        # dep5: 주입으로 되살아나는 키(BuiltinAccounts) → 정리 대상
        # dep6: 렌더에 그대로 실리는 키(Live.Key) → 보존 대상
        self.config["CimsAuth"] = {"BuiltinAccounts": [{"login_id": "admin"}]}
        self._seed_deployment(5, 10, 1, config={
            "Db.Host": "10.0.0.1", "CimsAuth.BuiltinAccounts": [{"login_id": "admin"}]})
        self._seed_deployment(6, 10, 1, config={"Db.Host": "10.0.0.1", "Live.Key": "keep-me"})
        r = self._sweep()
        self.assertEqual(r["removed_keys"], {5: ["CimsAuth.BuiltinAccounts"]})
        self.assertEqual(r["kept_keys"], {6: ["Live.Key"]})
        self.assertEqual(self._dep(5)["config"], {"Db.Host": "10.0.0.1"})
        self.assertEqual(self._dep(6)["config"], {"Db.Host": "10.0.0.1", "Live.Key": "keep-me"})

    def test_sweep_is_idempotent_and_dry_run_writes_nothing(self):
        self._seed_shared_pkg()
        self._seed_agent(10, "a")
        self.config["CimsAuth"] = {"BuiltinAccounts": [{"login_id": "admin"}]}
        self._seed_deployment(5, 10, 1, config={
            "Db.Host": "x", "CimsAuth.BuiltinAccounts": [{"login_id": "admin"}]})
        dry = self._sweep(apply=False)
        self.assertEqual(dry["removed_keys"], {5: ["CimsAuth.BuiltinAccounts"]})
        self.assertIn("CimsAuth.BuiltinAccounts", self._dep(5)["config"])   # 미적용
        self._sweep()
        self.assertEqual(self._sweep()["cleaned"], 0)                       # 멱등


class TestEvaluateGroupPackage(_R4Case):
    """evaluate_group_package — 드리프트 **판정**(읽기 전용). 콘솔 표시의 정본."""

    def _evaluate(self, g, pkg="csp"):
        from handlers.agents import evaluate_group_package
        return evaluate_group_package(self.config, g, pkg)

    def _reconcile(self, g, pkg="csp"):
        from handlers.agents import reconcile_group_package
        return reconcile_group_package(self.config, g, pkg, include_collections=False)

    def test_drift_matches_what_reconcile_would_change(self):
        """판정과 교정의 단일 진실 — drift 키 == 교정이 실제로 바꾼 키."""
        g = self._seed_r4_pair(active_agent=10)
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "out_of_sync")
        self.assertEqual(ev["active_agent_id"], 10)
        self.assertEqual(ev["compared_to"]["deployment_id"], 5)
        self.assertEqual([d["key"] for d in ev["drift"]],
                         ["Db.Host", "Db.Port", "Tls.Port"])
        # ACTIVE 에 있는 키는 복사, 없는 키는 제거(기본값 복귀)
        self.assertEqual({d["key"]: d["action"] for d in ev["drift"]},
                         {"Db.Host": "copy", "Db.Port": "reset", "Tls.Port": "copy"})
        # ACTIVE 에 없어 STANDBY 에서 지워질 키는 기준값이 없다
        self.assertIsNone(next(d for d in ev["drift"] if d["key"] == "Db.Port")["active"])
        # 어느 멤버가 무슨 값을 갖고 있는지 (present=False → overlay 부재)
        tls = next(d for d in ev["drift"] if d["key"] == "Tls.Port")
        self.assertEqual(tls["members"],
                         [{"deployment_id": 6, "agent_id": 11, "agent_name": "ctrl-b",
                           "value": None, "present": False}])
        # 실제 교정이 손대는 키 집합과 정확히 일치해야 한다
        rc = self._reconcile(g)
        self.assertEqual(sorted(rc["synced_keys"] + rc["removed_keys"]),
                         [d["key"] for d in ev["drift"]])

    def test_in_sync_after_reconcile(self):
        """교정 직후 재판정은 in_sync — 콘솔이 계속 '대기 중'을 띄우지 않는다."""
        g = self._seed_r4_pair(active_agent=10)
        self._reconcile(g)
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "in_sync")
        self.assertEqual(ev["drift"], [])

    def test_switch_off_still_judges(self):
        """스위치 OFF 는 교정만 멈춘다 — 정합 여부는 계속 판정 (교정 대기와 구분 표시)."""
        g = self._seed_r4_pair(active_agent=10, auto_sync={"csp": False})
        ev = self._evaluate(g)
        self.assertFalse(ev["auto_sync"])
        self.assertEqual(ev["status"], "out_of_sync")
        self.assertIsNone(ev["reason"])
        # 교정은 실제로 멈춰 있다
        self.assertEqual(self._reconcile(g)["reason"], "switch_off")

    def test_active_unknown_is_unknown_not_drift(self):
        """ACTIVE 판정 불가면 드리프트를 단정하지 않는다 (기준이 없으므로)."""
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12", self.VIP])   # 2명 보유 = 애매
        g = self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={"Db.Host": "x"})
        self._seed_deployment(6, 11, 1, config={})
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "unknown")
        self.assertEqual(ev["reason"], "active_unknown")
        self.assertEqual(ev["drift"], [])

    def test_version_mismatch_unknown_with_deferred(self):
        self._seed_pkg(1, "csp", version="0.1.0")
        self._seed_pkg(3, "csp", version="0.2.0")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={"Db.Host": "x"})
        self._seed_deployment(6, 11, 3, config={})
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "unknown")
        self.assertEqual(ev["reason"], "version_mismatch")
        self.assertEqual(ev["deferred"],
                         [{"deployment_id": 6, "package_version": "0.2.0"}])

    def test_all_active_compares_members_without_reference(self):
        """AA 는 기준(ACTIVE)이 없다 — 멤버 간 동일성만 판정하고 교정 지시는 없다."""
        from services import file_store
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "a", ["10.0.0.11"])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        g["mode"] = "all_active"
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        self._seed_deployment(5, 10, 1, config={"Db.Host": "10.0.0.1", "Db.Port": 3306})
        self._seed_deployment(6, 11, 1, config={"Db.Host": "10.0.0.2", "Db.Port": 3306})
        ev = self._evaluate(g)
        self.assertFalse(ev["auto_sync"])            # AA 는 동기화 개념 없음
        self.assertIsNone(ev["compared_to"])         # 기준 멤버 없음
        self.assertEqual(ev["status"], "out_of_sync")
        self.assertEqual([d["key"] for d in ev["drift"]], ["Db.Host"])
        d = ev["drift"][0]
        self.assertIsNone(d["action"])               # 자동 교정 주체 없음
        self.assertEqual([m["value"] for m in d["members"]], ["10.0.0.1", "10.0.0.2"])
        # 값이 같아지면 in_sync
        self._seed_deployment(6, 11, 1, config={"Db.Host": "10.0.0.1", "Db.Port": 3306})
        self.assertEqual(self._evaluate(g)["status"], "in_sync")

    def test_all_active_single_member_no_peers(self):
        from services import file_store
        self._seed_pkg(1, "csp")
        self._seed_agent_hb(10, "a", ["10.0.0.11"])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        g["mode"] = "all_active"
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        self._seed_deployment(5, 10, 1, config={"Db.Host": "10.0.0.1"})
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "unknown")
        self.assertEqual(ev["reason"], "no_peers")

    def test_members_expose_effective_values_and_source(self):
        """표시값은 렌더 실효값 — overlay/주입/기본값을 src 로 구분한다.

        overlay 만 보고 그리면 (a) 주입 값이 빈칸으로 보이고 (b) 판정(overlay 기준)과
        표시 기준이 달라 "값이 같은데 드리프트"가 된다. 그 근거를 응답이 들고 온다."""
        from services import file_store
        tmpl = {"version": 1, "sections": [
            {"key": "svc", "title": "S", "scope": "service", "fields": [
                {"key": "Db.Host", "type": "string", "default": "127.0.0.1"},
            ]},
            {"key": "_infra", "title": "I", "scope": "system", "fields": [
                {"key": "CimsAuth.JwtSecret", "type": "password", "default": ""},
                {"key": "Mgmt.Cidr", "type": "string", "default": ""},
            ]}]}
        file_store.save(file_store.domain_dir(self.config, "packages"), 1, {
            "id": 1, "name": "oam", "version": "0.1.0", "config_template": tmpl,
            "meta": {"shared_identity": True}})
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        # base(OAM 자신)가 주입 소스 — overlay 에 없어도 렌더에는 들어간다
        self.config["CimsAuth"] = {"JwtSecret": "shared-secret"}
        self.config["Mgmt"] = {"Cidr": "10.0.0.0/24"}
        self.config["CimsRuntimeDir"] = self.config["CimsRuntimeDir"]
        self._seed_deployment(5, 10, 1, config={"Db.Host": "10.9.9.9"})
        self._seed_deployment(6, 11, 1, config={})
        ev = self._evaluate(g, pkg="oam")
        by_agent = {m["agent_id"]: m["values"] for m in ev["members"]}
        self.assertEqual(sorted(by_agent), [10, 11])
        # ACTIVE: overlay 로 지정한 값
        self.assertEqual(by_agent[10]["Db.Host"], {"v": "10.9.9.9", "src": "overlay"})
        # STANDBY: overlay 미설정 → 기본값. 값은 달라 보여도 '지정 안 됨' 이 근거다.
        self.assertEqual(by_agent[11]["Db.Host"], {"v": "127.0.0.1", "src": "default"})
        # 주입 값 — overlay 에 없지만 노드 config.json 에는 들어간다(빈칸으로 보이면 오해)
        self.assertEqual(by_agent[11]["Mgmt.Cidr"], {"v": "10.0.0.0/24", "src": "injected"})
        self.assertEqual(by_agent[11]["CimsAuth.JwtSecret"]["src"], "injected")
        from handlers.agents import _SECRET_MASK
        self.assertEqual(by_agent[11]["CimsAuth.JwtSecret"]["v"], _SECRET_MASK)

    def test_members_present_even_when_judgment_deferred(self):
        """판정이 보류돼도(버전 혼재) 멤버 값 비교는 계속 보여야 한다."""
        self._seed_pkg(1, "csp", version="0.1.0")
        self._seed_pkg(3, "csp", version="0.2.0")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={"Db.Host": "x"})
        self._seed_deployment(6, 11, 3, config={"Db.Host": "y"})
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "unknown")
        self.assertEqual(ev["reason"], "version_mismatch")
        self.assertEqual({m["agent_id"] for m in ev["members"]}, {10, 11})

    def test_secret_values_masked(self):
        """password 필드 값은 조회 응답 관용대로 sentinel — 표시 경로로 평문이 새지 않게."""
        from handlers.agents import _SECRET_MASK
        tmpl = {"version": 1, "sections": [
            {"key": "svc", "title": "S", "scope": "service", "fields": [
                {"key": "Db.Password", "type": "password", "default": ""},
            ]}]}
        self._seed_pkg(1, "csp", template=tmpl)
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        g = self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={"Db.Password": "active-secret"})
        self._seed_deployment(6, 11, 1, config={"Db.Password": "standby-secret"})
        ev = self._evaluate(g)
        self.assertEqual(ev["status"], "out_of_sync")
        d = ev["drift"][0]
        self.assertEqual(d["active"], _SECRET_MASK)
        self.assertEqual(d["members"][0]["value"], _SECRET_MASK)


class TestGroupPkgConfig(_R4Case):
    """_put_group_pkg_config — 그룹 공통 설정 저장 (ON=전 멤버 / OFF=멤버 선택)."""

    def _gput(self, gid, pkg, body):
        from handlers.ha_groups import _put_group_pkg_config
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="PUT",
                         full_path=f"/api/v1/ha-groups/{gid}/packages/{pkg}/config",
                         client_ip="127.0.0.1", client_port=0, body=body)
        return asyncio.run(_put_group_pkg_config(gid, pkg, ha, self.config))

    def test_on_saves_all_members(self):
        self._seed_r4_pair()
        r = self._gput(1, "csp", {"values": {"Db.Host": "10.5.5.5"}})
        self.assertEqual(r.status, 200)
        self.assertTrue(r.body["sync_on"])
        self.assertEqual(len(r.body["members"]), 2)
        self.assertEqual(self._dep(5)["config"]["Db.Host"], "10.5.5.5")
        self.assertEqual(self._dep(6)["config"]["Db.Host"], "10.5.5.5")
        # 고유값 보존 (merge — overlay 교체 아님)
        self.assertEqual(self._dep(6)["config"]["SystemId"], "B")
        self.assertEqual(len(self._jobs()), 2)

    def test_non_service_key_400(self):
        self._seed_r4_pair()
        r = self._gput(1, "csp", {"values": {"SystemId": "X"}})
        self.assertEqual(r.status, 400)
        self.assertEqual(r.body["error"], "non_service_keys")
        self.assertEqual(self._dep(5)["config"]["SystemId"], "A")

    def test_on_with_target_400(self):
        self._seed_r4_pair()
        r = self._gput(1, "csp", {"values": {"Db.Host": "x"}, "target_deployment_id": 6})
        self.assertEqual(r.status, 400)
        self.assertEqual(r.body["error"], "target_not_allowed_while_sync_on")

    def test_on_mixed_versions_409(self):
        self._seed_pkg(1, "csp", version="0.1.0")
        self._seed_pkg(3, "csp", version="0.2.0")
        self._seed_agent_hb(10, "a", ["10.0.0.11", self.VIP])
        self._seed_agent_hb(11, "b", ["10.0.0.12"])
        self._seed_as_group()
        self._seed_deployment(5, 10, 1, config={})
        self._seed_deployment(6, 11, 3, config={})
        r = self._gput(1, "csp", {"values": {"Db.Host": "x"}})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "version_mismatch")

    def test_off_requires_target(self):
        self._seed_r4_pair(auto_sync={"csp": False})
        r = self._gput(1, "csp", {"values": {"Db.Host": "x"}})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "target_required_while_sync_off")

    def test_off_saves_target_only(self):
        self._seed_r4_pair(auto_sync={"csp": False})
        r = self._gput(1, "csp", {"values": {"Db.Host": "10.7.7.7"},
                                  "target_deployment_id": 6})
        self.assertEqual(r.status, 200)
        self.assertFalse(r.body["sync_on"])
        self.assertEqual(len(r.body["members"]), 1)
        self.assertEqual(self._dep(6)["config"]["Db.Host"], "10.7.7.7")
        self.assertEqual(self._dep(5)["config"]["Db.Host"], "10.9.9.9")   # 무변경

    def test_aa_group_409(self):
        self._seed_pkg(2, "cmp")
        self._seed_agent_hb(30, "m1", ["10.0.1.1"])
        from services import file_store
        d = file_store.domain_dir(self.config, "ha_groups")
        file_store.save(d, 2, {"id": 2, "name": "media", "mode": "all_active",
                               "members": [{"agent_id": 30, "priority": 100}]})
        self._seed_deployment(8, 30, 2, config={})
        r = self._gput(2, "cmp", {"values": {"Db.Host": "x"}})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "not_active_standby")


class TestGroupAutoSyncSwitch(_R4Case):
    """_put_group_auto_sync — 스위치 영속 + ON 전환 즉시 정합."""

    def _gswitch(self, gid, pkg, enabled):
        from handlers.ha_groups import _put_group_auto_sync
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="PUT",
                         full_path=f"/api/v1/ha-groups/{gid}/packages/{pkg}/auto-sync",
                         client_ip="127.0.0.1", client_port=0,
                         body={"enabled": enabled})
        return asyncio.run(_put_group_auto_sync(gid, pkg, ha, self.config))

    def _group_rec(self, gid):
        from services import file_store
        return file_store.by_id(file_store.domain_dir(self.config, "ha_groups"), gid)

    def test_off_persists_no_reconcile(self):
        self._seed_r4_pair()
        r = self._gswitch(1, "csp", False)
        self.assertEqual(r.status, 200)
        self.assertEqual(self._group_rec(1)["auto_sync"], {"csp": False})
        self.assertIsNone(r.body["reconcile"])
        self.assertEqual(self._dep(6)["config"]["Db.Host"], "10.0.0.1")   # 무변경

    def test_on_persists_and_reconciles(self):
        """OFF → ON 전환: 영속 + 즉시 정합 (ACTIVE 기준 STANDBY 교정)."""
        self._seed_r4_pair(auto_sync={"csp": False})
        r = self._gswitch(1, "csp", True)
        self.assertEqual(r.status, 200)
        self.assertEqual(self._group_rec(1)["auto_sync"], {"csp": True})
        rc = r.body["reconcile"]
        self.assertIsNotNone(rc)
        self.assertEqual(rc["active_agent_id"], 10)
        self.assertEqual(rc["status"], "synced")
        self.assertEqual(self._dep(6)["config"]["Db.Host"], "10.9.9.9")


class TestEffectiveScope(unittest.TestCase):
    """필드 유효 scope 헬퍼 — field.scope ?? section.scope, 기본 service."""

    def test_helpers(self):
        from handlers.agents import (_effective_scope, _service_scope_keys,
                                     _service_scope_collections)
        self.assertEqual(_effective_scope({}, "service"), "service")
        self.assertEqual(_effective_scope({}, "system"), "system")
        self.assertEqual(_effective_scope({}, None), "service")
        self.assertEqual(_effective_scope({"scope": "system"}, "service"), "system")
        self.assertEqual(_effective_scope({"scope": "service"}, "system"), "service")
        keys = _service_scope_keys(TEMPLATE)
        self.assertEqual(keys, {"Db.Host", "Db.Port", "Tls.Port"})
        colls = _service_scope_collections(TEMPLATE)
        self.assertEqual(colls, {"routes"})


class TestDerivedSharedStore(_R4Case):
    """공유 store = oam/oam-svc **배포설정에서 유도** — 그룹은 저장하지 않는다.

    옛 구조는 그룹 레코드에 같은 사실을 따로 적어서, 어긋남을 막는 가드를 또 써야 했다.
    (부트스트랩이 설치 시점에 store 를 잡으면 그룹은 그 사실을 모른 채 oam 을 HA 에서
    빼버렸다 — 실제 결함.)
    """

    TPL_OAM = {"version": 1, "sections": [
        {"key": "store", "title": "관리 store", "scope": "system", "fields": [
            {"key": "CimsRuntimeDir",   "type": "path", "default": ""},
            {"key": "CimsRuntimeMount", "type": "path", "default": ""},
            {"key": "Packages.Dir",     "type": "path", "default": ""},
        ]}]}
    # oam-svc 에는 `Packages.Dir` 이 없다 — 패키지 서빙은 base oam 만의 일.
    TPL_SVC = {"version": 1, "sections": [
        {"key": "store", "title": "관리 store", "scope": "system", "fields": [
            {"key": "CimsRuntimeDir",   "type": "path", "default": ""},
            {"key": "CimsRuntimeMount", "type": "path", "default": ""},
        ]}]}

    def _seed_lease_descriptor(self):
        """oam/oam-svc = 리스 보유 모듈 — 주입/전제 판정의 근거."""
        from services import file_store
        file_store.save(file_store.domain_dir(self.config, "services"), 1, {
            "id": "cims", "modules": [
                {"name": "oam",     "safety": {"requires_leader_lease": True}},
                {"name": "oam-svc", "safety": {"requires_leader_lease": True}},
            ]})

    def _seed_store_dep(self, did, aid, proc, mount, pid=None, tpl=None):
        from services import file_store
        pid = pid if pid is not None else (21 if proc == "oam" else 22)
        file_store.save(file_store.domain_dir(self.config, "packages"), pid, {
            "id": pid, "name": proc, "version": "0.1.0",
            "config_template": tpl or (self.TPL_OAM if proc == "oam" else self.TPL_SVC),
            "meta": {"shared_identity": True}})
        cfg = {}
        if mount:
            cfg = {"CimsRuntimeMount": mount, "CimsRuntimeDir": mount + "/runtime"}
        file_store.save(file_store.domain_dir(self.config, "deployments"), did, {
            "id": did, "agent_id": aid, "package_id": pid, "process_name": proc,
            "install_path": "/opt/x", "config": cfg})

    # ── 유도 ────────────────────────────────────────────────────────────────
    def test_derived_when_all_members_agree(self):
        from handlers.ha_groups import _derived_shared_store
        g = self._seed_as_group()
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self._seed_store_dep(6, 11, "oam", "/mnt/cims")
        self.assertEqual(_derived_shared_store(self.config, g),
                         {"mount_point": "/mnt/cims"})

    def test_not_derived_when_one_member_local(self):
        """한 노드만 공유면 절체가 성립하지 않는다 — 공유로 보지 않는다."""
        from handlers.ha_groups import _derived_shared_store
        g = self._seed_as_group()
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self._seed_store_dep(6, 11, "oam", "")
        self.assertEqual(_derived_shared_store(self.config, g), {})

    def test_not_derived_when_paths_differ(self):
        from handlers.ha_groups import _derived_shared_store
        g = self._seed_as_group()
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self._seed_store_dep(6, 11, "oam", "/mnt/other")
        self.assertEqual(_derived_shared_store(self.config, g), {})

    def test_trailing_slash_is_not_a_difference(self):
        from handlers.ha_groups import _derived_shared_store
        from services import file_store
        g = self._seed_as_group()
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self._seed_store_dep(6, 11, "oam", "/mnt/cims/")
        self.assertEqual(_derived_shared_store(self.config, g),
                         {"mount_point": "/mnt/cims"})
        del file_store

    def test_removed_deployment_ignored(self):
        """삭제된 배포가 유도를 깨지 않는다."""
        from handlers.ha_groups import _derived_shared_store
        from services import file_store
        g = self._seed_as_group()
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self._seed_store_dep(6, 11, "oam", "/mnt/cims")
        d = file_store.domain_dir(self.config, "deployments")
        row = file_store.load(d, 6); row["status"] = "removed"; row["config"] = {}
        file_store.save(d, 6, row)
        self.assertEqual(_derived_shared_store(self.config, g),
                         {"mount_point": "/mnt/cims"})

    def test_legacy_group_record_is_fallback_only(self):
        """전환기 — 유도 불가 + 옛 그룹 저장값 있으면 그것을 쓴다(HA 편입 유지)."""
        from handlers.ha_groups import _derived_shared_store
        from services import file_store
        g = self._seed_as_group()
        g["shared_store"] = {"mount_point": "/nas/legacy"}
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        self._seed_store_dep(5, 10, "oam", "")
        self.assertEqual(_derived_shared_store(self.config, g),
                         {"mount_point": "/nas/legacy"})
        # 배포설정이 채워지면 유도가 이긴다 (폴백 경로를 더 이상 타지 않는다)
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self.assertEqual(_derived_shared_store(self.config, g),
                         {"mount_point": "/mnt/cims"})

    # ── 전제 게이트 ─────────────────────────────────────────────────────────
    def test_lease_precondition_follows_derived_value(self):
        from handlers.ha_groups import _lease_precondition_unmet
        from services import file_store
        g = self._seed_as_group()
        # 리스 보유 선언 — descriptor 대신 그룹 명세로(운영자 명시가 descriptor 보다 우선).
        g["module_specs"] = {"oam": {"safety": {"requires_leader_lease": True}}}
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        self._seed_store_dep(5, 10, "oam", "")
        self._seed_store_dep(6, 11, "oam", "")
        self.assertEqual(_lease_precondition_unmet(g, "oam", self.config),
                         "no_shared_store")
        # 부트스트랩/이관이 배포설정을 채우면 그룹 조작 없이 편입된다
        self._seed_store_dep(5, 10, "oam", "/mnt/cims")
        self._seed_store_dep(6, 11, "oam", "/mnt/cims")
        self.assertIsNone(_lease_precondition_unmet(g, "oam", self.config))

    # ── 주입 (agents._materialize_deploy_config) ────────────────────────────
    def _materialize(self, pid, overlay):
        from handlers.agents import _materialize_deploy_config, _pkg_load
        return _materialize_deploy_config(self.config, _pkg_load(self.config, pid), overlay)

    def test_store_paths_injected_together(self):
        """Mount 가 빠지면 그 노드의 mount guard 가 꺼지고, Packages.Dir 이 빠지면
        절체 후 패키지를 못 찾는다 — 셋을 함께 준다."""
        self._seed_lease_descriptor()
        self._seed_store_dep(5, 10, "oam", "")
        # CimsRuntimeDir 은 file_store 루트라 tmpdir 그대로 둔다(바꾸면 그 경로를 만든다).
        self.config["CimsRuntimeMount"] = "/mnt/cims"
        self.config["Packages"] = {"Dir": "/mnt/cims/runtime/pkg_files"}
        eff = self._materialize(21, {})
        self.assertEqual(eff.get("CimsRuntimeDir"), self.config["CimsRuntimeDir"])
        self.assertEqual(eff.get("CimsRuntimeMount"), "/mnt/cims")
        self.assertEqual(eff.get("Packages.Dir"), "/mnt/cims/runtime/pkg_files")

    def test_injection_is_template_gated(self):
        """선언 없는 키는 심지 않는다 — oam-svc 에 Packages.Dir 이 생기면 유령 항목."""
        self._seed_lease_descriptor()
        self._seed_store_dep(6, 11, "oam-svc", "")
        self.config["CimsRuntimeMount"] = "/mnt/cims"
        self.config["Packages"] = {"Dir": "/mnt/cims/runtime/pkg_files"}
        eff = self._materialize(22, {})
        self.assertEqual(eff.get("CimsRuntimeMount"), "/mnt/cims")
        self.assertNotIn("Packages.Dir", eff)

    def test_overlay_wins_over_injection(self):
        """이관이 overlay 에 넣은 새 경로를 base 값이 되돌리면 안 된다."""
        self._seed_lease_descriptor()
        self._seed_store_dep(5, 10, "oam", "")
        self.config["CimsRuntimeMount"] = "/mnt/old"
        self.config["Packages"] = {"Dir": "/mnt/old/runtime/pkg_files"}
        eff = self._materialize(21, {"CimsRuntimeMount": "/mnt/new",
                                     "Packages.Dir": "/mnt/new/runtime/pkg_files"})
        self.assertEqual(eff.get("CimsRuntimeMount"), "/mnt/new")
        self.assertEqual(eff.get("Packages.Dir"), "/mnt/new/runtime/pkg_files")

    def test_non_lease_module_gets_no_store_paths(self):
        """서비스 모듈은 리스 획득 코드가 없다 — 경로를 주면 펜싱 없는 두 번째 writer."""
        from services import file_store
        file_store.save(file_store.domain_dir(self.config, "services"), 1, {
            "id": "cims", "modules": [{"name": "csc", "safety": {}}]})
        file_store.save(file_store.domain_dir(self.config, "packages"), 23, {
            "id": 23, "name": "csc", "version": "0.1.0",
            "config_template": self.TPL_OAM, "meta": {"shared_identity": True}})
        self.config["CimsRuntimeMount"] = "/mnt/cims"
        self.config["Packages"] = {"Dir": "/mnt/cims/runtime/pkg_files"}
        eff = self._materialize(23, {})
        self.assertNotIn("CimsRuntimeMount", eff)
        self.assertNotIn("Packages.Dir", eff)


class TestBootstrapConfigShape(_FsCase):
    """부트스트랩이 쓰는 `config.json` 형태 == agent 가 쓰는 실체화본.

    두 writer 가 다른 형태를 쓰면 드리프트 판정(`deploy_config_hash`)이 영구히 어긋나
    **설치 직후부터 A-PRC-003 이 뜬다** (실측: overlay 10키 vs 실체화본 13키 — 템플릿
    기본값 `Server.Role`·`ServiceLogging.{Alert,Event}RetainDays` 누락).
    부트스트랩은 `템플릿 기본값 + overlay` 를 쓴다(install.sh). 이 테스트는 그 규칙이
    `_materialize_deploy_config` 와 **같은 결과**를 내는지를 지킨다 — 새 주입 키가 생기고
    부트스트랩이 그것을 안 쓰면 여기서 깨진다.
    """

    OAM_TPL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "ems", "core", "oam", "config", "config_template.json")

    # install.sh 가 config.json 에 쓰는 키 (부트스트랩 overlay `ov`)
    BOOTSTRAP_KEYS = {
        "Server.Ip", "Server.Port", "CimsRuntimeDir", "Packages.Dir",
        "CimsAuth.JwtSecret", "CimsAuth.BuiltinAccounts", "Server.Role",
        "Server.CertSans", "CimsRuntimeMount", "Server.AgentOamUrl",
        "Mgmt.Cidr", "ServiceLogging.Dir",
    }

    def _bootstrap_shape(self, tpl, overlay):
        """install.sh 의 병합 규칙 — 빈 default 제외, overlay 의 빈 값은 default 를 안 지운다."""
        out = {}
        for sec in tpl.get("sections") or []:
            for f in sec.get("fields") or []:
                k, d = f.get("key"), f.get("default")
                if k and d is not None and d != "" and d != []:
                    out[k] = d
        for k, v in overlay.items():
            if v is None or v == "":
                continue
            out[k] = v
        return out

    def test_bootstrap_shape_equals_materialized(self):
        import json as _json
        from services import file_store
        from handlers.agents import _materialize_deploy_config
        with open(self.OAM_TPL) as f:
            tpl = _json.load(f)
        # 부트스트랩이 쓰는 overlay (값은 형태 비교용 — 실제 사이트 값과 무관)
        overlay = {
            "Server.Ip": "0.0.0.0", "Server.Port": 4419, "Server.Role": "base",
            "CimsRuntimeDir": "/mnt/cims/runtime", "CimsRuntimeMount": "/mnt/cims",
            "Packages.Dir": "/mnt/cims/runtime/pkg_files",
            "ServiceLogging.Dir": "/mnt/cims/service_log",
            "CimsAuth.JwtSecret": "s3cr3t",
            "CimsAuth.BuiltinAccounts": [{"login_id": "admin"}],
            "Server.AgentOamUrl": "https://10.0.0.1:4419",
            "Mgmt.Cidr": "10.0.0.0/24", "Server.CertSans": [],
        }
        pkg = {"id": 1, "name": "oam", "version": "0.1.0",
               "config_template": tpl, "meta": {"shared_identity": True}}
        file_store.save(file_store.domain_dir(self.config, "packages"), 1, pkg)
        # 주입원(살아있는 OAM 의 값) — overlay 가 이미 갖고 있으므로 no-op 이어야 한다.
        self.config["CimsRuntimeMount"] = "/mnt/cims"
        self.config["Packages"] = {"Dir": "/mnt/cims/runtime/pkg_files"}
        self.config["Mgmt"] = {"Cidr": "10.0.0.0/24"}

        want = self._bootstrap_shape(tpl, overlay)
        got = _materialize_deploy_config(self.config, pkg, overlay)
        # 키 집합이 같아야 한다 — 다르면 부트스트랩이 그 키를 안 써서 드리프트가 난다.
        self.assertEqual(sorted(got), sorted(want),
                         "부트스트랩 config.json 형태가 실체화본과 다르다 — "
                         "install.sh 의 config.json 기록에 누락된 키가 있다")
        self.assertEqual(got, want)

    def test_bootstrap_keys_cover_injected_keys(self):
        """주입 대상 키는 전부 부트스트랩 overlay 에도 있어야 한다.

        주입은 '살아있는 OAM 의 값' 을 넣는 것이라, 부트스트랩 노드(자기 자신이 원본)가
        그 키를 안 쓰면 실체화본에만 생겨 형태가 갈라진다.
        """
        injected = {"CimsAuth.JwtSecret", "CimsRuntimeDir", "CimsRuntimeMount",
                    "Packages.Dir", "Mgmt.Cidr", "ServiceLogging.Dir",
                    "CimsAuth.BuiltinAccounts"}
        self.assertEqual(injected - self.BOOTSTRAP_KEYS, set(),
                         "주입 키가 부트스트랩 overlay 에 없다 — install.sh 의 ov 에 추가하라")


class TestEnrollAutoMount(_FsCase):
    """등록(enroll) 직후 마운트 자동 적용 — "시스템 추가" 의 선언을 그 자리에서 집행.

    마운트를 별도 작업으로 두면 운영자가 잊고, 그 노드는 공유 store 를 못 써 **승격
    부적격**이 된다(실측: 계획 절체가 원본을 내려놓은 뒤에야 드러나 관리평면 약 1분 단절).
    집행 경로는 서버별 [마운트 관리]와 동일(`apply_mounts` → `cims-priv mount-add` →
    fstab `# cims-managed`) — 그래서 콘솔 마운트 화면에 자동으로 표시된다.
    """

    def _create_agent(self, name, pending_mounts=None):
        from handlers.agents import _create_agent
        from httpsrv.handler import HandlerArgs
        body = {"name": name}
        if pending_mounts is not None:
            body["pending_mounts"] = pending_mounts
        ha = HandlerArgs(method="POST", full_path="/api/v1/agents",
                         client_ip="10.0.0.9", client_port=0, body=body)
        return asyncio.run(_create_agent(ha, self.config))

    def _enroll(self, token):
        from handlers.agent_api import _enroll
        from httpsrv.handler import HandlerArgs
        ha = HandlerArgs(method="POST", full_path="/api/agent/enroll",
                         client_ip="10.0.0.9", client_port=0,
                         body={"enrollment_token": token, "hostname": "n1"})
        return asyncio.run(_enroll(ha, self.config))

    def _jobs(self, aid):
        from services import file_store
        return [j for j in file_store.load_all(file_store.domain_dir(self.config, "jobs"))
                if j.get("agent_id") == aid]

    MNT = [{"fstype": "nfs4", "source": "nas.example:/export/cims",
            "target": "/mnt/cims", "options": "defaults"}]

    def test_enroll_queues_apply_mounts(self):
        r = self._create_agent("n1", self.MNT)
        self.assertEqual(r.status, 201)
        aid, tok = r.body["id"], r.body["enrollment_token"]
        self.assertEqual(r.body["pending_mounts"], self.MNT)
        self.assertEqual(self._jobs(aid), [])          # 등록 전에는 job 없음
        er = self._enroll(tok)
        self.assertEqual(er.status, 200)
        jobs = [j for j in self._jobs(aid) if j.get("job_type") == "apply_mounts"]
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["params"]["mounts"],
                         [{"op": "add", "fstype": "nfs4",
                           "source": "nas.example:/export/cims",
                           "target": "/mnt/cims", "options": "defaults"}])

    def test_no_declaration_no_job(self):
        """선언이 없으면 아무 것도 하지 않는다 — 기존 동작 불변."""
        r = self._create_agent("n2")
        er = self._enroll(r.body["enrollment_token"])
        self.assertEqual(er.status, 200)
        self.assertEqual([j for j in self._jobs(r.body["id"])
                          if j.get("job_type") == "apply_mounts"], [])

    def test_declaration_survives_reenroll(self):
        """재설치(재 enroll)에도 선언이 남아 다시 적용된다 — mount-add 는 멱등."""
        from handlers.agents import _agent_update
        r = self._create_agent("n3", self.MNT)
        aid = r.body["id"]
        self._enroll(r.body["enrollment_token"])
        _agent_update(self.config, aid, {"enrollment_token": "tok-again"})
        self._enroll("tok-again")
        self.assertEqual(len([j for j in self._jobs(aid)
                              if j.get("job_type") == "apply_mounts"]), 2)

    def test_group_declaration_used_when_agent_has_none(self):
        """[+ 멤버 추가] 경로 — agent 에 선언이 없으면 **소속 그룹 선언**을 쓴다.

        AA 는 그 경로가 유일하고(모달은 서버를 안 만든다), AS 도 3번째 멤버부터는 그 경로다.
        멤버마다 마운트를 다시 입력하게 하면 결국 잊는다(실측: Media(AA) 두 노드가 마운트
        없이 재설치돼 서비스 로그를 못 썼다).
        """
        from services import file_store
        r = self._create_agent("m1")                     # 선언 없이 생성
        aid = r.body["id"]
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 7, {
            "id": 7, "name": "Media", "mode": "all_active",
            "members": [{"agent_id": aid, "role": "backup", "priority": 90}],
            "mounts": [{"target": "/mnt/cims", "source": "nas.example:/export/cims",
                        "fstype": "nfs", "options": "defaults"}]})
        er = self._enroll(r.body["enrollment_token"])
        self.assertEqual(er.status, 200)
        jobs = [j for j in self._jobs(aid) if j.get("job_type") == "apply_mounts"]
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["params"]["mounts"][0]["target"], "/mnt/cims")
        self.assertEqual(jobs[0]["params"]["mounts"][0]["op"], "add")

    def test_agent_declaration_wins_over_group(self):
        """agent 선언이 있으면 그것을 쓴다 — standalone·예외 구성용."""
        from services import file_store
        r = self._create_agent("m2", self.MNT)
        aid = r.body["id"]
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 8, {
            "id": 8, "name": "G", "mode": "all_active",
            "members": [{"agent_id": aid}],
            "mounts": [{"target": "/mnt/other", "source": "x:/y", "fstype": "nfs"}]})
        self._enroll(r.body["enrollment_token"])
        jobs = [j for j in self._jobs(aid) if j.get("job_type") == "apply_mounts"]
        self.assertEqual(jobs[0]["params"]["mounts"][0]["target"], "/mnt/cims")

    def test_explicit_empty_blocks_group_inheritance(self):
        """`pending_mounts: []` = "마운트하지 않음"(명시) — 그룹 선언으로 뒤집히지 않는다.

        [+ 멤버 추가]에서 체크를 끈 경우다. 미지정(키 없음)과 뜻이 달라야 한다 — 끈 것이
        조용히 상속되면 운영자 의도와 반대로 마운트된다.
        """
        from services import file_store
        r = self._create_agent("m4", [])                 # 명시적 없음
        aid = r.body["id"]
        self.assertEqual(r.body["pending_mounts"], [])   # 키가 저장돼야 한다
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 10, {
            "id": 10, "name": "G3", "mode": "all_active",
            "members": [{"agent_id": aid}],
            "mounts": [{"target": "/mnt/cims", "source": "nas.example:/export/cims",
                        "fstype": "nfs"}]})
        self._enroll(r.body["enrollment_token"])
        self.assertEqual([j for j in self._jobs(aid)
                          if j.get("job_type") == "apply_mounts"], [])

    def test_group_without_declaration_no_job(self):
        """그룹에도 선언이 없으면 무동작 — 조용히 엉뚱한 값을 붙이지 않는다."""
        from services import file_store
        r = self._create_agent("m3")
        aid = r.body["id"]
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 9, {
            "id": 9, "name": "G2", "mode": "all_active", "members": [{"agent_id": aid}]})
        self._enroll(r.body["enrollment_token"])
        self.assertEqual([j for j in self._jobs(aid)
                          if j.get("job_type") == "apply_mounts"], [])

    def test_invalid_declaration_dropped_at_save(self):
        """cims-priv 가 거부할 값은 레코드에 굳히지 않는다 — job 이 조용히 실패하지 않게."""
        bad = [
            {"fstype": "nfs4", "source": "s:/x", "target": "relative"},      # 절대경로 아님
            {"fstype": "nfs4", "source": "s:/x", "target": "/a/../b"},       # .. 포함
            {"fstype": "nfs4", "source": "",     "target": "/mnt/a"},        # source 없음
            {"fstype": "zzz",  "source": "s:/x", "target": "/mnt/b"},        # 미지원 fstype
            {"fstype": "nfs",  "source": "s:/x", "target": "/mnt/ok"},       # 유효
        ]
        r = self._create_agent("n4", bad)
        self.assertEqual(r.body["pending_mounts"],
                         [{"fstype": "nfs", "source": "s:/x",
                           "target": "/mnt/ok", "options": "defaults"}])


class TestSequencedArming(_R4Case):
    """순차 무장 — 선행 멤버가 VIP 를 잡은 것을 확인한 뒤 나머지를 무장.

    개시 국면에 양쪽 keepalived 를 동시에 켜면 선거가 priority 가 아니라 "누가 먼저
    track_script 를 통과하나" 로 결정된다(`nopreempt` 가 VRRP 의 우선순위 규칙을 끈다).
    실측: 0.18초 차이로 동시 무장 → priority 90 노드가 Active.
    원인은 발동 조건이 서비스 키를 **그룹 이름**으로 찾은 것 — 실제 키는 `g<id>` 라
    항상 None → 블록 통째로 스킵. 이 테스트가 그 회귀를 막는다.
    """

    def _armed_group(self, gid=1, member_agent_ids=(10, 11)):
        """무장(service_intent running) + VIP binding 있는 AS 그룹 + 멤버별 csp 배포."""
        from services import file_store
        self._seed_pkg(1, "csp")
        for i, aid in enumerate(member_agent_ids):
            self._seed_agent_hb(aid, f"n{aid}", [f"10.0.0.{aid}"])
            self._seed_deployment(100 + i, aid, 1, config={})
            d = file_store.domain_dir(self.config, "deployments")
            row = file_store.load(d, 100 + i)
            row.update({"process_name": "csp", "status": "running"})
            file_store.save(d, 100 + i, row)
        g = self._seed_as_group(gid=gid, member_agent_ids=member_agent_ids)
        # 렌더가 요구하는 필드 (keepalived vrrp_instance)
        g.update({"vrid": 51, "vip_mask": 24, "auth_pass": "pw",
                  "service_intent": {"csp": "running"}})
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), gid, g)
        return g

    def _jobs_of(self, aid, jt="update_ha"):
        from services import file_store
        return [j for j in file_store.load_all(file_store.domain_dir(self.config, "jobs"))
                if j.get("agent_id") == aid and j.get("job_type") == jt]

    def _group(self, gid=1):
        from services import file_store
        return file_store.load(file_store.domain_dir(self.config, "ha_groups"), gid)

    def _enqueue(self, gid=1, prefer=None):
        from handlers.ha_groups import _enqueue_update_ha_for_members
        return _enqueue_update_ha_for_members(gid, self.config, prefer)

    # ── 보류가 실제로 걸린다 (옛 버그: 서비스 키 오조회로 항상 스킵) ──────────
    def test_only_leader_armed_and_peer_held(self):
        self._armed_group()
        n = self._enqueue(prefer={10})
        self.assertEqual(n, 1, "선행 1대만 무장돼야 한다")
        self.assertEqual(len(self._jobs_of(10)), 1)
        self.assertEqual(self._jobs_of(11), [], "나머지는 job 자체가 없어야 한다")
        pend = self._group().get("pending_arm")
        self.assertEqual(pend["leaders"], [10])
        self.assertEqual(pend["peers"], [11])

    def test_master_is_leader_when_not_specified(self):
        """일괄 시작 — prefer 미지정이면 priority 최대(지정 마스터)가 선행."""
        self._armed_group()                       # priority 100(=10) / 99(=11)
        self._enqueue()
        self.assertEqual(self._group()["pending_arm"]["leaders"], [10])

    # ── 확인되면 해제 ────────────────────────────────────────────────────────
    def test_peer_armed_after_leader_holds_vip(self):
        from handlers.ha_groups import sweep_pending_arm
        self._armed_group()
        self._enqueue(prefer={10})
        # 아직 아무도 VIP 없음 → 보류 유지
        self.assertEqual(sweep_pending_arm(self.config), 0)
        self.assertIsNotNone(self._group().get("pending_arm"))
        self.assertEqual(self._jobs_of(11), [])
        # 선행이 VIP 를 잡았다고 관측 → 해제
        self._seed_agent_hb(10, "n10", ["10.0.0.10", self.VIP])
        self.assertEqual(sweep_pending_arm(self.config), 1)
        self.assertIsNone(self._group().get("pending_arm"))
        self.assertEqual(len(self._jobs_of(11)), 1, "나머지가 무장돼야 한다")

    def test_timeout_arms_peer_anyway(self):
        """상한 초과 — 서비스가 아예 안 뜨는 것보다 낫다. 대신 경고를 남긴다."""
        from services import file_store
        from handlers.ha_groups import sweep_pending_arm
        self._armed_group()
        self._enqueue(prefer={10})
        g = self._group()
        g["pending_arm"]["deadline"] = "2020-01-01T00:00:00"
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        self.assertEqual(sweep_pending_arm(self.config), 1)
        self.assertIsNone(self._group().get("pending_arm"))
        self.assertEqual(len(self._jobs_of(11)), 1)

    def test_other_member_holding_vip_releases_hold(self):
        """선행이 아닌 노드가 이미 VIP 를 잡았다 = 보류가 무의미 → 즉시 해제."""
        from handlers.ha_groups import sweep_pending_arm
        self._armed_group()
        self._enqueue(prefer={10})
        self._seed_agent_hb(11, "n11", ["10.0.0.11", self.VIP])
        self.assertEqual(sweep_pending_arm(self.config), 1)
        self.assertIsNone(self._group().get("pending_arm"))

    # ── 보류하지 않는 경우 ───────────────────────────────────────────────────
    def test_no_hold_when_someone_holds_vip(self):
        """운영 중 재렌더([적용]·[재적용]) — 붙어 있는 VIP 를 흔들지 않는다."""
        self._armed_group()
        self._seed_agent_hb(10, "n10", ["10.0.0.10", self.VIP])
        n = self._enqueue()
        self.assertEqual(n, 2, "전원 즉시 무장")
        self.assertIsNone(self._group().get("pending_arm"))

    def test_no_hold_when_unarmed(self):
        """미개시 그룹 — keepalived 를 켜지 않으므로 순서가 의미 없다."""
        from services import file_store
        self._armed_group()
        g = self._group()
        g["service_intent"] = {}                  # 무장 해제
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        n = self._enqueue()
        self.assertEqual(n, 2)
        self.assertIsNone(self._group().get("pending_arm"))

    def test_stale_hold_is_cleared_on_non_staggered_render(self):
        """보류를 남긴 채 조건이 바뀌면 걷는다 — 안 걷으면 그 멤버가 영구 미무장."""
        self._armed_group()
        self._enqueue(prefer={10})
        self.assertIsNotNone(self._group().get("pending_arm"))
        self._seed_agent_hb(10, "n10", ["10.0.0.10", self.VIP])   # 보유자 생김
        self._enqueue()
        self.assertIsNone(self._group().get("pending_arm"))


class TestOamUrlMisdirect(_R4Case):
    """OAM 접속 주소 어긋남 판정 — **VIP 가 실제로 붙은 뒤에만**.

    개시 전에는 어느 노드도 VIP 를 갖지 않아 전 agent 가 노드 IP 로 보고하는 것이 정상이다.
    그때도 어긋남으로 잡으면 설치 직후부터 상시 경고가 되어 신호가 무의미해진다(옛 콘솔
    배너가 그 상태였다). VIP 보유가 관측된 뒤에야 "이대로 절체하면 단절" 이 참이 된다.
    """

    def _oam_group(self, vip_holder=None):
        """oam 을 호스팅하는 AS 그룹 + agent 2대. vip_holder 가 있으면 그 노드가 VIP 보유."""
        from services import file_store
        self._seed_pkg(1, "oam")
        for i, aid in enumerate((10, 11)):
            self._seed_agent_hb(aid, f"n{aid}",
                                [f"10.0.0.{aid}"] + ([self.VIP] if aid == vip_holder else []))
            d = file_store.domain_dir(self.config, "deployments")
            self._seed_deployment(200 + i, aid, 1, config={})
            row = file_store.load(d, 200 + i)
            row.update({"process_name": "oam", "status": "running"})
            file_store.save(d, 200 + i, row)
        return self._seed_as_group(member_agent_ids=(10, 11))

    def _set_oam_url(self, aid, url):
        from services import file_store
        d = file_store.domain_dir(self.config, "agents")
        row = file_store.load(d, aid); row["oam_url"] = url
        file_store.save(d, aid, row)

    def _misdirected(self, g):
        from handlers.ha_groups import _agents_not_on_vip
        return sorted(a["agent_id"] for a in _agents_not_on_vip(self.config, g))

    def test_no_verdict_before_vip_is_held(self):
        """개시 전 — 아무도 VIP 를 갖지 않으면 판정하지 않는다(오탐 차단)."""
        g = self._oam_group(vip_holder=None)
        self._set_oam_url(10, "https://10.0.0.10:4419")   # 노드 IP = 개시 전 정상
        self._set_oam_url(11, "https://10.0.0.10:4419")
        self.assertEqual(self._misdirected(g), [])

    def test_detected_once_vip_is_held(self):
        """VIP 가 붙은 뒤 — 노드 IP 로 보고하는 agent 가 어긋남으로 잡힌다."""
        g = self._oam_group(vip_holder=10)
        self._set_oam_url(10, "https://10.0.0.10:4419")   # 노드 IP → 어긋남
        self._set_oam_url(11, f"https://{self.VIP}:4419") # VIP → 정상
        self.assertEqual(self._misdirected(g), [10])

    def test_all_on_vip_is_clean(self):
        g = self._oam_group(vip_holder=10)
        for aid in (10, 11):
            self._set_oam_url(aid, f"https://{self.VIP}:4419")
        self.assertEqual(self._misdirected(g), [])

    def test_loopback_counts_as_misdirect(self):
        """loopback 은 그 노드 자신의 OAM — Active 가 바뀌면 역시 끊긴다."""
        g = self._oam_group(vip_holder=10)
        self._set_oam_url(10, "https://127.0.0.1:4419")
        self._set_oam_url(11, f"https://{self.VIP}:4419")
        self.assertEqual(self._misdirected(g), [10])

    def test_no_report_is_not_judged(self):
        """구 버전 agent(보고 없음)는 판정 유보 — 오알람 없음."""
        g = self._oam_group(vip_holder=10)
        self._set_oam_url(11, f"https://{self.VIP}:4419")
        self.assertEqual(self._misdirected(g), [])        # 10 은 oam_url 없음

    def test_non_oam_group_is_skipped(self):
        """oam 이 없는 그룹(Signaling·Media)의 VIP 와 비교하면 전원 어긋남으로 잡혀
        그 그룹의 절체까지 막힌다 — 판정 대상이 아니다."""
        from services import file_store
        g = self._oam_group(vip_holder=10)
        d = file_store.domain_dir(self.config, "deployments")
        for did in (200, 201):                            # oam → csp 로 바꿔 호스팅 해제
            row = file_store.load(d, did); row["process_name"] = "csp"
            file_store.save(d, did, row)
        self._set_oam_url(10, "https://10.0.0.10:4419")
        self.assertEqual(self._misdirected(g), [])


if __name__ == "__main__":
    unittest.main()
