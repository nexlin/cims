# SIP 시그널링 transport — 도달 모델과 UDP/TCP/TLS 선택 지원

단말(UE)이 SIP 시그널링 transport 를 **UDP·TCP·TLS 중에서 고르는** 배치의 정본 문서다. 특정
transport 로 **일괄 전환하는 것이 목표가 아니다** — 서버는 세 transport 를 동시에 청취하고, 어떤
조합으로 등록해도 도달한다. 미디어(RTP)는 대상이 아니다 — 계속 UDP 를 쓴다.

핵심 주제는 둘이다: ①연결지향 transport 에서 **서버→UE 도달 주소의 의미가 UDP 와 다르다**는 점,
②그래서 도달 주소를 transport 종류로 추측할 수 없고 **경로(flow)의 생존으로 판정**해야 한다는 점.

관련 문서: [ue_nat_traversal.md](ue_nat_traversal.md) · [leg_liveness.md](leg_liveness.md) ·
[sip_runtime_config.md](sip_runtime_config.md) · [modules/csp.md](../modules/csp.md) ·
[android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md)

> **상태**: 서버는 세 transport 를 동시에 서비스하고 단말이 가용 목록에서 고른다
> ([§7.1](#71-선택-모델--단말이-고르고-서버는-가용-목록을-준다)). 혼합 운용 실측 완료(001=UDP·002=TLS
> 동거, 그룹콜 성립). 인증서는 **2단 사설 PKI**(개발사 오프라인 루트 → 사이트 CA → 서버 leaf,
> [§8](#8-인증서-운영))이며 단말은 루트 한 장을 앵커로 CSP TLS·CSC 4430 양 평면의 서버 인증서를
> **검증한다** — 신뢰하지 않는 인증서는 등록·로그인이 거절된다(`PJSIP_TLS_ECERTVERIF`). 남은 것은
> 사이트 CA 발급 도구와 루트 키 오프라인 이관이다([§9](#9-남은-과제)).

## 1. 범위와 전제

| 항목 | 내용 |
|---|---|
| 대상 | UE ↔ CSP access edge 의 SIP 시그널링 |
| 비대상 | RTP/RTCP(계속 UDP), floor control(RTCP APP), MSRP |
| 표준 배치 | `lb-access-tls :5061` ([02_deployment.md](../02_deployment.md)) |
| **선택 주체** | **단말**. 서버는 가용 transport 를 제공하고 강제하지 않는다([§7.1](#71-선택-모델--단말이-고르고-서버는-가용-목록을-준다)) |
| 혼합 운용 | 전제이자 정상 상태다 — 같은 그룹에 UDP 단말과 TLS 단말이 함께 있어도 서버가 각 단말의 등록 경로로 보낸다(실측) |
| 현재 배치 | `local_nodes.jsonl` = UDP:15060 + TCP:15060 + TLS:15061(랩 인증서). 실기기 001=UDP / 002=TLS |

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

### 3.3 TLS 를 고른 단말에서 소멸하는 문제군

| 문제 | TLS 를 고른 단말 |
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

### 4.2 스트림 등록 단말에서 무엇이 달라지는가

| 상황 | 처리 |
|---|---|
| 주소 변경 (NAT rebind·망 전환, transport 동일) | 같은 transport 재등록으로 바인딩 교체 |
| transport 변경 (단말이 다른 것을 고름) | 새 바인딩 추가. 옛 경로는 flow 실패 또는 만료로 회수 |
| 승격 TCP (대형 요청 이후의 ACK/BYE) | 바인딩을 만들지 않는다 — 생성은 등록의 권한이다 |

이 규율이 없으면 증상은 "등록 실패"가 아니라 더 나쁘다 — 단말은 200 OK 를 받아 등록됨으로
표시되고 **발신도 되는데**, 착신·통지가 안 되고 통화가 세션 타이머 주기로 끊긴다. 서버 발신
목적지만 죽은 경로를 가리키기 때문이다.

### 4.3 갱신 자격 — 등록된 flow 에서 온 요청만

CSP 는 가입자당 **바인딩 집합**을 갖고, 각 바인딩이 도달 경로(flow) 하나다
([registration_binding_set.md](registration_binding_set.md) 가 정본).

| 규율 | 내용 |
|---|---|
| 생성 권한 | **REGISTER 만** 새 바인딩을 만든다(RFC 3261 §10). 대형 요청 승격 후의 ACK/BYE 는 새 경로로 보여도 등록된 flow 가 아니므로 무시된다 |
| 주소 이동 | 비REGISTER 요청의 소스 변경은 **그 transport 의 기존 바인딩**만 옮긴다 |
| 선택 | 서버 발신은 **살아있는 바인딩 중 최신**. 생존은 psip 소켓맵에 직접 질의(`CSipStack::IsFlowAlive`) — transport 종류로 추측하지 않는다 |
| 만료 | 바인딩 단위. 마지막 바인딩이 사라질 때만 등록 해제로 통지 |

승격 TCP 로 온 재-REGISTER 는 바인딩이 되지만, 그 연결이 닫히면 생존 판정에서 탈락하므로
도달 주소를 오염시키지 않는다. transport 전환(UDP 등록 단말의 TLS 재등록)도 **새 바인딩 추가**로
자연히 성립한다 — 별도 판별 규칙이 필요 없다.

**관측**: latch 로그는 수신값과 바인딩 수를 함께 출력한다
(`user(..) is updated (ip:port:transport) bindings(N)`), 새 경로가 등록되면
`binding added (...) — total N`, 경로가 옮겨지면 `binding moved → ...` 이 남는다.

### 4.4 도달 경로 구조 — 바인딩 집합

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
| Service-Route transport | 200 OK 의 `Service-Route` 가 TCP/TLS 등록에는 `;transport=tcp|tls` 와 **그 리스너의 포트**를 싣는다(RFC 3608/TS 24.229 — 없으면 route 를 따르는 후속 요청이 UDP 로 강등). Setup 기동 primary TCP/TLS 리스너(id 0)는 `CspAddressing::GetLocalSipPortForTransport` 가 transport 별 Setup 포트로 폴백 |

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
| pjproject | `config_site.h` 의 `PJSIP_HAS_TLS_TRANSPORT 1` + `configure-android --with-ssl=<prefix>` (`sdk/android/build-native.sh`, config_site 정본 `sdk/engine/config_site/common.h`). SWIG 산출물은 불변 — `.so` 만 교체된다 |
| transport 생성 | `PjLib.kt` 가 UDP·TCP 에 이어 TLS transport 를 만든다. 실패해도 평문 transport 로 계속한다(구 `.so` 호환) |
| 서버 인증서 검증 | `TlsConfig.verifyServer = true` + `caBuf = CimsTrustStore.CA_BUNDLE`(APK 동봉 루트 CA — 사이트 CA·leaf 는 서버 체인으로, §8.1). 검사 자체는 플래그와 무관하게 항상 수행돼 `verify_status` 에 기록되고, 이 플래그가 **실패 시 연결을 끊을지**를 결정한다(`sip_transport_tls.c` 의 `verify_status && verify_server`). 실패 시 transport shutdown → 등록 503 `PJSIP_TLS_ECERTVERIF`. ⚠ `caListFile`/`certFile`/`privKeyFile` 이 설정되면 `caBuf` 가 무시된다 |
| 계정 설정 | 프로비저닝의 가용 목록에서 고른 transport·포트를 registrar·proxy URI 에 반영(`;transport=tls`). 선택 모델은 [§7.1](#71-선택-모델--단말이-고르고-서버는-가용-목록을-준다) |

### 7.1 선택 모델 — 단말이 고르고 서버는 가용 목록을 준다

**단말이 transport 를 자유롭게 고른다.** 서버는 강제하지 않는다 — 세 transport 를 동시에 청취하고,
단말이 등록해 온 경로가 그대로 그 단말의 도달 경로(바인딩)가 된다
([registration_binding_set.md](registration_binding_set.md)).

그래서 프로비저닝은 "하나의 transport 를 지정"하는 것이 아니라 **가용 목록과 기본값을 알려주는**
역할이어야 한다. transport 마다 포트가 다르기 때문에 목록에 포트가 함께 실린다 — 같은 포트로
평문과 TLS 를 겸하지 않는다.

```json
"sip": {
  "host": "121.161.164.45",
  "domain": "ptt.cims.example.kr",
  "transports": [ { "transport": "UDP", "port": 15060 },
                  { "transport": "TCP", "port": 15060 },
                  { "transport": "TLS", "port": 15061 } ],
  "default": "UDP"
}
```

`sip.port`/`sip.transport` 단일 필드는 **기본값의 유효 쌍**으로 함께 남는다 — 목록을 모르는 구 APK 가
이 두 필드만 읽기 때문이다. 목록이 없는 응답을 받은 단말은 선택 UI 를 숨기고 단일 필드로만 동작한다.

| 설정 | 역할 |
|---|---|
| `Provisioning.Services.{kind}.port` | 평문 포트. UDP 항목의 포트이며, `tcp_port` 미설정 시 TCP 도 이 값을 쓴다(CSP 는 UDP/TCP 를 같은 포트로 청취) |
| `Provisioning.Services.{kind}.tcp_port` | TCP 를 다른 포트로 분리 운용할 때만 지정. `0`/미설정 = 평문 포트 공용 |
| `Provisioning.Services.{kind}.tls_port` | TLS 포트. **`0`/미설정 = 목록에 TLS 를 넣지 않는다** — TLS `local_node` 를 만들기 전에 죽은 선택지를 광고하지 않기 위함 |
| `Provisioning.Services.{kind}.transport` | 서비스 기본값 |
| `{volte,ptt}_subscriptions.sip_transport` | **가입자 기본값**(NULL=서비스 기본값). `UDP`/`TCP` 는 권장값이며 단말이 바꿀 수 있다. **`TLS` 는 서버가 집행하는 채널 정책** — 목록이 TLS 하나로 좁혀지고(`sip.enforced=true`) 비-TLS 채널의 요청은 403 ([sip_access_security.md §3](sip_access_security.md)). `auth_scheme=aka` 가입자는 이 값과 무관하게 같은 강제 대상이다(§8.2) |

각 포트는 CSP `local_nodes` 의 `bind_port` 와 일치해야 한다 — CSC 는 CSP 가 실제로 무엇을 청취
중인지 조회하지 않고 위 설정을 그대로 내려보낸다. 기본값이 목록에 없으면(예: `sip_transport=TLS`
인데 `tls_port` 미설정) **목록의 첫 항목으로 강등**한다. 도달 불가한 기본값을 내리지 않는다.

#### 단말의 선택 — 저장·유지·반영

| 단계 | 동작 |
|---|---|
| 선택 | 설정 화면이 가용 목록만 선택지로 띄운다(항목 라벨 = `TLS · 15061`). 선택지가 2개 미만이면 행을 숨긴다 |
| 저장 | `ConfigStore.saveTransportChoice()` — `SipAccountConfig.withTransport()` 가 **transport 와 포트를 함께** 바꾼다. transport 만 바꾸면 옛 포트에 새 프로토콜로 붙어 등록이 실패한다 |
| 유지 | 사용자 선택 표시가 남아 **프로비저닝 재취득이 선택을 덮지 않는다**(`ConfigStore.saveProvisioned()`). 선택한 transport 가 새 가용 목록에서 사라지면 서버 기본값으로 강등하고 표시도 지운다 |
| 반영 | 설정 변경 경로를 그대로 탄다 — un-REGISTER → 2초 후 프로세스 재시작 → 새 계정으로 첫 등록 + 참여 채널 자동 복원. 계정 재생성이 필요하므로(registrar/proxy URI 가 transport·포트를 품는다) 재등록만으로는 부족하다. **가용 목록만 바뀐 경우는 재시작하지 않는다**(`sameRegistration()`) |
| 재취득 | 부팅 자동시작 시 항상 재취득한다(VoLTE·PTT 동일). PTT 설정의 `서버 설정 다시 받기` 로 사용자가 즉시 최신화할 수도 있다 |

수동 설정 모드(전 항목 직접 입력, 시험용)는 그대로 남는다 — 자동 구성을 끄지 않고 transport 만
고르는 경로가 위 선택이고, 수동 모드는 프로비저닝 자체를 중지시키는 별개 축이다.

서버측은 추가 작업이 없다 — 바인딩 집합이 transport 무관이고, 단말이 경로를 바꾸면 새 바인딩으로
자연히 반영된다(옛 경로는 같은 transport 재등록 교체 또는 flow 실패로 회수).

## 8. 인증서 운영

> **구현 상태**: 단말(앵커 1장으로 X.509 경로 검증)·서버(체인 파일 전송·무중단 교체)·감시(A-PRC-009)·
> 발급 도구(`service-cert.sh site-ca`/`issue`/루트 기준 `verify`)·**만료 방어**([§8.6](#86-만료-방어--leaf-자동-갱신과-만료-안내)
> — lifecycle 엔진의 leaf 자동 갱신·CSP 편입·같은 경로 내용 교체의 무중단 재적재·자동 갱신 실패 알람·관제조작반
> 경고·verify 게이트)는 구현이다. 앵커인 루트 인증서는 지금 APK 에 든 것이고, 루트가 직접 서명한 기존 leaf 도
> 유효한 경로라 배치된 노드는 무변경으로 이 구조 안에 있다(교차 인증서를 배치하면 다음 스윕에 체인으로 전환된다).
> **현 세대 루트(10년, 2036 만료)는 임시 사이트 배포 단계 동안 그대로 쓰고, 루트 CA 재발급은 버전 1.0 안정화
> 이후**다(§8.1, §9). 남은 것은 [§9](#9-남은-과제) — 루트 키 오프라인 이관과 사내 사이트 교차 인증서 배치(운영),
> 콘솔 상단 배너(도안)·Android 앱 표시, 루트 재발급.

### 8.1 PKI 구조 — 개발사 오프라인 루트 → 사이트 CA → 서버 leaf (2단)

```
CIMS 루트 CA       개발사 오프라인 보관. 자가서명 RSA 4096. 현 세대 2036 만료 → 1.0 안정화 이후 재발급. 단말이 내장하는 유일한 앵커
  └─ 사이트 CA     현장(고객·설치 단위)마다 1개. 기본 = 그룹 CA 키에 루트 교차 인증서(RSA 2048 이상, 신규 3072+). 유효기간 = 루트 만료일, CA:TRUE pathlen:0. 루트가 1회 서명
       └─ 서버 leaf CSC(4421·4430 공용 1장)·CSP TLS 접속점. RSA 2048, 2년, EKU=serverAuth, CA:FALSE
                   SAN = IP:<단말 접속 주소>, IP:<관리망 주소>, DNS:csc.cims.local | DNS:csp.cims.local
```

| 층 | 키 보관 위치 | 키가 쓰이는 때 | 단말이 아는가 |
|---|---|---|---|
| 루트 CA | 개발사, **오프라인 매체** — 네트워크에 없다. 생성부터 매체에서 한다 | 사이트 CA 서명(사이트당 1회 — 기본은 그룹 CA 교차 인증서) · 고객 CA 교차 서명 | **유일한 앵커** — APK `CimsTrustStore.CA_BUNDLE`, Windows 관제조작반 CA PEM, cimsue-cli `--ca` |
| 사이트 CA | 현장 OAM 노드 `_secrets/ca/`(= 그룹 CA 키에 루트 교차 인증서, 기본 — §8.6.1) 또는 별도 CA 보관 서버(§8.3 방식 A/B), 키 600 | 서버 leaf 발급 — 노드 추가·IP 변경·2년 갱신(엔진이 자동). 루트와 같은 날 만료하므로 갱신 이벤트가 없다 | 모른다. 서버가 체인으로 보낸다 |
| 서버 leaf | 각 노드 `runtime/cert/` | TLS 핸드셰이크 | 모른다 |

이 구조가 고정하는 성질:

- **현장과 개발사 사이에 네트워크가 없어도 된다.** 루트 키는 사이트 CA 를 서명할 때만 쓰이고 그 결과물
  (사이트 CA 인증서, 방식에 따라 키)은 설치 매체로 현장에 들어간다. 이후 노드 추가·IP 변경·leaf 갱신은
  현장 안에서 끝나고, 단말은 개발사에 닿을 일이 없다.
- **APK 는 사이트 수·서버 주소·사이트 CA 교체와 무관하다.** 앵커는 루트 한 장이고 사이트 CA 와 leaf 는
  서버가 핸드셰이크 `Certificate` 목록으로 보내므로 단말은 표준 경로 검증만 한다(pjsip OpenSSL·OkHttp
  모두 추가 코드 없음). 서버 IP 변경 = 현장에서 leaf 재발급이지 APK 가 아니다.
- **사이트 격리.** 사이트 CA 키가 새어도 다른 사이트의 서버 인증서를 만들 수 없고, `pathlen:0` 이라 하위
  CA 도 만들지 못한다. 루트 키는 온라인에 없으므로 침해 면이 사이트 CA 하나로 좁혀진다.
- **고객 PKI 수용.** 고객이 자기 CA 로 서버 인증서를 발급하려면 루트가 그 CA 인증서를 **교차 서명**한다
  (고객 CA 가 사이트 CA 자리에 선다). 체인은 leaf + 교차서명된 고객 CA 인증서이고 단말은 변화가 없다.

**유효기간 정책 — 사람이 하는 인증서 이벤트를 둘로 줄인다.** 인증서는 폐기 경로가 없어 만료가 곧 회수
시한이지만, 만료를 놓치면 단말 전체가 로그인·등록을 못 하는 장애다. 그래서 기간은 "잊어도 오지 않게"
잡는다: **루트는 가장 길게**(현 세대 2036 만료. 1.0 안정화 이후 재발급하며 유효기간은 그때 정한다 — APK 앵커
교체 = 개발사 이벤트) · **사이트 CA = 루트 만료일**(개설 시점의 루트 잔여 기간으로 발급 — 사이트 CA 갱신이라는
개발사 왕복이 사라진다) · **leaf 2년**(현장, 무중단, lifecycle 엔진이 자동 갱신 — §8.6). 남는 사람 이벤트는
루트 교체와 leaf 갱신뿐이고, leaf 는 A-PRC-009 가 지킨다. 루트 기간은 키 알고리즘의 권고 수명과 양자내성
전환 가능성을 보고 재발급 시점에 정하되, 어느 경우든 §8.5 루트 교체 절차(두 장 병기)가 그 경로다. Apple 의
825일 상한은 서버 leaf 에만 걸리므로 루트·사이트 CA 기간과 무관하다.

**현 세대 루트 = 현 APK 앵커 그대로.** `C=KR, O=CIMS, CN=CIMS Service CA`(2026-08-18 ~ 2036-08-15,
`basicConstraints=critical,CA:TRUE` pathlen 없음, `keyUsage=keyCertSign,cRLSign`)는 하위 CA 를 서명할 수
있는 인증서라 그대로 루트다. 임시 사이트 배포 단계에서는 앵커를 건드리지 않는다 — APK·Windows CA PEM
무변경, 루트가 직접 서명한 기존 leaf(경로 1단)도 유효하므로 배치된 노드는 그대로 두고 **다음 발급(2년
갱신·IP 변경·신규 노드)부터 사이트 CA 를 거친다.** 이 세대의 사이트 CA 는 2036-08-15 까지다.

**루트 CA 재발급은 버전 1.0 안정화 이후**에 한다(§9 #6). 새 루트는 **오프라인 매체에서 생성**해 처음부터
네트워크에 오른 적 없는 키로 시작하고(`CN=CIMS Root CA G1`, 유효기간은 그때 결정), 절차는 §8.5 루트 교체 그대로다 —
APK `CA_BUNDLE` 병기 → Windows CA PEM 교체 → 각 사이트 CA 를 **같은 키로 새 루트 아래 재서명**(만료일을
새 루트에 맞춤) → 노드 체인 파일의 두 번째 장 교체(§8.4 무중단) → 다음 APK 에서 구 루트 제거. 사이트 CA
키가 유지되므로 leaf 는 손대지 않고, 그때까지 발급된 사이트 CA 의 2036 만료도 재서명으로 연장된다.
전환 전에 현장이 늘수록 APK 배포 대상이 늘어나므로 1.0 릴리스 계획에 앵커 전환을 포함한다.

사이트 식별자 `site_id` 는 짧은 불변 슬러그(예 `hq-lab`, 고객 코드)다. 별도 사이트 CA(§8.3 방식 A/B)의 CN
(`CIMS Site CA <site_id>`)과 CA 디렉터리 이름에만 쓴다([identifier_model.md](../identifier_model.md)
— 표시명은 키가 아니다). 기본 방식의 교차 인증서는 그룹 CA 의 subject(`CN=CIMS-OAM-CA`)를 그대로 유지한다
— leaf 의 issuer 이름이 관리평면 체인과 같아야 하기 때문이다.

**이름 제약(선택).** 사이트 CA 에 RFC 5280 `nameConstraints` 를 걸면 그 사이트 주소 대역 밖의 leaf 는
발급되어도 단말이 거절한다(OpenSSL·Android 모두 집행). 기본은 걸지 않는다 — leaf SAN 은 agent 의 SAN
요구 목록(`DNS:<hostname>`·`IP:127.0.0.1`·노드 IPv4 전부·VIP, §8.3)의 상위집합이어야 하므로 제약을
걸려면 `IP:127.0.0.1/32`·관리망 대역·`DNS:<hostname>` 각각을 permitted 에 넣어야 하고, 하나라도 빠지면
그 노드 인증서가 통째로 거절된다. 고객 요구가 있을 때 사이트 단위로 결정한다.

관리평면(OAM↔agent mTLS)의 그룹 CA(`CIMS-OAM-CA`, `agent/lib/cert.sh`)의 **자가서명 인증서는 이 계층 밖**
이다 — 관리평면 신뢰 앵커를 사용자 기기로 내보내지 않는다. 대신 **그 키에 루트가 교차 인증서를 발급해
사이트 CA 로 삼는다**(§8.6.1): 관리평면은 종전대로 자가서명 그룹 CA 를 앵커로, 단말 대면(CSP TLS 접속점·
CSC 4421/4430)은 루트 → 교차 인증서 → leaf 체인으로 검증한다. 그래서 lifecycle 엔진의 노드 인증서 자동
발급이 단말 대면 인증서까지 맡고, 수동 발급(§8.3 (1)~(4))은 엔진이 없는 경우의 폴백이다.

CSC 인증서는 **버전무관 `<install>/../runtime/cert/server.{crt,key}`** 에 둔다. csc 는 이 경로를
버전 디렉터리보다 먼저 찾으므로(`csc_app.py`) 모듈 업그레이드에도 살아남는다 — 버전 디렉터리
(`current/csc/cert/`)에만 두면 업그레이드가 패키지 자가서명 인증서로 되돌려 놓는다.

### 8.2 서버는 체인을 전송한다

psip 은 `SSL_CTX_use_certificate_chain_file()` 로 인증서를 적재한다 — PEM 의 첫 인증서를 서버
인증서로, 뒤에 이어붙인 인증서를 중간 CA 체인으로 등록해 핸드셰이크 `Certificate` 목록에 함께
싣는다. CSC 도 같은 체인 파일(`server.crt`)을 `load_cert_chain` 으로 적재해 같은 목록을 보낸다.

`tls_cert_path` 는 **leaf + 사이트 CA**(기본 방식에서는 그룹 CA 의 교차 인증서 `ca-cross.crt`) 두 장을 이어붙인
체인 PEM 을 가리키고, 키는 `tls_key_path` 로 분리한다. **루트는 싣지 않는다** — 단말이 이미 가진 앵커이며 실어도 무해하지만 핸드셰이크만 커진다.
고객 PKI 교차 서명이면 두 번째 장이 교차서명된 고객 CA 인증서다. 루트 직서명 leaf 는 체인 1장이며
그대로 유효하다. 단말이 경로를 완성하지 못하는 유일한 경우는 **사이트 CA 장이 빠진 체인**이다 —
`verify` 의 "체인 2장" 판정이 이것을 잡는다.

### 8.3 발급·배치 절차 — 사이트 CA 는 개발사에서 1회, 서버 leaf 는 현장에서

새 노드에 패키지를 설치하면 CSC 는 agent 가 그룹 CA(`CIMS-OAM-CA`)로 자동 발급한 인증서를,
CSP 는 동봉 자가서명(`cert/csp.pem`)을 쓴다. 둘 다 루트 아래에 있지 않아 **단말은 로그인(HTTPS 4430)부터
TLS 거절로 막힌다** — CSC 로그에는 요청이 남지 않아 서버에서는 원인이 보이지 않는다. 그래서 단말 대면
노드마다 사이트 CA 발급 leaf 를 배치하는 단계가 초도 설치에 들어간다([initial_install.md §4.5](../../user-manual/initial_install.md#45-단말-대면-tls-인증서--없으면-단말이-로그인하지-못한다)).
절차와 검증은 `scripts/service-cert.sh` 한 파일이 담당하고, 묶음(bundle)에 자기 자신을 복사하므로
현장에서는 묶음과 `openssl` 만 있으면 된다. **(0) 의 교차 인증서만 배치하면 (1)~(3) 을 lifecycle 엔진이
자동으로 하고**(§8.6.1), 아래 (1)~(4) 수동 단계는 별도 사이트 CA 현장(방식 A/B)·엔진 없는 노드의 폴백이다.

**(0) 사이트 CA — 사이트 개설 시 1회, 개발사 오프라인 루트로.** 네 방식 중 하나 — 기본은 그룹 CA 교차
서명이다. 루트 키는 이 단계에서만 쓰이고 오프라인 매체를 떠나지 않는다.

| 방식 | 절차 | 쓰는 때 |
|---|---|---|
| **기본. 그룹 CA 교차 서명** | 현장 OAM 노드의 그룹 CA 인증서(`<oam>/runtime/_secrets/ca/ca.crt`, 설치 시 엔진이 생성)를 매체로 개발사 → `service-cert.sh site-ca sign --cross ca.crt` → `ca-cross.crt` 를 매체로 회수해 같은 디렉터리에 배치(join 이 피어에 복사). 키는 현장을 떠나지 않고, 이후 leaf 발급·갱신은 엔진이 자동(§8.6.1) | 모든 현장의 기본 |
| A. 별도 사이트 CA(턴키) | 루트 매체에서 `site-ca issue --site <site_id>` → 사이트 CA 키+인증서 → 설치 키트에 동봉 → 현장 CA 보관 서버 `<CA 디렉터리>/<site_id>/cims-site-ca.{crt,key}`(키 600). leaf 는 (1)~(4) 수동 | 사이트 CA 키를 OAM 노드 밖에 두어야 하는 현장 |
| B. 별도 사이트 CA(현장 키) | 현장에서 `site-ca csr --site <site_id>`(키+CSR) → CSR 만 매체로 → `site-ca sign <csr>` → 인증서 회수. 이후는 A 와 같다 | 위와 같되 키 생성도 현장이 하는 경우 |
| C. 고객 PKI | 고객 CA 인증서를 `site-ca sign --cross` 로 교차 서명. leaf 발급은 고객 PKI 절차, 체인 두 번째 장이 교차서명 인증서 | 고객이 자기 PKI 를 쓰는 현장 |

사이트 CA 프로파일: `basicConstraints=critical,CA:TRUE,pathlen:0` · `keyUsage=critical,keyCertSign,cRLSign`
· `subjectKeyIdentifier=hash` · `authorityKeyIdentifier=keyid` · **유효기간 = 루트 만료일**(개설 시점의 루트
잔여 일수 − 1). 하위 인증서는 상위보다 오래 살 수 없으므로 루트 만료 2년 전부터는 leaf 도 루트 만료일로
잘린다 — 그 시점은 이미 루트 교체 병기 기간이다.

**(1)~(4) 서버 leaf — 현장 CA 보관 서버와 대상 노드에서.** 묶음에는 사이트 CA 인증서와 노드 leaf/키,
그리고 단말 앵커 대조·Windows CA PEM 용 **루트 인증서**가 들어가고 사이트 CA 개인키는 들어가지 않는다.

| 단계 | 실행 위치 | 명령 | 하는 일 |
|---|---|---|---|
| issue | 현장 CA 보관 서버 | `service-cert.sh issue --ip <노드 IP> [--vip <VIP>] [--ca-dir <CA 디렉터리>/<site_id>]` | ssh 로 대상 노드에서 `collect` 를 돌려 hostname·SAN 을 자동 수집한 뒤 사이트 CA 로 csc·csp leaf 2장(RSA 2048·2년·EKU=serverAuth) 발급 → 체인 PEM(leaf+사이트 CA)·키·사이트 CA 인증서·루트 인증서·README·스크립트를 담은 묶음 `<CA 디렉터리>/cert-init-<host>/` + `.tgz`(있으면 덮어씀). `--vip` 는 HA 대표 주소를 SAN 에 추가, `--runbook FILE` 은 노드 전용 절차 문서를 동봉. 묶음의 `README.txt` 가 설치 완료 시점부터의 현장 절차(0~6 단계·출력/증상 판독·원복)를 전부 담아 **현장에는 tgz 와 openssl 만 있으면 된다**. ssh 가 없으면 대상 노드에서 `collect` 를 직접 돌려 `--host/--san` 으로 넘긴다 |
| collect | 대상 노드 | `service-cert.sh collect [--prefix P]` | 필요한 SAN 목록 출력(issue 가 ssh 로 대신 호출한다). agent `cert.sh` 가 있으면 그 함수(`_node_cert_san`)를 그대로 호출해 규칙이 어긋나지 않는다 |
| install | 대상 노드 | `bash cert-init-<host>/service-cert.sh install [--prefix P]` | 배치 전 노드 요구 SAN 과 대조(부족하면 거절) → CSC `runtime/cert/server.{crt,key}` 백업 후 교체(30초 핫리로드 확인) → CSP `runtime/cert/` 에 체인·키 배치 → `local_nodes` 에 넣을 값 안내 |
| csp-node | 대상 노드 | `bash cert-init-<host>/service-cert.sh csp-node [--port N] [--dry-run]` | OAM 에 로그인해 csp 배포의 `local_nodes` 를 읽고 TLS 행의 `tls_cert_path`/`tls_key_path` 를 배치 경로로 바꿔 `PUT …/collection/local_nodes` 로 저장(콘솔 저장과 같은 경로 — 스키마 검증·agent jsonl 원자 쓰기·SIGUSR1). 행이 없으면 `--port` 로 `access-tls` 생성. 끝나면 그 포트로 verify |
| verify | 어디서든 | `bash cert-init-<host>/service-cert.sh verify --csp-port <TLS 포트>` | 묶음에서 접속 IP 를 읽어(`--ip` 로 대체 가능) 포트별로 **루트를 앵커로** 판정한다: 체인 2장 전송·발급자=사이트 CA·루트까지 경로 OK·IP 신원 OK·**틀린 이름 거절(음성 대조군)**·만료 잔여(leaf·사이트 CA 중 이른 것). FAIL 이 하나라도 있으면 종료코드 1 |

CSP 는 파일을 두는 것만으로는 쓰지 않는다 — `local_nodes` TLS 행의 `tls_cert_path`(체인)/
`tls_key_path`(키)가 그 파일을 가리켜야 한다. `csp-node` 가 OAM 컬렉션 API 로 그 행을 갱신하며(파일 직접
편집은 다음 설정 push 에 되돌아가므로 하지 않는다), 콘솔에서 손으로 저장해도 같다. 저장 시 SIGUSR1 로
무중단 반영된다(§8.4).

**SAN 상위집합 조건.** agent 의 노드 인증서 보증(`agent/lib/cert.sh`)은 `O=CIMS` 인증서를 자기
관리 대상으로 보고, 요구 목록(`DNS:<hostname>`·`IP:127.0.0.1`·노드 IPv4 전부·VIP·
`Server.AgentOamUrl` host·`Server.CertSans`) 중 빠진 것이 있으면 재기동 때 그룹 CA 인증서로
**덮어쓴다**. 사이트 CA leaf 의 SAN 이 그 목록의 상위집합이면 손대지 않는다 — `collect` 가 그 목록을
뽑고 `install` 이 배치 전에 검사한다. 기본 경로는 `/opt/cims-agent/modules`, 개발 레이아웃은
`--prefix build/dist/<server>`.

**수동 등가 명령.** 스크립트가 하는 일은 아래 openssl 세 묶음과 같다. `cims-root-ca.*` 는 역할 이름이다 — 현 세대
루트의 실제 파일명은 `cims-service-ca.{crt,key}` 이고 재발급 세대부터 `cims-root-ca-g1.*` 를 쓴다.

```bash
# 루트 CA — 1.0 안정화 이후 재발급(§9 #6). 유효기간(-days)은 그때 정한다. 오프라인 매체에서 생성·보관(키는 매체를 떠나지 않는다). 전환은 §8.5 루트 교체 절차.
umask 077
openssl req -x509 -newkey rsa:4096 -sha256 -days <유효기간 일수> -nodes \
  -keyout cims-root-ca.key -out cims-root-ca.crt \
  -subj "/C=KR/O=CIMS/CN=CIMS Root CA G1" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign"

# 사이트 CA — 사이트당 1회, 루트 매체에서(방식 A. 방식 B 는 req 를 현장에서, x509 만 매체에서)
openssl req -newkey rsa:4096 -sha256 -nodes \
  -keyout cims-site-ca.key -out cims-site-ca.csr \
  -subj "/C=KR/O=CIMS/CN=CIMS Site CA <site_id>"
root_left=$(( ( $(date -d "$(openssl x509 -in cims-root-ca.crt -noout -enddate | cut -d= -f2)" +%s) - $(date +%s) ) / 86400 - 1 ))
openssl x509 -req -in cims-site-ca.csr -CA cims-root-ca.crt -CAkey cims-root-ca.key -CAcreateserial \
  -days "$root_left" -sha256 -out cims-site-ca.crt \
  -extfile <(printf 'basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n')

# 서버 leaf — 현장에서. 확장 = CA:FALSE · digitalSignature,keyEncipherment · serverAuth · SAN(<목록>,DNS:csc.cims.local | DNS:csp.cims.local)
openssl req -newkey rsa:2048 -sha256 -nodes -keyout csp.key -out csp.csr -subj "/C=KR/O=CIMS/CN=<접속 IP>"
openssl x509 -req -in csp.csr -CA cims-site-ca.crt -CAkey cims-site-ca.key -CAcreateserial -days 730 -sha256 \
  -out csp.crt -extfile <(printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:<접속 IP>,IP:127.0.0.1,DNS:<hostname>,DNS:csp.cims.local\n')
cat csp.crt cims-site-ca.crt > csp-chain.pem          # 루트는 넣지 않는다
```

### 8.4 교체 절차 — 무중단이다 (재기동 불필요)

`local_nodes` 의 `tls_cert_path`/`tls_key_path` 를 갱신하고 SIGUSR1 을 보내면 **소켓을 닫지 않고
인증서만 갈아끼운다.** 이미 맺어진 TLS 연결은 각자 SSL 객체가 옛 `SSL_CTX` 를 참조해 그대로
유지되고, **새 핸드셰이크부터** 새 인증서를 쓴다. 등록·통화가 끊기지 않는다. leaf 갱신·사이트 CA 교체·
루트 교체 모두 서버 쪽에서는 이 한 가지 동작(체인 파일 교체 + SIGUSR1)이다.

동작 구조:

| 계층 | 역할 |
|---|---|
| `SSLServerCtxReload()` (psip) | 새 ctx 를 **먼저 완성**한 뒤 전역 포인터를 뮤텍스 아래에서 교체하고 옛 ctx 의 자기 참조만 해제. 실제 소멸은 마지막 사용자가 끝난 뒤 |
| `SSLServerCtxAcquire()` (psip) | accept 경로가 전역 ctx 를 쓸 때 **참조를 획득**한다. 교체와 경합해도 dangling 을 잡지 않는 유일한 안전 경로 |
| `CSipStack::ReloadTlsServerCert()` | 위를 감싸고 성공분만 `m_clsSetup` 경로에 반영(실패 시 옛 경로 유지 → 다음 Sync 가 재시도) |
| `CspListenerManager::Sync()` | bootstrap 접속점의 인증서 경로 변경을 감지해 위를 호출 |

**bind 주소·포트가 바뀌면** 여전히 리스너 재개설이 필요하다 — 그 경우 ListenerManager 소유
접속점은 remove+add(hot rebind)로 처리되고, bootstrap 접속점은 런타임 제거가 불가하므로 재기동이
필요하다. 즉 **인증서만 바뀔 때가 무중단**이다(갱신 주기의 실제 사례).

교체가 실패하면(경로 오타·권한·키 불일치) **옛 인증서로 계속 서비스한다** — 접속점을 내리지 않고
`ERROR` 로그만 남기며, 설정값을 갱신하지 않으므로 다음 Sync 가 다시 시도한다.

⚠️ **`_reloadBootstrapTlsCertIfChanged()` 는 `Sync()` 가 `m_mutex` 를 잡은 상태로 호출된다.** 그 안에서
`CheckCertExpiry()`(같은 뮤텍스)를 부르면 메인 스레드가 자기 교착에 빠져 SIGUSR1·세션 타이머·등록
만료 sweep 이 **전부 정지한다**(실측 확인). 만료 재평가는 호출부가 `Sync()` 뒤에 수행한다.

교체는 단말을 만지기 전에 서버측만으로 검증할 수 있다. **앵커는 항상 루트다** — `-CAfile` 에 사이트 CA
만 주면 OpenSSL 은 자가서명 루트까지 이어지지 않아 실패한다(`-partial_chain` 없이). 단말도 같은 규칙이다.

```bash
openssl verify -CAfile cims-root-ca.crt -untrusted cims-site-ca.crt csp.crt   # 경로 leaf → 사이트 CA → 루트
diff <(openssl x509 -in csp.crt -noout -modulus | sha256sum) \
     <(openssl rsa  -in csp.key -noout -modulus | sha256sum)                   # 키↔인증서 짝

# 교체 후 — 체인 전송 장수 / 체인·신원 검증
openssl s_client -connect <IP>:15061 -showcerts </dev/null | grep -c "BEGIN CERTIFICATE"   # 2 (루트 직서명 기존 leaf 는 1)
openssl s_client -connect <IP>:15061 -CAfile cims-root-ca.crt \
        -verify_return_error -verify_ip <IP> -brief </dev/null
openssl s_client -connect <IP>:15061 -CAfile cims-root-ca.crt \
        -verify_return_error -verify_hostname wrong.example -brief </dev/null   # 실패해야 정상
```

마지막 대조군이 중요하다 — 통과해 버리면 신원 검사가 집행되지 않는다는 뜻이다.

`verifyServer=false` 단말은 인증서가 무엇이든 접속하므로, 서버 인증서 교체는 **기존 단말에 무영향**
이다(재기동에 따른 재등록만 발생). 따라서 서버측 전환을 먼저 끝내고 단말은 점진 전환할 수 있다.

### 8.5 운영 항목

| 항목 | 상태 |
|---|---|
| 단말 앵커 주입 + `verifyServer=true` | **적용됨** — `CimsTrustStore.CA_BUNDLE`(루트 1장) → pjsip `caBuf`, `core/net/CimsTls` → OkHttp 신뢰 관리자. 미신뢰 인증서는 등록 503 `PJSIP_TLS_ECERTVERIF`·로그인 `CertPathValidatorException` 으로 거절된다(음성 대조군 실측). `allowInsecureTls` 류 우회 스위치는 없다 — 앵커 생성 실패 시 예외이지 검증을 끄지 않는다 |
| 앵커 배포 경로 | **APK 동봉(루트만).** 신뢰의 최초 씨앗을 프로비저닝 채널(CSC 4430)로 받으면 그 채널 자체가 같은 앵커로 검증되므로 의미가 없다 — 앵커는 앱과 함께 배포하고, 그 아래 층(사이트 CA·leaf)은 서버가 체인으로 보낸다. Windows 관제조작반은 CA PEM 파일 경로(`TlsCaPemPath`)에 **루트 인증서**, cimsue-cli 는 `--ca` 에 루트 |
| 사이트 CA 발급 | 개발사 오프라인 루트, 사이트당 1회 — `service-cert.sh site-ca sign --cross <ca.crt>`(기본 = 현장 그룹 CA 의 교차 서명, `ca-cross.crt` 를 매체로 회수해 `_secrets/ca/` 에) · `site-ca issue --site`(방식 A) · `site-ca csr` + `site-ca sign <csr>`(방식 B) · `sign --cross <고객 CA.crt>`(방식 C). §8.3 (0) |
| 신규 노드 발급·배치 | 교차 인증서가 배치된 노드는 **lifecycle 엔진이 자동**(§8.6.1 — 설치 후 첫 기동에 체인 발급, `cims-svc cert` 로 즉시 실행 가능). 별도 사이트 CA 현장(방식 A/B)·엔진 없는 노드는 **`scripts/service-cert.sh`**(collect → `issue --site` → install → csp-node → verify, §8.3) 를 현장 CA 보관 서버에서 |
| CSC(4421·4430) 서버 인증서 | **적용됨** — 같은 체인 파일을 `runtime/cert` 에 배치. OAM 게이트웨이는 업스트림 TLS 를 검증하지 않으므로(`gateway.py` `_ssl_param`) 관리 경로 무영향 |
| **leaf 갱신 (2년)** | **lifecycle 엔진이 잔여 60일에 자동 재발급**(§8.6.1 — 일일 스윕, 사이트 CA 서명, 핫리로드/SIGUSR1). 사람·개발사·APK 무관. 엔진이 없는 노드만 현장에서 `issue` → `install` → SIGUSR1(§8.4) |
| **사이트 CA 교체 (침해 시에만)** | 정기 갱신은 없다 — 루트와 같은 날 만료한다. 침해 시 APK 무관: 개발사가 새 사이트 CA 를 서명 → 현장에서 leaf 전부 재발급 → §8.4. 다만 구 사이트 CA 는 만료 전까지 단말이 계속 신뢰하므로(폐기 경로 없음) 실질 회수는 루트 교체다 |
| **루트 교체 (만료·침해 시)** | **유일하게 APK 가 필요한 교체.** 다음 루트를 만료 2년 전에 오프라인 매체에서 만들어 `CA_BUNDLE` 에 병기(두 장 — pjsip 은 `caBuf` 의 PEM 을 전부 적재하고 `CimsTls` 도 전부 앵커로 등록) → 전 단말 배포 뒤 각 사이트 CA 를 새 루트로 재서명(키 동일, 인증서만) → 체인 파일 교체(§8.4) → 다음 APK 에서 구 루트 제거. 사이트 CA 키가 유지되므로 leaf 는 손대지 않는다. 첫 적용 = 1.0 안정화 이후 재발급(현 앵커 → `CIMS Root CA G1`, §8.1, §9 #6) |
| 클라이언트 인증서(상호 TLS) | 미채택. 단말 인증은 SIP Digest/AKA 가 담당([sip_access_security.md](sip_access_security.md)). 채택 시 `tls_verify_peer=true` + CA 파일 지정이 필요(psip 은 CA 미설정 시 `CertificateRequest` 를 보내지 않는다) |
| FQDN 전환 | 미결(§9 #1). 전환 시 프로비저닝 `host`·인증서 SAN·DNS 등록 세 개를 동시에 맞춰야 한다. 전환하면 IP 변경이 leaf 재발급 없이 DNS 변경으로 끝난다 |
| 폐기 | CRL/OCSP 배포 경로 없음(air-gapped). 사이트 CA 침해의 피해 범위는 그 사이트이고, 회수 수단은 루트 교체(APK)뿐이다 — 사이트 CA 가 루트와 함께 만료하므로 만료 대기는 수단이 아니다. leaf 침해는 2년 만료 또는 재발급으로 닫힌다 |
| 감시·안내 | `A-PRC-009 cert_expiring` — CSP 접속점(파일 안의 전 인증서 중 가장 이른 만료, 체인 PEM 이므로 사이트 CA 만료 포함)·CSC HTTPS·OAM·그룹 CA 는 **구현**, 30일 warning / 7일 critical, 임계 이탈 시 close. 만료된 인증서는 로드가 되므로 A-PRC-012(개설 실패)로는 잡히지 않는다 — 별 축이 필요한 이유. 자동 갱신 실패 알람·콘솔 배너·관제 앱 경고·verify 게이트는 §8.6.2 |
| 루트 키 보관 | 오프라인 매체 두 벌(원본·보관본), 키 파일 600. 온라인 노드에는 루트 **인증서**만 둔다. 현 세대 루트 키(`/home/cims/certs/`)는 매체로 이관하고 온라인 사본을 지운다(§9 #4). 재발급 세대는 처음부터 매체에서 생성한다(§9 #6) |

### 8.6 만료 방어 — leaf 자동 갱신과 만료 안내

인증서는 폐기 경로가 없어 만료가 곧 회수 시한이지만, 만료를 놓치면 단말 전체가 로그인·등록을 못 하는
장애다. 기간을 늘리는 것은 답이 아니다 — 드문 일일수록 잊힌다. **원칙: 사람이 잊어도 만료가 오지 않게
하고(1차 = 자동 갱신), 자동 갱신이 죽었을 때 사람이 반드시 알게 한다(2차 = 만료 안내). 유예는 없다.**

```
잔여 일수:   ── 갱신 임계 60일 ──── 경고 30일 ──── 위험 7일 ──── 0
             lifecycle 엔진이 재발급    A-PRC-009 warning   critical      만료 = 장애
             (사람 개입 없음)          = 자동 갱신 실패 신호  (콘솔 배너·관제 앱 경고)
```

사다리의 뜻: 자동 갱신 대상 인증서에 **30일 경고가 뜨는 것 자체가 자동 갱신 실패**다. 정상이라면 60일에
갱신되어 경고 임계에 닿지 않는다.

#### 8.6.1 leaf 자동 갱신 — 발급자는 lifecycle 엔진, 서명자는 사이트 CA

발급 주체는 이미 있다. lifecycle 엔진(`agent/lib/cert.sh`, [oam_ha.md §5.2](oam_ha.md))이 관리평면 모듈
(oam·oam-svc·csc) 기동 전에 노드 인증서를 보증한다 — 없으면 발급, SAN 이 부족하면 재발급, 운영자 상용
인증서는 손대지 않는다. 그룹 CA(`CIMS-OAM-CA`, `<oam>/runtime/_secrets/ca/`)가 서명하고 join 이 HA 피어에
같은 CA 를 복사한다. 단말 대면 자동 갱신은 이 축을 **확장**한다. 두 번째 발급 기계를 만들지 않는다 —
SAN 규칙·기동 전 보증·HA 복사·핫리로드가 전부 이 축에 있고, 같은 것을 복제하면 어긋난다.

**사이트 CA = 그룹 CA 키 + 루트 교차 인증서.** 사이트 개설 때 개발사 오프라인 루트가 그 사이트 그룹 CA 의
공개키를 **교차 서명**한다(`service-cert.sh site-ca sign --cross <ca.crt>` → `ca-cross.crt`, subject 는 그룹 CA
와 동일, 만료 = 루트 만료일, `CA:TRUE pathlen:0`). 결과 파일은 그룹 CA 옆(`_secrets/ca/ca-cross.crt`)에 두고
join 이 피어에 함께 복사한다. 이때 두 평면의 앵커는 서로 섞이지 않는다:

| 평면 | 앵커 | 체인 |
|---|---|---|
| 관리평면 (OAM↔agent·브라우저) | 자가서명 `ca.crt` — 종전 그대로 | leaf ← 그룹 CA |
| 단말 대면 (CSP TLS·CSC 4430) | 루트 (APK·Windows CA PEM) | leaf ← `ca-cross.crt` ← 루트 |

단말에는 그룹 CA 인증서가 나가지 않고(체인으로 교차 인증서만 전송), 관리평면은 루트를 모른다. 루트가 그룹
CA 키를 신뢰하는 만큼 그룹 CA 침해 = 그 사이트 서버 위조 가능인데, 이는 사이트 CA 의 정의 그대로다(§8.1
사이트 격리). 교차 인증서가 없는 노드는 종전과 같다(그룹 CA 단독 서명 — 관리평면만 유효, 단말 불신). 즉
**동작 차이는 체인 파일에 교차 인증서를 붙이느냐뿐**이라 도입 전 노드에 영향이 없다.

**엔진의 동작** (`cert.sh` — `ensure_node_cert <module>`, 기동 전 보증과 `cims-svc cert [module|all]` 이 같은 함수):

| # | 항목 | 내용 |
|---|---|---|
| E1 | 체인 조립 | `ca-cross.crt` 가 있고 그룹 CA 와 같은 공개키면 `server.crt`/`csp-chain.pem` = leaf + 교차 인증서(2장). 없으면 leaf 1장. 공개키가 다르면(그룹 CA 재생성 뒤 옛 교차 인증서 잔존) 없는 것으로 보고 경고 |
| E2 | 갱신 계기 | 갈래 = ① 없음 → 발급 · ② CIMS 발행 + 계기 → 재발급 · ③ 운영자 인증서 → 불변(SAN 부족은 경고만). ②의 계기 = SAN 부족(요구 목록 = hostname·loopback·노드 IPv4·VIP·`AgentOamUrl`·`CertSans`) / **④ 잔여 ≤ 60일** / **⑤ 교차 인증서가 있는데 체인 2번째 장이 그것이 아님**(루트 직서명 leaf·그룹 CA 단독 leaf 를 사이트 CA 체인으로 자동 전환). 발급 SAN 에는 `DNS:csc.cims.local`/`DNS:csp.cims.local` 을 더 넣되 요구 목록에는 넣지 않는다(그것만으로 재발급하지 않게) |
| E3 | 주기 실행 | agent 가 **매일 1회** 설치된 모듈마다 `cims-svc cert <module>` 을 돈다(`cims_agent.py` 스윕 스레드 — 첫 실행은 기동 5분 뒤 + hostname 해시(0~59분) 분산, 이후 24h). 기동 전 보증만으로는 2년 무재기동 노드의 만료를 못 막는다 |
| E4 | CSP 편입 | `ensure_node_cert csp` — `<csp>/runtime/cert/{csp-chain.pem,csp.key}`(§8.3 `install` 과 같은 경로·이름). 계약: `local_nodes` TLS 행의 `tls_cert_path`/`tls_key_path` 가 이 경로를 가리킨다(설치 시 `csp-node` 1회 또는 콘솔 저장, 이후 경로 불변). 발급·재발급 뒤 엔진이 CSP 에 SIGUSR1 을 보낸다(안 떠 있으면 다음 기동이 읽는다) |
| E5 | 강등 금지 | 유효한 기존 인증서가 있으면 재발급 실패 시 **기존 유지** + 갱신 실패 기록. 교차 인증서가 없는데 leaf 가 그룹 CA 밖에서 발급된 것(루트 직서명)이면 재발급 자체를 하지 않는다 — 그룹 CA 단독 leaf 로 바꾸면 단말이 끊긴다(`site_ca_missing`). 자가서명 폴백은 인증서가 아예 없을 때만 |
| E6 | 키 길이 | 그룹 CA 신규 생성 시 RSA 3072(기존 2048 CA 는 그대로 교차 서명 — 교체는 루트 세대 전환과 함께). leaf 는 RSA 2048·2년(730일)·`keyUsage` critical |
| E7 | 갱신 상태 | 판정마다 `<prefix>/run/cert/<module>.json` = `{module, ok, reason(issued|renewed|ok|site_ca_missing|renew_failed|issue_failed), days_left, cert, ts}` — agent heartbeat 가 `cert_renew` 로 싣고 OAM 이 A-PRC-009 를 파생한다(§8.6.2) |

**소비자의 동작**:

| 모듈 | 핫리로드 |
|---|---|
| csc·oam·oam-svc | `httpsrv` 가 30초마다 파일(mtime·size) 변경을 감지해 검증 후 `load_cert_chain` — 체인 2장도 그대로 전송 |
| CSP | `CspListenerManager` 가 접속점별 인증서 파일 지문(cert·key·ca 의 mtime+size)을 기억하고 SIGUSR1/Sync 때 달라졌으면 무중단 재적재 — ListenerManager 소유 접속점은 psip `ReloadTlsListenerCert`(리스너별 ctx 교체, accept 경로는 참조 획득으로 dangling 방지), bootstrap 접속점은 `ReloadTlsServerCert`(§8.4). 경로 변경과 같은 경로의 내용 교체를 모두 잡고, 실패 시 지문을 갱신하지 않아 다음 Sync 가 재시도한다. A-PRC-009 는 호출부가 Sync 뒤 재평가 |

**HA**: 사이트 CA(그룹 CA 키·교차 인증서)는 join 으로 양 노드에 있으므로 각 노드가 독립적으로 자기 leaf 를
갱신한다. 절체와 무관하다.

**수동 경로의 자리**: `service-cert.sh issue/install/csp-node`(§8.3)는 엔진이 없는 경우의 폴백이다 — 사이트
CA 키를 OAM 노드 밖에 두어야 하는 현장(§8.3 방식 A/B), agent 이전 세대 노드, 장애 시 수동 복구. 엔진이
관리하는 노드에서는 `verify` 만 쓴다.

#### 8.6.2 만료 안내 — 자동 갱신이 죽었을 때 사람이 알게

알람 축은 **`A-PRC-009 cert_expiring`** 하나다([alarm_catalog](../alarm_catalog.md) — warning 30일 / critical
7일, 접속점·모듈 단위 mo, 단계 severity 재통지, 임계 이탈 시 close). 여기에 "자동 갱신 실패" 를 더하고, 알람
목록을 보지 않는 사람에게도 닿는 표면을 겹으로 둔다.

| 층 | 상태 | 내용 |
|---|---|---|
| CSP 접속점 | 구현 | `_certEarliestDaysLeft` — 체인 파일 전 인증서 중 가장 이른 만료(교차 인증서 = 루트 만료도 포함). mo `<서버명>/csp/cert/<proto:port>` |
| CSC HTTPS | 구현 | `fm_reporter.CertExpiryProbe` 1시간 주기, mo `<서버명>/csc/cert/https` |
| OAM·OAM-SVC·그룹 CA | 구현 | 자기 HTTPS·그룹 CA 만료. 그룹 CA 행에 `ca-cross.crt` 만료를 함께 본다 |
| **엔진 자동 갱신 실패** | 구현 | 엔진의 갱신 상태 파일(E7)을 agent 가 heartbeat 원시 metric `cert_renew{module: {ok, reason, days_left, ts}}` 로 보고하고 OAM 규칙 `check=cert_renew_failed`(`service_registry`·`oam_app`)가 A-PRC-009 를 파생한다 — agent 계열은 FM push 를 쓰지 않는다([alarm_self_reporting.md](../alarm_self_reporting.md) §2). mo `<서버명>/agent/cert/<module>/renew`, `ok=false` 면 open(첫 실패 warning, 잔여 ≤ 30일 critical), 성공 보고에 close, 모듈이 보고에서 사라지면 미평가 close. 카탈로그 감지 행(AGENT, `cert/<module>/renew`) |
| **콘솔 상단 배너** | 미구현(도안 대기) | 활성 `cert_expiring` 알람이 하나라도 있으면 모든 라우트 상단에 "인증서 N일 후 만료 — 자동 갱신 실패, <mo>" 를 상시 표시. 닫기 없음, 알람 close 로만 소멸. 운영자가 알람 화면을 열지 않아도 마주친다(도안 = console_design_system 공통 셸 항목으로 추가해야 착수) |
| **관제조작반 경고** | 구현(Windows) | 관제사는 매일 앉아 있는 사람이라 폐쇄망에서 가장 확실한 채널이다. SDK 가 마지막 성공 핸드셰이크의 peer 인증서 `notAfter` 를 관측한다 — SIP TLS 는 pjsua2 `onTransportState`(`Engine::tlsPeerExpiry()`), HTTPS 는 OpenSSL 전송의 peer 인증서(`CscClient::tlsPeerExpiry()`) → C API `cimsue_engine_tls_peer_expiry`/`cimsue_csc_tls_peer_expiry`(`cimsue_tls_peer_expiry_t`) → .NET `Engine.TlsPeerExpiry`/`CscClient.TlsPeerExpiry`. 관제조작반은 둘 중 짧은 잔여가 ≤ 30일이면 관제 요약 띠([dispatch_desktop_ui.md §3.5](dispatch_desktop_ui.md))에 "서버 인증서 N일 후 만료 — 운영자에게 알리세요" 배지를 상시 표시(닫기 없음). Android 앱의 설정 화면 표시는 미구현(SDK 관측은 같은 코어) |
| 검증 게이트 | 구현 | `cims-verify` S3-HEALTH 가 단말 대면 접속점(CSC 4430·`local_nodes` enabled TLS 행)의 서빙 체인 중 가장 이른 만료 잔여가 갱신 임계 60일을 넘지 않으면 FAIL(접속 불가 접속점은 SKIP). `S3-SCN-TLS-CERT-RENEW` 가 엔진 갈래 ⑤·④·강등 금지와 CSP 지문 재적재를 시험 루트로 끝까지 돈다(§10) |

임계는 셋뿐이고 한 곳에서 정의한다: 갱신 60 / 경고 30 / 위험 7 (일). 엔진(`cert.sh`)·CSP(`CERT_EXPIRY_*`)·
CSC 프로브·verify 가 같은 값을 쓴다 — 갱신 임계가 경고 임계보다 크다는 순서가 "경고 = 실패" 의 뜻을 만든다.

#### 8.6.3 하지 않는 것

- **만료 인증서 유예 수용** — 단말이 만료된 서버 인증서를 일정 기간 받아주는 것은 검증을 끄는 것과 같다.
  잊은 관리자 문제를 보안 구멍으로 바꾸지 않는다.
- **기간 연장으로 대체** — leaf 를 5년·10년으로 늘리면 위험 창만 커지고 잊힐 확률은 그대로다(§8.1 기간 정책).
- **모듈 자체 발급** — 부트스트랩 순환(oam_ha §5.2). 발급은 언제나 엔진이다.

#### 8.6.4 기존 노드의 사이트 CA 체인 전환 절차

루트 직서명 leaf 가 배치된 노드(이미 단말이 붙어 있는 노드)를 사이트 CA 체인으로 옮기는 데 단말·정지창은
필요 없다: ① 노드의 그룹 CA 인증서 `<oam>/runtime/_secrets/ca/ca.crt` 를 매체로 루트 매체에 → `service-cert.sh
site-ca sign --cross ca.crt` → ② `ca-cross.crt` 를 같은 디렉터리에 배치(644, HA 피어는 join 이 복사) → ③ 다음
일일 스윕(또는 `cims-svc cert csc`/`cims-svc cert csp` 즉시)에서 갈래 ⑤가 CSC·CSP leaf 를 체인 2장으로
재발급하고 SIGUSR1/httpsrv 핫리로드로 무중단 반영 → ④ `service-cert.sh verify --csp-port <TLS 포트>` 전부
PASS(체인 2장·발급자=그룹 CA·사이트 CA → 루트) + 단말 무변경으로 로그인·등록 200. CSP 의 `local_nodes` TLS
행은 엔진이 쓰는 경로(`<csp>/runtime/cert/csp-chain.pem`·`csp.key`)를 가리켜야 한다 — 다른 자리를 가리키는
노드는 `csp-node` 로 한 번 옮긴다.

## 9. 남은 과제

transport 별 도달 모델([§2](#2-transport-별-도달-모델--latch-의-의미가-다르다))·서버 접속점
([§6](#6-서버-구현-상태))·단말 TLS([§7](#7-단말-구현-상태))·바인딩 구조
([registration_binding_set.md](registration_binding_set.md))·단말 선택 모델([§7.1](#71-선택-모델--단말이-고르고-서버는-가용-목록을-준다))·
서버 인증서 검증과 무중단 교체([§8](#8-인증서-운영))는 구현·실측 완료다. 남은 것은 아래다.

| # | 과제 | 성격 | 검증 |
|---|---|---|---|
| 1 | **FQDN 전환**(선택) — 프로비저닝 `host`·인증서 SAN·DNS 등록 3개 동시 정합 | 구성 | FQDN 으로 등록 성립 |
| 2 | **사내 사이트 체인 전환 + 루트 키 오프라인 이관** — §8.6.4 절차로 .45 그룹 CA 를 현 루트로 교차 서명해 `ca-cross.crt` 배치 → 스윕에서 .45/.49 의 루트 직서명 leaf 가 사이트 CA 체인으로 전환되는지 실측(단말 무변경 로그인·등록 200) → `/home/cims/certs/cims-service-ca.key` 를 매체 두 벌로 옮기고 온라인 사본 삭제. 선행 = 이 절의 코드가 든 agent·csp·oam·oam-svc·csc 라이브 배포(정지창 풀 S3 — `S3-SCN-TLS-CERT-RENEW` 포함) | 운영·배포 | 온라인 노드에 루트 키 부재 · .45 leaf 체인 2장 · A-PRC-009 `cert/<module>/renew` 미발화 |
| 3 | **(1.0 안정화 이후) 루트 CA 재발급** — 유효기간은 그때 결정. 오프라인 매체에서 `CIMS Root CA G1` 생성 → APK `CA_BUNDLE` 병기(구 루트와 두 장)·Windows CA PEM 교체 → 각 사이트 CA 를 같은 키로 새 루트 아래 재서명(만료 = 새 루트) → 노드 체인 파일 두 번째 장 교체(무중단) → 다음 APK 에서 구 루트 제거 → 구 루트 키 파기 | 운영 | 새 루트만 든 APK 로 로그인·등록 200 · 사이트 CA 만료가 새 루트에 맞춰 연장 · leaf 무변경 |
| 4 | **만료 안내 잔여 표면**(§8.6.2) — 콘솔 상단 배너(공통 셸 도안이 있어야 착수) · Android 앱 설정 화면의 서버 인증서 만료 표시(SDK 관측 `tlsPeerExpiry` 는 있음) | 코드·도안 | 갱신 실패 주입 시 알람→배너→관제 앱 경고 3단 표시 |

시험 클라이언트는 `cspsim -transport {udp,tcp,tls}` 다 — 단말 빌드 없이 서버측 전 구간을 실측할
수 있고, 한 계정을 여러 경로로 등록시켜 바인딩 집합도 만들 수 있다.

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
| 10 | **서버 인증서 검증(정상)** | 단말 로그 `CA certificates loaded from buffer (cnt=N)` + 등록 200 |
| 11 | **서버 인증서 검증(음성 대조군)** — 리스너를 CA 서명이 아닌 인증서로 교체 | 등록 **503 `PJSIP_TLS_ECERTVERIF`** + TLS 연결 미성립. 통과해 버리면 검증이 집행되지 않는다는 뜻 |
| 12 | **leaf 자동 갱신** `S3-SCN-TLS-CERT-RENEW` — 시험 루트가 dev 그룹 CA 를 교차 서명 → `cims-svc cert csp` | 갈래 ⑤ 체인 2장 전환 + 시험 루트 경로 검증 · 임시 TLS 접속점이 체인 2장 서빙 · 같은 경로에 잔여 10일 leaf 교체 + SIGUSR1 → 서빙 지문 변경(지문 재적재) · 스윕 → 2년 leaf 재발급 + 엔진 SIGUSR1 로 재변경 · 쓰기 불가 주입 → 기존 유지 + 상태 `renew_failed`. 종료 시 자기복원 |

진단 시 목적지 판정은 `Target=` 표기가 아니라 직후의 `UdpSend`/`TcpSend`/`TlsSend` NETWORK 로그를
정본으로 본다. latch 갱신 로그의 transport 값은 **수신값**이므로 저장 상태 판정에 쓸 수 없다
(0단계에서 저장값 출력을 추가한다).

## 11. 관련 파일

| 파일 | 역할 |
|---|---|
| `csp/UserMap.{h,cpp}` | latch 저장·갱신(`Insert`/`SetIpPort`), 만료 sweep, OPTIONS keepalive |
| `csp/ModuleDispatcher.cpp` | 주소 변경 감지 갱신(`EventIncomingRequestAuth`), in-dialog 목적지 제공(`EventGetLegDest`) |
| `csp/GroupCallService.cpp`, `csp/CspServer.cpp` | latch 소비 (fan-out INVITE, NOTIFY 2종) |
| `csp/CspListenerManager.{h,cpp}` | `local_nodes.jsonl` → 리스너 add/remove (UDP/TCP/TLS). TLS 인증서 파일 지문(mtime+size) 기억 → 같은 경로 내용 교체의 무중단 재적재(§8.6.1) · A-PRC-009 만료 점검 |
| `csp/CspLocalNodeMap.cpp` | primary 리스너 해석 |
| `ext/psip/SipStack/SipStack.cpp` | 리스너 생성·pool 초기화 (T1·T2) · `ReloadTlsListenerCert`(리스너별 ctx 무중단 교체 — `CSipStackTlsListener::AcquireSslCtx/SwapSslCtx` 참조 규약) · `ReloadTlsServerCert`(stack-global) |
| `ext/psip/SipStack/SipTlsThread.cpp` | TLS accept·worker (T1·T4) |
| `ext/psip/SipStack/SipTlsClientThread.cpp` | 아웃바운드 TLS 클라이언트 |
| `ext/psip/SipStack/TlsFunction.cpp` | SSL ctx 생성·accept·connect. 인증서는 체인 파일로 적재(§8.2), 무중단 교체(`SSLServerCtxReload`)와 참조 획득(`SSLServerCtxAcquire`), 클라이언트 인증서 요구는 CA 설정 시에만 |
| `ext/psip/SipStack/TcpSocketMap.cpp` | 연결 재사용 맵 (TCP·TLS 공용) |
| `ext/psip/SipStack/SipStackComm.hpp` | 송신 transport 분기, 수신 Via 각인 |
| `sdk/android/build-native.sh` | UE 엔진(`ext/pjproject`) Android 빌드 (OpenSSL 3.0.15 정적 + `PJSIP_HAS_TLS_TRANSPORT` — `sdk/engine/config_site/common.h`) |
| `android/core/.../sip/PjLib.kt` | UE transport 생성 + 서버 인증서 검증 설정 |
| `android/core/.../sip/CimsTrustStore.kt` | UE 신뢰 앵커 = **루트 CA** PEM 1장(APK 동봉). 루트 교체 세대에만 다음 루트를 병기한다 — 사이트 CA·leaf 는 여기 없다 |
| `csc/src/services/mcptt.py` | 프로비저닝 가용 transport 목록 제공(§7.1) |
| `scripts/service-cert.sh` | 단말 대면 인증서 발급·배치·검증(§8.3) — 사이트 CA(`site-ca`, 개발사 오프라인 루트)·서버 leaf(`issue`, 현장 사이트 CA)·`verify`(루트 앵커). 묶음에 자기 복사 — 현장 단독 실행 |
| `<oam>/runtime/_secrets/ca/` | 사이트 CA(기본) — 그룹 CA `ca.{crt,key}`(관리평면 앵커) + 루트 교차 인증서 `ca-cross.crt`(단말 대면 체인). join 이 피어에 복사, 디렉터리 700·키 600 |
| `agent/lib/cert.sh` | lifecycle 엔진의 노드 인증서 발급·갱신(그룹 CA = 사이트 CA 교차 인증서 체인, 잔여 60일 자동 재발급, CSP 편입, 강등 금지, 갱신 상태 `run/cert/<module>.json` — §8.6.1). 임계 3값(60/30/7)의 정의 자리. SAN 요구 목록의 정본 — `service-cert.sh` 가 상위집합 조건으로 검사. 진입 = `cims-svc cert [module|all]`(`cims.sh cert` 위임) |
| `agent/cims_agent.py` | 일일 스윕 스레드(모듈별 `cims-svc cert`, hostname 해시 분산) · heartbeat metric `cert_renew` |
| `ems/core/oam/src/services/service_registry.py` · `oam_app.py` | agent 규칙 `check=cert_renew_failed` → A-PRC-009 `<서버명>/agent/cert/<module>/renew` 파생 · `handlers/agent_api.py` metric 화이트리스트 `cert_renew` · `handlers/oam_join.py` join 번들에 `ca-cross.crt`(`deployment/bootstrap/install.sh` 가 전개) |
| `verify/lib/items/stage3/health.py` · `scn_tls_cert_renew.py` | S3-HEALTH 단말 대면 인증서 잔여 > 60일 게이트 · `S3-SCN-TLS-CERT-RENEW`(§10 #12) |
| `sdk/core/…` `Engine::tlsPeerExpiry` · `CscClient::tlsPeerExpiry` · `cimsue_c.h` `cimsue_*_tls_peer_expiry` · `sdk/windows/dotnet` `TlsPeerExpiry` · `windows/dispatch-desktop` 요약 띠 | 서버 인증서 만료 관측·관제조작반 경고(§8.6.2) |
| `/home/cims/certs/` | 사내 CA 보관 서버 — 루트 `cims-service-ca.{crt,key}`(키는 오프라인 매체로 이관 대상, §9 #4) · 사이트 CA `<site_id>/cims-site-ca.{crt,key}` · 노드 묶음 `cert-init-<host>/` (키 권한 600) |
