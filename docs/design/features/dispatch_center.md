# 관제 센터 — 관제 그룹·대표번호 병렬 호출·업무망 합법감청

> 관제 센터(소프트폰·관제용 앱) 요구 세 가지를 수용하는 CSP/CMP/CSC 설계와 설정 규약. 요구 = ① 업무망
> 통화를 선택해 합법감청(운영자 인가 기반 감독 청취), ② 관제센터로 걸려오는 전화를 N 명의 관제사가 선택적으로
> 수신(그룹핑), ③ 관제센터 그룹을 N 개 생성.
>
> **구현 상태**: ③ 관제 그룹 모델(CSC API·DB·콘솔·CSP 인메모리 맵), ② 대표번호 호출(parallel/sequential
> alerting·포크 집합·승자 확정·CANCEL·무응답·overflow·링잉 대표번호 호의 당겨받기), ① 업무망 합법감청(RFC 3911
> Join → CMP 청취 leg tap, dialog 인가 범위 §5.2, SSRC 2개 분리 인도·은닉), PTT 그룹콜 청취(§5.6 — recvonly
> 합류·2단 인가·floor 거절·로스터 은닉/공개), 감사 E-AUD-016 + 콘솔 감사 이력 화면(§5.7)까지 서버·콘솔·검증
> (`S3-SCN-FA`/`S3-SCN-MONITOR`/`S3-SCN-PTT-LISTEN`) 구현·실측 완료. 남은 것: 단말(관제용 앱)의 Join 발신·
> SSRC 디먹스 UI(U10 공용)·PTT 청취 채널 UI(U6), §10 향후 과제.
>
> 관련: [volte_supplementary_services.md](volte_supplementary_services.md)(내선·당겨받기·호 전달 —
> 본 설계가 그 위에 얹힌다), [registration_binding_set.md](registration_binding_set.md)(도달 경로 선택),
> [media_security.md](media_security.md)(SRTP), [recording.md](recording.md)(녹취 탭),
> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §R1(ambient listening),
> [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md)(단말 SSRC 디먹스),
> [../identifier_model.md](../identifier_model.md), [../../api/cmp_media_api.md](../../api/cmp_media_api.md).
>
> 합법감청 규격 근거: 3GPP TS 33.107(LI 아키텍처)·TS 33.108(인도 인터페이스), ETSI TS 101 671 —
> **통신 내용(CC)은 방향·당사자를 분리 가능한 형태로 인도**하며 서버 믹싱을 규정하지 않는다(§5.1).

---

## 1. 범위와 결론

세 요구는 **관제 그룹(dispatch group) 엔티티 하나**를 두고 그 위에 규격 기반 서비스 둘을 얹으면
한 축으로 수용된다.

| 요구 | 결론 | 표준 근거 |
|---|---|---|
| ③ 관제 그룹 N 개 | **관제 그룹 엔티티** 신설(§3). 기존 `pickup_group` 축을 이 엔티티의 id 로 채워 당겨받기·BLF 인가·병렬 호출·감청 범위가 **한 그룹 축**을 공유한다 | [identifier_model.md](../identifier_model.md) — 동작은 불변 id, 표시는 name |
| ② 대표번호 착신을 관제사 전원이 선택 수신 | 그룹의 **대표번호(pilot) 병렬 호출**(§4) — 대표번호 INVITE 를 등록 그룹원 전원에게 포크, 최초 200 OK 가 이기고 나머지는 CANCEL | 3GPP TS 24.239 Flexible Alerting(parallel alerting), RFC 3261 §16.7(포크 응답 처리), RFC 3455 `P-Called-Party-ID` |
| ① 업무망 통화 선택 합법감청 | **선택** = RFC 4235 dialog 이벤트(기존 구현)의 인가 범위를 그룹 `monitor_scope` 로 확장, **합류** = RFC 3911 `Join` INVITE(`a=recvonly`) → CMP **청취 leg(tap)** — 양 화자를 **분리 스트림(SSRC 2개)** 으로 인도(귀속 보존), **믹싱은 단말**(§5·§6). PTT 그룹콜은 `recv_only` 멤버로 JOIN | 3GPP TS 33.107/33.108·ETSI TS 101 671(LI — 분리 인도), RFC 4235, RFC 3911, RFC 3264, RFC 5576(소스 라벨링), TS 24.379(§5.6·§10) |

설계 원칙(CLAUDE.md 우선순위)대로 규격형을 채택하고, 기존 구현(픽업·전달·dialog 이벤트·CMP
ambient 플래그·녹취 탭)의 연장으로 구성한다. **INVITE 경로에 DB 질의를 넣지 않는다** — 관제 그룹
판정은 전부 인메모리 맵에서 답한다.

---

## 2. 현재 구현과의 갭 (구현 단계의 입력)

| 갭 | 현재 | 필요 |
|---|---|---|
| **G1 그룹 엔티티 부재** | `pickup_group` 은 가입자 컬럼의 자유 문자열. 그룹 자체는 실체가 없다(대표번호·범위·정책을 둘 곳이 없음) | `dispatch_groups` + 멤버 테이블(§3). `pickup_group` 은 멤버십에서 **파생**(CSC 단일 쓰기 주체) |
| **G2 1:N 포크 구조 부재** | `CCallMap` 은 leg 쌍(`m_strPeerCallId` 1:1). 바인딩 집합도 "사람당 leg 하나"(멀티 디바이스 금지) | A-leg 하나에 **대기 B-leg N 개**를 묶는 포크 집합(§4.4). 바인딩 집합의 원칙은 **한 사람** 범위이므로 그룹 포크와 충돌하지 않는다 — 문서에 경계 명시 |
| **G3 대표번호 해석 부재** | 미등록 착신은 `TryPickupDial`(피처코드) 아니면 404 | 미등록 착신 판정 앞에 **pilot 해석**(§4.2) |
| **G4 감청 합류 시그널링 부재** | 수신 INVITE-`Replaces`(교체)만 구현 | INVITE-`Join`(합류) 처리기(§5.3) — Replaces 처리기와 같은 자리 |
| **G5 CMP 3자 leg 부재** | `PRtpRelay` 는 peer 2개 고정(peer i 수신 → peer 1-i 송신). MIX 는 예약만 | 세션에 붙는 **청취 leg(tap)** — 양 peer ingress 복사, 상향 미중계, leg 별 SRTP(§6) |
| **G6 dialog 이벤트 인가 범위** | 같은 `pickup_group` 만 200, 아니면 403 | 같은 그룹 **또는** 관제 그룹 `monitor_scope` 가 대상을 포함하면 200(§5.2) |
| **G7 PTT 청취 시그널링·인가** | CMP `recv_only`/`floor_suppress` 는 구현, CSP 발행 미구현. `ptt_user_profile` 에 청취 자격 필드 없음 | 관제사의 그룹콜 합류를 청취 멤버로 PTT_JOIN + `allow_ambient_listening` 자격 게이트(§5.6) |
| **G8 감사** | 감청 사실을 남길 이벤트 없음 | `E-AUD-016 call_monitored`(§5.7) |

---

## 3. 관제 그룹 모델

### 3.1 정의

관제 그룹 = **픽업 그룹 + (선택) 대표번호 + (선택) 감청 범위**. 대표번호가 없는 그룹은 순수
당겨받기 그룹(현행 `pickup_group` 사용례)이고, 감청 범위가 `none` 인 그룹은 감청을 못 한다.
즉 기존 픽업 그룹은 이 모델의 부분집합이며 별도 축을 남기지 않는다.

| 필드 | 의미 |
|---|---|
| `id` | **불변 키**(CSC 발급, 예 `dg-7f3a91c2`). `volte_subscriptions.pickup_group` 에 그대로 들어가는 값이자 알람·이력의 상관 키 |
| `name` | 표시 이름(운영자 변경 가능, 어떤 키에도 쓰지 않는다) |
| `pilot_id` | 대표번호(다이얼 가능한 주소). 가입 id 주소 공간과 **겹치지 않아야** 한다(CSC 검증). NULL = 대표번호 없음 |
| `service_ref` | 대표번호가 속한 접속서비스 — 도메인·SRTP 정책·피처코드를 이 서비스에서 읽는다 |
| `alert_mode` | `parallel`(기본) / `sequential`(§10) — TS 24.239 의 두 모드 |
| `no_answer_sec` | 전원 무응답 판정 시간(기본 30) |
| `busy_members` | `skip`(기본 — 통화 중 그룹원은 호출 안 함) / `alert`(호출 — 단말 통화대기) |
| `overflow_target` | 무응답·전원 부재 시 넘김 대상(다른 대표번호 또는 내선). NULL = 480 |
| `monitor_scope` | `none`(기본) / `own`(자기 그룹) / `listed`(§3.2 대상 목록) / `all` |
| `ptt_listen` | `none`(기본) / `listed` / `all` — 청취 가능한 PTT 그룹 범위(§5.6) |
| `org_id` | 소속 조직(콘솔 필터·RBAC 스코프) |

### 3.2 멤버십과 파생

- `dispatch_group_members(user_id PK, group_id, alert_order)` — **가입자당 그룹 하나**. `pickup_group`
  이 단일 값이므로 이 제약이 축 통합의 전제다(겸임은 §10).
- CSC 가 멤버 추가/제거 시 `volte_subscriptions.pickup_group` 을 `group_id`/NULL 로 **함께 갱신**하고
  `USER_CHANGED` 를 보낸다(기존 경로). 관제 그룹 소속 가입자의 `pickup_group` 직접 편집은 409
  (`derived_from_dispatch_group`) — SoT 는 멤버십이다.
- `dispatch_group_monitor_targets(group_id, target_group_id)` — `monitor_scope=listed` 의 대상.
- `dispatch_group_ptt_targets(group_id, ptt_group_id)` — `ptt_listen=listed` 의 대상.

### 3.3 CSP 인메모리 맵

`CCspDispatchGroupMap`(`csp/CspDispatchGroup.h/.cpp`) — 그룹 id 인덱스 + pilot 인덱스 + 멤버 인덱스(가입자 →
그룹) + 감청/청취 범위 판정(`CanWatch`/`CanListenPtt`). 부팅 시 `DbManager` 가 적재하고(`dispatch_groups`
테이블 부재는 프로브로 감지 — INFO 로그 후 관제 기능 비활성) `DISPATCH_GROUP_CHANGED` UDP 통지(uri=그룹 id,
DELETE=제거·그 외 단건 재적재, 빈 uri=전량)와 `CSC_RESTART` 로 재적재한다. JSON fallback 은
`DataFolder.DispatchGroup`(기본 `dispatch_group/`)의 `<id>.json`(User/Group 관례 — 멤버·대상 배열 포함, 개발·시험
환경에서 DB 마이그레이션 없이 쓴다). 포크 대상은 **멤버 테이블(`alert_order` 순)** 이 SoT 이고 등록·생존 여부만
`UserMap` 으로 판정한다 — 등록 바인딩의 그룹 스냅샷 지연에 좌우되지 않는다. 당겨받기·BLF·감청 인가의 그룹 축
값은 `EffectiveGroupOf`(멤버 인덱스 → `CspUser.EffectivePickupGroup()` = `pickup_group` → org 폴백) 하나로 답한다.

---

## 4. 대표번호 호출 (TS 24.239 Flexible Alerting — parallel / sequential)

### 4.1 flow (parallel)

```
UE-A ──INVITE sip:7000@dispatch.cims──► CSP(TAS)
                                          │ [pilot 해석: 7000 → dg-7f3a91c2, alert_mode=parallel]
                                          │ [그룹원 중 등록·생존 바인딩·(busy_members=skip) 비통화 → B, C]
                                          │ ── RELAY_ADD (peer0=A 주소, peer1=0.0.0.0:0 미확정) ──► CMP
                                          │ ── INVITE ──► UE-B   SDP: local_port_b, 서버 키 offer(B 전용)
                                          │ ── INVITE ──► UE-C   SDP: local_port_b, 서버 키 offer(C 전용)
                                          │              From: A, To: B|C(그룹원 AoR), P-Called-Party-ID: <sip:7000@dispatch.cims>
 UE-A ◄── 180 Ringing ─────────────────── │ ◄── 180 ── B, C   (첫 180 만 A 에 전달)
                                          │ ◄── 200 OK ── C     ← 최초 응답 = 승자
                                          │ ── RELAY_MODIFY (peer_index=1, C 주소·C answer crypto, callee=C) ──► CMP
                                          │ ── CANCEL ──► B     (487 수신 → 대기 leg 정리)
 UE-A ◄── 200 OK (SDP: local_port, 서버 answer) ── │
 UE-A ◄════════════ RTP (relay) ═══════════════════► UE-C
```

핵심 계약: 대기 leg 전원에게 **같은 peer1 포트**(`local_port_b`)를 광고하고, 승자만
`RELAY_MODIFY` 로 peer1 에 고정한다. 픽업(§5.3, [volte_supplementary_services.md](volte_supplementary_services.md))과
동일한 "변경은 MODIFY, 재생성 금지" 계약이라 CMP 신규 명령이 없다. peer1 목적지·crypto 가 없는
동안 CMP 는 peer1 로 송신하지 않고 peer1 포트 수신도 폐기한다(패자 leg 의 조기 미디어 차단).

### 4.2 pilot 해석 지점

`ModuleDispatcher` 의 미등록 착신 분기에서 `TryPickupDial` **앞에** `CTasModule::TryDispatchPilot`
을 둔다(둘 다 "등록 가입자가 아닌 To" 를 다루는 TAS 훅). pilot 이면 §4.1 을 수행하고 true 를 돌려
일반 404 경로를 막는다. TAS 역할 off 노드에서는 비활성(모듈 게이트).

외부 발신(민원인 → 대표번호)은 IBCF/트렁크 유입 INVITE 이며 **이것이 주 사용례다.** pilot 해석은
"등록 가입자가 아닌 To" 훅이라 유입 경로와 무관하게 동작하되, 대표번호 도메인 라우팅이 이 INVITE 를
TAS 인에이블 CSP 로 보내야 한다. 트렁크 leg 는 SRTP·코덱 협상이 내부 단말과 다를 수 있으므로(평문
트렁크 등) tap 복사 시 그 leg 의 실제 미디어 속성을 따른다(§5.4).

### 4.3 헤더·신원

- B-leg `From` = 원 발신자(관제사가 발신자를 봐야 한다). `To` = 그룹원 AoR(B2BUA 관례 — 착신 leg 의 신원
  조회(NAT·서비스·PT)가 `GetToId` 에 걸려 있고 그룹콜 fan-out 도 같은 관례다). **`P-Called-Party-ID`
  (RFC 3455 / TS 24.229) = 대표번호** — 단말 앱이 "대표번호로 온 호" 임을 알고 데스크 UI 를 띄운다
  (그룹콜 fan-out 이 이미 이 헤더를 쓴다, `GroupCallService.cpp`).
- **재타게팅 이력의 표준 표현 = History-Info(RFC 7044)** 이지만 코드에 미구현이다. 현재는
  `P-Called-Party-ID` 로 "대표번호로 온 호" 표시를 대신하고, History-Info 는 향후 과제로 둔다(§10).
- A 에게는 180 을 **한 번만** 전달한다(첫 180). 183 조기 미디어는 전달하지 않는다 — 대기 leg 의
  미디어는 CMP 가 폐기하므로 A 에게 183 을 주면 무음 구간이 생긴다.

### 4.4 CallMap 포크 집합 (G2)

`CCallMap` 에 **포크 집합** `CForkSet { aCallId, pending[ ] bCallIds, relaySessionId, timer }` 를 둔다.

| 상태 | 대기 B-leg `CCallInfo` | A-leg `CCallInfo` |
|---|---|---|
| 포크 중 | `m_strPeerCallId = A`, `m_bEstablished=false`, `m_strRelaySessionId` 공유 | `m_strPeerCallId` **비움**(승자 미정), 포크 집합 참조 |
| 승자 확정 | 승자만 `m_bEstablished=true`, 패자 엔트리 삭제(CANCEL/487) | `m_strPeerCallId = 승자`, 이후 기존 1:1 경로와 동일 |

- **응답 경합**: 승자 확정 후 도착한 두 번째 200 OK(CANCEL 교차)는 ACK 후 즉시 BYE(RFC 3261
  §16.7 의 B2BUA 등가 처리). 대기 leg 의 4xx~6xx 는 집합에서 제거만 하고 A 에게 전달하지 않는다 —
  **전원 최종 실패** 시에만 A 에게 응답한다(486 우세면 486, 그 외 480).
- **무응답**: `no_answer_sec` 만료 → 전원 CANCEL → `overflow_target` 있으면 그 주소로 재시도
  (다른 대표번호면 재귀 1단계까지, 순환 금지), 없으면 480.
- **A 가 취소**(CANCEL) → 대기 leg 전원 CANCEL, relay 회수.
- sweeper: 대기 leg 는 `m_bEstablished=false` 라 기존 미확립 회수 정책이 그대로 적용된다.
- **링잉 대표번호 호의 당겨받기**: 대기 leg 는 `CCallMap` 밖(TAS 포크 집합)에 있으므로 `SelectToRing` 이
  보지 못한다 — `PickUp` 이 CallMap 후보에서 링잉 호를 못 찾으면 픽업자 그룹의 포크 집합을 본다
  (`FindForkForPickup`: 그룹 픽업 `<code>` = 그 그룹의 포크 중인 대표번호 호, 지정 픽업 `<code><대표번호>` /
  `<code><대기 leg 그룹원 내선>`). 인가 = 픽업자의 유효 그룹(`EffectiveGroupOf`)이 대표번호 그룹과 같을 때(403).
  `PickUpFork` 가 승자 확정과 같은 재키잉을 한다 — 대기 leg 전원 CANCEL(+대표번호 dialog terminated), (A, 픽업)
  쌍 CallMap 삽입, relay peer1 `RELAY_MODIFY`(픽업 단말 주소·crypto), A·픽업 양측 200(픽업 offer 기준 — 기존
  `PickUpLeg` 와 동형), 대표번호 dialog confirmed, `call.json answered_by`=픽업자. 검증 F5.

### 4.4a sequential alerting

`alert_mode=sequential` 이면 그룹원을 `alert_order` 순으로 **한 명씩** 호출한다(TS 24.239 sequential alerting).
포크 집합은 그대로 쓰되 대기 leg 가 항상 1개다:
- `no_answer_sec` 는 **단계 시한**(그룹원 1명당 링 시간, `ForkRingTimeoutSec` 로 clamp)이 되고, 남은 순번은
  집합의 큐(`vecQueue`)에 있다.
- 현 순번이 최종 실패(486/603/487 등)하면 즉시 다음 순번, 단계 시한 만료면 현 leg CANCEL(+dialog terminated) 후
  다음 순번. 등록·생존 판정은 포크 대상 결정 시점(`ResolveForkTargets`)에 한 번 하고, 순번 차례에 leg 생성이
  실패하면 건너뛴다.
- 큐 소진 = 전원 무응답 → `overflow_target`(있으면, 1단계) 또는 480 — parallel 과 같은 종결 규칙. overflow 대상이
  다른 대표번호면 그 그룹의 `alert_mode` 를 따른다.
- A 에게 180 은 첫 순번의 첫 180 한 번만(이후 순번 전환은 A 에게 보이지 않는다). 지정 픽업(F5)은 sequential 중에도
  같은 방식으로 성립한다(대기 leg 1개 + 큐 폐기). 검증 F6.

### 4.5 대표번호의 dialog 이벤트

그룹원은 대표번호 AoR 에 `Event: dialog` 를 구독할 수 있다(인가 = 그 그룹의 멤버). 대표번호에
걸려온 호의 early/confirmed/terminated 와 응답자(`remote` 신원)가 NOTIFY 된다 — 데스크 큐 표시·
"누가 받았나" 표시의 표준 경로다.

### 4.6 녹취·이력

relay 는 대표번호 호 1건이다. `RELAY_ADD` 의 `callee` 는 대표번호, 승자 확정 시 `RELAY_MODIFY` 의
`callee` 로 응답자를 갱신한다. `call.json` 에 `dispatch_group`, `pilot`, `alerted[]`, `answered_by`
를 기록한다(CSP 작성 메타 — [recording.md](recording.md) §3.6).

---

## 5. 업무망 합법감청 (통화 청취)

### 5.1 모델과 규격상 위치

**본 기능은 업무망 통화에 대한 운영자 인가 기반 합법감청(lawful intercept, 감독 청취)이다.** 대상이
청취 사실을 알지 못하는 **은닉**은 합법감청의 당연 전제이며(대상이 알면 감청이 성립하지 않는다),
회의(conference) 계열 규격의 통지 권고는 여기에 적용되지 않는다.

규격상 위치를 분명히 한다:
- **서버 믹싱은 감청 규격에 정의돼 있지 않다.** 3GPP LI(TS 33.107 아키텍처·TS 33.108 인도 인터페이스)
  와 ETSI(TS 101 671·TS 102 232)는 통신 내용(CC)을 감청 설비로 **인도**하는 것을 규정하며, 그 원칙은
  **양방향·당사자를 분리 가능한 형태로 전달**해 귀속(누가·언제·무엇을)을 보존하는 것이다. 믹싱은 이
  귀속을 파괴하므로 감청 규격은 믹싱을 규정하지 않는다 — 한 스트림으로 섞는 것은 conference
  focus+mixer(RFC 4353/4579, RFC 3911 Join 의 전형적 실현)의 개념이다.
- 따라서 본 설계의 **분리 스트림 인도(SSRC 2개) + 단말 재생 믹스**는 감청 규격의 분리·귀속 원칙에
  부합하는 선택이다(§5.4). 서버는 귀속 가능한 두 스트림을 인도하고, 믹스는 청취자 재생 편의를 위한
  단말 처리다.
- **업무망은 3GPP LI 인도 아키텍처(LEMF·HI2/HI3) 전체를 구현하지 않는다.** 운영자가 자기 망의 업무
  통화를 인가받아 청취하는 것이므로 미디어 인도 방식을 맞출 외부 규격 대상이 없다 — 인도 방식은 구현
  정의이며, 귀속·녹취 품질 때문에 분리 인도를 택한다. 법적 근거·인가·감사는 §5.8.

감청은 **관제사가 진행 중 세션의 청취 leg 로 합류**하는 것이며 두 단계로 나눈다.

| 단계 | 규격 | 구현 위치 |
|---|---|---|
| 선택 — 진행 중 통화 목록 | RFC 4235 dialog 이벤트(구현) + 인가 범위 확장 | `CscfModule` SUBSCRIBE 분기, `CTasModule::OnCallRing/Start/End` |
| 합류 — 특정 dialog 청취 | RFC 3911 `Join` INVITE, SDP `a=recvonly` | `CTasModule::OnIncomingCall`(Replaces 처리기 옆) + CMP tap(§6) |

RFC 3911 `Join` 은 **대상 dialog 지목·인가의 시그널링 수단으로만** 쓴다 — 미디어를 focus 가 믹싱하는
전형적 실현은 따르지 않고 tap 이 분리 인도한다(§5.4). 감청 leg 는 **세션에 붙는다**(leg 가 아님).
픽업·전달로 A/B leg 가 재고정돼도(`RELAY_MODIFY`) tap 은 그대로 남고, 세션 종료(`RELAY_REMOVE`)와
함께 사라진다.

### 5.2 선택 — dialog 이벤트 인가 범위 (G6)

`CanWatchDialog(watcher, target)`:
1. 같은 `pickup_group`(= 같은 관제 그룹) → 허용(현행).
2. watcher 의 관제 그룹 `monitor_scope` 가 `all`, 또는 `own` 이고 target 이 자기 그룹, 또는 `listed`
   이고 target 의 그룹이 대상 목록에 있음 → 허용.
3. 그 외 403.

"모든 통화" 목록의 구독 형태:
- **초기형**: 대상 내선별 dialog 구독(현행 구현 그대로 동작). 콘솔/OAM 의 활성 세션 뷰(`SESSION_LIST`/
  STATS)를 관제용 앱이 목록 소스로 쓰고, 클릭 시 소프트폰이 §5.3 의 Join INVITE 를 낸다.
- **표준형(후속)**: RFC 4662 RLS — `Supported: eventlist` 로 그룹의 감시 목록 URI 하나를 구독하고
  RLMI+multipart NOTIFY 로 전 대상의 dialog-info 를 받는다. 구독 N 개를 1개로 줄인다.

### 5.3 합류 — INVITE-with-Join (G4)

```
UE-A ◄──통화중──► CSP(B2BUA)+CMP(relay S) ◄──통화중──► UE-B
UE-M ──INVITE sip:A@dispatch.cims ──► CSP
      Join: <A-leg Call-ID>;to-tag=…;from-tag=…        (dialog NOTIFY 에서 얻은 식별자)
      SDP: m=audio … a=recvonly, crypto(M 수신 키)
                                      │ [CallMap 에서 Call-ID 조회 → 세션 S 확정(MatchDialog: Call-ID+태그)]
                                      │ [인가: M 의 관제 그룹 monitor_scope ∋ A 또는 B 의 그룹]
                                      │ ── RELAY_TAP_ADD (session_id=S, tap_id, M 주소, tap_mode=both, media_crypto) ──► CMP
                                      │ ◄── local_port_t ─────────────────────────────────────────────────── CMP
UE-M ◄── 200 OK  SDP: c=relay ip, m=audio local_port_t … a=sendonly ── │
UE-M ◄════ RTP (A ingress 복사 SSRC_A + B ingress 복사 SSRC_B, tap 키로 SRTP) ════ CMP
        A/B 에게는 아무 메시지도 가지 않는다 (re-INVITE·NOTIFY 없음)
```

- **대상 지목**: `Join` 의 Call-ID 는 A-leg 든 B-leg 든 세션의 어느 dialog 라도 된다(둘 다 같은
  `m_strRelaySessionId`). 태그 대조는 `MatchReplacesDialog` 를 일반화한 `MatchDialog` 로 한다.
- **응답 코드**: dialog 없음/조기 dialog → 481, 인가 실패 → 403, `recvonly` 아님·코덱 불일치·
  서비스 `media_srtp=required` 인데 crypto 없음 → 488, 세션당 tap 상한 초과 → 486.
- **인가 두 겹**: SIP 경로는 인메모리 판정(그룹 멤버십 + `monitor_scope`)만 한다. "누가 감청 가능
  그룹의 멤버가 될 수 있나" 는 CSC/콘솔의 RBAC(`users.role` operator 이상만 `monitor_scope≠none`
  그룹에 편입 가능, [mcptt_authorization.md](mcptt_authorization.md))가 프로비저닝 시점에 막는다.
- **재-INVITE**: M 의 주소 변경(NAT 재바인딩 등)은 `RELAY_TAP_MODIFY`. hold 는 의미 없음(488).
- **종료**: M 의 BYE → `RELAY_TAP_REMOVE`. 원 통화 종료 → CSP 가 세션의 tap 전부에 BYE 를 보내고
  `RELAY_REMOVE`(tap 은 세션과 함께 회수, 별도 명령 불요).

### 5.4 미디어 — 청취 leg 의 성격

- **패킷 복사, 트랜스코딩 없음 — 믹싱은 단말이 한다(확정 구조).** CMP 는 코덱을 열지 않는다(relay
  원칙). 양 peer 의 ingress 를 SRTP 복호 후 tap 키로 재암호화해 M 에게 보낸다. SSRC 는 원본 유지 →
  M 은 한 m-line 에서 **SSRC 2개**를 받아 디먹스·믹스·재생한다. "믹싱은 단말 몫" 원칙
  ([mcptt_standard_conformance.md](mcptt_standard_conformance.md) §R2)과 같고, 단말 구현은 U10(SSRC 디먹스,
  [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md))과 **공용**이다. 따라서 관제용 단말(소프트폰·
  관제용 앱)은 SSRC 디먹스·믹싱을 **필수 능력**으로 갖는다 — 이를 못 하는 범용 소프트폰은 감청 단말로
  지원하지 않는다.
- **PT 재작성**: A/B 의 wire PT 가 다를 수 있으므로(동적 96 vs 99) tap leg 에 M 의 수신 PT(`remote_pt`)
  를 스탬프한다 — 기존 leg 별 PT 재작성과 동일 메커니즘. 코덱 자체는 통화의 협상 코덱이어야 한다
  (M 의 answer 에 그 코덱이 없으면 488).
- **소스 라벨링(RFC 5576) — 합법감청의 귀속 요건**: 두 SSRC 가 PT 로도 구분되지 않으므로(둘 다 M
  수신 PT 로 통일) tap SDP 에 `a=ssrc:<SSRC_A> cname:… label:caller` / `a=ssrc:<SSRC_B> … label:callee`
  (RFC 5576)를 실어 단말·녹취가 발신자/착신자를 구분한다. 감청은 귀속이 법적으로 중요하므로 이
  라벨링은 **필수**다. 원본 A·B 의 SSRC 우연 충돌(RFC 3550)은 CMP 가 tap egress 에서 재매핑해
  유일성을 보장한다.
- `tap_mode=a|b` 는 **한쪽 화자만 듣는 운용 선택**(예: 민원인 발화만)이며 단말 능력 폴백이 아니다.
- **상향 차단**: M 의 RTP 는 CMP 가 폐기한다(PTT `recv_only` 와 동일 의미). M 의 인바운드 RTCP 는
  keepalive 로만 받는다. CMP 는 두 SSRC 각각의 **RTCP SR 을 tap 으로 송출**한다 — 립싱크(특히 영상)와
  수신 통계에 필요하다.
- **재키잉 독립**: A/B leg 가 re-INVITE 로 재키잉돼도 tap 키는 독립이라 무영향이다(CMP 가 복호
  프레임을 tap 키로 재암호화). tap 주소 변경만 `RELAY_TAP_MODIFY`.
- 영상은 두 번째 m-line(`a=recvonly`)으로 같은 방식(`tap_mode` 공통) — 양측 영상이면 M 은 영상
  SSRC 2개를 받아 격자 합성 렌더한다(§8.4 단말 요건).
- **은닉**: A/B 에게 SDP 변경·re-INVITE·NOTIFY 가 없다. dialog 이벤트 NOTIFY·BLF·`SelectToRing`
  픽업 후보·RFC 4575 로스터 어디에도 tap leg 를 노출하지 않는다(`CTasModule` 의 `OnCallRing/Start/End`
  가 tap leg 를 건너뛴다).

### 5.5 여러 관제사의 동시 감청

세션당 tap N 개(`tap_id` 로 구분). 상한은 csp.json `Setup.Sip.Dispatch.MaxTapsPerSession`(기본 2).
초과 시 486.

### 5.6 PTT 그룹콜 청취 (G7)

관제사가 `ptt_listen` 범위 안의 그룹 AoR 로 **SDP `a=recvonly` 초기 INVITE**(RFC 3264 — 수신 전용 offer 가
청취 합류의 시그널링 신호다; 통화 감청 Join 과 같은 표현)를 보내면 CSP(`CGroupCallService::ProcessGroupCall`)는
그룹 멤버 여부와 무관하게 **청취 멤버**로 합류시킨다:
- 인가 통과 후 answer 는 `a=sendonly`(RFC 3264 §6.1) + 멤버 전용 CMP 포트 + floor `m=application`. CMP 에는
  `PTT_JOIN recv_only=1` — 상향 미중계·floor 요청은 `DENY(cause receive-only)`. `floor_suppress` 는 **쓰지 않는다** —
  청취자는 Floor Taken 의 "Permission to Request the Floor=0" 변형(단말 U6 — PTT 버튼 비활성)으로 현재 발언자를
  알아야 하고, 청취자에게 가는 유니캐스트 floor 메시지는 다른 참가자에게 드러나지 않으므로 은닉과 무관하다.
- **합류만 한다** — 활성 세션(확립된 비청취 leg)이 없으면 480(상시 세션 `chat` 그룹은 예외). 청취 leg 는 세션
  활성·마지막 이탈 판정에서 제외되어 세션을 붙들지 못하고, 멤버 fan-out 을 일으키지 않으며, 긴급/임박 조건을
  개시·상향하지 못한다(mcptt-info 지시자 무시). 참가자 DB(`call_log`/participants)·PTT 세션 이력(`PttMemberLeave`
  등)에 남기지 않고 감사(§5.7)로만 남긴다. affiliation 은 만들지 않는다(청취는 제휴가 아니다).
- 비멤버의 일반(sendrecv) INVITE 는 403 (TS 24.379 §10.1.1 — 그룹 멤버가 아닌 사용자의 개시/합류 거절).

**인가 — TS 24.484 프로파일 자격 + 관제 그룹 범위(규격형, 2단)**:
- **자격 = `ptt_user_profile.allow_ambient_listening`**(TS 24.484 ruleset·TS 24.379 ambient listening 인가):
  이 사용자가 원격 청취를 수행할 자격. 관제사에게만 부여(기본 0). CSP 가 청취 개시 INVITE 에서 프로파일 행
  하나를 읽어 판정한다(`SelectUserProfile` — 인덱스 단건, 다른 프로파일 게이트와 같은 경로. 값 0·행 부재·DB
  불가는 모두 403 — 당사자 모르게 미디어를 인도하는 동작이라 fail-closed). 규격이 정한 인가 자리를 그대로 쓴다.
- **범위 = 관제 그룹 `ptt_listen`**(`none`/`listed`/`all`): 자격자가 어느 PTT 그룹을 들을 수 있는가.
- **편입 게이트 = `users.role`**(manager 승인): 자격 부여 자체를 콘솔에서 승인·감사한다(§5.7).

**로스터 노출 — `listen_visibility`(은닉·투명 둘 다 정식 지원)**: 규격이 청취 멤버 표시를 정의하지
않으므로 CIMS 정책축이며, **관제사의 관제 그룹 속성**으로 두 모드를 모두 지원한다.
- `hidden`(기본): 청취 멤버를 로스터(RFC 4575 conference-info)에서 제외하고 합류/이탈 시 참가자 통지도 내지
  않는다(청취 leg 자신은 NOTIFY 를 받는다). `FLOOR_TALKERS`·녹취 화자 트랙에는 `recv_only` 라 원래 오르지 않는다 —
  합법감청 은닉(§5.1). 사용은 §5.8 의 고지·동의 운영 규약을 전제한다.
- `visible`: 청취 멤버를 로스터에 `<roles><entry>listener</entry></roles>`(RFC 4575 §5.6.3)로 싣고 합류/이탈을
  통지한다 — 협업 무전 그룹처럼 청취 공개가 정상인 운용용. 이때도 발언 자격은 없다(`recv_only`).
- CMP 멤버 수에는 포함되므로 "참가자 1명" floor 거절(only-one) 판정이 청취자 합류로 풀릴 수 있다 — 청취가
  성립하려면 불가피한 관측 가능 변화다.

TS 24.379 **ambient listening**(`session-type=ambient-listening`, remote-init — 특정 단말 주변음을
원격 개시로 듣는 1:1 호)은 같은 `allow_ambient_listening` 자격을 재사용하되 단말의 무표시 자동응답이
필요해 시그널링은 별도 과제다(§10).

### 5.7 감사 이벤트 (G8)

감청은 당사자가 모르는 동작이므로 **감사 이벤트를 필수**로 남긴다 — 카탈로그 `E-AUD-016`
`event=call_monitored`(kind=audit, source=CSP): `monitor`(관제사 id), `group`(관제 그룹 id), `session`
(relay `session_id`/sesid), `targets`(A/B id), `started_at`/`ended_at`/`dur_ms`, `tap_mode`. 시작·종료
각 1건. PTT 그룹콜 청취(§5.6)도 같은 코드로 남긴다 — `session`/`target_a`=PTT 그룹 id, `target_b` 없음,
`tap_mode=ptt_listen`, `group`=관제사의 관제 그룹(`CGroupCallService::EmitPttListenAudit`). FM push 경로는
[../alarm_self_reporting.md](../alarm_self_reporting.md), 카탈로그 행은 [../alarm_catalog.csv](../alarm_catalog.csv).
`call.json` 에도 `monitors[]` 로 남긴다(당사자 표시 UI 에서는 숨기고 감사 화면에서만 노출). 감청 대상 범위
(`monitor_scope`)는 관제 업무 근거가 있는 통화로 한정하는 운영 규약을 전제한다.

**열람** — 콘솔 `장애 > 감사 이력`(`/alerts/audit`, `requiredRole=manager`): `kind=audit` 이벤트를 단계(시작/종료/
거절)·감청자·관제 그룹·세션·대상·방식·시간 열로 펼친다(`core.audit-history` 위젯, CSV). 서버 게이트는 OAM
`GET /api/v1/events` — manager 미만 계정에는 `kind=audit` 이벤트를 결과에서 제외하고 `kind=audit` 명시 조회는 403
(`code=` 필터 추가). 일반 이벤트 이력 화면의 "감사" 분류도 같은 게이트를 받는다.

감사 로그 자체의 무결성이 통제의 핵심이다: "누가 무엇을 감청했나" 의 **열람은 `manager` 이상으로
제한**하고(감청 수행 권한과 분리), 보존 기간은 조직 정책을 따르되 감청 감사는 일반 이벤트보다 길게
둔다. 감청 leg 개설 실패(403/481/488)도 시도로 남긴다(무단 시도 추적).

### 5.8 법적 근거·인가

- **감청은 운영자 인가에 근거한다.** 편입(누가 감청 가능 그룹의 멤버가 되나)은 `users.role` `manager`
  이상이 콘솔에서 명시 승인하고 그 자체가 감사된다(§5.7). SIP 경로는 인메모리 인가만 집행한다(§5.3).
- **동의·고지는 배포 정책**이다. 관할지 법제(업무 통화 감청 고지 의무·동의 요건)에 따라 가입자 온보딩
  시 고지하는 것을 전제하며, 시스템은 그 근거를 강제하지 않고 감사로 뒷받침한다.
- **본 기능은 3GPP LI 핸드오버(HI2/HI3·LEMF)가 아니다** — 업무망 내부 감독 청취다. 외부 사법기관
  인도가 요구되면 별도 LI 게이트웨이 설계가 필요하다(범위 밖, §10).

### 5.9 HA·재기동 시 수명

포크 집합·tap 매핑은 CSP `CCallMap` 인메모리 상태다. **tap 수명은 CMP relay 세션에 종속**되므로
(§6.2 — `RELAY_REMOVE`/`RELAY_ABORTED` 가 세션의 tap 을 일괄 회수) CSP 재기동으로 tap_id 기억을 잃어도
고아 tap 은 세션 종료·sweeper 로 회수된다 — 별도 복구 경로가 필요 없다. 진행 중 포크는 재기동 시
소실되며(미확립 leg — sweeper 회수) 수용 가능하다. active/standby 절체 시 회수는 active 역할만
수행한다([../ha_design.md](../ha_design.md) 역할 게이트).

---

## 6. CSP↔CMP 계약 — 청취 leg (cmp_media_api §6.5 신설)

RELAY 세션에 붙는 **tap** 자원. 키 = `(node, session_id, tap_id)`, 수명 = 세션.

### 6.1 RELAY_TAP_ADD (멱등)

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `session_id` | O | 대상 relay 세션. 없으면 `NOT_FOUND`(부활 금지) |
| `tap_id` | O | client 명명(세션 내 유일). 같은 키 재요청은 동일 포트 반환 |
| `remote_ip` / `remote_port` | O | 청취 단말 RTP 주소 |
| `remote_video_port` | - | 청취 단말 Video RTP 포트(영상 tap 시) |
| `remote_nat` / `remote_sig_ip` | - | RELAY_ADD 와 동일(목적지 latch·guard) |
| `remote_pt` / `remote_te_pt` | - | 청취 단말이 수신 선언한 PT — tap egress 스탬프 |
| `tap_mode` | - | `both`(기본, SSRC 2개) / `a` / `b` |
| `media_crypto` / `media_crypto_video` | - | tap leg SRTP 키(CMP→단말 tx 만 유효, rx 는 무시) |
| `monitor` | - | 청취자 id(flow 로깅·감사 메타) |

응답: `local_ip`, `local_port`, `local_video_port`(tap 전용 포트, RTCP +1). 오류: `NOT_FOUND`,
`NO_RESOURCE`, `LIMIT`(세션당 상한 — CMP 자체 상한 `relay.max_taps`, 기본 4).

### 6.2 RELAY_TAP_MODIFY / RELAY_TAP_REMOVE

MODIFY 는 ADD 와 같은 payload 로 주소·crypto 만 갱신(같은 포트). REMOVE 는 `session_id`+`tap_id`,
없으면 OK(자연 멱등). **RELAY_REMOVE 는 세션의 tap 을 모두 회수**한다 — RELAY_ABORTED 도 동일
(CSP 는 그 이벤트 처리에서 tap leg 들에 BYE).

### 6.3 CMP 내부

- 탭 지점 = **녹취 탭 지점과 동일**(egress PT 재작성 전, SRTP 복호 후 —
  [recording.md](recording.md) "녹취 오디오 PT/코덱 메타"). 녹취기와 tap 이 같은 복호 프레임을 본다.
- `PRtpRelay` 에 `_taps[]` 를 두고 peer i 수신 시 (peer 1-i 송신) + (tap 전원 송신). tap 소켓은
  audio/video RTP+RTCP 4개(peer leg 와 동형 포트 블록). tap 소켓 수신은 폐기(RTCP 는 keepalive 처리).
- **분리 인도·라벨링**: `tap_mode=both` 는 A·B ingress 를 **원본 SSRC 유지**로 각각 tap 에 송출한다
  (믹싱 없음). 우연 SSRC 충돌은 tap egress 에서 재매핑하고, CSP 가 채운 caller/callee SSRC 를 tap SDP
  `a=ssrc`(RFC 5576)로 광고한다(§5.4). CMP 는 두 SSRC 의 RTCP SR 을 tap 으로 송출한다.
- HEARTBEAT `resource.tap { total, used }` — 키 존재가 기능 광고(§5.1 규약). CSP 는 `resource.tap`
  이 없는 CMP 에 대해 Join 을 488 로 거절한다(기능 미지원 노드 격리).
- STATS `detail.sessions[].taps[]`.

---

## 7. 구현 구조

보조 서비스 로직은 기존대로 **`CTasModule` 소유**, `ModuleDispatcher` 는 B2BUA 골격만 유지한다.

| 컴포넌트 | 변경 | 상태 |
|---|---|---|
| **CSC** `handlers/dispatch.py` | `dispatch_groups`·멤버·대상 테이블(§8.1), `/api/v1/dispatch-groups` CRUD + `/members` + `/monitor-targets` + `/ptt-targets`(§8.2), `pickup_group` 파생 갱신(멤버 추가/제거/그룹 삭제 → USER_CHANGED) + 가입자 API 직접 편집 409 `derived_from_dispatch_group`, `DISPATCH_GROUP_CHANGED` 통지(uri=그룹 id), pilot↔가입 id·타 대표번호 충돌 409, RBAC(감청/청취 범위 변경·그 그룹 편입은 manager, 편입 가입자 `users.role` operator 이상), `ptt_user_profile.allow_ambient_listening` 편집·XCAP user-profile `<allow-ambient-listening>`, `/provisioning/me` `dispatch{groupId,groupName,pilotId,monitorScope,pttListen,listenVisibility}`. 테이블 미적용 DB 는 목록 `schema=not_migrated`·변경 400 | 구현 |
| **CSP `CCspDispatchGroupMap`** (`CspDispatchGroup.h/.cpp`) | 그룹 id·pilot·멤버 인덱스, `CanWatch`(§5.2)·`CanListenPtt`(§5.6) 범위 판정, `EffectiveGroupOf`(멤버 인덱스 → `pickup_group` → org 폴백), DbManager 적재(`SelectDispatchGroup`/`LoadAllDispatchGroups`, 부팅 프로브 `HasDispatchTables`)·`DISPATCH_GROUP_CHANGED`/`CSC_RESTART` 재적재·JSON fallback `DataFolder.DispatchGroup`(§3.3) | 구현 |
| **CSP `CTasModule` 포크 집합** | `CTasForkSet`(TAS 소유 — 대기 leg 는 승자 확정 전까지 `CCallMap` 밖) · `TryDispatchPilot`(§4.2, 미등록 착신 분기의 `TryPickupDial` 앞) · `ResolveForkTargets`(등록·`busy_members=skip` 비통화·발신자 제외·`alert_order` 순·`MaxForkTargets` 절삭) · `StartAlert`(`alert_mode` 분기 — parallel 전원 / sequential 큐+첫 순번) · `AdvanceSequential`(§4.4a 다음 순번·단계 시한 재설정) · `ForkAlert`(leg 전용 SDES 서버 키·`P-Called-Party-ID`=대표번호) · `OnForkRing`(첫 180 만 A 에, SDP 없이) · `OnForkStart`(승자 → (A,승자) 쌍 CallMap 삽입 후 디스패처 정상 answer 경로가 RELAY_MODIFY·A 200, 패자 CANCEL, 늦은 200 은 BYE) · `OnForkEnd`(패자 최종 응답 흡수, sequential 다음 순번, 전원 실패 486/480, A 취소 → 전원 CANCEL+relay 회수) · `Tick`(1초 — `no_answer_sec` 만료 → sequential 다음 순번 / `OverflowFork`(대표번호면 그 그룹원 재포크·내선이면 단일 leg, 1단계) 또는 480) · `FindForkForPickup`/`PickUpFork`(§4.4 링잉 대표번호 호 당겨받기 — `PickUp` 의 CallMap 후보 폴백) · 대표번호 AoR dialog 이벤트(§4.5 — early/confirmed/terminated) | 구현 |
| **CSP `CscfModule`** | dialog SUBSCRIBE 인가 → `CanWatch(EffectiveGroupOf(구독자), 대상 그룹)`; 대상이 대표번호면 그 그룹(§4.5) | 구현 |
| **CSP `ModuleDispatcher`** | `OnCallRing`/`OnCallEnd` 훅을 소비형으로(포크 leg 흡수) — CallMap leg 의 dialog 통지는 종전대로 통과 | 구현 |
| **CSP 설정** | `Setup.Sip.Dispatch.{MaxForkTargets,ForkRingTimeoutSec,MaxTapsPerSession}`, `Setup.DataFolder.DispatchGroup`(config_template·render 기본 `dispatch_group`) | 구현 |
| **CSP `CCallMap`** | 감청 leg 는 CallMap 밖(TAS `m_mapMonitorLeg`)에 두어 dialog 이벤트·픽업 후보에서 자연 제외(별도 표식 불요). Join 대상 대조는 `MatchReplacesDialog` 재사용 | 구현 |
| **CSP `CTasModule` 감청** | `HandleIncomingJoin`(§5.3 — Join 파싱·`CanWatch` 인가·recvonly·세션당 tap 상한·offer SDES→tap egress 서버 키·200 answer sendonly+`a=ssrc` 라벨) · `HandleMonitorLegEnd`/`ReleaseSessionMonitors`(M BYE·원 통화 종료 시 tap 회수) · `E-AUD-016` 발신(started/ended/denied) | 구현 |
| **CSP `CGroupCallService` PTT 청취** | `ProcessGroupCall` 의 청취 leg 분기(§5.6 — `a=recvonly` 판정·비멤버 403·`SelectUserProfile` 자격 + `CanListenPtt` 범위·활성 세션 없으면 480·answer sendonly·`PTT_JOIN recv_only=1`) · `CallSessionInfo.bListenOnly/bListenHidden`(세션 활성 판정 `HasActiveLeg`·로스터·조건 전파·참가자 DB/이력 제외) · `EmitPttListenAudit`(E-AUD-016 started/ended/denied) | 구현 |
| **CSP `CmpClient`** | `AddTap`(ssrc_a/ssrc_b 응답)/`ModifyTap`/`RemoveTap`, HEARTBEAT `resource.tap` 학습(`SupportsTap` — 미광고 CMP 는 Join 488) | 구현 |
| **CMP** | `PRtpTap`(청취 leg — SSRC 재매핑·SRTP egress·상향 폐기·RTCP SR 재매핑), `PRtpRelay::_taps` fan-out(복호 평문 ingress 복사), `RELAY_TAP_ADD/MODIFY/REMOVE` 핸들러, `resource.tap` 광고·STATS `taps[]`·풀(TapPoolSize/MaxTapsPerSession)·세션 회수 시 일괄 free(§6) | 구현 |
| **콘솔** | 관리>가입자 옆 **관제 그룹** 페이지(`DispatchGroupsPage` — 그룹 CRUD·멤버 transfer(VoLTE 가입자)·`alert_order`·감청/청취 범위(manager)·listed 대상 선택), 가입자 편집의 `pickup_group` 은 `dg-` 파생값이면 잠금 표시, `McpttProfile.allow_ambient_listening` 타입 · **장애>감사 이력**(`/alerts/audit`, manager — `AuditEventsSection`/`core.audit-history` 위젯, §5.7) | 구현 |
| **OAM** | `GET /api/v1/events` — `kind=audit` 열람 manager 게이트(미만은 결과 제외·명시 조회 403)·`code=` 필터 | 구현 |
| **OAM 게이트웨이** | csc `pkg.json` `gateway.routes` + `oam.json Gateway.Routes` 시드에 `/api/v1/dispatch-groups` | 구현 |
| **단말 SDK `libcimsue`** ([ue_sdk.md](ue_sdk.md)) | `calledParty`(P-Called-Party-ID), `dialogWatch`(RFC 4235)·`join`(RFC 3911 recvonly, 200 OK a=ssrc 라벨 → `sources`), `pickup`, `transfer`, `joinGroupCall(listenOnly)` — `cimsue-cli` 로 dev 실측(Join 200·감청 RTP·caller/callee 라벨·픽업·REFER) | 구현 |
| **단말 앱(관제용 UI)** | dialog 목록·클릭→Join, SSRC 별 활성/레벨 표시(U10 관측 API 후속), PTT 청취 채널 UI(U6) — 화면 설계 정본 [dispatch_desktop_ui.md](dispatch_desktop_ui.md)(Windows WPF, 다섯 구획·배너·핫키·응답 코드 문구) | 앱 파트 (UI 설계 완료, 구현 전) |
| **cspsim** | `hunt`(`-pilot`, `-hunt_noanswer`, `-hunt_pickup` — D 의 `<code><pilot>` 지정 픽업, 마커 `pickup_status`/`t_answer_ms`) · `monitor`(dialog 구독→INVITE-Join 청취, 마커 `join_status`/`M_ssrc`/A·B·M RTP delta — SSRC 2개·은닉 판정) · `ptt_listen`(멤버 그룹콜 중 M 의 recvonly INVITE, `-listen_sendrecv` 비멤버 대조 — 마커 `join_status`/`M_recv`/`M_grant`/`M_deny`/`hidden`) · 수신 SSRC 집합·floor DENY/TAKEN 카운터·conference 로스터 누적 | 구현 |

②의 포크 집합이 유일한 구조 변경이고 나머지는 기존 훅·계약의 연장이다.

**포크 집합의 위치(구현 결정)**: 대기 B-leg 는 `CCallMap`(leg 쌍 1:1 모델) 밖의 TAS 소유 맵에 두고, 승자 확정
시점에 (A, 승자) 쌍을 `CCallMap` 에 넣어 이후를 기존 1:1 경로(answer RELAY_MODIFY·re-INVITE·BYE·sweeper)에
넘긴다. 패자 leg 는 CANCEL 후 최종 응답(487)이 올 때까지 TAS 맵에 남아 이벤트를 흡수한다. B-leg INVITE 의
`To` 는 B2BUA 관례대로 그룹원 AoR 이고 `P-Called-Party-ID` 가 대표번호다(§4.3 — GroupCallService fan-out 과
동형). 포크는 공유 relay(peer1 포트 공용) 위에서만 성립하므로 RTP relay 비활성 노드에서는 503 이다.

---

## 8. 설정 정리 (운영 규약)

### 8.1 DB 스키마

```sql
-- sql/migrate_dispatch_groups.sql (재실행 안전)
CREATE TABLE IF NOT EXISTS dispatch_groups (
    id              VARCHAR(64)  NOT NULL COMMENT '불변 키 (CSC 발급 dg-xxxxxxxx) — pickup_group 값·상관 키',
    name            VARCHAR(128) NOT NULL DEFAULT '' COMMENT '표시 이름',
    pilot_id        VARCHAR(64)           DEFAULT NULL COMMENT '대표번호(AoR user part). NULL=대표번호 없음',
    service_ref     VARCHAR(64)           DEFAULT NULL COMMENT '대표번호 접속서비스 name',
    alert_mode      ENUM('parallel','sequential') NOT NULL DEFAULT 'parallel' COMMENT 'TS 24.239 alerting mode',
    no_answer_sec   INT          NOT NULL DEFAULT 30,
    busy_members    ENUM('skip','alert') NOT NULL DEFAULT 'skip',
    overflow_target VARCHAR(64)           DEFAULT NULL COMMENT '무응답 넘김 대상(대표번호/내선). NULL=480',
    monitor_scope   ENUM('none','own','listed','all') NOT NULL DEFAULT 'none',
    ptt_listen      ENUM('none','listed','all')       NOT NULL DEFAULT 'none',
    listen_visibility ENUM('hidden','visible')        NOT NULL DEFAULT 'hidden' COMMENT 'PTT 청취 멤버 로스터 노출',
    org_id          INT                   DEFAULT NULL,
    created_at      DATETIME              DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_pilot (pilot_id),
    CONSTRAINT fk_dg_org FOREIGN KEY (org_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='관제 그룹';

CREATE TABLE IF NOT EXISTS dispatch_group_members (
    user_id     VARCHAR(64) NOT NULL COMMENT '가입자 id — 가입자당 그룹 하나',
    group_id    VARCHAR(64) NOT NULL,
    alert_order INT         NOT NULL DEFAULT 0 COMMENT 'sequential 호출 순서',
    PRIMARY KEY (user_id),
    KEY idx_group (group_id),
    CONSTRAINT fk_dgm_group FOREIGN KEY (group_id) REFERENCES dispatch_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='관제 그룹 멤버';

CREATE TABLE IF NOT EXISTS dispatch_group_monitor_targets (
    group_id        VARCHAR(64) NOT NULL,
    target_group_id VARCHAR(64) NOT NULL,
    PRIMARY KEY (group_id, target_group_id),
    CONSTRAINT fk_dgt_group  FOREIGN KEY (group_id)        REFERENCES dispatch_groups (id) ON DELETE CASCADE,
    CONSTRAINT fk_dgt_target FOREIGN KEY (target_group_id) REFERENCES dispatch_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='monitor_scope=listed 대상';

CREATE TABLE IF NOT EXISTS dispatch_group_ptt_targets (
    group_id     VARCHAR(64) NOT NULL,
    ptt_group_id BIGINT      NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    PRIMARY KEY (group_id, ptt_group_id),
    CONSTRAINT fk_dgp_group FOREIGN KEY (group_id)     REFERENCES dispatch_groups (id) ON DELETE CASCADE,
    CONSTRAINT fk_dgp_ptt   FOREIGN KEY (ptt_group_id) REFERENCES ptt_groups (id)      ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='ptt_listen=listed 대상';
```

PTT 그룹콜 청취 자격은 `ptt_user_profile` 에 컬럼을 신설한다(TS 24.484, §5.6):

```sql
-- sql/migrate_ptt_ambient_listening.sql (재실행 안전 — 컬럼 존재 시 no-op)
ALTER TABLE ptt_user_profile
  ADD COLUMN allow_ambient_listening TINYINT(1) NOT NULL DEFAULT 0
    COMMENT 'allow-ambient-listening (TS 24.484 ruleset) — 원격 청취 수행 자격 (관제사)';
```

`volte_subscriptions.pickup_group` 컬럼은 그대로 쓴다(값 = `dispatch_groups.id`). 기존 자유 문자열
값은 마이그레이션 시 같은 값의 관제 그룹(대표번호 없음)으로 승격해 축을 맞춘다. 컬럼 미적용 DB
에서는 CSP 가 부팅 프로브로 감지해 관제 기능 전체를 비활성(INFO 로그)한다 — 기존
`pickup_group` 프로브와 같은 방식.

### 8.2 CSC 관리 API

`/api/v1/dispatch-groups` — `GET`(목록) / `POST` / `GET|PUT|DELETE /{id}` /
`POST /{id}/members` / `DELETE /{id}/members/{user_id}` / `PUT /{id}/monitor-targets` /
`PUT /{id}/ptt-targets`. PTT 그룹 API([../../api/admin_api.md](../../api/admin_api.md) §6)와 동형.
검증: `pilot_id` 가 `volte_subscriptions.id`/`ptt_subscriptions.id`/다른 pilot 과 충돌 → 409;
`monitor_scope≠none` 그룹에 `users.role` 이 operator 미만인 가입자 편입 → 403.

### 8.3 접속서비스·csp.json

신규 서비스 필드 없음 — 대표번호는 `dispatch_groups.service_ref` 로 서비스를 가리킨다.
csp.json `sections.tas` 신규 키:

| 키 | 기본 | 의미 |
|---|---|---|
| `Setup.Sip.Dispatch.MaxTapsPerSession` | 2 | 세션당 감청 leg 상한(§5.5) |
| `Setup.Sip.Dispatch.MaxForkTargets` | 32 | 대표번호 1건이 동시 포크하는 멤버 상한(제어평면 부하 방어 — 초과분은 `alert_order` 순 절삭). [../csp_control_plane_load_hardening.md](../csp_control_plane_load_hardening.md) |
| `Setup.Sip.Dispatch.ForkRingTimeoutSec` | 60 | `no_answer_sec` 상한(그룹 값이 이를 넘으면 clamp) |

### 8.4 단말

> 관제용 앱의 구현 토대는 [ue_sdk.md](ue_sdk.md)(C++ 코어 `libcimsue` + Android/Windows SDK) 이며, 아래 요건과
> 코어 API 의 대응표는 그 문서 §7 이다.

관제용 앱은 `/provisioning/me` 의 `dispatch` 블록으로 자기 데스크(그룹·대표번호·범위)를 안다.
Join INVITE 는 `Supported: join` 을 싣고, SDP 는 `a=recvonly` + 통화 표준 코덱(AMR-WB) +
SDES crypto(서비스 `media_srtp` 에 따름). 미디어 수신부는 한 m-line 의 **SSRC 2개를 디먹스해 각각
디코딩 후 로컬 믹스**해 재생하고, `a=ssrc … label`(RFC 5576)로 발신자/착신자를 구분 표기해야 한다
(§5.4 — U10 과 같은 수신 구조). 양측 영상 감청 시 영상 SSRC 2개를 격자 합성 렌더한다. 이 능력은
관제 단말의 필수 요건이다.

---

## 9. 검증

cspsim 시나리오(3~4 단말)와 S3 항목. 판정 정본은 기존 방식 그대로 — 각 단말의 **누적 수신 RTP delta**
+ 요청별 **최종 응답 마커**.

| 항목 | 검사 | 판정 |
|---|---|---|
| `S3-SCN-FA` | F1 병렬 호출·응답 | A→pilot, B·C 링, C 응답 → A·C 미디어, B 는 487 마커·무흐름 |
| | F2 응답 경합 | B·C 동시 200 → 한쪽만 확립, 다른 쪽 BYE 마커, A 는 200 1건 |
| | F3 무응답 | 전원 무응답 → `no_answer_sec` 후 A 480(overflow 없음) / overflow 내선 D 로 재시도(있음) |
| | F4 통화 중 제외 | B 통화 중(`busy_members=skip`) → C 만 링 |
| | F5 지정 픽업 | B·C·D 전원 ring-hold, D 가 `**<pilot>` → D 가 받음(포크 집합 재키잉·RELAY_MODIFY), `pickup_status=200`, A·D 미디어, B·C 무흐름 |
| | F6 sequential | `alert_mode=sequential`, `no_answer_sec=4` → B 먼저 링, 단계 시한 뒤 CANCEL → C 링·응답, `t_answer_ms ≥ 4000` |
| `S3-SCN-MONITOR` | M1 목록 | M 의 A dialog 구독(범위 안) → 200 + NOTIFY ≥1 |
| | M2 청취 | M Join INVITE → 200, M 수신 RTP delta>0 (SSRC 2개), A·B delta 변화 없음 |
| | M3 은닉 | A·B 에 re-INVITE/NOTIFY 0건, 같은 그룹 D 의 dialog NOTIFY 에 M leg 없음 |
| | M4 상향 차단 | M 송신 RTP → A·B 수신 delta 무변화 |
| | M5 인가 | 범위 밖 그룹의 M' → 구독 403 / Join 403; 미지 Call-ID → 481 |
| | M6 종료 | A BYE → M 에 BYE 수신 마커, CMP tap 회수(STATS `taps` 0) |
| | M7 감사 | `E-AUD-016` 시작·종료 2건 |
| `S3-SCN-PTT-LISTEN` | L1 청취 합류 | 멤버 A·B 그룹콜 중 M(비멤버, `allow_ambient_listening=1`, 관제 그룹 `ptt_listen=all`) recvonly INVITE → 200, M 수신 RTP delta>0, M floor 요청 → DENY(GRANT 0), A 의 conference 로스터에 M 없음(hidden) |
| | L2 자격 없음 | `allow_ambient_listening=0` → 403 |
| | L3 범위 밖 | `ptt_listen=none` → 403 |
| | L4 비멤버 일반 INVITE | sendrecv → 403 |
| | L5 공개 청취 | `listen_visibility=visible` → 200 + 로스터에 M(`roles` listener) |
| `S1` | CMP tap 단위(복사·PT 스탬프·상향 폐기·세션 종료 회수), CSP `Join` 파서·`MatchDialog` 단위 | gtest |

```bash
./cims-verify run --items S3-SEED,S3-SCN-FA          # F1·F3·F5·F6 대표번호 호출 (parallel/sequential·overflow·픽업)
./cims-verify run --items S3-SEED,S3-SCN-MONITOR     # M2·M5 감청
./cims-verify run --items S3-SEED,S3-SCN-PTT-LISTEN  # L1~L5 PTT 그룹콜 청취
```

`S3-SCN-PTT-LISTEN` 은 S3-SEED 의 PTT 자격 창(멤버 A·B)과 대상 그룹의 **비멤버** PTT 가입자(M)를 쓰고, M 의 관제
그룹(`dg-vfy-lsn-<group>`)과 `ptt_user_profile.allow_ambient_listening` 을 검사별로 시드·복원한다.

S3-SEED 가 관제 그룹 2개(대표번호 있는 `dg-verify-a`: A 제외 B·C·D 멤버 / `dg-verify-b`: M' 멤버,
범위 없음)와 감시 그룹(`monitor_scope=all` 의 M)을 시드하고 종료 시 복원한다(자기복원).

---

## 10. 범위 외 / 향후 과제

- **끼어들기(barge-in)·3자 통화** — 관제사의 상향을 A/B 에 섞으려면 믹서가 필요하다. CMP MIX
  예약 기능(`(service, conf_id)`)의 실체화로 다룬다. tap 은 그 전 단계다.
- **TS 24.379 ambient listening**(remote-init 1:1) — 그룹콜 청취와 같은 `allow_ambient_listening`
  자격(§5.6)을 재사용하되, 단말 무표시 자동응답 + CSP `session-type=ambient-listening` 시그널링이
  추가로 필요하다. 단말 파트 선행.
- **History-Info(RFC 7044)** — 대표번호 재타게팅 이력의 표준 표현(§4.3, 현재 `P-Called-Party-ID` 로 대체).
- **3GPP LI 핸드오버(HI2/HI3·LEMF)** — 외부 사법기관 인도가 요구되면 별도 LI 게이트웨이(§5.8). 본 설계 범위 밖.
- **관제사 겸임(N:M 멤버십)** — 채택하지 않는다(§3.2 확정). 겸임 요구는 `overflow_target`·지정 픽업으로
  흡수한다.
- **RFC 4662 RLS** 목록 구독(§5.2 표준형), **큐/ACD**(대기열·순번 안내), 대표번호 **발신 표시**(관제사가
  대표번호로 걸 때 `P-Preferred-Identity`=pilot).
- **청취 범위 그룹의 conference 이벤트 구독 인가** — 관제 앱의 PTT 세션 목록([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.3)은
  RFC 4575 conference 구독(`onRoster`)으로 "진행 중·참가자 수" 를 알아야 하는데, 현재 conference SUBSCRIBE 인가는 그룹 멤버 기준이다.
  청취 leg 와 같은 축(`allow_ambient_listening` 자격 + `CanListenPtt` 범위)으로 비멤버 관제사의 구독을 허용한다 — `listen_visibility=hidden`
  그룹이라도 관제사 자신이 받는 로스터에는 영향이 없다(청취 멤버 제외 규칙은 그대로).
- Android UE 의 Join 발신·SSRC 디먹스 UI — 서버 완성 후 단말 파트.

---

## 11. 문서 갱신 대상 (구현과 같은 변경에서)

- [volte_supplementary_services.md](volte_supplementary_services.md) §9 — "그룹 착신(hunt group) 별도 설계" 를 본 문서 참조로.
- [registration_binding_set.md](registration_binding_set.md) §2.2 — "병렬 포크 금지" 는 **한 사람의 멀티 디바이스** 범위임을 명시(그룹 포크는 §4).
- [../../api/cmp_media_api.md](../../api/cmp_media_api.md) — §6.5 `RELAY_TAP_*`(분리 인도·`a=ssrc` 라벨링·RTCP SR), §5.1 `resource.tap`, §5.2 STATS `taps`, §9 `LIMIT`.
- [../db_schema.md](../db_schema.md) — `dispatch_groups` 계열 4 테이블, `pickup_group` 값 의미, `ptt_user_profile.allow_ambient_listening`.
- [../../api/admin_api.md](../../api/admin_api.md) — `/api/v1/dispatch-groups`.
- [../alarm_catalog.csv](../alarm_catalog.csv) — `E-AUD-016 call_monitored` 정의·감지 행.
- [recording.md](recording.md) — `call.json` `dispatch_group/pilot/alerted/answered_by/monitors[]`.
- [android_ue_provisioning.md](android_ue_provisioning.md) — `/provisioning/me` `dispatch` 블록.
- [mcptt_authorization.md](mcptt_authorization.md) — 감청 편입 RBAC(`manager` 승인·감사 열람 분리, §5.7/§5.8).
- [../csp_control_plane_load_hardening.md](../csp_control_plane_load_hardening.md) — 포크 팬아웃 상한 `MaxForkTargets`(§8.3).
- [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §R1 — ambient listening 행에 본 문서 §5.6/§10 참조.
