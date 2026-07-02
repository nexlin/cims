# Agent — 배포/프로세스 제어 데몬

**단일 Python 바이너리** (`agent/cims_agent.py`) 로 구성된 배포 데몬. 각 호스트에서 실행되며 CSC 의 명령을 수행하고 설정 jsonl 을 관리합니다.

## 1. 역할

1. **Enroll**: 최초 기동 시 enrollment 토큰으로 CSC 에 등록 → session 토큰 수령
2. **Heartbeat**: 30초 주기로 CSC 에 상태 보고 + pending job 수령
3. **Job 실행**:
   - `install` / `upgrade`: 패키지 다운로드 + tar 풀기 + config 이관
   - `start` / `stop` / `restart`: `install_path/cims.sh` 호출
   - `update_config`: `config.json` 재기록
   - `uninstall`: install_path 제거
   - `upgrade_agent`: 신 버전을 `agent/<신버전>/` 에 전개 → `current` flip → execv self-restart
   - `rollback_agent`: `current` 를 직전(또는 지정) 버전 디렉토리로 flip → execv (다운로드 불요)
   - `health_check`: 포트 probe
4. **Sync REST 서버** (HTTPS): CSC 가 collection(jsonl) 을 즉시 read/write 할 수 있게 REST 엔드포인트 노출

## 2. 운영 정책 (단일 systemd + linger)

| 요구 | 보장 방식 |
|---|---|
| die 시 자동 재기동 | systemd `Restart=always`, `RestartSec=10` |
| host 재기동 시 자동 기동 | `loginctl enable-linger $USER` + unit `WantedBy=default.target` |
| 1 user = 1 agent | unit 이름 단일 (`cims-agent.service`). 같은 호스트에 여러 agent 필요 시 별도 user (`cims2` 등) 로 install |

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
- metric payload: `cpu_pct(/proc/stat) / mem_pct / disk_pct(root) / mounts[] (마운트별 사용률, /proc/mounts+statvfs) / load_avg / per_iface[] (rx/tx + rate + errors) / modules[]`.
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
