# CIMS Console UI 전면 리팩토링 계획

**작성일:** 2026-04-09
**최종수정:** 2026-04-10
**버전:** 3.0

---

## 1. 개요

### 1.1 목적
Console UI를 좌측 사이드바 메뉴 구조로 전면 개편하고, 가입자관리/서비스이력/통계를 세분화하며, PTT 그룹 확장 필드 및 MCPTT 연동을 반영한다.

### 1.2 진행 상태

| 단계 | 내용 | 상태 |
|------|------|------|
| Phase 1 | DB 스키마 + CSC API (PTT 그룹 확장) | ✅ 완료 |
| Phase 2 | CSP/CMP 로직 반영 | ✅ 완료 |
| Phase 3 | Console UI 리팩토링 | ✅ 기본 완료 (서비스이력/통계 세분화 페이지 미완) |
| Phase 4 | 테스트 + 검증 (92건 PASS) | ✅ 완료 |

### 1.3 변경 범위

| 계층 | 변경 내용 | 상태 |
|------|----------|------|
| **DB 스키마** | ptt_groups 확장 (6개 컬럼), organizations + code_path | ✅ |
| **CSC API** | PTT 그룹 CRUD 확장, GMS XML 반영, 조직 code_path | ✅ |
| **CSP** | CspPttGroup 확장, 세션시간/우선순위 관리 | ✅ |
| **CMP** | 우선순위 기반 floor 제어 (기존 구현 확인) | ✅ |
| **Console UI** | 좌측 사이드바, 서브페이지, 인라인 편집 | ✅ 기본 |
| **테스트** | 확장 필드 검증, MCPTT 시나리오 | ✅ |

---

## 2. DB 스키마 변경

### 2.1 ptt_groups 테이블 확장 ✅

```sql
ALTER TABLE ptt_groups
    ADD COLUMN session_start   DATETIME     DEFAULT NULL,
    ADD COLUMN session_end     DATETIME     DEFAULT NULL,
    ADD COLUMN priority        INT          DEFAULT 5,
    ADD COLUMN encryption      TINYINT(1)   DEFAULT 0,
    ADD COLUMN emergency_call  TINYINT(1)   DEFAULT 0,
    ADD COLUMN org_code        VARCHAR(32)  DEFAULT NULL;
```

### 2.2 organizations 테이블 확장 ✅

```sql
-- 기존 컬럼: id, code, name, parent_id, sort_order
-- 추가된 컬럼:
ALTER TABLE organizations ADD COLUMN code_path VARCHAR(512) DEFAULT '' COMMENT '루트부터 전체 경로';
```

**code_path 예시:**
```
root                    → "root"
root > 개발부           → "root/DEV"
root > 개발부 > 개발1팀 → "root/DEV/DEV_01"
```

하위 조직 검색: `WHERE code_path LIKE 'root/DEV%'` (재귀 불필요)

---

## 3. CSC API 변경 ✅

### 3.1 PTT 그룹 CRUD 확장 ✅

신규 필드 포함: priority, encryption, emergency_call, org_code, session_start, session_end

### 3.2 GMS XML 확장 ✅

우선순위, 암호화, 긴급통화 필드 XML에 포함

### 3.3 조직 API code_path ✅

- 조직 생성 시 `code_path` 자동 계산 (부모 code_path + "/" + code)
- 부모 변경 시 본인 + 모든 하위 조직 `code_path` 재계산
- 목록 `ORDER BY code_path` (트리 순서 정렬)

### 3.4 users API login_id 수정 ✅

`_list_users` SELECT에 `u.login_id` 추가

### 3.5 통계 API 세분화 (미완)

| 엔드포인트 | 데이터 소스 | 상태 |
|-----------|-----------|------|
| `GET /api/v1/stats/service/volte` | service_log/voip | 미완 |
| `GET /api/v1/stats/service/ptt` | service_log/ptt | 미완 |
| `GET /api/v1/stats/messages/sip` | msg_log/csp/sip.jsonl | 미완 |
| `GET /api/v1/stats/messages/cmp` | msg_log/csp/cmp.jsonl | 미완 |
| `GET /api/v1/stats/messages/csc` | msg_log/csp/csc.jsonl | 미완 |
| `GET /api/v1/stats/messages/https` | msg_log/csc/mcptt.jsonl | 미완 |

---

## 4. CSP 변경 ✅

- CspPttGroup 구조체 확장 (6개 필드)
- DbManager::SelectGroup 확장 컬럼 로딩
- GroupCallService::ProcessGroupCall 세션 시간 유효성 검증

---

## 5. Console UI 구조

### 5.1 전체 레이아웃 ✅

```
┌────────────┬──────────────────────────────────────────┐
│ 📡 CIMS    │ [페이지 제목]                             │
│            ├──────────────────────────────────────────┤
│ 대시보드    │                                          │
│            │ 페이지 내용                                │
│ 가입자관리 ▾│                                          │
│  ├ 조직    │                                          │
│  ├ 구성원  │                                          │
│  ├ VoLTE/  │                                          │
│  │ PTT번호 │                                          │
│  └ PTT그룹 │                                          │
│            │                                          │
│ 실시간상태  │                                          │
│            │                                          │
│ 서비스이력 ▾│                                          │
│  ├ VoLTE  │                                          │
│  ├ PTT    │                                          │
│  └ 녹취   │                                          │
│            │                                          │
│ 통계     ▾ │                                          │
│  ├ VoLTE  │                                          │
│  ├ PTT    │                                          │
│  ├ SIP    │                                          │
│  ├ CMP    │                                          │
│  ├ CSC    │                                          │
│  └ HTTPS  │                                          │
│            │                                          │
│ 검증       │                                          │
│ 문서       │                                          │
│            │                                          │
│ 관리자 [🔑]│                                          │
│ [로그아웃]  │                                          │
└────────────┴──────────────────────────────────────────┘
```

### 5.2 페이지별 상세 — 완료된 항목

#### 5.2.1 대시보드 ✅
- 헬스 상태, KPI 카드, CSP 역할 + 타이머 설정 + 녹취 상태
- 활성 VoIP/PTT 테이블, 알람 패널, 5초 자동갱신

#### 5.2.2 가입자관리 — 조직 ✅
- 트리 리스트 형태 + **인라인 편집** (모달 없음)
- 편집 클릭 → 해당 행이 input/select로 변환
- "＋" 클릭 → 해당 행 바로 아래에 추가 행 삽입
- 맨 아래 "＋ 조직 추가" 버튼
- code_path 자동 계산
- Excel 일괄 등록, 다중 선택 삭제

#### 5.2.3 가입자관리 — 구성원 ✅
- 좌측 조직트리(OrgTreePanel) + 우측 구성원 인라인 테이블
- 상위 조직 선택 시 하위 조직 구성원 모두 표시 (code_path startsWith)
- 조직 필드: 조직트리 select 박스
- 인라인 편집/추가/삭제, 다중 선택 삭제

#### 5.2.4 가입자관리 — VoLTE/PTT 번호 (통합) ✅
- **VoLTE와 PTT를 하나의 페이지로 통합**
- 좌측: 조직+구성원 통합 트리 (조직 노드에 인원수 뱃지)
- 우측 상반: VoLTE 번호 카드 리스트
- 우측 하반: PTT 번호 카드 리스트
- 카드: 가로 넓은 그리드 레이아웃, 보기/편집 동일 크기
- 편집 클릭 → 카드 내 input으로 변환 (인라인)
- 맨 아래 "＋ 번호 추가" 점선 카드
- 각 영역 독립 스크롤

```
┌──────────┬───────────────────────────────────────┐
│조직/구성원│ 홍길동 (test001)                       │
│(1/3폭)   │                                       │
│▼CIMS [13]│ ── VoLTE 번호 (1) ──                  │
│ ▼개발[8] │ ┌────────────────────────────────────┐│
│  ●홍길동 │ │ MSISDN +821357007001   DND OFF     ││
│  ○김철수 │ │ Auth   45003310...                  ││
│          │ │ 착신전환 —          [편집][삭제]     ││
│          │ └────────────────────────────────────┘│
│          │ ┌ ─ ＋ VoLTE 번호 추가 ─ ─ ─ ─ ─ ─ ┐│
│          │                                       │
│          │ ── PTT 번호 (1) ──                    │
│          │ ┌────────────────────────────────────┐│
│          │ │ MSISDN +82571900001  DND OFF       ││
│          │ │ ...                  [편집][삭제]    ││
│          │ └────────────────────────────────────┘│
│          │ ┌ ─ ＋ PTT 번호 추가 ─ ─ ─ ─ ─ ─ ─ ┐│
└──────────┴───────────────────────────────────────┘
```

#### 5.2.5 가입자관리 — PTT 그룹 ✅
- 인라인 그룹 목록 + 확장 필드 (우선순위, 암호화, 긴급통화, 조직)
- 우측 멤버 패널 (클릭 시 표시)
- 인라인 편집/추가/삭제

#### 5.2.6 서비스이력 — VoLTE ✅ (기존 CallLogsPage)
- 날짜/유형/종료사유 필터, 자동갱신 토글
- Flow 버튼 → 시퀀스 다이어그램
- 녹취 조회 버튼

#### 5.2.7 서비스이력 — PTT ✅ (기존 CallLogsPage 공유)
- PTT call_type 필터

#### 5.2.8 서비스이력 — 녹취 ✅
- RecordingsPage 기존 구현

### 5.3 페이지별 상세 — 미완료 항목

#### 실시간 서비스 상태 (개선 필요)
현재 기존 ServiceStatusPage. 개선 계획:
- 좌측 조직트리로 필터
- 구성원별 VoLTE/PTT 등록/통화/그룹참여 상태 통합 표시

#### 서비스이력 — VoLTE/PTT 분리 (개선 필요)
현재 CallLogsPage 하나로 공유. 개선 계획:
- VoLTE 전용: 유형(등록/통화/기타), 발신, 착신, 종료사유
- PTT 전용: 그룹명, 참여멤버수, 세션시간, 잔여시간

#### 통계 6개 서브페이지 (미완)
현재 StatsPage 하나로 공유. 개선 계획:
- VoLTE 서비스: 호 시도/성공률/평균통화시간/종료사유 분포
- PTT 서비스: 그룹콜 횟수/세션시간/참여멤버수/그룹별 Top10
- SIP: 메서드별 카운트, 응답코드별 카운트
- CMP: 명령별 카운트
- CSC: user_change/group_change/stats 카운트
- HTTPS: IdMS/GMS/CMS/KMS 메서드별 카운트

#### 검증 페이지 (미완)
- 검증 실행 버튼 → run_all.py 실행
- 실시간 진행률
- 결과 테이블 (모듈별 PASS/FAIL)
- 과거 검증 이력

---

## 6. 구현 파일 목록

### DB
| 파일 | 변경 | 상태 |
|------|------|------|
| `sql/migrate_ptt_groups_v2.sql` | ptt_groups 확장 | ✅ |
| `sql/migrate_organizations.sql` | organizations + code_path | ✅ |

### CSC
| 파일 | 변경 | 상태 |
|------|------|------|
| `csc/bin/csc_pihttp/src/cims_admin.py` | PTT 그룹 CRUD 확장, users login_id | ✅ |
| `csc/bin/csc_pihttp/src/csc_service.py` | GMS XML 확장, MCPTT 로깅 | ✅ |
| `csc/bin/csc_pihttp/src/cims_org.py` | 조직 CRUD + code_path 자동계산 | ✅ |
| `csc/bin/csc_pihttp/src/csc_flow.py` | 서비스이력/Flow/녹취 파일시스템 API | ✅ |
| `csc/bin/csc_pihttp/src/csc_logger.py` | CSC 서비스로그 + 메시지로그 | ✅ |
| `csc/bin/csc_pihttp/src/cims_stats.py` | 통계 API 세분화 | 미완 |

### CSP
| 파일 | 변경 | 상태 |
|------|------|------|
| `csp/CspPttGroup.h` | 구조체 확장 (6개 필드) | ✅ |
| `csp/DbManager.cpp` | SelectGroup 확장 컬럼 | ✅ |
| `csp/GroupCallService.cpp` | 세션시간/우선순위 체크 | ✅ |
| `csp/CallDir.h` | 서비스로그 통합 디렉터리 | ✅ |
| `csp/MsgLogger.cpp` | 메시지 통계 로그 | ✅ |

### Console
| 파일 | 변경 | 상태 |
|------|------|------|
| `App.tsx` | 좌측 사이드바 + 라우팅 | ✅ |
| `components/Sidebar.tsx` | 사이드바 컴포넌트 | ✅ |
| `components/OrgTreePanel.tsx` | 공유 조직트리 패널 | ✅ |
| `pages/OrganizationsPage.tsx` | 인라인 편집 트리 리스트 | ✅ |
| `pages/MembersPage.tsx` | 조직트리 + 구성원 인라인 | ✅ |
| `pages/SubscriptionsPage.tsx` | VoLTE/PTT 통합, 카드 UI | ✅ |
| `pages/PttGroupsPage.tsx` | 인라인 편집 + 확장 필드 | ✅ |
| `pages/DashboardPage.tsx` | 타이머/녹취 상태 추가 | ✅ |
| `pages/CallLogsPage.tsx` | 자동갱신/종료사유 필터 | ✅ |
| 서비스이력 VoLTE/PTT 분리 페이지 | 신규 | 미완 |
| 통계 6개 서브페이지 | 신규 | 미완 |
| 검증 페이지 | 신규 | 미완 |

---

## 7. 남은 작업

### 우선순위 1: 서비스이력 분리
- VoLTE 전용 이력 페이지 (VoLTE 필드 중심)
- PTT 전용 이력 페이지 (그룹/세션 필드 중심)

### 우선순위 2: 통계 세분화
- CSC API: msg_log JSONL 기반 통계 엔드포인트 6개
- Console: 6개 서브페이지 (차트 + KPI)

### 우선순위 3: 검증 페이지
- 백엔드: run_all.py 실행 API
- Console: 검증 실행/결과/이력 페이지

### 우선순위 4: 실시간 상태 개선
- 조직트리 필터 + 구성원별 VoLTE/PTT 통합 상태 표시
