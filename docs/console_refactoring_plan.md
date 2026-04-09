# CIMS Console UI 전면 리팩토링 계획

**작성일:** 2026-04-09
**버전:** 2.0

---

## 1. 개요

### 1.1 목적
Console UI를 좌측 사이드바 메뉴 구조로 전면 개편하고, 가입자관리/서비스이력/통계를 세분화하며, PTT 그룹 확장 필드 및 MCPTT 연동을 반영한다.

### 1.2 변경 범위

| 계층 | 변경 내용 |
|------|----------|
| **DB 스키마** | ptt_groups 확장 (6개 컬럼), organizations 활용 |
| **CSC API** | PTT 그룹 CRUD 확장, GMS XML 반영, 통계 API 세분화 |
| **CSP** | CspPttGroup 확장, 세션시간/우선순위 관리 |
| **CMP** | 우선순위 기반 floor 제어 |
| **Console UI** | 좌측 사이드바, 16개 서브페이지, 조직트리 연동 |
| **테스트** | 확장 필드 검증, MCPTT 시나리오 |

---

## 2. DB 스키마 변경

### 2.1 ptt_groups 테이블 확장

```sql
ALTER TABLE ptt_groups
    ADD COLUMN session_start   DATETIME     DEFAULT NULL  COMMENT '그룹 세션 시작시간',
    ADD COLUMN session_end     DATETIME     DEFAULT NULL  COMMENT '그룹 세션 종료시간 (NULL=무기한)',
    ADD COLUMN priority        INT          DEFAULT 5     COMMENT '그룹 우선순위 (1=최고, 10=최저)',
    ADD COLUMN encryption      TINYINT(1)   DEFAULT 0     COMMENT '암호화 여부 (SRTP/MIKEY-SAKKE)',
    ADD COLUMN emergency_call  TINYINT(1)   DEFAULT 0     COMMENT '긴급통화 허용 여부',
    ADD COLUMN org_code        VARCHAR(32)  DEFAULT NULL  COMMENT '소속 조직 코드',
    ADD INDEX idx_org_code (org_code);
```

### 2.2 organizations 테이블 (기존)

이미 생성됨. 변경 없음.

```
organizations: id, code, name, parent_id, sort_order
```

### 2.3 users 테이블

기존 `org_id` 컬럼을 `organizations.code`와 연결. 변경 없음.

---

## 3. CSC API 변경

### 3.1 PTT 그룹 CRUD 확장

**기존 API에 신규 필드 추가:**

```
POST /api/v1/ptt/groups
{
    "id": "+82571910003",
    "name": "긴급대응팀",
    "priority": 1,
    "encryption": true,
    "emergency_call": true,
    "org_code": "OPS_01",
    "session_start": "2026-04-09T09:00:00",
    "session_end": "2026-04-09T18:00:00",
    "members": [
        {"user_id": "+82571900001", "priority": 1},
        {"user_id": "+82571900002", "priority": 2}
    ]
}
```

**GET 응답에도 신규 필드 포함:**

```json
{
    "id": "+82571910003",
    "name": "긴급대응팀",
    "priority": 1,
    "encryption": true,
    "emergency_call": true,
    "org_code": "OPS_01",
    "session_start": "2026-04-09T09:00:00",
    "session_end": "2026-04-09T18:00:00",
    "members": [...]
}
```

### 3.2 GMS XML 확장

GMS 응답 XML에 신규 필드 반영:

```xml
<group xmlns="urn:oma:xml:poc:list-service">
  <list-service uri="tel:+82571910003">
    <display-name>긴급대응팀</display-name>
    <priority>1</priority>
    <encryption>true</encryption>
    <emergency-call>true</emergency-call>
    <org-code>OPS_01</org-code>
    <session-start>2026-04-09T09:00:00</session-start>
    <session-end>2026-04-09T18:00:00</session-end>
    <list>
      <entry uri="tel:+82571900001">
        <user-priority>1</user-priority>
      </entry>
    </list>
  </list-service>
</group>
```

### 3.3 통계 API 세분화

| 엔드포인트 | 데이터 소스 | 지표 |
|-----------|-----------|------|
| `GET /api/v1/stats/service/volte` | service_log/voip | 호 시도/성공/실패, 성공률, 평균통화시간, 최대동시호, 종료사유 분포 |
| `GET /api/v1/stats/service/ptt` | service_log/ptt | 그룹콜 횟수, 평균세션시간, 참여멤버수, 발언횟수, 그룹별 Top10 |
| `GET /api/v1/stats/messages/sip` | msg_log/csp/sip.jsonl | 메서드별 카운트 (REGISTER, INVITE, BYE, 1xx~6xx) |
| `GET /api/v1/stats/messages/cmp` | msg_log/csp/cmp.jsonl | 명령별 카운트 (add, remove, addgroup, joingroup 등) |
| `GET /api/v1/stats/messages/csc` | msg_log/csp/csc.jsonl | user_change, group_change, stats 카운트 |
| `GET /api/v1/stats/messages/https` | msg_log/csc/mcptt.jsonl | IdMS/GMS/CMS/KMS 메서드별 카운트 |

**공통 파라미터:** `date`, `hour`, `granularity` (5m, 10m, 1h, 1d, 1M, 1y)

---

## 4. CSP 변경

### 4.1 CspPttGroup 구조체 확장

```cpp
// csp/CspPttGroup.h
class CspPttGroup {
public:
    std::string _id;              // 그룹 ID
    std::string _name;            // 그룹명
    int         _priority;        // 그룹 우선순위 (1=최고)
    bool        _encryption;      // 암호화 여부
    bool        _emergencyCall;   // 긴급통화 허용
    std::string _orgCode;         // 소속 조직
    time_t      _sessionStart;    // 세션 시작시간 (0=즉시)
    time_t      _sessionEnd;      // 세션 종료시간 (0=무기한)
    bool        _videoEnabled;    // 영상 활성화
    std::vector<std::shared_ptr<CspPttUser>> _pusers;
};
```

### 4.2 GroupMap DB 로딩 확장

```sql
SELECT id, name, priority, encryption, emergency_call, org_code,
       session_start, session_end, video_enabled
FROM ptt_groups
```

### 4.3 GroupCallService 세션시간 관리

- `CheckGroupIntegrity()`에서 `session_start`/`session_end` 확인
  - 현재시간 < session_start → 그룹 비활성 (INVITE 거부)
  - 현재시간 > session_end → 그룹 세션 종료 (자동 BYE)
- `ProcessGroupCall()`에서 우선순위 확인
  - 긴급그룹(priority ≤ 2) → 일반 그룹콜 선점 가능

### 4.4 SDP/XML 암호화 지원

- `encryption = true` → SDP에 SRTP 속성 추가 (crypto 라인)
- INVITE의 multipart XML에 `<encryption>true</encryption>` 포함

---

## 5. CMP 변경

### 5.1 우선순위 기반 floor 제어

- `addgroup` 명령에 멤버 우선순위 전달 (기존 구현)
- Floor request 시 우선순위가 높은 멤버가 선점 (기존 구현 확인)
- 긴급그룹 → floor grant 즉시 (대기 없음)

---

## 6. Console UI 구조

### 6.1 전체 레이아웃

```
┌──────────────┬──────────────────────────────────────────┐
│  📡 CIMS     │  [페이지 제목]                [사용자 ▼] │
│              ├──────────────────────────────────────────┤
│  대시보드     │                                          │
│              │  페이지 내용                               │
│  가입자관리 ▾ │                                          │
│   ├ 조직     │                                          │
│   ├ 구성원   │                                          │
│   ├ VoLTE   │                                          │
│   ├ PTT     │                                          │
│   └ PTT그룹  │                                          │
│              │                                          │
│  실시간상태   │                                          │
│              │                                          │
│  서비스이력 ▾ │                                          │
│   ├ VoLTE   │                                          │
│   └ PTT     │                                          │
│              │                                          │
│  통계      ▾ │                                          │
│   ├ VoLTE   │                                          │
│   ├ PTT     │                                          │
│   ├ SIP     │                                          │
│   ├ CMP     │                                          │
│   ├ CSC     │                                          │
│   └ HTTPS   │                                          │
│              │                                          │
│  검증        │                                          │
│  문서        │                                          │
│              │                                          │
│  ──────────  │                                          │
│  관리자 [🔑] │                                          │
│  [로그아웃]   │                                          │
└──────────────┴──────────────────────────────────────────┘
```

### 6.2 페이지별 상세

#### 6.2.1 대시보드
기존 DashboardPage 유지 + 보강:
- 헬스 상태 (CSP/CMP/DB)
- KPI 카드 (등록사용자, 활성호, PTT세션, RTP포트)
- CSP 역할 상태 + 타이머 설정 + 녹취 상태
- 활성 VoIP/PTT 테이블
- 알람 패널

#### 6.2.2 가입자관리 — 조직
기존 OrganizationsPage:
- 좌측 조직트리 (추가/편집/삭제/드래그)
- 우측 조직 상세 + 소속 가입자
- Excel 일괄 등록/템플릿 다운로드
- 다중 선택 삭제

#### 6.2.3 가입자관리 — 구성원
신규 페이지:
- 좌측 조직트리 (선택 시 필터)
- 우측 구성원 목록 (이름, 로그인ID, 조직, 상세)
- 구성원 추가/편집/삭제 (전화번호 없이 사람 정보만)
- Excel 일괄 등록
- 다중 선택 삭제

#### 6.2.4 가입자관리 — VoLTE
신규 페이지:
- 좌측 조직트리 → 구성원 선택
- 우측 선택된 구성원의 VoIP MSISDN 목록
- MSISDN 추가: ID(MSISDN), Auth ID, Password, DND, 착신전환
- MSISDN 편집/삭제
- Excel 일괄 등록

#### 6.2.5 가입자관리 — PTT
신규 페이지 (VoLTE와 동일 구조):
- 좌측 조직트리 → 구성원 선택
- 우측 PTT MSISDN 목록
- MSISDN 추가/편집/삭제

#### 6.2.6 가입자관리 — PTT 그룹
기존 GroupsPage 확장:
- 좌측 그룹 목록 (검색, 조직 필터)
- 우측 그룹 상세:
  - 기본정보: 그룹명, 그룹번호, 우선순위, 조직코드
  - 세션: 시작시간, 종료시간
  - 보안: 암호화여부, 긴급통화허용
  - 멤버: 조직트리→구성원→PTT MSISDN 3단계 선택
- Excel 일괄 등록

#### 6.2.7 실시간 서비스 상태
기존 ServiceStatusPage 확장:
- 좌측 조직트리로 필터
- 구성원별 행:
  - VoLTE MSISDN별: 등록상태(●/○), 통화상태(통화중/대기/미등록)
  - PTT MSISDN별: 등록상태, 그룹참여상태
- 5초 자동갱신

#### 6.2.8 서비스이력 — VoLTE
기존 CallLogsPage에서 VoIP 전용:
- 필터: 유형(등록/통화/기타), 발신, 착신, 날짜, 종료사유
- 테이블: 유형, 발신, 착신, 상태, 종료사유, 시작/종료시간, 통화시간
- 행 클릭 → 상세 (참여자, SIP상태)
- Flow 버튼 → 시퀀스 다이어그램
- 녹취재생 버튼 → WAV/MP4 재생
- 자동갱신 토글

#### 6.2.9 서비스이력 — PTT
신규 페이지:
- 필터: 그룹명/번호, 날짜
- 테이블: 그룹명, 그룹번호, 참여멤버수, 시작시간, 종료시간, 남은시간
- 행 클릭 → 상세 (참여자, 세그먼트 목록)
- Flow 버튼 → SIP + HTTPS 병합 시퀀스 다이어그램
- 녹취재생 → 세그먼트별 WAV/MP4 재생

#### 6.2.10 통계 — VoLTE 서비스
- 시간 단위 선택 (5분~1년)
- KPI: 호 시도, 성공률, 평균통화시간, 최대동시호
- 차트: 시간대별 호 시도/성공
- 종료사유 분포 바 차트

#### 6.2.11 통계 — PTT 서비스
- 시간 단위 선택
- KPI: 그룹콜 횟수, 평균세션시간, 평균참여멤버수
- 차트: 시간대별 그룹콜 수
- 그룹별 사용빈도 Top10

#### 6.2.12 통계 — SIP
- SIP 메서드별 카운트 (REGISTER, INVITE, BYE, SUBSCRIBE, NOTIFY)
- SIP 응답코드별 카운트 (1xx, 2xx, 3xx, 4xx, 5xx, 6xx)
- 시간대별 추이 차트

#### 6.2.13 통계 — CMP
- CMP 명령별 카운트 (add, remove, addgroup, joingroup, leavegroup, removegroup)
- 시간대별 추이 차트

#### 6.2.14 통계 — CSC
- CSP↔CSC 메시지 카운트 (user_change, group_change, stats)
- 시간대별 추이 차트

#### 6.2.15 통계 — HTTPS
- CSC↔UE MCPTT 메시지 (IdMS authreq/tokenreq, GMS GET/PUT/DELETE, CMS GET, KMS init/keyprov)
- 시간대별 추이 차트

#### 6.2.16 검증
- 검증 실행 버튼 → 백엔드에서 run_all.py 실행
- 실시간 진행률 표시
- 결과 테이블 (모듈별 PASS/FAIL/SKIP)
- 실패 항목 상세
- 과거 검증 이력

#### 6.2.17 문서
기존 DocsPage:
- 시스템 아키텍처
- UE 인터페이스 가이드
- Admin API 레퍼런스
- 검증 계획서
- 검증 항목서

---

## 7. 구현 단계

### Phase 1: DB + CSC API (PTT 그룹 확장)
1. ptt_groups ALTER TABLE (6개 컬럼)
2. cims_admin.py PTT 그룹 CRUD 확장
3. csc_service.py GMS XML 확장
4. 빌드 + 기존 테스트 통과 확인

### Phase 2: CSP/CMP 로직
1. CspPttGroup 구조체 확장
2. GroupMap DB/파일 로딩 확장
3. GroupCallService 세션시간/우선순위 적용
4. CMP floor 우선순위 확인
5. 빌드 + 테스트

### Phase 3: Console UI 리팩토링
1. App.tsx → 좌측 사이드바 + 라우팅
2. 공통 컴포넌트: OrgTree (조직트리), UserPicker (구성원선택)
3. 가입자관리 5개 서브페이지
4. 실시간 서비스 상태
5. 서비스이력 VoLTE/PTT 분리
6. 통계 6개 서브페이지
7. 검증 페이지
8. Console 빌드

### Phase 4: 테스트 + 검증
1. PTT 그룹 확장 필드 테스트 추가
2. 전체 92건+ 검증 실행
3. Console UI 수동 확인

---

## 8. 파일 변경 목록

### DB
| 파일 | 변경 |
|------|------|
| `sql/migrate_ptt_groups_v2.sql` | 신규: ALTER TABLE |

### CSC
| 파일 | 변경 |
|------|------|
| `csc/bin/csc_pihttp/src/cims_admin.py` | PTT 그룹 CRUD 확장 |
| `csc/bin/csc_pihttp/src/csc_service.py` | GMS XML 확장 |
| `csc/bin/csc_pihttp/src/cims_stats.py` | 통계 API 세분화 |
| `csc/bin/csc_pihttp/src/csc_flow.py` | 서비스이력 API 분리 (VoLTE/PTT) |

### CSP
| 파일 | 변경 |
|------|------|
| `csp/CspPttGroup.h` | 구조체 확장 |
| `csp/GroupMap.cpp` | DB/파일 로딩 확장 |
| `csp/GroupCallService.cpp` | 세션시간/우선순위 |
| `csp/DbManager.cpp` | SELECT/INSERT 확장 |

### CMP
| 파일 | 변경 |
|------|------|
| `cmp/McpttGroup.cpp` | 우선순위 floor 확인 |

### Console
| 파일 | 변경 |
|------|------|
| `cims-console/src/App.tsx` | 사이드바 + 라우팅 전면 변경 |
| `cims-console/src/components/Sidebar.tsx` | 신규: 좌측 사이드바 |
| `cims-console/src/components/OrgTree.tsx` | 신규: 공유 조직트리 컴포넌트 |
| `cims-console/src/components/UserPicker.tsx` | 신규: 구성원 선택 컴포넌트 |
| `cims-console/src/pages/MembersPage.tsx` | 신규: 구성원 관리 |
| `cims-console/src/pages/VolteMsisdnPage.tsx` | 신규: VoLTE MSISDN 관리 |
| `cims-console/src/pages/PttMsisdnPage.tsx` | 신규: PTT MSISDN 관리 |
| `cims-console/src/pages/PttHistoryPage.tsx` | 신규: PTT 서비스이력 |
| `cims-console/src/pages/StatsSipPage.tsx` | 신규: SIP 통계 |
| `cims-console/src/pages/StatsCmpPage.tsx` | 신규: CMP 통계 |
| `cims-console/src/pages/StatsCscPage.tsx` | 신규: CSC 통계 |
| `cims-console/src/pages/StatsHttpsPage.tsx` | 신규: HTTPS 통계 |
| `cims-console/src/pages/VerificationPage.tsx` | 신규: 검증 실행/결과 |
| `cims-console/src/api/stats.ts` | 통계 API 확장 |
| `cims-console/src/api/groups.ts` | 그룹 API 확장 필드 |

### 테스트
| 파일 | 변경 |
|------|------|
| `tests/test_ptt_service.py` | 확장 필드 검증 추가 |
| `tests/test_csc.py` | PTT 그룹 CRUD 확장 필드 테스트 |
