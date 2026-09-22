# 등록 바인딩 집합 — flow 단위 도달 관리

CSP 가 "이 가입자에게 어떻게 도달하는가"를 관리하는 구조의 정본 문서다. 현재는 AoR(가입자)
당 **주소 칸 하나**를 두고 새 등록이 그것을 덮어쓴다. 이 문서는 그것을 **바인딩 집합**으로
바꾸는 설계를 정의한다.

관련 문서: [ue_nat_traversal.md](ue_nat_traversal.md) · [sip_tls_signaling.md](sip_tls_signaling.md) ·
[modules/csp.md](../modules/csp.md)

> **상태**: 구현·실측 완료(A안 — flow 추적) + UDP 생존 판정(keepalive 상향·침묵 판정·UDP 등록
> 수명 상한·STUN 응답). 남은 것은 reg-event 를 contact 목록으로 확장하는 정합 작업
> ([§5](#5-reg-eventrfc-3680-정합))과 단말의 STUN keepalive 채택
> ([§4.1a](#41a-nat-가-공인-포트를-바꾸면--복구는-재등록만-한다))이다. 멀티 디바이스(한 계정 여러 단말 동시 사용)는
> 이 문서의 범위가 아니다([§8](#8-멀티-디바이스는-범위-밖-b안)).

## 1. 왜 바인딩 집합인가

가입자당 주소 칸을 하나만 두면 "지금 들어온 등록이 살아있는 도달 경로인가"를 서버가 **추측**해야
한다. 저장값 `(IP, 포트)` 는 스트림 transport 에서 **연결의 이름표**일 뿐이라, 연결이 죽으면 같은
값의 의미가 "이 연결에 써라"에서 "이 주소로 새로 연결해라"로 조용히 뒤집히기 때문이다
([sip_tls_signaling.md §2.2](sip_tls_signaling.md#22-tcptls--저장값은-살아있는-연결을-찾는-열쇠)).

추측의 대가는 실측으로 드러났다 — 대형 요청이 TCP 로 승격되면 그 일회성 경로가 도달 주소를
덮어쓰고, 연결이 유휴로 닫힌 뒤에는 서버 발신이 전량 유실됐다. transport 종류로 걸러내는 규칙을
두면 승격은 막히지만 정당한 transport 전환(UDP→TLS)까지 막혀, 다시 예외 규칙이 필요해진다.

바인딩 집합은 그 전제를 없앤다: **경로마다 바인딩 하나**를 두고, 보낼 때 **살아있는 것을 고른다.**
살아있는지는 스택에 직접 묻는다.

## 2. 목표 구조

```
CUserMap:  AoR ──▶ [ Binding{ ip, port, transport, contact, 만료, last-seen },
                     Binding{ … }, … ]
```

- **키 = (ip, port, transport)** — 이 3원소가 그대로 flow 키다. 스트림에서는 psip
  소켓맵(`CTcpSocketMap`)의 키와 **같은 값**이고, UDP 에서는 NAT 매핑을 가리킨다.
- 새 등록은 **덮어쓰기가 아니라 바인딩 추가/갱신**이다. 승격 TCP 는 별개 바인딩으로 들어오고,
  그 연결이 죽으면 생존 판정에서 탈락한다 — **규칙 ①②의 추측이 필요 없어진다.**
- 서버 발신은 **살아있는 바인딩**을 고른다.

### 2.1 생존 판정

| transport | 판정 | 근거 |
|---|---|---|
| TCP / TLS | psip 소켓맵에 그 (ip,port) 연결이 있는가 (`CSipStack::IsFlowAlive`) | 연결이 닫히면 맵에서 제거된다(`TcpSessionList`) |
| UDP | **응용이 keepalive 수신 시각으로 판정한다** (`Setup.Sip.UdpFlowSilenceSec`, 기본 90초) | 연결 개념이 없어 스택은 답할 수 없다(`IsFlowAlive` 는 항상 true). 단말이 보내는 NAT keepalive 가 그 경로의 유일한 생존 신호다([§4.1](#41-udp--keepalive-로-판정한다)) |

psip 은 소켓맵 조회 API(`CTcpSocketMap::Select`)를 이미 갖고 있고, `IsFlowAlive` 는 그 얇은
래퍼다 — 연결 핸들을 응용까지 전달할 필요가 없다. UDP 는 스택이 답할 수 없으므로 판정을 응용에
위임하고, 응용은 `CUserInfo::m_iLastSeenTime`(저장 경로로 단말발 패킷이 마지막에 도착한 시각)으로
답한다. keepalive 는 SIP 메시지가 아니라 종전에는 소켓 계층에서 버려졌으나, 이제 psip 이
`ISipStackCallBack::EventKeepAlive` 로 응용까지 올린다(`ext/psip/SipStack/SipUdpThread.cpp`).

### 2.2 선택 정책 (서버 발신)

1. 살아있는 바인딩 중 **가장 최근에 갱신된 것** 하나
2. 살아있는 것이 없으면 마지막 바인딩(현 동작과 동일 — 도달 실패하지만 무해)

이 선택은 서버가 **처음 거는** 요청(fan-out INVITE·NOTIFY)뿐 아니라 **확립된 다이얼로그 안에서 서버가 보내는 요청**(BYE·re-INVITE·NOTIFY·REFER·INFO)에도 생성 직전에 적용된다 — psip `RefreshLegDest` → `EventGetLegDest` → `Select`([leg_liveness.md §6.3](leg_liveness.md#63-갱신-re-invite-규율)). 다이얼로그가 기억한 수신 당시 소스는 응용이 바인딩을 모를 때(미등록 peer)의 폴백이다.

멀티 디바이스를 지원하지 않으므로 **한 사람에게 병렬 포크는 하지 않는다**. 사람당 leg 하나가 유지된다.
이 원칙의 범위는 **한 사람(AoR)의 바인딩 집합**이다 — 전화 그룹 대표번호가 여러 그룹원에게 동시에 포크하는
것([dispatch_center.md](dispatch_center.md) §4)은 사람이 다르므로 이와 충돌하지 않는다(그룹원마다 위 선택
정책으로 바인딩 하나를 고른다).

## 3. 왜 소비자 26곳을 건드리지 않는가

`CUserMap` 의 공개 API 는 이미 "이 사용자에게 보낼 정보 하나를 달라"는 형태다.

```cpp
bool Select( const char *pszUserId, CUserInfo &clsInfo );   // 26곳이 이것을 쓴다
```

**시그니처를 유지하고 내부에서 최적 바인딩을 골라 반환**하면, fan-out INVITE·NOTIFY 2종·
서버 발신 in-dialog 요청(`EventGetLegDest` — BYE·re-INVITE·NOTIFY·REFER·INFO, 세션 갱신 포함)·MSRP·라우팅 등 소비자 전부가 무변경이다. 구조 교체가
`UserMap` 안에 갇힌다.

| API | 변경 |
|---|---|
| `Select(id, info)` | 내부에서 살아있는 최적 바인딩 선택 (시그니처 불변) |
| `Select(id)` | 바인딩이 하나라도 있는가 (의미 불변) |
| `Insert(msg, user)` | **바인딩 추가/갱신**. 생성은 REGISTER 만, 같은 transport 는 교체 |
| `SetIpPort(...)` | 그 transport 의 기존 바인딩 주소만 이동(생성하지 않음) |
| `TouchFlow(...)` | 해당 바인딩의 last-seen 갱신 |
| `DeleteTimeout(...)` | **바인딩 단위 만료** — 마지막 바인딩이 사라질 때 등록 해제로 통지 |
| `SendOptions()` | 살아있는 바인딩 대상 (UDP 바인딩 유지 목적) |
| `GetString()` | 바인딩 목록 표시 (운영 조회) |

## 4. 바인딩 수명과 정리 정책

바인딩을 지우는 계기는 **셋**이다. 각각 근거가 다르므로 하나로 합치지 않는다.

| # | 계기 | 대상 | 근거 |
|---|---|---|---|
| 1 | **같은 transport 재등록** | 그 transport 의 기존 바인딩 | 한 단말은 한 transport 에 살아있는 경로가 하나뿐이다. 새 등록이 온 시점에 옛 경로는 무효다 |
| 2 | **flow 실패** (스트림만) | 연결이 닫힌 바인딩 | **RFC 5626 — flow 실패는 바인딩 무효.** 만료를 기다리면 Expires+grace(현 배치 최대 ~77분) 동안 유령이 남는다 |
| 3 | **등록 만료** | `등록시각 + Expires + grace` 초과 | RFC 3261 §10 바인딩 수명 |

정리 시점은 **등록 처리(1)와 만료 sweep(2·3)** 이다. 조회 경로(`Select`)에서는 죽은 바인딩을
**건너뛰기만 하고 지우지 않는다** — 26곳이 호출하는 읽기 경로가 상태를 바꾸면 안 된다.

가입자당 상한(`MAX_BINDING_PER_USER = 8`)은 위 세 계기가 모두 늦을 때를 위한 **안전망**이다.
정상 상태에서는 transport 당 1개(전환 과도기만 2개)로 수렴한다.

### 4.1 UDP — keepalive 로 판정한다

UDP 는 연결이 없어 `IsFlowAlive` 가 판정할 수 없다(항상 true). 대신 **단말이 보내는 NAT
keepalive** 가 그 경로의 생존 신호다. psip 이 keepalive 를 응용까지 올리고(`EventKeepAlive`),
`CUserMap::TouchKeepAlive` 가 주소가 일치하는 바인딩의 `m_iLastSeenTime` 을 갱신한다.

| 규칙 | 값 |
|---|---|
| 침묵 임계 | `Setup.Sip.UdpFlowSilenceSec` (기본 90초 = keepalive 6회분) |
| 적용 대상 | keepalive 를 **한 번이라도 보낸** 바인딩만 (`m_bKeepAliveSeen`) |
| 침묵한 바인딩의 처리 | `Select` 가 **고르지 않는다**. 바인딩은 지우지 않는다 |

두 가지가 규율이다.

- **keepalive 를 보내지 않는 단말에는 적용하지 않는다.** 조용한 것이 정상인 구 SDK·시뮬레이터를
  끊어버리면 안 된다. 그래서 "보내던 단말이 멈췄다" 일 때만 도달 불가로 본다.
- **침묵으로 바인딩을 지우지 않는다.** 지우면 마지막 바인딩일 때 등록 해제로 이어지고, 등록에
  종속된 PTT affiliation 까지 회수되어 단말이 잘 때마다 그룹 소속이 출렁인다. 등록은 만료가
  회수하고([§4](#4-바인딩-수명과-정리-정책) 계기 3), 침묵은 **도달 경로 선택에서만** 뺀다.

### 4.1a NAT 가 공인 포트를 바꾸면 — 복구는 재등록만 한다

NAT 는 매핑이 유휴로 만료되면 그것을 지우고, 다음에 나가는 패킷에 **새 공인 포트**를 준다.
단말 소켓은 그대로이므로 단말도 자기 주소가 바뀐 것을 모른다(등록 때 학습한 값을 계속 믿는다).
서버의 바인딩은 낡은 포트를 가리키고, 그 사이 서버 발신은 전량 유실된다.

**keepalive 로는 이것을 고칠 수 없다.** keepalive 본문에는 신원이 없어 새 주소에서 온 것을 특정
가입자에게 귀속시킬 수 없고, 귀속시키면 같은 NAT 뒤의 다른 단말이 남의 착신을 가로챌 수 있다.
그래서 `TouchKeepAlive` 는 **이미 아는 바인딩과 주소가 정확히 일치할 때만** 기록한다.

복구는 Digest 인증이 붙는 **재등록**만이 할 수 있다. 그 창을 두 가지로 줄인다.

| 수단 | 효과 |
|---|---|
| `Setup.Sip.UdpRegisterExpires` (기본 300초) — UDP 등록에만 수명 상한. 스트림·IPsec 은 제외 | 창의 상한을 재등록 주기로 묶는다. 단말 변경 없음 |
| 단말의 STUN keepalive (RFC 5626 §4.4.2) | 단말이 응답의 XOR-MAPPED-ADDRESS 로 주소 변화를 즉시 알고 재등록 — 창이 keepalive 주기로 줄어든다 |

서버는 SIP 리스너에서 STUN Binding Request 에 이미 응답한다(`SipUdpThread.cpp`, psip `StunParser`).
단말이 keepalive 를 CRLF 대신 STUN 으로 보내면 그대로 동작한다. RFC 5626 §4.4.1 의 CRLF
ping(CRLF 2개)에는 규격대로 pong(CRLF 1개)으로 답한다.

가장 앞선 방어는 **매핑을 잃지 않는 것**이다. 매핑이 죽는 직접 원인은 단말이 절전에 들어 keepalive
타이머가 멈추는 것이므로, CIMS 단말 앱은 배터리 최적화 예외 + 등록 유지 서비스의 부분 wakelock 상시
보유로 절전 중에도 keepalive 를 이어 보낸다([android_ue_client.md §8](android_ue_client.md#8-안드로이드-런타임-설계)).
예외만으로는 부족하다 — Doze 는 망을 열어 주되 CPU 는 재우므로 wakelock 이 없으면 타이머가 서고, 그 상태에서
매핑이 매시간 새 포트로 옮겨 가는 것이 실측됐다. 위의 서버 판정·수명 상한·STUN 은 그래도 매핑이 죽는 경우
(공유기 재부팅·테이블 축출·Wi-Fi 순단)를 위한 두 번째 층이다.

### 4.1b 승격 TCP flow 는 일회성이다 — 응답 Contact 와 CANCEL

UDP 등록 단말이 RFC 3261 §18.1.1 로 TCP 승격해 보낸 INVITE 는 **바인딩이 아니다**(§3 `SetIpPort` 는 그 transport 의
기존 바인딩만 옮기므로 생기지도 않는다). pjsip 은 ACK 뒤 33초(`PJSIP_TRANSPORT_IDLE_TIME`)에 그 연결을 닫는다. 그래서
다이얼로그를 그 flow 에 묶으면 안 된다 — 양방향 모두.

| 방향 | 규칙 | 구현 |
|---|---|---|
| 서버 → 단말 in-dialog 요청 | 등록 바인딩으로 보낸다 ([leg_liveness.md](leg_liveness.md) §6.3) | `RefreshLegDest`/`EventGetLegDest` |
| 단말 → 서버 in-dialog 요청 | 서버가 광고하는 **응답 Contact 의 transport = 발신자의 등록 바인딩 transport**. 도착 flow 가 등록 바인딩이 아니고 transport 도 다르면(승격) `Select` 가 고른 살아있는 바인딩의 transport 를 적는다. 그러면 단말의 BYE·PRACK·소형 re-INVITE 가 등록 flow 로 오고 Via 가 바인딩과 일치해 재챌린지·TCP 재연결이 없다 | `CModuleDispatcher::EventIncomingCall` → `CSipUserAgent::SetContactTransport` → `CSipDialog::m_iContactTransport` → 응답(`CreateResponse` 계승)·in-dialog 요청(`CreateMessage`) 의 `CSipMessage::m_iContactTransport` → `CSipStack::Send` Contact 생성(같은 bind ip 의 그 transport listener) · TCP/TLS 송신 시 소켓 주소 덮어쓰기 생략 |
| CANCEL | **챌린지하지 않는다**(RFC 3261 §22.1 MUST NOT — 재제출 불가). 취소 대상 INVITE 와 같은 트랜잭션(최상위 Via sent-by+branch)일 때만 200 + 487, 아니면 481(§9.2) | psip `RecvCancelRequest` — 인증 훅 미호출. CSP `EventIncomingRequestAuth` 도 CANCEL 통과(방어) |

TCP/TLS 등록 단말은 도착 flow = 등록 바인딩이라 이 규칙에 걸리지 않는다. 단말 쪽 스위치(승격 자체를 끄는 `sip.udpNoTcpSwitch`)는
[sip_tls_signaling.md](sip_tls_signaling.md) §3.2a — 규격 이탈이라 기본 off, 서버 규칙은 그것과 무관하게 항상 적용된다.

### 4.2 죽은 바인딩을 통지에 실으면 안 되는 이유

RFC 3680 의 contact state 는 `active`(등록 유효) / `terminated`(등록 종료) 뿐이고 **"flow 가
죽었다"는 상태가 없다.** 등록은 유효하고 연결만 죽은 바인딩은 `active` 로 실으면 도달
가능성에 대해 거짓이고, `terminated` 로 실으면 등록 상태를 왜곡한다 — **어느 쪽도 정확하지 않다.**

그래서 해법은 통지 방식이 아니라 **보관 방식**이다. 죽은 것을 알았을 때 지우면(계기 2) 목록에
죽은 바인딩이 없으므로 이 문제가 생기지 않는다. 지울 때 알리고 싶으면 RFC 3680 의
`event="deactivated"`(바인딩 무효화, 재등록으로 복구 가능)가 이 상황을 위한 값이다.

### 4.3 이 정책이 바뀌는 조건

- **단말의 transport 자유 선택은 이 정책과 정합한다** — 단말이 UDP·TCP·TLS 중 무엇을 고르든
  한 시점에 등록하는 경로는 하나이므로 계기 1의 전제가 유지된다. 선택을 바꾸면 새 바인딩이
  추가되고 옛 경로는 flow 실패(스트림) 또는 만료로 회수된다
  ([sip_tls_signaling.md §7.1](sip_tls_signaling.md#71-선택-모델--단말이-고르고-서버는-가용-목록을-준다)).
- **멀티 디바이스 지원**([§8](#8-멀티-디바이스는-범위-밖-b안)) — 계기 1의 "한 transport 에 한 경로"
  전제가 깨진다. 그때는 instance-id 로 기기를 구분해 **같은 기기의 같은 transport** 만 교체해야
  한다. 단말의 instance-id 유일화가 선행 조건이다.
- **UDP 침묵을 계기 2로 승격** — 지금은 침묵한 UDP 바인딩을 선택에서 빼기만 하고 지우지 않는다
  ([§4.1](#41-udp--keepalive-로-판정한다)). 등록에 종속된 PTT affiliation 을 흔들지 않을 회수
  절차(예: 재등록 유예를 둔 단계적 회수)가 정해지면 계기 2를 UDP 로 확장할 수 있다.

## 5. reg-event(RFC 3680) 정합

reg-event NOTIFY 는 원래 **contact 목록** 모델이다. 지금은 바인딩이 하나뿐이라 단일 contact 로
내보내고 있는데, 바인딩 집합이 되면 각 바인딩이 `<contact>` 하나로 자연히 대응된다 —
구조가 규격에 가까워진다.

## 6. 남은 정합 작업

reg-event NOTIFY 를 contact 목록으로 확장하는 것([§5](#5-reg-eventrfc-3680-정합))이 남아 있다.
바인딩이 여러 개일 때 각각을 `<contact>` 로 싣고, 바인딩이 회수될 때
`event="deactivated"` 로 알리는 형태다.

## 7. 검증

`cspsim -transport {udp,tcp,tls}` 로 **한 계정을 여러 flow 로** 등록시켜 바인딩 집합을 만든다
(실기기 2대로는 만들기 어려운 조합).

| # | 시나리오 | 기대 |
|---|---|---|
| 1 | UDP 등록 후 같은 계정 TLS 등록 | 바인딩 2개, 발신은 최근(TLS) |
| 2 | TLS 연결을 강제 종료 | 그 바인딩이 생존 판정에서 탈락, 발신이 UDP 로 폴백 |
| 3 | 대형 INVITE 로 TCP 승격 유발 | 승격 flow 가 바인딩을 만들지 않고 **등록 flow 를 덮지 않음**. 응답 Contact 는 등록 바인딩 transport(UDP) — 단말 BYE 가 등록 flow 로 옴(§4.1b, `tests/psip_contact_transport_test.cpp`) |
| 4 | 모든 바인딩 만료 | 등록 해제 통지 1회 |
| 5 | 실기기 회귀 (그룹콜·NOTIFY·세션 갱신) | 오늘과 동일 동작 |
| 6 | UDP 등록 후 keepalive 중단 | 임계 경과 뒤 그 바인딩이 선택에서 빠짐 — 1:1 문자는 480, 바인딩은 유지 |
| 7 | keepalive 를 보낸 적 없는 UDP 등록 | 침묵 판정 미적용 (종전대로 도달 대상) |

psip 계층(응용까지 올라오는가·pong·STUN 응답)은 `tests/psip_keepalive_test.cpp` 가
`S1-UNIT-PSIP` 에서 루프백으로 검증한다.

## 8. 멀티 디바이스는 범위 밖 (B안)

한 계정으로 여러 단말을 동시에 쓰려면 서버가 **기기를 구분**해야 한다. RFC 5626 은 그 수단으로
`+sip.instance`(instance-id) + `reg-id` 를 규정하고, **우리 단말은 이미 그 파라미터를 보낸다**
— 그러나 값이 기기 고유가 아니다.

pjsip 은 instance-id 기본값을 **호스트명 해시**로 만든다
(`pjsua_acc.c: init_outbound_setting` — `pj_hash_calc(hostname)` 4바이트를 UUID 꼬리에 넣는다).
Android 기기의 호스트명은 관례적으로 `localhost` 이므로 **모든 단말이 같은 값**을 보낸다.

```
관측: urn:uuid:00000000-0000-0000-0000-0000e922f243   (양 단말 동일)
계산: pj_hash("localhost") = 43f222e9 → 리틀엔디안 기록 → e922f243   ✅ 일치
```

따라서 `(AoR, instance-id, reg-id)` 를 바인딩 키로 쓰면 **서로 다른 기기가 하나로 합쳐진다.**
멀티 디바이스를 지원하려면 앱이 `rfc5626_instance_id` 를 기기 고유값(ANDROID_ID·설치 UUID
등)으로 명시하는 것이 **선행 조건**이고, 그 위에 PTT fan-out 정책(기기당 leg 를 만들 것인가 —
floor 정원·녹취 슬롯·CMP 멤버 포트에 영향)을 정해야 한다. 별도 과제로 둔다.
