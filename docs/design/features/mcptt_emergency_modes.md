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

마이그레이션 `sql/migrate_ptt_emergency.sql`:

```sql
-- 그룹 능력(capability) — TS 24.481 group config
ALTER TABLE ptt_groups
  ADD COLUMN imminent_peril_call   TINYINT(1) NOT NULL DEFAULT 1 COMMENT 'allow-imminent-peril-call',
  ADD COLUMN emergency_alert       TINYINT(1) NOT NULL DEFAULT 1 COMMENT 'allow-MCPTT-emergency-alert',
  ADD COLUMN adhoc_enabled         TINYINT(1) NOT NULL DEFAULT 0 COMMENT 'ad hoc 그룹콜 허용';
-- emergency_call 은 기존 컬럼 재사용(allow-MCPTT-emergency-call)

-- 진행 중 상태(in-progress) — 세션 런타임. CSP가 권위(authoritative), DB는 관측·복구용.
ALTER TABLE ptt_groups
  ADD COLUMN inprogress_emergency  TINYINT(1) NOT NULL DEFAULT 0 COMMENT 'in-progress emergency 상태',
  ADD COLUMN inprogress_imminent   TINYINT(1) NOT NULL DEFAULT 0 COMMENT 'in-progress imminent peril';

-- 사용자 MCPTT 프로파일(TS 24.483) — 기본 긴급/임박 그룹, 긴급 개시 권한
CREATE TABLE IF NOT EXISTS ptt_user_profile (
  user_id                     INT NOT NULL PRIMARY KEY,
  allow_emergency_init        TINYINT(1) NOT NULL DEFAULT 1,
  allow_alert_init            TINYINT(1) NOT NULL DEFAULT 1,
  default_emergency_group_id  BIGINT NULL,
  default_imminent_group_id   BIGINT NULL,
  CONSTRAINT fk_pup_user  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ad hoc 멤버는 ptt_group_members 재사용 + 그룹에 adhoc 표식
ALTER TABLE ptt_groups
  ADD COLUMN is_adhoc TINYINT(1) NOT NULL DEFAULT 0 COMMENT '동적 생성 임시 그룹',
  ADD COLUMN adhoc_expires_at DATETIME NULL COMMENT 'ad hoc TTL(미사용 시 sweep 삭제)';
```

`in-progress` 상태를 DB에도 두는 이유: CSP 재기동 시 진행 중 긴급 세션의 관측/콘솔 표시·OAM stat. CSP가 SoT, 비동기 미러.

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
- emergency/imminent 발언자는 **T2(최대 발언시간) 제한에서 제외**한다(긴급 중 장시간 발언 허용).
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
  1. 그룹 capability `emergency_call` + 개시자 `allow_emergency_init` 게이트(불허 시 SIP 4xx 또는 강등).
  2. in-progress emergency 설정(메모리 + DB 미러), 개시자에 MCPTT emergency state.
  3. CMP `ADD/PTT_GROUP_MODIFY`에 `emergency=1`(+개시자 tier=emergency) → floor 선점 보장.
  4. fan-out INVITE의 `mcptt-info`에 `<emergency-ind>true` 광고(`BuildGroupInfoXml` 확장).
  5. 자원 우선순위: 송출 INVITE에 `Resource-Priority` 헤더(RFC 4412, namespace `mcpttp.x`) 부가.
- **업그레이드**: 진행 중 그룹콜에 re-INVITE(`emergency-ind=true`) → `EventReInvite`의 **PTT 인지 분기** → `PTT_FLOOR_TIER`/`PTT_GROUP_MODIFY`로 floor만 격상, 멤버에 re-INVITE/UPDATE 전파.
- **취소**: 권한자(개시자 또는 authorized_user)만 `emergency-ind=false`로 해제 → tier normal 복귀, 상태 클리어. 비권한자 취소 무시(규격).
- **imminent peril**: 동일 경로의 `imminentperil-ind`, tier=IMMINENT.

### 4.3 emergency alert (SIP MESSAGE)

`EventMessage`에서 `mcptt-info`(`alert-ind=true`) 판별 → SMS 경로와 분기:
- alert capability(`emergency_alert`) + 사용자 `allow_alert_init` 게이트.
- 대상 그룹 멤버에게 alert fan-out(MESSAGE 또는 그룹 NOTIFY) + 위치/신원 포함.
- `CallDir`에 `alert_sent`/`alert_cancelled` 이벤트 기록(그룹 events.jsonl + 전용 보안/긴급 로그).
- 취소(`alert-ind=false`) 권한자만.

### 4.4 상태/로깅

`CallDir::PttLogEvent` 신규 type: `emergency_activated|emergency_cancelled|imminent_activated|imminent_cancelled|alert_sent|alert_cancelled` (+ actor, reason). `BuildGroupDescriptor`(group.json)에 `inprogress_emergency` 등 반영.

---

## 5. CSC — 능력/프로파일 (TS 24.481/483/484)

- **XCAP group config**(`mcptt.py get_group_xml`): `allow-imminent-peril-call`·`allow-MCPTT-emergency-alert`를 DB값(`imminent_peril_call`,`emergency_alert`)으로 산출. ad-hoc 정책 element 추가(선택).
- **user profile**(`get_user_profile_xml`): `ptt_user_profile`에서 `default_emergency_group`·`allow-emergency-call/alert`를 DB 연동.
- **service config**(`get_service_config_xml`): `allow-emergency-call/allow-alert`를 user 프로파일과 정합.
- **admin API**(`admin.py` group create/update): `imminent_peril_call`·`emergency_alert`·`adhoc_enabled` 수용 + INSERT/UPDATE. user MCPTT 프로파일 엔드포인트 `PUT /users/{id}/mcptt-profile` 신설.
- **notify_csp**: capability 변경은 `GROUP_CHANGED`로 전파(CSP lazy-reload). in-progress 상태는 CSP→CSC 역방향 보고가 필요할 수 있음(관측용, 선택).

---

## 6. Ad hoc 그룹콜 (TS 22.179 Rel-18)

사전 프로비저닝 없이 개시 시점에 멤버를 동적 구성:

- **개시 입력**: INVITE의 `resource-lists+xml`(멤버 URI 목록) + `mcptt-info`(adhoc 표식). 또는 콘솔/Admin이 ad-hoc 그룹을 즉석 생성.
- **CSP 처리**: 영속 그룹 없이 **임시 CspPttGroup** 구성(temp group-id 발급, `is_adhoc=1`로 DB에 단명 레코드 + `adhoc_expires_at`), 멤버 fan-out, CMP `PTT_GROUP_ADD`. broadcast/emergency 조건도 ad-hoc 위에 얹힘.
- **수명**: 마지막 멤버 이탈 시 즉시 teardown(on-demand와 동일) + DB 레코드 sweep(만료/빈 ad-hoc). chat형 ad-hoc은 비범위.
- **권한**: 개시자/그룹 정책 `adhoc_enabled`.

---

## 7. 콘솔/관측

- **그룹 편집(PttGroupsWorkbenchPage)**: capability 체크박스 추가 — 임박위험 허용, 긴급경보 허용, ad-hoc 허용. (emergency_call 기존)
- **사용자(ProvisioningWorkbench)**: MCPTT 프로파일 섹션(기본 긴급그룹, 긴급/경보 개시 권한).
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
4. **ad hoc 멤버 소스**: UE resource-lists vs 콘솔 즉석생성 우선순위.

---

## 10. 관련 파일

- DB: `sql/migrate_ptt_emergency.sql`
- CMP: `cmp/PMcpttGroup.{h,cpp}`(tier·선점·로깅), `cmp/PCmpServer.cpp`(명령 파싱)
- CSP: `csp/McpttInfo.{h,cpp}`(파서), `csp/ModuleDispatcher.cpp`(EventIncomingCall/EventReInvite/EventMessage), `csp/GroupCallService.cpp`(condition·fan-out·descriptor), `csp/CmpClient.cpp`(필드 전송), `csp/CspPttGroup.{h,cpp}`·`csp/DbManager.cpp`(컬럼), `csp/CallDir.h`(이벤트)
- CSC: `csc/src/services/mcptt.py`(XCAP DB연동), `csc/src/handlers/admin.py`(CRUD·user 프로파일)
- 콘솔: `ems/core/console/src/api/groups.ts`, `.../pages/PttGroupsWorkbenchPage.tsx`, 사용자 워크벤치
- 문서: 본 문서 + `docs/design/features/ptt_flows.md` 갱신
