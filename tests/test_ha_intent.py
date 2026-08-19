"""
HA 무장/해제 의도(ha_intent) 계약 + keepalived 소유 경계 단위 테스트.

배경 — 옛 계약은 "`ha.json.services` 공백 = 해제 의도" 하나였다. 그래서
**"AA 그룹이라 keepalived 를 안 쓴다"(해제)** 와 **"AS 그룹인데 VIP 가 아직
설정되지 않았다"(미정)** 가 같은 신호가 됐고, 배포 진행 중의 미정 상태가 갓 enroll 한
노드로 해제를 내려보냈다. 그 해제는 `rm -rf /etc/keepalived` 로 패키지 소유 디렉토리를
통째로 지웠는데, 같은 배포가 keepalived 를 설치 중이라 패키지 제거는 dpkg 락에 막혀
실패하고 파일 삭제만 성공했다 — 패키지는 dpkg 상 `ii` 인데 배포판 conffile
(`keepalived.config-opts`)이 없어 keepalived 가 기동 불가, Control 그룹에 MASTER 가
없어 cold 모듈(`oam`/`oam-svc`/`csc`)이 어느 노드에서도 뜨지 못했다.

Covers:
  - OAM `_render_ha_for_agent` — armed / disarmed / unknown 3-값 판정
  - agent `job_update_ha` — 의도별 cims-ha 서브커맨드 분기 (구 OAM 호환 포함)
  - `agent/lib/ha.sh` 소유 경계 — apply 스테이징 목록 == disarm 제거 목록,
    배포판 conffile 불가침, `rm -rf /etc/keepalived` 부재

서버 본체는 안 띄운다 — 순수 함수 호출 + 셸 함수 평가만.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import types
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

# 다른 테스트가 csc 쪽 services/handlers 를 먼저 import 했을 수 있으므로 캐시를 비운다
# (fan-out 테스트와 같은 이유 — oam 이 정본).
for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))


class RenderIntentTest(unittest.TestCase):
    """OAM 렌더가 의도를 명시하는지 — 파괴 판단의 유일한 근거."""

    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.cfg = {"CimsRuntimeDir": self._td.name}
        os.environ["CimsRuntimeDir"] = self._td.name
        from handlers import ha_groups
        self.hg = ha_groups
        self.agent = {"id": 1, "name": "vm1", "ip_address": "10.0.0.1", "interfaces": []}
        self.peer = {"id": 2, "name": "vm2", "ip_address": "10.0.0.2", "interfaces": []}
        self.members = [{"agent_id": 1, "priority": 100}, {"agent_id": 2, "priority": 90}]

    def tearDown(self):
        self._td.cleanup()

    def _render(self, mode, vip_bindings):
        group = {"id": 1, "name": getattr(self, "_name", "Control"), "mode": mode, "vrid": 51,
                 "vip_mask": 24, "auth_pass": "x", "members": self.members}
        return self.hg._render_ha_for_agent(group, self.members, 1,
                                            self.agent, self.peer, vip_bindings, self.cfg)

    def test_as_with_vip_is_armed(self):
        r = self._render("active_standby", [{"slot": "svc", "ip": "10.0.0.9"}])
        self.assertEqual(r["ha_intent"], "armed")
        # 서비스 키는 **불변 id 파생**이다 — 이름이 아니다 (identifier_model.md §5.1).
        self.assertIn("g1", r["services"])
        self.assertNotIn("Control", r["services"])
        # 이름은 표시용 라벨로만 실린다 (키가 아님 — §4).
        self.assertEqual(r["services"]["g1"]["name"], "Control")

    def test_service_key_survives_rename(self):
        """이름을 바꿔도 서비스 키가 그대로여야 한다 — rename 이 재키잉이 되면
        옛 키의 절체 래치가 사라져 노드가 검증 없이 승격 후보로 되돌아온다."""
        binding = [{"slot": "svc", "ip": "10.0.0.9"}]
        before = self._render("active_standby", binding)
        self._name = "제어 시스템"          # 이름만 변경 (id 동일)
        after = self._render("active_standby", binding)
        self.assertEqual(list(before["services"]), list(after["services"]))
        self.assertEqual(after["services"]["g1"]["name"], "제어 시스템")

    def test_all_active_is_disarmed(self):
        # AA 는 VIP 를 쓰지 않는다 — keepalived 의도적 미사용 (ha_service_model.md §3).
        r = self._render("all_active", [])
        self.assertEqual(r["ha_intent"], "disarmed")
        self.assertEqual(r["services"], {})

    def test_as_without_vip_is_unknown(self):
        # 배포 진행 중 = 아직 정해지지 않음. 해제로 오독하면 안 된다(이번 사고 경로).
        r = self._render("active_standby", [])
        self.assertEqual(r["ha_intent"], "unknown")
        self.assertEqual(r["services"], {})

    def test_disarm_payload_declares_intent(self):
        # 그룹 이탈·삭제 경로 — 진짜 해제 신호는 여기서만 나온다.
        import inspect
        src = inspect.getsource(self.hg._enqueue_disarm_for_agent)
        self.assertIn('"ha_intent":     "disarmed"', src)


class AgentIntentDispatchTest(unittest.TestCase):
    """agent job_update_ha 가 의도별로 올바른 cims-ha 서브커맨드를 부르는지."""

    BASE = {"node_name": "vm1", "interface": "ens3", "local_ip": "1.1.1.1",
            "peer_ip": "1.1.1.2", "initial_state": "BACKUP", "vip_mask": 24,
            "auth_pass": "x"}
    SVC = {"Control": {"enabled": True, "vrid": 51, "cold_modules": ["oam"]}}

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location(
            "cims_agent_under_test", os.path.join(_REPO, "agent", "cims_agent.py"))
        cls.m = importlib.util.module_from_spec(spec)
        sys.modules["cims_agent_under_test"] = cls.m
        spec.loader.exec_module(cls.m)

    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.m._PREFIX = self._td.name
        self.calls = []

        def fake_run(argv, **kw):
            self.calls.append(list(argv))
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")

        self._orig_run = self.m.subprocess.run
        self.m.subprocess.run = fake_run
        self.m._resolve_cims_ha = lambda: "/fake/cims-ha"
        self.m._maybe_start_supervisor = lambda: None

    def tearDown(self):
        self.m.subprocess.run = self._orig_run
        self._td.cleanup()

    def _subcommands(self, ha_json):
        rc, out, err = self.m.job_update_ha({"ha_json": dict(ha_json)})
        self.assertEqual(rc, 0, f"out={out} err={err}")
        return [c[c.index("--ha-dir") + 2] for c in self.calls if "--ha-dir" in c]

    def test_armed_runs_install_config_apply(self):
        self.assertEqual(
            self._subcommands({**self.BASE, "ha_intent": "armed", "services": self.SVC}),
            ["install", "config", "apply"])

    def test_disarmed_runs_disarm_only(self):
        # 패키지를 건드리는 uninstall/purge 가 아니라 disarm 이어야 한다.
        self.assertEqual(
            self._subcommands({**self.BASE, "ha_intent": "disarmed", "services": {}}),
            ["disarm"])

    def test_unknown_is_noop(self):
        # 사고 경로 — 배포 중간의 빈 services 로는 아무 동작도 하지 않는다.
        self.assertEqual(self._subcommands({**self.BASE, "services": {}}), [])

    def test_unrecognized_intent_with_empty_services_is_noop(self):
        self.assertEqual(
            self._subcommands({**self.BASE, "ha_intent": "off", "services": {}}), [])

    def test_legacy_oam_with_services_is_armed(self):
        # ha_intent 를 모르는 구 OAM 호환 — services 가 있으면 무장으로 본다.
        self.assertEqual(self._subcommands({**self.BASE, "services": self.SVC}),
                         ["install", "config", "apply"])


class OwnershipBoundaryTest(unittest.TestCase):
    """cims-ha 가 지우는 경로가 자기 소유로 한정되는지 (apply/disarm 대칭)."""

    # keepalived 패키지가 /etc/keepalived 에 두는 conffile — 지우면 데몬이 기동 불가.
    DISTRO_CONFFILES = ("keepalived.conf.sample", "keepalived.config-opts")

    def _eval_ha_func(self, func):
        """cims-ha 실행 환경을 흉내내 ha.sh 의 함수 하나를 평가하고 stdout 을 돌려준다."""
        script = f'''
set -euo pipefail
SCRIPT_DIR="{_REPO}/agent/bin"
HA_DIR="{_REPO}/agent/keepalived"; HA_OUT="$HA_DIR/out"; HA_JSON="$HA_DIR/ha.json"
HA_UNIT_DIR="{_REPO}/agent/systemd"
info(){{ :; }}; ok(){{ :; }}; warn(){{ :; }}; err(){{ :; }}; header(){{ :; }}
source "{_REPO}/agent/lib/pkgstate.sh"
source "{_REPO}/agent/lib/ha.sh"
{func}
'''
        p = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        return [l for l in p.stdout.splitlines() if l.strip()]

    def test_owned_paths_match_staged_destinations(self):
        """apply 가 놓는 자리와 disarm 이 지우는 자리가 어긋나면 잔재/오삭제가 난다."""
        staged = [l.split(":", 1)[1] for l in self._eval_ha_func("_ha_staged_pairs")]
        owned = self._eval_ha_func("_ha_owned_paths")
        for dst in staged:
            self.assertIn(dst, owned, f"apply 가 설치하는데 disarm 이 안 지움: {dst}")
        # 스테이징 쌍이 아닌 소유 파일은 sysctl drop-in 하나뿐이다.
        self.assertEqual(sorted(set(owned) - set(staged)),
                         ["/etc/sysctl.d/99-cims-ha.conf"])

    def test_owned_paths_exclude_package_files(self):
        owned = self._eval_ha_func("_ha_owned_paths")
        self.assertNotIn("/etc/keepalived", owned,
                         "디렉토리 자체를 소유로 잡으면 패키지 conffile 까지 날아간다")
        for name in self.DISTRO_CONFFILES:
            self.assertNotIn(f"/etc/keepalived/{name}", owned)

    @staticmethod
    def _command_lines(rel):
        """주석·운영자 안내 문구를 뺀 **실행 라인**만 — 문자열 안의 예시를 오탐하지 않게."""
        src = open(os.path.join(_REPO, rel), encoding="utf-8").read()
        out = []
        for line in src.splitlines():
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if s.split(" ", 1)[0] in ("err", "warn", "info", "ok", "echo"):
                continue          # 로그·안내 문구 (실행이 아니라 사람이 읽는 예시)
            out.append(s)
        return out

    def test_no_recursive_removal_of_package_dir(self):
        """`rm -rf /etc/keepalived` 회귀 차단 — 이번 사고의 직접 원인."""
        for s in self._command_lines("agent/lib/ha.sh"):
            self.assertNotIn("rm -rf /etc/keepalived", s,
                             f"패키지 디렉토리 통삭제 부활 — {s}")
            self.assertNotIn('rm -rf "/etc/keepalived', s)

    def test_integrity_check_ignores_modified_conffiles(self):
        """운영자·배포가 고친 conffile 을 '이상' 으로 보면 매 기동 재설치가 돌고 그 수정이 덮인다.

        고쳐야 하는 것은 **누락(missing)** 뿐이다 — 체크섬 불일치(`??5??????`)는 정상으로
        본다. 실제로 배포가 `/etc/default/nfs-common` 을 편집하므로 오탐이 곧 회귀다.
        """
        script = (
            "set -euo pipefail\n"
            f'source "{_REPO}/agent/lib/pkgstate.sh"\n'
            # dpkg -V 스텁 ① 변조만 (누락 없음) → 정상으로 봐야 한다
            'dpkg() { [[ "${1:-}" == "-V" ]] || return 0; '
            "printf '??5?????? c /etc/default/keepalived\\n'; }\n"
            "_pkg_files_ok fakepkg && echo MODIFIED_OK || echo MODIFIED_BAD\n"
            # 스텁 ② 누락 → 반드시 잡아야 한다
            'dpkg() { [[ "${1:-}" == "-V" ]] || return 0; '
            "printf 'missing   c /etc/keepalived/keepalived.config-opts\\n'; }\n"
            "_pkg_files_ok fakepkg && echo MISSING_OK || echo MISSING_BAD\n"
        )
        p = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.split(), ["MODIFIED_OK", "MISSING_BAD"], p.stdout)

    def test_dpkg_calls_go_through_the_lock(self):
        """dpkg/apt 직접 호출은 설치 레인과 경합해 조용히 실패한다 — 반드시 _cims_dpkg 경유."""
        for rel in ("agent/lib/ha.sh", "agent/bin/cims-priv"):
            for s in self._command_lines(rel):
                if "_cims_dpkg" in s:
                    continue
                for bad in ("sudo dpkg ", "sudo apt-get ", "sudo apt "):
                    self.assertNotIn(bad, s, f"{rel}: 락 우회 호출 — {s}")


if __name__ == "__main__":
    unittest.main()
