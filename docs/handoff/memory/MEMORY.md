# Memory Index

## 🚀 다음 세션 진입 — M1 사용자 브라우저 클릭 검증 + M1.2 agent 재기동 후 LIVE 확인

⭐ **[project_session_2026_05_20_m1_ui_verify.md](project_session_2026_05_20_m1_ui_verify.md)** — 2026-05-20. M1 Console UI 검증 4 시나리오 PASS + **M1.1 `cims.sh tb` 명령** (`42e307d`) + **M1.2 ServiceIp [적용] 동기화** (`8fb6f55`, agent /apply-ip-config sync REST, 큐잉/30s polling 제거, 응답 5~15s→1s 미만). agent 재기동만 사용자 몫 (netns sudo). csc 는 HTTPS 4419 only.

## 옛 — 2026-05-19 L1~L6

⭐ **[project_session_2026_05_19_l1_l6.md](project_session_2026_05_19_l1_l6.md)** — 2026-05-19. L1~L6 순차 완료, 6 commits (bab4173..13a3423). **L1 LIVE 중 hidden 버그 발견**: `_fetch_deployment_for_proxy` 가 `package_name` enrich 안 해서 T1/T2 LIVE 에서 NO-OP였음 (LIVE deployment row 가 package_id 만 보존). **L5 hidden 문제**: UI 의 VolteMsisdn/PttMsisdn 의 service dropdown 이 비어 있었음 (csp_runtime/sip_service file_store 비어있음). GET /csp/services 가 진짜 SoT 인 access_services 컬렉션 읽도록 마이그레이션. 새 admin endpoint: `/csp/sync[/<sid>]` (L2), `/csp/sync/sweep` (L3), `/csp/drift[/resync]` (L4). 새 sweepers: sync_txn timeout 15s, drift 300s. 새 설정 키: `SyncTxnSweepSec` `DriftSweepSec` `AutoResyncDrift`. L6 자동화 스크립트 `scripts/verify_ibcf_outbound_smoke.sh` (cspsim 실행은 sudo netns 필요 → 사용자 LIVE).

⭐ **[project_session_2026_05_18_ha_fanout.md](project_session_2026_05_18_ha_fanout.md)** — 2026-05-18 오후. T1~T5 완료 (commits 9b5699b → 2cfd794 → cc3ed63). Console UI 검증 절차는 여전히 유효 (drift 배너 + A/S system scope 통합 편집).

## 옛 — 2026-05-18 오전 SIP outbound 트랙

⭐ **[project_session_2026_05_18_sip_outbound_addressing.md](project_session_2026_05_18_sip_outbound_addressing.md)** — commit 5개 (3ad3333~092047d). T2~T7 + (D)/(B)/(C) 모두 PASS. LIVE 검증: 5060=NO-OP, 5070=동적 분리 박힘. (G) IBCF 멀티 피어 routes LIVE / (H) AccessService dialog state / (E) IBCF proxy + Record-Route 본격. LIVE 상태: ctrl-a/b csp PID 2270316/2270317 (092047d 빌드).

## 옛 — CSC config-server 트랙 (2026-05-18 오전)

⭐ **[project_session_2026_05_18_csc_config_server.md](project_session_2026_05_18_csc_config_server.md)** — commit `79d7c46` + `66706b8` + `c2c8911`. 본체 + agent fix-up + **Phase X (csp.json bind 5키 제거 — local_nodes single SoT)**. csp 양 멤버 LIVE LISTEN (PID 2063816/2063861, 10.0.1.13:5060). 단계 5.1~5.3 완료, 5.4~5.5 미진행 (sudo + agent restart 필요). **단계 5 호 시험은 T7 LIVE 와 묶어서 진행 가능**.

## 옛 — 트랙 설계 (사용자 주안점 3가지)

⭐ **[project_csc_config_server_track.md](project_csc_config_server_track.md)** — Phase 0 결정 4개 확정 후 본 회기에서 Phase A~F+H smoke 진행. CMP/CSC 자체/cwrtc/phone 의 Phase G 는 별도 회기.

## 2026-05-17 — hands-on walkthrough 회기 (단계 1~4 + 11 patch + 아키텍처 결정)

⭐ **[project_session_2026_05_17_walkthrough.md](project_session_2026_05_17_walkthrough.md)** — 단계 1~4 LIVE 검증 + install-agent.sh DevMode 자동기동 + 권한 격리 (cims-priv) + [적용] polling + tarball bundle + scope 메타. 11 patch 는 commit `87dee0d` 로 들어감. 단계 5 (호 시험) skip → CSC config server 트랙으로 결정.

## ~~옛~~ — 단계 1~5 walkthrough plan (2026-05-17 회기에서 단계 1~4 진행, 단계 5 별도 트랙 결정)

⭐ ~~[project_next_session_e2e_walkthrough.md](project_next_session_e2e_walkthrough.md)~~ — 단계 1~4 까지 진행 결과는 [[project_session_2026_05_17_walkthrough]] 참조

## 2026-05-15 — deployment/ Phase 1~5 + verify + apply + dev-env + IMSI 별칭 + cmp restart 분석

⭐ **[project_session_2026_05_15_deployment_scaffold.md](project_session_2026_05_15_deployment_scaffold.md)** — 오늘 세션 전체

오늘 commits (`db58e71`, `814fe53`, `9c032d1`, `c26c1e9`, `0910c13`, `94ddd3d`, `a625bef`):
- `db58e71` docs(deployment): 환경/시나리오 SoT 디렉토리 scaffold (Phase 1~3) — 7 파일 1166 line
- `814fe53` fix(csp): file fallback 시 service_ref/imsi 미로드 — REGISTER 거부 원인
- `9c032d1` fix(cmp): GROUP_TIMEOUT 자원 leak — root cause `timeoutLoop` (당초 진단 `processAddGroup` 이 아님)
- `c26c1e9` feat(deployment): 모듈 일괄 배포 + 검증 자동화 + sim-a netns 추가 — 3-tier 토폴로지 (AS+AA+SA)
- `0910c13` fix(ha-groups): ha.json services.<group>.port/proto 자동 채우기 — 옵션 (E) sub-issue 해소
- `94ddd3d` feat(deployment): **Phase 4 generator (`bin/render.py`)** — env+scenario→bundle 자동화. LIVE diff 100% 일치
- `a625bef` feat(deployment): **Phase 5 verify + apply + dev-single-host + IMSI 별칭** — 후보 (A2/A3/B/F/C) 일괄
- `c7c6581` feat: **cmp/csp restart fix + prod-multi-host + LIVE 가이드** — is_running exe 검증, cmd_restart kill_stray + sleep 1→3. 3 환경 모두 render 통과
- `c8d9fd9` feat: **verify single-host + apply --restart + volte-only** — verify.py 가 netns 없는 환경도 지원, apply.py 가 CSC API 로 restart 자동 호출, tb 의 PTT 제외 시나리오
- `312c69a` feat: **full(IBCF) + routes/rules schema 정합** — csp 의 진짜 schema (RouteMap pair / RuleEvaluator field-op-value) 와 1:1 매핑. 옛 yaml 의 misnamed entries 정리. **route는 (local,remote) pair SOT — 외부 trunk 용. VoLTE/PTT 내부 호는 routes 없이 동작**
- `ac4cf95` fix: **access_services schema 정합 (allowed_local_node_refs) + cmp 활용 흐름 + dev smoke** — listener_ids → allowed_local_node_refs (csp 진짜 키). **cmp 는 Setup.MediaServer.Host 만 사용, remote_nodes cmp row 는 미래용 SOT** (AddEndpoint 미호출). dev-single-host 의 verify.smoke 활성화
- `53c0bbb` feat: **apply --restart auto + full ACL 데모 + listener_ids deprecation** — CSC API GET 후 (agent_id, package_name) 자동 매핑하여 --restart auto. full.yaml 의 ACL (negate=true 로 untrusted source deny). 옛 키 사용 시 stderr 경고
- `4b77ed9` feat: **CmpClient AddEndpoint LIVE 활성 + check-all + README** — `remote_nodes.tags=["cmp"]` 인 endpoint 들을 CspServer 가 AddEndpoint 호출 → multi-cmp endpoint consistent hash 분배 활성. 1.E-2 stub 종료. check-all.sh (5/5 PASS). README 통합 갱신
- `6ce0d5b` feat: **apply env.kind 분기 + --backup + render --diff** — single-host 도 apply 자동 적용 (build/dist/csp|cmp 직접). 기존 파일 .bak 백업. apply 전 LIVE 와 의미적 diff 미리보기 (CI 친화 rc)
- `8a1aa2e` fix: **local_nodes is_primary single 보장** — UDP 만 primary=true, TCP/TLS=false. LIVE 회기 중 발견된 "multiple is_primary" ERROR 제거. **LIVE 회기 끝까지 성공** — apply 자동, restart auto (job 142~149), AddEndpoint LIVE (primary 10.0.1.21 + add 10.0.1.22 total=2)
- `1155f57` feat: **sync-agent.sh + apply --verify / --restore** — lifecycle.sh + csp/cmp 바이너리 atomic 동기화 (install -m 755). apply 후 verify listen 자동. .bak 일괄 복원. 워크플로 1명령 단축
- `e51c51d` build: **make verify-scenarios + README 워크플로 통합** — CMake target 5/5 PASS. cmp LIVE 진단 (양 노드 LISTEN + freePttResource 동작 = 9c032d1 leak fix LIVE 검증). 1명령 end-to-end: `apply --backup --restart auto --verify`
- `a607ab3` feat: **apply --restart auto status-aware + 옛 shell deprecate** — CSC API status 보고 running→restart, stopped→start 자동 분기. deploy-modules.sh / verify-modules.sh 헤더에 DEPRECATED. csp.log timeout 6건 진단 (대부분 startup race — 무해)
- `daac1c8` feat: **health.sh + verify --json + apply --skip-restart-if-no-change** — sudo 없이 30초 LIVE 진단. verify CI parsable. 변경 0 이면 restart skip (불필요한 cycle 회피)

핵심 산출 + LIVE 결과:
- `deployment/` env/scenario schema + CSP 10 entity layer 모델 (csp-layers.md) + tb-netns-4-node × volte-ptt
- 이슈 #1 (csp VIP bind) ✅ — `config/local_nodes.jsonl` (CSP/config/, **`CSP/csp/config/` 아님**) 에 primary row + `ip_nonlocal_bind=1`
- 이슈 #3 (service_binding 부재) ✅ — `_loadUserFromFile` 에 service_ref/imsi 읽기 추가 + access_services.jsonl + user JSON seed
- fail-over LIVE ✅ — ctrl-a 다운 → ctrl-b 가 VIP 인수 → cspsim 동일 IP 재호 OK. preempt 자동 재인수
- cmp PTT pool leak ✅ — `timeoutLoop` cleanup `freePttResource` 누락 fix. LIVE 8↔10 oscillation 확인
- **3-tier 일괄배포** ✅ — sim-a netns 추가 (10.0.0.31/10.0.1.31/10.0.2.31). 14 데몬 (csp/isp/psp/csc × 2 + cmp/imp/pmp × 2) + cspsim 1. PTT 검증 1대1 호 Setup 101ms
- ~~새 sub-issue (이번 세션 발견)~~: ha.json port/proto 자동 채우기 ✅ `0910c13`. `_render_ha_for_agent` 가 멤버 agent 의 daemon deployment 들 보고 대표 module 의 default port/proto 자동 기재. LIVE 검증 (수동 패치 제거 후 apply → ha.json 재생성 → cims-health rc=0 → VIP 자동 부여). next: dev-single-host env / VoLTE seed 정합 / cmp restart job 분석

## 옛 — 2026-05-14 commit 7개 + LIVE 검증 결과

⭐ **[project_session_2026_05_14_deploy_verify.md](project_session_2026_05_14_deploy_verify.md)** — 자율 배포+검증 (csp on Control / cmp on Media). 통신/인증 layer ✓. **호 시도 실패 = 발견 이슈 3건**:
1. 🔴 csp 가 `Setup.Sip.LocalIp=0.0.0.0` 무시하고 mgmt IP bind → VIP 경유 fail-over 의미 제한
2. 🟡 모듈 default config IP 가 외부 환경 (`192.168.199.129`) 박힘 → 매번 patch 필요
3. 🟢 service_binding 데이터 부재 (NetNS DB 미연결, 외부 DB 위임 결정 영향)

수동 재현 cheat sheet 는 [project_session_2026_05_14_deploy_verify.md](project_session_2026_05_14_deploy_verify.md) §"명령 cheat sheet" 참조.

⭐ **[project_session_2026_05_14_phase1_refactor.md](project_session_2026_05_14_phase1_refactor.md)** — 이전 세션의 A/S 정상 설치 + commit 7개 (a3db6bf~5883d81)

이번 세션 commit (origin/main `63fbe1a`..`5883d81`):
- `a3db6bf` fix(agent): /opt/cims 권한 없을 때 cwd fallback
- `0073e70` fix(ha): VRRP split brain — interface 매칭 IP 를 local_ip/peer_ip 로 사용
- `5ae37c7` feat(agents): enrollment token regenerate API + ha_group cascade + DEV 패키지 자동등록
- `248f3ab` feat(console): HaServices/Services UI 보완 (Standalone, ServiceIp/Vip Panel, IME, 자동등록)
- `de84693` feat(console): AA 모드 그룹에서 VIP chip 숨김
- `c1e499e` feat(ha-ui): ServiceIpRow/VipBinding 상태를 agent.interfaces 매칭으로 동적 표시
- `5883d81` feat(agent+ha-ui): mgmt NIC 변경 차단 + lo 제외 — 백엔드에서 정책 집행

현재 상태 (자율 검증 끝):
- csp@ctrl-a/b (dep 27/28), cmp@media-a/b (dep 19/20) 모두 status=running, LISTEN ✓
- ctrl-a MASTER (VIP 10.0.1.13 보유)
- agent 4 PID (48521 base), keepalived 2 (ctrl-a/b)
- TB-CSC 4419 / Vite 3000 / NetNS 4 ns 정상

## 옛 세션 — 2026-05-13 file-store Phase 1~9 전체 완료 (`5068c08` + docs)

이전 진척 (Phase 1~8, 6 commits):
- `a000c1a` Phase 1 — cims_package
- `1352da3` Phase 2 — cims_agent + cims_instance (heartbeat 핫패스 포함)
- `1283440` Phase 3 — agent_deployment + agent_job + agent_metric (JSONL 시계열)
- `43a1af0` Phase 4 — ha_groups + ha_group_members (멤버 임베드, vrid 자동)
- `66eb57f` Phase 5 — CSP runtime 9 테이블 (listener/trunk/route/access/service/audit). csp_runtime.py 1277 lines + config_cache.py + mcptt.audit_config_change → JSONL.
- `5068c08` Phase 6/7/8 — stats(unused) / recordings(이미 file) / IdMS tokens(idms_storage.py 전면 재작성).
- Phase 9 — **DB 유지 결정 (no-op)** — `organizations` 는 `users.org_id` FK 대상으로 가입자 도메인과 함께 외부 이중화 DB 인계. `csc/src/handlers/org.py` 코드 변경 없음. docs (`runtime_store_design.md` §1/§11 + `db_schema.md` §2) 갱신.

**file-store 마이그레이션 전체 종결.** `{CimsRuntimeDir}/` 13 도메인 + audit JSONL:
agents, instances, packages, deployments, jobs, metrics (시계열), ha_groups, csp_listener, sip_trunk, routing_rule, routing_access_list, sip_service, csp_config_audit (audit JSONL), auth_codes, refresh_tokens

**DB 유지 도메인** (외부 이중화 DB 적재 — 가입자 도메인 + 조직):
users / volte_subscriptions / ptt_subscriptions / user_rejects / ptt_groups / ptt_group_members / ptt_session_seq / **organizations**

**다음 진입 후보**:
| 옵션 | 작업 | 상태 |
|---|---|---|
| (I) 옛 DB 테이블 DROP 마이그레이션 | `migrate_drop_filestored_tables.sql` — Phase 1~8 옛 테이블 `_legacy` rename 또는 DROP | ⚪ 1 릴리스 안정화 후 |
| (J) LIVE 검증 회차 — file_store 전 도메인 회기능 | TB-CSC 재기동 + Console UI 13 도메인 페이지 일괄 확인 | ⚪ 권장 |
| (B) 백로그 1 LIVE 검증 (2-node) | 1.H LIVE / Redis LIVE / CMP All-Active | ⚪ 환경 의존 |
| (D) 차후 트랙 (B1 상용 / WebRTC / cert-rotate) | 메인 백로그 5개 안정화 후 | ⚪ 보류 |

## 옛 — 2026-05-13 file-store Phase 1 완료 (`a000c1a`)

방금 끝낸 세션 (origin/main `7fee5e4`..`a000c1a`, 5 commits):
- `c4e7e5d` feat(alerts) — sweeper open state 복원 + 유형별 통계/일별 차트
- `ad2cd39` docs(db) — 외부 DB 위임 결정 + 테이블 인벤토리 SoT (`db_schema.md`)
- `2ac93c0` refactor(stats) — call_logs DB 의존 제거 → 파일 기반(call.json 스캔)
- `9dfadec` docs — call_logs 표기 정합화 (csc/csp/volte_flows/monitoring)
- `a000c1a` **feat(file-store) — cims_package DB → 파일 기반 (Phase 1)** ⭐

**file-store Phase 1 핵심**:
- 결정: 가입자 외 모든 데이터 파일 기반으로 이전 (외부 DB 인계 부담 최소화)
- SoT: [[../../work/cims/docs/design/runtime_store_design.md]] (9 Phase plan)
- 신규: `csc/src/services/file_store.py` (CRUD + .seq + atomic), 마이그레이션 스크립트
- Phase 1 완료: cims_package 9건 → file_store, agent_deployment JOIN 4건 client-side enrich
- LIVE 검증: TB-CSC 재기동 후 packages/deployments/modules 모두 정상

**다음 진입 후보**:
| 옵션 | 작업 | 상태 |
|---|---|---|
| (G) **file-store Phase 2** — cims_instance + cims_agent | 동일 패턴 적용 | 🟡 우선순위 높음 |
| (H) **file-store Phase 3** — agent_deployment + job + metric | DB 의존도 가장 큰 도메인 | ⚪ Phase 2 다음 |
| (B) 백로그 1 LIVE 검증 — 2-node 셋업 후 | 1.H LIVE / Redis LIVE / CMP All-Active | ⚪ 환경 의존 |

## 이전 — backlog 3 후속 (Alert 보완) + call_logs 정합화

- `c4e7e5d` Alert sweeper 재기동시 open 상태 복원 + 유형별 통계/일별 차트
- `2ac93c0` stats.py — dropped call_logs SELECT 제거 → 파일 기반 (call.json 스캔)
- `9dfadec` 잔존 stale docs 4건 (csc/csp/volte_flows/monitoring) 갱신
- `ad2cd39` 외부 DB 위임 + 테이블 인벤토리 SoT (`docs/design/db_schema.md`)

옛 진입 옵션 (F-2 외부 발송 / E UsersPage orphan / D 차후 트랙) 은 우선순위 낮음.
전 세션 (backlog 3 본체) 상세: [[project_session_2026_05_13_console_mgmt]]

## 백로그 가이드 (사용자 지정 2026-05-12 — 2026-05-13 dev 가능 작업 전부 완료)

5개 메인 트랙. **차후 트랙은 메인 5개 모두 완료 후**. 진입 가이드: **[project_backlog_main_track.md](project_backlog_main_track.md)** ⭐

1. **A/S 이중화 + All Active** — 🟢 **dev 작업 완료**. 1.A~1.H stub + HaServicesPage Phase 1+2 + 1.E-2 + 1.D-2 완료. **잔여**: 1.H LIVE / Redis LIVE / CMP All-Active LIVE / DB 이중화 — 모두 환경/사용자 의존
2. **모듈 설정 UX 개선** — 🟢 **dev 작업 완료 (preset 포함)**. ModuleConfigModal: diff/reset/restart/validation + **preset 셀렉터** (CSP/CMP/CSC 각 3개 시나리오). [[project_session_2026_05_13_preset]]
3. **Console 관리 개선** — 🟢 **dev 작업 완료 (2026-05-13 후속 세션)**. Dashboard 활성통화 ↔ FlowPage drill-down + initiator/callee → ServiceStatus(?q=) link, KPI sparkline (client-side 5분 trend), Alert 이력 (`/dashboard/alerts`, file-based JSONL persistence + CSC sweeper, csp/cmp/db down + rtp_high 자동 감지), MembersPage CSV export. _health 핸들러 active_voip 필드 mismatch (caller→initiator, started_at→invite_time) 동시 픽스. [[project_session_2026_05_13_console_mgmt]]
4. **개발/배포 콘솔 분리** — 🟢 **완료**. `VITE_CONSOLE_TARGET=prod` 빌드에서 패키징 메뉴 숨김 + 라우팅 차단 (Sidebar VISIBLE_SECTIONS, FLAT_ROUTES 필터). TB-Console (dev) 은 전체 노출.
5. **전체 안정화** — 🟡 **LIVE 회차 1회 (S5 pre-existing flaky), backlog 1-4 변경 회기능 무영향 확인**

## 다음 세션 진입 — backlog 1/2/4 dev 작업 종료, 3 (Console 관리) 진입 (2026-05-13 세션 끝)

**이번 세션 완료 작업** (commits 9개, origin/main `b6b74f4`..`710c6c5` + verify 회차):
- 백로그 1: HaServicesPage Phase 1 + Phase 2 (5 sub-items) + 1.E-2 + 1.D-2
- 백로그 2: ModuleConfigModal UX 4건 (diff/reset/restart/validation)
- 백로그 4: VITE_CONSOLE_TARGET prod/dev 분리

**다음 세션 진입 후보** — 사용자 선택:

| 옵션 | 작업 | 상태 |
|---|---|---|
| ~~(A)~~ **백로그 3** — 2026-05-13 후속 세션 완료. [[project_session_2026_05_13_console_mgmt]] | drill-down / sparkline / alert 이력 / CSV export | 🟢 완료 |
| (B) **백로그 1 LIVE 환경 검증** — 2-node 셋업 후 | 1.H LIVE / Redis LIVE / CMP All-Active LIVE | ⚪ 환경 의존 (DB 이중화는 **외부 DB 위임** 결정 2026-05-13 — [[project_db_external]]) |
| (D) **차후 트랙 진입** — 메인 5개 안정화 완료 가정 | B1 상용 환경 / WebRTC / S6-CERT-ROTATE | ⚪ 메인 안정화 후 |

(옛 (C) **백로그 2 preset** — 2026-05-13 추가 세션에서 완료. [[project_session_2026_05_13_preset]])

이번 세션 상세:
- 오전: HaServicesPage Phase 1 wiring → **[project_session_2026_05_13_ha_wiring.md](project_session_2026_05_13_ha_wiring.md)**
- 오후: Phase 2 + 1.E-2 + 1.D-2 + 백로그 2/4 + 안정화 → **[project_backlog_main_track.md §1.0](project_backlog_main_track.md)**

**브라우저 검증 (보조)**:
- `/deploy/services` — 5 standalone agents 표시, A/S/AA 생성, VipPanel/ServiceIpPanel [적용] 동작
- ModuleConfigModal — 변경 시 diff 패널 표시, 필드 옆 ↺ 버튼, "저장 + 재기동" 버튼 (deployment 모드)
- 배포본 console (port 8081) — 패키징 메뉴 사라짐 확인 (TB-Console 3000 은 그대로)

전 세션 (mock UI) 진입 컨텍스트: [[project_session_2026_05_12_ha_services_page]]

이 세션 commit 범위: **origin/main `f18fc97`..`fe5d49b`** (30 commits). 트랙별 세부:
- HA Phase 1.A~H 골격: **[project_ha_design_decisions.md](project_ha_design_decisions.md)** (e35b3a3..b53a347)
- cims.sh 운영 분리: **[project_session_2026_05_12_cims_sh_split.md](project_session_2026_05_12_cims_sh_split.md)** (9867679..bbd277e)
- HA 그룹 데이터 모델 + Console UI: **[project_session_2026_05_12_ha_groups.md](project_session_2026_05_12_ha_groups.md)** (5166ce7..a1f6cd4 + 8d2b260)
- HaServicesPage prototype: **[project_session_2026_05_12_ha_services_page.md](project_session_2026_05_12_ha_services_page.md)** (acbf474..fe5d49b)

LIVE pipeline-full 38 / PASS 34 / FAIL 0 / SKIP 4 / 268~270s. 회기능 무영향.

차후 트랙 (HaServicesPage wiring 완료 후): (1) docs 정합화 (deployment_workflow / VERIFICATION_MANUAL — cims.sh 운영 제거 + 새 HaServices 흐름) (2) **1.E-2 CmpClient endpoint 분배 LIVE 활성** (3) **1.D-2 hiredis 통합** (Redis hot replication LIVE) (4) **HA fail-over LIVE 검증** (2-node 환경 + S6-SCN-FAILOVER-* stub 활성). 메인 백로그 5개: [project_backlog_main_track.md](project_backlog_main_track.md)

## 옛 SoT — 한 줄 인덱스 (자세한 내용은 각 topic 파일)

- 2026-05-13 추가 — 백로그 2 preset (config_template.presets + PresetBar UI, 9 preset) → [project_session_2026_05_13_preset.md](project_session_2026_05_13_preset.md)
- 2026-05-13 오후 — Phase 2 + 1.E-2 + 1.D-2 + 백로그 2/4/5 → [project_session_2026_05_13_phase2_more.md](project_session_2026_05_13_phase2_more.md)
- 2026-05-13 오전 — HaServicesPage Phase 1 wiring → [project_session_2026_05_13_ha_wiring.md](project_session_2026_05_13_ha_wiring.md)
- 2026-05-12 HaServicesPage prototype 8 라운드 mock (`acbf474..fe5d49b`) — [project_session_2026_05_12_ha_services_page.md](project_session_2026_05_12_ha_services_page.md)
- 2026-05-11 TB-agent 완전 제거 (`f18fc97`) — TB 는 이제 TB-CSC + TB-Console 2종만
- 2026-05-11 ISP/IMP 토폴로지 통합 (`68ad390`) — [project_session_2026_05_11_topology.md](project_session_2026_05_11_topology.md)
- 2026-05-11 G10 + IBCF trunk LIVE + 인프라 픽스 3건 (`1159ae7`/`bdca6b5`) — [project_session_2026_05_11_g10.md](project_session_2026_05_11_g10.md)
- 2026-05-11 P2 Layer 3 IBCF routing seed (`54fc82a`) — [project_session_2026_05_11_p2_layer3.md](project_session_2026_05_11_p2_layer3.md)
- 2026-05-10 P2 ISP/IMP Layer 1+2 (`8194160`) — [project_session_2026_05_10_round2_w1w2.md](project_session_2026_05_10_round2_w1w2.md)
- 2026-05-09 docs 현행화 + SSOT 단일화 (`5868984`) — [project_session_2026_05_09_docs.md](project_session_2026_05_09_docs.md)
- 2026-05-08 패키징 메뉴 재정비 (`f4d90ef`) — [project_session_2026_05_08_packaging.md](project_session_2026_05_08_packaging.md)

**docs/ 동기화 가이드** (코드 변경 시 어느 문서를 손볼지):
- URL/메뉴 변경 → `README.md`, `VERIFICATION_MANUAL.md`, `user-manual/deployment_workflow.md`
- 빌드/패키지 변경 → `design/features/build_and_packaging.md` (SSOT)
- 검증 파이프라인 변경 → `VERIFICATION_PROCESS.md` (SSOT)
- 토폴로지 변경 → `VERIFICATION_PROCESS.md §1 S5` (SSOT)
- API 변경 → `api/admin_api.md` / `agent_api.md` / `collection_api.md`
- CSC handler 추가 → `design/modules/csc.md` §2.2 + §9 + `api/admin_api.md`

## 백로그 (사용자 지정 우선순위 2026-05-12) — 상단 가이드 참조

전체 가이드: **[project_backlog_main_track.md](project_backlog_main_track.md)** (각 트랙 sub-item 포함)

요약:
1. **A/S 이중화 + All Active** — 🟡 1.A~1.H 8 phase + HaServicesPage 트랙. **1번 진행 중 추가된 sub-items 는 §1.0 미완 표에 통합 기록됨** (HaServicesPage Phase 2, 1.E-2, 1.D-2, 1.H LIVE, DB 이중화)
2. 모듈 설정 UX 개선
3. Console 관리 개선
4. 개발/배포 콘솔 분리
5. 전체 안정화

### 차후 트랙 (메인 트랙 완료 후)

- B1 — 상용 환경 검증 도구
- WebRTC / cwrtc 검증
- S6-SCN-CERT-ROTATE (mTLS, 환경 의존)

### 옛 백로그 (해결됨, 2026-05-11 라운드)
- ~~`dist/packages/*-stale.tar.gz` 누적~~ → `bdca6b5`: cmd_pkg 끝에 mtime 기준 cleanup
- ~~`dist/csc/packages_tb/` 누적 → csc tarball 폭증~~ → `bdca6b5`: cmd_pkg exclude 추가
- ~~cims.sh stop all 변종 누락~~ → `bdca6b5`: _stop_one all 에 P1/P2 인스턴스 enumeration 추가
- ~~환경 변동성 (LIVE 회차마다 fail 다름)~~ → 위 인프라 픽스 3건이 변동성 원인 모두 해결. 2026-05-11 검증: pipeline-full 3 연속 회차 PASS / 263s 평균, prep-reset 무관.
- ~~ISP/IMP 별도 agent 분리~~ → `68ad390`: volte-sip/media-server 와 공존 (사용자 의도)
- ~~TB-agent stale 누적 / TB-agent 무용 fixture~~ → `f18fc97`: TB-agent 완전 제거 + cims_agent 매 reset 전체 TRUNCATE

## 옛 SoT (참고)

### 2026-05-08 직전 라운드들 (committed)
- `c79cbaf`: 패키지 정체성 분리 + 빌드 페이지 카드 그리드 (LIVE 34/34 PASS / 401초)
- `11a58b0`: 토폴로지 server name 정립
- `390cda1`: P1 — VoLTE/PTT 인스턴스 분리 (PSP/PMP)
- `c700744`: flow_logger CSC raw-data 원칙
- `fc855d2`: 36/36 PASS in 273s, prep-reset, S6 깊이검증 3종, mTLS 토글, 161 unit tests

### 옛 세션
- 2026-05-06: 6단계 파이프라인 본체 + DB→파일 store + history page
- 2026-05-07: 콘솔 빌드/패키지/모듈관리 UI 개편 (14c36c1), S5 native 22 step

자세한 내용은 git log.

## 보류/백로그 (사용자 결정 / 환경 의존)
1. target prod LIVE 검증 — 운영 배포본 셋업 시점
2. WebRTC / cwrtc 검증 항목 — 사용자 별도 요청 예정
3. CSP 정책 seed (acl_policies.jsonl 비어있음) — 외부 peer trust 모델은 G10 으로 임시 충족, AclPolicy 확장은 추후
4. IBCF 모듈 활성 (csp.json Roles.IBCF=false 기본) — ISP variant 가 별도 인스턴스로 운영

## psip 메모 (재진입 참조)
- `ext/psip/SipParser/SipUri.cpp:ParseUser` — Request-URI 의 user 는 **첫 `@`** 로 분리. `sip:a@b@c` 에서 user="a", host="b@c". cspsim 이 `sip:U@DOMAIN@HOST` 형태로 보내면 host 에 `@` 가 포함됨. rule 매칭 시 `req_uri_host` contains 로 우회 가능.

## User
- [user_profile.md](user_profile.md) — Developer working on CIMS (PTT/VoIP server system), Korean-speaking
- [user_credentials.md](user_credentials.md) — sudo 비밀번호 (절대 git/code/docs 에 평문 기록 금지)

## Project (활성)
- [project_session_2026_05_14_netns_ha_resume.md](project_session_2026_05_14_netns_ha_resume.md) — 2026-05-14 진입 즉시 참조. NetNS HA 검증 Step 1~5 매뉴얼
- [project_db_external.md](project_db_external.md) — 2026-05-13 DB 이중화 외부 위임 결정, 우리는 스키마만 유지 (docs/design/db_schema.md SoT)

## Project (참고용 — 옛 SoT)
- [project_verify_v2_plan.md](project_verify_v2_plan.md) — 옛 V2 mock prototype (2026-04-29)
- [project_verify_lib.md](project_verify_lib.md) — 옛 마이그레이션 SoT (Phase 1/2/3)
- [project_phase_status.md](project_phase_status.md) — 이전 SoT
- [project_verification_process.md](project_verification_process.md) — 옛 Phase 1/2/3 요약
- [project_psip_csp_refactor.md](project_psip_csp_refactor.md) — psip + CSP 리팩토링 R1~R8
- [project_cims_overview.md](project_cims_overview.md) — CIMS modular IMS architecture
- [project_deployment_arch.md](project_deployment_arch.md) — 배포 데이터 모델, install_path 규칙
- [project_flow_refactoring_status.md](project_flow_refactoring_status.md) — Flow 리팩토링 (완료)

## Feedback
- [feedback_code_style.md](feedback_code_style.md) — Python ast.parse validation, URL routing conflict
- [feedback_read_docs_first.md](feedback_read_docs_first.md) — 검증/배포 작업 전 docs/ 정독
- [feedback_csc_dist_sync.md](feedback_csc_dist_sync.md) — csc 변경 시 dist sync + restart csc + restart tb-csc 필수 (dev-console 3000 은 TB-CSC 4419 proxy)
- [feedback_ptt_later.md](feedback_ptt_later.md)
