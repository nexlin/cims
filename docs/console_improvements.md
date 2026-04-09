# CIMS Console UI 개선 계획

## 현재 페이지 현황

| # | 페이지 | 상태 | 주요 기능 |
|---|--------|------|----------|
| 1 | Dashboard | 정상 | 서비스 상태, KPI, 활성 통화 목록, 5초 자동갱신 |
| 2 | Users (가입자관리) | 정상 | 가입자 CRUD, VoIP/PTT 구독 관리, 인라인 편집 |
| 3 | Groups (그룹관리) | 정상 | PTT 그룹 CRUD, 멤버 우선순위 편집, 분할 레이아웃 |
| 4 | Service Status (서비스상태) | 정상 | 접속자 실시간 현황, 통화중/그룹참여 표시, 5초 갱신 |
| 5 | Call Logs (통화이력) | 정상 | 이력 검색/필터, 참여자 상세, Flow 연동 |
| 6 | Statistics (통계) | 정상 | 메시지/서비스 통계, 시간 단위 차트, 종료사유 분포 |
| 7 | Docs (문서) | 정상 | 마크다운 문서 뷰어, PPTX 다운로드 |
| 8 | FlowPage | 모달 전용 | 시퀀스 다이어그램 (Call Logs에서 호출) |
| 9 | RecordingsPage | **미노출** | 완성된 녹취 UI(333줄)인데 탭에 미등록 |
| 10 | LoginPage | 인증 전용 | 로그인/회원가입/비밀번호 변경 |

---

## 개선 항목

### A. 즉시 적용 (코드 있으나 미연결)

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| A-1 | 녹취 탭 추가 | RecordingsPage가 완성되어 있으나 App.tsx 탭에 미등록 | 매우 낮음 |
| A-2 | Flow에서 녹취 연결 | CallLogsPage 상세에서 "녹취 재생" 버튼 → RecordingsPage 연동 | 낮음 |

### B. 가입자관리 기능 보강

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| B-1 | 가입자/구독 Excel 일괄 등록 | Excel 파일(.xlsx)로 가입자+Call/PTT 번호 다건 import | 중간 |
| B-2 | 가입자/구독 다중 선택 삭제 | 체크박스로 여러 건 선택 후 일괄 삭제 | 낮음 |
| B-3 | 조직 트리 관리 | 트리 구조 조직 추가/편집/삭제, 가입자에 조직 연결 | 높음 |
| B-4 | 조직 Excel 일괄 등록 | 조직 계층 구조를 Excel 파일로 import | 중간 |
| B-5 | 조직별 가입자 필터 | 조직 트리에서 선택 시 해당 조직 가입자만 표시 | 낮음 |

### C. 기존 페이지 기능 보완

| # | 항목 | 대상 페이지 | 설명 |
|---|------|-----------|------|
| C-1 | Dashboard 타이머 설정 표시 | Dashboard | CSP/CMP 타이머 설정값 표시 |
| C-2 | Dashboard 녹취 상태 | Dashboard | 최근 녹취 건수, 변환 대기, 디스크 사용량 |
| C-3 | Call Logs 자동 갱신 | Call Logs | 선택적 자동갱신 토글 |
| C-4 | Call Logs 종료사유 필터 | Call Logs | SIP status, end_reason 별 필터 |
| C-5 | Service Status 그룹별 필터 | Service Status | 특정 그룹만 보기 |
| C-6 | Statistics 날짜 범위 | Statistics | 기간 범위 선택 (현재 단일 날짜) |
| C-7 | Groups 멤버 자동완성 | Groups | 가입자 목록에서 자동완성 |
| C-8 | 검증 결과 뷰어 | 신규 | verification_report.md 조회 |

### D. UX 개선

| # | 항목 | 설명 |
|---|------|------|
| D-1 | CSV/Excel 내보내기 | Call Logs, Statistics, Recordings에 다운로드 버튼 |
| D-2 | 테이블 컬럼 정렬 | 헤더 클릭으로 오름/내림차순 |
| D-3 | 빈 상태 안내 | 데이터 없을 때 가이드 메시지 |
| D-4 | 로딩 스켈레톤 | 데이터 로딩 중 스켈레톤 UI |
| D-5 | 토스트 메시지 개선 | 성공/실패 시 구체적 내용 표시 |

### E. 시스템 운영

| # | 항목 | 설명 |
|---|------|------|
| E-1 | 녹취 디스크 사용량 | 저장 공간 모니터링, 경고 임계값 |
| E-2 | 서비스 시작/정지 | CMP/CSP/CWRTC 재시작 |
| E-3 | 설정 조회 | csp.json/cmp.json 읽기 전용 뷰어 |
| E-4 | 로그 뷰어 | CSP/CMP 로그 tail (WebSocket) |
| E-5 | 감사 로그 | 관리자 CRUD 작업 이력 |

---

## B 항목 상세 설계

### B-1. 가입자/구독 Excel 일괄 등록

#### 기능 설명
관리자가 다수의 가입자와 Call/PTT 번호를 Excel 파일 하나로 일괄 등록한다.

#### Excel 템플릿 형식

**Sheet 1: 가입자 (users)**

| name* | login_id | org_code | details | reject_ids |
|--------|----------|----------|---------|------------|
| 홍길동 | hong | DEV_01 | 개발1팀 | +8210001,+8210002 |
| 김철수 | kim | DEV_02 | 개발2팀 | |

**Sheet 2: VoIP 구독 (voip_subscriptions)**

| name* | msisdn* | auth_id | password | dnd | forward_id |
|--------|---------|---------|----------|-----|------------|
| 홍길동 | +821357007100 | 45003310000100@domain | 123456 | N | |
| 김철수 | +821357007101 | 45003310000101@domain | 123456 | N | +821357007100 |

**Sheet 3: PTT 구독 (ptt_subscriptions)**

| name* | msisdn* | auth_id | password | dnd |
|--------|---------|---------|----------|-----|
| 홍길동 | +82571900100 | | 123456 | N |

- `name`으로 가입자를 매칭 (Sheet 1에 없으면 자동 생성)
- `auth_id` 생략 시 기본 규칙으로 자동 유도
- `password` 생략 시 기본값 "123456"
- `*` 표시: 필수 필드

#### UI 흐름

```
[가입자관리] 화면
  └─ 툴바에 "Excel 가져오기" 버튼 추가
       ├─ 1. 파일 선택 다이얼로그 (.xlsx)
       ├─ 2. 미리보기: 파싱 결과 테이블 (등록/수정/오류 건수 요약)
       │     ├─ 신규 가입자: N건
       │     ├─ 신규 VoIP 구독: N건
       │     ├─ 신규 PTT 구독: N건
       │     └─ 오류 (중복/누락): N건 (빨간색 행 표시)
       ├─ 3. "등록 실행" 버튼 → 서버 일괄 API 호출
       └─ 4. 결과 요약: 성공 N건, 실패 N건 (실패 상세 표시)

  └─ 툴바에 "템플릿 다운로드" 버튼
       └─ 빈 Excel 파일(.xlsx) 다운로드 (시트 구조 + 예시 1행)
```

#### Backend API

```
POST /api/v1/users/import
Content-Type: multipart/form-data
Body: file=<xlsx>

Response:
{
  "total": 50,
  "created_users": 20,
  "created_voip": 30,
  "created_ptt": 25,
  "errors": [
    {"row": 5, "sheet": "voip_subscriptions", "error": "MSISDN 중복: +821357007100"}
  ]
}
```

```
GET /api/v1/users/import/template
→ Excel 템플릿 파일 다운로드
```

---

### B-2. 가입자/구독 다중 선택 삭제

#### 기능 설명
테이블 왼쪽에 체크박스를 추가하여 여러 가입자(또는 구독)를 선택한 후 일괄 삭제한다.

#### UI 흐름

```
[가입자관리] 테이블
  ☐ 전체 선택 (헤더)
  ☑ 홍길동  +821357007100  +82571900100  [편집]
  ☑ 김철수  +821357007101  +82571900101  [편집]
  ☐ 이영희  +821357007102  +82571900102  [편집]

  툴바: [선택 삭제 (2건)] ← 체크 시 활성화, 빨간색 버튼
       └─ 확인 다이얼로그: "2명의 가입자와 관련 구독을 삭제합니다. 되돌릴 수 없습니다."
           └─ [삭제] [취소]
```

#### Backend API

```
DELETE /api/v1/users/batch
Body: { "ids": [1, 5, 12] }

Response:
{ "deleted": 3, "errors": [] }
```

---

### B-3. 조직 트리 관리

#### 기능 설명
트리 구조의 조직을 관리하고, 가입자를 조직에 소속시킨다.

#### DB 스키마 추가

```sql
CREATE TABLE IF NOT EXISTS organizations (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    code        VARCHAR(32) NOT NULL UNIQUE,   -- 조직 코드 (DEV_01)
    name        VARCHAR(128) NOT NULL,          -- 조직명 (개발1팀)
    parent_id   INT DEFAULT NULL,               -- 상위 조직 (NULL=최상위)
    sort_order  INT DEFAULT 0,                  -- 정렬 순서
    created_at  DATETIME DEFAULT NOW(),
    updated_at  DATETIME DEFAULT NOW() ON UPDATE NOW(),
    FOREIGN KEY (parent_id) REFERENCES organizations(id) ON DELETE SET NULL,
    INDEX idx_parent (parent_id),
    INDEX idx_code (code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

`users.org_id` 컬럼을 `organizations.code`와 연결.

#### 조직 트리 예시

```
본부
├─ 개발부
│  ├─ 개발1팀 (DEV_01)
│  ├─ 개발2팀 (DEV_02)
│  └─ QA팀 (QA_01)
├─ 운영부
│  ├─ 인프라팀 (OPS_01)
│  └─ 보안팀 (SEC_01)
└─ 경영지원
   ├─ 인사팀 (HR_01)
   └─ 총무팀 (GA_01)
```

#### UI 레이아웃

```
┌──────────────────────────────────────────────────────┐
│ 조직 관리                              [Excel 가져오기] [+ 조직 추가] │
├──────────────────┬───────────────────────────────────┤
│ 조직 트리         │ 조직 상세 / 소속 가입자             │
│                  │                                   │
│ ▼ 본부            │ 이름: 개발1팀                      │
│   ▼ 개발부        │ 코드: DEV_01                       │
│     ● 개발1팀 ←─│ 상위: 개발부                        │
│     ○ 개발2팀    │                                   │
│     ○ QA팀      │ ─── 소속 가입자 (5명) ───           │
│   ▶ 운영부       │ ☐ 홍길동  +821357007100            │
│   ▶ 경영지원     │ ☐ 김철수  +821357007101            │
│                  │ ☐ 박영수  +821357007102            │
│                  │                                   │
│ [선택 삭제 (0건)] │ [+ 가입자 추가] [선택 삭제 (0건)]    │
└──────────────────┴───────────────────────────────────┘
```

#### 트리 조작

| 동작 | 방법 |
|------|------|
| 노드 선택 | 클릭 → 우측에 상세 표시 |
| 노드 추가 | "+" 버튼 → 이름, 코드, 상위조직 입력 |
| 노드 편집 | 우측 상세에서 직접 수정 → 저장 |
| 노드 삭제 | 다중 선택 후 삭제 (하위 조직은 상위로 이동 또는 함께 삭제 선택) |
| 드래그 이동 | 트리 노드를 다른 부모로 드래그 (정렬 순서 변경) |

#### Backend API

```
GET    /api/v1/organizations              → 전체 트리 (flat list, parent_id로 조립)
POST   /api/v1/organizations              → 조직 생성 {code, name, parent_id}
PUT    /api/v1/organizations/{id}         → 조직 수정
DELETE /api/v1/organizations/{id}         → 조직 삭제
DELETE /api/v1/organizations/batch        → 다중 삭제 {ids: [...]}

POST   /api/v1/organizations/import       → Excel 일괄 등록
GET    /api/v1/organizations/import/template → 템플릿 다운로드

GET    /api/v1/organizations/{id}/users   → 소속 가입자 목록
```

---

### B-4. 조직 Excel 일괄 등록

#### Excel 템플릿 형식

**Sheet 1: 조직 (organizations)**

| code* | name* | parent_code | sort_order |
|--------|-------|-------------|------------|
| HQ | 본부 | | 1 |
| DEV | 개발부 | HQ | 1 |
| DEV_01 | 개발1팀 | DEV | 1 |
| DEV_02 | 개발2팀 | DEV | 2 |
| OPS | 운영부 | HQ | 2 |

- `parent_code`로 상위 조직 지정 (빈 값 = 최상위)
- 행 순서에 관계없이 `parent_code` 참조로 트리 구성
- 기존 `code`와 동일한 행은 수정(upsert) 처리

#### UI 흐름

```
[조직관리] 화면
  └─ "Excel 가져오기" 버튼
       ├─ 1. 파일 선택
       ├─ 2. 미리보기: 트리 형태로 파싱 결과 표시
       │     ├─ 신규: N건 (녹색)
       │     ├─ 수정: N건 (파란색)
       │     └─ 오류: N건 (빨간색, parent_code 미존재 등)
       ├─ 3. "등록 실행"
       └─ 4. 결과 요약
```

---

### B-5. 조직별 가입자 필터

#### 기능 설명
가입자관리 화면 좌측에 조직 트리를 접이식 패널로 표시하고, 조직 선택 시 해당 조직(및 하위) 소속 가입자만 필터링한다.

#### UI 흐름

```
┌──────────────────────────────────────────────────────────────┐
│ 가입자 관리            [Excel 가져오기] [+ 추가] [선택 삭제 (0건)] │
├─────────┬────────────────────────────────────────────────────┤
│ 조직     │ 검색: [____________]  전체 13명                     │
│ (접기 ◁) │                                                    │
│          │ ☐ 이름      조직     Call번호        PTT번호        │
│ ▼ 본부   │ ☑ 홍길동  개발1팀  +821357007100  +82571900100    │
│  ▼ 개발부│ ☑ 김철수  개발1팀  +821357007101  +82571900101    │
│   ●개발1팀│ ☐ 이영희  개발2팀  +821357007102  +82571900102    │
│    개발2팀│                                                    │
│    QA팀  │                                                    │
│  ▶운영부 │                                                    │
│  ▶경영지원│                                                    │
└─────────┴────────────────────────────────────────────────────┘
```

- 조직 트리에서 "개발1팀" 선택 → 테이블이 해당 조직 소속만 표시
- 상위 조직("개발부") 선택 시 하위 전체(개발1팀+개발2팀+QA팀) 가입자 표시
- "전체" 선택 → 필터 해제

---

## 우선순위 로드맵

### Phase 1 — 즉시 (A-1, A-2, B-2)
- 녹취 탭 노출
- Flow↔녹취 연결
- 다중 선택 삭제

### Phase 2 — 단기 (B-1, C-1~C-4)
- Excel 일괄 등록 (가입자/구독)
- Dashboard 보강
- Call Logs 편의성

### Phase 3 — 중기 (B-3, B-4, B-5, C-5~C-8)
- 조직 트리 관리 (DB 스키마 + API + UI)
- 조직 Excel import
- 조직별 필터
- 기타 기능 보완

### Phase 4 — 장기 (D-1~D-5, E-1~E-5)
- UX 개선 (CSV 내보내기, 정렬, 스켈레톤 등)
- 시스템 운영 도구 (로그 뷰어, 감사 로그 등)
