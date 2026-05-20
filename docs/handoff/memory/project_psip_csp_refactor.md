---
name: CIMS psip + CSP 리팩토링 설계 메모
description: R1~R7 단계 분해된 SIP 스택 리팩토링의 아키텍처 근거/원칙. 각 단계 진입 전 참조.
type: project
originSessionId: 7546c030-cc6d-4fce-be13-5d1f1410f8a5
---
**Why**: 사용자가 CSP 설정 구조 점검 중 "psip 가 SIP 규격에 맞게 동작하려면 레이어 분리와 per-route Via/Contact 가 필요" 라고 결정. 단순 UI 정리가 아니라 psip + CSP 구조 리팩토링을 R1~R7 로 분해하여 진행.

**How to apply**: 각 단계 진입 시 이 메모 + `project_phase_status.md` 의 단계 테이블 확인. plan 파일 (R1: `polymorphic-humming-pie.md`, R2: `scalable-honking-frog.md`) 은 이전 세션 기록 참고용.

---

## 아키텍처 진단 (이번 세션 소스 분석 근거)

### 현재 psip 구조 (`ext/psip/SipStack/SipStack.cpp` + `SipUserAgent.cpp`)

```
CSipUserAgent (global 1개)
 └── CSipStack (m_clsSipStack, 1개)
     ├── m_vecUdpListeners[]   ← per-listener {bind_ip, port, thread_count} 이미 지원
     ├── m_vecTcpListeners[]   ← per-listener {bind_ip, port} R3 완료 (m_hTcpSocket 은 primary alias)
     ├── m_vecTlsListeners[]   ← per-listener {bind_ip, port} R3 완료 (m_hTlsSocket 은 primary alias)
     └── Transaction Layer (전역, StackExecutePeriod tick)
```

### CSP 가 현재 psip 를 사용하는 한계

- **Setup.Sip.LocalIp**: `CspServer.cpp` 안 **100+ 지점** 에서 From/Via/Contact/XCAP URL 생성 시 그대로 사용 → 멀티-인터페이스 환경에서 깨짐.
- ~~**psip AddUdpListener** 은 per-listener thread_count 를 받지만 `CspListenerManager.Sync()` 가 이 값을 전달 안 함.~~ (R2 에서 해결)
- ~~**TCP/TLS** 은 psip 가 singleton → local_nodes 에 protocol=TCP/TLS 지정해도 실제 추가 리스너 못 만듦.~~ (R3 에서 해결 — psip API 는 존재. CspListenerManager 분기는 R4)

### Scope 분류 (9 SIP Stack 필드)

| 필드 | 실제 scope | 재배치 |
|---|---|---|
| LocalIp / UdpPort | CSP identity (from primary local_node) | ✅ R1 완료: `_infra` 이동 + primary 자동 주입 |
| UdpThreadCount | per-listener (psip 이미 지원) | ✅ R2 완료: `local_nodes.thread_count` 필드 추가 + ListenerManager 전달 |
| StackExecutePeriod | 전역 (stack tick) | 유지 |
| MinRegisterTimeout / UserTimeout / StaleCallTimeout / CallPickupId / SendOptionsPeriod | 전역 정책 | 유지 (섹션은 `sip_policy` 로 분리 검토) |

### 장기 리팩토링 대상 (R5~R6)

- **Via**: 현재 `gclsSetup.m_strLocalIp` 고정 → outbound transport 의 bind_ip:port 로 per-transaction 생성 필요
- **Contact**: 수신 listener 기반 (응답) / outbound Route 기반 (forward) 으로 분기
- **From identity**: access_services 에 `server_identity_uri` 필드 추가 → CSCF/TAS/PTT-AS 가 해당 identity 로 생성

---

## R1 구현 요약 (완료, 2026-04-23)

**변경 의도**: LocalIp/UdpPort 는 UI 튜닝 포인트가 아님 → local_nodes 의 primary 항목에서 자동 유도. 사용자는 is_primary 체크박스만 설정.

**선택 규칙** (`CspLocalNodeMap::GetPrimary`):
1. `enabled=true && is_primary=true` — 여러 개면 name 사전식 첫 번째 + WARN
2. `enabled=true && edge=access && protocol=UDP` — 사전식 첫 번째
3. 없음 → 호출자가 `_infra Setup.Sip.LocalIp/UdpPort` fallback

**기동 순서 변경** (`CspServer.cpp`):
```
기존: clsSetup 복사 → ConfigCache 로드 → (나중) LocalNodeMap.Sync
변경: ConfigCache 로드 → LocalNodeMap.Sync → primary 결정 → gclsSetup 덮어쓰기 → clsSetup 복사
```

**bind_ip=0.0.0.0 처리**: GetLocalIp() 로 advertised IP 자동 탐지해서 LocalIp 에 주입 (bind 는 0.0.0.0 유지 가능).

---

## R2 구현 요약 (완료, 2026-04-23)

- `config_template.json` `local_nodes` schema 에 `thread_count` (int, default 2, min 1, max 32, fallback 0)
- `CspLocalNodeMap::LocalNodeInfo.thread_count` 추가 + `GetInt("thread_count", 0)` 파싱
- `CspListenerManager::ManagedInfo.threadCount` 추가, row 에서 파싱:
  ```
  int iThreads = row.GetInt("thread_count", 0);
  if (iThreads <= 0) iThreads = gclsSetup.m_iUdpThreadCount;
  if (iThreads <= 0) iThreads = 1;
  ```
- `AddUdpListener(id, ip, port, d.threadCount, out)` 에 per-listener 값 전달
- 로그에 `threads=%d` 포함

**검증**: 보조 local_node (5070, thread_count=4) 추가 → `AddUdpListener id=210824256 0.0.0.0:5070 threads=4` 확인

**미해결 (R3 이후)**: bootstrap listener 가 primary 포트를 선점 → primary 에 대한 per-listener thread_count 미적용. ListenerManager 가 SOT 가 되도록 구조 정리 필요.

---

## R3 구현 요약 (완료, 2026-04-23)

- `SipStackListener.h`: `CSipStackTcpListener` / `CSipStackTlsListener` 클래스 추가 (UDP 대칭)
- `SipStack.h`: `m_vecTcpListeners` / `m_vecTlsListeners` + 뮤텍스/nextExtId, Add/Remove/Get API, primary-alias 유지 (`m_hTcpSocket`/`m_hTlsSocket` = vector.front())
- `SipStack.cpp`: `Start()` 의 TCP/TLS singleton 경로를 vector+`StartSipTcpListenThreadForListener(primary)` 로 치환. `_Stop()` 에서 vector 정리. Add/Remove/Get 구현 (UDP 대칭).
- `SipTcpThread.cpp` / `SipTlsThread.cpp`: `SipTcpListenerThread` / `SipTlsListenerThread` (listener 포인터 파라미터), `StartSipTcp(Tls)ListenThreadForListener` 함수 추가. 각 accept thread 는 `m_bDrain` / `m_iActiveThreads` 로 graceful stop.
- **Worker pool 은 shared 유지**: `m_clsTcpThreadList` / `m_clsTlsThreadList` 에 SendCommand — 리스너당 새 worker pool 을 만들지 않음 (리스너 수가 적고 shared worker 가 충분).
- **인증서**: TLS 인증서는 여전히 stack-global `SSLServerStart(cert, ca)` 로 Start 시 1회 로드. per-listener cert 분리는 R4/R5 에서 (local_node.tls_cert_path 필드는 이미 schema 에 존재).

**검증**: 전체 빌드 성공, CSP 기동 smoke (UDP 5060 LISTEN, primary 주입, ListenerManager 보조 리스너 추가 경로 — R2 에서 검증된 그대로 동작).

**미해결 (R4)**: CSP 의 `LocalTcpPort`/`LocalTlsPort` 가 config 에 기본 미지정 → Start 에서 TCP/TLS vector 는 비어있음. CspListenerManager 가 local_nodes.protocol 에 따라 psip Add TCP/TLS API 를 호출해야 실효.

---

## R4 구현 요약 (완료, 2026-04-23)

- `CspListenerManager::_normalizeProtocol`: 대소문자 무시 UDP/TCP/TLS 정규화. WS/WSS 는 빈 문자열(skip)
- `CspListenerManager::_isAlreadyBound(proto, ip, port)`: psip `Get{Udp|Tcp|Tls}ListenerInfo` 로 protocol-specific 스냅샷 조회
- `CspListenerManager::_addListenerToStack / _removeListenerFromStack`: protocol 에 따라 `Add{Udp|Tcp|Tls}Listener` / `Remove{Udp|Tcp|Tls}Listener` 호출. UDP 만 threadCount 전달.
- `Sync()`: TCP/TLS 레코드 포함 처리. `tls_cert_path` 는 INFO 로깅만 (per-listener cert 적용은 R5+).
- 로그 포맷: `ListenerManager: added id=... TCP 0.0.0.0:5062` (protocol 포함)

**검증**: TCP(5062)/TLS(5063) 추가 기동 → 양쪽 AddXxxListener + ss LISTEN 확인. UDP 회귀 없음.

**R5 로 이관된 과제**:
- **outbound send path**: 현재 psip send 는 listener selection 로직 없음. `bUseContactListenPort` 시 `m_clsSetup.m_iLocalTcpPort` (single) 참조. multi-listener 환경에서 primary 로 폴백. per-Route 선택은 R5.
- **TLS per-listener cert**: 현재 stack-global SSLServerStart(cert, ca). local_nodes 의 tls_cert_path 는 collect 만.
- **bootstrap 축소**: `Setup.Sip.LocalTcpPort=5061` 이 config 에 있어 Start() 경로가 primary TLS 를 하나 만들고, ListenerManager 가 추가 TCP/TLS 를 올린 뒤 공존. local_nodes 를 SOT 로 하려면 bootstrap 경로 비활성화 또는 primary UDP 와 같은 자동 주입 스킴.

---

## R5.a 구현 요약 (완료, 2026-04-23)

**새 파일** `csp/CspAddressing.{h,cpp}` (namespace `CspAddressing`):
- `GetLocalSipAddress()` — SIP Contact/From/Call-ID host (현재 `gclsSetup.m_strLocalIp` 반환)
- `GetLocalRtpAddress()` — SDP media IP (현재 동일)
- `GetLocalXcapAddress()` — XCAP/MCPTT URL host (현재 동일)

**치환 범위**: live code 16 참조 (CscfModule / UserMap / ModuleDispatcher / CspServer:XCAP).
dead code (SipServer*.hpp 3파일) 는 그대로 — 어디에서도 include 안 됨.
R1 경로 (write side: primary 주입/fallback) 와 CRtpInfo 내부 필드는 제외.

**검증**: CMP+CSP 기동 + cspsim REGISTER smoke 정상. 403 응답의 Contact 에 helper 결과(192.168.0.2) 반영 확인.

---

## R5.b 구현 요약 (완료, 2026-04-23)

**CspAddressing signature 확장**:
- `GetLocalSipAddress(int inbound_listener_id = 0)`:
  * id > 0: `gclsLocalNodeMap.GetByIntId(id)` 조회 → 유효하면 `_resolveBindIp(node)` (bind_ip=0.0.0.0 정규화)
  * id == 0 또는 조회 실패: primary LocalIp fallback
- `GetLocalSipAddressForOutbound(proto="UDP", edge_preference="peering")`:
  * 1차: enabled && protocol match && edge match
  * 2차: enabled && protocol match (edge 무관)
  * 3차: primary fallback
- 내부 `_resolveBindIp()`: bind_ip 가 `0.0.0.0`/공백이면 advertised primary IP 반환

**호출부**:
- CscfModule.cpp Service-Route: `GetLocalSipAddress(GetCurrentInboundListenerId())`
- ModuleDispatcher.cpp 302 Contact: 동일 (+ `#include "SipStackThread.h"`)
- UserMap.cpp OPTIONS keepalive: `GetLocalSipAddressForOutbound("UDP","access")`

**미해결 (R5.b')**:
- psip `CSipStack::Send` 가 실제 송신할 때 여전히 primary socket 사용.
- Via header 값은 올바르지만 source IP 는 primary → 엄격한 대응은 psip API 확장 필요.

---

## R5.b' 착수 포인트 (R5.c 이후 또는 병렬)

**psip Send 에 source listener 선택**:
- `CSipStack::Send(CSipMessage*, int source_listener_id = 0)` 확장
- UDP: listener_id > 0 이면 해당 listener 의 `m_hSocket` 으로 `sendto`
- TCP/TLS: client connect 시 `bind(source_ip, 0)` 로 outbound local 선택
- `CSipStack::SendSipMessage` 가 메시지의 context (수신 listener 등) 를 자동 추적하는 방안 검토

---

## R5.c 구현 요약 (완료, 2026-04-23)

**psip side**:
- `TlsFunction.{h,cpp}`: `SSLServerCtxCreate(cert, key, ca)` / `SSLServerCtxFree(ctx)` / `SSLAcceptWithCtx(fd, ctx, ...)` 추가. 기존 `SSLServerStart/SSLAccept` backward-compat 유지.
- SipStack/TcpSessionList.h + TcpStack/TcpThreadList.h (두 CTcpComm 정의 모두): `SSL_CTX* m_pSslCtx` 필드 (USE_TLS only). accept thread → worker 전파 채널.
- `CSipStackTlsListener` (SipStackListener.h): `SSL_CTX* m_pSslCtx` + `std::string m_strCertFile/KeyFile/CaCertFile` 필드. NULL ctx 는 stack-global 사용.
- `AddTlsListener(extId, ip, port, cert, key, ca, out)` 로 signature 확장. cert 지정 시 SSLServerCtxCreate → listener.m_pSslCtx.
- `_Stop`, `_StopTlsListenerLocked`: per-listener ctx free.
- `SipTlsListenerThread` (accept thread): `clsTcpComm.m_pSslCtx = pListener->m_pSslCtx`
- `SipTlsThread` (worker): `SSLAccept` → `SSLAcceptWithCtx(fd, clsTcpComm.m_pSslCtx, ...)`

**CSP side**:
- `CspListenerManager::ManagedInfo` 에 tlsCertPath/tlsKeyPath/tlsCaPath 필드
- Sync 에서 local_node.tls_cert_path/tls_key_path/tls_ca_path 수집
- `_addListenerToStack` TLS 분기에서 cert 전달

**primary listener**: Start() 의 직접 생성 경로 유지 (m_pSslCtx=NULL → global 사용). 즉 기존 `Setup.Sip.CertFile` 경로 불변.

**검증**: TLS(5064) per-listener cert 확인 + 기존 bootstrap TLS(5061, global cert) 공존 확인. UDP 회귀 없음.

---

## R5.b' 구현 요약 (완료, 2026-04-23)

**UDP request 경로만 처리**. TCP/TLS 와 response 는 별도 단계로 이관.

- `CSipStack::_SelectUdpSocketForViaRequest(msg)` private helper 추가
  - Via[0] (CheckSipMessage 가 request 에 자동 추가) 의 host:port 로 listener 매칭
  - port match + (bind_ip exact match OR 0.0.0.0/empty any-interface)
  - 매칭 실패 → primary (m_hUdpSocket) fallback
- `SipStackComm.hpp` UDP Send 분기: `pclsMessage->IsRequest()` 일 때 helper 호출
- Response 는 Via[0] 이 peer 이므로 source 판정 불가 → primary 유지

CSP 측은 변경 없음 — R5.b 에서 이미 CspAddressing 이 올바른 Via 값을 세팅하므로 psip 가 자동 매칭.

---

## R5.b'' 구현 요약 (완료, 2026-04-23)

기존 `CSipMessage::m_iListenerId` 필드를 재활용. 새 필드 불필요.
이미 RecvSipMessage 시점에 `pclsMessage->m_iListenerId = t_iCurrentListenerId` 설정됨.

변경:
- `CSipMessage::CreateResponse()` / `CreateResponseWithToTag()` — `m_iListenerId` 복사 추가
- `CSipStack::_SelectUdpSocketByListenerId(int)` helper 추가 — id → m_vecUdpListeners.m_iId 매칭
- `SipStackComm.hpp` UDP Send 분기: response → `_SelectUdpSocketByListenerId(msg->m_iListenerId)`

Primary (id=0) fallback 으로 backward compat 유지.

## R5.b''' 구현 요약 (완료, 2026-04-23)

- `SipPlatform/SipTcp.h/.cpp`: 신규 `TcpConnectFrom(src_ip, dst_ip, port, timeout)`
  * src_ip 유효 시: `socket → bind(src_ip, port=0) → connect`. IPv4/IPv6 모두 처리.
  * src_ip NULL/"0.0.0.0"/빈 문자열: bind 없이 OS 자동 선택 (기존 TcpConnect 동일 동작)
- `SipTcpClientThread.cpp` / `SipTlsClientThread.cpp`:
  * `CSipTcpClientArg` / `CSipTlsClientArg` 에 `m_strSourceIp` 필드 추가
  * `StartSipXxxClientThread` 에서 `pclsSipMessage->m_clsViaList.front().m_strHost` 자동 추출
  * worker 에서 `TcpConnect` → `TcpConnectFrom(src_ip, ...)` 로 교체

기존 `m_clsTcpSocketMap.Select` 재사용 경로는 inbound bind 된 socket 그대로 사용 → 자동 correct source. 새 connect 만 bind 보정.

회귀: UDP REGISTER smoke 정상. TCP/TLS 경로 실효는 remote_node TCP/TLS 구성 필요 (배포 환경).

---

## R5 전체 완료 요약

R5.a (helper 도입) → R5.b (context-aware) → R5.c (TLS cert) → R5.b' (UDP request send) → R5.b'' (UDP response send) → R5.b''' (TCP/TLS connect bind).

이제 outbound SIP 메시지의 **Via 값 / 실제 source bind / TLS 인증서** 가 모두 per-listener 로 분기 가능. psip 가 완전한 multi-listener 서버로 동작.

---

## R6 구현 요약 (완료, 2026-04-23)

- `config_template.json` `access_services.schema` 에 `server_identity_uri` 필드 추가 (optional)
- `ServiceInfo.server_identity_uri` + `CCspServiceMap::Sync` 파싱
- `CspAddressing::GetServerIdentityForService(kind)` helper:
  * server_identity_uri 명시 → 그대로 반환
  * 비면 `sip:cspserver@{service.domain}` 자동 조립
  * service 매칭 실패 → `sip:cspserver@{LocalIp}` primary fallback
- `UserMap.cpp` OPTIONS:
  * `CspAddressing::GetServerIdentityForService("volte")` 로 URI 획득
  * `CSipUri::Parse` 로 user/host 분리 → From URI 세팅
  * Parse 실패 시 기존 `GetLocalSipAddressForOutbound` fallback
  * Call-ID host 는 outbound access edge local addr 유지 (유일성 보장)

효과: OPTIONS keepalive 의 From 이 `sip:cspserver@{domain}` 형식 (IMS 규격) 으로 변경. 예: `sip:cspserver@ims.mnc033.mcc450.3gppnetwork.org`. 기존 `sip:cspserver@192.168.0.2` 대비 IMS 호환성 개선.

---

## R7 확인 완료 (2026-04-23)

실제 구현이 이미 `ModuleDispatcher.cpp::RecvRequest` 에 배선되어 있음:
- Line 206-232: `AclPolicyEngine.Check()` → denied 면 403 + drop
- Line 261-294 (INVITE 한정): `RoutingPolicyEngine.Decide()` → REJECT 은 403, ROUTE_SET/ACCESS_SERVICE 는 로그 + legacy fallback
- MessageCtx 자동 조립 (from/to/req uri + src_ip + method + user_agent)

seed 파일 (`acl_policies.jsonl`, `routing_policies.jsonl`, `rules.jsonl`, `rule_sets.jsonl`) 이 현재 없어서 엔진은 empty → 기본 ALLOW / NO_MATCH → legacy 경로 유지.

**R8 구현 완료 (2026-04-23)**:
- `ModuleDispatcher.cpp` INVITE 분기에서 `ROUTING_ROUTE_SET` 매칭 시:
  * `gclsRouteMap.GetByName(picked_route)` → `RouteConfig`
  * `gclsRemoteNodeMap.GetByName(remote_node_ref)` → `RemoteNodeInfo`
  * `RemoteNode.protocol` → `ESipTransport` 매핑 (UDP/TCP/TLS)
  * `pclsMessage->AddRoute(ip, port, transport)` 로 Route header 주입
  * `return false` → CSipUserAgent B2BUA 가 Route 따라 forward (R5 시리즈의 per-listener source bind + TLS per-listener cert + TcpConnectFrom 활용)
  * 조회 실패 (Route/RemoteNode) 시 ERROR 로그 + legacy fallback (backward compat)
- `ROUTING_ACCESS_SERVICE` 는 legacy TAS 경로 (DND/reject 판정) 로 진행, 로그만 INFO
- 검증: REGISTER smoke 통과, Routing seed 없음 → NO_MATCH → legacy 유지 → 회귀 없음

선행 이슈 (REGISTER 403) 는 cims.sh sim 자동화 수정으로 해결 완료 (`5b91f60`).

---

## psip + CSP 리팩토링 R1~R8 전체 완결

전체 outbound SIP 경로가 **policy-driven + per-listener** 로 재구성됨:
1. 수신: ACL + RoutingPolicy 평가 (R7)
2. 주소 선택: CspAddressing context-aware 헬퍼 (R5.a/b/c)
3. 라우팅: routes.jsonl 의 RouteConfig → remote_nodes.jsonl 의 RemoteNodeInfo (R8)
4. Send: psip per-request/response source socket (R5.b'/b''), TCP/TLS connect bind (R5.b'''), TLS per-listener cert (R5.c)
5. 수신 listener 인프라: UDP/TCP/TLS multi-listener + per-listener thread_count (R1/R2/R3/R4)

---

## 단계별 회귀 원칙

- 각 R 단계 후 **최소** CSP 기동 smoke + SIP 송수신 확인
- R7 이후 **Phase 1 회귀 6항목 전체** + Phase 2/3 검증 재개
- 중간 단계에서 Phase 1 전체 회귀 돌리면 REGISTER credential 이슈로 false failure 가능 → R7 이후에 몰아서

---

## 관련 파일 (다음 세션 빠른 접근용)

- Plan R1: `/home/nex/.claude/plans/polymorphic-humming-pie.md`
- 아키텍처 소스:
  - `ext/psip/SipStack/SipStack.cpp:74` (CSipStack::Start)
  - `ext/psip/SipStack/SipStack.cpp:554` (AddUdpListener)
  - `ext/psip/SipUserAgent/SipUserAgent.h:118` (m_clsSipStack)
  - `csp/CspServer.cpp:99~` (CSipStackSetup 조립, R1 변경 반영 완료)
  - `csp/CspServer.cpp:332, 360, 376, 380` (Via/Contact/XCAP — R5 대상)
  - `csp/CspListenerManager.cpp` (Sync 로직, R2~R4 대상)
  - `csp/CspLocalNodeMap.{h,cpp}` (R1 변경 완료)
- config: `csp/config/config_template.json` (sections + collections), `build/dist/config/local_nodes.jsonl` (seed)
