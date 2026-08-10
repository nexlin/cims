# Agent — 배포/프로세스 제어 데몬

**단일 Python 바이너리** (`agent/cims_agent.py`) 로 구성된 배포 데몬. 각 호스트에서 실행되며 CSC 의 명령을 수행하고 설정 jsonl 을 관리합니다.

## 1. 역할

1. **Enroll**: 최초 기동 시 enrollment 토큰으로 CSC 에 등록 → session 토큰 수령
2. **Heartbeat**: 2초 주기(기본, `--heartbeat-sec`)로 OAM 에 상태 보고 + pending job 수령. 보고 항목에 **`ha_state`**(verdict·래치 요약 — 래치는 노드 로컬 파일이라 이 보고 없이는 콘솔이 "승격 불가" 를 표시할 수 없다)와 **`oam_url`**(이 agent 가 실제로 보고하는 주소 — OAM 이 "VIP 로 전환 필요" 를 감지·경고하고 수동 절체 전에 점검하는 근거. 이 값이 없어 절체 후 fleet 단절을 아무도 몰랐다)과 **`mount_targets`**(실제 마운트 목록 `{target,fstype,source}` — 의사 FS 제외, cims-managed 여부 무관)이 포함된다: 공유 store 의 마운트 지점을 **자유 입력이 아니라 실제 마운트에서 고르게** 하는 근거이고(`mounts` 는 cims-managed desired 라 운영자가 미리 붙여둔 NAS 가 빠진다), 서버는 이 목록으로 저장·이관을 검증한다. 접속 주소는 **`<state-dir>/oam_url`(있으면) > unit 인자** — 상태 파일이 있어 재설치 없이 무중단 재지정된다(VIP 전환). OAM 불통 시 5→10→…→60초 지수 backoff 후 재시도
3. **Job 실행** — **heartbeat 루프와 분리된 전용 worker 스레드**. 긴 job(패키지·keepalived 설치, 업그레이드)이 heartbeat 를 막으면 OAM 이 그 노드를 offline 으로 오판하고 VIP 관측(heartbeat 의 `interfaces[]`)이 stale 이 되어 HA 판정까지 틀어진다. worker 는 **레인 2개**다 — `ha`(update_ha·ha_keepalived·ha_planned_release·ha_maintenance·ha_clear_holds)와 `module`(install/upgrade/start/stop/…). **레인 안에서는 직렬**(같은 모듈의 install→start 순서 보존), **레인끼리는 병렬**이다: VIP 가 걸린 A/S 그룹은 배포 생성마다 `update_ha` 가 큐에 들어가고 그 job 이 keepalived **dpkg 설치**를 포함하는데, 단일 worker 에서는 뒤따르는 모듈 install 이 수십 초~수 분 대기해 콘솔에서 `deploying` 에 멈춘 것처럼 보였다(실측). 두 작업은 자원이 겹치지 않는다(keepalived 유닛 vs 모듈 tarball). self-exec 계열(`upgrade_agent`/`rollback_agent`/`agent_restart`)은 worker 가 요청만 기록하고 execv 는 메인 루프가 수행:
   - `install` / `upgrade`: 패키지 다운로드 + tar 풀기 + config 이관
   - `start` / `stop` / `restart`: `install_path/cims.sh` 호출
   - `update_config`: `config.json` 재기록
   - **관리평면 설정 자가 복구**: `start_oam` 은 `/health` 게이트를 통과한 설정만 `config.json.last-good` 으로 승격하고, 기동 실패 시 그 값으로 되돌려 1회 재기동한다(실패 설정은 `config.json.failed-<시각>` 보관, 되돌림은 `config.json.rolled-back` 마커 → 콘솔 배너). 관리평면은 자기가 복구 통로라 잘못된 설정 하나로 영구 정지될 수 있다 ([oam_ha.md](../features/oam_ha.md) §9.5)
   - **base deps 보증기** (job 아님, 300초 루프): vendor deb(keepalived·NFS 클라이언트·공유 lib) 설치를 **백그라운드**에서 보증한다. 옛 구조는 heartbeat 루프 직전에 동기로 호출해, dpkg OS 락(`unattended-upgrade` 가 새 서버에서 수 분 점유)을 기다리는 동안 agent 가 **pending 으로 고착**했다(실측 102초). 상태 판정은 `cims-priv base-deps-status`(설치 없이 조회만, 락 무관)
   - **keepalived 설치 보증기** (job 아님, 상태 기반 60초 루프): ha.json 에 VIP 서비스가 있는데(무장) keepalived 패키지가 없으면 `install → config → apply` 를 재시도한다. 설치를 시도하는 주체가 `update_ha` job 뿐이면, 한 번 실패하고 이벤트가 소진됐을 때 **아무도 다시 시도하지 않아** VIP 주인이 영영 없고 cold 모듈(관리평면 포함)이 어디서도 기동하지 못한다(실측). 실패 원인은 대개 일시적이다 — 우분투 `unattended-upgrade` 가 수 분간 dpkg 를 점유(실측 14:21~14:28). 평가 루프를 막지 않도록 전용 스레드에서 돈다
   - `migrate_oam_store`: **관리 store 를 공유 마운트(NAS)로 이관** — 마운트·write 확인 → 모듈 정지 → 복사(`_secrets`·`cert` 제외, target 에 없는 항목만=멱등) → `config.json` 기록 → 기동. OAM 은 자기 store 를 자기가 옮길 수 없어 agent 가 주체다. 실패 시 **구 설정으로 되돌려 기동** ([oam_ha.md](../features/oam_ha.md) §9.4)
   - `set_oam_url`: **OAM 접속 주소 재지정** — 새 주소로 `/health` 도달 확인 후 `<state-dir>/oam_url` 기록 + self-restart. 도달 불가 시 미변경·실패(이중화 전환 시 fleet 단절 방지)
   - `uninstall`: install_path 제거
   - `upgrade_agent`: 신 버전을 `agent/<신버전>/` 에 전개 → `current` flip → execv self-restart
   - `rollback_agent`: `current` 를 직전(또는 지정) 버전 디렉토리로 flip → execv (다운로드 불요)
   - `health_check`: 포트 probe
4. **Sync REST 서버** (HTTPS): CSC 가 collection(jsonl) 을 즉시 read/write 할 수 있게 REST 엔드포인트 노출
5. **HA 로컬 판정**: Health Checker(모듈 생존·readiness·preflight) + Recovery Supervisor
   (로컬 복구·재기동 정책·verdict 생성·역할 reconcile). keepalived 는 이 verdict 만 입력으로
   받아 절체한다. 책임 분리·상태 모델·파일 레이아웃의 정본은
   [../features/ha_service_model.md](../features/ha_service_model.md).

## 2. 운영 정책 (단일 systemd + linger)

| 요구 | 보장 방식 |
|---|---|
| die 시 자동 재기동 | systemd `Restart=always`, `RestartSec=10` |
| host 재기동 시 자동 기동 | `loginctl enable-linger $USER` + unit `WantedBy=default.target` |
| 1 user = 1 agent | unit 이름 단일 (`cims-agent.service`). 같은 호스트에 여러 agent 필요 시 별도 user (`cims2` 등) 로 install |
| agent 재기동이 모듈에 무영향 | unit `KillMode=process` — agent 가 기동한 모듈(cims-svc & 백그라운드)은 unit cgroup 에 있지만 main process 만 kill. 모듈 lifecycle 은 pidfile 기반 cims-svc + watchdog 이 관리 (agent 는 감독자, 소유자 아님). update.sh 는 unit 을 유지하므로 기존 설치본은 agent 가 기동 시 drop-in(`cims-agent.service.d/10-cims-killmode.conf`)으로 자가 적용 |

install 시점부터 systemd 로 운영한다.

## 3. 파일 레이아웃

agent 자신도 **버전 단위 + `current` 심볼릭**으로 설치된다(모듈과 동일 — [02_deployment.md §2](../02_deployment.md)).
systemd `ExecStart` 와 sudoers 는 고정 경로 `agent/current/...` 를 가리키므로 버전이 바뀌어도 불변이다.

```
<INSTALL_DIR, 예: /opt/cims-agent>/   ← prefix (CIMS_AGENT_PREFIX)
├── agent/
│   ├── 0.0.40/  0.0.41/          버전 디렉토리 (최신 3개 유지)
│   │   ├── cims_agent.py             바이너리
│   │   ├── bin/{cims-priv, cims-ha, cims-svc, ...}
│   │   └── lib/, keepalived/, systemd/, pkg.json
│   └── current -> 0.0.41         활성 버전 심볼릭 (ExecStart·sudoers 의 고정 경로)
├── update.sh                     bundle 갱신(--update-only) + systemctl restart
├── uninstall.sh                  완전 제거 (systemd unit + sudoers + keepalived + 파일)
├── setup-sudoers.sh              root 실행 — sudoers + linger (install-agent.sh 가 자동 호출)
├── state/                        ← 버전 밖 (업그레이드/롤백 생존)
│   ├── state.json                {agent_id, session_token, name}
│   ├── agent.crt / agent.key     self-signed 인증서 (Sync REST 서버용)
├── run/                          ← 버전 밖 (supervised.json / managed_ips.json / pending_reports.jsonl)
└── modules/                      배포된 모듈들
    └── <module>/{<version>/, current -> <version>}

~/.config/systemd/user/cims-agent.service       (install-agent.sh 가 작성 — Environment=CIMS_AGENT_PREFIX, ExecStart=agent/current/cims_agent.py)
/etc/sudoers.d/cims-priv                         (setup-sudoers.sh 가 작성 — agent/current/bin/cims-{priv,ha})
```

- **prefix 도출**: `CIMS_AGENT_PREFIX`(systemd Environment) 우선, 없으면 `__file__` 에서 `agent`
  디렉토리 컴포넌트까지 거슬러 올라가 그 부모를 prefix 로 삼는다 — flat/버전화/`current` 경유 무관.

## 4. 상태 머신

| 상태 | 조건 |
|---|---|
| `pending` | CSC 가 agent 를 생성했으나 enroll 아직 안 됨 |
| `approved` | enroll 완료, heartbeat 시작 전 또는 중단 |
| `online` | 최근 heartbeat 수신 |
| `offline` | 일정 시간 heartbeat 끊김 (sweep `AgentStaleSec` 기본 8s = heartbeat 2s × 4) |
| `revoked` | 관리자가 세션 폐기 — re-enroll 불가 |

**re-enroll 시나리오 (host 다운/재install)**: token 재발급 후 enroll 호출 시 `pending`/`approved`/`offline` 상태 모두 허용 (`revoked` 만 차단).

## 5. 설치 방법 (2단계)

```bash
# 1단계: install-agent.sh — bundle 다운로드 + sub-script 작성
cd /opt/cims-agent && \
curl -k https://<CSC>:4419/install-agent.sh | bash -s -- \
  --csc-url https://<CSC>:4419 \
  --enrollment-token <TOKEN> \
  --name <AGENT_NAME>

# 2단계: init.sh — sudoers + linger + enroll + systemd unit + enable --now (sudo 비번 1회)
/opt/cims-agent/init.sh
```

설치 후 운영 명령:

```bash
systemctl --user status   cims-agent
systemctl --user restart  cims-agent
systemctl --user stop     cims-agent
journalctl  --user -u cims-agent -f
```

## 6. Sync REST 엔드포인트

TLS + `X-Agent-Token` 인증. 세부 명세는 `api/collection_api.md` 의 Agent 측 엔드포인트 참조.

| 메서드 | 경로 | 용도 |
|---|---|---|
| GET | `/health` | 헬스 + agent_id + 버전 |
| GET | `/health-check?scope=ha\|modules\|all` | on-demand 종합 진단 (keepalived/VIP + 모듈 상태 + verdict) — §10 |
| GET | `/collection?install_path=&name=` | jsonl 읽기 |
| PUT | `/collection?install_path=&name=` | jsonl 원자 쓰기 + 선택적 SIGUSR1 |
| POST | `/signal?install_path=&sig=usr1\|hup` | 프로세스에 시그널 |
| POST | `/apply-ip-config` | service IP / route 적용 (cims-priv ip-add/del, route-add/del) — §11 |
| POST | `/apply-mounts` | 마운트 적용 (cims-priv mount-add/del, fstab 영속) — §11 |

## 7. 프로세스 시그널 룰

Agent 는 PUT /collection 의 `signal=true` 파라미터 수신 시 `install_path/run/*.pid` 파일을 찾아 해당 pid 에 SIGUSR1 전송. CSP 는 SIGUSR1 수신 시 jsonl 을 재로드합니다.

## 8. 보안

- 세션 토큰은 `state/state.json` (파일 모드 0600)
- Sync REST: 자체 서명 인증서 (`state/agent.crt/key`) + session 토큰 헤더
- 업그레이드/롤백: 신 버전을 `agent/<ver>/` 에 병렬 전개하고 `current` 심볼릭만 flip 한 뒤 `os.execv` 로 같은 PID self-restart(`current/cims_agent.py` 가 ExecStart·argv 라 새 타겟 자동 실행). 실패 시 systemd `Restart=always` 가 `current` 로 부활. 구버전 디렉토리는 prune(최신 3개) 까지 보존되어 롤백은 다운로드 없이 flip 만으로 가능
- `setup-sudoers.sh` 가 NOPASSWD 화이트리스트로 `cims-priv`, `cims-ha` 만 허용. 그 외 sudo 권한 없음.

## 9. 운영 시 주의

- **`state/` 디렉토리 손실 = re-enroll 필요** (Console 에서 token 재발급 → install_command 다시 1줄 → init.sh)
- Agent 업그레이드는 `upgrade_agent` job 또는 `update.sh`, 롤백은 `rollback_agent` job 으로만 수행 (파일/심볼릭 수동 교체 시 세션 상태 불일치). 설치된 버전 목록은 heartbeat 가 `agent_versions` 로 보고 → 콘솔이 롤백 대상 선택에 사용
- 동일 호스트에 Agent 여러 개 필요 시 **별도 user 로 install** (`cims2` 등). 같은 user 에 다중 install 은 unit 이름 충돌로 불가

## 10. 관측성 (Observability)

ssh-free 운영을 위한 두 축 — **raw metric 시계열**(통계/알람)과 **on-demand health check**(즉시 진단).

### 10.1 Heartbeat / Metric (raw 시계열)

- agent 는 기본 **2초** 주기로 heartbeat + metric 전송 (`DEFAULT_HEARTBEAT_SEC` / `DEFAULT_METRIC_SEC`).
- metric payload: `cpu_pct(/proc/stat) / mem_pct / disk_pct(root) / mounts[] (마운트별 사용률, /proc/mounts+statvfs) / load_avg / per_iface[] (rx/tx + rate + errors) / modules[] / cfg_hashes{} / ha_transitions{}`.
  - `cfg_hashes` = {모듈: 배포 config.json canonical hash 12hex} — 설치 모듈 전체(중지 포함, `modules/<mod>/current/<mod>/config.json`, mtime 캐시). OAM `config_drift` 평가(`CIMS-PRC-003`) 입력.
  - `ha_transitions` = {svc: 최근 10분 keepalived 전이 수} — `/var/log/cims-ha/notify_<svc>.log` tail 집계. OAM `ha_flap` 평가(`CIMS-QOS-001`) 입력. 미가독/부재 시 생략.
  - ⚠ OAM `agent_api.py _metric()` 가 record 를 필드 화이트리스트로 저장 — **신규 metric 필드는 화이트리스트 추가 필수**(미추가 시 버려짐). 조회는 `jsonl_tail_recent`(tail-read, 2초 시계열 풀파싱 금지).
- OAM 가 `POST /metric` 수신 → `{CimsRuntimeDir}/metrics/<agent_id>/YYYY/MM/DD.jsonl` append.
- retention: `_sweep_metric_purge` 가 `MetricRetentionDays`(기본 3일) 초과 일별 파일 삭제.
- Console 시각화: ServersPage 메트릭 모달 + HaServicesPage 멤버별 cpu/mem/disk sparkline (공유 `MetricTrend`).

### 10.2 모듈 프로세스 탐지

`collect_metrics()` 와 `_health_check_modules()` 는 공유 `_pgrep_module(name)` 로 모듈 프로세스를 찾는다:

1. `pgrep -a <name>` — C++ 데몬(csp/cmp/isp/cwrtc, comm 매칭).
2. `pgrep -af <stem>_app.py` — python 데몬(csc/oam/oam-svc, comm 이 `python3` 라 1)로 안 잡힘). 패키지명은 하이픈을 가질 수 있으나(`oam-svc`) python 엔트리포인트 파일명은 언더스코어(`oam_svc_app.py`)이므로 `<stem>` 은 모듈명의 하이픈을 언더스코어로 정규화한 값이다.

탐지 대상 = **설치된 모듈**(`modules/<module>/`) ∪ 기본 집합 − 비데몬(`agent`/`console`). 설치 모듈을 동적 enumerate 하므로 isp 등 기본 집합 밖 모듈도 누락 없이 보고 → OAM 의 `module_down` alert 오탐 방지.

### 10.3 On-demand Health Check (`GET /health-check`)

- `scope=ha`: `keepalived` active 여부 + VIP 부여(`ip -j addr` secondary) + `journalctl` tail.
- `scope=modules`: 모듈별 running/pid/cpu/mem/uptime (`_health_check_modules`).
- `scope=all`: 위 둘 + 기본 시스템 metric.
- **verdict** `healthy` / `partial` / `broken` + `issues[]` 자동 산출 (예: keepalived inactive).
- 호출 경로: Console `[🩺 점검]`(서버 단위) 또는 HA 그룹 `[🩺 점검]`(온라인 멤버 동시) → csc admin `POST /api/v1/agents/{id}/health-check` 프록시 → agent sync REST.

### 10.4 Alert 규칙

OAM `_sweep_alerts` 가 평가. 규칙은 service descriptor(`service_registry.alert_rules`) 구동 — **코어 host 규칙** + 서비스 descriptor 규칙 병합.

| scope | type | check | 설명 |
|---|---|---|---|
| core | `disk_high` | disk_pct ≥ threshold(기본 90%) | online agent 별 |
| core | `module_down` | deployment(status=running) 모듈이 metric 실행 집합에 없음 | online agent 별. `process_down` 규칙 대상(csp/cmp)은 제외 — 중복 방지 |
| core | `config_drift` | metric.cfg_hashes[모듈] ≠ 배포기록 실체화본 hash | online agent 별, 배포(status=running/stopped) 단위. 미보고(구 agent) 시 미평가 — 오탐 없음 |
| core | `ha_flap` | metric.ha_transitions[svc] ≥ threshold(기본 6회/10분) | online agent 별. flap 정지 시 윈도 만료로 자동 close |
| service(CIMS) | `csp_down`/`cmp_down` | CSP/CMP stats 응답 없음 | 중앙 poll |
| service(CIMS) | `db_down` | DB 연결 실패 | 중앙 |
| service(CIMS) | `rtp_high` | RTP 포트 사용률 ≥ threshold | 중앙 |

per-agent(scope=agent) 규칙은 agent 별로 펼쳐 평가하며, 관측 불가(오프라인/metric 없음/배포 제거) 시 열린 alert 를 자동 close.
- **uninstall 은 반드시 `./uninstall.sh`** — 단순 `pkill` 만 하면 systemd 가 즉시 재기동 (`Restart=always` + linger). `uninstall.sh` 는 unit stop+disable 부터 처리하므로 안전
- linger 자동 해제 안 함 — 다른 user service 보호. 완전 정리 원하면 `sudo loginctl disable-linger $USER` 별도 실행

## 11. 관리 IP / 라우트 / 마운트 (cims-priv) + 부팅 영속성

운영자가 Console(서버 → 네트워크 탭)에서 추가/삭제하는 호스트 인프라는 모두 `cims-priv`
(NOPASSWD sudoers, 인자 자체검증) 를 통해 적용된다. agent 본체는 비권한.

| 종류 | cims-priv 서브커맨드 | 영속(재부팅 유지) | UI |
|---|---|---|---|
| service IP | `ip-add/ip-del <iface> <cidr>` (label `<iface>:cims` 부여 → managed 식별) | **agent 부팅 재적용** (§11.1) | ServiceIpPanel |
| route | `route-add/route-del <dst> <via> <dev>` (default GW 포함, `ip route replace`) | agent 부팅 재적용 | ServiceIpPanel |
| 마운트 | `mount-add <fstype> <source> <target> [opts]` / `mount-del <target>` | **`/etc/fstab`** (§11.2) | MountPanel |

- 망 분류(NIC role) 모델 폐지 — NIC 용도는 **용도(slot) 단일 키**. VIP→NIC 매핑도 VIP 바인딩
  slot 과 동일 용도(slot) 를 가진 `service_ip_rows` 의 iface 로 결정(`ha_groups._render_ha_for_agent`).
  mgmt 는 `oam Mgmt.Cidr` 자동 도출(편집 불가, 배지 표시), VIP(=HA vip_bindings IP)는 읽기전용.
  VIP 는 서비스망(예 121.161.164.x)에만 둔다 — 내부/관리망 VIP 불필요.

### 11.1 service IP 부팅 재적용 (영속성)
`cims-priv ip-add` 는 런타임 `ip addr add` 라 재부팅에 소실되고, 부팅 직후 OAM 가 unreachable
일 수 있다(실제 사례: reboot 후 OAM 미기동 → DB 서비스IP 소실 → csp 전체 장애). 이를 막기 위해:
- `job_apply_ip_config` 적용 성공 시 현재 cims-managed IP/route 를 `run/managed_ips.json` 에 스냅샷(`_snapshot_managed_ips`).
- agent 기동 시 `run_loop` 가 1회 `reapply_managed_ips()` 실행 — 파일이 있으면 idempotent 재적용,
  없으면(최초 도입/신규 호스트) 현재 상태를 시드 저장. **OAM 연결과 무관**하게 자력 복원.

### 11.2 마운트 (fstab 영속)
- `cims-priv mount-add` 는 target `mkdir -p` 후 `/etc/fstab` 에 `# cims-managed` 태그 라인을
  idempotent 기록(`.cims.bak` 백업) 하고 `mount`. **fstab 기록 = 재부팅 시 OS 자동 마운트**(별도 부팅훅 불필요).
- 네트워크 FS(nfs/nfs4/cifs)는 `_netdev,nofail` 강제 — 마운트 실패/지연이 부팅을 막지 않음.
- 시스템 경로(`/etc`,`/usr`,`/var`,`/home` 등) 보호(거부). `mount-del` 은 umount(`-l` fallback)+fstab 라인 제거.
- agent heartbeat 가 `collect_mounts()`(fstab cims-managed + `mounted` 상태) 보고 → Console MountPanel 표시.
