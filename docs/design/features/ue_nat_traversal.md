# 단말 NAT Traversal — 시그널링·미디어 평면 설계

단말(UE)이 NAT 뒤에 있는 환경에서 CIMS 가 시그널링(SIP)과 미디어(RTP)를 처리하는 방식의
정본 문서다. 상용 배치는 내부망(no-NAT)이 기본이지만, **일부 상용 구간과 공인 노출
검증망에서 NAT 뒤 단말을 정식 지원**한다.

관련 문서: [modules/csp.md](../modules/csp.md) · [modules/cmp.md](../modules/cmp.md) ·
[api/cmp_media_api.md](../../api/cmp_media_api.md)

> **진행 중 미해결 이슈**: PTT 그룹콜 조인 크래시(UE pjsua `med_prov_cnt` assert) + 발언 무음은
> [ptt_join_crash_and_silence.md](ptt_join_crash_and_silence.md) 참조 (UE 측 결함, 서버 결백).

## 1. 모델 요약

| 평면 | 원칙 | 메커니즘 |
|---|---|---|
| 시그널링 (CSP) | 항상 NAT-safe | RFC 3581 received/rport symmetric 응답 + 등록 바인딩 = 실소스 latch + SendDest 오버라이드 + TCP/TLS 연결 재사용 |
| 미디어 (CMP) | **leg 별 전용 포트 = 신원** | 각 leg(1:1 peer / PTT 멤버)에 전용 로컬 포트셋 할당. 소스 주소로 peer 를 추측하지 않는다 |
| NAT 미디어 | 제어평면 승인형 목적지 latch | CSP 가 leg 별 NAT 여부를 판정해 자원할당 명령에 명시 → CMP 는 지정된 leg 에서만 송신 목적지를 학습 |

핵심 설계 결정 — **공유 포트 + 소스 학습(과거 방식)을 배제**한다. 포트셋 1개를 여러 peer 가
공유하면 peer 구분이 수신 소스 주소에 의존하는데, NAT 환경(특히 같은 NAT 뒤 다중 단말)에서
소스 학습은 원리적으로 모호하다: 1:1 양쪽 NAT 시 녹취 발/착 스왑, PTT 에서 비-owner
keepalive 오인(남의 음성이 owner 세그먼트로 녹취), 청취 전용 멤버의 하향 목적지 식별 불가.
leg 별 전용 포트는 "그 포트로 온 것 = 그 leg" 로 이를 구조적으로 해소한다 (SBC 표준 방식).
NAT latch 는 신원 판정이 아닌 **송신 목적지 학습**으로 축소된다.

## 2. 시그널링 평면 (CSP + psip)

| 항목 | 동작 | 코드 |
|---|---|---|
| 응답 라우팅 | 모든 요청의 top Via 에 실소스 각인(`received` 항상, `rport` 실포트). 응답은 received/rport 로 송신 (RFC 3581) | `ext/psip/SipParser/SipMessage.cpp` `AddIpPortToTopVia`, `SipStackComm.hpp` 응답 경로 |
| 등록 바인딩 | Contact URI 원문은 에코/reginfo 전용. 실제 도달 주소는 top Via 의 received/rport(`GetTopViaIpPort`)를 `CUserInfo::m_strIp/m_iPort` 로 저장 | `csp/UserMap.cpp`, `csp/UserMap.h` |
| 인바운드 라우팅 | R-URI = 등록 Contact URI(target refresh), 실제 전송 목적지는 `m_strSendDestIp/m_iSendDestPort` 오버라이드(latch 주소) — 사설 Contact 여도 도달 | `csp/GroupCallService.cpp`, `csp/CspServer.cpp` |
| TCP/TLS | 등록에 쓰인 accept 소켓을 (IP:port) 키로 재사용. 인바운드 요청도 기존 연결로 송신 (RFC 5626 유사, flow-token 없음) | `ext/psip/SipStack/SipStackComm.hpp`, `SipTcpThread.cpp` |
| UE keepalive | 등록 단말 대상 주기 OPTIONS (`Setup.Sip.SendOptionsPeriod`, 기본 0=비활성). NAT 배치에서는 활성 필수 ([§7](#7-운영-요건)) | `csp/UserMap.cpp` `SendOptions` |

## 3. 미디어 평면 — leg 별 전용 포트셋

### 3.1 1:1 relay (VoLTE)

relay 세션은 **peer 별 4포트 블록 × 2 = 8포트**를 소유한다.

```
peer0 (발신 A):  Q    audio RTP     peer1 (착신 B):  Q+4  audio RTP
                 Q+1  audio RTCP                     Q+5  audio RTCP
                 Q+2  video RTP                      Q+6  video RTP
                 Q+3  video RTCP                     Q+7  video RTCP
```

- A-leg SDP(발신자에게 주는 answer)는 peer0 포트를, B-leg SDP(착신자에게 주는 offer)는
  peer1 포트를 광고한다. 각 peer 는 자기 전용 포트로만 송신하므로 **수신 소켓이 곧 peer
  신원**이다 — 소스 주소 매칭 없음.
- relay: peer i 소켓 수신 → peer 1-i 의 목적지 주소로 peer 1-i 소켓에서 송신
  (송신 소스 포트 = 그 peer 에게 광고한 포트 → symmetric RTP 정합).
- 녹취 track a/b = 수신 소켓 기준. NAT/도착 순서와 무관하게 발/착 귀속이 항상 정확하다.
- no-NAT leg 의 미협상 소스(선언 주소 불일치) 패킷은 드롭 + rate-limited WARN + 카운터.

### 3.2 PTT 그룹

- **floor control 포트는 그룹 공유 유지.** floor 메시지(RTCP APP, TS 24.380)는 User ID
  필드가 in-band 신원이라 공유 포트에서도 모호성이 없다. NAT 멤버의 floor 목적지는
  User-ID 기반 주소 latch 로 학습한다 (`PMcpttGroup::onFloorPacket`). latch 자격·안전
  조건은 RTP 와 동일하다 — nat 지정 멤버만 대상이고, `latch_ip_guard=strict` 면 소스
  IP == `user_sig_ip` 일 때만 학습·수락한다(포트만 일치하는 IP 학습 경로 포함, 불일치는
  드롭 카운터 + rate-limited WARN).
  - 상향 floor 메시지가 도착해야 하향(GRANT/TAKEN/IDLE) 목적지가 열린다. **한 번도 발언하지
    않는 청취 전용 멤버**는 자발적 상향이 없으므로, UE 가 참여 직후 및 주기적으로 Floor
    Ack(User ID 포함)를 송신해 매핑을 열고 유지해야 한다 ([§7](#7-운영-요건)).
  - latch 는 수신할 때마다 갱신되므로 NAT rebind 를 자연히 추종한다. 학습 실패 구간(첫 Ack
    이전)에는 SDP 선언 주소로 송신한다 — 사설이면 도달하지 않지만 무해하다.
- **audio(+video) RTP 는 멤버별 전용 포트.** 멤버 참가 시 멤버 포트 유닛(audio RTP,
  video RTP)을 할당하고, 그 멤버의 SDP 에 해당 포트를 광고한다.
  - 상향(발언): 멤버 유닛 소켓 수신 = 그 멤버의 미디어. floor owner 검증 후 분배·녹취
    (owner 가 아니면 드롭) — 녹취 세그먼트 귀속은 floor 상태가 결정하며, 소스 오인으로
    남의 음성이 섞일 경로가 없다.
  - 하향(청취): 각 멤버의 유닛 소켓에서 그 멤버의 목적지로 송신. NAT 멤버의 목적지는
    유닛 포트로 도착하는 첫 유효 패킷(RTP keepalive 포함)으로 latch — 유닛 포트 자체가
    멤버 신원이므로 같은 NAT 뒤 다중 청취 멤버도 안전하다.
  - PTT 는 발언 중에만 상향 RTP 가 흐르므로, 청취 전용 멤버의 하향 오디오는 **UE 의 RTP
    keepalive 에 전적으로 의존**한다 (RFC 6263, [§7](#7-운영-요건)).

### 3.3 포트 산정

| 자원 | 포트 소요 |
|---|---|
| 1:1 호 | 8 (audio/video RTP+RTCP × 2 peer) |
| PTT 그룹 | floor 1 (+예비 1) — 그룹당 |
| PTT 멤버 | audio RTP 1 (+RTCP 예비 1) + video RTP 1 (+예비 1) — 멤버당 |

풀 사이징: VoIP `RtpPoolSize` = 동시 1:1 호 수(호당 8포트), PTT `PttRtpPoolSize` = 동시
그룹 수, `PttMemberPoolSize` = 동시 참가 멤버 수. 방화벽은 해당 대역을 개방한다
([modules/cmp.md](../modules/cmp.md) 포트 풀 절 참조).

## 4. NAT 정책 — access service 단위

정책은 CSP **access_services** 설정(콘솔 편집, SIGUSR1 reload)에 둔다.

| 필드 | 값 | 의미 |
|---|---|---|
| `media_nat_mode` | `off`(기본) | NAT 미디어 비활성 — 모든 leg 를 no-NAT 로 취급 |
| | `auto` | **상용 표준.** CSP 가 leg 별로 판정: SDP 선언 미디어 IP 가 그 leg 의 SIP 시그널링 실소스 IP 와 다르거나 사설(RFC1918)이면 nat=true |
| | `force` | 해당 access service 의 모든 leg nat=true (판정 불가 단말·검증 환경) |
| `latch_ip_guard` | `strict`(기본) | latch 소스 IP == 그 leg 의 SIP 시그널링 실소스 IP 일 때만 latch 허용 (스푸핑 방어) |
| | `off` | IP guard 해제 — 시그널링/미디어가 다른 공인 IP 로 나오는 환경(carrier-grade NAT 등)만 |

CSP 는 판정 결과를 자원할당 명령에 leg 단위로 전달한다
([api/cmp_media_api.md](../../api/cmp_media_api.md)):

- `RELAY_ADD`/`RELAY_MODIFY`: `remote_nat`(0/1) + `remote_sig_ip`(그 leg 의 SIP 실소스 IP)
- `PTT_JOIN`: `user_nat`(0/1) + `user_sig_ip`

nat 필드 생략 = 0 (no-NAT). CMP 에는 NAT 전역 설정이 없다 — **latch 자격은 오로지
제어평면이 leg 단위로 부여**하며, no-NAT leg 는 latch 코드 경로 자체가 비활성이다.

판정·전달은 최초 협상뿐 아니라 재협상에도 적용된다 — CSP 는 re-INVITE 수신 시 재협상
leg 의 새 SDP 주소로 NAT 를 재판정해 다시 전달한다: 1:1 relay 는 `RELAY_MODIFY`
(수신 A leg=peer0 / 발신 B leg=peer1), PTT 멤버 leg 는 `PTT_JOIN` ②(멱등 재-JOIN).
망 전환 등으로 주소가 바뀐 leg 는 이 경로로 재-latch 된다.

## 5. 목적지 latch (CMP)

nat=true leg 의 전용 포트에서:

1. **첫 유효 RTP** 수신 시 소스 주소를 그 leg 의 송신 목적지로 latch.
   유효 조건 — RTP version=2, 협상된 payload type, 최소 길이, `latch_ip_guard=strict` 면
   소스 IP == `remote_sig_ip`/`user_sig_ip`.
2. latch 시 **SSRC 고정.** 이후 소스 주소 갱신(re-latch)은 동일 SSRC 의 패킷일 때만 허용
   — NAT rebind(매핑 변경) 추종과 제3자 주입 차단을 겸한다.
3. RTCP 목적지는 latch 소스 IP + RTCP 포트 관측으로 교정한다 (관측 전에는 선언 포트+1 추정).
4. `RELAY_MODIFY`/`PTT_JOIN` 으로 leg 주소가 갱신되면(re-INVITE) latch 상태를 리셋하고
   재-latch 를 허용한다. 선언 주소·nat·guard 가 직전과 동일한 재요청(세션 refresh 성
   re-INVITE, 재전송)은 latch 를 유지한다.
5. latch 전까지 송신 목적지는 선언 주소다 (사설이면 도달하지 않지만 무해).

관측: latch 발생 시 INFO 로그. `STATS` detail 에 nat leg 의 `learned_ip/learned_port`
노출. 미협상 소스 드롭은 세션/그룹별 카운터 + rate-limited WARN.
이벤트 채널이 활성화되면 `RELAY_NAT_LATCHED` 이벤트로 통지한다 (규격 예약 —
[api/cmp_media_api.md §8](../../api/cmp_media_api.md#8-이벤트-type-event)).

## 6. 자원 관리

- 자원 lifecycle 은 기존과 동일 (RELAY_ADD/REMOVE, PTT_GROUP_ADD/REMOVE, JOIN/LEAVE +
  sweeper). 유효 수신(신원 확정 패킷)만 활동시각을 갱신하므로, 미디어가 전혀 성립하지 않은
  세션(잘못된 NAT 설정 등)은 orphan(`OrphanReclaimSec`) 경로로 조기 회수된다.
- 멤버 포트 유닛은 PTT_LEAVE/그룹 해제 시 풀로 반환. 고갈 시 `NO_RESOURCE`.
- `HEARTBEAT`/`STATS` 의 resource 요약에 relay 블록·PTT 그룹·멤버 유닛 사용량을 보고한다.

## 7. 운영 요건

NAT 뒤 단말을 수용하는 access service 배치 체크리스트:

- **시그널링 바인딩 유지**: NAT UDP 매핑은 통상 수십 초에 소멸한다.
  `Setup.Sip.SendOptionsPeriod` 를 활성(권장 25s 이하)하거나 UE 의 재등록/keepalive 주기를
  그 이하로 설정한다. TCP/TLS 등록은 연결 유지로 충분하다.
- **미디어 바인딩 유지**: UE 는 RTP keepalive(무음 구간 empty RTP 등)를 송신해야
  하향 경로 latch 와 NAT 매핑이 유지된다. PJSIP 계열은 `PJMEDIA_STREAM_ENABLE_KA` 로
  제어되며 **기본값이 0(비활성)** — CIMS UE 빌드는 config_site.h 에서 활성화한다
  (empty RTP, 주기 5s — [android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md) §2.5).
- **정책**: 해당 access service 에 `media_nat_mode=auto` (판정 불가 단말만 `force`),
  `latch_ip_guard=strict` 유지. CGN 등 시그널링/미디어 공인 IP 상이 환경만 `off`.
- **포트/방화벽**: leg 별 포트 소요([§3.3](#33-포트-산정))에 맞춰 풀 크기와 방화벽 대역을
  산정한다.
- 공인 노출망의 인증 우회 스위치 `Setup.TestEnvOpenTermination` 은 검증 전용이다 —
  상용은 false 유지 (미등록 발신 INVITE 는 표준 401 챌린지).

### 7.1 UE 구현 요건 (PTT)

서버는 "UE 가 각 소켓으로 무언가 보낸다"는 전제 위에서만 하향 경로를 열 수 있다. NAT 배치의
PTT 단말은 **세 소켓 모두**에 대해 유입 매핑을 열고 유지해야 한다.

| 소켓 | UE 동작 | 주기 | 미이행 시 증상 |
|---|---|---|---|
| SIP | 재등록 또는 서버 OPTIONS 응답 | ≤25s (UDP) | 인바운드 INVITE 미도달 |
| floor (m=application) | **Floor Ack (User ID 포함)** — 참여 직후 1회 + 주기 송신 (`FloorClient` 내장, 15s) | ≤20s | 청취 중 GRANT/TAKEN/IDLE 미수신 (음성은 들리나 발언자 표시·발언권 응답 없음) |
| audio RTP (멤버 유닛) | **RTP keepalive** (`PJMEDIA_STREAM_ENABLE_KA=1`, empty RTP) | 5s (pjsip 기본) | 청취 전용 상태에서 하향 오디오 전무 |

Floor Ack 를 keepalive 로 쓰는 것은 TS 24.380 이 규정한 절차는 아니다 — 규격은 floor 평면의
NAT traversal 을 (ICE 전제로) 다루지 않으므로, User ID 필드를 이용한 주소 latch 와 그 유지는
CIMS 의 규격 적합 확장이다. Ack 는 서버 상태를 바꾸지 않아 부작용이 없다.

## 8. 표준 근거

| 규격 | 규정 내용 | 대응 |
|---|---|---|
| RFC 3581 / RFC 5626 | 시그널링 NAT — received/rport, outbound 연결 재사용 | [§2](#2-시그널링-평면-csp--psip) |
| RFC 4961 | Symmetric RTP/RTCP — latch 의 전제 | [§3](#3-미디어-평면--leg-별-전용-포트셋) 송신 소스 포트 = 광고 포트 |
| RFC 6263 | RTP keepalive (empty RTP 등) — 수신 전용 단말의 매핑 소멸 문제 | [§7.1](#71-ue-구현-요건-ptt) |
| RFC 7362 | Latching(Hosted NAT Traversal) — 기법과 한계(스푸핑, 무송신 단말 latch 불가) 문서화 | [§5](#5-목적지-latch-cmp) + IP guard + SSRC 고정 |
| RFC 8445 (ICE) | 양방향 connectivity check — 무송신 단말까지 해소 | **미지원** ([§9](#9-미구현향후-과제)) |
| 3GPP TS 23.228 Annex G / TS 24.229 | IMS NAT traversal 모델 (IMS-ALG + IMS-AGW) | CSP=ALG, CMP=AGW 로 동형 |
| 3GPP TS 29.334 (Iq, H.248) | IMS-AGW 에 remote source address 학습(latching)을 **제어평면이 leg 단위로 지시** | `remote_nat`/`user_nat` + `*_sig_ip` ([§4](#4-nat-정책--access-service-단위)) |
| 3GPP TS 24.380 | floor 메시지 User ID 필드, Floor Ack | [§3.2](#32-ptt-그룹) 멤버 식별 |

규격이 **정의하지 않은 것** — floor 제어 채널 자체의 NAT traversal 절차, 청취 전용 참가자의
하향 경로 부트스트랩(RFC 6263 은 단말 책임으로 둔다). 위 §3.2·§7.1 은 그 공백을 메우는
CIMS 구현 규약이다.

## 9. 미구현/향후 과제

- **ICE 미지원** — symmetric NAT(포트까지 변환)에서 floor/오디오 매핑을 UE keepalive 없이 여는
  방법은 없다. 서버측 ICE-lite 도입이 근본 해법이다.
