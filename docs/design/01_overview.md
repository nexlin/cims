# CIMS 시스템 아키텍처 및 메시지 Flow

> 이 문서는 모듈 간 전체 그림과 주요 메시지 흐름을 다룹니다.
> **분산 배포 측면** (Agent/Package/Deployment 모델, 설정 템플릿, jsonl 런타임 설정)
> 은 `02_deployment.md` 및 `features/sip_runtime_config.md` 를 참조하세요.

## 1. 시스템 구성

CIMS는 6개 컴포넌트로 구성된 MCPTT/VoIP 서버 시스템입니다.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                            Client                                       │
│                                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                               │
│  │  Phone   │  │ Console  │  │ VoLTE UE │                               │
│  │ (Web UI) │  │ (Web UI) │  │ (SIP UE) │                               │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘                               │
│       │WS/DTLS      │HTTPS        │SIP/RTP                              │
└───────┼─────────────┼─────────────┼─────────────────────────────────────┘
        │             │             │
┌───────┼─────────────┼─────────────┼─────────────────────────────────────┐
│       ▼             ▼             ▼              Server                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐                 │
│  │  cwrtc   │  │   CSC    │  │   CSP    │  │   CMP    │                 │
│  │ WebRTC GW│  │  HTTP API│  │ CSCF/TAS │  │   MRF    │                 │
│  │          │  │ IdMS/GMS │  │ PTT-AS   │  │(Call/PTT)│                 │
│  └────┬─────┘  └──────────┘  └────┬─────┘  └────┬─────┘                 │
│       │                           │              │                      │
│       │      SIP (UDP 5060)       │   UDP JSON   │                      │
│       └───────────────────────────┘──────────────┘                      │
│                                                                         │
│                         ┌──────────┐                                    │
│                         │  MySQL   │                                    │
│                         │   DB     │                                    │
│                         └──────────┘                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 컴포넌트 설명

### 2.1 CSP (Call Service Platform)

**역할:** SIP 시그널링 서버. 등록, 1:1 통화, PTT 그룹 통화 제어.

| 항목 | 값 |
|------|-----|
| 바이너리 | `bin/csp` |
| 설정 | `config/csp.json` |
| SIP 포트 | UDP 5060, TCP 25061, TLS 5061 |
| CMP 연동 | UDP JSON (포트 9000 → CMP, 포트 9001 ← CMP 응답) |

**csp.json 설정 파라미터:**

| 파라미터 | 타입 | 기본값 | 설명 |
|---------|------|--------|------|
| `LocalIp` | string | "0.0.0.0" | SIP 바인드 IP |
| `LocalPort` | int | 5060 | SIP UDP 포트 |
| `AuthRealm` | string | "csp" | SIP Digest 인증 realm (서비스 도메인과 독립) |
| `Realm` | array | — | 서비스별 도메인 배열 `[{"service":"volte"/"mcptt"/"system"/"console","domains":["..."]}]` |
| `CmpIp` | string | "127.0.0.1" | CMP 서버 IP |
| `CmpPort` | int | 9000 | CMP 제어 포트 |
| `LocalCmpPort` | int | 9001 | CSP의 CMP 응답 수신 포트 |
| `DataFolder` | string | "../csp" | User/Group JSON 폴더 경로 |
| `Database.Host` | string | "127.0.0.1" | MySQL 호스트 |
| `Database.Port` | int | 3306 | MySQL 포트 |
| `Database.User` | string | "cims" | MySQL 사용자 |
| `Database.Password` | string | "cims1234" | MySQL 비밀번호 |
| `Database.Db` | string | "cims" | MySQL 데이터베이스 이름 |

**예시:**
```json
{
  "LocalIp": "192.168.0.2",
  "LocalPort": 5060,
  "AuthRealm": "csp",
  "Realm": [
    { "service": "volte",  "domains": ["ims.mnc001.mcc450.3gppnetwork.org"] },
    { "service": "mcptt",  "domains": ["ptt.mnc001.mcc450.3gppnetwork.org"] },
    { "service": "system", "domains": ["csp"] }
  ],
  "CmpIp": "192.168.0.2",
  "CmpPort": 9000,
  "LocalCmpPort": 9001,
  "DataFolder": "../csp",
  "Database": {
    "Host": "127.0.0.1",
    "Port": 3306,
    "User": "cims",
    "Password": "cims1234",
    "Db": "cims"
  }
}
```

**핵심 클래스 (모듈러 IMS — 단일 프로세스에 CSCF/TAS/PTT-AS/IBCF 역할을 설정 기반으로 활성화):**
- `CModuleDispatcher` — 중앙 디스패처. 모든 SIP 이벤트를 콜 소유권 기반으로 모듈에 라우팅 (`ModuleDispatcher.h/.cpp`)
- `CCscfModule` — REGISTER / SUBSCRIBE / 인증 (Digest MD5)
- `CTasModule` — VoIP B2BUA: DND, 착신전환, 착신거부, 콜픽업
- `CPttAsModule` — PTT 그룹콜 (`CGroupCallService` 래핑)
- `CIbcfModule` — IP-PBX 트렁크 라우팅 (옵션)
- `CGroupCallService` — PTT 그룹 통화 (멤버 초대, 공유 RTP, multipart INVITE: SDP + OMA POC XML)
- `CCmpClient` — CMP에 RTP 세션 생성/삭제 (UDP JSON, 동기 요청/응답)
- `CSubscriptionManager` — SUBSCRIBE/NOTIFY 상태 (GMS/CMS)
- `CspUserMap` / `CGroupMap` — 가입자/그룹 캐시 (DB primary, JSON fallback)
- `CCallDir` — Session-ID 기반 서비스 로깅 (call.json, participants.jsonl, session.json)
- `SipMessageLogger` — psip 콜백 구현 (SIP TX/RX + CMP JSON → `sip.jsonl`)

자세한 모듈 구조와 콜백 순서 (B2BUA 라우팅) 는 `modules/csp.md` 참조.

### 2.2 CMP (Component Media Provider)

**역할:** RTP 미디어 릴레이 및 PTT Floor 제어.

| 항목 | 값 |
|------|-----|
| 바이너리 | `bin/cmp` |
| 설정 | `config/cmp.json` |
| 제어 포트 | UDP 9000 |
| RTP 포트 풀 | `RtpStartPort` ~ `RtpStartPort + (RtpPoolSize * 4) - 1` |

**cmp.json 설정 파라미터:**

| 파라미터 | 타입 | 기본값 | 설명 |
|---------|------|--------|------|
| `ServerIp` | string | "0.0.0.0" | 제어 포트 바인드 IP |
| `ServerPort` | int | 9000 | UDP JSON 제어 포트 |
| `RtpIp` | string | "0.0.0.0" | RTP 바인드 IP (외부 공개 IP) |
| `RtpStartPort` | int | 50000 | RTP 포트 풀 시작 |
| `RtpPoolSize` | int | 20 | 동시 세션 수 (세션당 4포트) |
| `EnableDtmfPtt` | boolean | false | DTMF 기반 PTT 활성화 |
| `DtmfPushDigit` | string | "*" | PTT Push DTMF 키 |
| `DtmfReleaseDigit` | string | "#" | PTT Release DTMF 키 |
| `LogDir` | string | "" | 로그 파일 디렉토리 (빈 문자열=stdout) |
| `LogLevel` | string | "INFO" | 로그 레벨 (DEBUG/INFO/WARN/ERROR) |
| `LogMaxSizeMB` | int | 10 | 로그 파일 최대 크기 (MB) |
| `LogMaxFiles` | int | 5 | 로그 파일 최대 개수 (로테이션) |

**예시:**
```json
{
  "ServerIp": "0.0.0.0",
  "ServerPort": 9000,
  "RtpIp": "192.168.0.2",
  "RtpStartPort": 50000,
  "RtpPoolSize": 20,
  "EnableDtmfPtt": false,
  "DtmfPushDigit": "*",
  "DtmfReleaseDigit": "#",
  "LogDir": "../log",
  "LogLevel": "INFO",
  "LogMaxSizeMB": 10,
  "LogMaxFiles": 5
}
```

**핵심 클래스:**
- `CmpServer` — UDP JSON 명령 수신 및 디스패치, 리소스 풀 관리
- `PRtpTrans` — RTP/RTCP 소켓 관리 (세션당 4포트: Audio RTP/RTCP + Video RTP/RTCP)
- `McpttGroup` — 그룹 RTP 멀티캐스트, RTCP APP Floor 제어, SSRC/시퀀스 번호 재작성

**리소스 풀 및 포트 계산:**
```
포트 계산 공식:
  세션 N의 시작 포트 = RtpStartPort + (N * 4)

세션당 4포트 할당:
  Audio RTP  = 시작포트 + 0
  Audio RTCP = 시작포트 + 1
  Video RTP  = 시작포트 + 2
  Video RTCP = 시작포트 + 3

예시 (RtpStartPort=50000, RtpPoolSize=20):
  세션  0: Audio RTP=50000, RTCP=50001, Video RTP=50002, RTCP=50003
  세션  1: Audio RTP=50004, RTCP=50005, Video RTP=50006, RTCP=50007
  세션  2: Audio RTP=50008, RTCP=50009, Video RTP=50010, RTCP=50011
  ...
  세션 19: Audio RTP=50076, RTCP=50077, Video RTP=50078, RTCP=50079

총 포트 범위: 50000 ~ 50079 (20 세션 * 4 포트 = 80 포트)
최대 동시 세션: RtpPoolSize (1:1 통화 + 그룹 통화 공유)
```

> 그룹 통화는 공유 세션 1개를 사용합니다. 즉 5개 멤버 그룹 통화도 리소스 풀에서 1개만 할당됩니다.

**CSP→CMP JSON 프로토콜 상세:**

모든 요청은 JSON 래퍼로 감싸집니다:
```json
{
  "trans_id": <순차 정수>,
  "payload": { <명령별 JSON + service + sesid + caller + callee> }
}
```

> **Flow/sesid 메타 필드**: 모든 CSP↔CMP payload 는 `service` (volte/mcptt/system/console), `sesid` (`{caller}::{module}::{ts}::{counter}`), `caller`, `callee` 를 포함합니다. CMP 는 이 값을 저장해 응답 Flow 로그에 상속하여 세션 상관관계를 유지합니다. 전체 규격은 [../features/flow_logging.md](./../features/flow_logging.md) 참고.

모든 응답:
```json
{
  "trans_id": <요청과 동일>,
  "response": "<문자열 또는 JSON 객체>"
}
```

#### `add` — 1:1 RTP 세션 생성

**요청:**
```json
{
  "trans_id": 1,
  "payload": {
    "cmd": "add",
    "session_id": "inv-20260331-001@192.168.0.2",
    "remote_ip": "0.0.0.0",
    "remote_port": 0,
    "remote_video_port": 0,
    "peer_index": 0,
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "inv-20260331-001@192.168.0.2",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

**성공 응답:**
```json
{
  "trans_id": 1,
  "response": "{\"status\":\"OK\",\"local_ip\":\"192.168.0.2\",\"local_port\":50000,\"local_video_port\":50002}"
}
```

**실패 응답 (리소스 부족):**
```json
{
  "trans_id": 1,
  "response": "ERROR No Resource"
}
```

> `add` 명령을 동일 session_id로 다시 보내면 기존 세션의 remote 정보를 업데이트합니다 (modify 역할).

#### `remove` — RTP 세션 삭제

**요청:**
```json
{
  "trans_id": 2,
  "payload": {
    "cmd": "remove",
    "session_id": "inv-20260331-001@192.168.0.2",
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "inv-20260331-001@192.168.0.2",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

**응답:**
```json
{
  "trans_id": 2,
  "response": "OK"
}
```

#### `addgroup` — PTT 그룹 생성 및 공유 RTP 세션 할당

**요청:**
```json
{
  "trans_id": 3,
  "payload": {
    "cmd": "addgroup",
    "group_id": "+82571910001",
    "count": 3,
    "members": "+82571900001:0,+821030432632:0,+82571900002:1",
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "0",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

| 필드 | 설명 |
|------|------|
| `group_id` | 그룹 MSISDN |
| `count` | 멤버 수 |
| `members` | `"id1:priority1,id2:priority2,..."` 형식 문자열 |

**성공 응답:**
```json
{
  "trans_id": 3,
  "response": "{\"status\":\"OK\",\"ip\":\"192.168.0.2\",\"port\":50000,\"video_port\":50002}"
}
```

> 동일 group_id로 다시 `addgroup`을 보내면 기존 그룹의 우선순위를 업데이트합니다 (`modifygroup`과 동일 동작).

#### `joingroup` — 멤버를 그룹에 추가

**요청:**
```json
{
  "trans_id": 4,
  "payload": {
    "cmd": "joingroup",
    "group_id": "+82571910001",
    "session_id": "+82571900001",
    "user_ip": "192.168.0.100",
    "user_port": 40000,
    "user_video_port": 40002,
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "+82571900001",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

| 필드 | 설명 |
|------|------|
| `session_id` | 멤버 식별자 (PTT MSISDN) |
| `user_ip` | 멤버의 RTP 수신 IP |
| `user_port` | 멤버의 Audio RTP 포트 |
| `user_video_port` | 멤버의 Video RTP 포트 (0이면 음성만) |

**성공 응답:**
```json
{
  "trans_id": 4,
  "response": "OK"
}
```

**실패 응답:**
```json
{
  "trans_id": 4,
  "response": "ERROR Group Not Found"
}
```

#### `leavegroup` — 멤버를 그룹에서 제거

**요청:**
```json
{
  "trans_id": 5,
  "payload": {
    "cmd": "leavegroup",
    "group_id": "+82571910001",
    "session_id": "+82571900001",
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "+82571900001",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

**응답:**
```json
{
  "trans_id": 5,
  "response": "OK"
}
```

> 현재 발언권 소유자가 나가면 자동으로 FLOOR_IDLE이 모든 멤버에게 브로드캐스트됩니다.

#### `removegroup` — 그룹 삭제

**요청:**
```json
{
  "trans_id": 6,
  "payload": {
    "cmd": "removegroup",
    "group_id": "+82571910001",
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "0",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

**응답:**
```json
{
  "trans_id": 6,
  "response": "OK"
}
```

#### `modifygroup` — 그룹 멤버/우선순위 변경

내부적으로 `addgroup`과 동일하게 처리됩니다.

**요청:**
```json
{
  "trans_id": 7,
  "payload": {
    "cmd": "modifygroup",
    "group_id": "+82571910001",
    "members": "+82571900001:0,+821030432632:1,+82571900002:2",
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "0",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

**응답:**
```json
{
  "trans_id": 7,
  "response": "{\"status\":\"OK\",\"ip\":\"192.168.0.2\",\"port\":50000,\"video_port\":50002}"
}
```

#### `alive` — 연결 확인 (Keep-Alive)

CSP가 3초 간격으로 전송합니다.

**요청:**
```json
{
  "trans_id": 100,
  "payload": {
    "cmd": "alive",
    "csp_id": "CSP_MAIN",
    "csp_sess_id": "0",
    "cmp_id": "CMP_MAIN",
    "cmp_sess_id": "0"
  }
}
```

**응답:**
```json
{
  "trans_id": 100,
  "response": "OK"
}
```

> CSP는 alive 응답 실패 시 CMP 연결 끊김으로 판단하고, 재연결 시 `m_fnConnectionCallback`을 호출합니다.

### 2.3 McpttGroup 상세 동작

**SSRC 할당:**
- CMP는 `joingroup` 시 각 멤버에게 순차적 SSRC를 할당합니다 (시작값 1000부터).
- 멤버의 `audioSsrcOut` = 1000 + 할당 카운터 (수신자에게 보내는 고정 SSRC)
- 멤버의 `videoSsrcOut` = 2000 + 할당 카운터
- 이렇게 하면 수신자는 항상 동일한 SSRC로 RTP를 받아 디코더 초기화가 안정적입니다.

**시퀀스 번호 재작성:**
- CMP는 각 수신자별로 독립적인 시퀀스 카운터(`audioSeqOut`, `videoSeqOut`)를 유지합니다.
- 송신자의 RTP를 수신자에게 전달할 때 SSRC와 시퀀스 번호를 수신자 전용 값으로 교체합니다:
```
[원본 RTP] seq=1234, ssrc=0xABCDEF  →  [멤버 B용] seq=42, ssrc=1005
                                     →  [멤버 C용] seq=87, ssrc=1006
```

**Floor 상태 머신:**
```
          ┌─────── FLOOR_REQUEST(floor free) ──────────┐
          │                                             ▼
     ┌────────┐                                   ┌──────────┐
     │  IDLE  │◄── FLOOR_RELEASE / Owner Left ────│  TAKEN   │
     │        │                                   │ (화자점유) │
     └────────┘                                   └──────────┘
          │                                             ▲
          │  FLOOR_REQUEST                              │
          │  (floor busy)                               │
          │                                             │
          ├─ requester prio < owner prio ───────────────┤
          │   → REVOKE(owner) → GRANT(requester)        │
          │   → TAKEN broadcast                         │
          │                                             │
          └─ requester prio >= owner prio               │
              → REJECT(requester)                       │
              └─────────────────────────────────────────┘
```

**Port Latching:**
- CMP는 멤버의 실제 RTP 소스 포트가 `joingroup`에서 등록한 포트와 다를 수 있음을 고려합니다.
- IP가 일치하고 해당 IP를 가진 멤버가 1명뿐이면 포트를 자동 업데이트합니다.
- 이 동작은 NAT 뒤에 있는 클라이언트를 지원합니다.

### 2.4 CSC (CIMS Service Controller)

**역할:** HTTP REST API 서버. 인증, 가입자 관리, MCPTT 서비스(IdMS/GMS/CMS/KMS).

| 항목 | 값 |
|------|-----|
| 바이너리 | `python3 app.py` |
| 설정 | `config/csc.json` |
| Admin API 포트 | HTTPS 4420 |
| MCPTT API 포트 | HTTPS 4430 |

**서비스 구성:**

| 포트 | 경로 | 서비스 |
|------|------|--------|
| 4420 | `/api/v1/auth/*` | 사용자 인증 (JWT) |
| 4420 | `/api/v1/users/*` | 가입자/구독 관리 |
| 4420 | `/api/v1/ptt/groups/*` | PTT 그룹 관리 |
| 4430 | `/idms/*` | MCPTT IdMS (OAuth2 PKCE) |
| 4430 | `/org.openmobilealliance.groups/*` | GMS (그룹 관리) |
| 4430 | `/org.3gpp.mcptt.user-profile/*` | CMS (사용자 프로파일) |
| 4430 | `/org.3gpp.mcptt.service-config/*` | CMS (서비스 설정) |
| 4430 | `/keymanagement/*` | KMS (키 관리) |

### 2.5 cwrtc (WebRTC-SIP Bridge) — 웹 테스트 단말

> **재설계 예정** — 현재 빌드(CMake)·dist·패키징·기동 대상에서 제외되어 있다
> (소스는 `cwrtc/` 에 보존). 아래는 재설계 참조용 기존 설계.
>
> cwrtc는 실제 MCPTT 단말이 없는 환경에서 브라우저로 시스템을 테스트하기 위한 보조 프로그램입니다.
> 상세 구조 및 프로토콜은 **부록 A**에서 설명합니다.

| 항목 | 값 |
|------|-----|
| 바이너리 | `bin/cwrtc` |
| 설정 | `config/cwrtc.json` |
| HTTP/WS 포트 | 8080 |
| 역할 | 브라우저 WebSocket ↔ SIP/RTP 프로토콜 변환 |

### 2.6 Phone / Console (웹 테스트 앱)

> Phone/Console 역시 cwrtc와 함께 브라우저에서 시스템을 테스트하기 위한 웹 앱입니다.

### 2.7 Phone (웹 단말 앱)

> **재설계 예정** — cwrtc 와 함께 빌드/패키징/기동 대상에서 제외 (소스는 `cims-phone/` 에 보존).

**역할:** 브라우저 기반 VoIP/PTT 클라이언트.

| 항목 | 값 |
|------|-----|
| 프레임워크 | React + TypeScript + Vite |
| 배포 경로 | `dist/phone/dist/` |
| WebSocket | `wss://<host>/cwrtc` |

### 2.8 Console (관리 콘솔)

**역할:** 브라우저 기반 관리자 도구.

| 항목 | 값 |
|------|-----|
| 프레임워크 | React + TypeScript + Vite |
| 배포 경로 | `dist/console/dist/` |
| API | `https://<host>/api/v1` |

---

## 3. 메시지 Flow 상세

### 3.1 사용자 등록 및 로그인

```
브라우저                   CSC(4420)                    DB
  │                         │                           │
  │── POST /auth/login ────►│                           │
  │   {                     │                           │
  │    "login_id":"admin",  │── SELECT * FROM users ───►│
  │    "password":"1234"    │   WHERE login_id='admin'  │
  │   }                     │◄── user row ──────────────│
  │                         │   SHA256("1234") 비교      │
  │                         │                           │
  │                         │── SELECT * FROM           │
  │                         │   voip_subscriptions ────►│
  │                         │   WHERE user_id=33        │
  │                         │◄── subscription rows ─────│
  │                         │                           │
  │                         │── SELECT * FROM           │
  │                         │   ptt_subscriptions ─────►│
  │                         │   WHERE user_id=33        │
  │                         │◄── subscription rows ─────│
  │                         │                           │
  │◄── 200 {token, user} ──│   JWT 생성 (HS256)         │
  │                         │                           │
```

### 3.2 Phone App 연결 및 PTT 그룹 참가

```
브라우저          CSC(4430)        cwrtc(8080)       CSP(5060)        CMP(9000)
  │                │                 │                 │                 │
  │ [1] MCPTT 인증                    │                 │                 │
  │── GET /idms/authreq ──►│         │                 │                 │
  │  ?user_name=tel:+82..  │         │                 │                 │
  │  &code_challenge=...   │         │                 │                 │
  │◄── {"code":"SplxlO.."} │         │                 │                 │
  │                         │         │                 │                 │
  │── POST /idms/tokenreq ►│         │                 │                 │
  │  code=SplxlO..          │         │                 │                 │
  │  code_verifier=dBjf..   │         │                 │                 │
  │◄── {"access_token":..}  │         │                 │                 │
  │                         │         │                 │                 │
  │ [2] 그룹 목록 조회                  │                 │                 │
  │── GET /gms/users/{me} ►│         │                 │                 │
  │  Authorization: Bearer  │         │                 │                 │
  │◄── [{uri,display_name}] │         │                 │                 │
  │                         │         │                 │                 │
  │ [3] WebSocket + SIP 등록            │                 │                 │
  │── WS connect ───────────────────►│                 │                 │
  │── {"type":"register",   ────────►│                 │                 │
  │    "user":"+82571900001",        │                 │                 │
  │    "password":"123456",          │                 │                 │
  │    "domain":"csp"}               │                 │                 │
  │                         │         │── REGISTER ────►│                 │
  │                         │         │◄── 401 ─────────│                 │
  │                         │         │── REGISTER ────►│                 │
  │                         │         │  (Digest auth)  │                 │
  │                         │         │◄── 200 OK ──────│                 │
  │◄── {"type":"registered",────────│                 │                 │
  │     "user":"+8257.."}            │                 │                 │
  │                         │         │                 │                 │
  │ [4] PTT 그룹 통화 시작               │                 │                 │
  │── {"type":"call",        ───────►│                 │                 │
  │    "to":"+82571910001",          │                 │                 │
  │    "sdp":"v=0\r\n.."}            │                 │                 │
  │                         │         │── INVITE grp ──►│                 │
  │                         │         │   sip:+825..    │                 │
  │                         │         │                 │── addGroup ────►│
  │                         │         │                 │  {"cmd":"addgroup",
  │                         │         │                 │   "group_id":"...",
  │                         │         │                 │   "members":".."}
  │                         │         │                 │◄── OK(port) ────│
  │                         │         │                 │  {"ip":"192..",  │
  │                         │         │                 │   "port":50000}  │
  │                         │         │◄── 200 OK ──────│                 │
  │◄── {"type":"answered"}  ────────│                 │                 │
  │                         │         │                 │                 │
  │ [5] CSP가 그룹 멤버에게 INVITE        │                 │                 │
  │                         │         │                 │                 │
  │                    cwrtc-B◄─ INVITE(multipart) ────│                 │
  │                         │         │  SDP + mcptt XML│                 │
  │                    cwrtc-B─── 200 OK(SDP) ────────►│                 │
  │                         │         │                 │── joinGroup ───►│
  │                         │         │                 │  {"session_id":  │
  │                         │         │                 │   "+8257..",     │
  │                         │         │                 │   "user_ip":"..",│
  │                         │         │                 │   "user_port":..}│
  │                         │         │                 │◄── OK ──────────│
  │                         │         │                 │                 │
  │ [6] 멤버 브라우저에 알림              │                 │                 │
  │         브라우저-B◄── {"type":"ptt_auto_answer", ───│cwrtc-B│         │
  │                        "call_id":"grp-..",                  │         │
  │                        "from":"+82571910001",               │         │
  │                        "group_id":"+82571910001",            │         │
  │                        "sdp":"v=0\r\n..."}                  │         │
```

### 3.3 PTT Floor 제어 (발언권)

```
브라우저-A         cwrtc-A          CMP              cwrtc-B         브라우저-B
  │                 │                │                 │                │
  │ [1] PUSH 버튼 클릭                │                 │                │
  │── {type:         │               │                 │                │
  │    ptt_request,  │               │                 │                │
  │    call_id:".."}►│               │                 │                │
  │                 │── RTCP APP ───►│                 │                │
  │                 │  [80 CC 00 07  │                 │                │
  │                 │   SSRC         │                 │                │
  │                 │   MCPT         │                 │                │
  │                 │   01(REQ)      │                 │                │
  │                 │   id_len       │                 │                │
  │                 │   speaker_id]  │                 │                │
  │                 │                │                 │                │
  │                 │◄── RTCP APP ───│                 │                │
  │                 │  opcode=2      │── RTCP APP ────►│                │
  │                 │  (GRANT)       │  opcode=6       │                │
  │                 │  speaker=A     │  (TAKEN)        │                │
  │                 │                │  speaker=A      │                │
  │◄── {type:       │               │                 │── {type:       │
  │    ptt_floor,   │               │                 │   ptt_floor,   │
  │    speaker:A}   │               │                 │   speaker:A} ──►│
  │                 │                │                 │                │
  │ [2] A가 음성/영상 송신              │                 │                │
  │══ DTLS-SRTP ══►│                │                 │                │
  │  (opus 암호화)   │── Plain RTP ──►│                 │                │
  │                 │  (AMR-WB)      │                 │                │
  │                 │                │──── RTP ───────►│══ DTLS-SRTP ══►│
  │                 │                │  SSRC+seq 재작성  │               │
  │                 │                │                 │                │
  │ [3] PUSH 해제                     │                 │                │
  │── {type:         │               │                 │                │
  │    ptt_release}  │               │                 │                │
  │                ──►│              │                 │                │
  │                 │── RTCP APP ───►│                 │                │
  │                 │  opcode=4      │── RTCP APP ────►│                │
  │                 │  (RELEASE)     │  opcode=5       │                │
  │                 │                │  (IDLE)         │── {type:       │
  │◄── {type:       │               │                 │   ptt_idle} ──►│
  │    ptt_idle}    │               │                 │                │
```

### 3.4 1:1 VoIP 통화

```
브라우저-A         cwrtc-A          CSP            CMP           cwrtc-B      브라우저-B
  │                 │                │              │              │              │
  │── {type:call,   │               │              │              │              │
  │    to:"+82..",  │               │              │              │              │
  │    sdp:".."}   ►│               │              │              │              │
  │                 │── INVITE ─────►│              │              │              │
  │                 │  sip:+82..@csp │              │              │              │
  │                 │  SDP: m=audio  │              │              │              │
  │                 │  30001 RTP/AVP │              │              │              │
  │                 │                │── add(A) ───►│              │              │
  │                 │                │  {cmd:add,   │              │              │
  │                 │                │   session_id}│              │              │
  │                 │                │◄── OK ───────│              │              │
  │                 │                │  {port:50000}│              │              │
  │                 │                │              │              │              │
  │                 │                │── INVITE ────────────────►│              │
  │                 │                │  SDP: m=audio│              │              │
  │                 │                │  50000       │              │              │
  │                 │                │              │              │── {type:     │
  │                 │                │              │              │   incoming,  │
  │                 │                │              │              │   from:"..", │
  │                 │                │              │              │   sdp:".."}─►│
  │◄── {type:       │◄── 180 ────────│              │              │              │
  │    progress}    │               │              │              │              │
  │                 │                │              │              │◄── {type:    │
  │                 │                │              │              │   answer,    │
  │                 │                │              │              │   sdp:".."}──│
  │                 │                │◄── 200 OK ──────────────│              │
  │                 │                │  SDP: m=audio│              │              │
  │                 │                │  30007       │              │              │
  │                 │                │── add(B) ───►│              │              │
  │                 │                │  {remote_ip, │              │              │
  │                 │                │   remote_port│              │              │
  │                 │                │   =30007}    │              │              │
  │                 │◄── 200 OK ─────│              │              │              │
  │◄── {type:       │               │              │              │              │
  │    answered}    │               │              │              │              │
  │                 │                │              │              │              │
  │═══ DTLS-SRTP ═►│═══ RTP ═══════►│    CMP       │◄═══ RTP ════│◄══ DTLS ═════│
  │◄═══════════════│◄══════════════│◄═══ relay ══►│════════════►│══════════════►│
  │                 │                │              │              │              │
  │── {type:hangup}►│── BYE ────────►│── remove ──►│              │              │
  │                 │                │  {cmd:remove}│              │              │
  │                 │                │── BYE ──────────────────►│── {type:     │
  │                 │                │              │              │   ended} ───►│
```

### 3.5 가입자 관리 Flow

```
Console          CSC(4420)              DB                CSP
  │                 │                    │                  │
  │── POST /users ──►│                   │                  │
  │  {name,login_id, │── INSERT INTO    │                  │
  │   password}      │   users ────────►│                  │
  │◄── 201 {user} ──│                   │                  │
  │                 │                    │                  │
  │── POST /users/  │                   │                  │
  │   {pid}/ptt ───►│                   │                  │
  │  {id:"+8257..", │── INSERT INTO     │                  │
  │   auth_id,      │   ptt_sub ───────►│                  │
  │   passwd}       │                   │                  │
  │◄── 201 {sub} ──│                   │                  │
  │                 │                    │                  │
  │── POST /ptt/   │                    │                  │
  │   groups ──────►│                   │                  │
  │  {id:"+8257..", │── INSERT INTO     │                  │
  │   name,members} │   ptt_groups ────►│                  │
  │                 │── INSERT INTO     │                  │
  │                 │   ptt_group_      │                  │
  │                 │   members ───────►│                  │
  │◄── 201 {group} ─│                  │                  │
  │                 │                    │                  │
  │                 │                    │   (CSP 주기적    │
  │                 │                    │    DB 동기화)    │
  │                 │                    │◄── SELECT ───────│
  │                 │                    │── group data ───►│
  │                 │                    │                  │── modifyGroup ►CMP
```

---

## 4. 포트 요약

| 컴포넌트 | 포트 | 프로토콜 | 용도 |
|---------|------|---------|------|
| CSP | 5060 | SIP/UDP | SIP 시그널링 |
| CSP | 25061 | SIP/TCP | SIP 시그널링 (TCP) |
| CSP | 5061 | SIP/TLS | SIP 시그널링 (암호화) |
| CSP | 9001 | UDP | CMP 응답 수신 |
| CMP | 9000 | UDP JSON | CSP→CMP 제어 |
| CMP | 50000-50079 | RTP/RTCP | 미디어 릴레이 (기본 설정) |
| CSC | 4420 | HTTPS | Admin/Auth API |
| CSC | 4430 | HTTPS | MCPTT API (IdMS/GMS/CMS/KMS) |
| cwrtc | 8080 | HTTP/WS | WebSocket + 정적 파일 |
| cwrtc | 30000+ | DTLS-SRTP | 브라우저↔cwrtc 미디어 |
| MySQL | 3306 | TCP | 데이터베이스 |

---

## 5. 기동 순서

```bash
# 0. 사전 확인
# MySQL이 실행 중이고 cims DB가 생성되어 있어야 합니다
mysql -u cims -pcims1234 -e "SELECT 1" cims

# 1. CMP (미디어 서버) - 반드시 먼저 시작
./bin/cmp config/cmp.json
# 확인: "Server listening on 0.0.0.0:9000" 로그 출력
# 확인: RTP 포트 풀 초기화 "Initialized N resources (port 50000-50076)"

# 2. CSP (SIP 서버) - CMP 연결 확인 후
./bin/csp config/csp.json -n
# 확인: "CMP Connected" 로그 출력 (3초 내)
# 확인: SIP 포트 바인드 "SIP listening on 0.0.0.0:5060"

# 3. CSC (API 서버)
cd csc/src && python3 csc_app.py
# 확인: "HTTPS server on port 4420" + "HTTPS server on port 4430"

# 4. cwrtc (WebRTC 브리지)
./bin/cwrtc config/cwrtc.json
# 확인: "HTTP server on port 8080"
# 확인: CSP SIP 등록 테스트 가능
```

**기동 순서가 중요한 이유:**
- CSP는 기동 시 CMP에 `alive` 명령을 3초 간격으로 전송합니다.
- CMP가 먼저 실행되지 않으면 CSP는 "CMP Disconnected" 상태로 유지되어 통화가 불가능합니다.
- cwrtc는 CSP에 SIP REGISTER를 보내므로 CSP가 먼저 실행되어야 합니다.

---

## 6. 트러블슈팅

### 6.1 CMP 연결 실패

**증상:** CSP 로그에 "CMP Disconnected" 반복
**원인:** CMP가 실행되지 않았거나, CSP의 `CmpIp`/`CmpPort` 설정이 잘못됨
**해결:**
```bash
# CMP 프로세스 확인
ps aux | grep cmp

# CMP 포트 확인
ss -ulnp | grep 9000

# CSP 설정 확인
cat config/csp.json | grep -E "CmpIp|CmpPort"
```

### 6.2 SIP 등록 실패

**증상:** 401 Unauthorized 반복 (nonce 재시도 후에도)
**원인:** 사용자 비밀번호 불일치 또는 realm 불일치
**해결:**
```bash
# DB에서 사용자 확인
mysql -u cims -pcims1234 cims -e "SELECT id, auth_id, passwd FROM voip_subscriptions WHERE id='+821012345678'"

# CSP realm 확인
cat config/csp.json | grep Realm
```

### 6.3 RTP 리소스 부족

**증상:** CMP 로그에 "allocResource: no free resources"
**원인:** `RtpPoolSize`보다 많은 동시 세션
**해결:**
```bash
# cmp.json에서 RtpPoolSize 증가
# 주의: 포트 범위가 다른 서비스와 겹치지 않도록 확인
# 필요 포트 수 = RtpPoolSize * 4
```

### 6.4 Floor 제어 동작 안 함

**증상:** PTT 버튼 눌러도 발언권 승인 안 됨
**원인:** RTCP 포트(RTP+1) 방화벽 차단, 또는 멤버가 그룹에 joinGroup 안 됨
**해결:**
```bash
# CMP 로그에서 RTCP 수신 확인
grep "Floor RTCP" /path/to/cmp.log

# "RTCP from unknown sender" 로그 → 포트 불일치
# "Member not found" → joinGroup 실패 확인
```

### 6.5 WebSocket 연결 끊김

**증상:** Phone App에서 주기적으로 연결 끊김
**원인:** ping 미전송 (30초 타임아웃), 또는 프록시/로드밸런서 타임아웃
**해결:**
- 클라이언트에서 30초 간격 `{"type":"ping"}` 전송 확인
- 프록시 WebSocket 타임아웃 설정 확인 (60초 이상 권장)

### 6.6 영상이 한쪽만 보임

**증상:** PTT 그룹 통화에서 화자의 영상이 수신자에게 보이지 않음
**원인:** Video RTP 포트 할당 안 됨 (그룹의 `video_enabled=false`)
**해결:**
```bash
# 그룹 설정 확인
curl -k -X GET https://192.168.0.2:4420/api/v1/ptt/groups \
  -H "Authorization: Bearer <token>" | python3 -m json.tool

# video_enabled를 true로 변경
curl -k -X PUT "https://192.168.0.2:4420/api/v1/ptt/groups/%2B82571910001" \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"video_enabled":true}'
```

---

## 7. 관련 문서

| 문서 | 설명 |
|------|------|
| [02_deployment.md](./02_deployment.md) | 분산 배포 + P1 토폴로지 (5 server) |
| [modules/csp.md](./modules/csp.md) | CSP 내부 모듈 설계 (CSCF/TAS/PTT-AS/IBCF) |
| [modules/cmp.md](./modules/cmp.md) | CMP 내부 모듈 설계 (RTP relay + Floor) |
| [modules/csc.md](./modules/csc.md) | CSC 내부 모듈 설계 (Admin/MCPTT API) |
| [modules/agent.md](./modules/agent.md) | Agent 데몬 (heartbeat / sync REST) |
| [features/flow_logging.md](./features/flow_logging.md) | Flow/Msg 로깅, sesid, Realm, 모듈 간 프로토콜 필드 |
| [features/sip_runtime_config.md](./features/sip_runtime_config.md) | SIP 런타임 설정 (jsonl + SIGUSR1) |
| [features/build_and_packaging.md](./features/build_and_packaging.md) | 빌드/패키징 워크플로우 (콘솔 `/release/package`) |
| [../VERIFICATION_PROCESS.md](../VERIFICATION_PROCESS.md) | 6단계 검증 파이프라인 (S1~S6) |

---

## 8. 참조 규격

| 규격 | 내용 |
|------|------|
| 3GPP TS 24.379 | MCPTT 호 제어 (SIP 기반) |
| 3GPP TS 24.380 | MCPTT 미디어 (RTP, Floor 제어) |
| 3GPP TS 24.481 | MCPTT GMS (그룹 관리) |
| 3GPP TS 24.484 | MCPTT CMS (설정 관리) |
| 3GPP TS 33.180 | MCPTT 보안 (IdMS, KMS) |
| OMA PoC | Push-to-Talk over Cellular (XML 그룹 정보) |
| RFC 3261 | SIP 기본 프로토콜 |
| RFC 3550 | RTP 프로토콜 |
| RFC 3711 | SRTP (암호화 미디어) |
| RFC 7636 | OAuth2 PKCE (Proof Key for Code Exchange) |

---

## 부록 A. cwrtc 웹 테스트 단말 상세

> cwrtc는 실제 MCPTT/VoIP 단말(UE)이 없는 환경에서 브라우저(Phone App)로 시스템 기능을 검증하기 위한 **WebRTC-SIP 브리지 프로그램**입니다. 운영 환경에서는 규격 준수 단말로 대체됩니다.

### A.1 cwrtc 구조

```
┌──────────┐    WebSocket (JSON)     ┌──────────┐    SIP (UDP 5060)     ┌──────────┐
│ Phone App│◄───────────────────────►│  cwrtc   │◄────────────────────►│   CSP    │
│ (브라우저) │    DTLS-SRTP            │ (브리지)  │    Plain RTP          │          │
│          │◄═══════════════════════►│          │◄════════════════════►│   CMP    │
└──────────┘                        └──────────┘                      └──────────┘
```

**핵심 역할:**
- **시그널링 변환**: 브라우저 WebSocket JSON ↔ SIP 프로토콜 (B2BUA)
- **미디어 변환**: DTLS-SRTP(암호화) ↔ Plain RTP(평문) 릴레이
- **Floor 릴레이**: RTCP APP Floor 패킷 ↔ WebSocket JSON 이벤트 변환

### A.2 cwrtc 설정 (cwrtc.json)

패키지에는 `config_template.json` 의 default 로 생성된 base `config/cwrtc.json` 이 동봉되며,
콘솔 배포설정(웹 UI)이 쓴 deployment overlay(`config.json`, flat dotted key)가 기동 시
merge 되어 우선한다 (csp/cmp 와 동일 계약). 개발 환경은 `configure.sh` 가 같은 템플릿의
`deploy_value` 로 렌더링한다.

| 파라미터 | 타입 | 기본값 | 설명 |
|---------|------|--------|------|
| `Setup.LocalIp` | string | "127.0.0.1" | cwrtc 노드 IP (SDP/SIP Contact 광고, 실 IP 필수) |
| `Setup.WsPort` | int | 8443 | WebSocket 서버 포트 |
| `Setup.Wss` | int | 1 | 1=WSS(TLS). HTTPS 페이지에서 접속하려면 필수 |
| `Setup.CertFile` | string | "cert/csp.pem" | WSS 용 인증서+키 결합 PEM |
| `Setup.DocRoot` | string | "html" | 내장 HTTP 서버 문서 루트 |
| `Setup.Sip.ServerIp` | string | "127.0.0.1" | CSP SIP 서버 IP |
| `Setup.Sip.ServerPort` | int | 5060 | CSP SIP 포트 |
| `Setup.Sip.Domain` | string | ims.mnc033.… | VoLTE 도메인 (CSP 와 일치 필요) |
| `Setup.Sip.PttDomain` | string | ptt.mnc033.… | PTT 도메인 (CSP 와 일치 필요) |
| `Setup.Sip.LocalPort` | int | 5062 | cwrtc SIP bind UDP 포트 |
| `Setup.Rtp.PortBase` | int | 50100 | RTP 포트 풀 시작 |
| `Setup.Rtp.PortCount` | int | 50 | RTP 포트 수 (동시 세션 한계) |
| `Setup.Log.Dir` | string | "log" | 로그 디렉토리 |
| `Setup.MsgLog.Dir` | string | "log" | SIP 메시지 로그 디렉토리 |

```json
{
  "Setup": {
    "LocalIp": "192.168.0.134",
    "WsPort": 8443,
    "Wss": 1,
    "CertFile": "cert/csp.pem",
    "Sip": { "ServerIp": "192.168.0.135", "ServerPort": 5060,
             "Domain": "ims.mnc033.mcc450.3gppnetwork.org",
             "PttDomain": "ptt.mnc033.mcc450.3gppnetwork.org", "LocalPort": 5062 },
    "Rtp": { "PortBase": 50100, "PortCount": 50 }
  }
}
```

### A.3 포트 레이아웃 (세션당 6포트)

```
세션 N의 기본 포트 = Setup.Rtp.PortBase + (N × 6)     (N < Setup.Rtp.PortCount)

  +0: Audio DTLS    (브라우저↔cwrtc, DTLS-SRTP 암호화)
  +1: Audio RTP     (cwrtc↔CMP, 평문 RTP)
  +2: Audio RTCP    (cwrtc↔CMP, Floor 제어 패킷 포함)
  +3: Video DTLS    (브라우저↔cwrtc, DTLS-SRTP 암호화)
  +4: Video RTP     (cwrtc↔CMP, 평문 RTP)
  +5: Video RTCP    (cwrtc↔CMP)

예시 (세션 0): 50100, 50101, 50102, 50103, 50104, 50105
예시 (세션 1): 50106, 50107, 50108, 50109, 50110, 50111

전체 UDP 범위 = PortBase ~ PortBase + PortCount×6 - 1 (기본 50100~50399).
방화벽은 이 범위를 열어야 브라우저 미디어가 흐른다.
```

### A.4 SDP 변환 (DTLS-SRTP → Plain RTP)

브라우저로부터 받은 WebRTC SDP:
```
v=0
o=- 4567890123 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE audio video
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:aB1c
a=ice-pwd:dEfGhIjKlMnOpQrStUvWxYz1
a=fingerprint:sha-256 A1:B2:C3:D4:E5:F6:...
a=setup:actpass
a=mid:audio
a=sendrecv
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1
a=rtcp-mux
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 0.0.0.0
a=mid:video
a=sendrecv
a=rtpmap:96 H264/90000
a=fmtp:96 profile-level-id=42e01f;packetization-mode=1
a=rtcp-mux
```

cwrtc가 CSP에 보내는 변환된 SDP (Plain RTP):
```
v=0
o=cwrtc 0 0 IN IP4 192.168.0.2
s=-
c=IN IP4 192.168.0.2
t=0 0
m=audio 30001 RTP/AVP 99
a=rtpmap:99 AMR-WB/16000
a=sendrecv
m=video 30004 RTP/AVP 97
a=rtpmap:97 H264/90000
a=fmtp:97 profile-level-id=42e01f
a=sendrecv
```

> **변환 과정:**
> 1. DTLS 핸드셰이크로 SRTP 마스터 키 추출
> 2. 브라우저 SRTP → `srtp_unprotect()` → 평문 RTP → CMP 전송
> 3. CMP 평문 RTP → `srtp_protect()` → 브라우저 SRTP 전송
> 4. 코덱은 SDP 협상에 따라 결정 (트랜스코딩 없이 릴레이)

### A.5 WebSocket 프로토콜 (브라우저 ↔ cwrtc)

연결 주소: `wss://<host>:8080/cwrtc` (30초 간격 ping으로 연결 유지)

#### 클라이언트 → 서버 메시지

**register (SIP 등록 요청):**
```json
{
  "type": "register",
  "user": "+82571900001",
  "password": "123456",
  "domain": "csp",
  "auth_id": "4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org"
}
```
> `auth_id`는 SIP IMPI (3GPP 형식). 미입력 시 user와 동일하게 사용.

**call (발신):**
```json
{
  "type": "call",
  "to": "+82571910001",
  "sdp": "v=0\r\no=- 123456 2 IN IP4 127.0.0.1\r\n..."
}
```
> `to`에 그룹 번호를 입력하면 PTT 그룹 발신, 개인 번호면 1:1 VoIP 발신.

**answer (착신 수락):**
```json
{
  "type": "answer",
  "call_id": "inv-20260331-001@192.168.0.2",
  "sdp": "v=0\r\n..."
}
```

**hangup (통화 종료):**
```json
{ "type": "hangup", "call_id": "inv-20260331-001@192.168.0.2" }
```

**ptt_request / ptt_release (발언권 제어):**
```json
{ "type": "ptt_request", "call_id": "grp-001@192.168.0.2" }
{ "type": "ptt_release", "call_id": "grp-001@192.168.0.2" }
```

**ping (연결 유지, 30초 간격):**
```json
{ "type": "ping" }
```

#### 서버 → 클라이언트 메시지

**registered / register_failed:**
```json
{ "type": "registered", "user": "+82571900001" }
{ "type": "register_failed", "reason": "unauthorized" }
```
> reason: `"missing_fields"`, `"unauthorized"`

**incoming (1:1 착신):**
```json
{
  "type": "incoming",
  "call_id": "inv-002@192.168.0.2",
  "from": "+821012345678",
  "sdp": "v=0\r\n...",
  "ptt": "false"
}
```

**ptt_auto_answer (PTT 그룹 초대 — SIP 자동 응답 완료):**
```json
{
  "type": "ptt_auto_answer",
  "call_id": "grp-member-001@192.168.0.2",
  "from": "sip:+82571910001@csp",
  "group_id": "+82571910001",
  "sdp": "v=0\r\n..."
}
```
> CSP가 SIP 200 OK를 자동 전송. 브라우저는 제공된 SDP로 WebRTC 연결만 수립.

**answered / progress:**
```json
{ "type": "answered", "call_id": "...", "sdp": "v=0\r\n..." }
{ "type": "progress" }
```

**ptt_floor (화자 변경):**
```json
{ "type": "ptt_floor", "speaker": "tel:+82571900001" }
```
> `speaker`가 null이면 화자 없음 (IDLE 상태)

**ptt_reject / ptt_idle:**
```json
{ "type": "ptt_reject" }
{ "type": "ptt_idle" }
```

**ptt_member_joined / ptt_member_left:**
```json
{ "type": "ptt_member_joined", "user_id": "tel:+82571900002" }
{ "type": "ptt_member_left", "user_id": "tel:+82571900002" }
```

**ended (통화 종료):**
```json
{ "type": "ended", "reason": "normal" }
```
> reason: `"normal"`, `"busy"`, `"not_registered"`, `"error"`, `"rejected"`, `"timeout"`

### A.6 cwrtc 내부 처리 흐름

```
[브라우저 PUSH]
  │
  ├─ 브라우저 → cwrtc (WS): {"type":"ptt_request","call_id":"..."}
  │
  ├─ cwrtc 내부: RtpThread의 RTCP 소켓으로 FLOOR_REQUEST 전송
  │   → CMP (UDP, RTP포트+1)
  │   RTCP APP: PT=204, name="MCPT", opcode=1
  │
  ├─ CMP → cwrtc (RTCP): FLOOR_GRANT (opcode=2)
  │
  ├─ cwrtc → 브라우저 (WS): {"type":"ptt_floor","speaker":"tel:+82571900001"}
  │
  ├─ 브라우저 → cwrtc: DTLS-SRTP 오디오/비디오 패킷
  │
  ├─ cwrtc 내부: srtp_unprotect() → Plain RTP
  │   → CMP (UDP, RTP포트+1)
  │
  ├─ CMP: 화자 확인 후 다른 멤버에게 RTP 포워딩
  │   (SSRC/Seq 재작성)
  │
  └─ 다른 멤버의 cwrtc: Plain RTP → srtp_protect() → DTLS-SRTP
     → 해당 브라우저
```

### A.7 Phone App 기능 요약

| 기능 | 설명 |
|------|------|
| VoIP 1:1 통화 | 다이얼패드, 발신/착신/종료 |
| PTT 그룹 통화 | PUSH 버튼, Floor 제어, 자동 응답 |
| 영상 송수신 | on/off 토글 (송신 제어), 회전, 화자 오버레이 |
| 음량 제어 | 음소거 토글, 실시간 음량 레벨 표시 |
| 멤버 목록 | 이름, 연결 상태, 화자 표시 (GMS에서 조회) |
| 문서 뷰어 | MD 문서 탭, PPT 다운로드 |

### A.8 Console App 기능 요약

| 기능 | 설명 |
|------|------|
| 가입자 관리 | 추가/편집/삭제, 조직/착신거부 설정 |
| Call 번호 관리 | MSISDN 등록, auth_id/passwd, DND/착신전환 |
| PTT 번호 관리 | MSISDN 등록, IMPI auth_id |
| PTT 그룹 관리 | 그룹 생성/편집, 멤버 추가/삭제, 우선순위, 영상 지원 |
| 통화현황 | 실시간 PTT 세션 상태, 통화 로그 |
| 문서 뷰어 | MD 문서 탭, PPT 다운로드 |
