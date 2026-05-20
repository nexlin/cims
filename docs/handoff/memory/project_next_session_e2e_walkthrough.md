---
name: project-next-session-e2e-walkthrough
description: "다음 세션 — 사용자 수동 E2E walkthrough 5단계 계획 (build→infra→register→install→verify). 각 단계 진입점, 검증 포인트, 알려진 우려 사항."
metadata: 
  node_type: memory
  type: project
  originSessionId: a4ed9ab8-8cbe-4284-9494-7be286625d35
---

# 다음 세션 — 사용자 수동 E2E walkthrough 5단계

**목표**: 사용자가 console + 터미널로 5단계를 직접 수행, 이번까지 보완한 내용이 LIVE 에서 회기능 동작하는지 검증. 이슈 발생 시 그 자리에서 보완.

**Why**: 자율 배포/검증은 모두 끝났지만 사용자 관점의 UX flow 회기능은 미검증. 콘솔/터미널 UX gap 발견용.

**How to apply**: 단계마다 진입점(URL/command)·검증 포인트·우려 사항을 미리 준비해 두고, 사용자가 막히면 즉시 진단/패치.

---

## 단계 1 — 빌드 > 패키지 다운로드 (console)

**진입점**: Console `/packages` (PackagesPage)

**선행 조건**: TB-CSC 4419 + Vite 3000 기동, dev 모드 console (`VITE_CONSOLE_TARGET=dev`)

**예상 흐름**:
1. 패키징 메뉴에서 csp/cmp/csc/agent 5종 빌드 트리거
2. manifest.json (SHA-256) + tarball 5개 생성 확인
3. 다운로드 버튼으로 tarball 받기

**검증 포인트**:
- 빌드 산출물이 `build/dist/` 에 정상 생성되는지
- manifest 의 SHA-256 매칭
- prod 모드 (`VITE_CONSOLE_TARGET=prod`) 에서는 패키징 메뉴 안 보이는지 (별도 확인)

**알려진 우려**:
- 빌드 시간 길어서 timeout — TB-CSC 진행률 polling 동작 확인 필요
- ext/ 외부 라이브러리 첫 빌드 시 대용량 다운로드

---

## 단계 2 — 인프라 환경 구성 (서버/IP/HA) (console + 터미널)

**진입점**:
- Console `/servers` (서버 목록 + agent enrollment token)
- Console `/deploy/services` (HaServicesPage — Standalone/AS/AA)
- 터미널: agent 설치 스크립트 (각 호스트에서)

**예상 흐름**:
1. 콘솔에서 호스트 등록 + enrollment token 발급
2. 각 호스트 터미널에서 `agent` 바이너리 + 토큰으로 install
3. agent 가 콘솔에 등록되어 status=online 확인
4. HA 그룹 생성 (A/S 또는 AA), 멤버 agent 지정
5. VIP/ServiceIp 설정 + `[적용]` 클릭 → keepalived 자동 기동 + VIP 부여

**검증 포인트**:
- enrollment token regenerate 동작 (`5ae37c7`)
- ha_group cascade 멤버 등록 (`5ae37c7`)
- mgmt NIC 변경 차단 + lo 제외 (`5883d81`)
- AA 그룹은 VIP chip 숨김 (`de84693`)
- ServiceIpRow/VipBinding 이 agent.interfaces 매칭으로 동적 표시 (`c1e499e`)
- VRRP split brain 픽스 — local_ip/peer_ip 가 interface 매칭 IP (`0073e70`)
- ha.json 의 port/proto 자동 채우기 (`0910c13`) — 수동 패치 없이 cims-health rc=0

**알려진 우려**:
- agent /opt/cims 권한 — fallback 동작 (`a3db6bf`)
- 단일 호스트 시나리오 (dev-single-host) 에서 HA 모드 어떻게 표현되는지

---

## 단계 3 — 패키지 등록 + 시스템별 패키지 추가 (console)

**진입점**: Console `/packages` (등록) + `/deploy/services` 의 모듈별 패키지 지정

**예상 흐름**:
1. 단계 1 에서 빌드한 tarball 을 패키지로 등록 (file_store: `cims_package` 9건 형식)
2. 각 서비스(csp/cmp/csc) 에 패키지 버전 지정
3. agent_deployment 가 (agent_id, package_name) 매핑으로 생성 — file_store JOIN 4건 client-side enrich

**검증 포인트**:
- DEV 패키지 자동 등록 (`5ae37c7`)
- 파일 기반 `cims_package` / `agent_deployment` 정상 동작 (file-store Phase 1~3)
- multi-host 환경에서 동일 패키지를 양 노드에 모두 배포 가능한지

**알려진 우려**:
- 패키지 + agent 조합 표시 UI — 버전 충돌/호환성 검증 없을 가능성

---

## 단계 4 — 패키지 설치 > 설정 > 기동 (console)

**진입점**: Console `/deploy/services` 의 [배포] / [설정] / [재기동] 버튼

**예상 흐름**:
1. agent 가 패키지를 다운로드 + 설치 (`/opt/cims/<module>/`)
2. 모듈별 ModuleConfigModal 에서 설정 — preset 선택 가능 (CSP/CMP/CSC × 3개)
3. diff 패널 → 변경 확인 → 저장 + 재기동 한 번에
4. `apply --backup --restart auto --verify` 가 1명령 1cycle 로 동작

**검증 포인트**:
- ModuleConfigModal: diff/reset/restart/validation + preset 셀렉터
- `apply --restart auto` status-aware 분기 (`a607ab3`): running→restart, stopped→start
- `apply --skip-restart-if-no-change` (`daac1c8`): 변경 0이면 cycle 회피
- `is_running` exe 검증 + kill_stray + sleep 3 (`c7c6581`)
- `sync-agent.sh` lifecycle.sh + csp/cmp 바이너리 atomic 동기화 (`1155f57`)
- `--backup` 으로 .bak 생성 (`6ce0d5b`)
- `--restore` 로 .bak 복원 (`1155f57`)
- `health.sh` (sudo 없이 30초 LIVE 진단), `verify --json` CI parsable (`daac1c8`)

**알려진 우려**:
- csp.log timeout 6건 (대부분 startup race — 무해, `a607ab3` 진단 결과)
- 단계 2 의 HA VIP 와 단계 4 의 csp `Setup.Sip.LocalIp` 의 정합 — `local_nodes.jsonl` 의 primary row 가 VIP IP 인지

---

## 단계 5 — 서비스 검증 (VoLTE / PTT 호 시험) (console + 터미널)

**진입점**:
- Console `/verify-v2` (VerificationV2Page) — S1~S6 파이프라인 UI
- Console `/dashboard` — 활성통화 표시 + FlowPage drill-down
- 터미널: `cspsim` 직접 호출 (수동 테스트)

**예상 흐름**:
1. Console `/verify-v2` 에서 S6 (통합 검증) 실행 — VoLTE/PTT 음성·영상 시나리오
2. 터미널에서 cspsim 2~4 세션으로 수동 1대1 / 그룹콜
3. Dashboard 활성통화 표시 → 클릭 → FlowPage 로 drill-down
4. initiator/callee 링크 → ServiceStatus(?q=) 로 이동
5. PTT 검증: 1대1 호 Setup 101ms 수준 재현

**검증 포인트**:
- S6 통합 검증 PASS
- `make verify-scenarios` CMake target 5/5 PASS
- `check-all.sh` 5/5 PASS
- VoLTE B2BUA: A → INVITE → CSP → CMP relay → B
- PTT 그룹콜: addGroup → joinGroup (floor_port + user_floor_port)
- cmp PTT pool leak fix LIVE 검증 (`9c032d1`) — 양 노드 LISTEN + freePttResource 동작
- multi-cmp endpoint 분배 LIVE (`4b77ed9`) — primary 10.0.1.21 + add 10.0.1.22 total=2
- fail-over: ctrl-a 다운 → ctrl-b VIP 인수 → cspsim 동일 IP 재호 OK + preempt 자동 재인수

**알려진 우려**:
- routes 는 외부 trunk 용 — VoLTE/PTT 내부 호는 routes 없이 동작 (`312c69a`). 만약 호 실패하면 routes 가 아닌 access_services/local_nodes/service_binding 부터 확인
- `_loadUserFromFile` 의 service_ref/imsi 누락 픽스 (`814fe53`) — REGISTER 단계에서 거부되지 않는지
- 단계 2 의 HA fail-over 와 단계 5 의 호 시험 조합 — 호 중 fail-over 시 미디어 어떻게 되는지

---

## 단계별 fallback / 빠른 진단 cheat sheet

| 증상 | 1차 확인 |
|---|---|
| 콘솔 페이지 빈 화면 | TB-CSC 4419 + Vite 3000 기동 여부 |
| agent 등록 안 됨 | enrollment token 만료 / mgmt NIC 변경 시도 |
| VIP 안 잡힘 | `ha.json` port/proto 자동 채워졌는지, keepalived 로그 |
| 패키지 설치 실패 | `/opt/cims` 권한 (agent cwd fallback) |
| 재기동 실패 | `is_running` exe 검증 (`c7c6581`), `cmd_restart` kill_stray |
| 호 시도 실패 (REGISTER) | `service_ref/imsi` 로드 (`814fe53`), access_services |
| 호 시도 실패 (INVITE) | local_nodes.is_primary single (`8a1aa2e`), Setup.Sip.LocalIp |
| PTT 그룹콜 자원 leak | `freePttResource` (timeoutLoop) — `9c032d1` |
| multi-cmp 분배 안 됨 | `remote_nodes.tags=["cmp"]` + AddEndpoint LIVE (`4b77ed9`) |

## 관련 메모리

- [[project_session_2026_05_15_deployment_scaffold]] — 이번까지 모든 보완 commit 상세
- [[project_session_2026_05_14_deploy_verify]] — 이전 자율 배포 + 발견된 이슈 3건
- [[project_backlog_main_track]] — 메인 백로그 5트랙
