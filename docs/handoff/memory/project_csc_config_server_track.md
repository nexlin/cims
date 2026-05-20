---
name: project-csc-config-server-track
description: 다음 세션 진입점 — 모듈 설정 재설계 + CSC 설정 서버 + A/S 서버별/일괄 구분 (사용자 주안점 2026-05-17)
metadata: 
  node_type: memory
  type: project
  originSessionId: c6ff1f28-72b3-4642-8b21-90ea35b33861
---

# 다음 세션 — 모듈 설정 재설계 + CSC 설정 서버 + A/S 구분 (사용자 주안점)

**Why**: 2026-05-17 walkthrough 회기에서 csp 설정 모델의 fundamental 한계 노출.
walkthrough 단계 5 (호 시험) 가 무리 — csp 설정이 제대로 정리 안 됨. 정공법으로
모듈 설정 모델 재설계 + 중앙 설정 서버 패턴 도입.

**How to apply**: 본 메모리 + [[project_session_2026_05_17_walkthrough]] 읽고
설계 문서부터 시작. 사용자 주안점 3개를 모든 phase 의 가이드로.

## 사용자 주안점 (2026-05-17 회기 끝 정의)

### 1. 모듈 별 설정을 배포 시 & 운용 시를 고려한 재설계

- **배포 시**: agent 가 install 후 처음 모듈 띄울 때 어떻게 설정 받는지
- **운용 시**: 운영자가 console 에서 설정 변경 → 모듈이 어떻게 반영하는지 (재기동 / live reload)
- 두 시점이 일관된 모델로 통합되어야
- **CSP 기준 먼저 정공법 구현** → 그 패턴으로 다른 모듈 (cmp / csc / cwrtc / phone) 점진적 보완

### 2. CSC 또는 별도 설정 서버 추가 — 각 배포 모듈이 설정 서버에서 동기화

- 옵션 a: 기존 CSC 가 설정 서버 역할 겸직 (관리/배포 + 설정)
- 옵션 b: 별도 config server (예: cims-config) 분리 — CSC 는 관리/배포만, config 는 별도 daemon
- 결정 기준: 부하 분리 / 단일 책임 / 가용성 / fail-over 패턴
- 모듈 (csp/cmp 등) 이 부트 시 설정 서버에서 fetch → in-memory + local cache
- 변경 시 push (또는 pull notify) → reload

### 3. A/S 의 경우 서버별 설정 vs 일괄 설정 구분 정의

- 서버별 (system / member-specific): LocalIp, hostname, NIC name, member-role 등
- 일괄 (service / group-common): SIP timeout, role 활성화, access_services, routes, rules 등
- 분류 spec — config_template 의 `scope` 메타 (2026-05-17 walkthrough 에서 1차 도입)
- UI 가 분류 자동 반영 — 서버별은 멤버 카드에서, 일괄은 그룹 카드에서
- placeholder 처리 (예: `{member.svc_ip}`) — 일괄 설정 안에 멤버별 값 자동 치환 패턴 가능

## 진입 phase

| Phase | 작업 | 예상 |
|---|---|---|
| **0 — 진입** | 본 메모리 + walkthrough 메모리 읽기. 사용자와 핵심 설계 결정 5개 확정 | 0.5일 |
| **A — 설계 문서** | `docs/design/csc_config_server.md` — API 스펙, 데이터 모델, 부트스트랩, fallback, 변경 push, member-aware query, 마이그레이션 plan, A/S 일괄/개별 spec | 1일 |
| **B — CSP 설정 모델 통합** | 옛 csp.json + JSONL collections → 단일 spec (config_template 의 unified view). 분류 (system/service) 일관화 | 1일 |
| **C — 설정 서버 endpoint** | CSC (or 별도) 에 GET/PUT/notify endpoint. member-aware query (group_id, member_id, role) | 1일 |
| **D — CSP loader 변경** | 시작 시 설정 서버 fetch + local cache. CSC 다운 시 fallback. SIGUSR1 reload 어댑터 | 1-2일 |
| **E — 변경 push** | CSC → agent → SIGUSR1 패턴 확장 (현 패턴 + config server 통합) | 1일 |
| **F — Console UI** | 그룹/멤버 분리 자연스럽게. 서버별 / 일괄 분류 자동 반영. scope 메타 활용 | 1일 |
| **G — 다른 모듈 점진적 보완** | CMP / CSC / cwrtc / phone — CSP 패턴 복제 | 2-3일 |
| **H — LIVE 검증** | 멤버 정합 자동 확인 + 단계 5 (호 시험) 진행 | 1일 |
| **합계** | | **약 10일~2주** |

## 핵심 설계 결정 (Phase 0 에서 확정 필요)

### 결정 1. 설정 서버 위치 — CSC 겸직 vs 별도 daemon
| | CSC 겸직 | 별도 daemon |
|---|---|---|
| 장점 | 부트스트랩 단순 (이미 CSC URL 알림) | 부하/책임 분리 |
| 단점 | CSC 부하 ↑, 단일 장애점 | 추가 daemon 운영 |
| 권장 | dev 단순화 | prod 운영 |

### 결정 2. 부트스트랩 — 모듈이 설정 서버 URL 어떻게 알리나
- 옵션: agent 가 install 시 모듈에 inject (현재 패턴 확장) / env var / DNS SRV / config file 의 fixed key

### 결정 3. fallback — 설정 서버 다운 시 모듈 동작
- 옵션 a: local cache 로 시작 (변경은 못 받음) — 가장 robust
- 옵션 b: 시작 실패 → agent 재시도 — 단순
- 옵션 c: cache + 백그라운드 retry

### 결정 4. 변경 push — 설정 서버 → 모듈
- 옵션 a: 모듈이 long-poll
- 옵션 b: 설정 서버 → 모듈 HTTP push
- 옵션 c: 설정 서버 → agent → SIGUSR1 → 모듈이 fetch (현 패턴 확장 — 권장)

### 결정 5. A/S 일괄/개별 분류 spec
- config_template 의 `scope: 'system' | 'service'` (이미 도입)
- placeholder `{member.svc_ip}` 같은 자동 치환 패턴 도입 여부
- 부분 override 패턴 — service config 안에 멤버별 override layer

## 영향 (해소되는 옛 task)

이 트랙 완료 시 자연스럽게 해결되는 walkthrough task:
- #11 — HaServicesPage 설정 통합 (그룹 단위 설정 UI 가 본 트랙 일부)
- #14 — update_config 후 SIGUSR1 (변경 push 패턴 통합)
- #15 — GroupServiceConfigModal 활성화 (Phase F 의 UI 결과)

별도 유지:
- #10 — NetNS-aware keepalived (dev infra 트랙, 본 트랙과 무관)
- #12 — install 전 "설정" 버튼 UX gap (작은 보완)

## 진입 cheat sheet

```bash
# 환경 확인 (walkthrough 끝 상태)
ps -ef | grep cims_agent | grep -v grep      # ctrl-a/b agent nex 권한 살아있는지
curl -sk https://192.168.199.129:4419/api/v1/agents | python3 -m json.tool

# 설계 문서 시작
mkdir -p docs/design
$EDITOR docs/design/csc_config_server.md     # Phase A 진입
```

## 관련 메모리

- [[project_session_2026_05_17_walkthrough]] — 이전 회기 결과 + 11 patch + 명령 cheat sheet
- [[project_session_2026_05_15_deployment_scaffold]] — deployment/ Phase 1~5 (배경)
- [[project_db_external]] — DB 외부 위임 결정 (config 와 데이터 분리 컨텍스트)
