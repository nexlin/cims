# MCPTT Emergency / Imminent-Peril / Alert / Ad-hoc 지원 설계

3GPP MCPTT 규격(TS 22.179 / 23.379 / 24.379 / 24.380 / 24.481 / 24.483·24.484)에 따라
**긴급(emergency)·임박위험(imminent peril)·긴급경보(emergency alert)·애드혹(ad hoc)** 그룹콜을
기존 CIMS PTT(prearranged/chat/broadcast)에 추가하는 설계.

---

## 1. 설계 원칙 (규격 정합)

1. **조건(condition)은 group_type가 아니다.** emergency·imminent-peril은 prearranged/chat/broadcast
   **위에 얹히는 런타임 상태**다. group_type enum을 늘리지 않는다.
   - 사용자 단위: **MCPTT emergency state**
   - 그룹/세션 단위: **in-progress emergency / in-progress imminent peril**
2. **두 축은 직교**: `session-type ∈ {prearranged,chat,broadcast,adhoc}` × `condition ∈ {normal,imminent,emergency}` + `alert`(별도 신호). SIP `mcptt-info+xml`에서 `<session-type>`과 `<emergency-ind>/<imminentperil-ind>/<alert-ind>`가 별도 필드인 규격과 일치.
3. **Floor 우선순위 서열(TS 24.380)**: `emergency > imminent-peril > (chair) > 수치 priority`. 기존 chair/priority 비교 **앞에** condition tier를 삽입.
4. **능력 게이트(capability)와 런타임(state) 분리**: "이 그룹/사용자가 긴급을 *할 수 있는가*"(설정, TS 24.481/24.483)와 "지금 긴급 *상태인가*"(런타임)는 다른 레이어.
5. **점진 적용·하위호환**: 신규 컬럼 기본값은 기존 동작 보존. 조건 미지정 호는 현재와 동일.

---

## 2. 데이터 모델 (DB)

`ptt_groups` capability (TS 24.481 group config, `sql/cims_schema.sql`):

- `emergency_call` — allow-MCPTT-emergency-call. **condition(긴급·임박위험) 공통 게이트** —
  두 tier 는 floor 서열만 다르고 능력 축은 하나로 둔다(별도 임박위험 컬럼 없음).
  XCAP `allow-imminent-peril-call` 은 이 값의 미러로 산출한다.
- `emergency_alert` — allow-MCPTT-emergency-alert (기본 1).

`ptt_user_profile` — 사용자 MCPTT 프로파일 (TS 24.484 user profile, 키=`ptt_subscriptions.id`):

- `allow_emergency_call`·`allow_emergency_alert`·`allow_adhoc_call` — 사용자 단위 개시 인가
  (기본 1=허용). ad hoc 은 규격 요소가 없어 자체 정책 컬럼(시스템 `Setup.PttAdhocEnabled` 와 AND).
- `emergency_group_mode` — SOS(새 긴급콜) 대상 결정 (TS 24.484 `MCPTTGroupInitiation` entry-info):
  `DedicatedGroup`(기본, 전용 긴급그룹으로) | `UseCurrentlySelectedGroup`(단말 선택 그룹으로).
- `emergency_group_id` — 전용 긴급그룹(`ptt_groups.mcptt_group_id` FK, 삭제 시 NULL). 콜·경보
  (`EmergencyAlert` entry) 공통 대상. **DedicatedGroup 모드에서 미지정이면 긴급 개시가 전부
  미인가**(403) — 콘솔이 지정을 필수화하고 미지정을 경고 배지로 표시한다.
- `allow_emergency_private_call` — 긴급 사설콜 개시 인가 (TS 24.484 ruleset
  `allow-emergency-private-call`, 기본 1). 사설콜은 그룹문서가 없어 capability 축이 공허 —
  사용자 축이 유일 게이트(§7).
- `private_emergency_mode` — 긴급 사설콜 대상 결정 (TS 24.484 `MCPTTPrivateRecipient` entry-info):
  `LocallyDetermined`(기본, 발신자가 상대 지정) | `UsePreConfigured`(사전 지정 수신자 고정).
- `emergency_private_recipient` — 사전 지정 긴급 수신자(`ptt_subscriptions.id` FK, 해지 시 NULL).
  **UsePreConfigured 모드에서 미지정이면 긴급 사설콜 미인가**(403). 적용은
  `sql/migrate_ptt_user_profile_v3.sql` — CSP/CSC 코드 배포보다 선행.
- 행 부재 = 기본값(모드 DedicatedGroup·긴급그룹 미지정·인가 전부 허용)으로 판정.

ad hoc 은 그룹 컬럼을 두지 않는다 — 임시 그룹은 비영속 in-memory(ephemeral)로만 존재하고(§6),
판정 시점에 그룹이 없으므로 인가는 사용자(`allow_adhoc_call`)/시스템(`Setup.PttAdhocEnabled`)
정책으로 건다.

**미구현 (향후 과제, §10)**:
- in-progress 상태 DB 미러 — 현재 CSP 인메모리(`m_mapGroupCondition`) + events.jsonl 관측만.
  CSP 재기동 시 진행 중 긴급 상태는 소실된다.

---

## 3. CMP — Floor 우선순위 tier (TS 24.380)

### 3.1 우선순위 모델

`Peer`(또는 보조 맵)에 condition tier 추가:

```cpp
enum FloorTier { TIER_NORMAL = 0, TIER_IMMINENT = 1, TIER_EMERGENCY = 2 };
// PMcpttGroup 내부
std::map<std::string,int>      _priorities;   // 수치 우선순위(TS 24.380: 0~255, 클수록 우선)
std::map<std::string,std::string> _roles;     // 기존 chair/participant
std::map<std::string,int>      _tier;          // 신규: 세션별 현재 condition tier
```

**유효 우선순위 = (tier, chairFlag, -numericPrio)** 사전식 비교. `handleFloorRequest`의 기존 비교(현 `PMcpttGroup.cpp` chair override 블록) **앞에** tier 비교 삽입:

```cpp
int rt = tierOf(requester), ot = tierOf(owner);
bool bPreempt;
if      (rt != ot)               bPreempt = (rt > ot);        // emergency > imminent > normal
else if (requesterChair!=ownerChair) bPreempt = requesterChair; // 동tier면 chair 우선(기존)
else                              bPreempt = (reqPrio < ownPrio); // 동tier·동role 수치(기존)
```

- broadcast 독점 검사는 **그대로 최우선**(기존). 단, **개시자의 emergency**는 그대로 통과(개시자만 floor).
- emergency 발언자는 **T2(최대 발언시간) 제한에서 제외**한다(긴급 중 장시간 발언 허용).
  imminent 발언자는 T2 를 일반 적용한다.
  T1(무RTP 발언 종료 판정)은 tier 와 무관하게 동일 적용된다 — 규격상 T1 은 징벌이 아니라
  "발언이 끝났다"는 판정이고, 긴급 화자만 예외로 두면 조용해진 긴급 화자가 floor 를 무한
  점유해 다음 긴급 발언자가 막힌다.

### 3.2 tier 설정 경로

- `PTT_GROUP_ADD`/`PTT_GROUP_MODIFY` payload에 그룹 condition: `"emergency":0/1`, `"imminent":0/1`(세션 전체 in-progress)와, 필요 시 **개시자(또는 특정 멤버) tier**.
- 멤버 tier 갱신: `PTT_JOIN`의 `"tier":"normal|imminent|emergency"` 필드, 또는 경량 명령 `PTT_FLOOR_TIER {group_id, session_id, tier}`(업그레이드/취소 시 floor만 갱신, 미디어 재협상 불필요 — [../modules/cmp.md](../modules/cmp.md) §3.2).
- members 문자열 확장은 **하위호환 유지**: `id:prio:role[:tier]` (4번째 토큰 옵션, 미존재 시 normal).

### 3.3 wire 포맷(floor 패킷)

현 RTCP-APP `FloorControlPacket.reserved`(2B 미사용)에 tier/flag 인코딩:
```
reserved(16bit): [15:14]=tier(0~2) [13]=chair [12:8]=numericPrio(0~31) [7:0]=spare
```
- 단, CMP는 floor 판정을 중앙 맵(`_tier/_priorities/_roles`)으로 수행하므로 **패킷 필드는 관측·UE 호환용**(선택). UE가 REQUEST에 priority를 실어 보내면 파싱해 반영, 없으면 중앙값 사용. → **하위호환**.

### 3.4 로깅

`floor.jsonl`/flow의 floor 이벤트에 `"tier":"emergency"`, GRANT/REVOKE에 `"reason":"emergency_preempt"|"imminent_preempt"` 추가(기존 `_logFloorLocal` extraJson 활용).

---

## 4. CSP — 시그널링 (TS 24.379)

### 4.1 수신 mcptt-info 파싱 (공용 유틸)

`McpttInfo.{h,cpp}`: multipart/mixed 바디에서 `application/vnd.3gpp.mcptt-info+xml` 파트를 추출하고
얕은 XML에서 다음을 읽는 경량 파서(정규식/문자열 스캔, 외부 의존 없이):
```
session-type, emergency-ind, imminentperil-ind, alert-ind, broadcast-ind,
mcptt-request-uri, mcptt-calling-user-id, (alert) originated-user-id, location(있으면)
```
`EventIncomingCall`/`EventReInvite`에서 `CSipMessage*`로 바디 접근 → 파싱.

### 4.2 긴급 개시 / 업그레이드 / 취소

- **개시**: 그룹 INVITE에 `emergency-ind=true` → `ProcessGroupCall(condition=EMERGENCY)`:
  1. **개시 인가 3중 판정** (TS 24.379 §6.3.3.1.13.2, `IsConditionInitAuthorized`) — condition
     (긴급·임박) 공통: ①그룹 capability `emergency_call` ②사용자 프로파일 `allow_emergency_call`
     ③`DedicatedGroup` 모드면 호출 대상=전용 긴급그룹 일치. 미인가는 **403 거절**(§6.3.3.1.14) —
     단말이 normal 재발신으로 폴백한다(강등 수용 아님). 프로파일 DB 불가 시 사용자 축은
     fail-open(그룹 축만 판정).
  2. in-progress emergency 설정(메모리 + DB 미러), 개시자에 MCPTT emergency state.
  3. CMP `ADD/PTT_GROUP_MODIFY`에 `emergency=1`(+개시자 tier=emergency) → floor 선점 보장.
  4. fan-out INVITE의 `mcptt-info`에 `<emergency-ind>true` 광고(`BuildGroupInfoXml` 확장).
  5. 자원 우선순위: 송출 INVITE에 `Resource-Priority` 헤더(RFC 4412/8101) 부가 —
     emergency=`mcpttp.15` / imminent=`mcpttp.8` / normal=`mcpttp.0` (mcpttp 서열은 .0 최저~.15 최고).
  6. **조인/재조인 200 OK 동봉**: 진행 중 조건(긴급/임박) 세션에 조인하면 200 OK 를
     multipart(mcptt-info + SDP)로 보내 현재 `emergency-ind` 를 동봉 — 조인 단말이 개시자의
     다음 발언(floor TAKEN)을 기다리지 않고 즉시 세션 긴급 표시를 갖는다. 활성 세션에
     같거나 낮은 조건으로 조인해도 **세션 조건은 유지**된다(조인은 하향이 아니다 — 하향은
     개시자의 취소 re-INVITE 만). 조건 리셋은 새 세션 개시 시에만, 세션 종료 시에도 정리
     (`RemoveGroupSesId`). 활성 세션 조인이 조건을 상향시키면(normal 진행 중 긴급 조인)
     기존 확립 멤버 leg 에도 re-INVITE 재광고(아래) — fan-out INVITE 는 미참여 멤버만 커버.
- **업그레이드**: 진행 중 그룹콜에 re-INVITE(`emergency-ind=true`) → dispatcher `RecvRequest` 의
  **PTT 인지 분기** → 개시와 동일한 3중 인가(`IsInCallUpgradeAllowed`) 적용. 미인가 상향은
  **403 + mcptt-info(`emergency-ind=false`)** 로 거절(§6.3.3.1.14) — 재-INVITE 거절은 다이얼로그를
  깨지 않아 호는 normal 유지, 단말은 낙관 latch 를 되돌린다. 인가되면 `PTT_FLOOR_TIER`/
  `PTT_GROUP_MODIFY`로 floor 격상 + **확립 멤버 leg 에 re-INVITE 재광고**
  (`PropagateConditionToMembers`, TS 24.379 §6.3.3.1.15) — mcptt-info 의
  `emergency-ind`/`imminentperil-ind` 를 true/false 로 **명시**하고 actor(변경 유발 멤버) leg 는
  제외한다. SDP 는 초기 오퍼와 동일 구성(audio=멤버 전용 포트 + m=application=floor 포트)으로
  재산출되어 미디어 불변 — psip 2단계 API(`CreateReInvite` 생성 → mcptt-info multipart 부가 →
  전송, `AcceptCall` 2단계도 동일 패턴)로 구현. 단말 pjsua 의 자동 200 OK 응답은 psip
  `EventReInviteResponse`(CSP no-op)로 격리된다.
- **취소**: 권한자(개시자 또는 authorized_user)만 `emergency-ind=false`로 해제 → tier normal 복귀,
  상태 클리어. 비권한자 취소 무시(규격). 하향도 확립 멤버 leg 에 re-INVITE(`emergency-ind=false`)
  재광고(§6.3.3.1.16) — 수신 단말 세션 긴급 표시의 정본 해제 신호(경보 취소 MESSAGE 정합은
  보조, §4.3).
- **imminent peril**: 동일 경로의 `imminentperil-ind`, tier=IMMINENT. capability 는
  `emergency_call` 공통 게이트를 따른다.

### 4.3 emergency alert (SIP MESSAGE)

`EventMessage`에서 `mcptt-info`(`alert-ind=true`) 판별 → SMS 경로와 분기:
- alert 게이트 = 그룹 capability(`emergency_alert`) AND 사용자 프로파일
  (`allow_emergency_alert`, TS 24.484 allow-activate-emergency-alert) — 허용 시에만 그룹 등록
  멤버에게 같은 MESSAGE 를 fan-out(발신자 제외, affiliation 요구 그룹은 affiliate 된 멤버만,
  **원본 Content-Type `application/vnd.3gpp.mcptt-info+xml` 보존** — 단말이 content-type 으로
  경보를 분기한다). 미인가 경보는 **거절이 아니라 스트립**(무전파 — 규격이 콜(403 거절)과
  다르게 정의). 취소(`alert-ind=false`)는 동일 본문 전파이며 **사용자 게이트 비대상**(잔존
  경보 정리 경로 보존). 등록(온라인) 멤버에게만 전달된다 — 저장 후 전달(경보 보류함)은 없다.
- `CallDir`에 `alert_sent`/`alert_cancelled` 이벤트 기록(그룹 events.jsonl).
- **단말(ptt-client)**: SOS 개시가 규격 시퀀스대로 **경보 MESSAGE 를 먼저** 보내고 긴급콜을
  개시한다(`McpttXml.alertInfo` + `PttController.sendAlert` — 호 성립과 무관하게 신원·그룹 전파).
  SOS 해제 시 경보 취소도 함께. 수신 측은 mcptt-info MESSAGE 를 파싱해 활성 경보
  상태(`alerts` StateFlow)로 들고, 전 탭 상단 배너(`AlertBanner`)+경고음으로 표시 —
  발신자 취소로 자동 해제, [닫기]는 로컬 표시만 제거. 이력 이벤트 `ALERT/ALERT_IN/ALERT_END`.
- **단말 SOS 대상 결정** (새 긴급콜 — 통화 중이면 항상 현재 주채널 통화 격상): user-profile
  문서의 `MCPTTGroupInitiation` entry-info 를 따른다 — `DedicatedGroup`(기본)이면 프로비저닝된
  전용 긴급그룹(미지정 시 "전용 긴급그룹 미지정" 불발), `UseCurrentlySelectedGroup` 이면
  **선택 그룹 = 마지막 주채널**(`ChannelStore.lastPrimary` 영속 — 참여 전부 이탈 후에도 유지,
  이력 없으면 그룹 목록 첫 그룹 폴백). 프로파일 미수신이면 선택 그룹으로 현행 유지(서버
  게이트가 최종 판정). 경보 MESSAGE 도 같은 대상 그룹으로 보낸다(`EmergencyAlert` entry 공통).
- **단말 403 폴백**: 긴급 개시 INVITE 가 403 이면 같은 그룹으로 normal 재발신(호 자체는 보존),
  in-call 상향 re-INVITE 가 403(`emergency-ind=false` 본문)이면 낙관 latch 를 되돌린다
  (`SipController.emergencyDenied` — tsx 원문 관측, 재-INVITE 거절은 CallState 불변이라 별도 이벤트).
- **403 폴백 시 선발신 경보 정합**: 경보와 콜은 별개 게이트(서버: 경보 미인가=스트립·무전파)
  라 콜 403 만으로 경보를 일괄 자동회수하지 않는다. 단말은 403 수신 시 user-profile 을
  재조회(`reconcileAlertAfterDenied` — ETag 캐시라 저비용)해 **경보 인가까지 없다고 판명되면**
  (=서버가 스트립해 아무도 받지 못한 유령 배너) 로컬 표시만 회수한다 — 서버에 활성 경보가
  없으므로 취소 MESSAGE 는 보내지 않는다. 경보 인가가 확인되면(실전파된 경보) 유지 — 해제는
  사용자의 SOS 해제로. 재조회 실패 시도 유지(fail-open).
- **미인가 발신자 로컬 배너 정책(확정)**: 프로파일이 명시적으로 미인가면 발신 자체를 선차단
  (`PttFeedback.blocked` 거부음+토스트)하고 로컬 경보 배너를 켜지 않는다 — 전파되지 않을
  경보의 거짓 안심 상태를 만들지 않는다. 프로파일 미수신(null)이면 낙관 발신 유지(서버
  게이트가 최종 판정, 사후 정합은 위 403 폴백 경로).
- **수신측 세션 긴급 표시와의 정합**: 수신 단말의 in-call 긴급 표시(`session.emergency`)의
  **정본 신호는 CSP 의 조건 재광고** — 상향/하향 re-INVITE(멤버 전파)와 조인/재조인 200 OK
  동봉(§4.2)을 tsx 원문에서 파싱해(`SipController.sessionEmergency`) latch/un-latch 한다
  (un-latch 는 비개시자 한정 — 개시자는 자기 취소로만 해제). 보조 신호 2종을 유지한다:
  ①floor TAKEN 의 emergency 비트 latch(재광고 유실·발언 선행 케이스) ②경보 취소 수신 시
  같은 그룹에 잔여 활성 경보가 없으면 latch 해제(비개시자 한정). UI 는 두 배너를 별개 신호로
  **동시 표시**한다 — 경보 배너(주황·📢·발신자 표기)와 세션 긴급 배너(`EmergencyBanner`,
  빨강 깜빡임·🚨·그룹 표기 "긴급 통화")로 시각 구분. 비개시자 배너에는 로컬 [닫기](표시
  latch 만 해제)를 둔다 — 취소 신호 유실 대비 탈출구.

### 4.4 상태/로깅

`CallDir::PttLogEvent` 신규 type: `emergency_activated|emergency_cancelled|imminent_activated|imminent_cancelled|alert_sent|alert_cancelled` (+ actor, reason). `BuildGroupDescriptor`(group.json)에 `inprogress_emergency` 등 반영.

---

## 5. CSC — 능력/프로파일 (TS 24.481/483/484)

- **XCAP group config**(`mcptt.py get_group_xml`): `allow-MCPTT-emergency-call`·
  `allow-imminent-peril-call`(=`emergency_call` 미러)·`allow-MCPTT-emergency-alert`(=`emergency_alert`)
  산출 — 규격 요소는 셋 다 내보내되 설정 축은 둘이다.
- **admin API**(`admin.py` group create/update): `emergency_call`·`emergency_alert` 수용 + INSERT/UPDATE.
- **user profile XCAP**(`mcptt.py get_user_profile_xml`, CMS `/org.3gpp.mcptt.user-profile/...`):
  `ptt_user_profile` DB 연동 산출 — `MCPTT-group-call > EmergencyCall/EmergencyAlert` 의
  `entry-info`+`uri-entry`(SOS 대상 결정, TS 24.484)와 `PrivateCall > EmergencyCall >
  MCPTTPrivateRecipient`(긴급 사설콜 대상 결정, §7), `ruleset`(`allow-emergency-group-call`·
  `allow-activate/cancel-emergency-alert`·`allow-emergency-private-call`). ad hoc 인가는 규격
  요소가 없어 `cims:` 확장 네임스페이스(`cims:allow-adhoc-group-call`)로 노출. ETag 는 내용
  파생(변경 시 자동 갱신).
- **시스템 축 게이트**(`mcptt_service_config` — TS 24.484 service-config): `allow-emergency-call`·
  `allow-alert` 이 위 사용자 인가와 **AND** 로 겹친다(단말 선차단 · 콘솔 `구성 > MCPTT 정책`).
  그룹 축(`emergency_call`/`emergency_alert`)·사용자 축(ruleset)·시스템 축 셋이 모두 허용해야 열린다.
- **admin 프로파일 API**: `GET/PUT /api/v1/users/:pid/ptt/:msisdn/profile` — UPSERT + 캐시 갱신 +
  `USER_CHANGED` notify. `DedicatedGroup` 의 `emergency_group_id` 는 존재 그룹만 수용(400),
  `emergency_private_recipient` 는 존재 가입자만 수용(400).
  사용자 상세(`GET /users/:pid`)의 ptt 행에 `mcptt_profile` 동봉.
- **notify_csp**: capability 변경은 `GROUP_CHANGED`로 전파(CSP lazy-reload), 프로파일 변경은
  `USER_CHANGED`(CSP 는 프로파일을 게이트 판정 시점에 DB 직조회하므로 별도 캐시 무효화 불요).
  in-progress 상태는 CSP→CSC 역방향 보고가 필요할 수 있음(관측용, 선택).

---

## 6. Ad hoc 그룹콜 (TS 22.179 Rel-18)

사전 프로비저닝 없이 개시 시점에 멤버를 동적 구성:

- **개시 입력**: INVITE의 `resource-lists+xml`(멤버 URI 목록). 단말(ptt-client)은 **연락처 탭
  long-press 다중 선택 → [그룹통화 N]** 으로 발신(`PttController.startAdhocCall`).
- **임시 그룹 ID**: 단말이 `adhoc-<발신자번호>-<epoch초>` 로 생성. `adhoc-`/`priv-` 접두사는
  **편성 그룹 ID 예약어** — CSC admin 이 그룹 생성 시 400 거부해 즉석 세션 라우팅과의 충돌을
  차단한다.
- **CSP 처리**: 영속 그룹 없이 **임시 CspPttGroup** 구성(in-memory, `_isAdhoc` — DB 레코드 없음),
  멤버 fan-out, CMP `PTT_GROUP_ADD`. broadcast/emergency 조건도 ad-hoc 위에 얹힘.
- **수명**: 마지막 멤버 이탈 시 즉시 teardown(on-demand와 동일) + GroupMap 에서 제거(ephemeral).
  단말도 대칭 — 애드혹 세션은 채널 영속(ChannelStore)·affiliation·로스터 구독 대상이 아니고
  (참가자는 in-dialog NOTIFY 폴백), 통화 중엔 전용 오버레이(`AdhocCallOverlay`)가 전면 표시되며
  PTT 는 애드혹 세션을 주채널보다 우선한다. chat형 ad-hoc은 비범위.
- **권한**: 시스템 정책 `Setup.PttAdhocEnabled`(csp.json, 미지정 시 허용) AND 사용자 프로파일
  `allow_adhoc_call`(합성 직전 판정, 미인가 403). 판정 시점에 그룹이 존재하지 않으므로 그룹
  속성으로는 성립 불가 — 그래서 사용자/시스템 정책 축이다. 단말도 프로파일로 선차단(UX).
- **긴급 조건과의 결합**: 합성 그룹은 그룹문서가 없어 capability 축이 공허 — `_emergencyCall=true`
  로 합성해 긴급/브로드캐스트 조건이 ad-hoc 위에 얹힐 수 있게 한다(사용자 축 게이트는 그대로
  적용. 단 `DedicatedGroup` 모드 사용자는 대상 불일치로 긴급 ad-hoc 이 미인가 — 규격 외 영역의
  의도된 보수 동작).

---

## 7. 긴급 사설콜 (emergency private call, TS 24.379 §11)

1:1 사설콜(`priv-<caller>-<callee>` 합성 그룹, 계약 §A.1)에 긴급 condition 을 얹는다 —
그룹 긴급콜과 같은 condition 파이프(§4.2)를 그대로 타되, 인가 축이 다르다.

- **인가 (CSP `IsConditionInitAuthorized` private 분기)**: 합성 그룹은 그룹문서가 없어
  capability·Dedicated 축이 공허(`_emergencyCall=true` 합성, ad hoc 과 동일) — **사용자 축이
  유일 게이트**. ①`allow_emergency_private_call` ②`UsePreConfigured` 모드면 사전 지정
  수신자(`emergency_private_recipient`)와 착신자 일치까지 판정(그룹 긴급콜의 DedicatedGroup
  대상 일치와 대칭 — 미지정/불일치 403). in-call 상향(re-INVITE)도 같은 분기를 탄다
  (`IsInCallUpgradeAllowed` → 동일 함수).
- **프로파일 문서 (TS 24.484)**: `PrivateCall > EmergencyCall > MCPTTPrivateRecipient` 의
  `entry-info`(`LocallyDetermined`|`UsePreConfigured`)+`uri-entry`(지정 수신자), `ruleset`
  `allow-emergency-private-call` — §5 참조.
- **단말 (ptt-client)**: 연락처 상세 [긴급] 액션 → `startEmergencyPrivateCall(peer)` — 대상
  결정은 entry-info 를 따른다(`UsePreConfigured`=지정 수신자로 고정·미지정 불발,
  `LocallyDetermined`=고른 상대). INVITE 는 `session-type=private` + `emergency-ind=true`.
  발신 즉시 낙관 latch(세션 긴급 배너+경고음), 403 이면 **일반 1:1 로 폴백**(그룹 긴급콜과
  동일 패턴 — 1:1 은 그룹 경보 선발신이 없어 경보 정합은 불요). 프로파일 명시 미인가는
  선차단(`blocked` 피드백), 미수신(null)은 낙관 발신.
- **CMP**: 별도 처리 없음 — condition 상향 시 CSP 가 `PTT_FLOOR_TIER` 로 개시자 tier 를 올리는
  기존 경로가 사설 2인 floor 세션에도 그대로 적용된다(full-duplex(floor off) 세션은 floor 가
  없어 tier 무의미 — 시그널링·UI 축만 동작).
- **경보(alert)와의 관계**: 긴급 사설콜은 그룹 경보 MESSAGE 를 선발신하지 않는다 — 경보는
  그룹 대상 기능(§4.3)이고, 사설 긴급은 세션 condition 만 올린다.

---

## 8. 콘솔/관측

- **그룹 편집(PttGroupsWorkbenchPage)**: capability 체크박스 — 긴급콜(condition 공통 게이트),
  긴급경보. (임박위험·ad-hoc 은 별도 축을 두지 않는다 — §2.)
- **사용자(ProvisioningWorkbench)**: 사용자 상세에 PTT 번호별 **MCPTT 프로파일 행** — SOS 대상
  모드(`전용 긴급그룹`/`선택 그룹(주채널)`)·전용 긴급그룹 선택·인가 4종(긴급콜/경보/애드혹/
  긴급 사설콜) 편집. 긴급 사설콜 인가 시 **사설 대상**(`단말 선택 상대`/`사전지정 수신자`)과
  `UsePreConfigured` 의 지정 수신자(PTT 번호 — 저장 시 서버 존재검증 400)까지 편집.
  `DedicatedGroup` 저장 시 긴급그룹 지정 필수화(미지정은 "SOS 불발" 경고 배지),
  `UsePreConfigured` 저장 시 수신자 필수화(미지정은 "긴급 사설콜 불발" 배지).
  프로파일 PUT 은 **행 전체 교체**라 콘솔 폼은 항상 전체 행을 보낸다(부분 PUT 은 나머지
  필드가 기본값으로 회귀 — raw API 호출 시 주의).
- **PTT 세션 이력**: 진행 중/과거 emergency·imminent 에피소드, alert 발신 타임라인 표시(events.jsonl·floor.jsonl tier 활용).
- **OAM stats**: 긴급콜/경보 카운터, 진행 중 긴급 그룹 수.

---

## 9. 메시지 흐름 (긴급 개시 예시)

```
UE(개시자) ──INVITE(mcptt-info: session-type=prearranged, emergency-ind=true)──▶ CSP
  CSP: capability/권한 게이트 → in-progress emergency 설정
  CSP ──ADD/PTT_GROUP_MODIFY{emergency=1, initiator tier=emergency}──▶ CMP
  CSP ──200 OK(multipart: mcptt-info emergency-ind=true + SDP)──▶ UE(개시자)
  CSP ──fan-out INVITE(mcptt-info: emergency-ind=true)──▶ 미참여 멤버들
  CSP ──re-INVITE(mcptt-info: emergency-ind=true)──▶ 참여 중(확립) 멤버들   ← §6.3.3.1.15 멤버 전파
  CMP: 개시자 FLOOR_REQUEST → tier=emergency → 기존 발언자 REVOKE(reason=emergency_preempt) → GRANT
  ... 통화 ... (이후 조인/재조인 200 OK 에도 emergency-ind=true 동봉)
UE(권한자) ──re-INVITE(emergency-ind=false)──▶ CSP → PTT_FLOOR_TIER normal → 상태 해제
  CSP ──re-INVITE(mcptt-info: emergency-ind=false)──▶ 확립 멤버들 (un-latch)  ← §6.3.3.1.16
```

---

## 10. 미해결/결정 필요

1. **floor 패킷 priority 필드**: 중앙판정으로 단일화 — tier 는 CSP 지시(`PTT_FLOOR_TIER`,
   긴급 개시자 한정)로만 변한다. REQUEST 의 Floor Indicator(emergency/imminent)는 **호 단위**
   표식(TS 24.380 §8.2.3.15)이라 긴급 호에선 수신 멤버 요청에도 실려 오므로, 요청자 tier
   승격에 쓰면 전원이 emergency 로 비겨 선점이 chair/priority 로 퇴화하고 CSP 사용자 인가도
   우회된다(08-10 실측) — 판정에 쓰지 않는다.
2. **in-progress 상태 DB 미러**: CSP→CSC 역보고 채널이 없으면 관측 정확도 한계. group.json/flow로 관측, DB 미러는 best-effort.
3. **권한자(authorized) 취소 판정**: 개시자 외 authorized_user/관리자 취소 허용 범위.
4. **ad hoc 콘솔(관제) 개시 입구**: 단말 resource-lists 입구는 구현됨 — 관제사가 콘솔에서
   인원을 골라 서버가 개시하는 dispatcher 입구는 미착수.

---

## 11. 관련 파일

- DB: `sql/cims_schema.sql` (`ptt_groups.emergency_call`/`emergency_alert`,
  `ptt_user_profile` — `sql/migrate_ptt_user_profile_v2.sql`)
- CMP: `cmp/PMcpttGroup.{h,cpp}`(tier·선점·로깅), `cmp/PCmpServer.cpp`(명령 파싱)
- CSP: `csp/McpttInfo.{h,cpp}`(파서), `csp/ModuleDispatcher.cpp`(EventIncomingCall/EventReInvite/EventMessage·in-call 403·경보 스트립·ad-hoc 인가), `csp/GroupCallService.cpp`(condition·`IsConditionInitAuthorized`·fan-out·descriptor·`PropagateConditionToMembers`(멤버 전파)·`WrapInfoMultipart`(조인 200 OK 동봉)), `csp/CmpClient.cpp`(필드 전송), `csp/CspPttGroup.{h,cpp}`·`csp/CspUser.h`(`CspUserProfile`)·`csp/DbManager.cpp`(`SelectUserProfile`), `csp/CallDir.h`(이벤트)
- psip: `ext/psip/SipUserAgent`(2단계 API — `CreateReInvite`·`AcceptCall(…, CSipMessage**)`: 생성/전송 분리로 mcptt-info multipart 부가 지점 제공)
- CSC: `csc/src/services/mcptt.py`(XCAP DB연동), `csc/src/handlers/admin.py`(CRUD·user 프로파일)
- 콘솔: `ems/core/console/src/api/{groups,users}.ts`, `.../pages/PttGroupsWorkbenchPage.tsx`,
  `ems/service/console/src/pages/ProvisioningWorkbenchPage.tsx`(MCPTT 프로파일 행)
- 단말: `android/ptt-client/.../PttController.kt`(SOS 대상 결정·403 폴백·user-profile 파싱·
  세션 긴급 latch), `ChannelStore.kt`(lastPrimary),
  `android/core/.../sip/{SipController,CimsCall}.kt`(emergencyDenied·sessionEmergency 관측)
- 문서: 본 문서 + `docs/design/features/ptt_flows.md` 갱신
