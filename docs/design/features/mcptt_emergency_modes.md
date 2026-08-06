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

ad hoc 은 그룹 컬럼을 두지 않는다 — 임시 그룹은 비영속 in-memory(ephemeral)로만 존재하고(§6),
인가는 판정 시점에 그룹이 없으므로 그룹 속성이 아니라 **사용자/시스템 정책** 소관이다(미구현, §9).

**미구현 (향후 과제, §9)**:
- in-progress 상태 DB 미러 — 현재 CSP 인메모리(`m_mapGroupCondition`) + events.jsonl 관측만.
  CSP 재기동 시 진행 중 긴급 상태는 소실된다.
- 사용자 MCPTT 프로파일(TS 24.483 — 긴급/경보 개시 권한, 기본 긴급그룹). 사용자 단위 게이트 없음.

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
  1. 그룹 capability `emergency_call` 게이트 — condition(긴급·임박) 공통. 불허 시 normal 로
     강등 수용(호는 거절하지 않는다). 개시자(사용자) 단위 게이트는 미구현(§9).
  2. in-progress emergency 설정(메모리 + DB 미러), 개시자에 MCPTT emergency state.
  3. CMP `ADD/PTT_GROUP_MODIFY`에 `emergency=1`(+개시자 tier=emergency) → floor 선점 보장.
  4. fan-out INVITE의 `mcptt-info`에 `<emergency-ind>true` 광고(`BuildGroupInfoXml` 확장).
  5. 자원 우선순위: 송출 INVITE에 `Resource-Priority` 헤더(RFC 4412, namespace `mcpttp.x`) 부가.
- **업그레이드**: 진행 중 그룹콜에 re-INVITE(`emergency-ind=true`) → `EventReInvite`의 **PTT 인지 분기** → `PTT_FLOOR_TIER`/`PTT_GROUP_MODIFY`로 floor만 격상, 멤버에 re-INVITE/UPDATE 전파.
- **취소**: 권한자(개시자 또는 authorized_user)만 `emergency-ind=false`로 해제 → tier normal 복귀, 상태 클리어. 비권한자 취소 무시(규격).
- **imminent peril**: 동일 경로의 `imminentperil-ind`, tier=IMMINENT. capability 는
  `emergency_call` 공통 게이트를 따른다.

### 4.3 emergency alert (SIP MESSAGE)

`EventMessage`에서 `mcptt-info`(`alert-ind=true`) 판별 → SMS 경로와 분기:
- alert capability(`emergency_alert`) 게이트 — 허용 시에만 그룹 등록 멤버에게 같은 MESSAGE 를
  fan-out(발신자 제외, affiliation 요구 그룹은 affiliate 된 멤버만). 취소(`alert-ind=false`)도
  동일 본문 전파. 등록(온라인) 멤버에게만 전달된다 — 저장 후 전달(경보 보류함)은 없다.
- `CallDir`에 `alert_sent`/`alert_cancelled` 이벤트 기록(그룹 events.jsonl).
- **단말(ptt-client)**: SOS 개시가 규격 시퀀스대로 **경보 MESSAGE 를 먼저** 보내고 긴급콜을
  개시한다(`McpttXml.alertInfo` + `PttController.sendAlert` — 호 성립과 무관하게 신원·그룹 전파).
  SOS 해제 시 경보 취소도 함께. 수신 측은 mcptt-info MESSAGE 를 파싱해 활성 경보
  상태(`alerts` StateFlow)로 들고, 전 탭 상단 배너(`AlertBanner`)+경고음으로 표시 —
  발신자 취소로 자동 해제, [닫기]는 로컬 표시만 제거. 이력 이벤트 `ALERT/ALERT_IN/ALERT_END`.
- 사용자 단위 개시 권한(`allow_alert_init`)은 미구현(§9).

### 4.4 상태/로깅

`CallDir::PttLogEvent` 신규 type: `emergency_activated|emergency_cancelled|imminent_activated|imminent_cancelled|alert_sent|alert_cancelled` (+ actor, reason). `BuildGroupDescriptor`(group.json)에 `inprogress_emergency` 등 반영.

---

## 5. CSC — 능력/프로파일 (TS 24.481/483/484)

- **XCAP group config**(`mcptt.py get_group_xml`): `allow-MCPTT-emergency-call`·
  `allow-imminent-peril-call`(=`emergency_call` 미러)·`allow-MCPTT-emergency-alert`(=`emergency_alert`)
  산출 — 규격 요소는 셋 다 내보내되 설정 축은 둘이다.
- **admin API**(`admin.py` group create/update): `emergency_call`·`emergency_alert` 수용 + INSERT/UPDATE.
- user profile XML 의 DB 연동(`default_emergency_group`·`allow-emergency-call/alert`)과
  user MCPTT 프로파일 엔드포인트는 미구현(§9) — 현재 정적/기본값.
- **notify_csp**: capability 변경은 `GROUP_CHANGED`로 전파(CSP lazy-reload). in-progress 상태는 CSP→CSC 역방향 보고가 필요할 수 있음(관측용, 선택).

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
- **권한**: 시스템 정책 `Setup.PttAdhocEnabled`(csp.json, 미지정 시 허용) — false 면 임시 그룹을
  합성하지 않는다(이후 비가입 착신 거절). 판정 시점에 그룹이 존재하지 않으므로 그룹 속성으로는
  성립 불가 — **사용자 단위 인가**는 MCPTT 프로파일 트랙(§9)에서.

---

## 7. 콘솔/관측

- **그룹 편집(PttGroupsWorkbenchPage)**: capability 체크박스 — 긴급콜(condition 공통 게이트),
  긴급경보. (임박위험·ad-hoc 은 별도 축을 두지 않는다 — §2.)
- **사용자(ProvisioningWorkbench)**: MCPTT 프로파일 섹션(기본 긴급그룹, 긴급/경보 개시 권한) — 미구현(§9).
- **PTT 세션 이력**: 진행 중/과거 emergency·imminent 에피소드, alert 발신 타임라인 표시(events.jsonl·floor.jsonl tier 활용).
- **OAM stats**: 긴급콜/경보 카운터, 진행 중 긴급 그룹 수.

---

## 8. 메시지 흐름 (긴급 개시 예시)

```
UE(개시자) ──INVITE(mcptt-info: session-type=prearranged, emergency-ind=true)──▶ CSP
  CSP: capability/권한 게이트 → in-progress emergency 설정
  CSP ──ADD/PTT_GROUP_MODIFY{emergency=1, initiator tier=emergency}──▶ CMP
  CSP ──fan-out INVITE(mcptt-info: emergency-ind=true)──▶ 멤버들
  CMP: 개시자 FLOOR_REQUEST → tier=emergency → 기존 발언자 REVOKE(reason=emergency_preempt) → GRANT
  ... 통화 ...
UE(권한자) ──re-INVITE(emergency-ind=false)──▶ CSP → PTT_FLOOR_TIER normal → 상태 해제
```

---

## 9. 미해결/결정 필요

1. **floor 패킷 priority 필드**: UE가 REQUEST에 priority/emergency를 실어보내는 규격 동작을 수용할지(상호운용), 아니면 CMP 중앙판정만 쓸지. → 중앙판정 + 패킷필드 옵션 파싱.
2. **in-progress 상태 DB 미러**: CSP→CSC 역보고 채널이 없으면 관측 정확도 한계. group.json/flow로 관측, DB 미러는 best-effort.
3. **권한자(authorized) 취소 판정**: 개시자 외 authorized_user/관리자 취소 허용 범위.
4. **ad hoc 콘솔(관제) 개시 입구**: 단말 resource-lists 입구는 구현됨 — 관제사가 콘솔에서
   인원을 골라 서버가 개시하는 dispatcher 입구는 미착수.
5. **사용자 단위 인가**: 긴급/경보 개시 권한·ad hoc 개시 권한(현재 무게이트)·기본 긴급그룹 —
   사용자 MCPTT 프로파일(TS 24.483) 트랙으로 일괄 설계.
6. **in-call 업그레이드 capability 게이트**: 개시(INVITE)만 게이트하고 re-INVITE 업그레이드는
   미적용 — `emergency_call=0` 그룹도 통화 중 격상 가능.

---

## 10. 관련 파일

- DB: `sql/cims_schema.sql` (`ptt_groups.emergency_call`/`emergency_alert`)
- CMP: `cmp/PMcpttGroup.{h,cpp}`(tier·선점·로깅), `cmp/PCmpServer.cpp`(명령 파싱)
- CSP: `csp/McpttInfo.{h,cpp}`(파서), `csp/ModuleDispatcher.cpp`(EventIncomingCall/EventReInvite/EventMessage), `csp/GroupCallService.cpp`(condition·fan-out·descriptor), `csp/CmpClient.cpp`(필드 전송), `csp/CspPttGroup.{h,cpp}`·`csp/DbManager.cpp`(컬럼), `csp/CallDir.h`(이벤트)
- CSC: `csc/src/services/mcptt.py`(XCAP DB연동), `csc/src/handlers/admin.py`(CRUD·user 프로파일)
- 콘솔: `ems/core/console/src/api/groups.ts`, `.../pages/PttGroupsWorkbenchPage.tsx`, 사용자 워크벤치
- 문서: 본 문서 + `docs/design/features/ptt_flows.md` 갱신
