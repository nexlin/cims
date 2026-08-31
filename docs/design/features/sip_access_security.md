# SIP 접속 보안 — 채널 정책 게이트(P0) + 인증 자료 경계(P1) + 보안 메커니즘 협상(P2) + IMS AKA over TLS(P3)

UE↔CSP 접속 구간의 보안 체계를 3GPP TS 33.203 정합 구조로 끌어올리는 설계 정본이다.
이 문서는 도입 로드맵(P0~P4) 중 **P0(채널 정책 게이트)**·**P1(인증 자료 경계 재편)**·
**P2(Sec-Agree 협상)**·**P3(IMS AKA over TLS)**·**P4(IMS AKA+IPsec)** 의 상세 설계를 담는다.

> **상태**: P0·P1·P2·P3 구현 반영(P2·P3 = 서버·CSC·cspsim·verify·**Android UE** — pjsip 패치(sec-agree echo·
> AKA/OPc)와 `/provisioning/me` `account.aka`/`sip.security` 단말 연결 포함, §8.1·§8.2). P4 = 서버(CSP)·psip/cspsim·
> agent·CSC·verify 구현 반영 — 범위 고정: Annex M 미포함(NAT 와 두 겹 상호배제), psip/cspsim 한정. 실설치 검증
> (S3-SCN-IPSEC-LIVE)은 `CAP_NET_ADMIN`·IPSEC LocalNode·AKA 마이그레이션이 있는 환경에서만 돈다. [§4.7 배포 순서](#47-소비자-전환--프로비저닝시험-도구) 의 ⑤(코드)까지
> 반영 — CSC 는 `passwd` 컬럼에 쓰지 않고 `/provisioning/me` 의 `sipPassword` 는 항상 `null` 이다.
> 값 소거(⑤-c)는 배포 환경별 운영 절차, ⑥ 컬럼 DROP 은 `sql/migrate_subscription_drop_passwd.sql`
> (후속 릴리스 — CSP/cspsim 이 `passwd` 를 SELECT 하지 않게 된 뒤 적용).

관련 문서: [sip_tls_signaling.md](sip_tls_signaling.md) · [registration_binding_set.md](registration_binding_set.md) ·
[ue_nat_traversal.md](ue_nat_traversal.md) · [../modules/csp.md](../modules/csp.md) ·
[../modules/csc.md](../modules/csc.md) · [../identifier_model.md](../identifier_model.md)

## 1. 표준 근거 — 지원 조합은 표준 조합으로 한정한다

TS 33.203 이 정의하는 접속 보안 조합만 지원한다. 자유 조합(예: AKA+평문 UDP)은 만들지 않는다.

| 조합 | 근거 | transport | CIMS 도입 단계 |
|---|---|---|---|
| SIP Digest (평문) | TS 33.203 Annex N | UDP/TCP | 현행 |
| SIP Digest + TLS | Annex N+O | TLS/TCP | 현행(게이트 없음) → **P0 에서 완성**, 협상·강등 방지는 **P2** |
| IMS AKA + TLS | Annex X | TLS | **P3 — 구현 반영**([§8.2](#82-p3--ims-aka-over-tls-annex-x--구현-반영)) |
| IMS AKA + IPsec | 본문 §6~7 | UDP/TCP (SA 공용) | **P4 — 구현 반영**([§8.3](#83-p4--ims-aka--ipsec-본문-67--구현-반영)), NAT(Annex M) 미포함 |

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
| 과도기 | 같은 테이블 `passwd` — CSC 는 더 쓰지 않고(구 스키마 fallback 제외), CSP 는 ha1 이 비었을 때만 읽는다 | 평문 (값 소거 → 후속 릴리스 DROP) |
| CSP 메모리 | `CspUser::m_strHa1` (`csp/CspUser.h`) — 전 가입자 캐시 | H(A1) |
| 검증 지점 | `CCscfModule::CheckAuthorizationResponse` (`csp/CscfModule.cpp`) — 저장 H(A1) 로 response 합성 | H(A1) 소비 |
| 쓰기 경로 | CSC `_add_subscription`/`_update_subscription` (`csc/src/handlers/admin.py`) — 요청 본문의 `passwd` 를 H(A1) 로 변환 | 평문은 요청 본문에만 |
| 단말 전달 | `/provisioning/me` 응답 `services[].account.sipHa1` (`csc/src/services/mcptt.py`) | H(A1) (`sipPassword` 는 항상 `null`) |
| 시험 도구 | cspsim `-db`(DB `ha1` 우선) / `-ha1 <hex>`, verify 시드 `VOIP_HA1`/`PTT_HA1` | H(A1) |
| **AKA** DB SoT | `*_subscriptions.auth_scheme`/`k_enc`/`opc_enc`/`sqn`/`amf` (`sql/migrate_subscription_aka.sql`) | K/OPc 는 CSC `AuC.Kek` 로 **암호화 보관**, SQN 은 CSC 만 갱신 |
| **AKA** 발급자 | CSC `services/auc/` — `POST /internal/aka/av` (§8.2) | AV(RAND·AUTN·XRES·CK·IK)만 나간다 |
| **AKA** CSP 메모리 | `CNonceInfo` (nonce=base64(RAND‖AUTN) 에 XRES·발급 신원 결부, nonce 수명) | XRES — 검증 직후 폐기 |
| **AKA** 단말 전달 | `/provisioning/me` `account.aka{k,opc,amf}` (`authScheme:"aka"`) — 소프트-K | 평문 K/OPc(토큰 인증 + TLS 채널 전제) |

> **CSP 의 AV 조회 HTTPS 클라이언트는 SIP TLS 리스너와 독립**이어야 한다 — psip `SSLConnect` 가 클라이언트
> `SSL_CTX` 를 첫 사용 시 지연 생성한다(`SSLEnsureClientCtx`, CSipMutex 보호). 종전엔 부팅 시 primary TLS
> 접속점(stack-global TLS 기동)이 있어야만 ctx 가 만들어져, per-listener 인증서만 쓰거나 TLS 접속점을
> hot-add 한 노드에서 `[auc] AV request failed … (connect/timeout)` → AKA 챌린지 504 가 났다(dev S3 실측,
> TcpConnect 성공 후 `SSL_new(NULL)` 무로그 실패). 라이브가 동작한 것은 tls-lab 이 primary 였기 때문이다.
> 클라이언트 ctx 는 서버 인증서를 검증하지 않는다(내부 API 인증 = Bearer 토큰).

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
  한다(`sql/migrate_subscription_drop_passwd.sql` — CSP/cspsim 의 `passwd` SELECT 제거 릴리스에서
  적용). CSP 는 `ha1` 우선, 비어 있으면 `passwd` 로 종전 계산을 한다.
- AKA 용 컬럼(`auth_scheme`, `k_enc`, `opc_enc`, `sqn`, `amf`)은 `sql/migrate_subscription_aka.sql` 이
  추가한다([§8.2](#82-p3--ims-aka-over-tls-annex-x--구현-반영)) — CSP/CSC 는 `auth_scheme` 컬럼을 같은 방식으로
  프로브해 미적용 DB 에서는 전 가입자를 digest 로 취급한다.

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
aka    →  { rand, autn, xres, ck, ik }            // P3: CSC HTTP API POST /internal/aka/av — SQN 단일 발급자 (§8.2)
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
| `/provisioning/me` | `account.sipHa1` (H(A1)). `account.sipPassword` 는 항상 `null`(평문 미배포 — 키는 단말 호환으로 유지). 단말은 `sipHa1` 우선, 없으면 로그인 비번으로 ha1 계산 |
| Android UE | `sipHa1` 수신 시 pjsip cred 를 `PJSIP_CRED_DATA_DIGEST`(ha1) 로 설정한다(`android/core` SipController — cred realm 은 `*` 유지, pjsip 이 DIGEST cred 의 algorithm 미지정을 MD5 로 기본화). `sipHa1` 이 없으면 평문 cred(`sipPassword` → 로그인 비번) 폴백 — H(A1) 은 realm 에 결박된 값이라 challenge realm 추종이 필요한 상황은 평문 경로만 흡수한다. 수동 설정에서 도메인/IMSI/IMPI/비번을 편집하면 결박이 깨진 저장 ha1 을 함께 소거한다 |
| cspsim | `-db` 모드는 DB `ha1`(평문 컬럼은 없다 — 비면 등록 불가), CLI `-ha1 <hex32>` 로 직접 지정. **`-creds <file>`** = 단말별 자격 파일(JSONL: `user`/`ha1`/`authId`/`password`/`k`/`opc`/`sqn` + IdMS 로그인 `login`/`loginPw` — XCAP 토큰은 SIP 자격이 아니라 이 로그인 자격으로 받는다) — `-count` 전개 단말 각각에 자기 자격을 주며(`-users_from_creds` 면 파일의 user 순서가 로스터), 전개 user 가 파일에 없으면 기동 전 즉시 중단(fail fast). psip 클라(`CSipServerInfo::m_strHa1`, `MakeA1`)가 H(A1) 입력을 받고, 등록 스레드는 비밀번호가 비어도 ha1 이 있으면 자격으로 인정한다(passwd 소거 후 ha1 단독 등록). `-password` 는 유지(직접 계산) |
| verify 하네스 | `subscribers.py` 가 "전원 `ha1` 보유 + UDP Digest(채널 정책·AKA 가입자 제외)" 창(window)을 골라(번호 연속 불요 — cspsim `-users_from_creds`) 단말별 자격(`{KIND}_CREDS`)을 시드하고, 시험 항목은 `cred_args()` 가 쓴 JSONL 자격 파일로 `-no-db -creds` 전개한다(`-no-db` 인 이유 = DB 모드는 `-user` 를 무시하고 DB 첫 N 행을 쓴다). `ha1` 이 빈 가입자는 시드 대상이 아니고 `-password` 폴백은 없다 |

**배포 순서가 계약이다**:
① 스키마 — `migrate_subscription_transport.sql` + `migrate_subscription_ha1.sql` (컬럼 추가만,
구 코드에 무해·재실행 안전). `sip_transport` 는 CSP SELECT 의 전제이고, `ha1` 은 프로브로 흡수되므로
②와 순서가 바뀌어도 기동은 되지만 그동안 H(A1) 저장이 일어나지 않는다 →
② CSP/CSC/cspsim/단말 앱의 ha1 소비 코드 배포 →
③ `migrate_subscription_ha1.py` 로 기존 행 ha1 일괄 계산 →
④ 전 조합 등록 회귀(§6) →
⑤ `passwd` 값 소거(CSC 의 `passwd` 병행 쓰기 제거 + `/provisioning/me` 의 `sipPassword` 항상 null) →
⑥ 컬럼 DROP — `sql/migrate_subscription_drop_passwd.sql`(멱등). 현행 코드는 `passwd` 컬럼을 읽지도 쓰지도
않는다(CSP `DbManager` SELECT·cspsim `-db`·CSC admin·verify 시드 전부 `ha1` 단독) — DB 가입자의 Digest 자격은
`ha1` 뿐이며 `ha1` 컬럼이 없는 DB 에서는 CSP 가 ERROR 로그와 함께 인증 불가, CSC 는 자격 저장을 503
`schema_not_migrated` 로 거부한다. 평문 경로는 JSON 파일 fallback(`csp/User`)과 cspsim CLI/`-creds` 의 `password`
에만 남는다.

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
| V9 | sec-agree 정상 협상 — `Security-Client`+`Require` → 401(`Security-Server`) → TLS 위 `Security-Verify` echo | 200, `Service-Route` 에 `;transport=tls`. 등록 유지 중 같은 신원의 UDP 요청 403(게이트 합류), 해제 후 401 |
| V10 | `Security-Verify` 변조(강등 시뮬레이션) | 494 + 새 `Security-Server` |
| V11 | `Require: sec-agree` 만 있고 `Security-Client` 없음 | 494 |
| V12 | 협상 후 `Security-Verify` 생략 | 494 |
| V13 | `Setup.SecAgree.Require=true` 에서 TLS 정책 가입자의 협상 없는 초기 REGISTER | 421 + `Security-Server` (설정 의존 — 수동) |
| V14 | CSC 내부 AV API — 토큰 없음 / 발급 / AUTS 재동기 / AUTS 변조 | 401 / 200(XRES·MAC-A 가 K·OPc·SQN 과 정합, SQN+1) / 200 `resynced` + DB `sqn`=SQN_MS+1 / 422 |
| V15 | AKA 가입자의 TLS REGISTER — 401(`algorithm=AKAv1-MD5`, nonce=base64(RAND‖AUTN)) → RES 답안 | 200, `Service-Route` `;transport=tls` |
| V16 | 틀린 K(AUTN MAC 불일치) — 단말이 빈 `response` 로 보고 | 403 |
| V17 | 단말 SQN_MS 가 앞서 있음 — `auts` 동봉 → 서버 재동기 → 새 401 → 답안 | 200, DB `sqn` = SQN_MS+1 |
| V18 | AKA 가입자의 UDP REGISTER (sip_transport 와 무관, `Security-Client` 없음) | 403 — 보호 채널 밖. (sec-agree 제안을 실은 초기 REGISTER 는 협상 부트스트랩이라 통과한다 — V19) |
| V19 | AKA 가입자의 `ipsec-3gpp` 제안 + NAT 흔적(top Via `sent-by` 사설 IP ≠ 실소스) 초기 REGISTER | 401 의 `Security-Server` 에 `ipsec-3gpp` 없음(`tls` 만) + `ipsec: nat detected` 로그. 같은 가입자의 제안 없는 UDP REGISTER 는 403. 환경에 IPSEC LocalNode·CSP ipsec 가용이 있으면 NAT 흔적 없는 제안은 `ipsec-3gpp` 첫 항목(양성 대조) — 커널 특권 불필요 |
| V20 | Digest 가입자의 `ipsec-3gpp` 제안 | `Security-Server` 에 `tls` 만 — 커널 특권 불필요 |
| V21 | AKA 가입자 cspsim `-ipsec` 등록 — 401(`Security-Server` ipsec-3gpp spi/port) → ESP 위 답안 | 200 (`ipsec(ue): registered over SA`), 서버 로그 `ipsec: sa set established` |
| V22 | `Security-Verify` 변조(`-sec_verify`) | 494 + 임시 SA 회수 — 등록 실패 |
| V23 | 해제(`Expires: 0`) | 200, 단말·서버 SA 회수 로그 |
| V24 | `sec_mechanisms` 에 `ipsec-3gpp` 이면서 `media_nat_mode≠off` 인 access service | 로드 시 `ipsec-3gpp` 무시 + ERROR (서비스 유지) — CSP 로그로 확인(수동) |
| V25 | IPsec 등록 유지 중(cspsim `-hold`) 같은 신원의 비보호 포트(5060) MESSAGE / 제안 없는 REGISTER | 둘 다 403 — 보호 채널 밖(비-REGISTER 는 flow 밖 403, REGISTER 는 정책 게이트) |
| V26 | cspsim `-ipsec -transport tcp` 등록 | 200 + `registered over SA … (tcp)` — 단말 발신 연결이 port_uc → port_ps 로 맺힘(소스포트 bind), 서버 발신은 port_pc → port_us |

**V1·V2·V7 은 S3 검증 항목으로 자동화되어 있다** — 원시 SIP 프로브(`verify/lib/common/sip_probe.py`):
- `S3-SCN-CHANNEL-POLICY`(V1·V2): 대상 가입자 `sip_transport` 를 DB 에서 `TLS` 로 올리고 CSP 4421
  `USER_CHANGED` 통지로 캐시를 갱신(`ReloadFromDb`)한 뒤, UDP REGISTER→403(V1)·UDP MESSAGE→403(V2)을
  확인하고 원값으로 되돌린다(자기복원). 정책 부여 전 같은 프로브가 401 인 대조를 함께 실어 403 이
  게이트임을 증명한다. **V2 를 INVITE 가 아닌 MESSAGE 로 재현하는 이유**: dev 의 `TestEnvOpenTermination`
  이 착신이 로컬 가입자/그룹인 INVITE 를 게이트 앞에서 통과시키고(수신통화 허용), SDP 없는 INVITE 는
  미디어 협상에서 488 로 조기 거절되어 게이트에 도달하지 않는다 — MESSAGE 는 두 우회를 피해 게이트를
  직접 탄다(상용은 flag off 라 INVITE 도 게이트 대상). "인증보다 먼저" 는 첫 최종응답이 401 이 아니라
  403 인 것으로 판정한다.
- `S3-SCN-REALM-MISMATCH`(V7): REGISTER→401 챌린지로 서버 realm/nonce 를 얻어 **틀린 realm** Digest 로
  재전송 → 401 재챌린지(response 검증 전 realm 대조)를 확인한다.
- `S3-SCN-SEC-AGREE`(V9~V12): `sip_probe.SecAgreeTlsSession` 이 TLS 연결 하나를 열어 두고 초기
  REGISTER → 401 → `Security-Verify` 재-REGISTER 를 수행한다. 연결을 유지한 채 UDP MESSAGE 프로브로
  게이트 합류(403)를 보고, `Expires: 0` 으로 해제한 뒤 401 복원을 확인한다. 변조/제안 없음/Verify
  생략은 각각 새 연결에서 494 를 본다(등록이 성립하지 않으므로 잔여 없음). V13 은 설정 의존이라 수동.
- `S3-SCN-AKA`(V14~V18): 시드 VoLTE 가입자를 TS 35.208 시험 K/OPc 로 `auth_scheme=aka` 프로비저닝(CSC 와
  같은 keystore 형식으로 DB 직접 기록 + `USER_CHANGED`)한 뒤 내부 AV API → UDP 게이트 → TLS AKA 등록/해제 →
  틀린 K → SQN 재동기 순으로 보고, 원 인증 자료(5 컬럼)를 복원한다. 단말 계산(`sip_probe.aka_answer`)은
  CSC 의 Milenage(`csc/src/services/auc/milenage.py`)를 소프트-USIM 으로 빌려 CSP↔CSC↔단말 세 변의 정합을
  본다. psip/cspsim 의 OpenSSL Milenage 는 `tests/psip_aka_test.cpp` 가 같은 벡터로 검증한다. 전제(마이그레이션
  미적용·`AuC.Kek`/`InternalApi.Token` 미설정)가 없으면 SKIP.

- `S3-SCN-IPSEC`(V19·V20): `sip_probe.probe_register_offer` 가 `Security-Client: ipsec-3gpp;…` 를 실은 초기 REGISTER 를
  보내고 `Security-Server` 를 읽는다(Via sent-by 를 꾸며 NAT 판정을 재현). V19 는 시드 가입자를 AKA 로 잠시 올렸다가
  복원한다(AKA 컬럼이 없으면 V19 만 생략). `S3-SCN-IPSEC-LIVE`(V21~V23·V25·V26): cspsim `-ipsec -aka_k …` 를 구동해 단말
  로그로 판정한다 — IPSEC LocalNode·CSP `ipsec: available`·cspsim `cap_net_admin`·AKA 마이그레이션이 없으면 SKIP.

- `S3-SCN-TLS-REBIND`(V3)·`S3-SCN-MIXED-TRANSPORT`(V4): 순수 TLS Digest 등록(sec-agree 헤더 없이) —
  V3 는 연결을 끊고 새 연결에서 재등록해 바인딩 이동을, V4 는 UDP 바인딩 유지 중 TLS 등록으로
  혼합 공존(게이트 미작동)을 본다. dev TB 에 TLS 접속점이 없으면 항목이 임시 TLS local_node 를
  추가(SIGUSR1 hot-add)하고 종료 시 제거한다(자기복원, dist csc 자가서명 cert 차용).
- `S3-SCN-AKA-MIGRATE-IDEMPOTENT`(V6): `migrate_subscription_aka.sql` 2회 실행(둘 다 성공 —
  컬럼 존재 시 no-op) + 시드 가입자 ha1 보존 + 재등록 401→200. 가입자 PUT 경유 ha1 보존은
  CSC 관리 API 경로라 별도(§8.4 잔여).
- `S3-SCN-PROVISIONING-HA1`(V8): IdMS PKCE 토큰(scope=`cims:provisioning`) → `/provisioning/me` 의
  `sipHa1` 존재·평문 비밀번호 값 부재(`sipPassword` 키는 항상 null — §4.7 ⑤) → 그 ha1 로
  REGISTER 401→200(단말 부트스트랩 등가). 시드 가입자의 로그인 계정(`users.login_id`)이 없으면 SKIP.

V5(등록)는 기존 transport 별 등록 스모크가 커버하고, V5 의 **호** 부분(TCP/TLS 호 회귀)도
cspsim 이 call INVITE 를 세션 transport 로 보내(§7 보완 완료) UDP/TCP/TLS 전 조합에서 성립한다.
반복 위반 알람은 A-SEC-003 으로 채번·구현되어 있다(§3.3).

## 7. 배포 순서와 잔여

P0 의 게이트 자체는 DB 변경이 없지만, 이 릴리스의 CSP 는 `sip_transport` 컬럼을 읽으므로
`migrate_subscription_transport.sql` 이 CSP 기동의 선행 조건이다(`ha1`·`auth_scheme` 은 프로브로 흡수 —
`migrate_subscription_aka.sql` 미적용 DB 에서는 AKA 가입자가 없을 뿐 기동·Digest 는 그대로다). P1 은 배포 순서 계약(§4.7 ①~⑥)이 단계를
강제한다. ②(ha1 소비)·⑤(passwd 미기록·`sipPassword` null)·⑥(코드의 `passwd` 컬럼 의존 제거) 코드가 반영되어 있고, ①·③·⑤-c(값 소거)·⑥ 스크립트(`sql/migrate_subscription_drop_passwd.sql`) 적용은 배포 환경별 운영 절차다 — 이 릴리스의 CSP 는 `passwd` 를 SELECT 하지 않으므로 DROP 은 코드 배포 뒤 언제든 적용할 수 있다.

P3 의 배포 전제: `migrate_subscription_aka.sql` 적용 → `configure` 재실행(`csc.json AuC.Kek`·
`InternalApi.Token`, `csp.json Setup.Csc.*` 렌더 — **`AuC.Kek` 는 기존 값을 이어받는다**, 바꾸면 보관된 K/OPc
전량 복호 불가) → CSC·CSP 재기동 → 가입자 `auth_scheme=aka` + `k`/`opc` 프로비저닝(CSC API).

잔여 항목:
- V6 의 가입자 PUT 절반(관리 API 로 비밀번호 미포함 갱신 → ha1 보존) — 나머지 V 항목은 S3 로
  자동화 완료(§6).
- Android UE 의 AKA·sec-agree 는 단말 연결까지 구현(§8.1 클라이언트·§8.2 Android UE 절 — pjsip 패치 +
  `account.aka`/`sip.security` 소비). **실기기 등록 검증이 잔여** — AKA 계정 프로비저닝(`auth_scheme=aka`,
  콘솔 번호 행 인증 열) 후 TLS 등록 200·비-TLS 403 확인. `Setup.SecAgree.Require` 는 구 APK 수용을 위해
  false(제안하는 단말만 협상) 유지.
- AKA 의 CK/IK 는 Annex X(TLS) 에서 쓰이지 않는다 — P4(IPsec, [§8.3](#83-p4--ims-aka--ipsec-본문-67--구현-반영)) 에서 SA 키로 소비한다.
- `/provisioning/me` 의 `enforced`/TLS 목록 축소는 CSC `Provisioning.Services.*.tls_port` 가 설정된 환경에서만
  드러난다(미설정이면 TLS 항목 자체가 없다).

## 8. 로드맵 (P2~P4)

P2·P3·P4 모두 구현 반영.

### 8.1 P2 — Sec-Agree (RFC 3329) — 구현 반영

보안 메커니즘 협상과 강등(bidding-down) 방지. TS 24.229 §5.1.1.5.1/§5.2.2 프로파일을 따른다
(P/S-CSCF 가 한 프로세스이므로 `Security-Server` 는 S-CSCF 의 401 에 실린다).

```
UE                                   CSP
 │ REGISTER (초기)                     │
 │  Security-Client: tls               │
 │  Require/Proxy-Require: sec-agree   │
 │────────────────────────────────────▶│  ParseSecAgree → gclsSecAgreeMap.Issue(user)
 │ 401 Unauthorized                    │
 │  WWW-Authenticate: Digest …         │
 │  Security-Server: tls;q=0.1         │
 │◀────────────────────────────────────│
 │ (TLS 연결 위) REGISTER              │
 │  Authorization: Digest …            │
 │  Security-Client / Require 반복     │
 │  Security-Verify: tls;q=0.1  ← echo │
 │────────────────────────────────────▶│  인증 통과 → transport==TLS 확인 →
 │ 200 OK                              │  Verify(user, echo) 바이트 대조 →
 │  Service-Route: <…:5061;transport=tls;lr>  바인딩 integrity-protected 결부
 │◀────────────────────────────────────│
```

**서버 규칙** (`csp/SecAgree.{h,cpp}`, `CCscfModule::RecvRequestRegister`):

| 상황 | 응답 | 근거 |
|---|---|---|
| `Require: sec-agree` 인데 `Security-Client`·`Security-Verify` 둘 다 없음 | 494 + `Security-Server` | RFC 3329 §2.2 — 협상할 재료가 없다 |
| `Setup.SecAgree.Require=true` 이고 TLS 정책(`sip_transport=TLS`) 가입자가 협상 없이 초기 REGISTER | 421 + `Security-Server` | 정책이 협상의 하한 — 강등 진입 자체를 막는다 |
| 초기 REGISTER 에 `Security-Client` | 401 에 `Security-Server: tls;q=0.1` 동봉, 발급 원문 보관(user 키, nonce 와 같은 수명) | TS 24.229 §5.2.2 |
| 인증 통과한 재-REGISTER 에 sec-agree 헤더가 있는데 `Security-Verify` 없음 | 494 | RFC 3329 §2.1 — 협상 후 모든 REGISTER 는 Verify 동봉 |
| 인증 통과했으나 **TLS 가 아닌** 채널 | 494 | 협상 결과(tls)를 지키지 않은 요청 |
| `Security-Verify` ≠ 발급 원문(바이트 대조) 또는 발급 기록 없음 | 494 + 새 `Security-Server` | RFC 3329 §2.3 — 강등/변조 → 재시작 |
| 대조 통과 | 200, 바인딩 `m_bIntegrityProtected=true` | 3GPP integrity-protected 상당 내부 플래그 |

대조는 **인증 뒤**에 둔다 — 미인증 요청이 494 로 발급 상태를 흔들지 못하게 한다. 서버 목록은
`tls` 뿐이다(`ipsec-3gpp` 는 P4 — [§8.3](#83-p4--ims-aka--ipsec-본문-67--구현-반영) — 에서 파라미터와 함께 추가). 등록 해제(`Expires: 0`)
시 발급 기록을 지운다.

**게이트 합류** (`CCscfModule::CheckChannelPolicy`): TLS 강제의 근거가 둘이 된다 — 정책 축
(`sip_transport=TLS`)과 협상 결과 축(integrity-protected 바인딩이 살아있음,
`CUserMap::IsIntegrityProtected`). 어느 쪽이든 비-TLS 채널의 그 신원 요청은 403 이며 로그가
근거(`policy`/`sec-agree`)를 구분한다. 협상으로 TLS 를 결부한 단말이 평문으로 돌아오는 것이 곧
강등이므로 REGISTER 도 예외가 아니다(TLS 재접속·재등록은 V3 경로).

**Service-Route**: 스트림 transport 로 등록한 단말에는 `;transport=tcp|tls` 를 명시하고 포트는 그
리스너의 것을 쓴다(`CspAddressing::GetLocalSipPortForTransport` — Setup 기동 primary TCP/TLS 리스너는
id 0 이라 transport 별 Setup 포트로 폴백). 없으면 route 를 따르는 후속 요청이 UDP 로 강등되어 게이트에
걸린다(RFC 3608/TS 24.229).

**설정**: `Setup.SecAgree.Require`(bool, 기본 false, SIGUSR1 재로드). false 는 단말이 제안할 때만
협상한다 — sec-agree 를 못 만드는 단말(현 Android pjsip)을 수용하는 운영값. `Security-Verify`
불일치 494 는 설정과 무관하게 항상이다.

**관측**: 거절은 `sec-agree reject user=<id> transport=<t> src=<ip>:<port> → 494|421 (<사유>)` 로그. 반복 거절
(강등 공격/단말 오설정 탐지)은 **A-SEC-004**(`security_violation`, X.736 — [alarm_catalog](../../alarm_catalog.md))
알람이다: `SendSecAgreeReject` 의 494/421 전 건을 `SipStatsMonitor` 가 A-SEC-003 과 같은 소스 계수기로 세고
(소스 IP 상위 32), `Setup.SipStats.EvalSec` 윈도우당 건수가 `Setup.SipStats.SecAgreeRejectMajor`(기본 10, 0=off)
이상이면 major 발화(mo `<서버명>/csp/sec_agree`, params count/window/ip/top_count), 미만 윈도우에서 해소한다.
monitor `sip_stats` 에 누적 `sec_agree_reject` 와 `window sec_agree_reject` 가 노출된다.

**클라이언트**: psip `CSipServerInfo::m_bSecAgree`(+`m_strSecurityClient`/`m_strSecurityServer`/
`m_strSecurityVerifyOverride`) — REGISTER 에 `Security-Client`/`Require`/`Proxy-Require` 를 싣고,
응답(401/494/421)의 `Security-Server` 원문을 보관해 다음 REGISTER 의 `Security-Verify` 로 echo 한다.
cspsim `-sec_agree`(+`-sec_verify <값>` 강등 변조 재현). **Android UE**: pjproject `sip_reg.c` 패치가
같은 규율을 구현 — 앱(`SipController`)이 TLS 접속 + 프로비저닝 `sip.security` 에 `tls` 가 있을 때
`Security-Client: tls` + `(Proxy-)Require: sec-agree` 를 REGISTER 헤더로 싣고, regc 가 401 의
`Security-Server` 를 보관해 인증 재시도·갱신 REGISTER 에 `Security-Verify` 로 echo 한다
(Security-Client 가 있는 요청에서만 동작 — 구 APK/미제안 단말은 종전 그대로).

**검증**: §6 V9~V13, `S3-SCN-SEC-AGREE`.

### 8.2 P3 — IMS AKA over TLS (Annex X) — 구현 반영

**IPsec 없이 AKA 를 도입하는 표준 경로.** 상호 인증·키 신선도를 TLS 채널 위에서 얻는다. P0 게이트(TLS 강제)가
선행 조건이며, CSC 가 HSS/AuC 로 승격되어 인증 벡터(AV)의 **단일 발급자**가 된다.

**체계 결정 — 협상이 아니라 프로비저닝.** Cx 의 `SIP-Authentication-Scheme` 처럼 가입자 행의 `auth_scheme`
(`digest`|`aka`)이 CSP 의 챌린지 체계를 정한다. Annex P.4 의 "판별" 은 이 값과 단말 답안의 `algorithm` 을
대조하는 것으로 환원되고, "shall not change" 는 nonce 에 체계를 기록해 두고 답안의 체계와 다르면 거부하는
것으로 고착된다. 프로파일은 3GPP 가 IMS 에 지정한 **AKAv1-MD5**(RFC 3310, TS 24.229 §5.1.1.5.1 — pjsip 등
실단말이 구현하는 알고리즘)이다. AKAv1 의 약점(RES 만으로 응답)은 TLS 가 채널을 보호하므로 Annex X 조합
안에서 문제가 되지 않는다. AKA 가입자는 `sip_transport` 값과 무관하게 **TLS 채널 강제 대상**이다
(`CspUser::requiresTls()` = `sip_transport=TLS` ∨ `auth_scheme=aka` — §3 게이트가 그대로 집행).

```
UE (soft-USIM: K/OPc)          CSP (S-CSCF)                          CSC (HSS/AuC)
 │ REGISTER (TLS, 무인증)        │                                       │
 │─────────────────────────────▶│ auth_scheme=aka → POST /internal/aka/av│
 │                              │──────────────────────────────────────▶│ SELECT … FOR UPDATE
 │                              │  {av:{rand,autn,xres,ck,ik}}           │ SQN_HE+1, RAND 신선, Milenage
 │ 401  WWW-Authenticate:       │◀──────────────────────────────────────│
 │   Digest realm, algorithm=AKAv1-MD5, qop="auth",                     │
 │   nonce=base64(RAND‖AUTN)    │  nonce→(XRES, 신원) 보관 (CNonceMap)   │
 │◀─────────────────────────────│                                       │
 │ Milenage: AK=f5, SQN=AUTN⊕AK, XMAC=f1 ?= MAC, SQN>SQN_MS ?           │
 │ REGISTER  Authorization: Digest … algorithm=AKAv1-MD5,               │
 │   response=MD5-Digest(H(A1)=MD5(impi:realm:RES))                     │
 │─────────────────────────────▶│ H(A1)=MD5(impi:realm:XRES) 로 대조     │
 │ 200 OK  Service-Route ;transport=tls                                 │
 │◀─────────────────────────────│                                       │
```

예외 흐름(RFC 3310 §3.4, TS 24.229 §5.1.1.5.3 / §5.4.1.2.2):

| 단말 상황 | 단말 답안 | CSP |
|---|---|---|
| AUTN MAC 불일치(K 다름) | `response=""`, `auts` 없음 | 403 |
| SQN 이탈(SQN ≤ SQN_MS) | `auts="base64((SQN_MS⊕AK*)‖MAC-S)"` + 빈 password 로 계산한 response | CSC 에 `{rand, auts}` 재동기 요청 → SQN_HE := SQN_MS → 새 AV 로 **다시 401** (단말은 그 401 을 실패가 아닌 새 챌린지로 처리) |
| AUTS 의 MAC-S 불일치 | — | 403 (`auts_invalid`) |

**CSC — AuC (`csc/src/services/auc/`)**

- `milenage.py`: TS 35.205/206 f1·f1*·f2·f3·f4·f5·f5* (순수 python AES-128 `aes128.py` — vendor 에 암호 라이브러리가
  없다). `keystore.py`: K/OPc 보관 형식 `v1:<iv><ct><hmac>`(AES-128-CTR + HMAC-SHA256, KEK = `csc.json AuC.Kek`,
  hex32 또는 SHA-256 정규화). `auc.py`: 발급·재동기(행 잠금 트랜잭션, `sqn` 갱신은 여기서만).
- 스키마(`sql/migrate_subscription_aka.sql`, volte/ptt 동일): `auth_scheme ENUM('digest','aka') DEFAULT 'digest'`,
  `k_enc`/`opc_enc VARCHAR(160)`, `sqn BIGINT UNSIGNED`(48-bit SQN_HE), `amf CHAR(4) DEFAULT '8000'`.
- 프로비저닝(`admin.py` POST/PUT `…/{kind}[/{msisdn}]`): `auth_scheme`, `k`(hex32), `opc`(hex32) 또는 `op`(→ OPc
  유도), `amf`. 키를 넣으면 `sqn=0`. `aka` 로 바꾸는데 보관 키가 없으면 400. 조회는 `auth_scheme`·
  `aka_provisioned` 만 — **K/OPc 원문은 어떤 API 응답에도 없다.** KEK 미설정이면 503(평문 보관 fallback 없음).
- 내부 AV API(`handlers/auc_api.py`, admin 서버 4421 — `/api/v1` 밖이라 OAM 게이트웨이가 프록시하지 않는다):

```
POST /internal/aka/av            Authorization: Bearer <InternalApi.Token>
{ "msisdn": "+82…", "service": "volte"|"ptt"|"", "rand": "<hex32>", "auts": "<hex28>" }   // rand/auts 는 재동기만
200 { "scheme":"aka", "msisdn", "service", "resynced":bool, "av": { "rand","autn","xres","ck","ik" } }   // hex
401 토큰 불일치 · 404 unknown_subscriber · 409 scheme_mismatch|keys_not_provisioned · 422 auts_invalid ·
500 key_material(KEK 불일치) · 503 auc_disabled|schema_not_migrated
```

  토큰은 configure 가 `csc.json InternalApi.Token` 과 `csp.json Setup.Csc.InternalToken` 에 같은 값으로 렌더한다
  (`@INTERNAL_TOKEN@`). `AuC.Kek`(`@AUC_KEK@`)는 JWT 시크릿과 달리 **재생성하지 않는다** — configure 는 기존
  `csc.json` 값을 이어받고 없을 때만 만든다.
- SQN 관리(TS 33.102 Annex C): 발급마다 `SQN_HE+1`, 재동기는 AUTS 의 MAC-S(AMF*=0000) 검증 후 `SQN_HE := SQN_MS`.
  단말(소프트-USIM)은 `SQN > SQN_MS` 단조 규칙으로 신선도를 판정한다(배열/윈도우 없음 — 단일 단말 전제).

**CSP (`csp/CscfModule.cpp`, `csp/CscAvClient.{h,cpp}`, `csp/NonceMap.{h,cpp}`)**

| 상황 | 동작 |
|---|---|
| 초기/재 REGISTER 챌린지 | `SendRegisterChallenge` — From 가입자가 `aka` 면 `SendAkaChallenge`(CSC AV → nonce=base64(RAND‖AUTN), `algorithm=AKAv1-MD5`, `qop="auth"`, `CNonceMap::InsertAka(nonce, 신원, RAND, XRES)`), 아니면 Digest 401 |
| CSC 미도달·미설정·타임아웃 | 504 Server Time-out (HSS 미도달 상당 — 단말 재시도) |
| CSC 409(캐시 불일치) | 가입자 캐시 `ReloadFromDb` 후 그 체계로 재챌린지 |
| 답안 검증 | nonce 의 체계 ≠ 가입자 체계 → 403(고착) · nonce 발급 신원 ≠ From → 403 · `algorithm≠AKAv1-MD5` → 403 · `auts` 있음 → `E_AUTH_AKA_RESYNC`(CSC 재동기 → 새 401) · `response` 비고 `auts` 없음 → 403(MAC 실패 보고) · H(A1)=MD5(impi:realm:XRES 이진) 로 Digest 대조, qop/nc 재사용 규칙은 Digest 와 동일 |
| 비-REGISTER | 등록이 결부한 TLS flow(ip,port) 위의 요청은 종전대로 통과(`TouchFlow`). 그 flow 밖(미등록·새 연결)의 AKA 가입자 요청은 Digest 챌린지 대신 **403** — 단말은 그 연결에서 REGISTER 부터(Annex O/X) |
| 채널 | `requiresTls()` 가 `aka` 를 포함하므로 UDP/TCP 의 AKA 신원 요청은 REGISTER 포함 403(§3 게이트, A-SEC-003 계수 동일) |
| ik/ck | P/S-CSCF 가 한 프로세스라 401 에 싣지 않는다(P-CSCF 가 제거하는 파라미터). TLS 위에서는 소비처가 없다 |

설정 `Setup.Csc.{Host,Port,Scheme,InternalToken,TimeoutMs}`(config_template `csc_internal` = "CSC 연동 (내부 API)",
SIGUSR1 재로드). `Host` 비면 LocalIp. 같은 자격으로 단말용 MCPTT 서비스 주소도 취득한다
(`GET /internal/mcptt/endpoint` → xcap-diff NOTIFY 의 `xcap-root`, `CscEndpointCache`). DB 는 `auth_scheme`
컬럼을 프로브(`CDbManager::m_bHasAkaColumns`)해 미적용이면 전원 digest.

**단말**

- psip `SipAka.{h,cpp}`(OpenSSL AES) + `CSipServerInfo::m_strAkaK/m_strAkaOpc/m_iAkaSqnMs`: `AddAuth` 가
  `AKAv1-MD5` 챌린지에 Milenage 로 답한다(MAC 실패 → 빈 response, SQN 이탈 → `auts` + `m_bAkaResyncSent` 로
  다음 401 을 새 챌린지로 처리). K 도 등록 자격이다(`SipRegisterThread`).
- cspsim `-aka_k`/`-aka_opc`/`-aka_sqn`(초기 SQN_MS — 큰 값으로 재동기 재현), `-creds` JSONL 의 `k`/`opc`/`sqn`.
  `-transport tls` 필수.
- `/provisioning/me`: `account.authScheme` (`digest`|`aka`), `aka` 면 `account.aka={k,opc,amf}` 와 `sipHa1:null`.
  소프트-K 프로비저닝 — 단말이 USIM 역할이므로 K 원문이 토큰 인증 + TLS 채널로 내려간다(이 채널의 신뢰가 전제).
- **Android UE**(`android/core`): `account.aka` 를 pjsip AKA cred 로 연결(`AuthCredInfo`
  `PJSIP_CRED_DATA_EXT_AKA` + akaK/akaOp/akaAmf — pjsua2 가 AKA 콜백을 상시 결선). **자격 표현 = hex
  문자열**(K/OPc 32자·AMF 4자): pjsua2/Java 경로는 바이너리를 실을 수 없어, pjsip 패치([2-15])가
  길이 검사(hex 폭 허용)와 소비 시점 디코드(`aka_cred_val` — slen 이 2×바이너리 길이·전부 hex 면
  디코드, 바이너리 입력 하위호환)로 흡수한다 — AuC 와 동일한 16B K/OPc 로 Milenage 를 계산해
  RES 가 일치한다(미적용 pjsip 은 계정 add 에서 `op.slen > PJSIP_AKA_OPLEN` assert 로 SIGABRT).
  pjproject 패치
  (`config_site.h` `PJSIP_HAS_DIGEST_AKA_AUTH` + `PJSIP_AKA_OP_IS_OPC`): AuC 는 OP 가 아니라 **OPc** 를
  배포하므로 milenage 에 `f1_opc`/`f2345_opc`(ComputeOPc 생략) 를 신설해 `sip_auth_aka.c` 가 그것을 쓴다.
  libmilenage 는 third_party 빌드·링크에 편입. **AUTS 재동기는 미구현** — pjsip 은 단말측 SQN 을 추적하지
  않아(MAC 검증만) 재동기 계기가 없고, CSC AuC 가 단일 SQN 발급자라 서버측 SQN 이 뒤처질 일도 없다
  (소프트-USIM 재동기 경로 검증은 psip/cspsim 이 담당).

**검증**: §6 V14~V18, `S3-SCN-AKA`. 단위: `csc/src/tests/test_milenage.py`(TS 35.208 세트 1~3)·`test_auc.py`,
`tests/psip_aka_test.cpp`.

### 8.3 P4 — IMS AKA + IPsec (본문 §6~7) — 구현 반영

3GPP 액세스(UICC 단말)와의 직접 interop 을 위한 **표준 완결 단계**다 — 자가망 단말에는 P3(Annex X) 가 같은 보호
수준을 주므로, 이 단계의 가치는 보안 강화가 아니라 표준 조합 표(§1)의 마지막 칸을 채우는 데 있다. 아래 네 결정으로
범위를 닫는다.

| 축 | 결정 | 근거 |
|---|---|---|
| NAT | **Annex M(UDP 캡슐화 ESP) 미포함.** ESP transport mode 만. NAT 뒤 단말은 IPsec 대상이 아니며 access service 단위로 상호배제한다(아래 "NAT 상호배제 — 두 겹") | transport mode ESP 는 NAT 를 지나지 못하고(ESP 에는 포트가 없어 NAT 가 다중화 못 하고 SA selector 가 어긋난다) 실패가 **무증상 폐기**로 나타난다. 표준도 NAT 통과를 본문 밖(Annex M)에 둔다 |
| 알고리즘 | 무결성 `hmac-sha-1-96`(선호)·`hmac-md5-96`, 암호 `aes-cbc`·`null`. `des-ede3-cbc` 는 제시하지 않는다 | TS 33.203 §6.3 — 무결성 두 알고리즘은 UE/P-CSCF 필수이고 커널(XFRM)이 둘 다 제공하므로 비용이 없다. 3DES 는 폐기 방향 |
| 단말 | psip/cspsim(소프트-USIM) 한정. Android(pjsip) 는 범위 밖 | pjsip 은 IPsec SA 를 만들지 않는다 — 단말 쪽 별도 구현이며 이 단계의 검증 대상은 서버·프로토콜 정합이다 |
| transport | UDP 가 검증 프로파일. TCP 는 같은 보호 포트쌍 위에서 규격상 허용되며 psip 의 발신 소스포트 bind 가 잔여 | TS 33.203 §7.1 — 보호 포트쌍은 UDP·TCP 공용 |

**체계와 메커니즘의 분리 (§8.2 결정 유지).** 인증 체계(`auth_scheme`)는 프로비저닝이 정하고, 채널 보호 메커니즘
(`tls` | `ipsec-3gpp`)은 RFC 3329 협상이 정한다. IPsec 은 AKA 의 CK/IK 없이는 성립하지 않으므로 `ipsec-3gpp` 는
**AKA 가입자에게만 제시**된다 — Digest 가입자가 제안하면 `Security-Server` 에는 `tls` 만 실린다. 따라서 가입자
스키마는 바뀌지 않는다. `CspUser::requiresTls()` 는 `requiresProtectedChannel()` 로 일반화된다: AKA 가입자의 요청은
**TLS 연결 또는 이 등록에 결부된 SA** 위에서만 받는다(§3 게이트). `sip_transport=TLS` 정책 축은 Digest 가입자용으로
그대로다.

**SA 모델 (TS 33.203 §7.1).** 노드 고정 보호 포트쌍 (port_ps, port_pc) 과 단말 포트쌍 (port_uc, port_us) 사이에
단방향 ESP transport-mode SA 4개(2쌍). SPI 는 **수신 측**이 고른다.

| SA | 방향 | 실리는 것 | SPI | 선택 주체 |
|---|---|---|---|---|
| 1 | UE:port_uc → CSP:port_ps | UE 요청 | spi_ps | CSP |
| 2 | CSP:port_ps → UE:port_uc | UE 요청에 대한 응답 | spi_uc | UE |
| 3 | CSP:port_pc → UE:port_us | CSP 요청(INVITE·NOTIFY·OPTIONS·MESSAGE) | spi_us | UE |
| 4 | UE:port_us → CSP:port_pc | CSP 요청에 대한 응답 | spi_pc | CSP |

키 확장(TS 33.203 §6.3): IK_esp = IK(`hmac-md5-96`, 128 bit) / IK ‖ IK[0..31](`hmac-sha-1-96`, 160 bit);
CK_esp = CK(`aes-cbc`, 128 bit) / 없음(`null`). SA 수명 = 부여 만료(`Expires`) + 30 s(§7.4 — 늦은 재등록 응답 창).
보호 포트쌍은 모든 단말이 공유하고 단말은 (IP, 포트, SPI) 로 갈린다. `Security-Client` 의 `spi-c/spi-s/port-c/port-s`
는 단말 값(spi_uc/spi_us/port_uc/port_us), `Security-Server` 의 같은 파라미터는 서버 값(spi_pc/spi_ps/port_pc/port_ps)이다.

**절차**

```
UE (soft-USIM: K/OPc)                  CSP (P/S-CSCF, 한 프로세스)                        CSC (HSS/AuC)
 │ REGISTER (비보호 포트 5060, 무인증)   │                                                    │
 │  Security-Client: ipsec-3gpp; alg=hmac-sha-1-96; ealg=aes-cbc;                         │
 │    spi-c=spi_uc; spi-s=spi_us; port-c=port_uc; port-s=port_us  (, tls)                 │
 │  Require/Proxy-Require: sec-agree     │                                                    │
 │─────────────────────────────────────▶│ NAT 판정(top Via sent-by vs received/rport)         │
 │                                      │ auth_scheme=aka → POST /internal/aka/av ──────────▶│
 │                                      │◀──────────── {rand,autn,xres,ck,ik} ───────────────│
 │                                      │ spi_ps/spi_pc 할당 · (alg,ealg)=단말 최고 선호 ∩ 서버 │
 │                                      │ CK/IK 확장 키로 **임시 SA 4개 + 정책 커널 설치**       │
 │ 401  WWW-Authenticate: Digest algorithm=AKAv1-MD5, nonce=base64(RAND‖AUTN)   (ck/ik 없음)│
 │      Security-Server: ipsec-3gpp; q=0.2; alg=…; ealg=…; spi-c=spi_pc; spi-s=spi_ps;     │
 │                       port-c=port_pc; port-s=port_ps,  tls; q=0.1                        │
 │◀─────────────────────────────────────│                                                    │
 │ Milenage → RES·CK·IK, 자기 SA 4개 설치                                                      │
 │ REGISTER  (UE:port_uc → CSP:port_ps, ESP 위)                                               │
 │  Authorization: Digest … algorithm=AKAv1-MD5, response=…, integrity-protected="yes"       │
 │  Security-Client(동일) / Security-Verify(401 의 Security-Server echo)                       │
 │─────────────────────────────────────▶│ RES 대조 → 수신 listener=port_ps ∧ 소스=(UE ip, port_uc) │
 │                                      │  ∧ Verify 바이트 대조 → 임시 SA **확정**(수명=expires+30) │
 │ 200 OK  (CSP:port_ps → UE:port_uc, ESP 위)                                                 │
 │  Service-Route: <sip:<bind_ip>:port_ps;transport=udp;lr>                                  │
 │◀─────────────────────────────────────│ 바인딩: 식별 (UE ip, port_uc) · 서버 발신 목적지 (UE ip, port_us) │
```

이후 CSP 가 거는 요청은 port_pc 소켓에서 (UE ip, port_us) 로 나가고(SA 3) 응답은 SA 4 로 돌아온다. 단말 요청은
계속 SA 1/2 다. 서버는 401 을 보내기 **전에** SA 를 설치한다 — 단말의 답안 REGISTER 가 401 직후 ESP 로 도착한다.

**서버 규칙** (§8.1 표에 얹는다 — 같은 `RecvRequestRegister` 경로):

| 상황 | 동작 | 근거 |
|---|---|---|
| AKA 가입자의 **평문** 초기 REGISTER 에 `Security-Client` 가 있음 | 게이트 통과 → 챌린지 (그 401 이 `ipsec-3gpp` 또는 `tls` 목록을 싣는다) | RFC 3329 §2.2 / TS 33.203 §7.2 — 협상의 초기 REGISTER 는 비보호 채널로 온다(IPsec 부트스트랩). 제안 없는 평문 REGISTER 와 모든 비-REGISTER 는 종전대로 403 |
| 초기 REGISTER 가 `ipsec-3gpp` 제안, 가입자 `auth_scheme=digest` | `Security-Server` 에 `tls` 만 (SA 미설치) | IPsec 키는 AKA 의 CK/IK — §1 조합 표 밖 |
| `ipsec-3gpp` 제안인데 **NAT 감지**(top Via `sent-by` ≠ `received` 또는 포트 ≠ `rport`) | `tls` 만 + 로그 `ipsec: nat detected user=… sent-by=… received=…` | Annex M 미지원 — 협상 단계에서 갈라야 무증상 폐기를 피한다. 공통 메커니즘이 없으면 단말이 스스로 중단한다(RFC 3329 §2.3) |
| `ipsec-3gpp` 제안인데 이 노드에 보호 포트쌍(IPSEC LocalNode)이 없거나 기동 자기점검에서 SA 설치 불가 | `tls` 만 + ERROR 로그(억제 1회) | 접속점 부재 = 제시 불가 |
| 제안 파라미터 불량(spi/port 결측·중복, 지원 alg/ealg 없음) | 494 + `Security-Server` | RFC 3329 §2.2 — 협상 재시작 |
| AV 발급 뒤 SA 설치 실패(커널 오류) | 504 + 임시 상태 회수 | HSS 미도달과 같은 "재시도" 부류(§8.2) |
| 보호 포트로 온 답안 REGISTER: 인증 통과 ∧ 수신 listener=port_ps ∧ 소스=(ip, port_uc) 가 임시 SA 셋과 일치 ∧ Verify 대조 통과 | 200, SA 확정, 바인딩 `m_eProtection=ipsec` | TS 24.229 §5.2.2.1 — `integrity-protected="yes"` 판정 |
| 답안 REGISTER 가 비보호 포트로 옴 (협상은 ipsec) | 494 + 임시 SA 회수 | 협상 결과 불이행 — §8.1 "negotiated tls but not on TLS" 규칙의 일반화 |
| `Security-Verify` 불일치 | 494 + 새 `Security-Server` + 임시 SA 회수 | 강등/변조 |
| 임시 SA 에 `Setup.Ipsec.TempSaTimeoutSec`(기본 32 = 64×T1) 안에 답안 없음 | 임시 SA 회수 + 발급 기록 삭제 | TS 24.229 §5.2.2.1 P-CSCF 임시 SA 처리 |
| 등록 갱신 — 챌린지 없이 답안(nonce 재사용, nc 증가) 통과 | SA 수명 연장(`XFRM_MSG_UPDSA`) | §7.4 — 재인증 없는 갱신은 SA 유지 |
| 재인증 갱신 — 새 401(새 AV) | 새 SA 셋(새 spi 4개)을 임시 설치, 답안은 **새 SA 위로**. 확정 시 구 SA 는 새 SA 위 첫 요청 수신 후(또는 잔여 수명) 회수 | TS 33.203 §7.4.1a / TS 24.229 §5.2.2.1 |
| 해제(`Expires: 0`) | 200 OK 송신 뒤 SA 회수(응답 전송 유예 2 s) | §7.4 |
| 바인딩 만료(`CUserMap::DeleteTimeout`) | SA 회수(커널 hard lifetime 이 이중 안전장치) | |
| 비-REGISTER | AKA 가입자 요청은 TLS 결부 flow **또는** 결부 SA(listener=port_ps ∧ (ip, port_uc)) 위에서만 통과, 밖이면 403 — §3 게이트·§8.2 규칙의 일반화 | Annex O / 본문 §7 |

채널-신원 결속: SA selector 가 (UE ip, 포트) 이므로 커널이 "이 SPI 로 복호된 패킷의 소스가 selector 와 같다" 를
보장한다 — §3.2 의 (ip, port) 대리키 판정이 IPsec 에서는 **암호학적 결속**이 된다. §8.4 의 잔여는 TLS 에만 남는다.

**NAT 상호배제 — 두 겹**

| 겹 | 위치 | 규칙 |
|---|---|---|
| 정적(설정) | access service `sec_mechanisms` 로드(`CCspServiceMap::Sync`) | `ipsec-3gpp ∈ sec_mechanisms` ⇒ `media_nat_mode = off`. 위반이면 **`ipsec-3gpp` 만 무시 + ERROR** — 서비스(등록)는 살아 있고 `tls` 만 제시한다. 설정 오류가 가입자 등록을 끊지 않도록 한다 |
| 동적(등록) | `RecvRequestRegister` 의 `Security-Server` 발급 직전 | 초기 REGISTER 의 NAT 판정(위 표) — 서비스가 허용해도 **그 단말**이 NAT 뒤면 `ipsec-3gpp` 를 빼고 제시 |

정적 겹만으로 부족한 이유: 정책은 서비스 단위, NAT 는 단말 단위다 — "NAT 없음" 서비스에도 공유기 뒤 단말이
들어올 수 있다. `latch_ip_guard` 는 미디어 latch 규칙이라 무관하다. 시그널링 NAT 처리(Via received/rport 각인,
[ue_nat_traversal.md §2](ue_nat_traversal.md))는 IPsec 등록에도 그대로 돌지만 NAT 가 없으므로 항등이다.

**설정**

| 위치 | 키 | 의미 |
|---|---|---|
| LocalNode(`local_nodes.jsonl`) | `protocol=IPSEC`, `edge=access`, `bind_ip`, `bind_port`(=port_ps), `client_port`(=port_pc) | 보호 포트쌍 접속점 하나(노드당 하나). CSP 는 `bind_port` 에 **UDP 소켓 + TCP 리스너**를, `client_port` 에 발신용 UDP 소켓(+TCP 소스 bind)을 연다. 개설 실패는 [sip_tls_signaling.md §6.2](sip_tls_signaling.md) 의 접속점 격리·A-PRC-012 와 같은 계약 |
| AccessService(`access_services.jsonl`) | `sec_mechanisms: ["tls"]`(기본) \| `["tls","ipsec-3gpp"]` | 이 서비스 가입자에게 제시할 메커니즘. `ipsec-3gpp` 는 노드에 IPSEC LocalNode 가 있고(`inbound_policy=restricted` 면 `allowed_local_node_refs` 에 포함) `media_nat_mode=off` 일 때만 유효 |
| `Setup.Ipsec.SpiMin` / `SpiMax` | 기본 `0x10000000` ~ `0x1FFFFFFF` | 이 노드가 고르는 spi_ps/spi_pc 범위(수신 측 고유, 사용 중 값 회피) |
| `Setup.Ipsec.ReqIdBase` | 기본 `0x43490000` | 이 프로세스가 만든 XFRM state/policy 의 소유 표식 — reqid ∈ [Base, Base+0xFFFF]. 기동·종료 시 그 범위를 일괄 회수. (XFRM `mark` 는 **패킷 매칭 조건**이라 표식으로 쓸 수 없다 — mark 0 인 SIP 패킷이 SA 에 걸리지 않는다) |
| `Setup.Ipsec.TempSaTimeoutSec` | 기본 32 | 임시 SA 유예 |
| `Setup.Ipsec.EalgPreference` | `aes-cbc`(기본) \| `null` | 단말 제안 q 가 동률일 때 서버 선호 |

`Setup.Ipsec.*`·`sec_mechanisms` 는 SIGUSR1 재로드(다음 REGISTER 부터). IPSEC LocalNode 의 포트 변경은 살아있는 SA
를 무효화하므로 **재기동** 항목이다.

**커널 프로그래밍 — `ext/psip/SipStack/XfrmSa.{h,cpp}`** (CSP 와 psip UA 가 같이 쓴다 — 서버·단말의 SA 코드가 하나)

- 순수 netlink(`NETLINK_XFRM`, libnl 미사용): `XFRM_MSG_NEWSA`(proto ESP, mode transport, spi, src/dst,
  `XFRMA_ALG_AUTH_TRUNC` `hmac(sha1)`/`hmac(md5)` trunc 96, `XFRMA_ALG_CRYPT` `cbc(aes)`/`ecb(cipher_null)`,
  replay window 32, hard lifetime = expires+30, reqid = SA 셋 id) + `XFRM_MSG_NEWPOLICY`(dir in/out, selector = src/dst
  ip+포트+proto, template ESP transport reqid). selector 의 포트는 proto 가 있어야 유효하므로 **SA 하나당 정책
  둘(udp, tcp)** — 등록당 state 4 + policy 8. 연장은 `XFRM_MSG_UPDSA`/`UPDPOLICY`, 회수는 `DELSA`/`DELPOLICY`.
  기동·종료 시 전역 flush 가 아니라 state/policy 를 덤프해 **reqid 범위로 걸러 일괄 삭제**(같은 호스트의 다른 IPsec
  사용자와 공존). 서버·단말이 같은 코드를 쓴다 — 양쪽 모두 "내 서버포트/내 클라이언트포트 ↔ 상대" 로 대칭이다.
- 특권: `CAP_NET_ADMIN` **파일 capability**. 모듈은 agent(user systemd unit, 비특권)가 기동하므로 unit 의
  `AmbientCapabilities` 로는 줄 수 없다 — agent 가 csp/cspsim 설치·기동(current flip)마다 `sudo -n cims-priv
  setcap-net-admin <bin>`(sudoers 화이트리스트의 기존 특권 헬퍼, 대상은 csp/cspsim 실행 파일로 한정)을 호출한다.
  파일이 교체되면 capability 가 사라지므로 매번 다시 건다. 소스 트리(`cims.sh`)에서는 `sudo setcap cap_net_admin+ep
  build/bin/csp`(cspsim 도)가 수동 절차다. 특권이 없거나 IPSEC LocalNode 가 없으면 기동 시 자기점검(reqid 범위 회수 +
  더미 셋 설치·삭제)이 실패 → `ipsec-3gpp is not offered` 로그, `tls` 만 제시 — 기동은 계속된다.
- HA 절체: SA 는 복제하지 않는다. 절체 뒤 단말의 보호 트래픽은 새 활성 노드의 커널이 버리고, 단말은 등록 갱신 실패
  → 초기 등록(비보호 포트)으로 복귀한다(TS 24.229 §5.1.1.5 의 단말 복구) — [ha_service_model.md](ha_service_model.md)
  의 재등록 모델과 같다.

**CSC / 프로비저닝**: 스키마 변경 없음. `/provisioning/me` 의 `services[].sip.security` 에 서비스의 `sec_mechanisms`
(`["tls"]` | `["tls","ipsec-3gpp"]`)를 싣고, `ipsec-3gpp` 가 있으면 `sip.ipsec={port_ps, port_pc}`(IPSEC LocalNode 에서).
AKA 가입자는 `enforced=true` 그대로(보호 채널 강제). 콘솔 access service 편집 화면에 `sec_mechanisms` 다중선택 +
`media_nat_mode≠off` 와의 상호배제 검증(저장 거부).

**단말 (psip / cspsim)**

- psip `SipIpsec.{h,cpp}`(`CSipIpsecClient`, `CSipServerInfo::m_clsIpsec`): REGISTER 를 만들 때마다 제안할 포트쌍
  (port_uc/port_us — 스택 로컬 포트+1 부터 2씩, 다중 UDP 리스너(R5.b)로 런타임 개방)과 SPI 둘(난수)을 준비해
  `Security-Client` 에 `ipsec-3gpp` 만 싣는다(sec-agree 자동 활성). 401 의 `Security-Server` 에서 서버 spi/port 를
  파싱하고 `AddAuth` 의 AKA 계산이 남긴 CK/IK(`m_strAkaCk/m_strAkaIk`)로 `XfrmSa` 에 SA 4개를 설치한 뒤, 답안
  REGISTER 에 Via = port_uc 를 명시해 그 리스너 소켓에서 **서버 port_ps 로**(Route 목적지 = `ServerPort()`) 보낸다
  (SA 1). 200 OK 에 스택 식별 포트를 port_uc 로 바꿔 이후 모든 요청(INVITE·BYE·갱신·해제)이 SA 위로 port_ps 를 향해
  나가게 하고, 구 셋이 있으면 회수한다(재인증 = **새 포트쌍·새 SPI**, TS 33.203 §7.4.1a — 같은 포트를 재제안하는
  단말은 서버 커널 정책 충돌로 504 를 받는다). 해제 200 OK·등록 실패 시 전부 회수하고 식별 포트를 복원한다. Verify
  변조는 `m_strSecurityVerifyOverride` 그대로.
- **TCP 위 보호 포트쌍**(§7.1): 커널 SA selector 가 (ip, 포트) 로 잡히므로 TCP 연결의 소스 포트가 맞아야 ESP 로
  나간다. psip 스택에 발신 소스 포트 집합(`CSipStack::AddTcpSourcePort` — 요청의 Via 포트가 집합에 있으면 새 연결을
  그 포트에서 `SO_REUSEADDR` 로 bind 후 connect, `TcpConnectFrom(srcIp, srcPort, …)`)을 두고, 단말은 port_uc 를,
  서버는 IPsec 접속점의 port_pc 를(`CspListenerManager` 가 client 역할 리스너 개설 시) 등록한다. 단말은 port_us 에
  TCP 리스너를 더 열어 서버 발신 연결(port_pc → port_us, SA 3)을 받고, 재인증 시 port_ps 로의 기존 연결(구 port_uc)을
  연결 맵에서 떼어 답안이 새 port_uc 에서 새 연결을 열게 한다. TCP LISTEN 소켓이 있는 포트는 Linux 가 소스로 bind
  하지 못하게 하므로 port_ps/port_us 는 수신 전용, port_uc/port_pc 는 UDP 리스너 + TCP 발신만이다
  (`tests/psip_tcp_srcport_test.cpp`).
- cspsim `-ipsec`(+`-ipsec_alg hmac-sha-1-96|hmac-md5-96`, `-ipsec_ealg aes-cbc|null`), `-aka_k/-aka_opc` 필수,
  `-transport udp|tcp`. 세션당 스택이 하나라 단말마다 포트쌍이 분리된다. 등록 뒤 요청 목적지는 `EventRegister(200)`
  에서 SA 셋의 port_ps 로 바뀐다(`SimSession::RoutePort()`). `-scenario register -hold <secs>` 는 등록을 그 시간
  유지한 뒤 해제한다 — 유지 창에 외부 프로브(V25)를 건다.

**CSC / 프로비저닝 값의 출처**: `csc.json Provisioning.Services.<kind>.sec_mechanisms`(기본 `["tls"]`) 와
`ipsec_port_ps`/`ipsec_port_pc` — CSP 의 access service·IPSEC LocalNode 와 일치시키는 것은 운영 계약이다(csc 는 CSP
를 조회하지 않는다, `tls_port` 와 같은 규칙). 포트쌍이 없으면 `ipsec-3gpp` 를 목록에서 뺀다.

**검증**: §6 V19·V20 = `S3-SCN-IPSEC`(order 55, 커널 특권 불필요), V21~V23·V25·V26 = `S3-SCN-IPSEC-LIVE`(order 56,
cspsim `-ipsec` — IPSEC LocalNode·CSP `ipsec: available`·cspsim `cap_net_admin`·AKA 마이그레이션이 없으면 SKIP.
V25 는 `-hold 6` 유지 창에서 `run_cspsim(on_line=…)` 이 `registration held` 마커에 반응해 비보호 포트로 프로브한다).
단위: `tests/psip_xfrm_test.cpp`(키 확장 벡터 + netlink 메시지 인코딩, 특권이 있으면 실설치 왕복까지),
`tests/psip_tcp_srcport_test.cpp`(TCP 소스 포트 bind — 지정 포트 connect·같은 포트 연속 connect·UDP 리스너 공존).

**구현 위치**

| # | 항목 | 위치 |
|---|---|---|
| P4-1 | XFRM 모듈(netlink) — 셋 설치/연장/회수, reqid 범위 일괄 회수, 자기점검, 시험 훅 | `ext/psip/SipStack/XfrmSa.{h,cpp}`, `tests/psip_xfrm_test.cpp` |
| P4-2 | IPSEC LocalNode → psip 리스너 셋(port_ps UDP/TCP + port_pc UDP, 역할 태그 int id `CspIpsecListenerIntId`), 역할별 포트·역조회, inbound_policy 대조 정규화 | `csp/CspLocalNodeMap.{h,cpp}`, `csp/CspListenerManager.cpp`, `csp/CspAddressing.cpp`, `csp/CspServiceMap.cpp` |
| P4-3 | Sec-Agree: `SelectIpsecOffer`(제안 파싱·(alg,ealg) 선택)·`BuildIpsecServerList`·`Issue(user, list)`; `EvaluateIpsecOffer`(AKA·서비스·가용·NAT 판정) → `SendAkaChallenge` 가 임시 셋 설치 후 401; 답안의 보호 포트·SA 대조·Verify(확정 셋 원문 폴백) | `csp/SecAgree.{h,cpp}`, `csp/CscfModule.{h,cpp}` |
| P4-4 | SA 셋 생명주기 `CIpsecSaSetMap`(임시→확정→연장→retiring→회수, sweep) + 바인딩 결부(`CUserInfo::m_iSaReqId/m_iSendPort/m_iSendListenerId`, 바인딩 소멸 훅) | `csp/IpsecSaSet.{h,cpp}`, `csp/UserMap.{h,cpp}`, `csp/CspServer.cpp`(Init/Sweep/Shutdown) |
| P4-5 | 게이트: 보호 포트 요청은 결부 SA 일치, 평문은 정책(TLS ∨ AKA)+협상 축 — AKA 의 `Security-Client` 초기 REGISTER 만 통과; 서버 발신 (ip, port_us) via port_pc(`GetCallRoute` outbound local, NOTIFY/OPTIONS Via) | `csp/CscfModule.cpp` `CheckChannelPolicy`, `csp/UserMap.cpp`, `csp/CspServer.cpp`, `csp/ModuleDispatcher.cpp` |
| P4-6 | 설정: LocalNode `protocol=IPSEC`+`client_port`, access service `sec_mechanisms`(NAT 상호배제), `Setup.Ipsec.*` | `csp/config/config_template.json`, `csp/SipServerSetup.{h,cpp}`, `csp/CspServiceMap.{h,cpp}` |
| P4-7 | 특권: `cims-priv setcap-net-admin <bin>` + agent 가 csp/cspsim 설치·기동 flip 마다 호출 | `agent/bin/cims-priv`, `agent/cims_agent.py` `_grant_ipsec_capability` |
| P4-8 | CSC `/provisioning/me` `sip.security`/`sip.ipsec` | `csc/src/services/mcptt.py` |
| P4-9 | 단말: `CSipIpsecClient`(포트쌍·SPI 제안, SA 설치, port_ps 라우팅, 식별 포트 전환, 회수, TCP 리스너/소스포트), 등록 흐름 훅, cspsim 옵션(`-ipsec`, `-hold`, `RoutePort()`) | `ext/psip/SipUserAgent/SipIpsec.{h,cpp}`, `SipServerInfo.{h,cpp}`, `SipUserAgentRegister.hpp`, `cspsim/SimSession.{h,cpp}`, `cspsim/CspsimMain.cpp` |
| P4-11 | psip 발신 TCP 소스 포트 bind — 스택 집합(`AddTcpSourcePort/SelectTcpSourcePort`), 플랫폼 `TcpConnectFrom(srcIp, srcPort, …)`, 클라 스레드 적용, 서버 port_pc 등록 | `ext/psip/SipStack/SipStack.{h,cpp}`, `SipTcpClientThread.cpp`, `ext/psip/SipPlatform/SipTcp.{h,cpp}`, `csp/CspListenerManager.cpp`, `tests/psip_tcp_srcport_test.cpp` |
| P4-10 | verify `S3-SCN-IPSEC`/`S3-SCN-IPSEC-LIVE` + `sip_probe.probe_register_offer` | `verify/lib/items/stage3/scn_ipsec.py`, `verify/lib/common/sip_probe.py` |

실설치 검증(S3-SCN-IPSEC-LIVE V21~V26)은 `CAP_NET_ADMIN` + IPSEC LocalNode 가 있는 환경에서 라이브로 통과한다
(단말은 별도 netns 로 격리해 UE·서버 XFRM 충돌을 피한다 — XFRM state/policy 는 netns 단위): V21 SA 위 등록,
V22 `Security-Verify` 변조 → 494 + 임시 SA 회수, V23 해제 → SA 회수, V25 등록 유지 중 비보호 포트 403,
V26 TCP 위 등록(port_uc→port_ps 소스포트 bind). 자동화 항목은 dev 계정에 특권/userns 가 없으면 SKIP.

잔여: TCP 재인증의 구 연결은 구 SA 회수 뒤 수신 타임아웃으로 닫히는 것까지 실측, Android(범위 밖).

### 8.4 psip 확장 (단계 무관)

TLS 연결 핸들/세션 ID 의 응용 노출 — §3.2 게이트의 (ip,port) 대리키 판정을 암호학적
결속으로 승격해 NAT 포트 재사용 오일치 잔여 위험을 제거한다. P2~P4 어느 단계와도 독립 — IPsec 등록([§8.3](#83-p4--ims-aka--ipsec-본문-67--구현-반영))은
SA selector 가 이 결속을 커널에서 제공하므로 대상이 아니다.
