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
> 동거, 그룹콜 성립). 인증서는 **사설 CA 기반**이며 단말이 서버 인증서를 **검증한다**
> ([§8](#8-인증서-운영)) — 신뢰하지 않는 인증서는 등록이 거절된다(`PJSIP_TLS_ECERTVERIF`).
> 남은 것은 단말 대면 나머지 평면(CSC 4430)의 검증 활성과 인증서 갱신·감시 운영이다([§8.5](#85-운영-잔여-항목)).

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
| 서버 인증서 검증 | `TlsConfig.verifyServer = true` + `caBuf = CimsTrustStore.CA_BUNDLE`(APK 동봉 사설 CA). 검사 자체는 플래그와 무관하게 항상 수행돼 `verify_status` 에 기록되고, 이 플래그가 **실패 시 연결을 끊을지**를 결정한다(`sip_transport_tls.c` 의 `verify_status && verify_server`). 실패 시 transport shutdown → 등록 503 `PJSIP_TLS_ECERTVERIF`. ⚠ `caListFile`/`certFile`/`privKeyFile` 이 설정되면 `caBuf` 가 무시된다 |
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
  "domain": "ptt.mnc033.mcc450.3gppnetwork.org",
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
| `{volte,ptt}_subscriptions.sip_transport` | **가입자 기본값**(NULL=서비스 기본값). 강제가 아니라 권장값이며, 단말이 바꿀 수 있다 |

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

### 8.1 PKI 구조 — 단말 대면 전용 사설 CA

```
CIMS Service CA  (자가서명, RSA 4096, 10년, 단말 대면 전용)
   └─ CSP 서버 인증서  (RSA 2048, 2년, EKU=serverAuth, CA:FALSE)
        SAN = IP:<CSP 접속 주소>, IP:<관리망 주소>, DNS:csp.cims.local
```

**중간 CA 를 두지 않는 1단 구조**다. X.509·TLS 규격은 중간 CA 를 요구하지 않으며(중간 필수는
공인 CA 대상 CA/Browser Forum 정책), 사설망 규모에서 루트가 leaf 를 직접 서명하는 것은 정상 구성이다.
대가로 루트 개인키를 발급 작업에 쓰게 되므로 키 파일 권한을 `600` 으로 제한한다.

관리평면(OAM↔agent mTLS)의 `CIMS Agent CA` 와는 **별도 CA** 다. 관리평면 신뢰 앵커를 사용자 기기로
내보내지 않기 위한 격리이며, **단말 대면 인증서는 이 Service CA 로 묶는다** — CSP 15061 과
CSC(4421 관리·4430 단말, 인증서 한 장 공용)가 모두 여기서 발급된다. 단말에 심는 앵커가 하나로 유지된다.

CSC 인증서는 **버전무관 `<install>/../runtime/cert/server.{crt,key}`** 에 둔다. csc 는 이 경로를
버전 디렉터리보다 먼저 찾으므로(`csc_app.py`) 모듈 업그레이드에도 살아남는다 — 버전 디렉터리
(`current/csc/cert/`)에만 두면 업그레이드가 패키지 자가서명 인증서로 되돌려 놓는다.

> agent 에는 노드 인증서 자동 발급 체계가 있다(`agent/lib/cert.sh` — 그룹 CA 서명, SAN 추종 재발급,
> 기동 전 `ensure_node_cert` 보증). 배포된 agent 0.2.80 에는 아직 이 파일이 없어 **잠들어 있다.**
> 나중에 agent 를 올렸을 때 재발급으로 덮이지 않도록, CSC 인증서 SAN 은 그 체계가 요구하는 목록
> (`DNS:<hostname>`, `IP:127.0.0.1`, 노드 IPv4 전부, VIP)의 **상위집합**으로 발급해 두었다.
> 장기적으로는 그룹 CA 를 Service CA 로 통일해 발급·갱신을 이 체계에 맡기는 것이 옳다.

### 8.2 서버는 체인을 전송한다

psip 은 `SSL_CTX_use_certificate_chain_file()` 로 인증서를 적재한다 — PEM 의 첫 인증서를 서버
인증서로, 뒤에 이어붙인 인증서를 중간 CA 체인으로 등록해 핸드셰이크 `Certificate` 목록에 함께
싣는다. 인증서 1장뿐인 PEM 에서는 동작이 동일하다.

`tls_cert_path` 는 leaf + CA 를 이어붙인 체인 PEM 을 가리키고, 키는 `tls_key_path` 로 분리한다.
중간 CA 를 도입하거나 고객사 PKI 에서 인증서를 받아오면 그 중간 인증서를 이 파일에 추가하면 된다.

### 8.3 발급 절차

```bash
umask 077
# 1) 사설 CA (10년)
openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
  -keyout cims-service-ca.key -out cims-service-ca.crt \
  -subj "/C=KR/O=CIMS/CN=CIMS Service CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign"

# 2) 서버 키 + CSR
openssl req -newkey rsa:2048 -sha256 -nodes -keyout csp.key -out csp.csr \
  -subj "/C=KR/O=CIMS/CN=<CSP 접속 주소>"

# 3) SAN·용도 확장 — HA 를 쓰면 VIP 주소를 반드시 포함한다
cat > csp-ext.cnf <<'EXT'
basicConstraints = critical, CA:FALSE
keyUsage         = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName   = IP:<CSP 접속 주소>, IP:<관리망 주소>, DNS:csp.cims.local
EXT

# 4) CA 서명 (2년) → 5) 체인 PEM 조립
openssl x509 -req -in csp.csr -CA cims-service-ca.crt -CAkey cims-service-ca.key \
  -CAcreateserial -out csp.crt -days 730 -sha256 -extfile csp-ext.cnf
cat csp.crt cims-service-ca.crt > csp-chain.pem
chmod 600 cims-service-ca.key csp.key
```

### 8.4 교체 절차 — 리스너 재개설이 필요하다

⚠️ **인증서·키 경로는 기동 캡처 항목이다.** `local_nodes` 를 갱신하고 SIGUSR1 을 보내면 새 경로는
읽히지만, bind 주소·포트가 그대로면 `ListenerManager` 가 `skip … bootstrap 이 이미 바인딩` 으로
리스너를 다시 열지 않아 **옛 SSL 컨텍스트가 유지된다.** 반영에는 CSP 재기동이 필요하다.

교체는 단말을 만지기 전에 서버측만으로 검증할 수 있다.

```bash
openssl verify -CAfile cims-service-ca.crt csp.crt          # 체인
diff <(openssl x509 -in csp.crt -noout -modulus | sha256sum) \
     <(openssl rsa  -in csp.key -noout -modulus | sha256sum)  # 키↔인증서 짝

# 교체·재기동 후 — 체인 전송 장수 / 체인·신원 검증
openssl s_client -connect <IP>:15061 -showcerts </dev/null | grep -c "BEGIN CERTIFICATE"
openssl s_client -connect <IP>:15061 -CAfile cims-service-ca.crt \
        -verify_return_error -verify_ip <IP> -brief </dev/null
openssl s_client -connect <IP>:15061 -CAfile cims-service-ca.crt \
        -verify_return_error -verify_hostname wrong.example -brief </dev/null   # 실패해야 정상
```

마지막 대조군이 중요하다 — 통과해 버리면 신원 검사가 집행되지 않는다는 뜻이다.

`verifyServer=false` 단말은 인증서가 무엇이든 접속하므로, 서버 인증서 교체는 **기존 단말에 무영향**
이다(재기동에 따른 재등록만 발생). 따라서 서버측 전환을 먼저 끝내고 단말은 점진 전환할 수 있다.

### 8.5 운영 잔여 항목

| 항목 | 상태 |
|---|---|
| 단말 CA 주입 + `verifyServer=true` | **적용됨**(`CimsTrustStore.CA_BUNDLE` → `caBuf`). 음성 대조군까지 실측 — 미신뢰 인증서는 503 `PJSIP_TLS_ECERTVERIF` 로 거절된다 |
| CA 배포 경로 | **APK 동봉.** 프로비저닝(CSC 4430)은 자신도 자가서명 + `allowInsecureTls=true` 라 신뢰의 최초 씨앗을 그 채널로 받으면 의미가 반감된다 |
| CSC(4421·4430) 서버 인증서 | **적용됨** — Service CA 발급, `runtime/cert` 배치. openssl(체인·IP 신원·틀린 이름 대조) + 검증을 켠 클라이언트로 로그인→토큰→프로비저닝 전 구간 실측. OAM 게이트웨이는 업스트림 TLS 를 검증하지 않으므로(`gateway.py` `_ssl_param`) 관리 경로 무영향 |
| CSC 검증 — **앱측** | **미적용** — 앱이 `allowInsecureTls=true`(core 5곳 + ptt-client 1곳). `insecure()` 를 `CimsTrustStore.CA_BUNDLE` 기반 TrustManager 로 교체하면 **앵커 추가 없이** 끝난다. 이 채널로 로그인 비밀번호와 SIP 접속 정보가 오가므로 우선순위가 높다 |
| CA 교체(무중단) | `CA_BUNDLE` 에 신규 CA 를 추가한 APK 선배포 → 서버 인증서 교체 → 다음 배포에서 구 CA 제거 |
| 클라이언트 인증서(상호 TLS) | 미채택. 단말 인증은 SIP Digest 가 담당. 채택 시 `tls_verify_peer=true` + CA 파일 지정이 필요(psip 은 CA 미설정 시 `CertificateRequest` 를 보내지 않는다) |
| FQDN 전환 | 미결. 전환 시 프로비저닝 `host`·인증서 SAN·DNS 등록 세 개를 동시에 맞춰야 한다 |
| 갱신 | leaf 2년. 무중단 교체는 재기동이 필요하다(§8.4) |
| 폐기 | CRL/OCSP 배포 경로 없음(air-gapped) → **짧은 유효기간 + 정기 교체**로 대체 |
| 감시 | 만료 임박 알람 미구현 — 인증서 만료는 전 단말 동시 등록 불가의 단일 장애점. `A-PRC-009 cert_expiring` 패턴 확장 대상 |

## 9. 남은 과제

transport 별 도달 모델([§2](#2-transport-별-도달-모델--latch-의-의미가-다르다))·서버 접속점
([§6](#6-서버-구현-상태))·단말 TLS([§7](#7-단말-구현-상태))·바인딩 구조
([registration_binding_set.md](registration_binding_set.md))는 구현·실측 완료다.

단말 선택 모델([§7.1](#71-선택-모델--단말이-고르고-서버는-가용-목록을-준다))과 서버 인증서 검증
([§8](#8-인증서-운영))도 구현·실측 완료다. 남은 것은 운영 항목이다.

| # | 과제 | 성격 | 검증 |
|---|---|---|---|
| 1 | **CSC 검증 — 앱측** — 서버 인증서는 Service CA 로 전환 완료. 남은 것은 앱의 `insecure()` 를 CA 신뢰로 교체하는 것 | 보안 | 프로비저닝·MCPTT API 가 검증 하에 동작 + 미신뢰 인증서 거절(대조군) |
| 2 | **인증서 갱신·만료 감시**([§8.5](#85-운영-잔여-항목)) — leaf 2년, 교체에 CSP 재기동 필요, 만료 알람 미구현 | 운영 | 만료 임박 알람 발생 + 무중단 교체 |
| 3 | **FQDN 전환**(선택) — 프로비저닝 `host`·인증서 SAN·DNS 등록 3개 동시 정합 | 구성 | FQDN 으로 등록 성립 |

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
| `ext/psip/SipStack/TlsFunction.cpp` | SSL ctx 생성·accept·connect. 인증서는 체인 파일로 적재(§8.2), 클라이언트 인증서 요구는 CA 설정 시에만 |
| `ext/psip/SipStack/TcpSocketMap.cpp` | 연결 재사용 맵 (TCP·TLS 공용) |
| `ext/psip/SipStack/SipStackComm.hpp` | 송신 transport 분기, 수신 Via 각인 |
| `android/docs/scripts/m1_build_pjsip.sh` | UE pjproject 빌드 (OpenSSL 3.0.15 정적 + `PJSIP_HAS_TLS_TRANSPORT`) |
| `android/core/.../sip/PjLib.kt` | UE transport 생성 + 서버 인증서 검증 설정 |
| `android/core/.../sip/CimsTrustStore.kt` | UE 신뢰 앵커(APK 동봉 사설 CA PEM). CA 교체 시 여기에 추가 |
| `csc/src/services/mcptt.py` | 프로비저닝 가용 transport 목록 제공(§7.1) |
| `/home/cims/certs/` | `cims-service-ca.{crt,key}` · `csp.{crt,key}` · `csp-chain.pem` (키 권한 600) |
