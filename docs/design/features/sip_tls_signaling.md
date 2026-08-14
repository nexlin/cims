# SIP TLS 시그널링 — transport 별 도달 모델과 TLS 전환 설계

단말(UE)의 SIP 시그널링을 **TLS 로 운반**하는 배치의 정본 문서다. 미디어(RTP)는 대상이 아니다 —
계속 UDP 를 쓴다. 핵심 주제는 두 가지다: ①연결지향 transport 에서 **서버→UE 도달 주소(latch)의
의미가 UDP 와 다르다**는 점, ②그 차이 때문에 현재 latch 갱신 규율이 TLS 배치에서 성립하지 않는다는
점이다.

관련 문서: [ue_nat_traversal.md](ue_nat_traversal.md) · [leg_liveness.md](leg_liveness.md) ·
[sip_runtime_config.md](sip_runtime_config.md) · [modules/csp.md](../modules/csp.md) ·
[android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md)

> **상태**: 서버·단말 모두 TLS 등록·통화가 성립한다(실기기 실측). 남은 것은 **인증서 운영
> 방침**([§8](#8-인증서-운영-요건-미정)) — 현재는 자가서명 인증서에 단말 검증을 끈 랩 구성이라
> 상용 배치 전에 CA·검증·갱신 체계를 세워야 한다. 전환은 가입자 단위로 진행할 수 있다.

## 1. 범위와 전제

| 항목 | 내용 |
|---|---|
| 대상 | UE ↔ CSP access edge 의 SIP 시그널링 |
| 비대상 | RTP/RTCP(계속 UDP), floor control(RTCP APP), MSRP |
| 표준 배치 | `lb-access-tls :5061` ([02_deployment.md](../02_deployment.md)) |
| 전환 단위 | **가입자 단위** — `{volte,ptt}_subscriptions.sip_transport`(NULL=서비스 설정). 계정 하나씩 옮기며 관찰할 수 있다 |
| 현재 배치 | 전 단말 UDP 등록. `local_nodes.jsonl` = UDP primary + TCP:15060 2행 |

## 2. transport 별 도달 모델 — latch 의 의미가 다르다

NAT 뒤 단말의 Contact URI 는 사설 주소이므로 도달에 쓸 수 없다. CSP 는 등록 요청의 실소스
(top Via `received`/`rport`)를 `CUserInfo{m_strIp, m_iPort, m_eTransport}` 에 저장하고, 서버가 먼저
거는 요청의 목적지를 이 값으로 오버라이드한다. 이것이 latch 다
([ue_nat_traversal.md §2](ue_nat_traversal.md#2-시그널링-평면-csp--psip)).

**저장하는 값은 같지만 그 값의 역할이 transport 에 따라 다르다.**

### 2.1 UDP — 저장값은 목적지 주소

```
UdpSend( socket, buf, len, "121.161.164.99", 45733 );   // 주소가 인자 = 실제 목적지
```

유효 조건은 **NAT 매핑 생존**이다. 매핑이 사라지면 패킷이 NAT 에서 폐기된다.

### 2.2 TCP/TLS — 저장값은 살아있는 연결을 찾는 열쇠

연결지향 transport 의 송신 API 에는 주소 인자가 없다(`ext/psip/SipPlatform/SipTcp.cpp`).

```c
int TcpSend( Socket fd, const char * szBuf, int iBufLen );   // 주소 없음. SSL_write 도 동일
```

목적지는 연결을 맺는 순간 소켓 안에 확정된다. 그래서 저장한 (IP, 포트)는 "어디로 보낼까"가 아니라
**"어느 fd 에 쓸까"** 를 정하는 데만 쓰인다. 그 조회 자료구조가 `CTcpSocketMap` /
`m_clsTlsSocketMap` 이다(`ext/psip/SipStack/TcpSocketMap.cpp`).

| 항목 | 내용 |
|---|---|
| 키 | `"IP:포트"` 문자열 (`GetKey`) |
| 값 | `{ Socket m_hSocket, SSL * m_psttSsl }` |
| 삽입 | accept 직후, `TcpAccept` 가 채운 **peer(단말) 주소**로 (`SipTlsThread.cpp`) |
| 삭제 | 연결 종료 시 (`TcpSessionList.cpp`) |

송신 분기(`ext/psip/SipStack/SipStackComm.hpp`):

```
transport == UDP  →  UdpSend( sock, ..., ip, port )                    주소로 송신
transport == TCP  →  Select(ip, port, sock) 성공 → SipTcpSend(sock,…)  그 소켓에 write
                                          실패 → StartSipTcpClientThread   신규 연결 시도
transport == TLS  →  SendTls(ip, port, …)  성공 → 그 SSL 에 write
                                          실패 → StartSipTlsClientThread   신규 연결 시도
```

**이 `Select` 의 성공/실패가 도달 여부를 결정한다.** 성공하면 단말이 열어둔 연결에 write 하므로
NAT 를 이미 통과한 경로로 무조건 도달한다. 실패하면 서버가 그 주소로 **새 연결을 시도**하는데,
NAT 뒤 단말은 그 포트에 리스닝 소켓이 없어 실패한다(`TcpConnect error`).

> 열쇠의 의미가 조용히 뒤집힌다는 점이 이 모델의 위험이다 — 같은 저장값이 연결 생존 중에는
> "이 연결에 써라", 사망 후에는 "이 주소로 새로 연결해라"로 해석된다.

### 2.3 latch 값과 맵 키가 일치하는 이유

두 값은 우연히 같은 게 아니라 **같은 소켓의 peer 주소**에서 나온다.

```
① accept          TcpAccept → peer 주소 121.161.164.99:45733 → 맵 키
② 같은 연결의 REGISTER
                  psip 이 수신 소스로 top Via 에 각인 (received/rport)
                  `SipStackComm.hpp` RecvSipMessage → AddIpPortToTopVia — UDP/TCP/TLS 공통 경로
③ CSP            GetTopViaIpPort → latch 저장
④ 서버 발신      Select(latch.ip, latch.port) → ①의 소켓 히트 → write
```

R-URI 는 등록 Contact(사설 주소)를 그대로 유지하고(target refresh), 전송 계층 목적지만 latch 로
오버라이드하는 구조다.

### 2.4 요약

| | UDP | TCP/TLS |
|---|---|---|
| 저장값의 의미 | 데이터그램 목적지 | 살아있는 연결을 찾는 열쇠 |
| 송신 API | `sendto(addr)` | `write(fd)` — 주소 인자 없음 |
| 유효 조건 | NAT 매핑 생존 (UE keepalive) | 연결 생존 (UE keepalive) |
| 무효화되면 | 패킷이 NAT 에서 폐기 | **신규 연결 시도로 의미가 뒤집힘** → NAT 뒤 실패 |
| 복구 | 재등록으로 latch 갱신 | 재등록으로 latch 갱신 |

## 3. transport 승격과 TLS 에서의 소멸

### 3.1 승격은 UDP 전용 현상이다

pjsip 은 요청이 임계값을 넘으면 TCP 로 승격한다(RFC 3261 §18.1.1).
`ext/pjproject/pjsip/src/pjsip/sip_util.c`:

```c
if ( … && tdata->dest_info.addr.entry[0].type == PJSIP_TRANSPORT_UDP)   // 진입 조건
{
    len = tdata->buf.cur - tdata->buf.start;
    if (len >= PJSIP_UDP_SIZE_THRESHOLD) {        // 1300 바이트
        tdata->dest_info.addr.entry[i].type = PJSIP_TRANSPORT_TCP;
    }
}
```

진입 조건이 **해결된 transport 가 UDP 일 때**다. 등록 transport 가 TLS 면 이 블록에 들어가지
않는다 — 크기 임계값 검사조차 하지 않는다. 이미 congestion-controlled transport 위에 있으므로
규격이 요구하는 조건이 성립하지 않기 때문이다.

### 3.2 유지되는 flow vs 방치되는 flow

승격 자체보다 중요한 구분이다.

| | 승격 TCP | 등록 flow (UDP 또는 TLS) |
|---|---|---|
| 생기는 이유 | 큰 요청 1건 | 단말 설정 `transport` |
| 유지 주체 | **없음** — 그 메시지 송신 후 방치 | pjsip keepalive |
| 수명 | 유휴 타이머로 사망 | UE keepalive 90초(`PJSIP_TCP/TLS_KEEP_ALIVE_INTERVAL`) < 서버 유휴 종료 600초(`SIP_TCP_RECV_TIMEOUT`) → 유지 |
| latch 가 추종해야 하나 | ❌ | ✅ |

**latch 의 요건은 "상시 살아 있는 경로"** 이므로, 판단 기준은 UDP/TCP 라는 transport 종류가 아니라
**유지되는 flow 인지**다. 현재 코드가 `== E_SIP_UDP` 로 쓰여 있는 것은, 오늘 배치에서 유지되는
flow 가 UDP 하나뿐이라 대리 표현으로 정확히 일치하기 때문이다. TLS 배치에서는 그 대리 표현이
어긋난다.

### 3.3 TLS 전환으로 소멸하는 문제군

| 문제 | TLS 전환 후 |
|---|---|
| 승격 TCP 소스로 latch 오염 | 소멸 — 오염원 자체가 없음 |
| 임시 TCP 사망으로 서버가 그 주소에 도달 불가 | 소멸 — 연결이 하나뿐이고 유지됨 |
| 도달 주소 3원소 혼용(TCP 주소에 UDP 발송) | 소멸 — transport 단일 |
| 대형 요청의 UDP 조각화·재전송 | 소멸 — stream |
| 승격 연결 사망 시 pjsip 이 기존 확립 호를 로컬 종료 | 이 경로로는 소멸 (실제 망 소실 시에는 잔존 — [§5](#5-실패-모드의-변화)) |

**남는 것은 "변화 추종"이다.** 망 전환·IP 변동·앱 재시작·절전으로 연결이 죽으면 단말은 새 연결로
재등록하고, latch 는 그 새 주소를 따라가야 한다.

## 4. latch 갱신 규칙

### 4.1 갱신 경로는 정확히 두 곳

| 경로 | 위치 | 조건 |
|---|---|---|
| 등록 | `csp/UserMap.cpp` `Insert` | 신규는 무조건 삽입, 기존 엔트리는 가드 통과 시 갱신 |
| 수신 요청의 주소 변경 감지 | `csp/ModuleDispatcher.cpp` `EventIncomingRequestAuth` → `UserMap::SetIpPort` | 인증 통과 + 가드 통과 |

소비자는 네 곳이다. 세션 갱신이 포함되면서 latch 는 **모든 활성 통화의 90초 주기 경로**가 되었다.

| 소비자 | 위치 | stale 시 증상 |
|---|---|---|
| fan-out INVITE | `csp/GroupCallService.cpp` `InviteMember` | 착신 불가 |
| conference NOTIFY | `csp/CspServer.cpp` `SendNotifyToSubscriber` | 로스터 stale |
| terminated NOTIFY | `csp/CspServer.cpp` `SendTerminatedNotify` | 이탈 통지 유실 |
| 세션 갱신 re-INVITE / 만료 BYE | `csp/ModuleDispatcher.cpp` `EventGetLegDest` | **통화가 세션 타이머 주기에 절단** (단말이 `cause=408` BYE) |

### 4.2 stream 등록 단말에서 깨지는 지점

| 상황 | 현재 |
|---|---|
| **주소 변경** (NAT rebind·망 전환, transport 동일) | 규칙 ①([§4.3](#43-갱신-규칙--수신-transport-가-저장-transport-와-같을-때))로 추종 |
| **전환** (UDP 엔트리 존재 + TLS REGISTER) | 규칙 ②([§4.4](#44-전환-규칙--새-transport-가-tcp-가-아닐-때))로 등록 flow 교체 |
| **승격 TCP** (대형 요청 이후의 ACK/BYE·재-REGISTER) | 차단 — 등록 flow 를 바꾸지 못한다 |

이 규칙들이 없으면 운영자에게 보이는 증상은 "등록 실패"가 아니라 더 나쁘다 — 단말은 200 OK 를
받아 등록됨으로 표시되고 **발신도 되는데**, 착신·통지가 안 되고 통화가 세션 타이머 주기로 끊긴다.
서버 발신 목적지만 죽은 주소를 가리키기 때문이다.

### 4.3 갱신 규칙 — 수신 transport 가 저장 transport 와 같을 때

**규칙 ①.** 갱신 자격은 **등록에 쓰인 flow 에서 온 요청**에 한한다. 그 판정을 수신 transport 와
저장 transport 의 일치로 근사한다 — 승격 TCP 는 등록 flow 와 transport 가 다르므로 걸러지고,
등록 flow 는 단말 keepalive 로 유지되므로 통과한다. 갱신 경로 두 곳에 동일하게 적용된다.

| 저장 \ 수신 | UDP | TCP (승격) | TLS |
|---|---|---|---|
| UDP | 갱신 | 차단 | 차단 |
| TCP | 차단 | 갱신 | 차단 |
| TLS | 차단 | 차단 | 갱신 |

저장=UDP 행은 "UDP 소스만 갱신"과 동치다 — UDP 로만 등록하는 배치에서는 관찰 가능한 동작이
달라지지 않는다.

"UDP 이거나 저장과 동일" 형태의 OR 규칙은 채택하지 않는다 — 저장=TLS 인데 UDP 요청 하나로
latch 가 UDP 주소로 덮이는 downgrade 구멍이 생기고, 저장=UDP 에서는 기여하는 바가 없다.

**관측**: latch 로그는 수신값과 갱신 판정 후 저장값을 함께 출력한다
(`user(..) is updated (ip:port:transport) latch(ip:port:transport)`) — 가드에 막히면 둘이
달라지므로 저장 상태는 `latch(...)` 쪽을 본다. 저장 flow 로 요청이 도착한 시각은
`CUserInfo::m_iLastSeenTime` 에 기록된다(진단용 — 판정에는 쓰지 않는다).

### 4.4 전환 규칙 — 새 transport 가 TCP 가 아닐 때

**규칙 ②.** 규칙 ①만으로는 전환(저장 UDP ≠ 수신 TLS)이 영구 차단된다. 판별 근거는 다음이다.

> **우발적으로 나타날 수 있는 transport 는 TCP 뿐이다.** 크기 초과 승격의 목적지는 TCP 이고,
> TLS 는 단말 설정 없이는 나타나지 않는다.

규칙: **인증된 REGISTER 이고 새 transport 가 TCP 가 아니면** latch 를 새 flow 로 교체한다.
TCP 로의 전환이 필요해지면 설정 스위치로 연다(기본 비활성).

메서드만으로 판별하는 규칙("REGISTER 면 허용")은 채택하지 않는다 — RFC 5626 `;ob` 플로우 재사용
때문에 재-REGISTER 도 승격 TCP 로 도착하므로 승격 오염을 걸러내지 못한다(실측 확인).

| 시나리오 | 규칙 ①+② 결과 |
|---|---|
| UDP → TLS 전환 | 허용 |
| TLS → UDP 롤백 | 허용 |
| 승격 TCP 재-REGISTER | 차단 (현 동작 보존) |
| TLS 단말의 rebind 추종 | 허용 |
| 저장 TLS 에 UDP 요청 | 차단 |

### 4.5 불일치 transport REGISTER 는 수명을 갱신한다

가드에 막힌 REGISTER 도 바인딩 수명·Contact 는 갱신한다. "수명 갱신까지 거부해 만료 sweep 으로
자가복구시키자"는 안은 **채택하지 않는다.**

규칙 ②가 정당한 전환을 흡수하므로, 남는 불일치는 **승격 TCP 재-REGISTER**(RFC 5626 `;ob` 플로우
재사용)뿐이다. 그 단말은 UDP 등록 flow 로 멀쩡히 살아 있고 latch 도 옳다 — 여기서 수명 갱신을
거부하면 **살아있는 단말을 등록 만료로 삭제**하게 된다. 즉 자가복구가 필요한 상태가 아니라
정상 상태다.

전환 경로가 막혀 latch 가 stale 이 되는 구간은 규칙 ②로 사라졌고, 그래도 남는 이상은
`CUserInfo::m_iLastSeenTime`(저장 flow 로 요청이 마지막에 도착한 시각)으로 관측한다.

### 4.6 정본 구조 — flow 단위 바인딩 집합

(IP, 포트)는 연결의 **대리키**일 뿐 연결 자체가 아니다. 그래서 두 한계가 남는다.

- 죽은 열쇠의 의미가 "신규 연결 시도"로 뒤집힌다([§2.2](#22-tcptls--저장값은-살아있는-연결을-찾는-열쇠)).
- NAT 이 포트를 재사용하면 죽은 열쇠가 다른 연결과 우연히 일치할 수 있다.

정확한 모델은 RFC 3261 §10.2.4 다중 바인딩 + RFC 5626 flow 단위다 — AoR 당 바인딩 **집합**을 두고
각 바인딩이 `{Contact, transport, flow(연결 핸들 또는 flow-token), 만료, last-seen}` 을 가지며,
서버 발신은 살아있는 등록 flow 를 선택한다. 이 모델에서는 승격 TCP 가 애초에 등록 flow 가 아니므로
후보에 들지 않고, 전환도 특수 규칙 없이 성립한다. 이중 등록·멀티 디바이스·reg-event 정합·재기동
구독 복구가 같은 모델로 정리된다.

이 구조의 설계는 [registration_binding_set.md](registration_binding_set.md) 가 정본이다 —
범위는 **flow 추적**(서버 내부 구조 교체, 단말 무변경)이며, 멀티 디바이스는 단말의
instance-id 가 기기 고유가 아니어서 별도 선행 과제로 분리했다.

## 5. 실패 모드의 변화

TLS 는 도달 신뢰성을 높이지만 실패의 성질을 바꾼다. 배치 전에 인지해야 하는 대가다.

| | UDP | TLS |
|---|---|---|
| 패킷 단위 | 독립 — 하나 잃어도 재전송으로 회복 | 연결 하나에 전 시그널링 집중 |
| 경로가 깨질 때 | 부분 유실 (해당 요청만) | **전체 단절** — 재등록까지 등록·통지·통화 도달이 동시 중단 |
| pjsip 의 반응 | 재전송 | transport 사망 처리 → **그 위 다이얼로그를 로컬 종료** |

마지막 행은 stream transport 일반의 성질이므로, 절전(doze) 중 연결이 반쯤 죽는 상황에서 통화가
로컬 종료되는 현상이 등록 flow 가 TLS 일 때도 성립할 수 있다. 검증에 절전 시나리오를 포함한다
([§10](#10-검증-시나리오)). 서버측 잔여 leg 은 세션 타이머가 회수한다([leg_liveness.md](leg_liveness.md)).

## 6. 서버 구현 상태

### 6.1 동작하는 것

| 항목 | 근거 |
|---|---|
| psip TLS 컴파일 | `ext/psip/SipStack/SipStackDefine.h` 가 `USE_TLS` 를 무조건 정의 — 빌드 옵션이 아니다 |
| 리스너별 SSL_CTX | `CSipStack::AddTlsListener` → `SSLServerCtxCreate(cert, key, ca)`. 미지정 시 stack-global ctx 폴백(`SSLAcceptWithCtx`) |
| 설정 소비 | `csp/CspListenerManager.cpp` 가 `local_nodes.jsonl` 의 `protocol=TLS` 행과 `tls_cert_path`/`tls_key_path`/`tls_ca_path` 를 소비 |
| primary 해석 | `csp/CspLocalNodeMap.cpp` `GetPrimaryByProtocol` — `is_primary` 가 없어도 `enabled && edge=access` 행이 primary 로 채택된다 |
| 부트 배선 | `csp/CspServer.cpp` 가 primary TLS 행에서 `Setup.Sip.TlsPort`/`CertFile`/`CaCertFile` 주입 → `CSipStack::Start` 가 리스너·SSL·worker pool 생성 |
| 도달 소켓 재사용 | accept 소켓을 `m_clsTlsSocketMap` 에 (IP:포트) 키로 보관 → 서버 발신이 그 연결을 재사용 |
| 타이머 정합 | 서버 유휴 종료 600초 > UE keepalive 90초 |
| 무중단 추가 | 런타임에 TLS 행을 추가하면 worker pool 을 지연 초기화해 접속점을 연다. 리스너별 인증서가 없으면 stack-global ctx 를 그 자리에서 기동하고, 그것도 불가하면 리스너를 만들지 않는다(조용히 죽는 리스너 금지) |
| 인증서·키 분리 | `tls_cert_path`/`tls_key_path` 를 부트 경로와 런타임 경로가 **같은 의미로** 사용한다. 키 미지정이면 인증서 파일에서 읽는다(cert+key 결합 PEM) |

### 6.2 접속점 개설 실패의 처리

**TLS·TCP 접속점 개설 실패는 그 접속점만 비활성으로 격리한다.** 인증서 오타 하나로 UDP·TCP 까지
내려가면 SIP 서버 전체가 기동하지 못하고 감독자 재시작 루프에 빠진다(실측). 실패해도 나머지
transport 는 서비스를 계속하고, 실패 사실은 로그와 알람으로 드러낸다.

| 신호 | 내용 |
|---|---|
| 로그 | `SSLServerStart() error — TLS 접속점(<port>) 비활성, 나머지 transport 는 계속` 등, 실패 지점별 ERROR |
| 알람 | **A-PRC-012 `listener_unavailable`**(major) — mo = `<node>/csp/listener/<proto>:<port>`, params = protocol·bind_ip·port. 접속점이 열리면 close. 카탈로그 정의는 `csp/config/fm_catalog.json` |
| accept 인계 실패 | worker pool 미초기화·포화로 수락한 연결을 닫을 때 ERROR 로그를 남긴다(무로그 close 금지 — 클라이언트에는 "handshake 직전 끊김"으로만 보인다) |

**제약**: 부트스트랩이 만든 TCP/TLS primary 리스너(id=0)는 `ListenerManager` 소유가 아니라
**런타임 제거가 불가**하다 — `local_nodes` 에서 행을 지워도 다음 재기동까지 유지된다.
`ListenerManager` 는 부트스트랩이 이미 바인딩한 접속점을 add 대상에서 제외한다(중복 bind 실패와
그로 인한 오탐 알람 방지).

### 6.3 설정

`local_nodes.jsonl` TLS 행:

| 필드 | 내용 |
|---|---|
| `protocol` | `TLS` |
| `bind_ip` / `bind_port` | 표준 배치는 5061. 현 access 배치가 15060 을 쓰므로 포트 번호는 배치 결정 사항 |
| `tls_cert_path` / `tls_key_path` / `tls_ca_path` | 리스너별 지정. 비면 stack-global 폴백 |
| `edge` | `access` (primary 해석 Rule 2 의 조건) |

fallback 키(`Setup.Sip.TlsPort`·`CertFile`·`TlsAcceptTimeout`)는 `csp/config/config_template.json`
의 `sip_fallback` 그룹에 있다. **인증서 파일 형식 함정**: stack-global 경로는 인증서와 개인키에
같은 파일을 넘기므로(`SSLServerStart`) cert+key 를 한 PEM 에 담아야 한다. 리스너별 경로는 key
파일을 분리할 수 있다(`tls_key_path` 미지정 시 cert 파일에서 읽는다).

## 7. 단말 구현 상태

TLS 로 등록·통화한다. 구성 요소는 다음과 같다.

| 항목 | 내용 |
|---|---|
| OpenSSL | android-arm64 정적 빌드(`android/docs/scripts/m1_build_openssl.sh` → `$HOME/opt/openssl-android-arm64`) |
| pjproject | `config_site.h` 의 `PJSIP_HAS_TLS_TRANSPORT 1` + `configure-android --with-ssl=<prefix>` (`m1_build_pjsip.sh`). SWIG 산출물은 불변 — `.so` 만 교체된다 |
| transport 생성 | `PjLib.kt` 가 UDP·TCP 에 이어 TLS transport 를 만든다. 실패해도 평문 transport 로 계속한다(구 `.so` 호환) |
| 서버 인증서 검증 | `TlsConfig.verifyServer = false` — 자가서명 인증서 배치 전제. CA 배포 방침이 서면 `caListFile`/`caBuf` 를 채우고 켠다([§8](#8-인증서-운영-요건-미정)) |
| 계정 설정 | 프로비저닝의 `sip.transport`/`sip.port` 를 그대로 registrar·proxy URI 에 반영(`;transport=tls`) |

### 7.1 전환 단위 — 가입자 단위 override

`{volte,ptt}_subscriptions.sip_transport`(`UDP`/`TCP`/`TLS`, NULL=서비스 설정)가 우선한다.
CSC `/provisioning/me` 가 이 값을 단말 프로파일의 `sip.transport` 로 내리고, **TLS 로 해석되면
포트도 서비스의 `tls_port`** 로 바꾼다 — 같은 포트로 평문과 TLS 를 겸하지 않기 때문이다.

| 설정 | 위치 |
|---|---|
| 가입자 override | `{volte,ptt}_subscriptions.sip_transport` (`sql/migrate_subscription_transport.sql`) |
| 서비스 기본 transport | `Provisioning.Services.{kind}.transport` |
| TLS 포트 | `Provisioning.Services.{kind}.tls_port` (CSP 의 `protocol=TLS` local_node `bind_port` 와 일치) |

단말은 **프로비저닝을 재취득해야** 전환이 반영된다(부팅 자동시작 경로에서 재취득 — 수동 실행은
캐시 설정을 쓴다).

## 8. 인증서 운영 요건 (미정)

| 항목 | 결정 필요 사항 |
|---|---|
| 발급 | 사설 CA 운영 여부, CN/SAN (단말이 서버를 IP 로 지칭하는지 FQDN 인지) |
| 배포 | 서버 인증서 경로·권한, 단말 CA 신뢰 저장소 주입 방식 |
| 검증 정책 | 단말의 서버 인증서 검증 on/off, 서버의 클라이언트 인증서 요구 여부(`tls_verify_peer`) |
| 갱신 | 만료 전 교체 절차. 무중단 교체는 T1·T2 수정에 종속 |
| 감시 | 만료 임박 알람 — 인증서 만료는 전 단말 동시 등록 불가로 이어지는 단일 장애점 |

## 9. 구현 계획

각 단계는 그 단계 끝에서 검증 가능한 단위로 나눈다.

latch 갱신·전환 규칙([§4](#4-latch-갱신-규칙))·서버 접속점([§6](#6-서버-구현-상태))·단말
([§7](#7-단말-구현-상태))은 구현·실측 완료다. 시험 클라이언트는 `cspsim -transport tls`.
남은 단계는 다음과 같다.

| 단계 | 작업 | 검증 | 산출 |
|---|---|---|---|
| **3. 인증서 운영** | [§8](#8-인증서-운영-요건-미정) 결정 — 사설 CA·검증 정책·갱신·만료 감시 | 단말 `verifyServer=true` 로 등록 성립 | csc/console + 단말 |
| **4. 구조** | [§4.6](#46-정본-구조--flow-단위-바인딩-집합) flow 단위 바인딩 집합 | — | 설계 문서 선행 |

의존 관계:

```
3단계 (인증서 운영) ──▶ 4단계 (구조)
```

**cspsim TLS 클라이언트화가 순서상 중요하다** — psip 에 클라이언트 TLS 경로(`SSLClientStart`,
`StartSipTlsClientThread`, 클라이언트 연결로의 인바운드 요청 수신)가 이미 있으므로, `cspsim` 에
TLS 설정 필드와 목적지 transport 지정을 추가하면 **단말 빌드 트랙 없이 서버측 전 구간을 실측**할
수 있다. 가장 비싼 의존(3단계)을 검증 경로에서 분리한다.

## 10. 검증 시나리오

| # | 시나리오 | 확인 |
|---|---|---|
| 1 | TLS 등록 | REGISTER 200 OK, latch 에 (IP, 포트, TLS) 저장 |
| 2 | fan-out INVITE 도달 | 그룹콜 착신 |
| 3 | conference NOTIFY 도달 | 로스터 정합 (마지막 이탈자 포함) |
| 4 | **180초+ 통화** | 세션 갱신 2회 이상 통과, `cause=408` BYE 0건 |
| 5 | 소스 포트 변경 후 재등록 (rebind) | latch 추종 → 1~4 재성립 |
| 6 | UDP 등록 상태에서 TLS 재등록 (전환) | `registration flow switched: ip:port:0 → ip:port:2` + latch 추종 |
| 7 | UDP 계정의 승격 TCP 오염 차단 | 회귀 확인 (과도기 필수) |
| 8 | 절전(doze) 구간 통과 | [§5](#5-실패-모드의-변화) 의 로컬 종료 발현 여부 |
| 9 | TLS 리스너 hot-add / 잘못된 인증서로 부트 | handshake 성립 / 서버는 뜨고 TLS 만 비활성 + A-PRC-012 open |

진단 시 목적지 판정은 `Target=` 표기가 아니라 직후의 `UdpSend`/`TcpSend`/`TlsSend` NETWORK 로그를
정본으로 본다. latch 갱신 로그의 transport 값은 **수신값**이므로 저장 상태 판정에 쓸 수 없다
(0단계에서 저장값 출력을 추가한다).

## 11. 관련 파일

| 파일 | 역할 |
|---|---|
| `csp/UserMap.{h,cpp}` | latch 저장·갱신(`Insert`/`SetIpPort`), 만료 sweep, OPTIONS keepalive |
| `csp/ModuleDispatcher.cpp` | 주소 변경 감지 갱신(`EventIncomingRequestAuth`), in-dialog 목적지 제공(`EventGetLegDest`) |
| `csp/GroupCallService.cpp`, `csp/CspServer.cpp` | latch 소비 (fan-out INVITE, NOTIFY 2종) |
| `csp/CspListenerManager.{h,cpp}` | `local_nodes.jsonl` → 리스너 add/remove (UDP/TCP/TLS) |
| `csp/CspLocalNodeMap.cpp` | primary 리스너 해석 |
| `ext/psip/SipStack/SipStack.cpp` | 리스너 생성·pool 초기화 (T1·T2) |
| `ext/psip/SipStack/SipTlsThread.cpp` | TLS accept·worker (T1·T4) |
| `ext/psip/SipStack/SipTlsClientThread.cpp` | 아웃바운드 TLS 클라이언트 |
| `ext/psip/SipStack/TlsFunction.cpp` | SSL ctx 생성·accept·connect |
| `ext/psip/SipStack/TcpSocketMap.cpp` | 연결 재사용 맵 (TCP·TLS 공용) |
| `ext/psip/SipStack/SipStackComm.hpp` | 송신 transport 분기, 수신 Via 각인 |
| `android/docs/scripts/m1_build_pjsip.sh` | UE pjproject 빌드 (TLS 비활성 상태) |
| `android/core/.../sip/PjLib.kt` | UE transport 생성 |
| `csc/src/services/mcptt.py` | 프로비저닝 `transport` 제공 |
