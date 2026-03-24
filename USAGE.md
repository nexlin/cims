# CIMS 사용 가이드

## 목차
1. [시스템 구성](#1-시스템-구성)
2. [서비스 실행/관리](#2-서비스-실행관리)
3. [Web UI 사용법](#3-web-ui-사용법)
4. [소프트폰 사용법](#4-소프트폰-사용법)
5. [단말 시뮬레이터 (cspsim)](#5-단말-시뮬레이터-cspsim)
6. [계정 정보](#6-계정-정보)
7. [포트 구성](#7-포트-구성)
8. [빌드](#8-빌드)
9. [설정 파일](#9-설정-파일)
10. [로그](#10-로그)
11. [CSC REST API](#11-csc-rest-api)
12. [트러블슈팅](#12-트러블슈팅)

---

## 1. 시스템 구성

```
브라우저
  │
  ├─ HTTP/WS :3000 ──→ csc_client (React Web UI)
  │                         │ /api/v1/* 프록시
  │                         ↓
  │                    csc (Python REST API) :4420 HTTPS
  │                         │ MariaDB
  │
  ├─ WS :8080 ──────→ cwrtc (WebRTC 게이트웨이)
  │    DTLS-SRTP            │ SIP UDP :5062
  │                         ↓
  │                    CSP (SIP 서버) :5060
  │                         │ UDP JSON :9001→9000
  │                         ↓
  │                    CMP (RTP 미디어 서버) :9000
  │
  └─ RTP ←──────────────────┘  (포트 50000~50039)
```

| 컴포넌트 | 역할 | 바이너리 |
|---|---|---|
| **csp** | SIP 호 처리 서버 (등록/발착신/PTT 그룹통화) | `build/bin/csp` |
| **cmp** | RTP 미디어 릴레이 + PTT 발언권 제어 | `build/bin/cmp` |
| **csc** | REST API 서버 (가입자/그룹 관리, 인증) | Python (app.py) |
| **cwrtc** | WebRTC ↔ SIP/RTP 게이트웨이 | `build/bin/cwrtc` |
| **csc_client** | 관리자 Web UI + 소프트폰 | React/Vite |
| **cspsim** | SIP 단말 시뮬레이터 (테스트용) | `build/bin/cspsim` |

---

## 2. 서비스 실행/관리

모든 명령은 프로젝트 루트(`/home/nex/work/cims`)에서 실행합니다.

### 전체 시작/중지

```bash
./cims.sh start       # 전체 시작 (cmp → csp → csc → cwrtc → csc_client 순)
./cims.sh stop        # 전체 중지
./cims.sh restart     # 전체 재시작
./cims.sh status      # 상태 확인
```

### 개별 컴포넌트

```bash
./cims.sh start cmp      # CMP만 시작
./cims.sh start csp      # CSP만 시작
./cims.sh start csc      # REST API 서버만 시작
./cims.sh start cwrtc    # WebRTC 게이트웨이만 시작
./cims.sh start client   # Web UI만 시작

./cims.sh stop  csp      # CSP만 중지
./cims.sh restart cwrtc  # cwrtc만 재시작
```

### 상태 확인

```bash
./cims.sh status
```

출력 예시:
```
=== CIMS 상태 ===
  ● cmp           실행 중  (pid=12345)
  ● csp           실행 중  (pid=12346)
  ● csc           실행 중  (pid=12347)
  ● cwrtc         실행 중  (pid=12348)
  ● client        실행 중  (pid=12349)
```

> **참고:** PID 파일은 `test_run/run/*.pid`에 저장됩니다.
> 기존에 실행 중인 stray 프로세스는 시작 시 자동으로 정리됩니다.

---

## 3. Web UI 사용법

브라우저에서 접속: **http://192.168.199.129:3000**

### 로그인

| 구분 | 이메일 | 비밀번호 |
|---|---|---|
| 관리자 | `jcryu74@gmail.com` | `1234` |
| 일반 사용자 | `test001@cims.co.kr` ~ `test010@cims.co.kr` | `1234` |

### 권한별 화면

| 권한 | 가입자 관리 | PTT 그룹 관리 | 통화현황 | 소프트폰 |
|---|:---:|:---:|:---:|:---:|
| **admin** | ✅ | ✅ | ✅ | ✅ |
| **user** | ❌ | ❌ | ❌ | ✅ |

### 탭별 기능

#### 👤 가입자 관리 (admin)
- 가입자 추가/수정/삭제
- VoLTE Call 번호 등록 (MSISDN, 인증ID, 비밀번호)
- PTT 번호 등록
- 착신 거부 목록 관리

#### 📢 PTT 그룹 관리 (admin)
- 그룹 생성/수정/삭제
- 그룹 멤버 추가/삭제 (발언 우선순위 설정)

#### 📞 통화현황 (admin)
- 실시간 통화 목록 (발신/수신/그룹통화)
- 통화 이력 조회 (날짜/번호/유형 필터)

#### 🔑 비밀번호 변경
- 헤더 우측 🔑 버튼 클릭

#### 로그아웃
- 헤더 우측 「로그아웃」 버튼 클릭
- 세션은 로컬스토리지 JWT로 유지 (7일)

---

## 4. 소프트폰 사용법

### 접속 조건

> ⚠️ WebRTC(마이크)는 **HTTPS** 또는 **localhost** 환경이 필요합니다.
> HTTP에서 마이크를 사용하려면 Chrome에서 다음 설정이 필요합니다:
>
> 1. `chrome://flags/#unsafely-treat-insecure-origin-as-secure` 접속
> 2. `http://192.168.199.129:3000` 추가
> 3. Chrome 재시작

### 로그인 후 자동 연결

1. 로그인하면 소프트폰 탭에서 **자동으로 cwrtc에 연결**됩니다.
2. 할당된 Call/PTT 번호 버튼이 표시됩니다.
3. 번호 버튼을 클릭하면 해당 번호로 전환됩니다.

### VoLTE 일반 통화

1. **📞 Call 번호** 버튼 선택
2. 다이얼패드에서 상대방 번호 입력 (예: `+821357007002`)
3. **📞 통화** 버튼 클릭
4. 상대방이 수신하면 통화 연결
5. **📵 종료** 버튼으로 종료

### 착신 수신

1. 상대방이 전화하면 착신 알림 표시
2. **📞 수신** 또는 **📵 거절** 선택

### PTT 그룹 통화

1. **🎙 PTT 번호** 버튼 선택 (자동으로 cwrtc 등록)
2. PTT 그룹 통화가 착신되면 알림 표시 → **📞 수신**
3. 대형 버튼을 **누르고 있는 동안** 발언 (발언권 획득)
4. 버튼을 **놓으면** 청취 모드
5. **📵 통화 종료** 버튼으로 그룹 통화 종료

---

## 5. 단말 시뮬레이터 (cspsim)

### 기본 사용법

```bash
./cims.sh sim [옵션]
```

### 옵션

| 옵션 | 설명 | 기본값 |
|---|---|---|
| `-mode voip\|ptt` | 단말 유형 | `voip` |
| `-scenario <name>` | 자동 시나리오 | `call` |
| `-count N` | 단말 수 | `2` |
| `-user ID` | 시작 사용자 ID | `1001` |
| `-domain D` | SIP 도메인 | `csp` |
| `-password P` | 인증 비밀번호 | `1234` |
| `-group ID` | PTT 그룹 ID | `1000` |
| `-duration SEC` | 통화 지속 시간(초) | `10` |
| `-ip IP` | CSP 서버 IP | csp.json에서 자동 |

### 시나리오

| 시나리오 | 설명 |
|---|---|
| `register` | SIP 등록만 수행 |
| `subscribe` | 등록 + GMS/CMS 구독 (PTT) |
| `call` | 등록 후 짝끼리 VoLTE 통화 |
| `group-call` | 등록 + 구독 + PTT 그룹 통화 |
| `full` | 등록 → 구독 → 통화 반복 |

### 예시

```bash
# VoLTE 통화 테스트 (1001↔1002)
./cims.sh sim -mode voip -scenario call -count 2 -user 1001

# PTT 그룹통화 테스트 (4단말, 그룹 +82571910001)
./cims.sh sim -mode ptt -scenario group-call -count 4 \
    -user +82571900001 -group +82571910001 -duration 15

# 등록만 (10단말)
./cims.sh sim -mode voip -scenario register -count 10 -user 1001

# 인터랙티브 모드로 직접 실행
cd test_run && ../build/bin/cspsim -server_ip 192.168.199.129 \
    -count 2 -user 1001 -domain csp -password 1234 \
    -mode voip -scenario call -call_duration 30
```

### 인터랙티브 명령 (cspsim 실행 중)

| 키 | 동작 |
|---|---|
| `s` | 통계 출력 |
| `c` | VoLTE 통화 발신 |
| `g` | PTT 그룹 통화 발신 |
| `t` | PTT 발언권 요청 (Talk) |
| `r` | PTT 발언권 해제 (Release) |
| `sub` | GMS/CMS 구독 |
| `q` | 종료 |

---

## 6. 계정 정보

### Web UI 로그인 계정

| 이름 | 이메일 | 비밀번호 | 권한 |
|---|---|---|---|
| 관리자 | `jcryu74@gmail.com` | `1234` | admin |
| 테스트001 | `test001@cims.co.kr` | `1234` | user |
| 테스트002 | `test002@cims.co.kr` | `1234` | user |
| 테스트003 | `test003@cims.co.kr` | `1234` | user |
| 테스트004 | `test004@cims.co.kr` | `1234` | user |
| 테스트005 | `test005@cims.co.kr` | `1234` | user |
| 테스트006 | `test006@cims.co.kr` | `1234` | user |
| 테스트007 | `test007@cims.co.kr` | `1234` | user |
| 테스트008 | `test008@cims.co.kr` | `1234` | user |
| 테스트009 | `test009@cims.co.kr` | `1234` | user |
| 테스트010 | `test010@cims.co.kr` | `1234` | user |

### SIP 번호 (소프트폰/cspsim)

| 이름 | Call 번호 (VoLTE) | PTT 번호 | SIP 도메인 | 비밀번호 |
|---|---|---|---|---|
| 테스트001 | `+821357007001` | `+82571900001` | `csp` | `1234` |
| 테스트002 | `+821357007002` | `+82571900002` | `csp` | `1234` |
| 테스트003 | `+821357007003` | `+82571900003` | `csp` | `1234` |
| 테스트004 | `+821357007004` | `+82571900004` | `csp` | `1234` |
| 테스트005 | `+821357007005` | `+82571900005` | `csp` | `1234` |
| 테스트006 | `+821357007006` | `+82571900006` | `csp` | `1234` |
| 테스트007 | `+821357007007` | `+82571900007` | `csp` | `1234` |
| 테스트008 | `+821357007008` | `+82571900008` | `csp` | `1234` |
| 테스트009 | `+821357007009` | `+82571900009` | `csp` | `1234` |
| 테스트010 | `+821357007010` | `+82571900010` | `csp` | `1234` |

### PTT 그룹

| 그룹 ID | 그룹명 | 멤버 |
|---|---|---|
| `+82571910001` | PTT Group 1 | 테스트001 ~ 테스트005 |
| `+82571910002` | PTT Group 2 | 테스트006 ~ 테스트010 |

---

## 7. 포트 구성

| 포트 | 프로토콜 | 컴포넌트 | 용도 |
|---|---|---|---|
| **3000** | HTTP/WS | csc_client | Web UI (개발 서버) |
| **4420** | HTTPS | csc | REST API |
| **5060** | UDP/TCP | CSP | SIP 신호 (단말 접속) |
| **5062** | UDP | cwrtc | cwrtc SIP (CSP 연결) |
| **8080** | WS | cwrtc | WebRTC WebSocket |
| **9000** | UDP | CMP | 미디어 제어 (CSP→CMP) |
| **9001** | UDP | CSP | 미디어 제어 응답 (CMP→CSP) |
| **25061** | TCP | CSP | SIP TCP |
| **50000~50039** | UDP | CMP | RTP 미디어 포트 |
| **50100~50199** | UDP | cwrtc | DTLS-SRTP 포트 |

---

## 8. 빌드

```bash
./cims.sh build         # C++ 전체 빌드 + Web UI 빌드
./cims.sh build -j 4    # 병렬 4개로 빌드
```

또는 직접:

```bash
# C++ 빌드
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)

# Web UI 빌드
cd csc_client
npm install
npm run build
```

빌드 결과물: `build/bin/` (csp, cmp, cwrtc, cspsim 등)

---

## 9. 설정 파일

| 파일 | 컴포넌트 | 주요 설정 |
|---|---|---|
| `test_run/csp.json` | CSP | SIP IP/포트, CMP 주소, DB 접속, 로그 |
| `test_run/cmp.json` | CMP | RTP 포트 범위, PTT DTMF 키 |
| `test_run/cwrtc.json` | cwrtc | 로컬 IP, WS 포트, SIP 서버, RTP 범위 |
| `csc/bin/csc_pihttp/config/csc.json` | csc | API 서버 포트, DB 접속, 인증서 |
| `test_run/User/*.json` | CSP | 사용자별 SIP 인증 정보 |
| `test_run/Group/*.json` | CSP | PTT 그룹 멤버/우선순위 |

### IP 변경 시

서버 IP가 바뀌면 아래 파일의 IP를 수정합니다:

```bash
# csp.json
"LocalIp": "192.168.199.129"   # 새 IP로 변경

# cwrtc.json
"LocalIp": "192.168.199.129"   # 새 IP로 변경
```

---

## 10. 로그

```bash
./cims.sh log csp       # CSP 로그 실시간
./cims.sh log cmp       # CMP 로그 실시간
./cims.sh log csc       # REST API 로그 실시간
./cims.sh log cwrtc     # WebRTC 게이트웨이 로그 실시간
./cims.sh log client    # Web UI 로그 실시간
```

로그 파일 위치: `test_run/log/`

| 파일 | 내용 |
|---|---|
| `csp.log` | CSP stdout (시작/종료) |
| `cmp.log` | CMP stdout |
| `csc.log` | REST API 서버 로그 |
| `cwrtc.log` | WebRTC 게이트웨이 로그 |
| `client.log` | Web UI 개발 서버 로그 |
| `YYYYMMDD_N.txt` | CSP/cwrtc 통합 로그 (날짜별) |
| `log/cdr/` | CDR (통화 상세 기록) |

---

## 11. CSC REST API

CSC는 Python HTTP 서버(`csc/bin/csc_pihttp/`)로 구현된 REST API를 제공합니다.

- **Base URL:** `https://192.168.199.129:4420`
- **Content-Type:** `application/json` (MCPTT 서비스 API는 XML 별도 명시)
- **인증:** JWT Bearer Token — `Authorization: Bearer <token>`

> **Note:** 관리자 API는 `admin` 권한 계정의 토큰이 필요합니다.

---

### 11-1. 인증 (Auth)

#### `POST /api/v1/auth/login` — 로그인
```http
POST /api/v1/auth/login
Content-Type: application/json

{
  "email": "jcryu74@gmail.com",
  "password": "1234"
}
```
**응답 (200):**
```json
{
  "token": "<JWT>",
  "user": {
    "id": 1,
    "name": "관리자",
    "email": "jcryu74@gmail.com",
    "role": "admin",
    "call_subscriptions": [...],
    "ptt_subscriptions": [...]
  }
}
```

---

#### `POST /api/v1/auth/register` — 계정 생성
```http
POST /api/v1/auth/register
Content-Type: application/json

{
  "name": "홍길동",
  "email": "user@example.com",
  "password": "1234"
}
```
**응답 (201):** 로그인과 동일 구조 (role=user)

---

#### `GET /api/v1/auth/me` — 내 정보 조회
```http
GET /api/v1/auth/me
Authorization: Bearer <token>
```
**응답 (200):** 로그인 응답의 `user` 객체와 동일

---

#### `PUT /api/v1/auth/password` — 비밀번호 변경
```http
PUT /api/v1/auth/password
Authorization: Bearer <token>
Content-Type: application/json

{
  "old_password": "1234",
  "new_password": "newpass"
}
```
**응답 (200):** `{ "ok": true }`

---

### 11-2. 가입자 관리 (Users) — admin

#### `GET /api/v1/users` — 전체 가입자 목록
```http
GET /api/v1/users
Authorization: Bearer <token>
```
**응답 (200):**
```json
{
  "users": [
    {
      "id": 1,
      "name": "테스트001",
      "email": "test001@cims.co.kr",
      "org_id": "ORG001",
      "details": null,
      "reject_id": [],
      "call_subscriptions": [...],
      "ptt_subscriptions": [...],
      "create_time": "2026-01-01T00:00:00",
      "update_time": "2026-01-01T00:00:00"
    }
  ]
}
```

---

#### `POST /api/v1/users` — 가입자 추가
```http
POST /api/v1/users
Authorization: Bearer <token>
Content-Type: application/json

{
  "name": "테스트011",
  "email": "test011@cims.co.kr",
  "org_id": "ORG001",
  "details": null,
  "reject_id": []
}
```
**응답 (201):** `{ "id": 11 }`

---

#### `GET /api/v1/users/{pid}` — 가입자 조회
```http
GET /api/v1/users/1
Authorization: Bearer <token>
```

---

#### `PUT /api/v1/users/{pid}` — 가입자 수정
```http
PUT /api/v1/users/1
Authorization: Bearer <token>
Content-Type: application/json

{
  "name": "수정된 이름",
  "reject_id": ["+821357007005"]
}
```
**응답 (200):** `{ "id": 1 }`

---

#### `DELETE /api/v1/users/{pid}` — 가입자 삭제
```http
DELETE /api/v1/users/1
Authorization: Bearer <token>
```
**응답 (200):** `{ "id": 1 }`

---

### 11-3. Call 구독 관리 — admin

Call 구독 = VoLTE 통화에 사용하는 SIP 번호(MSISDN) 매핑

#### `GET /api/v1/users/{pid}/call` — 구독 목록
```http
GET /api/v1/users/1/call
Authorization: Bearer <token>
```
**응답 (200):**
```json
{
  "subscriptions": [
    {
      "id": "+821357007001",
      "auth_id": "4503811357007001@ims.nex-cims.co.kr",
      "dnd": false,
      "forward_id": "",
      "register_time": "2026-03-24T10:00:00",
      "logout_time": null
    }
  ]
}
```

---

#### `POST /api/v1/users/{pid}/call` — 구독 추가
```http
POST /api/v1/users/1/call
Authorization: Bearer <token>
Content-Type: application/json

{
  "id": "+821357007001",
  "auth_id": "4503811357007001@ims.nex-cims.co.kr",
  "passwd": "1234",
  "dnd": false,
  "forward_id": ""
}
```
**응답 (201):** `{ "id": "+821357007001" }`

---

#### `PUT /api/v1/users/{pid}/call/{msisdn}` — 구독 수정
```http
PUT /api/v1/users/1/call/+821357007001
Authorization: Bearer <token>
Content-Type: application/json

{
  "passwd": "newpass",
  "dnd": true,
  "forward_id": "+821357007002"
}
```
**응답 (200):** `{ "id": "+821357007001" }`

---

#### `DELETE /api/v1/users/{pid}/call/{msisdn}` — 구독 삭제
```http
DELETE /api/v1/users/1/call/+821357007001
Authorization: Bearer <token>
```
**응답 (200):** `{ "id": "+821357007001" }`

---

### 11-4. PTT 구독 관리 — admin

PTT 구독 = PTT 통화에 사용하는 SIP 번호 매핑.
엔드포인트 구조는 Call 구독과 동일하며 경로만 `/ptt`로 변경.

| 메서드 | 경로 | 설명 |
|---|---|---|
| GET | `/api/v1/users/{pid}/ptt` | PTT 구독 목록 |
| POST | `/api/v1/users/{pid}/ptt` | PTT 구독 추가 |
| PUT | `/api/v1/users/{pid}/ptt/{msisdn}` | PTT 구독 수정 |
| DELETE | `/api/v1/users/{pid}/ptt/{msisdn}` | PTT 구독 삭제 |

요청/응답 형식은 Call 구독과 동일합니다.

---

### 11-5. PTT 그룹 관리 — admin

#### `GET /api/v1/ptt/groups` — 전체 그룹 목록
```http
GET /api/v1/ptt/groups
Authorization: Bearer <token>
```
**응답 (200):**
```json
{
  "groups": [
    {
      "id": "+82571910001",
      "name": "PTT Group 1",
      "members": [
        { "user_id": "+82571900001", "priority": 5 },
        { "user_id": "+82571900002", "priority": 3 }
      ]
    }
  ]
}
```

---

#### `POST /api/v1/ptt/groups` — 그룹 생성
```http
POST /api/v1/ptt/groups
Authorization: Bearer <token>
Content-Type: application/json

{
  "id": "+82571910003",
  "name": "PTT Group 3",
  "members": [
    { "user_id": "+82571900001", "priority": 5 },
    { "user_id": "+82571900003", "priority": 3 }
  ]
}
```
**응답 (201):** `{ "id": "+82571910003" }`

---

#### `GET /api/v1/ptt/groups/{id}` — 그룹 조회
```http
GET /api/v1/ptt/groups/+82571910001
Authorization: Bearer <token>
```

---

#### `PUT /api/v1/ptt/groups/{id}` — 그룹 수정
```http
PUT /api/v1/ptt/groups/+82571910001
Authorization: Bearer <token>
Content-Type: application/json

{
  "name": "변경된 그룹명",
  "members": [
    { "user_id": "+82571900001", "priority": 10 }
  ]
}
```
**응답 (200):** `{ "id": "+82571910001" }`

---

#### `DELETE /api/v1/ptt/groups/{id}` — 그룹 삭제
```http
DELETE /api/v1/ptt/groups/+82571910001
Authorization: Bearer <token>
```
**응답 (200):** `{ "id": "+82571910001" }`

---

#### `GET /api/v1/ptt/groups/{id}/members` — 멤버 목록 조회
```http
GET /api/v1/ptt/groups/+82571910001/members
Authorization: Bearer <token>
```
**응답 (200):**
```json
{
  "group_id": "+82571910001",
  "members": [
    { "user_id": "+82571900001", "priority": 5 }
  ]
}
```

---

#### `POST /api/v1/ptt/groups/{id}/members` — 멤버 추가
```http
POST /api/v1/ptt/groups/+82571910001/members
Authorization: Bearer <token>
Content-Type: application/json

{
  "user_id": "+82571900006",
  "priority": 2
}
```
**응답 (201):** `{ "group_id": "+82571910001", "user_id": "+82571900006" }`

---

#### `DELETE /api/v1/ptt/groups/{id}/members/{uid}` — 멤버 삭제
```http
DELETE /api/v1/ptt/groups/+82571910001/members/+82571900006
Authorization: Bearer <token>
```
**응답 (200):** `{ "group_id": "+82571910001", "user_id": "+82571900006" }`

---

### 11-6. 통화 이력 (Call Logs) — admin

#### `GET /api/v1/call/logs` — 통화 이력 조회

| 쿼리 파라미터 | 타입 | 설명 |
|---|---|---|
| `state` | string | `ringing` \| `active` \| `ended` |
| `caller` | string | 발신 MSISDN |
| `callee` | string | 착신 MSISDN |
| `msisdn` | string | 발신 또는 착신 MSISDN (OR 검색) |
| `group_id` | string | PTT 그룹 ID |
| `call_type` | string | `voip` \| `ptt` |
| `from_dt` | string | 시작일 `YYYY-MM-DD` |
| `to_dt` | string | 종료일 `YYYY-MM-DD` |
| `limit` | int | 최대 건수 (기본: 200, 최대: 1000) |
| `offset` | int | 오프셋 (페이징, 기본: 0) |

```http
GET /api/v1/call/logs?call_type=ptt&from_dt=2026-03-01&limit=50
Authorization: Bearer <token>
```
**응답 (200):**
```json
{
  "total": 42,
  "limit": 50,
  "offset": 0,
  "logs": [
    {
      "id": 1,
      "call_id": "abc123@192.168.199.129",
      "call_type": "ptt",
      "group_id": "+82571910001",
      "initiator": "+82571900001",
      "callee": "+82571910001",
      "state": "ended",
      "invite_time": "2026-03-24T10:00:00",
      "answer_time": "2026-03-24T10:00:01",
      "end_time": "2026-03-24T10:05:00",
      "duration": 299,
      "sip_status": 200,
      "end_reason": "normal",
      "end_reason_ko": "정상 종료",
      "participants": [
        {
          "msisdn": "+82571900001",
          "role": "caller",
          "join_time": "2026-03-24T10:00:00",
          "leave_time": "2026-03-24T10:05:00"
        }
      ]
    }
  ]
}
```

---

#### `GET /api/v1/call/logs/active` — 현재 진행 중인 통화
```http
GET /api/v1/call/logs/active
Authorization: Bearer <token>
```
`state=ringing 또는 active`인 통화만 반환. 응답 형식은 `/call/logs`와 동일.

---

### 11-7. MCPTT 서비스 API (3GPP IdMS/GMS/CMS)

IMS/MCPTT 단말(cspsim, 상용 PTT 단말)이 사용하는 표준 3GPP 인터페이스입니다.

#### IdMS — OAuth 2.0 (PKCE)

| 메서드 | 경로 | 설명 |
|---|---|---|
| GET | `/idms/authreq` | Authorization Code 요청 (PKCE) |
| POST | `/idms/tokenreq` | Access/Refresh Token 발급 |

```http
# 1. Authorization Code 요청
GET /idms/authreq?user_name=+82571900001
    &user_password=1234
    &client_id=MCPTT_UE
    &redirect_uri=mcptt://callback
    &code_challenge=<S256_hash>
    &code_challenge_method=S256

# 2. Token 발급
POST /idms/tokenreq
Content-Type: application/json

{
  "grant_type": "authorization_code",
  "code": "<authorization_code>",
  "code_verifier": "<PKCE_verifier>",
  "client_id": "MCPTT_UE",
  "redirect_uri": "mcptt://callback"
}
```

---

#### GMS — 그룹 정보 (OMA POC Groups XML)

| 메서드 | 경로 | 설명 |
|---|---|---|
| GET | `/org.openmobilealliance.groups/users/{user_id}/{group_id}` | 그룹 XML 조회 |
| PUT | `/org.openmobilealliance.groups/users/{user_id}/{group_id}` | 그룹 XML 갱신 |
| DELETE | `/org.openmobilealliance.groups/users/{user_id}/{group_id}` | 그룹 삭제 |

응답: `application/vnd.oma.poc.groups+xml` + `Etag` 헤더

---

#### CMS/Service Config — 사용자 프로파일 및 서비스 설정

| 메서드 | 경로 | 설명 |
|---|---|---|
| GET | `/org.3gpp.mcptt.user-profile/users/{user_id}/user-profile` | MCPTT 사용자 프로파일 XML |
| GET | `/org.3gpp.mcptt.service-config/users/{user_id}/service-config` | MCPTT 서비스 설정 XML |

응답: `application/vnd.3gpp.mcptt-user-profile+xml` / `application/vnd.3gpp.mcptt-service-config+xml`

---

#### KMS — 키 관리

| 메서드 | 경로 | 설명 |
|---|---|---|
| GET | `/keymanagement/identity/v1/init` | KMS 초기화 |
| GET | `/keymanagement/identity/v1/keyprov` | 키 프로비저닝 |

---

### 11-8. API 빠른 참조

```
# 로그인
POST   /api/v1/auth/login
POST   /api/v1/auth/register
GET    /api/v1/auth/me
PUT    /api/v1/auth/password

# 가입자
GET    /api/v1/users
POST   /api/v1/users
GET    /api/v1/users/{pid}
PUT    /api/v1/users/{pid}
DELETE /api/v1/users/{pid}

# Call 구독 (VoLTE 번호)
GET    /api/v1/users/{pid}/call
POST   /api/v1/users/{pid}/call
PUT    /api/v1/users/{pid}/call/{msisdn}
DELETE /api/v1/users/{pid}/call/{msisdn}

# PTT 구독 (PTT 번호)
GET    /api/v1/users/{pid}/ptt
POST   /api/v1/users/{pid}/ptt
PUT    /api/v1/users/{pid}/ptt/{msisdn}
DELETE /api/v1/users/{pid}/ptt/{msisdn}

# PTT 그룹
GET    /api/v1/ptt/groups
POST   /api/v1/ptt/groups
GET    /api/v1/ptt/groups/{id}
PUT    /api/v1/ptt/groups/{id}
DELETE /api/v1/ptt/groups/{id}
GET    /api/v1/ptt/groups/{id}/members
POST   /api/v1/ptt/groups/{id}/members
DELETE /api/v1/ptt/groups/{id}/members/{uid}

# 통화 이력
GET    /api/v1/call/logs
GET    /api/v1/call/logs/active

# MCPTT (IdMS/GMS/CMS/KMS)
GET    /idms/authreq
POST   /idms/tokenreq
GET|PUT|DELETE /org.openmobilealliance.groups/users/{user_id}/{group_id}
GET    /org.3gpp.mcptt.user-profile/users/{user_id}/user-profile
GET    /org.3gpp.mcptt.service-config/users/{user_id}/service-config
GET    /keymanagement/identity/v1/init
GET    /keymanagement/identity/v1/keyprov
```

---

## 12. 트러블슈팅

### 서비스가 시작되지 않을 때

```bash
./cims.sh log csp    # 오류 메시지 확인
./cims.sh status     # 프로세스 상태 확인
```

### 포트 충돌

```bash
ss -tlnp | grep -E "5060|8080|3000|4420|9000"
```

### CMP RTP 포트 오류 (`Failed to init resource`)

이미 실행 중인 CMP 프로세스가 있을 수 있습니다:

```bash
./cims.sh stop cmp && ./cims.sh start cmp
```

### cwrtc 마이크 접근 실패 (Web UI)

Chrome 설정 필요:

1. `chrome://flags/#unsafely-treat-insecure-origin-as-secure` 접속
2. `http://192.168.199.129:3000` 추가
3. **Relaunch** 클릭

### DB 연결 실패

```bash
mysql -u cims -pcims1234 cims -e "SHOW TABLES;"
```

접속 안 되면 MariaDB 서비스 확인:

```bash
systemctl status mariadb
sudo systemctl start mariadb
```

### cspsim 등록 실패 (403)

CSP의 `test_run/User/` 폴더에 해당 사용자 JSON 파일이 없는 경우입니다.
가입자 관리 Web UI에서 번호를 먼저 등록하거나 JSON 파일을 확인하세요.

---

*최종 업데이트: 2026-03-24 (CSC REST API 문서 추가)*
