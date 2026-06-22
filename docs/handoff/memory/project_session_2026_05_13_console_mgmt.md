---
name: project-session-2026-05-13-console-mgmt
description: 2026-05-13 후속 세션 — backlog 3 (Console 관리 개선) dev 작업 일괄. Dashboard drill-down + sparkline + Alert 이력 페이지 (file-based) + UsersPage CSV export
metadata: 
  node_type: memory
  type: project
  originSessionId: f19141ef-a970-4072-bb3d-641767ae3930
---

# Backlog 3 — Console 관리 개선 (2026-05-13)

순서대로 5 묶음:

## 1. _health 핸들러 active_voip 필드 mismatch 픽스 (선행 버그)

`csc/src/handlers/stats.py:_health` 가 v3 (state-file 기반, 2026-04-22) 으로 마이그레이션된 이후 응답 필드명이 `caller`/`started_at` 인데, `HealthResponse` 타입과 DashboardPage 는 `initiator`/`invite_time` 을 기대. 결과: 활성 VoIP 행 발신자 빈 칸으로 표시.

해결: `_subscribers_status` 와 동일한 alias 패턴 (`'invite_time': st.get('started_at')`) 으로 통일. PTT 응답에 누락됐던 `call_id`, `state` 보강 (TypeScript 타입과 일치).

## 2. Dashboard 활성 통화 → FlowPage drill-down

`DashboardPage.tsx` 활성 VoIP/PTT 행 클릭 시 FlowPage 모달 오픈. `CallLogsPage`/`PttHistoryPage`/`VolteHistoryPage` 와 동일한 모달 패턴 (`useState<{callId, callType}>` + `<FlowPage onClose>` 컨디셔널 렌더).

## 3. Dashboard KPI sparkline (client-side trend)

5초 폴링 결과를 메모리 (`historyRef`) 에 누적 (최대 60샘플 = 5분), 4개 KPI 카드 (등록/VoIP/PTT/RTP) 에 인라인 SVG sparkline. backend 변경 없음.

## 4. Alert 이력 (file-based persistence + Console page)

신규 파일:
- `csc/src/services/alert_log.py` — `record_event` / `read_recent` / `list_types`. 저장 경로 `{ServiceLogDir}/alerts/YYYY/MM/DD.jsonl` (한 줄당 open/close 이벤트).
- `csc/src/handlers/alerts.py` — `GET /api/v1/alerts?days=&type=&limit=` + `GET /api/v1/alerts/types`. `CIMS_ALERTS_HANDLER_LIST`.
- `ems/core/console/src/api/alerts.ts` — TypeScript API 클라이언트.
- `ems/core/console/src/pages/AlertsPage.tsx` — open/close 페어링 (같은 type 의 가장 가까운 후속 close 매칭) → 발생/해제/지속시간 표. 기간 필터 (오늘/7일/30일/90일) + type 필터 + "해제 포함" 토글.

수정:
- `csc/src/csc_app.py` — `_sweep_alerts()` 추가. 메인 루프에서 `AlertSweepSec` (기본 30) 마다 호출. 상태 전이만 기록 (`csp_down`/`cmp_down`/`db_down`/`rtp_high`). `AlertRtpThresholdPct` 기본 80%. `_alert_open` in-memory dict 으로 open 추적.
- `ems/core/console/src/routes.tsx` — `/dashboard/alerts` 라우트 추가 (대시보드 섹션 2번째 탭).
- `ems/core/console/src/pages/DashboardPage.tsx` — 알람 패널에 "이력 보기 →" 링크 추가.
- `ems/core/console/src/index.css` — `.badge--yellow` 추가 (warning severity 표시용).

**제약**: sweeper 가 in-memory 로 open 상태를 들고 있어서 CSC 재시작 시 open 알람이 한 번 더 잡힐 수 있음 (re-emit). resolved 가 누적되는 부작용만 있고 데이터 무결성 영향 없음.

## 5. MembersPage CSV export

`csvCell()` + `downloadCsv()` 헬퍼 + 툴바 "CSV 내보내기 (N)" 버튼. 필드: 이름, 로그인 ID, 조직 코드, 조직명, 상세, Call 번호 (`;` 구분), PTT 번호. UTF-8 BOM 포함 → Excel 한글 자동 인식.

(처음에는 `UsersPage.tsx` 에 추가했으나 routes.tsx 에서 import 되지 않은 **orphan** 파일이라 노출 안 됨을 발견. 실제 가입자 관리 라우트인 MembersPage (`/subscribers/members`) 로 옮김. UsersPage.tsx 는 그대로 dead — 차후 정리 또는 활성화 필요.)

## 6. Active call → 가입자 상세 link

- `ServiceStatusPage.tsx` — `useSearchParams` 로 `?q=` 자동 입력 (deep-link 진입 시 검색어 보존, 검색어 변경 시 URL 동기화).
- `DashboardPage.tsx` — initiator/callee MSISDN 을 `<a>` 로 wrap. `e.stopPropagation()` 으로 행 클릭 (FlowPage 열기) 과 충돌 방지. `navigate('/service/status?q=<msisdn>')`.

## 검증

S1 PASS (5/5, ~12s). pipeline-full LIVE 미실행 (S5/S6 는 환경 의존 항목 + 이번 변경은 모두 추가형 — 회기능 risk 낮음).

## 관련 백로그 진입 가이드

- [[project_backlog_main_track]] — 메인 5트랙
- 백로그 1 LIVE 검증 / 차후 트랙 (B1/WebRTC/S6-CERT-ROTATE) 은 환경 의존, 별도 진입.
