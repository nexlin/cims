# 비정상 종료 leg 감지 — SIP 세션 타이머(RFC 4028) 기반 leg 생존 관리

단말이 **BYE 없이 사라진 leg**(앱 강제종료·전원 차단·배터리 소진·망 소실)를 서버가 스스로
감지해 정리하는 정본 문서다. 감지 수단은 **SIP 세션 리프레시(RFC 4028)** 이며, 감지 이후의
정리는 기존 teardown 연쇄(BYE 수신 경로)를 그대로 재사용한다.

관련 문서: [modules/csp.md](../modules/csp.md) · [ptt_flows.md](ptt_flows.md) ·
[mcptt_emergency_modes.md](mcptt_emergency_modes.md) · [ue_nat_traversal.md](ue_nat_traversal.md) ·
[api/cmp_media_api.md](../../api/cmp_media_api.md)

> **상태**: 구현·배포 완료(csp 0.2.89). 실기기 검증 6항목 전부 통과 — [§13](#13-검증).

## 1. 문제

BYE 없이 leg 이 사라지면 CSP·CMP 어디에서도 그 사실을 알 수 없다. 다이얼로그는 살아 있는 것으로
취급되어 다음이 모두 어긋난다.

| 영향 | 내용 |
|---|---|
| 잔여 1인 해제 미발동 | private call(2인)·ad-hoc 은 마지막 상대 이탈 시 세션을 해제한다. 그 트리거가 `OnCallTerminated`(BYE) 하나뿐이라, 상대가 강제종료되면 **살아있는 쪽이 죽은 통화에 무한정 남는다** |
| 로스터 유령 멤버 | RFC 4575 conference-info 에 이탈이 반영되지 않아 참여자 목록이 사실과 어긋난다 |
| 미디어 자원 점유 | CMP 멤버 유닛(포트)이 반납되지 않는다. 그룹은 `getMemberCount()==0` 일 때만 회수되므로 유령 1명이 그룹 전체를 붙든다 |
| 세션 종료 지연 | 녹취 세션·PTT 세션 이력의 종료 시각이 실제와 어긋난다 |
| floor 오염 | 죽은 멤버가 발언 중이었다면 T1(End of RTP media, 4초)이 회수하지만, 대기열·정원은 멤버가 남아 있는 한 그를 후보로 센다 |

현재 자기 회수가 있는 구간은 **1:1 relay 뿐**이다 — CMP sweeper 가 RTP 무활동 relay 를 회수하고
`RELAY_ABORTED` 로 CSP 에 통지한다(`orphan_no_rtp` 120초 / `hold_timeout` 600초). PTT **그룹 멤버
단위**에는 대응물이 없다.

## 2. 계층 선택 — 왜 SIP 인가

### 2.1 미디어 평면 신호는 규격이 아니라 단말 구현에 종속된다

미디어 평면에 멤버별 주기 신호가 **없지는 않다.** 다만 그 신호는 규격이 요구하는 것이 아니라
NAT 매핑 유지를 위해 CIMS 단말이 스스로 넣은 확장이다([ue_nat_traversal.md §7.1](ue_nat_traversal.md#71-ue-구현-요건-ptt)).

| 신호 | 주기 | 출처 |
|---|---|---|
| floor Ack keepalive (User ID 동반) | 15초 | 앱 `FloorClient.ACK_PERIOD_SEC` — 참여 즉시 시작, 청취 전용 멤버 포함 전원 |
| audio RTP keepalive (empty RTP) | 5초 | pjmedia `PJMEDIA_STREAM_ENABLE_KA` (CIMS UE 빌드에서 활성) |
| 발언 중 RTP | 20ms | floor GRANT 구간에만 |

유휴 구간의 무음 RTP(50pps)는 vendored pjproject 패치로 제거되어(`ext/pjproject/.../stream.c`
"브리지 미연결(무전 유휴) zero-PCM" 분기) 유휴 상향은 위 keepalive 뿐이다.

규격 쪽 신호는 없다. TS 24.380 은 floor 평면에 주기 keepalive 를 정의하지 않고, RTCP 는
단말(pjmedia)이 **미디어 구동**으로만 보내며(`stream.c` `check_tx_rtcp()` 는 `put_frame()`/
`on_rx_rtp()` 에서만 호출), CMP 멤버 유닛은 애초에 RTCP 포트(+1)를 바인드하지 않는다
(`cmp/PPttMemberPort.cpp` `init()` — audio/video RTP 소켓 2개).

따라서 미디어 축 감지는 **"우리 앱이 keepalive 를 보낸다"는 전제 위에서만** 성립한다. 그 전제가
깨지는 단말(타사 스택·구버전 APK)을 살아 있는데 죽었다고 판정할 위험이 있고, 커버리지도 floor 를
쓰는 PTT 멤버로 한정된다(VoLTE·`floor_control:"off"`·MCData 제외). 반대로 감지 속도는 SIP 축보다
빠르고(15초 주기 → 3회 누락 ≈45초), **시그널링은 살아 있는데 미디어만 죽는 경우**를 잡는 것은
미디어 축뿐이다 — 그래서 폐기가 아니라 **보조축**으로 [§14](#14-후속-과제) 에 남긴다.

### 2.2 SIP 계층에는 표준과 연쇄가 모두 이미 있다

| 근거 | 내용 |
|---|---|
| 표준이 이 문제를 정면으로 정의한다 | RFC 4028 은 "BYE 유실·비정상 종료로 남는 hung session 을 시한으로 회수한다"가 목적인 규격이다. IMS 세션(TS 24.229)·MCPTT 세션(TS 24.379)은 모두 SIP 다이얼로그이므로 그대로 적용된다 |
| 감지 후 처리 연쇄가 이미 완비돼 있다 | 트랜잭션 무응답 → `CSipUserAgent::SendTimeout()` → `EventCallEnd(callId, SIP_GONE)`(`ext/psip/SipUserAgent/SipUserAgentSipStack.hpp`) → `CModuleDispatcher::EventCallEnd` → `CGroupCallService::OnCallTerminated()` → PTT_LEAVE·잔여 1인 해제·conference NOTIFY·CallMap 정리 |
| 서비스 무관 단일 메커니즘 | VoLTE(B2BUA 양 leg)·PTT 그룹·private·ad-hoc·MCData 가 같은 규율을 공유한다 |
| 단말 무관 | 세션 타이머 미지원 단말도 서버가 refresher 를 맡으면 감지된다([§5.3](#53-refresher-결정)) |

즉 **없는 것은 "주기적 생존 확인" 하나**이고, 그것을 규격이 정의한 자리에 놓는 것이 이 설계다.

## 3. 규격 근거 (RFC 4028)

| 항목 | 규정 |
|---|---|
| `Session-Expires` (compact `x`) | 세션 간격(delta-seconds) + `refresher=uac\|uas`. INVITE/UPDATE 요청과 그 2xx 응답에만 실린다 (§4) |
| `Min-SE` | 수용 가능한 최소 간격. 요청/응답 어디서든 **90초 미만 금지**, 미표기 시 기본 90초. 응답에는 **422 에만** 실린다 (§5) |
| 최소·권장 값 | 절대 최소 90초. 권장값은 1800초이고, 삽입 주체는 30분 미만을 고르지 **않아야 한다(SHOULD NOT)** — 다만 §4 는 "정확한 값은 대역폭·지연·토폴로지, 그리고 물론 **응용 시나리오**에 달렸다"며 MUST 가 아님을 명시한다 |
| 옵션 태그 | `timer`. 지원 UA 는 ACK 를 뺀 **모든 요청에 `Supported: timer`** 를 실어야 한다(MUST, §7.1) |
| 422 (Session Interval Too Small) | 요청의 SE 가 서버 최소치보다 작으면 발행. **`Min-SE` 동봉 필수** (§6, §9) |
| refresher 선정 | UAS 가 2xx 의 `refresher` 값을 정한다. UAC 가 값을 지정했으면 UAS 는 **뒤집을 수 없다** (§9 Table 2) |
| `Require: timer` | 2xx 의 refresher 가 `uac` 면 **MUST**, `uas` 이고 요청에 `Supported: timer` 가 있었으면 SHOULD (§9) |
| 갱신 수단 | re-INVITE 또는 UPDATE. 상대가 `Allow: UPDATE` 를 광고하면 UPDATE 권장. re-INVITE 는 offer 를 실어야 하고, **내용이 안 바뀌었으면 SDP origin(`o=`) 을 직전과 같게** 해 "변경 없음"을 표시해야 한다 (§7.4) |
| 갱신 시점 | refresher 는 **세션 간격의 절반**이 지나면 갱신 (RECOMMENDED, §7.2/§9) |
| 갱신 실패 | 갱신 트랜잭션이 timeout 되거나 408/481 이면 **BYE** (§10) |
| 만료 판정 | 갱신을 받지 못한 쪽(non-refresher)은 만료 **직전**에 BYE 를 보낸다. 선행 시간은 `min(32초, 세션간격/3)` 권장 (§10) |
| 타이머 해제 | 갱신의 2xx 에 `Session-Expires` 가 없으면 그 세션의 타이머는 해제된 것으로 본다 (§7.2) — 갱신 응답에 항상 echo 해야 하는 이유 |
| 부수 효과 | 세션 갱신 목적이 아닌 in-dialog re-INVITE/UPDATE 도 **갱신 효과를 갖는다** (§7.2) |

## 4. 상태 모델

세션 타이머 상태는 **SIP 다이얼로그(leg) 1개당 1벌**이며, B2BUA 의 두 leg 은 서로 독립적으로
협상한다(간격도 refresher 도 달라질 수 있다).

| 필드 | 의미 |
|---|---|
| `sessionExpires` | 협상된 세션 간격(초). 0 = 타이머 없음(협상 실패·비활성) |
| `localRefresher` | 갱신 주체가 우리(CSP)인가 |
| `lastRefreshTime` | 마지막 세션 갱신 트랜잭션의 2xx 송/수신 시각 — 만료·갱신 시점의 기준점 |
| `peerMinSE` | 상대가 요구한 최소 간격(422 수신값 및 수신 요청의 `Min-SE` 중 최대) |
| `peerSupportsTimer` | 상대 요청/응답에서 관측한 `Supported: timer` |

만료 시각 = `lastRefreshTime + sessionExpires`. 갱신 발사 시각 = `lastRefreshTime + sessionExpires/2`.

## 5. 협상 규칙

### 5.1 CSP 가 UAS 인 leg (단말 발신 INVITE 수락)

해당 경로: PTT 개시자 leg(`GroupCallService::ProcessGroupCall` → `AcceptCall`), VoLTE 착신측 B2BUA
leg, MCData MSRP leg.

1. 수신 INVITE 에서 `Supported: timer` / `Session-Expires` / `Min-SE` 를 읽어 다이얼로그에 보관한다.
2. `Session-Expires` 가 로컬 최소치(`MinSE`)보다 작으면 **422 + `Min-SE`** 로 거절한다.
3. 2xx 에 실을 간격 = `min(요청 SE, 로컬 SessionExpires)` 를 기본으로 하되,
   **요청의 `Min-SE`(없으면 90) 미만으로 줄일 수 없고, 요청 SE 보다 키울 수 없다**(§9).
   요청에 `Session-Expires` 가 없으면 로컬 `SessionExpires` 를 그대로 제안한다.
4. `refresher` 는 [§5.3](#53-refresher-결정) 규칙으로 정하고, `Require: timer` 를 §3 표대로 붙인다.

### 5.2 CSP 가 UAC 인 leg (서버 발신 INVITE)

해당 경로: PTT fan-out 초대(`GroupCallService::InviteMember`), VoLTE 발신측 B2BUA leg.

1. 모든 요청(ACK 제외)에 `Supported: timer` 를 싣는다 — 현재 CSP·psip 은 이 태그를 광고하지
   않는다(신규).
2. INVITE 에 `Session-Expires: <SessionExpires>;refresher=uac` + `Min-SE: <MinSE>` 를 싣는다
   (`refresher=uac` = CSP 자신이 갱신).
3. 2xx 처리:
   - `Session-Expires` 있음 → 그 값과 `refresher` 를 정본으로 채택한다.
   - `Session-Expires` 없음(상대 미지원) → §7.2 규정대로 **요청 값 + `refresher=uac`** 로 간주하고
     CSP 가 갱신을 수행한다. 미지원 단말도 이 경로로 감지된다.
   - 422 수신 → `Min-SE` 를 올려 CSeq 증가 후 재시도(§7.3).

### 5.3 refresher 결정

**CSP 는 가능한 한 자신이 refresher 를 맡는다.** 근거: ① 단말 구현 편차와 무관하게 감지 지연이
일정하고, ② 세션 타이머를 지원하지 않는 단말(구 APK·타사 단말)까지 동일하게 커버되며,
③ 살아 있는 단말을 오탐으로 끊을 여지가 없다(무응답이라는 능동 증거로만 판정).

RFC 4028 §9 Table 2 는 UAS 의 선택지를 다음으로 제한하므로, 규칙은 그 안에서 정의된다.

| 상대(UAC) `Supported: timer` | 요청의 `refresher` | 2xx 의 `refresher` | CIMS 의 선택 |
|---|---|---|---|
| 없음 | 없음 | `uas` 강제 | CSP 가 갱신 |
| 있음 | 없음 | `uas` 또는 `uac` | **`uas`(CSP)** 를 고른다 |
| 있음 | `uac` | `uac` 고정(뒤집기 불가) | 단말이 갱신, CSP 는 만료 감시만 |
| 있음 | `uas` | `uas` 고정 | CSP 가 갱신 |

설정 `Refresher` 로 `server`(기본) / `ue` / `auto` 를 고를 수 있게 하되, 규격상 뒤집을 수 없는
조합에서는 항상 규격이 우선한다.

## 6. 갱신 절차

### 6.1 서버가 갱신하는 경우 (기본)

```
        CSP                                   UE
         │  ── INVITE  Supported: timer ───────▶
         │     Session-Expires: 180;refresher=uac, Min-SE: 90
         │  ◀── 200 OK  Session-Expires: 180;refresher=uac ──
         │     (Require: timer)
         │  ── ACK ────────────────────────────▶
         │
         │   … SE/2 = 90초 경과 …
         │  ── re-INVITE (SDP 동일, o= 버전 불변) ─▶
         │  ◀── 200 OK  Session-Expires: 180;refresher=uac ──   ← lastRefreshTime 갱신
         │
         │   … 단말 강제종료 …
         │  ── re-INVITE ──────────────────────▶  ✗ 무응답
         │   (INVITE 트랜잭션 Timer B ≈ 32초)
         │  SendTimeout → EventCallEnd(SIP_GONE) → OnCallTerminated
         │  ── BYE (도달 불가, 규격 준수용) ─────▶
```

### 6.2 단말이 갱신하는 경우

CSP 는 아무것도 보내지 않고 in-dialog re-INVITE(또는 향후 UPDATE) 수신으로 `lastRefreshTime` 을
갱신한다. 만료 시각 `- min(32초, SE/3)` 에 도달하면 BYE 를 보내고 teardown 연쇄를 태운다(§10).

### 6.3 갱신 re-INVITE 규율

| 규율 | 이유 |
|---|---|
| 갱신·만료 요청은 **등록 바인딩(latch) 주소**로 보낸다 | 다이얼로그가 기억한 주소는 요청 **수신 당시의 소스**다. 단말은 큰 INVITE(multipart mcptt-info+SDP)를 TCP 로 승격해 보내는데, 그 연결은 곧 닫히고 NAT 뒤라 서버가 다시 열 수 없다 — 그 주소로 갱신을 보내면 도달하지 못하고 단말이 규격대로 세션을 끊는다(§10, `cause=408`). psip 은 요청 생성 직전 `EventGetLegDest` 로 현재 도달 주소를 응용에 묻고, 응답이 있으면 다이얼로그의 목적지·transport 를 그 값으로 갱신한다. Record-Route 가 있는(프록시 경유) 다이얼로그는 손대지 않는다 |
| SDP offer 는 직전과 **동일한 `o=` 세션 버전**으로 만든다 | RFC 4028 §7.4 의 "변경 없음" 표시. 현재 `CSipDialog::AddSdp()` 는 호출마다 `++m_iSessionVersion` 하므로 갱신 경로에서는 증가를 억제해야 한다 |
| 갱신 2xx 에는 `Session-Expires` 를 **항상 echo** 한다 | 빠지면 상대가 타이머 해제로 해석한다(§7.2). psip 의 re-INVITE 자동 200 OK 생성 지점(`SipUserAgentInvite.hpp` `RecvInviteRequest`)이 싣는다 |
| 수신 갱신에 대한 **answer 도 `o=` 를 유지**한다 | §7.4 는 answer 에도 "변경 없음" 표시를 요구한다 — 상대 offer 가 무변경일 때 answer 의 세션 버전도 올리지 않는다 |
| 수신 갱신 re-INVITE 는 **미디어 재협상으로 처리하지 않는다** | 선언 주소·코덱이 직전과 같으면 CMP `RELAY_MODIFY`/`PTT_JOIN` 재호출과 NAT latch 재평가를 생략한다(`CModuleDispatcher::EventReInvite`). 불필요한 latch 리셋은 NAT 뒤 단말의 하향 경로를 흔든다 |
| 갱신 수단은 당분간 **re-INVITE** | psip 은 UPDATE 를 구현하지 않는다(`SIP_METHOD_UPDATE` 부재). 다이얼로그 `Allow` 에도 UPDATE 가 없어 규격 준수 단말은 re-INVITE 를 쓴다([§12](#12-호환성리스크)) |
| 조건 상향 등 다른 목적의 in-dialog re-INVITE 도 **갱신으로 계산**한다 | §7.2 — 중복 갱신을 줄인다 |

## 7. 만료·실패 처리 = 기존 teardown 연쇄

새 계약을 만들지 않는다. 판정이 서면 **BYE 를 보내고 기존 종료 경로로 들어간다**.

```
 세션 만료 / 갱신 실패(timeout·408·481)
   → gclsUserAgent.StopCall(callId)          ← BYE (RFC 4028 §10)
   → EventCallEnd(callId, SIP_GONE)
   → CModuleDispatcher::EventCallEnd
        ├ CallMap 있음  : OnCallEnded → GroupCallService::OnCallTerminated → CallMap::Delete
        └ CallMap 없음  : GroupCallService::OnCallTerminated  (PTT 개시자 leg)
   → OnCallTerminated 내부
        ├ PTT_LEAVE (CMP 멤버 포트 반납) + 멤버 포트 캐시 무효화
        ├ private/ad-hoc 잔여 1 leg 종료 (min-participants)
        ├ 마지막 확립 leg 이면 PTT_GROUP_REMOVE + 세션 sesid 정리 + 녹취 세션 종료
        └ conference-info NOTIFY (RFC 4575, status=disconnected)
```

즉 **강제종료가 BYE 와 동일한 경로로 수렴**한다. 종료 사유만 구분해 기록한다([§11](#11-관측)).

## 8. 값과 지연

| 설정 키 (`Setup.Sip.SessionTimer`) | 기본 | 의미 |
|---|---|---|
| `Enable` | `true` | 세션 타이머 협상·감시 전체 스위치 |
| `SessionExpires` | `180` | 제안 세션 간격(초). 90 미만은 규격 위반이라 90으로 clamp |
| `MinSE` | `90` | 로컬 최소 간격 — 이보다 작은 요청은 422 + `Min-SE` |
| `Refresher` | `"server"` | `server`(우리가 갱신) / `ue`(단말이 갱신) — 규격상 뒤집을 수 없는 조합에서는 [§5.3](#53-refresher-결정) Table 2 가 우선한다 |

설정은 전역이다 — leg/서비스 단위 예외는 [§14](#14-후속-과제).

**감지 지연**

| refresher | 최악 감지 시점 | SE=180 기준 |
|---|---|---|
| 서버(CSP) | `SE/2` + INVITE 트랜잭션 Timer B(≈32초) | ≈ 122초 |
| 단말 | `SE − min(32, SE/3)` | ≈ 148초 |

실측은 이보다 빠를 수 있다 — 프로세스가 죽은 단말은 소켓이 닫혀 갱신 요청이 즉시 전송 오류로
떨어지므로 트랜잭션 수명을 다 기다리지 않는다 (실기기 강제종료 56초).

**부하** — leg 당 갱신 1회 왕복 / (SE/2). 40명 그룹·SE=180 이면 초당 약 0.44 트랜잭션(0.9 메시지)로,
CSP 의 정상 호처리량 대비 무시할 수준이다.

**권장값 이탈의 근거** — RFC 4028 §4 는 1800초를 권장하고 30분 미만을 SHOULD NOT 으로 둔다. 본
설계는 180초를 기본으로 삼아 이를 의도적으로 벗어난다. 근거는 ① 규격 자신이 "값은 응용
시나리오에 달렸다"며 MUST 가 아님을 명시한 점, ② MCPTT 는 상시 세션이 아니라 통화 단위 세션이고
유령 leg 이 잔여 1인 해제·로스터·미디어 포트를 즉시 붙드는 비용이 큰 점, ③ 폐쇄망 소규모 배치라
위 부하 산식이 성립하는 점이다. 값은 설정으로 노출하므로 대규모 배치에서는 상향할 수 있다.

## 9. 적용 범위

| leg | CSP 역할 | 적용 | 비고 |
|---|---|---|---|
| PTT 그룹 개시자 | UAS | O | `AcceptCall` 경로 — CallMap 미등록이라 종료는 `EventCallEnd` else 분기가 처리 |
| PTT 그룹 참여자(fan-out) | UAC | O | 기존 수동 `Session-Expires` 헤더를 본 설계로 대체 |
| private call (2인) | 양쪽 | O | **최대 수혜** — 잔여 1인 해제의 유일한 트리거를 보완 |
| ad-hoc | 양쪽 | O | 동일 |
| VoLTE A/B leg | 양쪽 | O | CMP relay sweeper(`hold_timeout` 600초)보다 빠르고, 시그널링 상태까지 정리 |
| MCData MSRP leg | UAS | O | `McDataMediaService` 의 `AcceptCall` 경로 |
| 제휴(IBCF) leg | 양쪽 | 전역 설정 | 제휴 규격·상대 정책이 우선한다. 현재 예외 축이 전역 스위치뿐이라, 제휴 연동 시 leg 단위 제외가 선행 과제다([§14](#14-후속-과제)) |
| cspsim | UAS/UAC | 대응 | 서버가 refresher 인 기본 구성에서는 **200 OK 응답만으로 동작**한다. 단말 refresher 검증에는 갱신 발신 기능이 필요 |

## 10. 구현 배치

세션 타이머 상태는 다이얼로그의 속성이므로 **psip(SIP UA 계층)** 에 두고, 정책·설정·서비스 연동은
**CSP** 가 갖는다. 이 분리로 cspsim 도 같은 규격을 공유한다.

| 컴포넌트 | 접점 | 내용 |
|---|---|---|
| psip | `SipUserAgent/SipUserAgentSessionTimer.hpp` | 세션 타이머 절차 일체 — 헤더 파싱(`Session-Expires`·compact `x`·`Min-SE`·옵션 태그 `timer`), 협상(요청/응답), `SetSessionTimer()`, `CheckSessionTimer()`, `IsSessionRefreshReInvite()` |
| psip | `SipUserAgent/SipDialog` | [§4](#4-상태-모델) 상태 필드 + `CreateInvite(bKeepSdpVersion)` / `AddSdp(msg, bKeepSdpVersion)` — 갱신 시 `o=` 세션 버전 유지 |
| psip | `SipUserAgentInvite.hpp` `RecvInviteRequest` | 초기 INVITE 협상 입력 보관 / re-INVITE = 갱신 인지(+미디어 무변경 판정) + 자동 200 OK 에 `Session-Expires` echo / SE < 최소치면 422 + `Min-SE` |
| psip | `SipUserAgentCall.hpp` `AcceptCall`·`CreateCall`, `SipUserAgent.cpp` `SendInvite` | 2xx 에 협상 결과, 송신 INVITE 에 `Supported: timer`·`Session-Expires`·`Min-SE` |
| psip | `SipUserAgent.cpp` `SetInviteResponse` | 2xx 수신 시 타이머 확정 / 422 는 `Min-SE` 반영 1회 재시도(§7.3) / 갱신의 408·481 은 세션 사망 표시(§10) |
| psip | `SipUserAgentSipStack.hpp` `SendTimeout` | 현행 유지 — 갱신 무응답이 곧 `EventCallEnd(SIP_GONE)` |
| psip | `SipUserAgentCallBack.h` `EventGetLegDest` (신규 콜백) | 서버 발신 in-dialog 요청의 현재 도달 주소를 응용에 묻는다. 기본 구현은 `false`(기존 동작 유지)라 다른 psip 사용자는 영향 없다. 콜백은 **다이얼로그 락 밖**에서 호출한다(psip 규약 — CheckSessionTimer 는 선별→조회→생성 3단계) |
| CSP | `ModuleDispatcher::EventGetLegDest` | 등록 단말이면 `UserMap` 의 latch (IP·포트·transport 한 세트)를 돌려준다 — fan-out INVITE·NOTIFY 가 쓰는 것과 동일 출처. 미등록(제휴 노드 등)이면 false |
| CSP | `SipServerSetup` | `Setup.Sip.SessionTimer` 설정 파싱([§8](#8-값과-지연)) |
| CSP | `ModuleDispatcher::Start` | UA 기동 직후 `SetSessionTimer()` 주입 |
| CSP | `CspServer.cpp` 주기 루프 | 1초 tick 에서 `gclsUserAgent.CheckSessionTimer()` 호출 |
| CSP | `ModuleDispatcher::EventReInvite` | `IsSessionRefreshReInvite()` 가 참이면 즉시 반환 — CMP 재호출·NAT 재평가 생략([§6.3](#63-갱신-re-invite-규율)) |
| CSP | `GroupCallService::InviteMember` | 수동 `Session-Expires: 7200;refresher=uac` + `Min-SE: 180` 헤더 제거 → 정본 경로로 통합 |
| CSP | `csp/config/config_template.json` | 콘솔 노출 필드 4종 (활성·간격·최소간격·갱신주체) |
| 단말 | pjsua 기본값 | `timerUse` 미설정 = pjsua 기본(OPTIONAL)이라 `Supported: timer` 광고와 서버 제안 수용이 기본 동작이다. **앱 변경 없이 동작하는지 실기기 확인 필요** |
| cspsim | 세션 타이머 | 서버 refresher 기본 구성은 무변경 동작(갱신 re-INVITE 에 200 OK 만 주면 된다) |

`CscfModule.h` 의 `SIP_ALLOW_METHODS` 는 UPDATE 를 광고하지만 psip 은 UPDATE 를 구현하지 않는다.
갱신 수단 선택은 다이얼로그 `Allow`(UPDATE 없음)를 보므로 실동작은 안전하나, 광고와 실제를
일치시키는 것은 [§14](#14-후속-과제) 로 남긴다.

## 11. 관측

| 자리 | 내용 |
|---|---|
| CSP 로그 | 협상 결과 1줄(`session timer: leg=<callId> se=<n> refresher=<local\|remote>`), 갱신 발사·수신, 만료/실패 판정(`session_timer_expired` / `session_refresh_failed`) |
| Flow 로그 | 갱신 re-INVITE 는 기존 SIP flow 에 그대로 남는다. 폭주 방지를 위해 갱신 성공은 DEBUG, 실패·만료만 INFO |
| 종료 사유 | CDR·PTT 세션 이력의 종료 사유를 BYE(`normal`)와 구분해 `timeout` 으로 남긴다 |
| FM 이벤트 | 세션 타이머 만료 teardown 은 운영자 모르게 통화가 끊기는 동작이므로 감사 이벤트로 보고한다 (`session_reclaimed` 계열과 동형 — [alarm_module_catalog.md](../alarm_module_catalog.md) 등록 대상) |

## 12. 호환성·리스크

| 항목 | 내용·대응 |
|---|---|
| 현행 fan-out INVITE 의 반쪽 상태 | `GroupCallService::InviteMember` 는 `Session-Expires: 7200;refresher=uac` + `Min-SE: 180` 을 광고하면서 **갱신을 전혀 수행하지 않는다**(`Supported: timer` 도 없다). 규격을 따르는 단말이라면 7200초 뒤 세션을 스스로 종료할 수 있다. 본 설계가 이를 대체한다 |
| `Allow` 광고 불일치 | CSCF 응답의 `SIP_ALLOW_METHODS` 에는 `UPDATE` 가 있으나 psip 은 UPDATE 를 구현하지 않고, 다이얼로그 `Allow`(psip)에는 UPDATE 가 없다. 갱신 수단 선택은 다이얼로그 `Allow` 를 보므로 실동작은 re-INVITE 로 안전하지만, 광고는 실제 지원과 일치시킨다 |
| 갱신 re-INVITE 가 단말 미디어를 흔들 위험 | `o=` 불변 + SDP 동일 규율로 최소화한다. 앱 재시작 레이스 등 기존 re-INVITE 관련 결함이 있었던 만큼 실기기 검증 항목에 포함한다 |
| 세션 타이머 미지원 단말 | 서버 refresher 기본값으로 커버된다. 단말은 평범한 re-INVITE 로 인지하고 200 OK 만 주면 된다 |
| 422 루프 | 상대 `Min-SE` 가 우리 제안보다 크면 상향 재시도는 **1회**로 제한하고, 그래도 실패하면 그 leg 은 타이머 없이 진행(로그 1줄) |
| CSP 재기동 | 다이얼로그 상태가 사라지는 것은 기존과 동일하다. 재기동 후 살아남은 단말 leg 은 갱신 대상에서 빠지므로, 기존 등록·재조인 복구 경로가 그대로 담당한다 |
| glare(양쪽 동시 갱신) | refresher 를 한쪽으로 고정하므로 구조적으로 발생하지 않는다. 조건 상향 re-INVITE 와 겹치면 후자를 갱신으로 계산해 회피한다 |

## 13. 검증

| 단계 | 항목 | 상태 |
|---|---|---|
| S1 | `clang-format` (변경분이 새 위반을 만들지 않을 것) | 통과 |
| S2 | 빌드 | 통과 |
| 루프백 | psip UA 를 UAS 로 띄우고 원시 UDP 소켓이 단말을 흉내내는 시험 — 라이브 서비스와 무관한 127.0.0.1 포트만 사용 | 아래 2 시나리오 통과 |
| S3 | `S3-SCN-PTT-SMOKE`·`S3-SCN-VOIP-SMOKE` 회귀 — 갱신 re-INVITE 가 스모크 호를 깨지 않을 것 | 미실시 |
| 실기기 | 아래 표 | **6항목 전부 통과** |

**루프백 시험 결과** (세션 간격 90초 = 규격 하한으로 시험)

| 시나리오 | 관측 |
|---|---|
| 서버가 갱신자 | 200 OK = `Session-Expires: 90;refresher=uas` + `Require: timer` / 갱신 re-INVITE 가 **+45초**(간격의 절반)에 `Supported: timer`·`Session-Expires: 90;refresher=uac` 로 도착 / **SDP `o=` 세션 버전 불변** / 단말 무응답 시 **+34초**(트랜잭션 수명)에 `EventCallEnd(408)` → teardown |
| 단말이 갱신자 (`refresher=uac` 요구) | 서버가 역할을 뒤집지 않고 `Require: timer` 동봉(§9 Table 2 MUST) / 갱신 미수신 시 **+60초**(= 90 − min(32, 30))에 **BYE 송신** + `EventCallEnd(408)` → teardown |

**실기기 검증** (PTT 앱 pjsua2, 사내 단말 2대 · SE=180)

| 항목 | 결과 |
|---|---|
| 협상 | 앱 INVITE = `Supported: replaces, 100rel, timer, …` + `Session-Expires: 1800` + `Min-SE: 90` → 서버 200 OK = `Session-Expires: 180;refresher=uas` + `Require: timer` (규격대로 값만 낮추고 갱신자는 서버) — **앱 변경 없이 동작** |
| 갱신 도달 | 개시 leg 은 앱이 TCP 로 승격해 보내므로 다이얼로그 주소가 곧 죽는다 → 등록 latch 로 목적지 교정(`…:27060(TCP) → …:22622(UDP)`) 후 정상 도달, 앱이 2xx 응답 |
| 통화 유지 | 갱신 3회(6 트랜잭션) 이상 통과, 5분+ 유지, `cause=408` 종료 0건 |
| **강제종료 감지 (그룹콜)** | 상대 단말 앱 force-stop(BYE 없음) → **56초** 만에 `SessionTimer expired` → `EventCallEnd(408)` → `PTT_LEAVE` → conference NOTIFY → 남은 단말 로스터 **구성원 (2)→(1)**, CMP `member_used` 반납 |
| **강제종료 감지 (private 1:1)** | 착신 단말 force-stop → 갱신 무응답(INVITE 재전송 10회) → Timer B 만료에 `SessionTimer expired` → `PTT_LEAVE` → **잔여 1인 해제**(`private(...) — 상대 leg 1 개 종료(BYE)`) → 남은 단말이 **BYE 에 200 OK 응답**(통화 정리 확증) → CMP 그룹 회수. 강제종료로부터 **75초** |
| **장시간 통화 무해성** | 6분간 갱신 **4회** 전부 성공. 단말 pjsip 이 매회 `received updated media offer` → `SDP negotiation done: Success` → **`stream #0 (audio) unchanged`** 로 처리 — 스트림을 재생성하지 않는다([§6.3](#63-갱신-re-invite-규율) `o=` 고정 규율의 효과). 갱신 시각의 오디오 스트림 재시작 0회, 코덱(AMR-WB sendrecv) 유지 |
| **망 소실 (기내 모드)** | 착신 단말 기내 모드 ON → 갱신 무응답 → **35초** 만에 `SessionTimer expired` → `PTT_LEAVE` → conference NOTIFY(`disconnected/deleted`) → 남은 단말 UI 가 **접속 중 1 / 오프라인 1** 로 갱신. 기내 모드 OFF 후 **47초**에 재등록(UDP latch 정상) + 자동 재조인으로 완전 복구 |

**순서 보장** — 잔여 leg 으로 나가는 teardown BYE 는 항상 **교정된 주소**로 간다. 죽은 leg 의
판정은 `SE/2 + Timer B`(≈122초)인데 살아있는 leg 의 첫 갱신은 `SE/2`(≈90초)라, teardown 이
발동하는 시점엔 그 leg 의 목적지가 이미 latch 로 교정돼 있다([§6.3](#63-갱신-re-invite-규율)).
private 실측이 이를 확인했다(살아남은 leg `…:47734(TCP) → …:22622(UDP)` 교정 후 BYE 도달).

## 14. 후속 과제

| 과제 | 내용 |
|---|---|
| UPDATE 지원 | RFC 4028 §7.4 는 상대가 지원하면 UPDATE 갱신을 권장한다. psip 에 UPDATE(RFC 3311) 를 구현하면 갱신이 SDP 없이 끝나 부하·재협상 위험이 함께 준다. 그 전까지 `SIP_ALLOW_METHODS` 의 UPDATE 광고를 실제 지원과 일치시킨다 |
| leg 단위 예외 | 현재 스위치는 전역 하나다. 제휴(IBCF) leg 처럼 상대 정책을 따라야 하는 구간을 leg 단위로 제외하려면 CSP 가 다이얼로그별로 타이머 사용 여부를 지정하는 API 가 필요하다 |
| 루프백 회귀 자동화 | 검증에 쓴 UAS↔원시 UDP 루프백 시험을 `tests/` 로 승격하면 협상·갱신·만료 3축이 회귀로 고정된다 |
| 미디어 평면 보조축 | 시그널링은 살아 있는데 **미디어만 죽는 경우**(NAT rebind·일방 무음)는 본 설계가 잡지 못하고, 감지도 SIP 축이 느리다. 설계 골자는 CMP 가 멤버별 `lastSeen`(floor Ack keepalive 15초 + RTP)을 유지하다 N회 누락 시 [api/cmp_media_api.md](../../api/cmp_media_api.md) §8 이벤트 채널로 멤버 단위 사망(`PTT_MEMBER_ABORTED`)을 통지하고, CSP 가 그 leg 만 종료하는 것이다(≈45초). **오탐 방지 규율 필수** — keepalive 를 실제로 관측한 멤버(연속 2회 이상)만 감시 대상에 넣어, keepalive 를 보내지 않는 단말을 죽었다고 판정하지 않는다 |
| 등록 계층 연동 | TCP/TLS 등록 단말은 전송 연결 종료가 즉시 신호가 된다(`CSipUserAgent::TcpSessionEnd`). 등록 소실과 활성 leg 정리를 연결하면 감지가 초 단위로 당겨진다 |
