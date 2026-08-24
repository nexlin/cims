# SIP 접속 보안 — 채널 정책 게이트(P0) + 인증 자료 경계(P1)

UE↔CSP 접속 구간의 보안 체계를 3GPP TS 33.203 정합 구조로 끌어올리는 설계 정본이다.
이 문서는 도입 로드맵(P0~P4) 중 **P0(채널 정책 게이트)** 와 **P1(인증 자료 경계 재편)** 의
상세 설계를 담는다. P2(Sec-Agree)·P3(AKA over TLS)·P4(IMS AKA+IPsec)는 [§8 로드맵](#8-로드맵-p2p4)
에 자리만 둔다.

> **상태**: P0·P1 구현 반영. 인증 자료 이행은 [§4.7 배포 순서](#47-소비자-전환--프로비저닝시험-도구) 의
> ②~④ 구간(과도기 fallback 동작) — `passwd` 값 소거·DROP(⑤·⑥)은 전 조합 등록 회귀 후 후속 작업.

관련 문서: [sip_tls_signaling.md](sip_tls_signaling.md) · [registration_binding_set.md](registration_binding_set.md) ·
[ue_nat_traversal.md](ue_nat_traversal.md) · [../modules/csp.md](../modules/csp.md) ·
[../modules/csc.md](../modules/csc.md) · [../identifier_model.md](../identifier_model.md)

## 1. 표준 근거 — 지원 조합은 표준 조합으로 한정한다

TS 33.203 이 정의하는 접속 보안 조합만 지원한다. 자유 조합(예: AKA+평문 UDP)은 만들지 않는다.

| 조합 | 근거 | transport | CIMS 도입 단계 |
|---|---|---|---|
| SIP Digest (평문) | TS 33.203 Annex N | UDP/TCP | 현행 |
| SIP Digest + TLS | Annex N+O | TLS/TCP | 현행(게이트 없음) → **P0 에서 완성** |
| IMS AKA + TLS | Annex X | TLS/TCP | P3 |
| IMS AKA + IPsec | 본문 §6~7 | UDP/TCP (SA 공용) | P4 |

이 문서의 두 단계가 기대는 표준 조항:

- **Annex O (게이트)** — 등록 200 OK 시점에 P-CSCF 는 TLS 연결을 IMPI/IMPU 에 결부하고,
  이후 "**shall not accept any SIP signalling messages outside the TLS connection** other than
  REGISTER messages, messages relating to emergency services …, and error messages". 보호
  채널로 등록한 신원의 평문 요청 수용은 표준 위반이다.
- **Cx 모델 (인증 자료 경계)** — HSS 는 인증 벡터(AV) 발급자다. SIP Digest 의 AV 는
  **H(A1)** 이며 평문 비밀번호는 HSS(CIMS 에선 CSC/DB) 밖으로 나가지 않는다. AKA 의 AV
  (RAND, AUTN, XRES, CK, IK)는 SQN 상태를 가지므로 발급자가 단일해야 한다.
- **Annex P.4 (판별·고착)** — REGISTER 의 `Authorization`/`integrity-protected` 로 인증
  체계를 판별하고, "For subsequent REGISTER requests, the authentication scheme shall not
  change" — 등록 단위로 체계를 고착한다. P0/P1 은 이 판별이 앉을 자리(정책 축·AV 계약)를
  만든다.

## 2. 구조 — 인증 자료 경계와 채널-신원 결부

### 2.1 인증 자료의 위치와 형태

| 구간 | 위치 | 형태 |
|---|---|---|
| DB SoT | `volte_subscriptions.ha1` / `ptt_subscriptions.ha1` (`sql/cims_schema.sql`, CHAR(32)) | **H(A1)** |
| 과도기 | 같은 테이블 `passwd` — CSC 가 ha1 과 함께 쓰고, CSP 는 ha1 이 비었을 때만 읽는다 | 평문 (소거·DROP 예정) |
| CSP 메모리 | `CspUser::m_strHa1` (`csp/CspUser.h`) — 전 가입자 캐시 | H(A1) |
| 검증 지점 | `CCscfModule::CheckAuthorizationResponse` (`csp/CscfModule.cpp`) — 저장 H(A1) 로 response 합성 | H(A1) 소비 |
| 쓰기 경로 | CSC `_add_subscription`/`_update_subscription` (`csc/src/handlers/admin.py`) — 요청 본문의 `passwd` 를 H(A1) 로 변환 | 평문은 요청 본문에만 |
| 단말 전달 | `/provisioning/me` 응답 `services[].account.sipHa1` (`csc/src/services/mcptt.py`) | H(A1) (`sipPassword` 는 과도기 병행) |
| 시험 도구 | cspsim `-db`(DB `ha1` 우선) / `-ha1 <hex>`, verify 시드 `VOIP_HA1`/`PTT_HA1` | H(A1) |

Digest 경로의 부수 규칙: nonce 는 OpenSSL `RAND_bytes` 16 바이트 hex(`csp/NonceMap.cpp`), 단말이
보낸 realm 은 서비스 realm 과 대조해 불일치면 401 재챌린지, 인증 실패 로그는 정답 해시를 남기지
않는다, CSC 는 `passwd` 미전송 시 ha1 을 유지한다(부분 업데이트).

### 2.2 채널-신원 결부

- 바인딩 집합은 (IP, 포트, transport) 키로 동작한다([registration_binding_set.md](registration_binding_set.md)).
  그 위에 **요청 채널과 가입자 정책(`sip_transport`)을 대조하는 게이트**(§3)가 인증보다 앞에 선다.
- psip 은 응용에 (IP, 포트, transport enum, listener id)를 전달한다. TLS/TCP 연결은 `"<ip>:<port>"`
  문자열 키(`ext/psip/SipStack/TcpSocketMap.cpp`)로 식별되고 **SSL 세션 ID·연결 핸들은 노출되지
  않는다** — Annex O 의 "그 TLS 연결" 을 암호학적으로 결속하는 것은 [§8.4](#84-psip-확장-단계-무관) 의 잔여 항목이다.
- `m_iListenerId` 는 세 transport 모두에서 채워진다 — TCP/TLS 는 연결을 수락한 listener id 가
  `CTcpComm → CTcpSessionListInfo → (큐 엔트리)` 를 타고 수신 스레드의 thread-local 로 복원된다
  (`ext/psip/SipStack/SipTcpThread.cpp`, `SipTlsThread.cpp`, `SipQueueThread.cpp`). `inbound_policy=restricted`
  검사가 TLS 요청에도 유효하다.
- 가입자별 `sip_transport ENUM('UDP','TCP','TLS')` 는 프로비저닝 힌트이면서 `TLS` 값은 서버 집행
  정책이다(§3.1). `/provisioning/me` 는 `TLS` 정책 가입자에게 transport 목록을 TLS 하나로 좁히고
  `sip.enforced=true` 를 표시한다.

## 3. P0 — 채널 정책 게이트

### 3.1 정책 축: `sip_transport` 를 힌트에서 집행 정책으로 승격

가입자(subscription)별 `sip_transport` 컬럼의 의미를 재정의한다:

| 값 | 프로비저닝(현행 유지) | 서버 집행(신설) |
|---|---|---|
| `TLS` | 단말에 TLS 접속 지시 | **TLS 채널 강제** — 비-TLS 채널의 이 신원 요청은 전부 거부 |
| `UDP` / `TCP` | 단말에 해당 transport 권장 | 집행 없음 (힌트로만 — 평문 간 강제는 보안 의미가 없다) |
| `NULL` | 전체 transport 목록 제공 | 집행 없음 — 단말 자유 선택([sip_tls_signaling.md §7.1](sip_tls_signaling.md)) |

단말에 주는 힌트와 서버가 집행하는 정책이 **같은 컬럼에서** 나오므로 어긋날 수 없다.
자유(`NULL`) 가입자의 UDP+TLS 혼합 바인딩([registration_binding_set.md §7](registration_binding_set.md)
시나리오)은 그대로 유효하다 — 게이트는 `TLS` 정책 가입자에게만 작동한다.

Annex O 는 "협상으로 TLS 를 선택한 등록" 에 게이트를 규정한다. CIMS 는 Sec-Agree(P2) 이전
단계이므로 **협상 결과의 자리를 가입자 정책이 대신한다** — P2 도입 시 협상 결과가 이 정책과
합쳐지는 구조다(협상이 tls 로 끝난 등록 = 그 등록에 한해 TLS 정책 적용).

### 3.2 게이트 알고리즘

위치: `csp/ModuleDispatcher.cpp` `EventIncomingRequestAuth` — 현행 단계(peer 우회 →
TestEnv 우회 → 미등록 처리 → 주소 변경 판정) 중 **주소 변경 판정 앞**. peer(PendingRouteMap)
경로와 TestEnv 우회는 가입자 요청이 아니므로 게이트보다 앞에 그대로 둔다.

```
policy = 가입자.sip_transport            // CspUser 에 신규 적재 (§3.4)
if policy == TLS:
    if 요청.m_eTransport != E_SIP_TLS:
        → 403 Forbidden, 로그 "channel policy violation" (REGISTER 포함)
    // TLS 채널이면: 기존 주소 변경 판정으로 계속
    //  - 등록된 TLS 바인딩과 (ip,port) 일치 → 통과 (TouchFlow)
    //  - 불일치(새 TLS 연결) → Digest 재인증 후 SetIpPort — TLS→TLS 이동만 허용
else:
    // 현행 동작 그대로 (주소 변경 재인증 포함)
```

- **평문 REGISTER 도 403 이다.** Annex O 가 REGISTER 를 예외로 두는 것은 보안 협상 재개
  용도인데, CIMS 의 정책은 협상이 아니라 프로비저닝으로 확정되므로 정책 위반 REGISTER 를
  받아줄 이유가 없다. 단말이 TLS 로 재접속해 재등록하는 경로만 유효하다.
- **연결 동일성 판정은 (ip, port, TLS) + `IsFlowAlive`** 다. SSL 세션 ID 결속이 아니라
  대리키 판정이며, NAT 포트 재사용 시 오일치 가능성이 남는다([sip_tls_signaling.md §4.4](sip_tls_signaling.md)).
  이 잔여 위험은 "새 연결이면 Digest 재인증" 규칙이 흡수한다 — 대리키가 우연히 일치해도
  공격자는 인증 자료 없이는 기존 신원의 요청을 만들 수 없고, nonce/nc 단조증가 검사
  (`csp/NonceMap.cpp`)가 재전송을 막는다. 암호학적 결속(연결 핸들 노출)은 [§8](#8-로드맵-p2p4)
  의 psip 확장 항목이다.
- 오류 응답(4xx~6xx)은 게이트 대상이 아니다 — Annex O 의 오류 메시지 예외에 상응한다.

### 3.3 관측

- 로그: `channel policy violation user=<id> transport=<t> src=<ip>:<port>` — 기존 바인딩
  로그(`binding added`/`binding moved`) 관례를 따른다.
- 반복 위반(스캔/오설정 탐지)은 **A-SEC-003**(`security_violation`, X.736) 알람이다
  ([alarm_catalog](../alarm_catalog.md)). 게이트 403 을 `SipStatsMonitor` 가 소스 로그 억제와
  무관하게 전 건 계수(소스 IP 상위 32 동반)하고, `Setup.SipStats.EvalSec` 윈도우당 건수가
  `Setup.SipStats.ChannelPolicyMajor`(기본 10, 0=off) 이상이면 major 발화
  (mo `<서버명>/csp/channel_policy`, params count/window/ip/top_count), 미만 윈도우에서
  해소한다. 폭주 억제는 미가입 계정 403 과 같은 소스 단위 로그 억제(`csp/CscfModule.cpp`)가
  담당한다 — 계수·알람은 억제와 독립이다.

### 3.4 구현 위치

| # | 항목 | 위치 |
|---|---|---|
| P0-1 | `sip_transport` 가 CSP 가입자 캐시에 적재 — DB SELECT + `CspUser::m_strSipTransport`/`requiresTls()` + JSON fallback 키 `sip_transport` | `csp/DbManager.cpp`, `csp/CspUser.{h,cpp}` |
| P0-2 | 게이트 `CCscfModule::CheckChannelPolicy` — REGISTER 는 `OnSipRequest` 에서 챌린지 앞, 비-REGISTER 는 `EventIncomingRequestAuth` 에서 주소 변경 판정 앞. 403 + 소스 단위 로그 억제 | `csp/CscfModule.cpp`, `csp/ModuleDispatcher.cpp` |
| P0-3 | psip: TCP/TLS 수신 경로의 `m_iListenerId` 전파 (`CTcpComm::m_iListenerId`, `CSipQueueEntry::m_iListenerId`, thread-local 복원) | `ext/psip/SipStack/TcpSessionList.{h,cpp}`, `SipQueue.{h,cpp}`, `SipQueueThread.cpp`, `SipTcpThread.cpp`, `SipTlsThread.cpp`, `SipStackComm.hpp` |
| P0-4 | CSC API `sip_transport` 입·출력(검증 UDP/TCP/TLS) + 콘솔 번호 표 "채널" 열(자유/UDP/TCP/TLS(강제)) + `/provisioning/me` 의 TLS 목록 축소·`enforced` | `csc/src/handlers/admin.py`, `csc/src/services/mcptt.py`, `ems/service/console/src/pages/ProvisioningWorkbenchPage.tsx` |

기본 스키마(`sql/cims_schema.sql`)에도 `sip_transport` 가 있다 — 신규 설치에서 CSP SELECT 가 깨지지
않도록 마이그레이션(`migrate_subscription_transport.sql`)과 쌍으로 둔다.

## 4. P1 — 인증 자료 경계 재편

### 4.1 원칙: SoT 는 H(A1), 평문 비밀번호는 저장하지 않는다

Cx 모델을 CIMS 구조에 사상한다. **CSC(HSS 역할) = 인증 자료의 유일한 쓰기 주체,
저장 형식 = H(A1)**, CSP(S-CSCF 역할) = 소비자다.

```
H(A1) = MD5( <impi> ":" <realm> ":" <password> )
  impi  = <imsi>@<service.domain>      // CSCF 기대 username 과 동일 (csp/CscfModule.cpp)
  realm = EffectiveRealm(service)      // access_services.auth_realm ?? domain (csp/CspServiceMap.cpp)
```

- 평문 `password` 는 **CSC API 요청 본문에만 존재**하고, 저장 전에 H(A1) 로 변환된다.
- H(A1) 은 그 realm 에 한한 비밀번호 등가물이다 — 유출 시 CIMS 인증은 가능하지만,
  사용자가 다른 서비스에 재사용한 원문 비밀번호는 노출되지 않는다. 전달·보관 전 구간이
  이 등가물로 좁혀진다.

### 4.2 스키마

```sql
-- sql/migrate_subscription_ha1.sql (재실행 안전 — 컬럼 존재 시 no-op)
ALTER TABLE volte_subscriptions
  ADD COLUMN ha1 CHAR(32) NOT NULL DEFAULT '' COMMENT 'MD5(imsi@domain:realm:password) — SIP Digest H(A1)';
ALTER TABLE ptt_subscriptions
  ADD COLUMN ha1 CHAR(32) NOT NULL DEFAULT '' COMMENT '동일';
```

- CSP(`CDbManager::ProbeSchema`)와 CSC(`_has_ha1_column`)는 연결 시 `ha1` 컬럼 존재를 1회 프로브한다 —
  마이그레이션 미적용 DB 에서는 SELECT 에서 `ha1` 을 빼고(`''`) passwd fallback 으로 동작하며 CSC 는
  passwd 만 저장한다(ERROR/WARN 로그 1회). 새 바이너리가 마이그레이션 순서에 묶이지 않는다.
- 기존 행의 `ha1` 은 `sql/migrate_subscription_ha1.py` 가 `passwd` + 서비스 realm 으로 일괄 계산한다
  (멱등 — `ha1=''` 행만 대상). 서비스 정의(domain/auth_realm)는 DB 가 아니라 OAM 스토어에 있으므로
  `--services-json <services.json>` 또는 `--service NAME=DOMAIN[:REALM]` 로 받는다. `--dry-run` 지원.
- `passwd` 컬럼은 이행 검증(전 조합 등록 회귀) 후 **값 소거 → 후속 마이그레이션에서 DROP**
  한다. 과도기 동안 CSP 는 `ha1` 우선, 비어 있으면 `passwd` 로 종전 계산을 한다.
- AKA 용 컬럼(`auth_scheme`, `k`, `opc`, `sqn`)은 P3 에서 추가한다 — P1 스키마에 선반영하지
  않는다(빈 자리를 늘리지 않는다).

### 4.3 H(A1) 의 결박과 재키잉 — 운영 계약

H(A1) 은 `(imsi, 서비스 domain/realm)` 에 결박된다. 다음 변경은 **기존 ha1 을 무효화**하며,
서버는 원문 비밀번호를 모르므로 재계산할 수 없다:

| 변경 | 결과 | 절차 |
|---|---|---|
| 가입자 `imsi` 변경 | 그 가입자 ha1 무효 | 변경 API 가 `passwd` 동시 입력을 **요구**한다(400) |
| 서비스 `domain`/`auth_realm` 변경 | 그 서비스 전 가입자 ha1 무효 | 운영 절차로 격상 — 전 가입자 비밀번호 재설정(대량 등록 경로) 없이는 변경 금지 |

이는 [identifier_model.md](../identifier_model.md) 의 재키잉 절차와 같은 성격이다 — realm
은 사실상 인증 도메인의 키가 되므로, `access_services` 편집 화면과 CSC API 에 이 결박을
명시한다. 같은 이유로 **CSP 는 단말이 보낸 realm 을 신뢰하지 않는다** — §4.6 의 realm
대조가 P1 의 전제 조건이다(저장된 ha1 은 서버 realm 로만 검증 가능하다).

### 4.4 AV 계약 — Cx(MAR/MAA) 상당의 논리 계약

CSP↔CSC 의 인증 자료 교환을 scheme 별 AV 계약으로 정의한다. **P1 에서는 계약만 확정하고,
물리 전달은 현행 DB 공유 읽기를 유지한다** (Digest AV 는 무상태라 매체가 무엇이든 등가다).

```
AV 요청:  { impi, scheme }
digest →  { realm, ha1 }                          // P1: DB 컬럼 (ha1 + 서비스 realm)
aka    →  { rand, autn, xres, ck, ik }            // P3: CSC HTTP API — SQN 단일 발급자
```

- Digest: CSP `DbManager` 의 SELECT 가 `passwd` 대신 `ha1` 을 읽고, `CspUser` 는
  `m_strPassWord` 대신 `m_strHa1` 을 갖는다. 프로세스 메모리의 평문 상주가 사라진다.
- AKA(P3)가 HTTP API 로 물리화되는 이유: AV 소모·SQN 증가·AUTS 재동기는 경쟁하면 안 되는
  상태 기계라 발급자가 CSC 하나여야 한다. Digest 를 미리 API 화하지 않는 것도 같은 기준의
  역방향이다 — 무상태 자료에 왕복을 만들 이유가 없다.

### 4.5 검증 경로 전환 (CSP)

`CheckAuthorizationResponse` 의 H(A1) 계산부가 저장값 사용으로 바뀐다:

```
현행:  H(A1) = MD5(username:realm:m_strPassWord)   // 매 요청 계산
전환:  H(A1) = m_strHa1                            // 저장값 직접 사용
```

H(A2)·response 합성(qop 분기 포함)은 그대로다. 전환과 함께 다음을 정비한다(§2.1 부수 결함):

### 4.6 Digest 경로 정비

| # | 정비 | 내용 |
|---|---|---|
| P1-a | **realm 대조** | 단말 `Authorization` 의 realm ≠ `EffectiveRealm(service)` → 401 재챌린지. 저장 ha1 검증의 전제 |
| P1-b | **nonce 난수화** | `CNonceMap::GetNewValue` 를 OpenSSL `RAND_bytes`(16B, hex) 로 교체. `PRIVATE_KEY` 상수 제거. nc 단조증가·TTL 검사는 현행 유지 |
| P1-c | **정답 해시 로그 제거** | 인증 실패 로그에서 `correct response is [...]` 삭제 — 실패 사실과 username 만 남긴다 |
| P1-d | **CSC 부분 업데이트 수정** | `_update_subscription` 이 `passwd` 미전송 시 ha1 을 **유지**한다(빈값 덮어쓰기 제거) |
| P1-e | **대량 등록 기본값** | subscriber_import 는 `password` 칸이 비면 행별 난수(`secrets.token_urlsafe(9)`)를 생성하고 결과 `credentials[]` 로만 돌려준다(콘솔 가져오기 결과 표). 고정 기본값은 없다. 콘솔 번호 추가 폼도 비밀번호를 필수 입력으로 받는다 |

### 4.7 소비자 전환 — 프로비저닝·시험 도구

Digest 클라이언트는 원문 비밀번호 없이 **H(A1) 만으로 response 를 계산할 수 있다**
(response = f(HA1, nonce, …)). 이를 이용해 전 소비자를 ha1 로 전환한다:

| 소비자 | 동작 |
|---|---|
| `/provisioning/me` | `account.sipHa1` (H(A1)) + `account.sipPassword` (과도기 — DB `passwd` 가 소거되면 항상 `null`). 단말은 `sipHa1` 우선 |
| Android UE | `sipHa1` 수신 시 pjsip cred 를 `PJSIP_CRED_DATA_DIGEST`(ha1) 로 설정한다(`android/core` SipController — cred realm 은 `*` 유지, pjsip 이 DIGEST cred 의 algorithm 미지정을 MD5 로 기본화). `sipHa1` 이 없으면 평문 cred(`sipPassword` → 로그인 비번) 폴백 — H(A1) 은 realm 에 결박된 값이라 challenge realm 추종이 필요한 상황은 평문 경로만 흡수한다. 수동 설정에서 도메인/IMSI/IMPI/비번을 편집하면 결박이 깨진 저장 ha1 을 함께 소거한다 |
| cspsim | `-db` 모드는 DB `ha1` 우선(비면 `passwd`), CLI `-ha1 <hex32>` 로 직접 지정. **`-creds <file>`** = 단말별 자격 파일(JSONL: `user`/`ha1`/`authId`/`password`) — `-count` 전개 단말 각각에 자기 자격을 주며, 전개 user 가 파일에 없으면 기동 전 즉시 중단(fail fast). psip 클라(`CSipServerInfo::m_strHa1`, `MakeA1`)가 H(A1) 입력을 받는다. `-password` 는 유지(직접 계산) |
| verify 하네스 | `subscribers.py` 가 "번호 연속 + 전원 `ha1` 보유" 창(window)을 골라 단말별 자격(`{KIND}_CREDS`)을 시드하고, 시험 항목은 `cred_args()` 가 쓴 JSONL 자격 파일로 `-no-db -creds` 전개한다("같은 비밀번호 구간" 의존 소멸. `-no-db` 인 이유 = DB 모드는 `-user` 를 무시하고 DB 첫 N 행을 쓴다). `ha1` 없는 구 DB 는 "전원 동일 비밀번호" 창 + `-password` 로 폴백 |

**배포 순서가 계약이다**:
① 스키마 — `migrate_subscription_transport.sql` + `migrate_subscription_ha1.sql` (컬럼 추가만,
구 코드에 무해·재실행 안전). `sip_transport` 는 CSP SELECT 의 전제이고, `ha1` 은 프로브로 흡수되므로
②와 순서가 바뀌어도 기동은 되지만 그동안 H(A1) 저장이 일어나지 않는다 →
② CSP/CSC/cspsim/단말 앱의 ha1 소비 코드 배포 →
③ `migrate_subscription_ha1.py` 로 기존 행 ha1 일괄 계산 →
④ 전 조합 등록 회귀(§6) →
⑤ `passwd` 값 소거(CSC 의 `passwd` 병행 쓰기 제거 + `/provisioning/me` 의 `sipPassword` 항상 null) →
⑥ (후속 릴리스) 컬럼 DROP.
②~④ 사이에는 과도기 fallback(§4.2)이 양쪽 형식을 흡수한다.

## 5. 범위 밖 (이 설계가 바꾸지 않는 것)

- **reg-event contact 목록 확장** — [registration_binding_set.md §6](registration_binding_set.md) 의 잔여 항목. 독립 진행.
- **멀티 디바이스**(계정당 복수 단말) — [registration_binding_set.md §8](registration_binding_set.md). 게이트는 바인딩 집합 단위라 도입 시에도 그대로 성립한다.
- **관리평면(CSC 4421/4430, OAM) 인증** — JWT/OAuth 경로는 별도 체계([oam_csc_split.md](../oam_csc_split.md)).
- **IdMS(OAuth PKCE) 로그인 비밀번호** — `users` 계정 비밀번호의 저장 방식은 이 문서 범위 밖이다(SIP Digest 자료만 다룬다).

## 6. 검증 (S 게이트)

최소 관련 stage: S3(기능)·S4(회귀). 시나리오는 [registration_binding_set.md §7](registration_binding_set.md)
의 transport 혼합 5종을 유지한 위에 추가한다:

| # | 시나리오 | 기대 |
|---|---|---|
| V1 | `sip_transport=TLS` 가입자가 UDP 로 REGISTER | 403 + `channel policy violation` 로그 |
| V2 | 동일 가입자 신원으로 평문 INVITE 위조(유효 Digest 포함) | 403 — 게이트가 인증보다 먼저 |
| V3 | TLS 정책 가입자의 TLS 재접속(연결 끊김 후 새 연결) → 재등록 → 호 | 정상 — 재인증 후 바인딩 이동 |
| V4 | `NULL` 정책 가입자의 UDP+TLS 혼합 등록 회귀 | 현행과 동일(게이트 미작동) |
| V5 | ha1 이행 후 전 조합(UDP/TCP/TLS × volte/ptt) 등록·호 회귀 | PASS — 응답 계산 등가 |
| V6 | 이행 스크립트 멱등성(2회 실행) + `passwd` 미전송 PUT 후 재등록 | ha1 보존, 인증 유지 |
| V7 | realm 불일치 Authorization | 401 재챌린지 (V5 이후 상시 회귀) |
| V8 | `/provisioning/me` 의 `sipHa1` 로 단말 등록 | 정상 — 평문 필드 부재 확인 포함 |

V1/V2 는 cspsim `-transport udp` + TLS 정책 가입자로 재현한다(별도 옵션 불필요 — 게이트가 REGISTER 부터
막는다). V2 의 "인증보다 먼저" 는 Authorization 없는 평문 INVITE 의 첫 최종응답이 401 이 아니라 403 인
것으로 판정한다. V7 은 서버 nonce 로 틀린 realm 의 유효 Digest 를 만드는 원시 SIP 프로브로 재현한다.
자동화 항목(S3/S4)은 **잔여**다. 반복 위반 알람은 A-SEC-003 으로 채번·구현되어 있다(§3.3).

## 7. 배포 순서와 잔여

P0 의 게이트 자체는 DB 변경이 없지만, 이 릴리스의 CSP 는 `sip_transport` 컬럼을 읽으므로
`migrate_subscription_transport.sql` 이 CSP 기동의 선행 조건이다(`ha1` 은 프로브로 흡수). P1 은 배포 순서 계약(§4.7 ①~⑥)이 단계를
강제한다. 현재 ②(코드)까지 반영되어 있고 ①·③~⑥ 은 운영 절차다.

잔여 항목:
- §6 V1~V8 의 S3/S4 자동화 시나리오. (반복 위반 알람은 A-SEC-003 으로 구현 — §3.3.)
- cspsim 의 call 시나리오가 `-transport` 를 무시하고 INVITE 를 UDP 로 보낸다(등록만 transport 를 따른다) —
  TCP/TLS **호** 회귀(V5 의 호 부분)는 cspsim 보완 전까지 UDP 로만 성립. 같은 맥락에서 CSP 의
  `Service-Route` 에 `;transport=` 파라미터가 없어(RFC 3608/TS 24.229) TLS 등록 단말이 route 를 따라갈 때
  transport 를 잃는다 — [sip_tls_signaling.md](sip_tls_signaling.md) 쪽 보완 항목.
- `/provisioning/me` 의 `enforced`/TLS 목록 축소는 CSC `Provisioning.Services.*.tls_port` 가 설정된 환경에서만
  드러난다(미설정이면 TLS 항목 자체가 없다).

## 8. 로드맵 (P2~P4)

상세 설계는 각 단계 착수 시 이 문서를 확장한다. 여기서는 범위·선행 조건·핵심 결정만 적는다.

### 8.1 P2 — Sec-Agree (RFC 3329)

보안 메커니즘 협상과 강등(bidding-down) 방지. 초기 REGISTER 의 `Security-Client`(+
`Require`/`Proxy-Require: sec-agree`) → 401 에 `Security-Server`(q값) → 보호 채널 위
재-REGISTER 의 `Security-Verify` 를 서버 원본과 **바이트 대조**, 불일치면 494 로 재시작.

- 서버 목록은 도입 시점 기준 `tls` 뿐이다(`ipsec-3gpp` 는 P4 에서 추가). 협상이 tls 로
  끝난 등록은 §3 게이트의 적용 대상이 된다 — 정책 축(`sip_transport`)과 협상 결과가
  같은 게이트로 합류하고, 정책은 협상의 하한(제안 목록 제한)으로 남는다.
- 협상 결과를 `integrity-protected` 상당의 **내부 플래그**로 등록 상태에 결부한다 —
  P/S-CSCF 가 한 프로세스여도 이 계약을 두면 이후 역할 분리 배치와 Annex P.4 판별(P3)이
  그대로 얹힌다.
- 단말: pjsip 은 sec-agree 미지원 — 헤더 생성/echo 패치가 선행 조건. cspsim 도 동일 확장.
- 검증 핵심: `Security-Server` 변조(강등 시뮬레이션) → `Security-Verify` 불일치 494.

### 8.2 P3 — IMS AKA over TLS (Annex X)

**IPsec 없이 AKA 를 도입하는 표준 경로.** 상호 인증·키 신선도를 TLS 채널 위에서 얻는다.
P0 게이트(TLS 강제)가 선행 조건이다.

- **CSC(AuC 승격)**: Milenage(TS 35.205/206) 구현 + 스키마 `auth_scheme`/`k`/`opc`(암호화
  보관)/`sqn` + **AV HTTP API**(§4.4 aka 항의 물리화 — SQN 증가·AV 소모·AUTS 재동기는
  CSC 단일 발급자). k/opc 는 어떤 API 응답에도 원문 노출하지 않는다(AV 만 나간다).
- **CSP**: Annex P.4 판별 — `Authorization` 의 `integrity-protected="tls-connected"` +
  `algorithm=AKAv2-SHA-256` → AKA 분기. 401 nonce=base64(RAND‖AUTN), 검증 RES==XRES,
  AUTS 수신 시 CSC 재동기 요청. **scheme 은 등록 단위 고착**("shall not change").
- **단말**: soft-K 프로비저닝(K/OPc — TLS 프로비저닝 채널 전제). pjsip 은 Digest-AKA 와
  Milenage 를 내장하므로 응답 계산은 기성 경로다. cspsim 에 AKA 응답 계산 추가.
- 검증 핵심 3종: AKA 등록 성공 / AUTN MAC 실패(단말 중단) / SQN 이탈 → AUTS 재동기.

### 8.3 P4 — IMS AKA + IPsec (본문 §6~7)

**착수 게이트: 3GPP 액세스(UICC 단말) 직접 interop 요구가 확정될 때만.** 그 요구가 없으면
P3 까지가 자가망 구조에서 표준이 제공하는 최대치다.

- P2 협상에 `ipsec-3gpp` 추가(spi-c/spi-s, port-c/port-s, alg 파라미터).
- 등록 절차 중 CK/IK 로 **커널 XFRM 에 ESP transport mode SA 4개(2쌍)** 를 동적
  프로그래밍(netlink), 보호 포트쌍(port_ps/port_pc)은 **UDP·TCP 공용** — 같은 포트에
  UDP 소켓+TCP 리스너를 함께 연다. SA 수명은 등록 수명 결부, 탈등록·만료 시 회수.
- 운영 비용이 네 조합 중 최대: `CAP_NET_ADMIN` 특권(agent/배포 계약 변경), HA 절체 시
  SA 재프로그래밍, 장애 시 커널 상태 잔류 청소.

### 8.4 psip 확장 (단계 무관)

TLS 연결 핸들/세션 ID 의 응용 노출 — §3.2 게이트의 (ip,port) 대리키 판정을 암호학적
결속으로 승격해 NAT 포트 재사용 오일치 잔여 위험을 제거한다. P2~P4 어느 단계와도 독립.
