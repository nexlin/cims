# 유선 VoIP 보조 서비스 — 데스크폰·소프트폰·관제 앱 (내선·당겨받기·호 전달)

> **설계 정본.** USIM 없는 유선 단말(데스크폰·소프트폰·관제 앱)이 **유선 VoIP 접속서비스**(`kind=voip`,
> [sip_service_model.md §2-9](sip_service_model.md))에 붙어 내선 라벨로 서로를 부르고, 당겨받기(call pickup)·호 전달
> (call transfer)·대표번호(전화 그룹, [dispatch_center.md](dispatch_center.md))를 쓰는 시나리오의 CSP 설계와 설정 규약.
> 당겨받기·대표번호·BLF 는 **유선 전화의 일반 기능**이며 관제 권한과 무관하다.
> **P0(미디어 정합 — RELAY_MODIFY·SRTP)·P1(그룹 축 — 픽업 그룹·서비스별 피처코드·전달 권한)·
> P2(표준형 — 수신 INVITE-Replaces·RFC 4235 dialog 이벤트 패키지·489)·P3(구조 — `CTasModule`
> 소유 이관)은 구현 반영, cspsim 시나리오 5종·S3 검증(`S3-SCN-XFER`/`PICKUP`/`DIALOG` — happy-path
> 미디어 재고정 + 그룹 경계 403/404·`transfer_allowed` 403·미지 Event 489 게이트)으로 `pickup_group`
> 실컬럼 축에서 실측 PASS.** 픽업 축의 org 폴백·전역 `CallPickupId` 는 제거됨(§5.1·§5.2). 유선 VoIP 접속환경 `kind=voip` 는 구현 반영(§10.2) —
> 이동 서비스로 만들어진 기존 회선의 `voip` 이관(H(A1) 재결박)은 §10.3a.
>
> 관련: [sip_access_security.md](sip_access_security.md)(인증), [sip_service_model.md](sip_service_model.md)
> (접속서비스), [media_security.md](media_security.md)(SRTP), [volte_flows.md](volte_flows.md)(기본 호 flow),
> [../api/cmp_media_api.md](../../api/cmp_media_api.md) §6(RELAY_MODIFY).

---

## 1. 범위와 전제

| 요구 | 결론 |
|---|---|
| USIM 없는 단말 | **이미 지원** — `auth_scheme=digest` + `sip_transport=TLS` + `ha1`. USIM 자료(k/opc) 불요. 신규 코드 없음, 규약만 고정(§3) |
| 내선번호 | **가입 id(AoR) = E.164**, 내선 = 표시 라벨(끝자리 N — `Provisioning.ExtensionDigits`)(§4). 서버 다이얼 플랜 없음 — 내선 확장은 단말이 한다. 별칭(내선을 망 주소로) 계층은 향후 과제(§9) |
| 당겨받기 | 기존 `PickUp()` 경로를 보완 — 그룹 축을 `pickup_group`(= 전화 그룹 id)으로 독립(§5), 미디어를 CMP 경유로 정합(§7) |
| 호 전달 | psip REFER(blind/attended) 위에 미디어 재고정을 `RELAY_MODIFY` 로 정합(§6, §7) |

표준 근거: 호 전달 = 3GPP TS 24.629(ECT) / RFC 3515(REFER) / RFC 3891(Replaces) /
RFC 4488(norefersub). 당겨받기는 3GPP MMTel 정의 밖의 PBX 계열 서비스로, 표준 관례는
RFC 4235(dialog event) + INVITE-with-Replaces 다. 본 설계는 1차로 **피처코드 방식**(기존
구현 계승)을 정식화하고, 표준형(Replaces 픽업·BLF)은 확장 단계(§8 P2)로 둔다.

---

## 2. 현재 구현과의 갭 (구현 단계의 입력)

모든 설계 갭이 해소됐다:
- **G1(미디어 재고정)** — 전달·픽업 전 경로가 원 통화의 relay 세션을 유지한 채 교체 leg 만
  `RELAY_MODIFY` 로 재고정하고 SRTP 를 유지한다(§6·§7, P0).
- **G2(픽업 그룹 축)** — 가입자별 `pickup_group` 으로 독립 축을 신설했다(§5.1, P1). org 폴백은 없다. `CUserInfo::m_strGroupId` 공급원 = `CspUser::EffectivePickupGroup()`.
- **G3(피처코드)** — 접속서비스별 `pickup_feature_code`(그룹/지정 픽업)로 이관, 전역 `Setup.Sip.CallPickupId` 는
  제거 (§5.2, P1).
- **G4(수신 INVITE-with-Replaces)** — `EventIncomingCall` 이 `Replaces`(RFC 3891)를 해석해 대상
  다이얼로그를 Call-ID+태그로 찾아(`CSipUserAgent::MatchReplacesDialog`) 픽업 재고정 코어
  (`PickUpLeg`)로 교체한다. 표준 attended 완결·BLF 클릭 픽업의 서버 수신부 (§6.2, P2).
- **G5(dialog 이벤트 패키지 + 489)** — `Event: dialog`(RFC 4235) SUBSCRIBE 분기·같은 픽업 그룹
  인가(403)·dialog-info NOTIFY(`SendDialogEventNotify`, 호 상태 변경 트리거)를 추가하고, 미지
  Event 를 489 Bad Event 로 교정했다(구 `else→gms` 오분류 제거) (§6.2, P2).
- **G6(O(n) 스캔)** — `UserMap` 에 픽업 그룹 인덱스를 두어 `SelectGroup` 전수 스캔을 제거했다
  (`SelectToRing` 은 그룹원 한정 조회라 유지, P1).
- **G7(실검증 공백)** — cspsim 에 `transfer`/`transfer_attended`/`pickup`/`dialog_pickup`/
  `subscribe_event` 시나리오와 `S3-SCN-XFER`/`S3-SCN-PICKUP`/`S3-SCN-DIALOG` 검증 항목을 추가했다.
  판정 정본 = 재고정 후 각 단말의 누적 수신 RTP delta(재고정된 relay 로 미디어가 흐르고 빠진
  단말은 무흐름) + 요청별 최종 응답 마커(`pickup_status`/`dialog_sub_status`/`refer_status`) —
  §8 검증 행.

INVITE 경로에 DB 질의를 넣지 않는다 — 모든 신규 판정(내선 해석·픽업 그룹·전달 권한)은
인메모리 맵(`gclsCspUserMap`/`CspServiceMap`)에서 답한다 (`ModuleDispatcher.cpp` 의
부하시험 사후분석 주석이 근거).

---

## 3. 신원 모델 — USIM 없는 유선 가입자

유선 단말은 **Digest over TLS** 가입자다. 지원 조합 표
([sip_access_security.md](sip_access_security.md) §1 "SIP Digest + TLS")의 현행 경로 그대로이며
신규 메커니즘이 없다. 규약만 고정한다:

| 필드 | 유선 규약 | 근거 |
|---|---|---|
| `id` (가입 id = AoR) | **E.164**(예: `+821310001001`) — 망 신원. 내선(`1001`)은 끝자리 라벨 | 이동 가입자와 같은 주소 공간이라 도메인 간 호·대표번호·연락처가 한 번호 체계로 통한다. 라벨 자릿수 = `Provisioning.ExtensionDigits` |
| `imsi` | 번호 숫자(USIM 없음 규약 — 관제 앱 관리 API 가 비어 있으면 채운다) | Digest 경로에서 imsi 는 AKA 와 무관한 "IMPI user part" — Digest username = `imsi@<domain>`, ha1 도 이에 묶임 |
| `auth_scheme` | `digest` (기본값) | AKA 아님 = USIM 불요 |
| `sip_transport` | `TLS` | 채널 정책 게이트가 평문 유입을 403 차단 (A-SEC-003). 서비스 `sec_mechanisms=[tls]` |
| `service_ref` | 유선 VoIP 접속서비스 name (`voip`, §10.2) | 비면 REGISTER 거부 |
| `pickup_group` | 전화 그룹 id — 멤버십에서 파생(§5.1, [dispatch_center.md §3.2](dispatch_center.md)) | |

**식별자 모델과의 관계** ([identifier_model.md](../identifier_model.md)): 가입 id(E.164)는 **다이얼 가능한
주소 = id 축**이고, 내선은 그 끝자리를 잘라 보여 주는 **표시 라벨**이다(어떤 키에도 쓰지 않는다). 표시 이름은
`users.name` 이 담당한다. 번호 변경은 rename 이 아니라 재키잉(구독 삭제·재생성 + H(A1) 재결박)이며, 이는
전화망의 일반 관례와 같다.

## 4. 내선 다이얼링

서버에는 **다이얼 플랜 계층이 없다.** INVITE 의 To user 는 항상 E.164 이고 기존 `EventIncomingCall` 의
`gclsCspUserMap.isAlive(pszTo)` 조회가 그대로 맞는다. 내선 다이얼(짧은 번호로 걸기)은 단말이 한다 — 관제 앱의 빠른
발신 줄·그룹원 띠는 프로비저닝이 내려준 `extension` 라벨을 같은 전화 그룹/조직의 E.164 로 확장해 건다
([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.3). 데스크폰은 단축 다이얼 또는 전화번호부로 대신한다.

내선을 망 주소(별칭)로 받아야 하는 요구가 생기면, 그때 별칭 인덱스를 `CspUserMap` 에 두고(`isAlive` 직전 정규화,
인메모리) CSC 에 SoT 컬럼을 신설한다 — routing-policy 의 예약 필드 `transform_rule_set_refs` 를 이 용도로 실체화하는
방안 포함. 본 설계 범위 외(§9).

---

## 5. 당겨받기 (Call Pickup)

### 5.1 그룹 축 — `pickup_group`

픽업 대상 판정 축은 **가입자별 `pickup_group`** 이고, 그 값은 **전화 그룹 id**([dispatch_center.md §3.1](dispatch_center.md))다.

- SoT: `volte_subscriptions.pickup_group VARCHAR(64) NULL` / `ptt_subscriptions.pickup_group`(§10.1) — 값은 전화 그룹 멤버십에서
  CSC 가 파생한다(person 단위, 직접 편집 409).
- CSP: `CspUser::m_strPickupGroup` → 등록 시 `CUserInfo::m_strGroupId`. **비어 있으면 그 회선은 어떤 픽업·BLF 축에도 속하지
  않는다** — `org_id` 폴백은 두지 않는다(같은 조직이라는 사실만으로 남의 호를 당겨받거나 dialog 를 구독할 수 없다).
  전화 그룹이 없던 현장의 org 단위 사용례는 같은 값의 전화 그룹을 만들어 승격한다([dispatch_center.md §8.1](dispatch_center.md)).

구현: `CspUser::EffectivePickupGroup()` 은 `pickup_group` 만 돌려주고, `CTasModule::PickUp` 은 픽업자의 축 값이 비면 404 로
끊는다(지정 픽업의 "같은 그룹" 비교가 빈 값끼리 참이 되지 않게). `CUserMap` 그룹 인덱스는 빈 값을 넣지 않는다.

### 5.2 피처코드 — 서비스별 + 지정 픽업

피처코드는 **접속서비스 필드 `pickup_feature_code`**(§10.2) — 도메인의 번호계획이다. 판정은 발신 가입자의
접속서비스(`CspServiceMap::GetForUser`)에서 읽는다. 유선 VoIP 서비스에만 두고 이동 VoLTE 서비스는 비워 둔다(픽업 비활성).

| 다이얼 | 의미 |
|---|---|
| `<code>` (정확일치, 예: `**`) | **그룹 픽업** — 내 `pickup_group` 에서 링 중인 아무 호 |
| `<code><번호>` (prefix, 예: `**+821310001003`, 앱은 내선 라벨을 확장해 보낸다) | **지정 픽업** — 해당 가입자에 링 중인 호만. 같은 `pickup_group` 소속일 때만 허용 |
| 코드 빈 문자열 | 그 서비스에서 픽업 비활성 |

전역 `Setup.Sip.CallPickupId` 는 **없다** — 서비스 필드가 없는 레거시 폴백이었고 이동 VoLTE 가입자까지 픽업을 여는 부작용이
있어 제거했다(`SipServerSetup`·config_template `sections.tas`·render 에서 삭제. 기존 csp.json 의 값은 무시된다).

### 5.3 픽업 flow (미디어 경로는 현행 — 그룹 축·피처코드 축은 §8 P1)

```
UE-A ──INVITE──► CSP ──INVITE──► UE-B (링 중, CMP relay 할당됨: peer0=A, peer1=B)
                                  UE-C (B 와 같은 전화 그룹)
UE-C ──INVITE **──► CSP
                    │ [발신자 서비스의 pickup_feature_code 매칭]
                    │ [pickup_group=C 의 그룹에서 링 중 leg 탐색 — 그룹 인덱스, 실패 시 다음 후보]
                    │ [B leg StopCall(487), CallMap 재키잉: B leg → C leg]
                    │ ── RELAY_MODIFY (peer_index=1, C 의 SDP 주소·crypto) ──► CMP
 UE-C ◄── 200 OK ── │   SDP: 기존 relay 포트 (A 에게 광고된 포트 불변)
 UE-A ◄── (통화 성립 — A 재INVITE 불요, relay 포트 유지) ──► UE-C
```

핵심 계약: **A 에게 이미 광고된 relay 포트는 불변**이고, 바뀌는 것은 CMP 의 peer1 목적지뿐
— 이것이 `RELAY_MODIFY` 의 정의("변경은 MODIFY, 재생성 금지")와 일치한다. SRTP 는 C leg 의
crypto 를 MODIFY 에 실어 재수립한다.

실패 응답: 그룹에 링 중 호 없음/지정 대상 링 없음 → 404, 픽업 단말 offer 의 SDES 협상 불가
→ 488(원 호 무영향), 픽업 비활성 → 다이얼 플랜상 일반 호 처리(= 미등록 내선이므로 404),
타 그룹 지정 픽업 → 403.

---

## 6. 호 전달 (Call Transfer)

psip 은 이미 REFER 수신·발신·`Event: refer` NOTIFY 진행보고를 완비하고 있고
(`SipUserAgentRefer.hpp`), CSP 는 B2BUA 로서 REFER 를 자신이 종단한다(규격상 올바른 위치).
보완은 두 갈래다.

### 6.1 Blind transfer (무확인 전달) — 미디어 정합

```
A ◄──통화중──► CSP(B2BUA)+CMP ◄──통화중──► B
                    │ ◄── REFER (Refer-To: sip:C@domain) ── B
                    │ ── 202 Accepted ──► B
                    │ ── NOTIFY(100 Trying / 180 / 200) ──► B   (기존 구현 유지)
                    │ ── INVITE ──► C     (SDP: 기존 relay 의 B측 leg 포트 재사용)
                    │ ◄── 200 OK ── C
                    │ ── RELAY_MODIFY (peer_index=B측, C 의 주소·crypto) ──► CMP
                    │ ── BYE ──► B
A ◄══ RTP (relay 포트 불변) ══► C
```

`EventBlindTransfer` 는 신규 relay 를 만들지 않는다 — 전환 대상은 REFER 지시자 leg 의
peer index·relay 포트를 승계하며(신규 leg SDES offer 상태만 trans entry 에 보관), 완결은
`EventCallStart` 의 trans 분기가 answer SDES 검증(실패 시 전환만 중단·원 통화 유지) 후
`CmpClient::ModifySession(sessionId, …, peer_index)` 로 재고정한다. leg 별 crypto 재작성은
VoLTE relay SDES 종단 경로([media_security.md](media_security.md) §5)를 그대로 재사용하고,
신규 leg 서버 키는 새로 생성한다(떠나는 단말에 알려진 키 재사용 금지). C 거절 시 원 통화가
그대로 남는다(신규 relay 가 없으므로 회수할 것도 없다).

### 6.2 Attended transfer (확인 전달)

- **Refer-To 내 Replaces** (psip 기해석): `EventTransfer` 가 원 통화의 relay 를 유지하고
  합류 단말을 승계 index 로 `RELAY_MODIFY` 재고정, 상담 통화의 relay 는 회수한다. screened
  는 합류 단말의 기존 SDES leg 키를 이관(재키잉 없음), unscreened 신규 leg 는 §6.1 과 동일한
  신규 서버 키 offer — answer 재고정은 `EventCallStart` 정상 경로가 수행한다(entry 의
  `m_bRecv`=peer0 표식으로 answer leg index 를 일반화 — 재결합 pair 는 남는 쪽이 peer1 일 수
  있다).
- **수신 INVITE-with-Replaces** (G4, 구현): `EventIncomingCall` 최상단에서
  `HandleIncomingReplaces` 가 `Replaces` 헤더(RFC 3891, 미이스케이프 `call-id;to-tag;from-tag`)를
  파싱해 대상 Call-ID 를 `CallMap` 에서 찾고 psip `MatchReplacesDialog`(Call-ID 키 + 태그 양방향
  대조, 태그 미전달 시 Call-ID 만으로 허용)로 확인한 뒤, 픽업 재고정 코어 `PickUpLeg` 로 그 leg 를
  새 INVITE 로 교체한다(재키잉 + `RELAY_MODIFY`, §5.3 과 동일). 대상 부재/태그 불일치 481, 같은
  픽업 그룹 아니면 403(무단 가로채기 방지). 표준형 attended transfer 완결이자 BLF 클릭 픽업의
  서버 수신부다.

### 6.3 권한

전달 허용은 접속서비스 플래그 `transfer_allowed`(§10.2)로 게이트한다. 불허 서비스 가입자의
REFER 는 403. 기본 true(기존 동작 보존).

---

## 7. CSP↔CMP 계약 — 재고정은 RELAY_MODIFY 하나로

[cmp_media_api.md](../../api/cmp_media_api.md) §6.2 가 이미 정의한 계약을 그대로 쓴다. 신규
CMP 명령 없음.

| 상황 | 명령 |
|---|---|
| 픽업 — 링 중 leg 를 픽업 단말로 교체 | `RELAY_MODIFY` (해당 `peer_index`, 새 remote 주소·NAT·crypto) |
| 전달 — 한쪽 leg 를 전달 대상으로 교체 | `RELAY_MODIFY` (동일 — 원 통화 relay 유지, 상담 통화 relay 는 `RELAY_REMOVE`) |
| 세션 자체가 없던 경우 (전달 중 신규 leg) | `RELAY_ADD` 로 생성 후 상대 확정 시 `RELAY_MODIFY` (기존 계약) |

금지: 포트 산술로 상대 relay 포트를 추정하는 것, MODIFY 상황에서 ADD 재생성(광고된 SDP 와
모순). 픽업·전달 후에도 세션 종료는 기존대로 `session_id` 기준 `RELAY_REMOVE`.

---

## 8. 구현 구조

P0(미디어 정합 — `RELAY_MODIFY`·SRTP)·P1(관제 축 — 픽업 그룹·서비스 플래그·그룹 인덱스)·
P2(표준형 — 수신 INVITE-Replaces·dialog 이벤트 패키지·489)·P3(구조 — 모듈 이관)은 모두
구현 반영이다 (§5·§6·§7·§10).

**모듈 구조 (P3)**: 보조 서비스 로직은 **`CTasModule` 소유**다 — `ModuleDispatcher` 는 B2BUA
골격(라우팅·relay 수명)만 유지하고, 각 이벤트 시점에 `IModule` 훅으로 위임한다
(TAS 역할 off 시 보조 서비스 전체 비활성 — 모듈 게이트).

| CTasModule 진입점 | 담당 |
|---|---|
| `OnSipRequest` | REFER 게이트 — `transfer_allowed=false` 403 (§6.3) |
| `OnIncomingCall` | 수신 INVITE-Replaces(RFC 3891) → `PickUpLeg` 교체 (§6.2) |
| `OnCallRing` / `OnCallStart` / `OnCallEnd` | dialog-event early/confirmed/terminated 통지 (§6.2) + blind transfer 진행 NOTIFY·완결(재고정·재결합)·실패 정리 (§6.1) |
| `OnTransfer` / `OnBlindTransfer` | attended / blind transfer (§6) |
| `ScreenInvite` | RecvRequest INVITE 조기 스크린 — DND/착신거부 603 (다이얼로그 생성 전) |
| `TryPickupDial` | 미등록 착신의 픽업 피처코드 판정·수행 (§5.2) |
| `ApplyTerminationServices` | 착신 가입자 DND/착신거부 603·착신전환 302 |

relay leg SDES 평가/재작성 헬퍼(`EvalRelayOfferSdes`/`ApplyRelayLegOffer`/`EvalRelayAnswerSdes`/
`ReadReinviteSdes`/`RewriteRelaySdpForLeg`)는 `MediaSdes` 네임스페이스에 있고 디스패처(B2BUA
정상 경로)와 TAS(픽업·전달 재고정)가 공용한다 ([media_security.md](media_security.md) §5.2).

**P2 상세**: (a) 수신 INVITE-with-Replaces — `HandleIncomingReplaces`(§6.2). (b) dialog 이벤트
패키지 — `CscfModule` SUBSCRIBE 에 `dialog` 분기 + 같은 픽업 그룹 인가(403), `CspServer` 에
dialog-info NOTIFY 빌더·발신(`SendDialogEventNotify`), `CTasModule` 의 `OnCallRing/Start/End`
에서 호 상태(early/confirmed/terminated) 트리거. picker 는 NOTIFY 의 `call-id`(+태그)로
INVITE-Replaces 를 조립한다. 본문 규칙(RFC 4235 §4.1) — 호의 두 당사자 각각에게 **그 당사자가 가진
dialog** 를 낸다: `id`/`call-id` = 그 당사자 쪽 CSP leg Call-ID(발신자 = A-leg, 착신자 = B-leg —
picker 는 착신자의 것을 Replaces 로 쓴다), `local` = entity 자신, `remote` = 상대 leg 의 원단 사용자,
`direction` = 그 당사자가 INVITE 를 보냈으면(CSP 수신 leg) `initiator`, 아니면 `recipient`. 초기 full
스냅샷(`CollectInitialDialogs`)도 같은 규칙이다. 당사자·개시 방향은 `CallLegParty`
(`CCallMap::ResolveLegParties` — 당사자 = `GetToId`(leg 원단, 송·수신 무관), 개시자 = psip `IsSendCall`
거짓)에서만 해석한다 — psip dialog 의 From/To 는 "CSP 가 요청을 보내는 입장" 으로 저장돼 수신 leg 에서는
From=다이얼된 번호·To=발신자로 뒤집혀 있어, 이를 caller/callee 로 읽으면 발신자 BYE(A-leg) 때 두 당사자가
바뀐다. (c) 미지 Event → 489 Bad Event(구 `else→gms` 오분류 제거). BLF
클릭 픽업 = dialog 구독으로 링잉 leg 를 알고(G5) 그 leg 를 Replaces 로 가져온다(G4).

검증(구현 반영, G7): cspsim VoLTE 시나리오 — `transfer`(A→B 후 A blind REFER→C),
`transfer_attended`(A→B + A→C 상담 후 attended REFER), `pickup`(A→B 링잉 중 C 가 `-pickup_code`
[+`-pickup_target <내선>`=지정 픽업] 다이얼), `dialog_pickup`(C 가 B 를 dialog 구독 → 링잉 NOTIFY →
C INVITE-Replaces), `subscribe_event`(등록 후 `-event <token>` 으로 자기 AoR SUBSCRIBE 1건) — 는
3 단말(A,B,C)을 세우고 시나리오 말미에 각 단말의 누적 수신 RTP delta 와 요청별 최종 응답 마커
(`pickup_status=`·`dialog_sub_status=`·`refer_status=`·`SUBSCRIBE-EVENT result: … status=`)를
출력한다(`SimSession::RecvPackets` — RtpThread 수신 카운터, `EventCallEnd`/`EventTransferResponse`/
SUBSCRIBE 응답 캡처). 착신/재-INVITE leg 는 UAS 경로라 `m_bInCall` 플래그가 신뢰되지 않으므로
**RTP delta 가 미디어 판정 정본**이다: 전달 후 살아남는 두 단말(B·C)로 미디어가 흐르고 빠진
단말(전달자 A / 픽업 대상 B)은 무흐름이면 재고정 성공.

S3 항목은 같은 org VOIP 가입자 4명(A,B,C,D)을 DB 에서 고르고, `pickup_group` 컬럼이 있으면
A,B,C 에 같은 그룹·D 에 다른 그룹을 명시 부여(DB UPDATE + CSP `USER_CHANGED` → `ReloadFromDb`)해
**실컬럼 축**으로 판정한 뒤 원값을 복원한다(자기복원). 컬럼이 없으면(마이그레이션 미적용) 전원
org 폴백이라 happy-path 만 판정하고 그룹 경계 검사는 SKIP 으로 보고한다.

| 항목 | 검사 | 판정 |
|---|---|---|
| `S3-SCN-XFER` | X1 blind 전달 / X2 attended 전달 | B·C 미디어, A 드롭 (X1 은 `refer_status` 2xx) |
| | X3 `transfer_allowed=false` | A 의 `service_ref` 를 S3-SEED 시드 `volte-noxfer`(같은 도메인, `transfer_allowed=false`)로 플립 → REFER 403, 원 통화 A·B 미디어 유지·C 무흐름 |
| `S3-SCN-PICKUP` | P1 그룹 픽업 `**` / P2 지정 픽업 `**<B>` | A·C 미디어, B 무흐름, `pickup_status=200` |
| | P3 타 그룹 지정 픽업 / P4 그룹 밖 그룹 픽업 | D(다른 `pickup_group`, 같은 org) → 403 / 404, 재고정 없음 (컬럼 축 한정) |
| `S3-SCN-DIALOG` | D1 dialog NOTIFY / D2 Replaces 재고정 | C 구독 200 + NOTIFY ≥1, A·C 미디어·B 무흐름 |
| | D3 그룹 밖 감시 / D4 미지 Event | D 의 B dialog 구독 → 403·NOTIFY 0 (컬럼 축 한정) / `Event: cims-verify-bogus` → 489, 대조 `Event: dialog` 자기감시 → 200 |
| | D5 미등록 SUBSCRIBE Digest 수락 | PTT 가입자가 REGISTER 없이(`-no_register`) 자기 AoR dialog 구독 → 401 의 realm = 요청자 서비스 realm(ptt, volte 폴백 아님) → Digest 재전송 → 200 ([csp.md](../modules/csp.md) 구독 절 — 등록 아닌 신원 인가) |

피처코드는 S3-SEED 가 volte 접속서비스에 `pickup_feature_code="**"` 를 시드해 **서비스 필드
경로**(전역 `CallPickupId` 폴백 아님)를 태운다.

```bash
./cims-verify run --items S3-SEED,S3-SCN-XFER      # X1/X2 전달 + X3 transfer_allowed 403
./cims-verify run --items S3-SEED,S3-SCN-PICKUP    # P1/P2 그룹·지정 픽업 + P3/P4 그룹 경계 403/404
./cims-verify run --items S3-SEED,S3-SCN-DIALOG    # D1/D2 BLF + D3 그룹 밖 감시 403 + D4 미지 Event 489
```

---

## 9. 범위 외 / 향후 과제

- 내선을 망 주소로 받는 별칭 계층 (§4) — `transform_rule_set_refs` 실체화 후보.
- 호 보류(hold)/파킹(park) — 별도 설계. 그룹 착신(대표번호 병렬 호출)·전화 그룹 엔티티·통화 감청은
  [dispatch_center.md](dispatch_center.md) 가 설계 정본이다(`pickup_group` 값이 전화 그룹 id 로 파생된다).
- Android UE 의 REFER 발신·픽업 UI — 서버 완성 후 단말 파트.
- `listener_id` 전파의 TCP/TLS 확장 — 관제 전용 listener 를 `inbound_policy=restricted` 로
  울타리 치려면 필요 ([sip_service_model.md](sip_service_model.md) §9 갭). 그 전까지 유선
  서비스 격리는 domain·가입자 `service_ref` 로만 성립한다.

---

## 10. 설정 정리 (운영 규약)

### 10.1 DB 스키마 (신설 1컬럼)

```sql
-- sql/migrate_subscription_pickup_group.sql (재실행 안전 — 컬럼 존재 시 no-op)
ALTER TABLE volte_subscriptions ADD COLUMN pickup_group VARCHAR(64) NULL DEFAULT NULL;
ALTER TABLE ptt_subscriptions   ADD COLUMN pickup_group VARCHAR(64) NULL DEFAULT NULL;
```

`NULL`/빈 값 = 기존 동작(조직 축 폴백). JSON fallback(`csp/User/<id>.json`)에는 `pickup_group`
키로 대응한다. **컬럼 미적용 DB 에서도 안전**하다 — CSP `DbManager` 가 부팅 시 컬럼을 프로브해
없으면 SELECT 에서 생략(전원 org 폴백, INFO 로그), CSC 도 `pickup_group` 을 응답에서 빼고 그
키를 담은 프로비저닝 요청은 400(`schema_not_migrated`)으로 거절한다. CSP 반영은 다음 REGISTER
갱신부터(등록 바인딩 스냅샷).

### 10.2 접속서비스 (`access_services` — 콘솔 관리>설정, `config/access_services.jsonl`)

유선 단말은 **유선 VoIP 접속서비스 `kind=voip`** 에 붙는다([sip_service_model.md §2-9](sip_service_model.md)). CSP 는
`volte`·`voip` 를 같은 전화 경로로 다루고, 다른 것은 이 레코드의 정책 필드다. 이동 VoLTE 서비스에는 피처코드를 두지 않는다.

서비스 필드(config_template `access_services` collection — 콘솔 편집 UI 자동 노출):

| 필드 | 타입/기본값 | 의미 |
|---|---|---|
| `pickup_feature_code` | string, `""` | 이 서비스의 당겨받기 코드. 빈 값=비활성. `<code>`=그룹 픽업, `<code><번호>`=지정 픽업 (§5.2) |
| `transfer_allowed` | bool, `true` | REFER(호 전달) 허용 — 도메인 기본값 (§6.3) |

유선 VoIP 서비스 권장 레코드:

```json
{ "name": "voip", "kind": "voip", "enabled": true,
  "domain": "voip.cims.example.kr",     // 유선 도메인 — Digest username = <imsi>@voip.cims.example.kr
  "auth_realm": "",                     // 비움 = domain 상속
  "inbound_policy": "any",              // restricted 는 TLS 에서 무효(§9) — 사용하지 않는다
  "sec_mechanisms": ["tls"],            // ipsec-3gpp 불필요 (AKA 가입자 없음)
  "media_srtp": "required",             // 사내망 권장 — TLS 강제와 결합 (media_security.md §4)
  "media_nat_mode": "off",              // 사내망 전제. NAT 구간이 있으면 auto
  "latch_ip_guard": "strict",
  "pickup_feature_code": "**",
  "transfer_allowed": true,
  "priority": 100 }
```

### 10.3 가입자 프로비저닝 (CSC `POST /users/{pid}/call`)

유선 단말 1대 = 가입 1건. 규약(§3)을 그대로 payload 로:

```json
{ "id": "+821310001002",      // E.164 = AoR (내선 라벨 1002 는 끝자리)
  "imsi": "821310001002",     // 규약: 번호 숫자 (Digest username = 821310001002@voip.cims.example.kr)
  "passwd": "…",              // CSC 가 ha1 로 파생 저장 (평문 미보관)
  "service_ref": "voip",
  "sip_transport": "TLS",
  "auth_scheme": "digest",    // 기본값 — 생략 가능. k/opc 없음
  "dnd": false, "forward_id": "" }
```

`pickup_group` 은 payload 에 두지 않는다 — 전화 그룹 멤버십(`/api/v1/phone-groups/{id}/members`)에서 파생되며 직접 지정은
409 다([dispatch_center.md §3.2](dispatch_center.md)). 반영은 기존 `USER_CHANGED` UDP 통지 경로. 표시 이름은 `users.name`,
내선 라벨은 프로비저닝 `extension`.

### 10.3a 기존 회선의 `voip` 이관 (H(A1) 재결박)

이동 서비스(`service_ref=volte`)로 만들어진 유선 회선(관제석 등)을 `voip` 서비스로 옮기는 절차. Digest 자료 H(A1) =
MD5(`<imsi>@<domain>:<realm>:<pw>`) 는 **서비스 도메인·realm 에 묶여** 있어, 도메인이 다른 서비스로 `service_ref` 만 바꾸면
REGISTER 가 403 이 된다 — CSC 는 평문을 보관하지 않아 재파생할 수 없다([sip_access_security.md §4.3·§4.7](sip_access_security.md)).

1. 접속서비스 `voip` 레코드 생성(§10.2) → CSP 반영(콘솔 저장 = agent 반영 + reload) — OAM 이 관리 store 미러를 따라 갱신하고
   CSC 는 그 미러에서 정의(domain·realm·SRTP·sec-agree·피처코드)를 읽는다. csc.json `Provisioning.Services.voip` 에는 단말 도달
   정보(host·포트·transport)만 채운다(`name` 을 레코드와 같게 — [sip_service_model.md §2-9](sip_service_model.md)).
2. 회선마다 `PUT /users/{pid}/call/{msisdn}` 로 `service_ref="voip"` **와 `passwd` 를 함께** 보낸다 — CSC 가 새 도메인·realm 으로
   H(A1) 을 다시 파생해 저장하고 CSP 에 `USER_CHANGED` 를 보낸다. `service_ref`(또는 `imsi`)가 바뀌는데 `passwd` 가 없으면 400
   (결박 변경 = 재입력 필수). 콘솔 가입자 회선 편집·관제 앱 [관리] 도 같은 규칙이다.
3. 전화 그룹의 `service_ref` 를 `voip` 로 바꾼다(`PUT /api/v1/phone-groups/{id}`) — 대표번호 포크의 도메인이 이 값을 따른다.
4. 단말은 재로그인(재프로비저닝) — `/provisioning/me` `services[]` 에 `kind=voip` 항목이 내려오고 관제 앱은 그것을 전화 회선으로
   잡는다([android_ue_provisioning.md §3](android_ue_provisioning.md)). 구 프로파일을 캐시한 단말은 옛 도메인으로 REGISTER 해
   403 을 받으므로 이관은 정지창에 회선 단위로 한다.

**같은 도메인 배치(전환기)**: `voip` 레코드를 volte 와 **같은 도메인**(priority 150 — `GetByDomain` 의 1순위를 빼앗지 않는다)으로
두면 H(A1) 이 그대로라 재결박·단말 재로그인 없이 `service_ref` 만 옮길 수 있다. 정책 필드(피처코드·전달·SRTP)는 `service_ref` 로
붙으므로 도메인이 같아도 유선 정책은 voip 레코드를 따르고, 프로비저닝 `services[].kind` 도 voip 다. dev 검증 스택이 이 배치로
시드하며(`verify/lib/common/access_services.py`), 라이브도 이 배치로 먼저 옮긴 뒤 도메인 분리(§10.2)는 도메인 간 대표번호
포크(`S3-SCN-FA`)·관제 앱 SRTP 실기 확인 후 정지창에서 위 1~4 로 마무리한다. 관제석이 NAT 뒤면 `media_nat_mode=auto` 를 유지한다.

### 10.4 csp.json / 템플릿 (`sections.tas`)

| 키 | 처분 |
|---|---|
| `Setup.Sip.CallPickupId` | **없음(제거됨)** — 피처코드는 접속서비스 `pickup_feature_code` 만(§5.2). 기존 csp.json 의 값은 무시된다 |
| `Setup.Sip.SessionTimer.*` | 그대로 — 관제 소프트폰도 세션 타이머 대상 |
| `Setup.Sip.StaleCallTimeout` | 그대로 |

신규 전역 키 없음 — 픽업·전달의 정책은 전부 접속서비스(§10.2)와 전화 그룹 멤버십 레벨이다.

### 10.5 Local Node / 단말

- LocalNode 는 기존 TLS listener(5061) 재사용 — 유선 전용 listener 를 추가해도 되지만
  `restricted` 귀속은 TLS 에서 성립하지 않으므로(§9) 격리 근거로 삼지 않는다.
- 단말(소프트폰) 설정: SIP TLS, Digest(내선/비밀번호), SRTP(SDES) — 능력 선언
  `Security-Client: sdes-srtp` + `mediasec` ([media_security.md](media_security.md) §3).
  프로비저닝 프로파일은 CSC `Provisioning.Services.<kind>` 경로 재사용
  ([android_ue_provisioning.md](android_ue_provisioning.md)).

### 10.6 검증 진입점

```bash
./cims-verify run --items S3-SEED,S3-SCN-XFER      # X1/X2 blind·attended 전달 + X3 transfer_allowed=false → REFER 403
./cims-verify run --items S3-SEED,S3-SCN-PICKUP    # P1/P2 그룹·지정 픽업 + P3/P4 타 그룹 403 / 그룹 밖 404
./cims-verify run --items S3-SEED,S3-SCN-DIALOG    # D1/D2 dialog NOTIFY·Replaces + D3 그룹 밖 감시 403 + D4 미지 Event 489
```

세 항목은 같은 org VOIP 가입자 4명을 DB 에서 골라 cspsim 3 단말 시나리오를 돌리고, 각 단말의
누적 수신 RTP delta 와 요청별 최종 응답 마커로 판정한다(§8 검증 표). 그룹 경계 검사(P3/P4/D3)는
`pickup_group` 컬럼에 값을 명시 부여해 실컬럼 축으로 보며(자기복원), 컬럼이 없는 DB 에서는 SKIP
으로 보고한다. `S3-SEED` 가 선행되어야 한다 — volte 서비스의 `pickup_feature_code` 와 X3 용
`volte-noxfer` 서비스를 시드한다.

---

## 11. 문서 갱신 대상 (구현과 같은 변경에서)

- [volte_flows.md](volte_flows.md) — C1 "Proxy 모드" 및 부록 판정 트리 현행화(현재 전 VoIP
  INVITE 가 B2BUA+CMP), 픽업·전달 케이스 추가.
- [sip_service_model.md](sip_service_model.md) §2-9 — `pickup_feature_code`/`transfer_allowed` 행.
- [../db_schema.md](../db_schema.md) — `pickup_group` 컬럼.
- [../../api/admin_api.md](../../api/admin_api.md) — 구독 payload `pickup_group`.
