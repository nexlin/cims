---
name: project-session-2026-05-18-sip-outbound-addressing
description: 2026-05-18 회기 — CSP SIP outbound Via/Contact 자기 주소를 route_결정 / inbound_listener 기반으로 동적화. commit 5개 (3ad3333~092047d). T7 LIVE + (D)/(B)/(C) 모두 PASS. plan /home/nex/.claude/plans/csp-sip-prancy-panda.md
metadata: 
  node_type: memory
  type: project
  originSessionId: 87eb52a0-a002-49e9-a52f-91e779a2b6e4
---

# 2026-05-18 회기 — CSP SIP outbound 자기 주소 동적화

## 발단

사용자 지적: IBCF 멀티 피어 / multi-realm 환경에서 SIP UA 의 "내 IP:port identity" 는
하나가 아니라, Rule/Route/RouteSet 결정 또는 access_service binding 에 따라
dialog/transaction 단위로 동적 결정되어야 함. 단말이 직접 붙는 Access Service 도 동일.

그런데 c2c8911 이전에 만든 단일 primary 모델은 이를 못 반영. T1 조사로 확인:
- 데이터 스키마 (`routes.local_node_ref`, `access_services.allowed_local_node_refs`) 는 이미 그 모델
- `CspAddressing::GetLocalSipAddress/GetLocalSipAddressForOutbound` 인프라도 부분 존재
- 그러나 핵심 송신 자리 (psip `SipDialog::CreateInvite/Ack/Cancel`, `SendNotifyToSubscriber`) 가
  여전히 `m_clsSetup.m_strLocalIp` 한 값만 참조 → 통합 미완성

## 한 일 (commit 5개 — origin/main `c2c8911`..`092047d`)

1. **3ad3333** `feat(csp+psip): SipDialog outbound local identity hint + NOTIFY 자기 주소 listener 기반`
   - psip `CSipDialog` 에 `m_strOutboundLocalIp` + `m_iOutboundLocalPort` 추가 + Via 분기 3자리
   - `SubscriptionInfo.iInboundListenerId` 추가, SUBSCRIBE 수신 시 캡처
   - `SendNotifyToSubscriber` 가 listener 기반 자기 주소 결정
   - `CspAddressing::GetLocalSipPort(listener, fallback)` 신규
   - 7 files +56/-5
2. **b727167** `feat(csp+psip): route 결정 → outbound leg Via/Contact 자기 주소 동적화`
   - `CSipCallRoute` 에 outbound hint 멤버 추가, `CreateCall`/`StartCall` 이 dialog 에 복사
   - `PendingRouteEntry.local_node_ref` 추가, `RecvRequest` 에서 저장
   - `EventIncomingCall` 의 Take 직후 `LocalNodeMap.GetByName` → bind_ip:port 추출 → `clsRoute` hint 설정
   - 4 files +49/-3
3. **95e87b2** `feat(csp): REGISTER/302 응답 port 도 inbound listener 기반으로 통일 + primary fallback 의미 명시`
   - REGISTER 200 OK Service-Route port + 302 Moved Temporarily Contact port → `GetLocalSipPort`
   - CspServer primary 부재 fail-fast 분기 주석 갱신 — primary 의미를 "hint 없는 경로의 fallback identity" 로 명시 (옵션 B 채택)
   - 3 files +15/-9
4. **8ecf4c4** `feat(csp): PTT GroupCallService InviteMember outbound 자기 주소를 access_services(kind=ptt) listener 기반으로`
   - GroupCallService.cpp 의 CreateCall 호출 전 access_services(kind=ptt).allowed_local_node_refs[0] → LocalNodeMap → clsRoute hint set
   - 1 file +17
5. **092047d** `feat(psip): SipStackComm 자동 Contact 추가 시 incoming listener 의 bind_ip:port 사용`
   - psip `CSipStack::_GetListenerBind(id, transport, ...)` helper 신규
   - SipStackComm 의 Contact 자동 추가 자리에서 `m_iListenerId > 0 + 매칭 성공` 시 listener bind 값 사용
   - **B-leg outbound INVITE 외에도 모든 응답 (401/403 등) 의 Contact 가 incoming listener 기반으로 박힘** — psip 영역
   - 3 files +99/-11

**LIVE 검증**:
- T7-A LIVE 회귀 (single listener) — Contact `10.0.1.13:5060` NO-OP 확인
- T7-B 다중 listener (5070 test row 동적 추가) — Contact `10.0.1.13:5070` 정확히 박힘. baseline 복원
- 161 unit test PASS, hint 미설정 경로 NO-OP regression 확인

## 단계 진행 결과

| 단계 | 상태 | 비고 |
|---|---|---|
| **T1** 송신 자리 전수 조사 | ✅ | 7자리 핵심 변경 + 5자리 SDP/CMP (별도 트랙) + 12자리 INIT (변경 불필요) 식별 |
| **T2** psip dialog hint 인프라 | ✅ | commit 3ad3333 |
| **T3** route 결정 → outbound hint | ✅ | commit b727167. B-leg outbound 자기 주소 |
| **T4** AccessService 응답 통합 | ✅ (확장) | commit 95e87b2 (REGISTER/302 port) + 092047d (psip 자동 Contact 영역 통합 — listener_id 기반). dialog state matched_service_name 추적은 후속 (H 후보) |
| **T5** primary 의미 축소 | ✅ | 옵션 B 채택 (fail-fast 유지) + 주석 명시 |
| **T6** Record-Route | ✅ 인프라 확인 | psip AddRecordRoute + 응답 echo back 이미 완전. B2BUA 디자인상 자기 leg 의 적극 push 는 IBCF proxy 활성화 시점의 후속 (E 후보) |
| **T7** LIVE 검증 | ✅ | single (5060=NO-OP) + dual (5060/5070 분리 박힘) 모두 sip.jsonl LIVE 확인 |
| **(D)** PTT GroupCallService | ✅ | commit 8ecf4c4 |
| **(C-extended)** psip 자동 Contact listener 기반 | ✅ | commit 092047d. dual-listener 환경의 대칭 라우팅 완성 |

## 다음 회기 진입 후보 (사용자 시점 우선순위)

| 후보 | 작업 | 환경/조건 | 예상 작업량 |
|---|---|---|---|
| ~~(A) T7 LIVE 회귀~~ | ✅ 완료 — single listener NO-OP 검증. Contact `10.0.1.13:5060` 옛 동작 그대로 | | |
| ~~(B) T7 다중 listener LIVE~~ | ✅ 완료 — 5070 test row 추가 + SIGUSR1 → dual-listener 검증 (5060=NO-OP, 5070=동적). 추가 row 제거 후 baseline 복원 | | |
| ~~(C) T4 심화 (실현 형태)~~ | ✅ 완료 — psip SipStackComm 의 자동 Contact 추가가 m_iListenerId 기반으로 동작. 응답 Contact 가 incoming listener bind_ip:port 로 박힘 | | |
| ~~(D) GroupCallService InviteMember 통합~~ | ✅ 완료 — access_services(kind=ptt).allowed_local_node_refs[0] 기반 hint | | |
| **(E) IBCF proxy 활성화 (Phase 4) + Record-Route 적극 push** | IbcfModule stub 본격 구현. RR 헤더 양 leg push. 다중 listener 환경의 B2BUA dialog 추적 | 큰 작업 | 큼 |
| **(F) Console UI 정비 (옛 거절된 트랙)** | `config_template.json` 의 deprecated bind 5키 schema 제거 + Setup.Sip 영역에 "Local Nodes / Access Services 에서 관리" 안내 배너 + local_nodes↔access_services 매트릭스 시각화 | UI 변경 | 중간 |
| **(G) IBCF 멀티 피어 routes LIVE** | routes.jsonl 에 외부 peer 1개 셋업 + remote_nodes 추가 + routing_policies + cspsim 으로 trunk 호 → B-leg outbound 의 Via/Contact 가 route.local_node_ref 의 IP:port 인지 sip.jsonl 검증 (b727167 의 LIVE 효과 확인) | 환경 셋업 + peer simulator | 중간 |
| **(H) AccessService 의 dialog state matched_service_name 추적** | (C) 가 listener_id 기반으로 비슷한 효과 달성하지만, access_service 단위 정책 (server_identity_uri 등) 까지 가려면 dialog 에 service_name 추적 필요. T4 원래 plan 의 심화 | 코드 | 중간 |

**다음 세션 추천 순서**: **(G) → (F) → (H) → (E)**

- **(G)** 이번 회기 변경의 마지막 LIVE 검증 영역 (b727167 의 B-leg outbound). IBCF peer simulator 가 따로 없어도 cspsim 의 `-callee_override` 와 `-no_register` 옵션으로 흉내 가능.
- **(F)** 사용자가 본 회기 초반에 의도했던 UX 트랙. 데이터 모델은 완성되었으므로 UI 안내가 자연스럽게 따라옴.
- **(H)** access_services 별 정책 (server_identity_uri override 등) 활성화 시 필요.
- **(E)** 가장 큰 작업. IBCF proxy 본격 구현 + RR. 우선순위 가장 낮음.

## 옛 트랙과의 관계

- **CSC config-server 트랙** (commit `79d7c46`/`66706b8`/`c2c8911`, [[project-session-2026-05-17-walkthrough]], [[project-session-2026-05-18-csc-config-server]]):
  이번 회기의 출발점. c2c8911 이 "csp.json bind 5키 제거 → local_nodes SoT" 를 정착시켰는데,
  outbound Via/Contact 결정의 동적화는 그 자연스러운 다음 단계.
  - 이전 회기 미진행 영역: 단계 5 호 시험 (LIVE 회기) — 본 회기에서도 미진행. (A) 후보로 통합.
- **deployment Phase 1~5** ([[project-session-2026-05-15-deployment-scaffold]]):
  render.py 의 `_build_local_nodes` 가 primary 1행 보장. 본 회기의 옵션 B (fail-fast 유지) 가
  이 체계를 신뢰하는 결정.

## 핵심 파일 (변경 위치 기억용)

- `ext/psip/SipUserAgent/SipDialog.{h,cpp}` — outbound hint 멤버 + Via 분기 3자리
- `ext/psip/SipUserAgent/SipUserAgent.h` + `SipUserAgentCall.hpp` — CSipCallRoute hint + dialog 복사
- `csp/SubscriptionManager.h` + `csp/CscfModule.cpp:397~` — SubscriptionInfo listener id 캡처
- `csp/CspServer.cpp:458~` — SendNotifyToSubscriber 자기 주소 (listener 기반)
- `csp/CspAddressing.{h,cpp}` — GetLocalSipPort 신규
- `csp/CspPendingRouteMap.h` + `csp/ModuleDispatcher.cpp:266 / 454~ / 569~` — RoutingDecision → outbound hint
- `csp/CscfModule.cpp:280~` — REGISTER Service-Route port
- `csp/ModuleDispatcher.cpp:514~` — 302 Contact port
- `csp/CspServer.cpp:119~` — primary fail-fast 주석 명시
- `csp/GroupCallService.cpp:340~` — PTT InviteMember access_services(kind=ptt) listener hint
- `ext/psip/SipStack/SipStack.{h,cpp}` — `_GetListenerBind(id, transport, outIp, outPort)` helper 신규
- `ext/psip/SipStack/SipStackComm.hpp:465~` — Contact 자동 추가 자리 listener_id 분기

## LIVE 환경 상태 (다음 회기 진입 시)

- ctrl-a/b csp PID 2270316/2270317 (092047d 빌드, 20:57 기동)
- 5060 UDP / 25061 TCP / 5061 TLS — primary listener 3개
- CMP media-a/b (옛 PID 640474/640475, 옛 빌드 — cmp 코드는 변경 없음)
- VIP 10.0.1.13 ctrl-a MASTER
- TB-CSC 4419, Vite 3000 정상
- service_log: `build/dist/ext_mnt/service_log/2026/05/18/20/csp_01_sip.msg.jsonl`

## plan 파일

`/home/nex/.claude/plans/csp-sip-prancy-panda.md` — T1~T7 단계 상세 + 검증 방법 + scope 명시.
다음 회기에서 참고 시 그대로 사용 가능.
