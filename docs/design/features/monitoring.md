# CIMS 모니터링·이력·통계 설계서

> 버전: 4.0 (2026-04-10)

---

## 개요

Console UI에서 제공하는 운영 기능을 3개 파트로 구분한다.

| 파트 | 목적 | 데이터 성격 |
|------|------|------------|
| **Part 1. 실시간 모니터링** | 현재 서비스 상태 감시 | 메모리 (CSP/CMP 내부 상태) |
| **Part 2. 서비스 이력 조회** | 통화 이력 + 메시지 Flow + 녹취 재생 | NAS 파일 (raw 로그 + 녹취) |
| **Part 3. 통계** | 메시지/서비스 통계 지표 | NAS 파일 기반 집계 |

### 데이터 저장 원칙

- **NAS 우선**: 모든 raw 데이터(메시지 로그, 녹취)는 공유 NAS(`ext_mnt/`)에 비동기 기록
- **DB 최소화**: DB에는 raw 데이터 접근을 위한 인덱스 정보만 저장 (call_id, 시간, 파일 경로)
- **on-demand 처리**: 트랜스코딩, 통계 집계는 조회 시점에 수행하거나 백그라운드 배치

### NAS 디렉터리 구조

```
ext_mnt/                                ← 공유 스토리지 마운트 포인트
  ├─ msg_log/                            ← SIP/인터페이스 메시지 로그 (full message body 포함)
  │   └─ csp/sip/                        ← CSP SIP 메시지 (SipMessageLogger)
  │       └─ {YYYY}/{MM}/{DD}/{HH}/
  │           └─ sip.jsonl               ← Call-ID, method, from/to, direction, full SIP text
  │
  ├─ service_log/                        ← 서비스 이력 + Flow + 녹취 (body 포함)
  │   ├─ voip/{YYYY}/{MM}/{DD}/{HH}/     ← VoIP 통화 이력
  │   │   └─ {prefix}/{caller}/{session_id}.d/
  │   │       ├─ call.json               ← 통화 메타 (state, times, reason)
  │   │       ├─ participants.jsonl      ← 참여자
  │   │       ├─ session.json            ← Session-ID ↔ Call-ID 매핑 {session_id, call_ids: [leg_a, leg_b]}
  │   │       ├─ raw_a.rtp              ← 녹취 raw RTP (발신측)
  │   │       └─ raw_b.rtp              ← 녹취 raw RTP (착신측)
  │   │
  │   ├─ ptt/{YYYY}/{MM}/{DD}/{HH}/      ← PTT 그룹통화 이력
  │   │   └─ {prefix}/{group_id}.d/
  │   │       ├─ call.jsonl              ← 세션별 누적 JSONL
  │   │       ├─ participants.jsonl      ← 참여자
  │   │       └─ seg_*.rtp              ← 발언 단위 녹취 raw
  │   │
  │   └─ {type}/{YYYY}/{MM}/{DD}/{HH}/
  │       └─ index.json                  ← 시간 단위 인덱스 (JSONL)
  │
  └─ stats/                              ← 통계 집계 결과 캐시
      └─ {YYYYMMDD}/
          ├─ 5m.json                     ← 5분 단위 (7일 보관)
          ├─ 10m.json                    ← 10분 단위 (14일 보관)
          └─ 1h.json                     ← 1시간 단위 (90일 보관)
```

#### Session-ID 기반 통합 로깅

B2BUA 모드에서는 하나의 통화가 두 개의 Call-ID로 분리된다. Session-ID는 이를 하나의 세션으로 통합하여 로깅한다.

- **Session-ID 형식**: `S{YYYYMMDDHHMMSS}{microseconds}` (예: `S20260410143015123456`)
- **생성 시점**: B2BUA `CreateCall()` 시 CSP가 생성
- **매핑**: 발신 leg Call-ID + 착신 leg Call-ID → 동일 Session-ID → 동일 `.d` 디렉터리
- **record_dir 전달**: CSP가 CMP에 세션 디렉터리 경로를 전달, CMP가 녹취 raw RTP 기록
- **session.json**: `.d` 디렉터리에 `{session_id, call_ids: [leg_a, leg_b]}` 저장, Flow 재구성 시 양 leg 상관 분석에 사용

### DB 역할 (최소화)

통화 이력은 **파일 기반**(call.json / call.jsonl)으로 저장하며, DB는 사용하지 않는다.
통화 이력 조회는 `service_log/` 디렉터리를 직접 스캔하거나 `index.json`을 참조한다.

DB에 저장하는 데이터:
```
subscriptions       ← 가입자 구독 정보 (VoIP/PTT)
ptt_groups          ← PTT 그룹 설정 (멤버, 우선순위, 암호화 등)
organizations       ← 조직 트리 (code_path 기반)
admin_users         ← 관리자 계정
```

---

## Part 1. 실시간 서비스 상태 모니터링

CSP/CMP 내부 메모리 상태를 주기적으로 수집하여 Console UI 대시보드에 표시한다.

### 1.1 헬스체크

| 항목 | 판정 기준 | 수집 방식 |
|------|----------|----------|
| CSP 프로세스 | PID + 응답 | CSC → CSP UDP (port 4421) |
| CMP 프로세스 | `alive` 응답 | CSC → CMP UDP (port 9000) |
| CSP↔CMP 연결 | heartbeat 상태 | CSP 내부 keepalive |
| DB 연결 | SELECT 1 | CSC 내부 |
| CSC 프로세스 | HTTP /test | self-check |

### 1.2 가입자 접속 상태

| 항목 | 설명 | 소스 |
|------|------|------|
| 등록 사용자 수 | REGISTER 유효 가입자 | CSP `gclsUserMap` |
| 사용자 목록 | ID, IP:Port, 등록시간, 서비스타입, 만료시간 | CSP `gclsUserMap.GetString()` |
| 미등록 사용자 수 | 전체 가입자 - 등록 사용자 | DB `subscriptions` vs UserMap |

### 1.3 VoIP 통화 상태

| 항목 | 설명 | 소스 |
|------|------|------|
| 활성 VoIP 호 수 | B2BUA 진행 중 | CSP `gclsCallMap.GetCount()` |
| 활성 호 목록 | CallId, 발신, 착신, 시작시간, 상태(ringing/active), 모듈(TAS/IBCF) | CSP `gclsCallMap` + `m_mapCallOwner` |
| RTP 포트 사용률 | 사용중 / 전체 | CSP `gclsRtpMap` 또는 CMP `_freeResources` |

### 1.4 PTT 그룹통화 상태

| 항목 | 설명 | 소스 |
|------|------|------|
| 활성 그룹 세션 수 | CMP 그룹 세션 | CMP `_groups.size()` |
| 그룹 목록 | 그룹ID, 그룹명, 멤버수, Floor 상태, 발언자 | CMP `McpttGroup` |
| 그룹별 참여 멤버 | 현재 joinGroup 멤버 목록 | CMP `McpttGroup::_members` |
| Floor 상태 | IDLE / GRANTED(발언자) / PENDING | CMP `McpttGroup::_floorTaken` |

### 1.5 시스템 리소스

| 항목 | 소스 |
|------|------|
| CSP/CMP 프로세스 메모리 (RSS) | `/proc/{pid}/status` |
| SIP 트랜잭션 수 (ICT/NICT/IST/NIST) | CSP `CSipStack.GetString()` |
| Nonce 풀 크기 | CSP `gclsNonceMap.GetCount()` |
| Subscription 수 (GMS/CMS) | CSP `gclsSubscriptionManager` |

### 1.6 알람 조건

| 조건 | 심각도 | 설명 |
|------|--------|------|
| CMP 연결 끊김 | Critical | heartbeat 실패 |
| DB 연결 끊김 | Critical | 가입자 조회 불가 |
| RTP 포트 소진 | Major | 신규 호 불가 |
| RTP 포트 80%+ | Warning | 포트 풀 부족 예고 |
| 등록 사용자 0명 | Major | 전체 서비스 장애 의심 |
| 인증 실패 급증 (분당 N건) | Warning | 무차별 공격 의심 |

### 1.7 Console UI: 대시보드

```
┌─────────────────────────────────────────────────────────────┐
│  CIMS Dashboard                                              │
│                                                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐       │
│  │ CSP  🟢  │ │ CMP  🟢  │ │ DB   🟢  │ │ CSC  🟢  │       │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘       │
│                                                              │
│  등록: 42명     VoIP: 3건     PTT 그룹: 2건     RTP: 8/20   │
│                                                              │
│  ── VoIP 활성 통화 ──────────────────────────────────────    │
│  발신    착신    시작      상태    모듈                        │
│  1001   1002   14:30:15  active  TAS(B2BUA)                  │
│  1003   1004   14:32:00  ringing TAS(B2BUA)                 │
│                                                              │
│  ── PTT 그룹 세션 ──────────────────────────────────────    │
│  그룹     멤버  Floor      발언자                             │
│  Alpha팀  4명   GRANTED   김철수(1001)                       │
│  Beta팀   3명   IDLE      -                                  │
└─────────────────────────────────────────────────────────────┘
```

---

## Part 2. 서비스 이력 조회

통화 종료 후 이력을 조회하고, 메시지 Flow 시각화 + 녹취 재생을 **하나의 화면**에서 제공한다.

### 2.1 데이터 기록 흐름

```
통화 시작:
  CSP → service_log/{type}/.../session.d/call.json    (통화 메타 생성, state=ringing)
  CSP → service_log/.../session.d/session.json         (Session-ID ↔ Call-ID 매핑)
  CSP → CMP: add/addgroup + record_dir 파라미터 전달
  SipMessageLogger → msg_log/csp/sip/.../sip.jsonl    (SIP TX/RX 기록 시작)

통화 중:
  SipMessageLogger → msg_log/csp/sip/.../sip.jsonl    (모든 SIP 메시지 + CMP JSON, Call-ID 포함)
  CMP → service_log/.../session.d/raw_a.rtp           (발신측 RTP 녹취, record_dir)
  CMP → service_log/.../session.d/raw_b.rtp           (착신측 RTP 녹취, record_dir)

통화 종료:
  CSP → service_log/.../session.d/call.json 업데이트  (state=ended, end_time, reason)
  CSP → service_log/.../index.json 추가               (시간 단위 인덱스)
```

### 2.2 VoIP 통화 이력

#### 목록 조회

| 필드 | 설명 | 소스 |
|------|------|------|
| call_id | SIP Call-ID (또는 Session-ID) | call.json |
| initiator | 발신자 | call.json |
| callee | 착신자 | call.json |
| invite_time | 호 시도 시간 | call.json |
| answer_time | 응답 시간 | call.json |
| end_time | 종료 시간 | call.json |
| duration | 통화 시간(초) | call.json |
| end_reason | 종료 사유 (normal/busy/cancel/timeout/error) | call.json |
| has_recording | 녹취 존재 여부 | 파일 존재 체크 |

#### 상세 보기 (클릭 시)

하나의 화면에서 3가지를 모두 제공:

```
┌──────────────────────────────────────────────────────────────┐
│  VoIP 통화 상세 — 1001 → 1002 (2026-04-03 14:30:15)         │
│                                                               │
│  ┌─ 통화 정보 ────────────────────────────────────────────┐  │
│  │ 발신: 1001    착신: 1002    시간: 2:35                  │  │
│  │ 상태: 정상종료  SIP: 200    모듈: TAS(B2BUA)            │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─ 메시지 Flow ──────────────────────────────────────────┐  │
│  │                                                         │  │
│  │  UEᴼ(1001)        CSP            CMP          UEᵀ(1002)│  │
│  │    │── INVITE ───→│               │              │      │  │
│  │    │               │── add ──────→│              │      │  │
│  │    │               │←─ add-resp ──│              │      │  │
│  │    │               │── INVITE ──────────────────→│      │  │
│  │    │←── 180 ───────│←──────────── 180 ───────────│      │  │
│  │    │←── 200 ───────│←──────────── 200 ───────────│      │  │
│  │    │── ACK ───────→│── ACK ─────────────────────→│      │  │
│  │    │    ~~~ 통화 중 (RTP via CMP) ~~~             │      │  │
│  │    │── BYE ───────→│── BYE ─────────────────────→│      │  │
│  │    │←── 200 ───────│←──────────── 200 ───────────│      │  │
│  │    │               │── remove ──→│              │      │  │
│  │    │               │←─ remove-resp│              │      │  │
│  │                                                         │  │
│  │  * csp.jsonl: CSP SIP 메시지 (from/to: ue_o, csp, ue_t)│  │
│  │  * cmp.jsonl: CMP RTP 제어 (session-start/end)          │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─ 녹취 재생 ───────────────────────────────────────────┐  │
│  │  ▶  ━━━━━━━●━━━━━━━━━━  1:20 / 2:35                   │  │
│  │  [음성] [발신측 영상▾] [착신측 영상]                     │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

#### 메시지 Flow 데이터 소스

`msg_log/csp/sip/YYYY/MM/DD/HH/sip.jsonl`에서 Call-ID로 검색하여 B2BUA 양 leg 메시지를 시간순 재구성:

1. `session.json`에서 `call_ids: [leg_a, leg_b]` 읽음
2. `sip.jsonl`에서 양 Call-ID로 검색 → 시간순 병합
3. ACK 포함 완전한 B2BUA Flow 표시

```jsonl
{"ts":"14:30:15.123","call_id":"leg_a_xxx","dir":"RX","method":"INVITE","from":"1001","to":"1002","msg":"..."}
{"ts":"14:30:15.130","call_id":"","dir":"TX","method":"","from":"csp","to":"cmp","msg":"{\"cmd\":\"add\",...}"}
{"ts":"14:30:15.150","call_id":"leg_b_yyy","dir":"TX","method":"INVITE","from":"csp","to":"1002","msg":"..."}
{"ts":"14:30:16.200","call_id":"leg_b_yyy","dir":"RX","method":"180","from":"1002","to":"csp","msg":"..."}
{"ts":"14:30:16.201","call_id":"leg_a_xxx","dir":"TX","method":"180","from":"csp","to":"1001","msg":"..."}
```

- **sip.jsonl**: SipMessageLogger(ILogCallBack)가 기록. psip SIP TX/RX + CMP JSON 메시지 (전체 SIP text 포함)
- Flow API: `session.json`의 Call-ID로 `sip.jsonl` 검색 → B2BUA 양 leg 상관 분석

CSC의 Flow API (`GET /api/v1/flow/{session_id}?date=...`)에서 session.json 읽고 sip.jsonl 검색 반환.

#### Console UI: Flow 상세 페이지

FlowPage는 상단 SVG 시퀀스 다이어그램 + 하단 메시지 목록/상세로 구성:
- 상단: UE(발신) ↔ CSP ↔ CMP ↔ UE(착신) 간 화살표 다이어그램
- 하단: 메시지 클릭 시 전체 SIP text 표시

### 2.3 PTT 통화 이력

#### 목록 조회

| 필드 | 설명 |
|------|------|
| call_id | 그룹 세션 ID |
| group_id | 그룹 번호 |
| initiator | 최초 발언자 |
| invite_time | 세션 시작 |
| end_time | 세션 종료 |
| segment_count | 발언 수 |
| total_speech_sec | 총 발언 시간 |
| rec_status | 녹취 상태 |

#### 상세 보기

```
┌──────────────────────────────────────────────────────────────┐
│  PTT 통화 상세 — Alpha팀(1000)  2026-04-03                    │
│                                                               │
│  ┌─ 세션 정보 ────────────────────────────────────────────┐  │
│  │ 그룹: Alpha팀(1000)  시작: 09:00  종료: 17:30          │  │
│  │ 발언: 47건 / 30분 32초                                  │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─ 메시지 Flow (세션 시작) ──────────────────────────────┐  │
│  │  UE(1001)        CSP           CMP          UE(1002)   │  │
│  │    │              │── addGroup→│              │         │  │
│  │    │←── INVITE ───│             │              │         │  │
│  │    │── 200 ──────→│             │              │         │  │
│  │    │              │── joinGroup→│              │         │  │
│  │    │              │── INVITE ──────────────────→│         │  │
│  │    │              │←──────────── 200 ──────────│         │  │
│  │    │              │── joinGroup→│              │         │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─ 발언 타임라인 ───────────────────────────────────────┐  │
│  │ 09:00       10:00       11:00       12:00              │  │
│  │ ├──┤        ├┤          ├──┤                           │  │
│  │ 1001        1003        1001                           │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─ 발언 목록 ───────────────────────────────────────────┐  │
│  │ #  │화자   │시간      │길이  │영상│재생                 │  │
│  │ 1  │김철수 │09:00:12 │4:51 │ ○ │ ▶ ━━●━━━ 2:30       │  │
│  │ 2  │박영희 │09:30:45 │0:38 │   │ ▶                    │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

### 2.4 API 설계

```
# 통화 이력 (파일 기반, VoIP/PTT 분리)
GET /api/v1/call/logs?date=YYYY-MM-DD&hour=HH&call_type=voip|ptt&msisdn=...&limit=50

# 메시지 Flow (sip.jsonl에서 Call-ID 검색, session.json으로 B2BUA 상관)
GET /api/v1/flow/{session_id}?date=YYYY-MM-DD&hour=HH
GET /api/v1/flow/list?date=YYYY-MM-DD&hour=HH

# 녹취 재생
GET /api/v1/recordings/{call_id}/audio?date=YYYY-MM-DD
GET /api/v1/recordings/{call_id}/video?date=YYYY-MM-DD
GET /api/v1/recordings?date=YYYY-MM-DD&call_type=voip|ptt
```

모든 API는 `service_log/` 디렉터리를 직접 스캔하여 데이터 반환 (DB 미사용).

### 2.5 녹취 파일 형식 및 트랜스코딩

#### Raw RTP 녹취 형식

CMP RtpRecorder가 기록하는 raw 파일 형식 (`.rtp`):
```
[uint32 len][int64 recv_usec][rtp_pkt]  ← 패킷 반복
```
- `len`: RTP 패킷 길이 (4바이트)
- `recv_usec`: 수신 시각 wall-clock (마이크로초, 8바이트) — 오디오/비디오 동기화에 사용
- `rtp_pkt`: 원본 RTP 패킷

#### 트랜스코딩 파이프라인

**음성 (Audio)**:
1. RTP → AMR-WB 페이로드 추출 (DTX 인식: 타임스탬프 기반 NO_DATA 프레임 삽입)
2. AMR-WB → PCM 16kHz 디코딩
3. 발신측 + 착신측 PCM → amix 필터로 믹싱
4. 믹싱 PCM → WAV 출력

**영상 (Video)**:
1. RTP → H.264 NAL 재조립 (FU-A fragment reassembly)
2. 프레임레이트: RTP 타임스탬프로부터 자동 계산
3. 발신측(좌) + 착신측(우) → side-by-side 합성 + 오디오 믹싱
4. recv_usec 오프셋으로 오디오/비디오 동기화 (`-itsoffset`)
5. → MP4 출력

#### Console UI: VoLTE 이력 상세

VolteHistoryPage 상세 모달에서 오디오 플레이어 + 비디오 플레이어 제공.

---

## Part 3. 통계

NAS의 raw 데이터(`msg_log/` 인터페이스 통계, `service_log/` 서비스 이력)를 기반으로 통계를 집계한다.
UI에서 **5분 / 10분 / 1시간 / 1일 / 1월 / 1년** 단위를 선택하여 조회 가능.

### 3.0 시간 단위 (Granularity)

| 단위 | 코드 | X축 포인트 수 | 기본 조회 범위 | 용도 |
|------|------|:------------:|:------------:|------|
| **5분** | `5m` | 12점/시간 | 최근 1시간 | 실시간 모니터링, 장애 탐지 |
| **10분** | `10m` | 6점/시간 | 최근 3시간 | 단기 추이 확인 |
| **1시간** | `1h` | 24점/일 | 당일 | 일간 패턴 분석 |
| **1일** | `1d` | 28~31점/월 | 최근 30일 | 주간/월간 추이 |
| **1월** | `1M` | 12점/년 | 최근 1년 | 연간 추이 |
| **1년** | `1y` | 전체 | 전체 | 장기 추이 |

모든 통계 API는 `granularity` 파라미터로 시간 단위를 지정:
```
GET /api/v1/stats/...?granularity=5m&from=...&to=...
```

### 3.1 메시지 통계

각 프로토콜별 메시지 수를 선택한 시간 단위로 집계.

#### 데이터 소스
NAS `msg_log/csp/sip/{YYYY}/{MM}/{DD}/{HH}/sip.jsonl` 파일에서 `method`, `dir`, `ts` 필드를 집계.

#### 통계 항목

| 프로토콜 | 메시지 | 집계 항목 |
|----------|--------|----------|
| **SIP** | REGISTER | 성공 / 실패 건수 |
| **SIP** | INVITE | 발신 / 착신 건수 |
| **SIP** | BYE / CANCEL | 건수 |
| **SIP** | SUBSCRIBE / NOTIFY | 건수 |
| **SIP** | 응답코드 | 200/3xx/4xx/5xx 코드별 분포 |
| **JSON** (CSP↔CMP) | add / remove / addGroup / joinGroup | 건수 |
| **RTCP** | Floor REQUEST / GRANT / RELEASE | 건수 |

#### Console UI: 메시지 통계

```
┌──────────────────────────────────────────────────────────────┐
│  메시지 통계                                                  │
│                                                               │
│  [5분] [10분] [1시간] [1일] [1월] [1년]   [SIP▾] [전체▾]     │
│  ─────                                                        │
│  2026-04-03 09:00 ~ 10:00                                     │
│                                                               │
│  ┌───────────────────────────────────────────────────────┐   │
│  │ 35│  ██                                                │   │
│  │ 30│  ██    ██                                          │   │
│  │ 25│  ██ ██ ██                                          │   │
│  │ 20│  ██ ██ ██ ██ ██                                    │   │
│  │ 15│  ██ ██ ██ ██ ██ ██ ██       ██                     │   │
│  │ 10│  ██ ██ ██ ██ ██ ██ ██ ██ ██ ██ ██                  │   │
│  │  5│  ██ ██ ██ ██ ██ ██ ██ ██ ██ ██ ██ ██               │   │
│  │   └──────────────────────────────────────              │   │
│  │   :00 :05 :10 :15 :20 :25 :30 :35 :40 :45 :50 :55    │   │
│  └───────────────────────────────────────────────────────┘   │
│                                                               │
│  ■ REGISTER  ■ INVITE  ■ BYE  ■ SUBSCRIBE                   │
│                                                               │
│  SIP 응답 코드 분포 (선택 구간)                                │
│  200 OK: 87%  │  4xx: 8%  │  5xx: 2%  │  Timeout: 3%        │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 서비스 통계

`service_log/` 디렉터리의 `call.json`/`call.jsonl` + 녹취 정보를 기반으로 서비스 품질 지표를 산출.
시간 단위 선택에 따라 집계 범위가 변경됨.

#### VoIP 서비스 통계

| 지표 | 산출 방법 | 5m~1h | 1d~1y |
|------|----------|:-----:|:-----:|
| 호 시도 수 | call_logs count per bucket | ✅ | ✅ |
| 호 성공률 | 200 OK / 전체 INVITE | ✅ | ✅ |
| 평균 통화 시간 | avg(duration) WHERE end_reason='normal' | ✅ | ✅ |
| 동시 통화 피크 | max(concurrent) per bucket | ✅ | ✅ |
| 실패 사유 분포 | count GROUP BY end_reason | - | ✅ |
| DND/착신전환 처리율 | count WHERE end_reason='dnd'/'forward' | - | ✅ |
| 평균 호 설정 시간 | avg(answer_time - invite_time) | ✅ | ✅ |

#### PTT 서비스 통계

| 지표 | 산출 방법 | 5m~1h | 1d~1y |
|------|----------|:-----:|:-----:|
| 그룹콜 수 | call.jsonl count (파일 스캔) | ✅ | ✅ |
| 그룹별 사용 빈도 | count GROUP BY group_id | - | ✅ |
| 평균 세션 시간 | avg(end_time - invite_time) | - | ✅ |
| 발언 횟수 | recording_segments count | ✅ | ✅ |
| 평균 발언 시간 | avg(duration_ms) | - | ✅ |
| Floor 경합률 | REJECT / REQUEST | - | ✅ |
| 사용자별 발언 비율 | segments GROUP BY speaker_id | - | ✅ |

#### IBCF 트렁크 통계

| 지표 | 산출 방법 | 5m~1h | 1d~1y |
|------|----------|:-----:|:-----:|
| 트렁크별 호 수 | call_logs GROUP BY trunk | ✅ | ✅ |
| 트렁크 가용률 | 성공 / 전체 | ✅ | ✅ |
| 실패 사유 분포 | GROUP BY sip_status | - | ✅ |

#### Console UI: 서비스 통계

```
┌──────────────────────────────────────────────────────────────┐
│  서비스 통계                                                  │
│                                                               │
│  [5분] [10분] [1시간] [1일] [1월] [1년]   [VoIP▾]            │
│                              ────                             │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐        │
│  │호 시도    │ │호 성공률  │ │평균통화   │ │동시호피크 │        │
│  │  1,247건  │ │  94.3%   │ │  2:35    │ │   23건   │        │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘        │
│                                                               │
│  호 시도 수 추이 (1일 단위 × 30일)                             │
│  ┌───────────────────────────────────────────────────────┐   │
│  │ 1500│        ██                                        │   │
│  │ 1200│     ██ ██ ██        ██                           │   │
│  │  900│  ██ ██ ██ ██ ██  ██ ██ ██                        │   │
│  │  600│  ██ ██ ██ ██ ██  ██ ██ ██ ██ ██  ...             │   │
│  │  300│  ██ ██ ██ ██ ██  ██ ██ ██ ██ ██                  │   │
│  │     └──────────────────────────────────                │   │
│  │     03/05          03/15          03/25          04/03  │   │
│  └───────────────────────────────────────────────────────┘   │
│                                                               │
│  호 성공률 추이                                               │
│  ┌───────────────────────────────────────────────────────┐   │
│  │ 100%│─── ─── ─── ─── ─── ───                          │   │
│  │  95%│──                       ───                      │   │
│  │  90%│                             ──                   │   │
│  │     └──────────────────────────────                    │   │
│  └───────────────────────────────────────────────────────┘   │
│                                                               │
│  실패 사유 분포 (선택 구간 합산)                               │
│  ████████████████████████░░░░░░  정상종료 82%                │
│  ████░░░░░░░░░░░░░░░░░░░░░░░░  발신취소 8%                  │
│  ███░░░░░░░░░░░░░░░░░░░░░░░░░  착신거부 5%                  │
│  ██░░░░░░░░░░░░░░░░░░░░░░░░░░  시간초과 3%                  │
│  █░░░░░░░░░░░░░░░░░░░░░░░░░░░  오류 2%                      │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 통계 집계 및 저장 방식

#### 집계 단위별 저장

| 단위 | 저장 | 집계 시점 | 보관 |
|------|------|----------|------|
| **5분** | NAS `stats/{date}/5m.json` | 5분 주기 배치 | 7일 |
| **10분** | NAS `stats/{date}/10m.json` | 10분 주기 배치 | 14일 |
| **1시간** | NAS `stats/{date}/1h.json` | 1시간 주기 배치 | 90일 |
| **1일** | DB `stats_daily` 테이블 | 자정 배치 | 2년 |
| **1월** | DB `stats_monthly` 테이블 | 월초 배치 | 영구 |
| **1년** | DB `stats_yearly` 테이블 | 연초 배치 | 영구 |

#### 집계 흐름

```
msg_log/*.jsonl  ─┐
                   ├─→  5분/10분/1시간 집계 → NAS stats/{date}/*.json
DB call_logs     ─┘

NAS stats/1h.json  ──→  1일 집계 → DB stats_daily
DB stats_daily     ──→  1월 집계 → DB stats_monthly
DB stats_monthly   ──→  1년 집계 → DB stats_yearly
```

짧은 주기(5m/10m/1h)는 NAS 파일로 저장하여 DB 부하 최소화.
긴 주기(1d/1M/1y)는 DB에 요약 저장하여 장기 조회 성능 확보.

#### 당일 데이터 (on-demand)

아직 배치가 완료되지 않은 현재 시간대는 API 요청 시 **실시간 집계**:
- 5m/10m: msg_log jsonl에서 직접 파싱
- 1h: NAS 캐시 + 미완료 구간 보충
- 1d 이상: DB 쿼리 + 당일 보충

#### DB 스키마 (집계 테이블)

```sql
-- 일간 집계
CREATE TABLE stats_daily (
    date          DATE NOT NULL,
    category      VARCHAR(32) NOT NULL,    -- 'voip', 'ptt', 'ibcf', 'sip_msg', 'cmp_msg'
    metric        VARCHAR(64) NOT NULL,    -- 'call_attempts', 'success_rate', 'avg_duration', ...
    value         DOUBLE NOT NULL,
    detail_json   TEXT DEFAULT NULL,        -- 세부 분포 (end_reason 등) JSON
    PRIMARY KEY (date, category, metric)
);

-- 월간 집계
CREATE TABLE stats_monthly (
    year_month    VARCHAR(7) NOT NULL,     -- '2026-04'
    category      VARCHAR(32) NOT NULL,
    metric        VARCHAR(64) NOT NULL,
    value         DOUBLE NOT NULL,
    detail_json   TEXT DEFAULT NULL,
    PRIMARY KEY (year_month, category, metric)
);

-- 연간 집계
CREATE TABLE stats_yearly (
    year          SMALLINT NOT NULL,
    category      VARCHAR(32) NOT NULL,
    metric        VARCHAR(64) NOT NULL,
    value         DOUBLE NOT NULL,
    detail_json   TEXT DEFAULT NULL,
    PRIMARY KEY (year, category, metric)
);
```

#### NAS 집계 파일 형식

`stats/{YYYYMMDD}/1h.json` 예시:
```json
{
  "date": "2026-04-03",
  "granularity": "1h",
  "buckets": [
    {
      "time": "2026-04-03T09:00:00",
      "sip_msg": { "REGISTER": 45, "INVITE": 32, "BYE": 28, "200": 95, "4xx": 5 },
      "voip": { "attempts": 32, "success": 30, "avg_duration": 155, "concurrent_peak": 12 },
      "ptt": { "group_calls": 3, "floor_grants": 15, "floor_rejects": 2, "segments": 18 },
      "cmp": { "add": 32, "remove": 28, "addGroup": 3, "joinGroup": 12 }
    },
    {
      "time": "2026-04-03T10:00:00",
      "sip_msg": { "..." : "..." }
    }
  ]
}
```

### 3.4 API 설계

모든 통계 API에 공통 파라미터:
- `granularity`: `5m` | `10m` | `1h` | `1d` | `1M` | `1y`
- `from`: 시작 시간 (ISO 8601)
- `to`: 종료 시간 (ISO 8601)

```
# 메시지 통계
GET /api/v1/stats/messages?granularity=1h&from=2026-04-03T00:00&to=2026-04-03T23:59&proto=SIP
GET /api/v1/stats/messages/sip-codes?granularity=1d&from=2026-03-01&to=2026-04-03

# 서비스 통계
GET /api/v1/stats/service/voip?granularity=1d&from=2026-03-01&to=2026-04-03
GET /api/v1/stats/service/ptt?granularity=1h&from=2026-04-03T00:00&to=2026-04-03T23:59
GET /api/v1/stats/service/ibcf?granularity=1M&from=2026-01-01&to=2026-12-31

# 요약 (카드에 표시할 KPI)
GET /api/v1/stats/service/summary?granularity=1d&date=2026-04-03
```

#### 응답 예시: `GET /api/v1/stats/service/voip?granularity=1h&from=...&to=...`

```json
{
  "granularity": "1h",
  "from": "2026-04-03T09:00:00",
  "to": "2026-04-03T17:00:00",
  "summary": {
    "total_attempts": 1247,
    "success_rate": 94.3,
    "avg_duration_sec": 155,
    "concurrent_peak": 23
  },
  "buckets": [
    { "time": "2026-04-03T09:00:00", "attempts": 82, "success": 78, "success_rate": 95.1, "avg_duration": 148, "concurrent_peak": 12 },
    { "time": "2026-04-03T10:00:00", "attempts": 105, "success": 98, "success_rate": 93.3, "avg_duration": 162, "concurrent_peak": 18 },
    "..."
  ],
  "end_reasons": {
    "normal": 1048, "cancel": 100, "busy": 62, "timeout": 25, "error": 12
  }
}
```

---

## Console UI 탭 구조

기존:

```
[가입자 관리] [PTT 그룹 관리] [통화현황] [녹취 관리] [문서]
```

개선:

```
[대시보드] [가입자 관리] [PTT 그룹 관리] [서비스 이력] [통계] [문서]
```

| 탭 | Part | 내용 |
|-----|------|------|
| **대시보드** | Part 1 | 헬스체크, 접속현황, 활성 통화/그룹, 알람 |
| **가입자 관리** | (기존) | 가입자 CRUD |
| **PTT 그룹 관리** | (기존) | 그룹 CRUD |
| **서비스 이력** | Part 2 | VoIP/PTT 통화 이력 + Flow + 녹취 (통합) |
| **통계** | Part 3 | 메시지 통계 + 서비스 통계 |
| **문서** | (기존) | API 문서 |

---

## 컴포넌트별 역할 요약

| 컴포넌트 | 기록 (NAS) | DB | Console 제공 |
|----------|-----------|-----|-------------|
| **CSP** | msg_log/csp/sip/.../sip.jsonl (SipMessageLogger) + service_log/.../call.json, session.json, participants.jsonl | - | - |
| **CMP** | service_log/.../*.rtp (녹취 raw, record_dir) | - | - |
| **CSC** | - | - | REST API: 이력조회, Flow(sip.jsonl 검색), 녹취 on-demand 변환, 통계 집계 |
| **Console** | - | - | UI: 대시보드, 이력+Flow+녹취, 통계 차트 |

---

## 변경 이력

| 날짜 | 버전 | 내용 |
|------|------|------|
| 2026-04-02 | 1.0 | 초기 정의 — CSP 모듈별 상태/통계 항목 |
| 2026-04-03 | 2.0 | 3파트 재설계 — 실시간 모니터링 / 서비스 이력(Flow+녹취 통합) / 통계 |
| 2026-04-03 | 2.1 | Part 3 통계 보완 — 다중 시간 단위(5m/10m/1h/1d/1M/1y), 계층적 집계/저장, DB 스키마 |
| 2026-04-10 | 4.0 | VoLTE B2BUA 전환: Proxy 모드 제거, SipMessageLogger(sip.jsonl) 기반 Flow, session.json 매핑, 녹취 recv_usec 추가, 트랜스코딩(DTX/FU-A/sync) 상세화 |
