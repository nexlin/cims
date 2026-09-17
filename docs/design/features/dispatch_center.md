# 관제 센터 — 전화 그룹(대표번호·당겨받기)·역할(감청·관리)·업무망 합법감청

> 관제 센터(소프트폰·관제용 앱) 요구 세 가지를 수용하는 CSP/CMP/CSC 설계와 설정 규약. 요구 = ① 업무망
> 통화를 선택해 합법감청(운영자 인가 기반 감독 청취), ② 관제센터로 걸려오는 전화를 N 명의 관제사가 선택적으로
> 수신(그룹핑), ③ 그룹을 N 개 생성.
>
> 모델은 **세 축**이다.
> - **접속환경** — 관제석·데스크폰·소프트폰은 이동 VoLTE 와 다른 **유선 VoIP 접속서비스**(`kind=voip`,
>   [sip_service_model.md §2-9](sip_service_model.md))에 붙는다. TLS·SRTP·NAT·피처코드·호 전달 기본값은 이 서비스의 필드다.
> - **전화 그룹**(§3.1) — 대표번호(TS 24.239 Flexible Alerting)·당겨받기 축·그룹원 BLF. **유선 전화의 일반 기능**이며
>   관제와 무관하게 어느 조직이든 쓴다. `pickup_group` 값 = 전화 그룹 id.
> - **역할**(§3.3, 정본 [mcptt_authorization.md](mcptt_authorization.md)) — 감청·PTT 청취·이력/녹취 열람·조직/구성원/
>   번호/PTT 그룹 관리 **권한**. 콘솔 관리 권한과 같은 역할 모델이고, **관제사 = 역할을 배정받은 가입자**다.
>
> **구현 상태**: 대표번호 호출(§4)·업무망 합법감청(§5 — Join → CMP tap, 분리 인도·은닉)·PTT 그룹콜 청취(§5.6)·
> 감사 E-AUD-016(§5.7)·CMP tap(§6)·검증(`S3-SCN-FA`/`S3-SCN-MONITOR`/`S3-SCN-PTT-LISTEN`)은 구현·실측 완료.
> **전화 그룹/역할 분해(§3·§8.1)는 구현 반영** — CSP `CCspPhoneGroupMap`+`CCspRoleMap`, CSC `/api/v1/phone-groups`·`/api/v1/roles`·
> `authz.can()`·두 블록 discovery, 콘솔 `구성 > 전화 그룹`·`시스템 > 역할`, 검증 시드, 전환 마이그레이션. **유선 VoIP 접속환경
> `kind=voip` 도 구현 반영**(§8.3 — CSP kind 검증·같은 전화 경로·로그/통계 축 합산, CSC `service_ref`↔서비스 name 매칭 프로비저닝,
> 관제 앱 전화 회선 voip 우선). 라이브 반영은 정지창(마이그레이션 + csp·csc·oam-svc 동시 + 기존 관제 회선의 `voip` 이관 —
> [volte_supplementary_services.md §10.3a](volte_supplementary_services.md)). 남은 것:
> 단말(관제용 앱)의 SSRC 디먹스 UI(U10 공용)·PTT 청취 채널 UI(U6), §10 향후 과제.
>
> 관련: [volte_supplementary_services.md](volte_supplementary_services.md)(유선 VoIP 규약·내선·당겨받기·호 전달 —
> 본 설계가 그 위에 얹힌다), [mcptt_authorization.md](mcptt_authorization.md)(역할·능력·범위 — 권한 모델 정본),
> [sip_service_model.md](sip_service_model.md)(접속서비스 `kind`), [registration_binding_set.md](registration_binding_set.md)
> (도달 경로 선택), [media_security.md](media_security.md)(SRTP), [recording.md](recording.md)(녹취 탭),
> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §R1(ambient listening),
> [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md)(단말 SSRC 디먹스),
> [../identifier_model.md](../identifier_model.md), [../../api/cmp_media_api.md](../../api/cmp_media_api.md).
>
> 합법감청 규격 근거: 3GPP TS 33.107(LI 아키텍처)·TS 33.108(인도 인터페이스), ETSI TS 101 671 —
> **통신 내용(CC)은 방향·당사자를 분리 가능한 형태로 인도**하며 서버 믹싱을 규정하지 않는다(§5.1).

---

## 1. 범위와 결론

세 요구는 **전화 그룹**(유선 전화 기능)과 **역할**(권한) 두 엔티티 위에 규격 기반 서비스를 얹으면 수용된다.
관제는 접속환경도 그룹도 아닌 **권한**이다 — 대표번호를 받고 당겨받는 것은 어느 유선 사용자든 하는 일이고,
감청·청취·관리는 역할이 있는 사람만 한다.

| 요구 | 결론 | 표준 근거 |
|---|---|---|
| ③ 그룹 N 개 | **전화 그룹 엔티티**(§3.1). 기존 `pickup_group` 축을 이 엔티티의 id 로 채워 당겨받기·그룹원 BLF·병렬 호출이 **한 그룹 축**을 공유한다. 감청·관리 범위는 여기 두지 않는다 | [identifier_model.md](../identifier_model.md) — 동작은 불변 id, 표시는 name |
| ② 대표번호 착신을 그룹원 전원이 선택 수신 | 전화 그룹의 **대표번호(pilot) 병렬 호출**(§4) — 대표번호 INVITE 를 등록 그룹원 전원에게 포크, 최초 200 OK 가 이기고 나머지는 CANCEL | 3GPP TS 24.239 Flexible Alerting(parallel alerting), RFC 3261 §16.7(포크 응답 처리), RFC 3455 `P-Called-Party-ID` |
| ① 업무망 통화 선택 합법감청 | **선택** = RFC 4235 dialog 이벤트(기존 구현)의 인가 범위를 **역할 `monitor_call`** 로 확장, **합류** = RFC 3911 `Join` INVITE(`a=recvonly`) → CMP **청취 leg(tap)** — 양 화자를 **분리 스트림(SSRC 2개)** 으로 인도(귀속 보존), **믹싱은 단말**(§5·§6). PTT 그룹콜은 `recv_only` 멤버로 JOIN(역할 `ptt_listen`) | 3GPP TS 33.107/33.108·ETSI TS 101 671(LI — 분리 인도), RFC 4235, RFC 3911, RFC 3264, RFC 5576(소스 라벨링), TS 24.379(§5.6·§10) |

설계 원칙(CLAUDE.md 우선순위)대로 규격형을 채택하고, 기존 구현(픽업·전달·dialog 이벤트·CMP
ambient 플래그·녹취 탭)의 연장으로 구성한다. **INVITE 경로에 DB 질의를 넣지 않는다** — 전화 그룹·역할
판정은 전부 인메모리 맵에서 답한다.

---

## 2. 현재 구현과의 갭 (구현 단계의 입력)

대표번호·감청·청취·감사의 절차는 구현돼 있다. 남은 갭은 **엔티티와 인가의 자리**다.

| 갭 | 현재 | 필요 |
|---|---|---|
| **G1 전화 기능과 권한이 한 엔티티** | `dispatch_groups` 가 픽업 그룹·대표번호와 함께 `monitor_scope`·`ptt_listen`·`listen_visibility`·`directory_admin` 을 든다 — 편입이 곧 범위 취득이고, 대표번호만 필요한 조직도 "관제 그룹" 을 만들어야 한다 | `phone_groups`(전화) + `roles`/`role_assignments`(권한)로 분해(§3, §8.1). 절차(포크·tap·감사)는 그대로, **인가의 질문만 둘**로 — "같은 전화 그룹인가"(BLF·픽업) / "역할 범위 안인가"(감청·청취·관리) |
| **G2 자격 부여 경로** | 관제 앱 관리 API(`PUT …/ptt/profile`)는 `allow_ambient_listening` 변경을 400 `not_editable` 로 거절한다(현재값과 같은 값은 무시 — 구 앱 호환, 표시는 유지). 부여는 콘솔 프로파일 편집(manager) | 청취 자격은 **역할 배정의 결과**로 CSC 가 동기하고, 배정은 `authz.manage`(콘솔 manager)에만 있다([mcptt_authorization.md §2.4](mcptt_authorization.md)) — 그때 콘솔 직접 편집도 잠근다 |
| **G3 org 폴백·전역 피처코드** | 해소 — 픽업 축 = `pickup_group` 만(없으면 픽업·BLF 불가, `PickUp` 404), 피처코드 = 서비스 필드만(전역 `CallPickupId` 제거) ([volte_supplementary_services.md §5](volte_supplementary_services.md)) | — |
| **G4 프로비저닝 블록** | `/provisioning/me` `dispatch` 블록이 전화 그룹 정보(대표번호·그룹원)와 권한(범위·관리)을 한 블록에 싣는다 | `phoneGroup` + `dispatch` 두 블록(§8.4). 전환기에는 현 블록 모양을 합성해 유지 |
| **G5 접속환경** | 관제 회선·전화 그룹이 이동 VoLTE 서비스(`volte`)에 있다 | 유선 VoIP 서비스 `kind=voip`(§8.3, [sip_service_model.md](sip_service_model.md)) — 관제 회선 `service_ref` 와 `phone_groups.service_ref` 를 `voip` 로 |

---

## 3. 전화 그룹과 역할

### 3.1 전화 그룹 (phone group)

전화 그룹 = **픽업 그룹 + (선택) 대표번호**. 유선 VoIP 서비스의 그룹 기능이며 관제 권한과 무관하다 — 관제석
전화도, 총무팀 대표번호도 같은 엔티티다. 대표번호가 없는 그룹은 순수 당겨받기 그룹이다.

| 필드 | 의미 |
|---|---|
| `id` | **불변 키**(CSC 발급, 예 `pg-7f3a91c2`. 전환 전 발급된 `dg-…` 값도 그대로 유효 — 재키잉하지 않는다). `*_subscriptions.pickup_group` 에 그대로 들어가는 값이자 알람·이력의 상관 키 |
| `name` | 표시 이름(운영자 변경 가능, 어떤 키에도 쓰지 않는다) |
| `pilot_id` | 대표번호(다이얼 가능한 주소). 가입 id 주소 공간과 **겹치지 않아야** 한다(CSC 검증). NULL = 대표번호 없음 |
| `service_ref` | 대표번호가 속한 접속서비스 — 유선 VoIP 서비스(`voip`). 도메인·SRTP 정책·피처코드를 이 서비스에서 읽는다 |
| `alert_mode` | `parallel`(기본) / `sequential`(§4.4a) — TS 24.239 의 두 모드 |
| `no_answer_sec` | 전원 무응답 판정 시간(기본 30) |
| `busy_members` | `skip`(기본 — 통화 중 그룹원은 호출 안 함) / `alert`(호출 — 단말 통화대기) |
| `overflow_target` | 무응답·전원 부재 시 넘김 대상(다른 대표번호 또는 가입 번호). NULL = 480 |
| `org_id` | 소속 조직(콘솔 필터) |

### 3.2 멤버십과 파생

- `phone_group_members(user_id PK, group_id, alert_order)` — **가입자당 그룹 하나**. `pickup_group`
  이 단일 값이므로 이 제약이 축 통합의 전제다(겸임은 §10). 멤버 행의 `user_id` 는 **대표번호 포크·dialog
  감시 대상인 회선**(유선 회선 — `voip_subscriptions`, [sip_service_model.md §2-9](sip_service_model.md))이다 — CSP
  `ResolveForkTargets` 는 서비스 구분 없이 등록된 멤버 전원에게 포크하므로 PTT 회선을 멤버로 넣지 않는다(PTT 앱까지 울린다).
- **전화 그룹은 person 귀속이다.** CSC 가 멤버 추가/제거/그룹 삭제 시 그 회선이 속한 person 의 **volte·voip·ptt 전
  회선** `pickup_group` 을 유효 그룹(`effective_phone_group` = 자기 멤버십 → 없으면 같은 person 의 멤버십, 여럿이면
  `alert_order`·회선 id 순 첫째)으로 **재계산**하고, 값이 바뀐 회선마다 `USER_CHANGED` 를 보낸다(CSP 는 회선별
  사용자 캐시로 `pickup_group` 을 든다). 관제사의 PTT 회선이 그룹을 물려받아야 PTT 세션 가시성(§5.6a)의 "자기 그룹원"
  판정과 PTT 회선 dialog 인가 규칙 1 이 성립한다. 기존 데이터는 마이그레이션 끝의 백필(같은 규칙, 재실행 안전)로 맞춘다.
- 파생 회선(자기 멤버십 여부와 무관)의 `pickup_group` 직접 편집은 409(`derived_from_phone_group`), 전화 그룹
  귀속 person 에 새 회선을 개설하면 파생값을 물려받는다(지정값이 다르면 409) — SoT 는 멤버십이다.
- **org 폴백은 없다.** `pickup_group` 이 비어 있으면 그 회선은 어떤 픽업·BLF 축에도 속하지 않는다
  ([volte_supplementary_services.md §5.1](volte_supplementary_services.md)).

### 3.3 역할 — 관제 권한

권한 모델의 정본은 [mcptt_authorization.md](mcptt_authorization.md) 다. 여기서는 본 문서의 절차가 읽는 필드만 적는다.

- **역할 = 능력 + 범위** 하나의 엔티티(`roles`). 콘솔 프리셋(`admin/manager/operator/monitor`, 전역 범위)과 관제
  프리셋(`감독 / 관리 / 전체`, 한정 범위)이 같은 엔티티다. 사람은 역할 **하나**에 배정된다(`role_assignments`).
- **CSP 가 읽는 것**(SIP 경로 인가): `monitor_call`(none/own/listed/all) + `role_monitor_targets`(전화 그룹 id) —
  통화 감청·타인 세션 관측(§5.2·§5.3·§5.6a) / `ptt_listen`(none/listed/all) + `role_ptt_targets` — PTT 청취·conference
  구독(§5.6) / `listen_visibility`(hidden/visible) — 청취 로스터 노출(§5.6).
- **CSC 가 읽는 것**: `directory_write`(none/own/all) + `org_id` — 관제 앱 관리 평면(§3.4) / `monitor_call`·`ptt_listen` —
  이력·녹취 열람 범위(§5.7a·§5.7b)·`/provisioning/me` 감시 대상 목록(§8.4).
- **자격 동기**: `ptt_listen≠none` 인 역할에 배정되면 CSC 가 그 person 의 PTT 프로파일 `allow_ambient_listening`
  (TS 24.484)을 켜고, 해제·역할 변경으로 범위가 없어지면 끈다. CSP 는 규격 자리(프로파일)에서 자격을 읽는다(§5.6).
- **부여**: 역할 생성·범위 변경·배정은 `authz.manage` 능력(콘솔 `manager` 이상)에만 있고 한정 범위 역할에는
  부여할 수 없다 — 관제 앱에서는 어떤 경로로도 감청·청취 권한을 만들 수 없다. 배정·해제는 감사된다(§5.7).

### 3.4 관리 범위 — 조직/구성원/번호·PTT 그룹 관리 (관제 앱)

관제사(가입자, PKCE 토큰)가 관제 앱에서 조직 트리·구성원(person)·VoLTE/VoIP/PTT 번호(가입)·전화 그룹·PTT 그룹을 관리하는 권한은
역할의 **`directory_write`** 하나로 정한다 — `monitor_call`·`ptt_listen` 과 같은 결의 범위 enum 이며 서버가 해석하고 앱은
결과만 받는다. 가입자 프로비저닝은 3GPP 규격 밖(MC 서비스 제공자 정책)이라 CIMS 확장이다.

- **범위 해석**: `own` = 역할 `org_id` 조직과 그 하위(코드 집합), `all` = 전 조직. `own` 인데 `org_id` 가 없으면 범위가
  비어 관리 불가.
- **쓰기에만 걸리는 범위다.** 읽기(`GET /provisioning/directory` — 조직 트리 + 가입자 목록)는 provisioning scope 토큰만
  요구하고 호출자 범위를 보지 않는다(`services/mcptt.py` `handle_provisioning_directory`). 관제사는 발신 대상을 고르려면
  전 주소록이 보여야 하므로 의도된 동작이다 — 범위는 **누구를 고칠 수 있나**에만 적용한다.
- **부여** = 콘솔 `관리 > 역할`(manager 이상 — 가입자에게 조직·번호 쓰기 권한을 여는 승인 사항). 앱은 `/provisioning/me`
  `dispatch.directoryWrite`/`orgCode` 로 안다.
- **API** = `/provisioning/directory/{admin,orgs,members,groups}`(CSC 4430, 계약
  [android_ue_provisioning.md §3-3](android_ue_provisioning.md)) — 콘솔 관리 API(`/api/v1/organizations`·`/api/v1/users`,
  콘솔 토큰)와 **같은 쓰기 코드**(`handlers/admin.py`·`org.py`)와 **같은 판정**(`can(principal, directory.write, org)`)을
  지난다. 토큰 realm 은 섞지 않는다 — 섞이는 것은 정책이다.
- **범위 게이트**: 대상 조직(생성 부모·이동 부모·구성원 소속)이 범위 안이어야 한다(`403 out_of_scope`). `own` 은
  범위 루트를 옮기거나 지우거나 루트를 새로 만들 수 없다. 조직 삭제는 하위 조직·구성원이 없을 때만(`409 not_empty`).
  자기 자신 삭제 불가(`409 self_delete`). 번호 변경(다른 msisdn) = 종전 회선 삭제 + 신규 개설이라 SIP 비밀번호 필수.
- **PTT 프로파일**: 관리 범위 안 구성원의 `allow_create_group`·긴급 계열 자격은 편집할 수 있다. **`allow_ambient_listening`
  은 편집 대상이 아니다**(역할 배정의 결과 — `authz.manage`). 현재값과 다른 값이 실려 오면 400 `not_editable`, 같은 값은 무시(구 앱은
  두 플래그를 함께 보낸다). 구현 = `dispatch_directory.py` `_LOCKED_PROFILE_KEYS`.
- **PTT 그룹**: 관리 범위 안 그룹(`org_code` 가 범위 안 또는 내 소유)은 소유자가 아니어도 GMS XCAP GET/PUT/DELETE 를
  허용하고(소유권은 바뀌지 않는다 — `authorized_user_id` 유지), 신규 생성은 `allow_create_group` 없이도 된다
  (`ptt_group.manage` 능력 — [mcptt_authorization.md §3](mcptt_authorization.md)). 관리용 열거는
  `GET /provisioning/directory/groups`(멤버 그룹 목록과 별개) — 관리 범위 안 ∪ 내 소유 ∪ 청취 범위(`ptt_listen`
  all|listed) ∪ 내 멤버 그룹을 주고, 행마다 `canManage`·`inListenScope`·`isMember` 를 싣는다.
- **전화 그룹**: 관리 범위 안 조직의 전화 그룹(대표번호·호출 방식·멤버)도 같은 능력으로 관리한다. 역할·배정은 관리 대상이 아니다.
- **감사**: 모든 쓰기는 `E-AUD-006 config_change`(actor = `user:<users.id>`, entity = organization|user|subscription|
  ptt_profile|phone_group, reason = `dispatch_directory`).

### 3.5 CSP 인메모리 맵

두 맵이다(현 코드 `CCspDispatchGroupMap` 을 분리한다).

- **`CCspPhoneGroupMap`** — 그룹 id 인덱스 + pilot 인덱스 + 멤버 인덱스(가입자 → 그룹). 부팅 시 `DbManager` 가
  적재하고(`phone_groups` 테이블 부재는 프로브로 감지 — INFO 로그 후 전화 그룹 기능 비활성) `PHONE_GROUP_CHANGED` UDP
  통지(uri=그룹 id, DELETE=제거·그 외 단건 재적재, 빈 uri=전량)와 `CSC_RESTART` 로 재적재한다. JSON fallback 은
  `DataFolder.PhoneGroup`(기본 `phone_group/`)의 `<id>.json`. 포크 대상은 **멤버 테이블(`alert_order` 순)** 이 SoT 이고
  등록·생존 여부만 `UserMap` 으로 판정한다. 당겨받기·BLF 규칙 1 의 그룹 축 값은 `EffectiveGroupOf`(멤버 인덱스 →
  `CspUser.m_strPickupGroup`) 하나로 답한다 — org 폴백은 없다.
- **`CCspRoleMap`** — 회선 id → 역할(`monitor_call`·감시 대상 집합·`ptt_listen`·청취 대상 집합·`listen_visibility`).
  적재 시 `role_assignments(principal_type='user')` 를 그 person 의 **전 회선**(volte·voip·ptt) id 로 펼친다 — PTT 회선의
  청취 인가와 유선 회선의 감청 인가가 같은 사람의 역할을 본다. `ROLE_CHANGED`(uri=역할 id 또는 배정 person id — 전량)·
  `USER_CHANGED`(POST/DELETE = 회선 개설/삭제 — 전량)·`CSC_RESTART` 로 재적재. JSON fallback `DataFolder.Role`. 콘솔 principal 의 배정은 SIP
  신원이 없으므로 적재하지 않는다. 판정 = `CanWatch`(§5.2)·`CanListenPtt`(§5.6)·`ListenVisibility`.

---

## 4. 대표번호 호출 (TS 24.239 Flexible Alerting — parallel / sequential)

### 4.1 flow (parallel)

```
UE-A ──INVITE sip:7000@dispatch.cims──► CSP(TAS)
                                          │ [pilot 해석: 7000 → pg-7f3a91c2, alert_mode=parallel]
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

그룹원은 대표번호 AoR 에 `Event: dialog` 를 구독할 수 있다(인가 = 그 전화 그룹의 멤버 — 역할 불요). 대표번호에
걸려온 호의 early/confirmed/terminated 와 응답자(`remote` 신원)가 NOTIFY 된다 — 데스크 큐 표시·
"누가 받았나" 표시의 표준 경로다.

**포크 집합당 dialog 하나(RFC 4235 정합).** 착신 한 건은 그룹원 N명에게 포크되지만 감시자에게는
**dialog 하나**로 보여야 한다(데스크 큐에 한 행). `CTasModule::NotifyPilotDialog` 이 dialog `id` 를
**A-leg(발신자→대표번호 INVITE) Call-ID 로 고정**해 early(첫 180 에서 1회)→confirmed(승자 응답)→
terminated(집합 완전 실패 또는 확립 호 종료 시 1회)가 같은 dialog 를 갱신한다. 포크 leg(그룹원)별
Call-ID 를 id 로 쓰지 않는다(그러면 착신 한 건이 N행으로 뜬다). 패자 CANCEL·sequential 단계 전환은
terminated 를 내지 않는다(dialog 는 early 유지). 초기 full 스냅샷(§5.2)에도 진행 중 대표번호 호가 실린다.

**세 entity 의 본문은 당사자 기준으로 고정(RFC 4235 §4.1).** 확립된 대표번호 호는 `CTasModule::m_mapPilotOfCall`
항목(`PilotCall` = 대표번호·A-leg Call-ID·발신자)에 고정돼, **어느 leg 의 BYE 든**(발신자 A-leg 든 승자 leg 든) 대표번호
entity 에는 같은 id·`direction=recipient`·`remote=발신자` 로 terminated 가 **1회** 나간다. 발신자·승자 entity 는 일반
호와 같은 규칙(`CTasModule::NotifyDialogState`)으로 각자 자기 dialog(발신자 = A-leg Call-ID·`initiator`·remote 승자,
승자 = 자기 leg Call-ID·`recipient`·remote 발신자)를 confirmed→terminated 로 받는다. 당사자·개시 방향은
`CallLegParty`(`CCallMap::ResolveLegParties` — 당사자 = leg 의 원단 사용자, 개시자 = CSP 수신 leg)로만 해석한다.
psip dialog 의 From/To 는 "CSP 가 요청을 보내는 입장" 이라 수신 leg 에서 뒤집혀 있어 이를 caller/callee 로 읽으면
A-leg BYE 때 두 당사자가 바뀐다. 검증 F7. `version` 은 구독별 NOTIFY CSeq 파생으로 단조 증가 — 단말 재기동으로
같은 entity 구독이 하나 더 열리면 이전 구독은 첫 NOTIFY 실패(481) 때 회수되며 그 NOTIFY 의 version 은 별개 열이다
(감시 앱은 dialog 구독(Call-ID) 단위로 version 을 비교해야 한다).

### 4.6 녹취·이력

relay 는 대표번호 호 1건이다. `RELAY_ADD` 의 `callee` 는 대표번호, 승자 확정 시 `RELAY_MODIFY` 의
`callee` 로 응답자를 갱신한다. `call.json` 에 `phone_group`, `pilot`, `alerted[]`, `answered_by`
를 기록한다(CSP 작성 메타 — [recording.md](recording.md) §3.6).

---

### 4.7 대표번호 발신 표시 (P-Preferred-Identity = pilot)

관제사가 **대표번호로 걸 때**(민원인이 관제석 개인 번호가 아닌 대표번호를 보고, 회신이 대표번호 포크로 돌아오게)
는 TS 24.239 FA 그룹원의 pilot 신원 발신이다. 단말은 발신 INVITE 에 `P-Preferred-Identity: <sip:+821310001000@volte…>`
(RFC 3325, `tel:` 도 수락)를 싣는다. CSP(TAS `ResolveOriginatingIdentity`, B2BUA `CreateCall` 직전)는:
- PPI 의 신원이 **발신자가 속한 전화 그룹의 `pilot_id`** 이면 B-leg 의 `From` 을 대표번호로 낸다 — psip 는 B-leg
  `P-Asserted-Identity` 를 From 과 같은 값으로 넣으므로 착신자(내부 단말·트렁크)는 대표번호를 본다.
- 그 외의 PPI(다른 그룹의 대표번호·타인 번호)는 TS 24.229 §5.4.3.2 대로 **무시**하고 기본 신원(발신자 자신)으로 낸다
  (403 이 아니다 — 등록·인가된 신원이 아닌 PPI 는 기본 신원으로 대체). 인가 = 그룹 멤버십 하나(대표번호 착신을 받는
  사람이 대표번호로 걸 수 있다). 별도 플래그는 두지 않는다.
- **표시 신원만 바뀐다**: CallMap 당사자·CDR(`call_log`)·세션 이력·dialog 이벤트(§4.5 `NotifyDialogState` 의
  `CallLegParty`)는 실제 발신자다 — 그룹원 띠에는 "관제1석 통화 중" 으로, 착신자에게는 대표번호로 보인다.
- 대표번호로 나간 호의 회신은 §4.1 의 일반 대표번호 착신(포크)이다.

앱: 빠른 발신 줄의 "대표번호로 발신" 토글([dispatch_desktop_ui.md §4.3](dispatch_desktop_ui.md)) — `dispatch.pilotId` 가
있을 때만 노출. SDK 는 INVITE 헤더 추가(`makeCall` 옵션)로 싣는다.

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

### 5.2 선택 — dialog 이벤트 인가 범위

`CanWatch(watcher, target)` — 질문이 둘이다.
1. **전화 그룹**: 같은 `pickup_group`(= 같은 전화 그룹) → 허용. 그룹원 BLF·지정 픽업의 근거이며 역할이 필요 없다.
2. **역할**: watcher 의 역할 `monitor_call` 이 `all`, 또는 `own` 이고 target 이 watcher 의 전화 그룹, 또는 `listed`
   이고 target 의 전화 그룹이 `role_monitor_targets` 에 있음 → 허용. `own` 은 규칙 1 과 대상이 같지만 **이력·녹취·
   Join 청취**(§5.3·§5.7a)까지 여는 값이다 — 규칙 1 은 BLF 만 연다.
3. 그 외 403.

"모든 통화" 목록의 구독 형태:
- **초기형**: 대상 회선별 dialog 구독. 대상 목록은 `/provisioning/me` `dispatch.members[]`(§8.4) — CSC 가
  역할 `monitor_call` 을 위 `CanWatch` 와 **같은 규칙**으로 해석해 내려준 가입자 집합이라 앱은 enum 을
  해석하지 않고 그대로 구독한다. 자기 전화 그룹원은 `phoneGroup.members[]` 로 따로 온다(규칙 1). 클릭 시 소프트폰이
  §5.3 의 Join INVITE 를 낸다.
  - **구독 수락 직후 full 스냅샷**(RFC 4235 §3.2): CSP 가 감시 대상이 당사자인 **진행 중 호**(멤버 BLF = CallMap
    caller-facing leg)와 **대표번호 착신**(TAS 포크 집합=울림 / 확립 집합)을 모아 `state=full` NOTIFY 로 준다
    (`CspServer.cpp CollectInitialDialogs`·`BuildDialogInfoBodyMulti`, `CallMap::Iterate`·`CTasModule::CollectPilotDialogs`).
    재로그인·재구독 즉시 이미 울리는 대표번호 호·통화 중 그룹원이 보인다(활성 호 없으면 빈 full, 이후 partial 갱신).
- **표준형(후속)**: RFC 4662 RLS — `Supported: eventlist` 로 감시 목록 URI 하나를 구독하고
  RLMI+multipart NOTIFY 로 전 대상의 dialog-info 를 받는다. 구독 N 개를 1개로 줄인다.

### 5.3 합류 — INVITE-with-Join (G4)

```
UE-A ◄──통화중──► CSP(B2BUA)+CMP(relay S) ◄──통화중──► UE-B
UE-M ──INVITE sip:A@dispatch.cims ──► CSP
      Join: <A-leg Call-ID>;to-tag=…;from-tag=…        (dialog NOTIFY 에서 얻은 식별자)
      SDP: m=audio … a=recvonly, crypto(M 수신 키)
                                      │ [CallMap 에서 Call-ID 조회 → 세션 S 확정(MatchDialog: Call-ID+태그)]
                                      │ [인가: M 의 역할 monitor_call ∋ A 또는 B 의 전화 그룹]
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
- **인가 두 겹**: SIP 경로는 인메모리 판정(역할 `monitor_call` + 대상의 전화 그룹, §5.2 규칙 2)만 한다. "누가
  감청 권한을 갖는가" 는 역할 배정이 정하고, 배정은 `authz.manage`(콘솔 계정 `manager` 이상)에만 있다
  ([mcptt_authorization.md §2.4](mcptt_authorization.md)). 관제 앱의 관리 범위(`directory_write`)로는 배정할 수 없다.
  콘솔 계정과 가입자는 다른 저장소·다른 principal 이지만 **같은 역할 모델**을 본다.
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

관제사가 역할 `ptt_listen` 범위 안의 그룹 AoR 로 **SDP `a=recvonly` 초기 INVITE**(RFC 3264 — 수신 전용 offer 가
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

**인가 — TS 24.484 프로파일 자격 + 역할 범위(규격형, 2단)**:
- **자격 = `ptt_user_profile.allow_ambient_listening`**(TS 24.484 ruleset·TS 24.379 ambient listening 인가):
  이 사용자가 원격 청취를 수행할 자격. CSP 가 청취 개시 INVITE 에서 프로파일 행 하나를 읽어 판정한다
  (`SelectUserProfile` — 인덱스 단건, 다른 프로파일 게이트와 같은 경로. 값 0·행 부재·DB 불가는 모두 403 — 당사자
  모르게 미디어를 인도하는 동작이라 fail-closed). 값은 사람이 직접 켜지 않는다 — **`ptt_listen≠none` 역할에 배정되면
  CSC 가 켜고, 해제되면 끈다**(§3.3). 규격이 정한 인가 자리를 그대로 쓴다.
- **범위 = 역할 `ptt_listen`**(`none`/`listed`/`all`) + `role_ptt_targets`: 자격자가 어느 PTT 그룹을 들을 수 있는가.
  청취 INVITE·conference SUBSCRIBE 의 **SIP 신원 = PTT 회선 id** 로 `CCspRoleMap` 에 묻는다(회선 → person → 역할, §3.5).
  자격과 범위는 같은 배정에서 나오지만 판정은 둘 다 한다: 프로파일이 0 이면 `allow_ambient_listening=0` 403, 역할이 없거나
  범위 밖이면 `ptt_listen scope` 403(CSP 로그 `ProcessGroupCall … denied (사유)`).
- **부여 게이트 = `authz.manage`(콘솔 `manager`)**: 청취 범위가 있는 역할의 생성·범위 변경·배정을 콘솔에서 승인·감사한다
  (§5.7). 편입되는 가입자 쪽에 별도 역할 게이트는 없다.

**로스터 노출 — `listen_visibility`(은닉·투명 둘 다 정식 지원)**: 규격이 청취 멤버 표시를 정의하지
않으므로 CIMS 정책축이며, **관제사 역할의 속성**으로 두 모드를 모두 지원한다.
- `hidden`(기본): 청취 멤버를 로스터(RFC 4575 conference-info)에서 제외하고 합류/이탈 시 참가자 통지도 내지
  않는다(청취 leg 자신은 NOTIFY 를 받는다). `FLOOR_TALKERS`·녹취 화자 트랙에는 `recv_only` 라 원래 오르지 않는다 —
  합법감청 은닉(§5.1). 사용은 §5.8 의 고지·동의 운영 규약을 전제한다.
- `visible`: 청취 멤버를 로스터에 `<roles><entry>listener</entry></roles>`(RFC 4575 §5.6.3)로 싣고 합류/이탈을
  통지한다 — 협업 무전 그룹처럼 청취 공개가 정상인 운용용. 이때도 발언 자격은 없다(`recv_only`).
- CMP 멤버 수에는 포함되므로 "참가자 1명" floor 거절(only-one) 판정이 청취자 합류로 풀릴 수 있다 — 청취가
  성립하려면 불가피한 관측 가능 변화다.

**conference 이벤트 구독 인가 — TS 24.379 §10.1.3.4.1(규격형)**: 관제 앱의 PTT 세션 목록("진행 중·참가자 수",
[dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.2 ② 범위 채널)은 그룹 AoR 의 RFC 4575 conference 구독으로 안다. CSP(controlling
function, `CscfModule` SUBSCRIBE 초기 구독)는 구독자를 그룹 문서(TS 24.481)의 **`<on-network-allow-conference-state>`**
로 판정하고, 불허 시 **403 + `Warning: 138 CIMS "subscription of conference events not allowed"`**, 브로드캐스트 그룹은
**480 + Warning 105** 로 거절한다(`CGroupCallService::CheckConferenceSubscribe`). CIMS 해석:
- **멤버** = 그룹 속성 `ptt_groups.allow_conference_state`(기본 1 — GMS 문서 `<cp:actions>` 요소로 노출, 관리 API·GMS PUT·콘솔
  편집). 0 이면 멤버도 403.
- **비멤버 관제사** = 청취 leg 와 같은 2단 인가(자격 `allow_ambient_listening` + 역할 범위 `ptt_listen`)를 같은 요소의 해석으로
  두어 **합류 전 사전 모니터링 구독**을 허용한다(규격 흐름은 "세션 참가자"의 구독이고 청취 leg 로 합류한 관제사는
  참가자이므로 규격 그대로 — 합류 전 구독만 CIMS 확장). 프로파일 부재·DB 불가는 불허(fail-closed).
- 즉석 세션(`adhoc-`/`priv-`)은 그룹 문서가 없다 — 참가자(fan-out 대상)는 허용, 그 외는 §5.6a 의 즉석 세션 관측
  인가(자격 + 참가자 전화 그룹이 관측자 역할 `monitor_call` 안). in-dialog refresh 는 재검사하지 않는다(RFC 6665 — 자원·이벤트 불변). 구독자에게
  가는 로스터는 `listen_visibility` 규칙 그대로(청취 leg 은닉/공개).
- 검증 = `S3-SCN-PTT-LISTEN` L1b(범위 안 200)·L2b/L3b(403 + Warning 138) — cspsim `ptt_listen` 이 합류 전 M 의 conference
  SUBSCRIBE 결과를 `M_conf_sub`/`M_conf_warn` 마커로 낸다.

TS 24.379 **ambient listening**(`session-type=ambient-listening`, remote-init — 특정 단말 주변음을
원격 개시로 듣는 1:1 호)은 같은 `allow_ambient_listening` 자격을 재사용하되 단말의 무표시 자동응답이
필요해 시그널링은 별도 과제다(§10).

### 5.6a PTT 세션 가시성 — 타인 간 사설콜·애드혹 (dialog 이벤트, RFC 4235)

관제 앱 ② 범위 채널의 "타인 세션"([dispatch_desktop_ui.md §4.2](dispatch_desktop_ui.md))은 **관제 범위 안 사람들이
지금 어떤 PTT 세션에 참가 중인가**다. 사설콜(`priv-<발신>-<착신>`)·애드혹(`adhoc-…`)은 PTT 그룹이 아니라 **사람 사이의
세션**이라 그룹 AoR 구독(§5.6)으로는 알 수 없다. 규격에 제3자 관측 절차가 없으므로 VoLTE 통화 감시와 **같은 패키지·
같은 인가**로 푼다: 관제 앱이 범위 안 사람의 **PTT 회선 AoR 에 `Event: dialog` 를 구독**한다(대상 = `/provisioning/me`
`dispatch.members[].pttId`, VoLTE 회선 `volteAor` 와 나란히). 인가는 §5.2 `CanWatch` 그대로 — 대상의 전화 그룹은
`EffectiveGroupOf`(PTT 회선은 파생 `ptt_subscriptions.pickup_group`, §3.2)로, 감시자의 범위는 역할(`CCspRoleMap`)로 판정한다
(`CscfModule` SUBSCRIBE 초기 구독, VoLTE 와 같은 코드).

**dialog 본문 (PTT 회선)** — `CGroupCallService` 가 참가 leg 마다 dialog 1건을 낸다(`SendPttDialogEventNotify`,
`CollectPttDialogs` 초기 full 스냅샷):
- `entity`/`<local>` = 참가자 PTT 회선(`sip:+82…@<PTT 도메인>` — 도메인은 감시 대상 회선의 서비스 종류로 정한다,
  `DialogDomainSuffixFor`), `id` = 참가자 leg Call-ID, `direction` = 개시자 `initiator` / fan-out 초대 `recipient`.
- `<remote><identity>` = **세션 URI** `sip:<group id>@<PTT 도메인>`(`priv-…`·`adhoc-…`·`g002`) — UE 의 dialog 상대는
  focus(PTT-AS)이므로 RFC 4235 §4.1.6 그대로다. 앱은 같은 remote 를 가진 dialog 를 한 세션으로 묶는다(참가자 = 감시
  중인 회선 중 그 세션에 있는 사람).
- dialog 안의 확장 요소(RFC 4235 §4.1 `xs:any ##other`)
  `<mcptt xmlns="urn:cims:xml:ns:dialog-info:mcptt" session-type="private|adhoc|prearranged|chat|broadcast"
  session-id="priv-…" initiator="+82…" emergency="false" imminent-peril="false"/>` — 카드의 종류 배지·개시자·긴급 표시.
- 상태: fan-out 18x `early` → 200/개시자 accept `confirmed` → BYE·세션 해제·등록 해제·미디어 노드 다운 `terminated`
  (`OnCallStarted`/`OnCallTerminated`/`ClearUserCall`/`TerminateGroupLocal`/pending 취소 전부). **청취 leg(recvonly)는
  내지 않는다** — 참가가 아니고, 은닉 정책과 무관하게 일관되게 뺀다. TAS 의 `NotifyDialogState` 는 PTT 세션 leg
  (`GetGroupCallSession`)를 만나면 여기로 위임한다(종전에는 VoLTE 도메인·remote 없는 반쪽 dialog 가 나갔다).
- 그룹 세션(멤버 그룹)도 같은 규칙으로 나간다 — 앱은 remote 가 멤버/청취 범위 그룹이면 ①/② 카드의 참가 정보로 흡수하고,
  `priv-`/`adhoc-` 이면 타인 세션 카드로 그린다. `members[]` 에는 역할 `monitor_call=all` 일 때 **PTT 전용 가입자**(VoLTE 회선
  없음, `volteAor=""`)도 실린다 — 현장 PTT 단말 간 사설콜이 보이려면 그 회선을 구독해야 한다.

**즉석 세션의 참가자 명단·청취** — 세션 URI 를 알게 된 관제사가 `Event: conference` 구독(로스터)·`a=recvonly` 합류(청취)를
하면 CSP 는 **즉석 세션 관측 인가** `CGroupCallService::CanObserveEphemeral` 로 판정한다: 참가자(fan-out 대상)는 항상 허용,
그 외는 자격 `allow_ambient_listening`(§5.6 과 같은 TS 24.484 자격) **+ 참가자 중 한 명의 전화 그룹이 관측자 역할의
`monitor_call` 안**(`CanWatch` — VoLTE Join 의 "어느 한 당사자 범위 안" 과 같은 규칙). 즉석 세션에는 그룹 문서·
`ptt_listen` 대상 항목이 없으므로 `ptt_listen` 축을 쓰지 않는다. 불허는 conference 403 + `Warning: 138`, 청취 INVITE
403(사유 `ephemeral …`, 감사 `denied`). 종전 "즉석 세션은 게이트 없음" 은 폐기 — 세션 id 를 아는 것만으로 타인의
사설콜 로스터가 열리지 않는다.

### 5.7 감사 이벤트 (G8)

감청은 당사자가 모르는 동작이므로 **감사 이벤트를 필수**로 남긴다 — 카탈로그 `E-AUD-016`
`event=call_monitored`(kind=audit, source=CSP): `monitor`(관제사 회선 id), `role`(관제사 역할 id), `session`
(relay `session_id`/sesid), `targets`(A/B id), `started_at`/`ended_at`/`dur_ms`, `tap_mode`. 시작·종료
각 1건. PTT 그룹콜 청취(§5.6)도 같은 코드로 남긴다 — `session`/`target_a`=PTT 그룹 id, `target_b` 없음,
`tap_mode=ptt_listen`, `role`=관제사의 역할(`CGroupCallService::EmitPttListenAudit`). FM push 경로는
[../alarm_self_reporting.md](../alarm_self_reporting.md), 카탈로그 행은 [../alarm_catalog.csv](../alarm_catalog.csv).
`call.json` 에도 `monitors[]` 로 남긴다(당사자 표시 UI 에서는 숨기고 감사 화면에서만 노출). 감청 대상 범위
(역할 `monitor_call`)는 관제 업무 근거가 있는 통화로 한정하는 운영 규약을 전제한다.

**열람** — 콘솔 `장애 > 감사 이력`(`/alerts/audit`, `requiredRole=manager`): `kind=audit` 이벤트를 단계(시작/종료/
거절)·감청자·역할·세션·대상·방식·시간 열로 펼친다(`core.audit-history` 위젯, CSV). 서버 게이트는 OAM
`GET /api/v1/events` — manager 미만 계정에는 `kind=audit` 이벤트를 결과에서 제외하고 `kind=audit` 명시 조회는 403
(`code=` 필터 추가). 일반 이벤트 이력 화면의 "감사" 분류도 같은 게이트를 받는다.

감사 로그 자체의 무결성이 통제의 핵심이다: "누가 무엇을 감청했나" 의 **열람은 `audit.read`(콘솔 `manager` 이상)로
제한**하고(감청 수행 권한 `monitor_call` 과 분리), 보존 기간은 조직 정책을 따르되 감청 감사는 일반 이벤트보다 길게
둔다. 감청 leg 개설 실패(403/481/488)도 시도로 남긴다(무단 시도 추적).

### 5.7a 통합 이력 조회 · 메시지 모니터링 (관제 데스크 ②④ 패널)

관제 앱의 내역 패널(② PTT 내역 · ④ 통화 내역)과 **메시지 모니터링**은 하나의 계약으로 지난 이력을 받는다:
`GET /provisioning/history?kind=call|ptt|message&since=&limit=`(CSC 4430, PKCE, 계약 정본
[android_ue_provisioning.md §3-2](android_ue_provisioning.md)). 구조는 **하이브리드**다 — 진행 중(live)
상태(링잉·floor·참가자 수)는 표준 구독(RFC 4235 dialog · RFC 4575 conference)이 그대로 담당하고
(폴링으로 대체하지 않는다 — 수초 미만 상태 유실·부하), 지난 이력만 이 API 가 커서(`since`→`nextSince`)로 준다.

- **범위 게이트**(서버 해석, 앱은 enum 미해석): `call`·1:1 `message` = 역할 `monitor_call`(감시 대상 가입자,
  §5.2 `CanWatch` 규칙 2 와 같은 규칙) / `ptt`·그룹 `message` = 역할 `ptt_listen`(청취 대상 PTT 그룹, §5.6 `CanListenPtt`).
  역할이 없거나 두 범위가 모두 `none` = `403 no_monitor_scope`. 백엔드 = 공유 NAS 파일 SoT(통화 `call.json`·PTT `session.json`·그룹 SDS
  `message/…/messages.jsonl`·1:1 SDS `message_direct/…`)를 역할 범위로만 걸러 주는 얇은 구독자 뷰
  (`csc/src/services/dispatch_history.py`) — 콘솔 이력 API(oam-svc `flow_logger`)를 재구현하지 않는다. 콘솔의 같은
  열람은 같은 능력(`history.read`, 전역 범위)이다.
- **메시지 모니터링 = 실시간(수 초 이내) 이력 조회만.** SIP MESSAGE 사본 전달은 채택하지 않는다(원 발·수신자
  은닉·중복 트랜잭션 회피). 그룹 SDS 는 이미 보관되고([mcdata_messaging.md §4.1](mcdata_messaging.md)), **1:1
  SDS/SMS 는 `Setup.McData.StoreOneToOneSds` 를 켜야** 보관된다(전량 보관, 열람은 조회 시점에 역할 `monitor_call`
  로 게이트 — 범위 한정 보관은 배정 변동 시 이력 결손이라 채택 안 함, [mcdata_messaging.md §4.3](mcdata_messaging.md)).
- **감사**: 열람 자체가 당사자 모르게 이력을 여는 동작이라 `E-AUD-016 call_monitored`(`tap_mode=history`,
  `hist_kind`·`count` 포함)로 남기고 열람은 §5.7 과 같은 manager 게이트를 받는다.
- **창 조회**: 같은 API 에 `until` 을 주면 [since, until] 창(관제 앱 [이력] 화면 — 하루 단위 페이지)이고,
  없으면 폴링 커서다. 종료분 항목에는 녹취 식별자 `recordingId`(세션 디렉터리의 `ServiceLogging.Dir` 상대 경로 — OAM
  `/api/v1/recordings/{id}` 와 같은 키)와 `hasRecording` 이 실린다. 항목에는 콘솔 VoLTE/PTT 이력과 같은 열을 그릴 종류별 확장
  필드(통화 = 상태·시작/응답/종료·종료사유, PTT = 종류·발언 턴/화자/발화/동시 발언·참여자·floor 축)와 최상위 시간대 분포 `hours` 가
  함께 실린다. **PTT 창 조회는 OAM 세션 인덱스(`/api/v1/ptt/sessions`, 콘솔 PTT 이력의 읽기 모델)를 청취 그룹의 저장 키로 좁혀
  프록시**하고(집계값은 CMP `segments.jsonl` 에서 나오므로 CSC 가 재구현하지 않는다) OAM 미도달 시 파일 스캔으로 폴백한다.
  **PTT 세션 상세**(참여자·입퇴장·floor 타임라인)는 `GET /provisioning/history/ptt/{recordingId}` — §5.7b 와 같은 범위 게이트 +
  OAM `/api/v1/ptt/history/{group_key}/{session}`·`/floor` 프록시(계약 [android_ue_provisioning.md §3-2a](android_ue_provisioning.md),
  감사 `tap_mode=history`·`hist_kind=ptt_session`).

### 5.7b 녹취 열람·재생 (관제 앱)

관제 앱은 이력 항목의 `recordingId` 로 `GET /provisioning/recordings/{id}`(세션·세그먼트 메타)와
`GET /provisioning/recordings/{id}/segments/{seq}/audio?slot=`(MP4/AAC — 202 변환 중이면 재시도) 를 부른다
(계약 [android_ue_provisioning.md §3-4](android_ue_provisioning.md)). CSC 는 **범위 게이트 + 프록시**만 한다 —
원시 RTP → MP4 변환·캐시·다중 버킷 결합은 oam-svc `handlers/recording.py` 하나가 소유하고(변환 상태·워커 풀·failed
마커) CSC 가 재구현하지 않는다(`csc/src/handlers/dispatch_recordings.py`, OAM 주소 = csc.json `Recording.OamUrl`,
비면 `https://{Fm.OamIp}:4419`).
- **범위** = §5.7a 와 같은 집합: `ptt/{groupKey}/…` 는 그룹 키(`ptt_groups.id` 또는 mcptt id)·`session.json` 의
  그룹이 역할 `ptt_listen` 대상, `volte/…/{cid}.d` 는 `call.json` 의 발·수신자 중 하나가 역할 `monitor_call` 대상. 범위 밖
  403, 경로 이탈(`..`) 400, 역할 없음 403.
- **감사**: 오디오 200 마다 `E-AUD-016 call_monitored`(`tap_mode=recording`, `recording`·`segment`·`slot`).

### 5.8 법적 근거·인가

- **감청은 운영자 인가에 근거한다.** 배정(누가 감청 범위가 있는 역할을 받나)은 `authz.manage`(콘솔 계정 `manager`
  이상)가 콘솔에서 명시 승인하고 그 자체가 감사된다(§5.7). 관제 앱의 관리 권한으로는 만들 수 없다. SIP 경로는 인메모리 인가만 집행한다(§5.3).
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

### 5.10 인가 회수 — 자격을 거두면 이미 선 것도 걷는다

인가는 **성립 시점에 한 번** 판정하고 그 결과를 세션에 담는다 — 구독은 dialog 에, 감청 leg 는 tap 에,
PTT 청취 leg 는 그룹 세션에. 그래서 역할을 거둬도 이미 선 것은 저절로 무너지지 않는다. 구독은 최대
1시간(RFC 6665 §4.2.1.1, `SUBSCRIBE_MAX_EXPIRES_SEC`) 갱신으로 살고 leg 는 통화가 끝날 때까지 산다.
감청·청취처럼 **회수가 즉시여야 하는 권한**에서 이것은 구멍이다.

회수는 두 겹이다.

| 겹 | 언제 | 무엇을 | 구현 |
|---|---|---|---|
| ① 능동 종료 | 인가 축이 바뀐 직후 | 그 순간 인가를 잃은 구독·leg 전부 | `csp/AuthzRevoke.cpp` `CspAuthz::RevokeUnauthorized()` |
| ② 갱신 시 재검사 | 다음 SUBSCRIBE refresh | ①이 놓친 잔여 | `csp/CscfModule.cpp` — dialog·conference 인가에서 `!bRefresh` 게이트 제거 |

**②는 403 만으로 끝나지 않는다.** RFC 6665 §4.1.2.2 는 갱신이 실패해도 "the original subscription is still
considered valid for the duration of the most recently known 'Expires' value" 라고 하고, 구독자가 종료로
해석해야 할 응답 목록(404·405·410·416·480·481·484·489·501·604)에 **403 은 없다**. 거절만 하면 구독자는 구독이
살아 있다고 보고 서버 기록도 남아 만료까지 NOTIFY 가 계속 나간다. 그래서 갱신을 거절할 때는 **종료 NOTIFY 를
함께 보내고 구독 기록을 지운다**(`TerminateDeniedRefresh`). 사유는 `deactivated` 다 — 이 거절은 «지금 이
순간의 판정» 이라 그룹 이동의 두 통지 사이에 도착한 갱신이면 `rejected`("재구독하지 말 것")가 정당한 관제사를
영구히 눈멀게 한다(아래 사유 표와 같은 판단). 초기 구독 거절에는 끝낼 구독이 없으므로 403 만 보낸다.
기준은 «갱신인가» 가 아니라 **«구독자 dialog 가 이미 서 있는가»** 다(`bDialogStanding` = `bRefresh` 또는 요청에
To tag 가 있음) — 등록 뒤 재검사(아래 세대 규칙 2)에서 자격을 잃은 경우도 같은 기준으로 종료 NOTIFY 를 보낸다.
한계 하나: CSP 재기동 뒤의 **상태 없는 in-dialog 갱신**을 인가 검사에서 거절할 때는 서버에 그 구독 기록이 아직
없어 종료 NOTIFY 를 보낼 수 없다 — 403 만 간다. 서버 기록이 없으니 NOTIFY 도 나가지 않아 정보가 새지는 않고,
구독자 쪽 구독만 자기 만료까지 «살아 있는데 조용한» 상태로 남는다.

**인가 축이 바뀌는 계기 넷** — 넷 다 CSC 통지이고, 맵을 재적재한 **뒤에** 스윕한다(`csp/CscInterface.cpp`).

- `ROLE_CHANGED` — 역할 능력·범위·대상·배정 변경
- `USER_CHANGED`(POST/DELETE) — 회선 개설·삭제. 회선이 사라지면 그 회선의 역할 펼침도 사라진다
- `USER_CHANGED`(PUT) — **파생 `pickup_group` 이 바뀐 경우만.** 회선 집합은 그대로이므로 역할 맵은 재적재하지
  않고 그룹 축만 다시 판정한다. 이 계기가 필요한 이유: CSC 는 멤버 이동을 `PHONE_GROUP_CHANGED` + `USER_CHANGED(PUT)`
  **두 통지**로 보내는데, 앞 통지의 스윕 시점에는 멤버 색인에서 빠졌어도 `EffectiveGroupOf` 가 **아직 낡은 사용자
  캐시로 폴백**해 «여전히 같은 그룹» 으로 판정한다(`csp/CspPhoneGroup.cpp` `EffectiveGroupOf`). 그래서 캐시가 실제로
  바뀐 뒤 한 번 더 걷어야 회수가 성립한다
- `PHONE_GROUP_CHANGED` — 그룹 멤버십은 `CanWatch` 규칙 1(같은 전화 그룹)과 `monitor_call=own` 의 답이다
- `CSC_RESTART` — 재기동 중에 바뀐 것을 재적재 값으로 다시 판정

**걷는 대상 넷과 판정** — 판정식은 **성립 시점과 같아야 한다.** 다르면 허용된 것을 걷거나 잃은 것을 남긴다.

| 대상 | 판정 | 회수 방법 |
|---|---|---|
| dialog 구독 | `CanWatch(구독자 회선, 감시 대상의 전화 그룹)` — 자기 자신 감시는 예외로 유지 | `NOTIFY Subscription-State: terminated` + 구독 삭제 (사유는 계기별 — 아래 표) |
| conference 구독 | `CheckConferenceSubscribe()` (§5.6 과 같은 함수) | 같음 |
| 감청 leg(tap) | 당사자 본인은 유지 / 역할 없으면 회수 / 양 peer 전화 그룹 중 하나라도 `CanWatch` 면 유지 (§5.3 Join 과 같은 식) | 감청자에게 BYE → `HandleMonitorLegEnd` 가 tap 회수·감사 `ended` |
| PTT 청취 leg | 자격 `allow_ambient_listening` + 범위(즉석 세션 `CanObserveEphemeral` / 그 외 `CanListenPtt`) — §5.6 과 같은 2단 | 청취자에게 BYE **+ `OnCallTerminated` 직접 호출** |

**종료 사유는 계기에 따라 둘이다**(RFC 6665 §4.1.3). 안전성이 아니라 **복구 가능성**의 문제다 — 어느 쪽이든
서버는 다음 구독을 다시 판정하므로 회수는 그대로 성립한다.

| 계기 | 사유 | 왜 |
|---|---|---|
| `ROLE_CHANGED`·`CSC_RESTART` | `rejected` | 인가 정책 자체가 바뀌었다. 규격 정의가 그대로 이 경우다("terminated due to change in authorization policy"). 재구독해도 403 이므로 하지 말라고 알린다 |
| `PHONE_GROUP_CHANGED`·`USER_CHANGED` | `deactivated` | CSC 는 한 사람의 그룹 이동을 **통지 둘**(옛 그룹 PUT + 새 그룹 POST)로 보낸다. 그 사이 스윕은 «아직 어느 그룹에도 없는» 순간을 볼 수 있고, 거기에 `rejected` 를 보내면 정당한 관제사가 재구독하지 않아 **영구히 눈이 먼다**. `deactivated`("SHOULD retry immediately")면 즉시 재구독하고 서버가 그때 옳게 판정한다 |

만료(`timeout`)와는 셋 다 뜻이 다르므로 섞어 쓰지 않는다.

**적재가 실패하면 회수하지 않는다.** 스윕의 판정 근거는 방금 재적재한 맵이다. 그 적재가 부분적으로만
성공하면 «역할은 있는데 회선 펼침이 없는» 맵이 서고, 스윕은 그것을 확정 철회로 읽어 **정상 감청·구독을
전부 끊는다** — DB 일시 장애가 서비스 정지가 된다. 두 겹으로 막는다.

- `CDbManager::LoadAllRoles` 는 대상·배정 조회 중 **하나라도 실패하면 기존 맵을 그대로 두고 `false`** 를
  돌린다(종전에는 빈 결과로 진행하고 `true` 를 돌렸다). 게시는 모든 조회가 성공했을 때만.
- `csp/CscInterface.cpp` 의 계기들은 **적재 성공일 때만** 스윕한다. 실패하면 기존 성립물을 유지한다 —
  회수를 늦추는 쪽이 멀쩡한 감청을 끊는 쪽보다 안전하다.
- **늦추는 것과 잃는 것은 다르다.** 통지는 한 번뿐이므로, 실패한 자리에서 끝내면 DB 가 복구돼도 **다음 변경
  통지가 올 때까지 옛 권한이 그대로 유지된다.** 그래서 실패를 **빚으로 남기고**(`NotePolicyReloadOwed`) 1초
  주기에서 갚는다(`RetryPendingPolicyReload`) — 재적재에 성공한 그 시점에 회수한다. 간격은 2·4·8…60초로
  늘리되 **포기하지 않는다**: tap·멤버 회수와 달리 정책 반영에는 대신 갚아 줄 최종 안전망이 없다.
- 빚에는 **세대**가 붙는다. 재적재는 락을 놓고 도는데(DB 왕복) 그동안 CSC 수신 스레드가 새 실패를 기록할 수
  있고, 완료 처리에서 플래그를 무조건 내리면 **그 새 빚까지 지워진다**. 집어 온 세대와 지금 세대가 같을 때만
  갚은 것으로 치고, 다르면 유지한 채 곧바로 다시 돈다. 같은 이유로 **이미 빚이 있을 때는 기한을 미루지
  않는다** — 실패 통지가 연달아 오면 매번 2초씩 밀려 영원히 갚지 못한다.
- 갚을 때는 **사용자 캐시도 같이 읽는다**(`gclsCspUserMap.LoadFromDb`). `EffectiveGroupOf` 의 폴백이 그 캐시라,
  낡은 채로 판정하면 그룹에서 빠진 사람이 «여전히 같은 그룹» 으로 보인다(위 `USER_CHANGED(PUT)` 계기와 같은 이유).
  이때 성패는 반환값이 아니라 **조회 불능 여부**(`CDbManager::LoadAllUsers` 의 `pbUnavailable`)로 본다 — 반환값은
  «적재된 행이 있는가» 라 가입자 0명인 현장을 영구 미납으로 만든다. `CSC_RESTART` 의 재동기도 같은 판정을 쓴다.

**없음·불허·조회 불능을 가른다.** 셋을 섞으면 DB 장애가 «권한 상실» 로 읽힌다. 방향은 성립이냐 회수냐에
따라 반대다.

| | 새로 세울 때(합류·구독) | 이미 선 것을 걷을 때(회수) |
|---|---|---|
| 조회 불능 | **막는다** (fail closed) | **걷지 않는다** (fail open) — 빚으로 남긴다 |

막을 때의 **응답 코드도 사실과 같아야 한다.** 판정 자료를 읽지 못한 것은 «자격이 없다» 가 아니므로 conference
SUBSCRIBE 는 403 이 아니라 **503 + `Retry-After`** 로 답한다 — 403 을 받은 구독자는 자격을 잃은 것으로 보고
물러나 DB 가 살아나도 스스로 복구하지 않는다. 같은 자리에서 갱신(이미 한 번 인가받은 구독)은 **끊지 않고 그대로
둔다**(fail open) — 판정도 못 한 채 눈을 감기는 쪽이 더 나쁘다. 등록 뒤 재검사에서의 조회 실패도 같다. 세 경우
모두 빚으로 남긴다.

fail open 의 기준은 «갱신처럼 보이는가» 가 아니라 **서버가 전에 그 구독을 승인했다는 기록이 있는가**(`bRefresh`)
다. 재기동 뒤의 상태 없는 in-dialog 갱신에는 그 기록이 없고 To tag 는 요청이 주장하는 값일 뿐이라, 판정 자료 없이
그것만 믿고 열어 주면 인증된 계정이 장애 창을 노려 남의 그룹 로스터를 얻을 수 있다 — 그 경우는 새 구독과 같이
다룬다(503).

같은 판정 함수가 양쪽에 쓰이므로 판정이 «불능» 여부를 함께 돌려주고(`ListenDenyReason`·`CanObserveEphemeral`·
`CheckConferenceSubscribe` 의 `pbUnavailable`, `CDbManager::SelectPhoneGroup`), 호출자가 방향을 정한다.

**게시는 원자적이어야 한다.** 재적재를 `Clear()` + `Insert()` 반복으로 하면 독자가 빈 맵이나 절반만 찬 맵을
본다. 그 순간 구독 갱신 재검사가 돌면 **권한이 그대로인 관제사가 403 + `rejected` 를 맞는다.** 완성된
스냅샷을 락 하나로 건다(`CCspRoleMap::Replace`·`CCspPhoneGroupMap::Replace`).

**비용** — 스윕은 구독 전수를 순회하지만 판정은 인메모리 맵 조회뿐이라 아무것도 걷을 것이 없는 흔한 경우는
값싸다. DB 를 읽는 것은 conference 구독과 PTT 청취 leg 의 자격 확인(`allow_ambient_listening`)뿐이고 둘 다 수가
적다. 스윕은 CSC 통지를 처리하는 그 스레드에서 동기로 돈다 — 같은 자리에서 이미 맵 전량 재적재를 하므로
새로 생긴 제약은 아니다. 빚을 갚는 경로만 예외로 **CSP 메인 루프(1초)** 에서 도는데(`CspServer.cpp`
`RetryPendingPolicyReload`), 빚이 없으면 락 하나 잡고 즉시 반환하므로 평소 비용은 없다.

**판정식은 한 곳에만 둔다.** 같은 규칙을 개설·개설 중 재확인·회수 스윕 세 곳에 따로 적으면 반드시
갈라지고, 갈라지면 허용된 것을 걷거나(서비스 장애) 잃은 것을 남긴다(보안 구멍). 그래서 각 축의 판정을
함수 하나로 모으고 셋이 그것만 부른다 — 감청은 `CTasModule::CanMonitorPair`, PTT 청취는
`CGroupCallService::ListenDenyReason`.

**개설 중인 것은 스윕이 못 본다.** 스윕은 **등록된 것만** 본다. 그런데 인가 판정과 등록 사이에는 틈이
있다 — 감청 leg 는 CMP 왕복(`RELAY_TAP_ADD`), PTT 청취 leg 는 세션 등록 뒤의 `PTT_JOIN`, 구독은 응답 조립.
그 사이에 자격을 거두면 스윕은 아직 없는 것을 지나치고, 개설 경로는 판정을 다시 하지 않아 **권한 없는
것이 확립된다.** PTT 는 더 나쁘다 — 스윕이 세션 맵에서 지우고 `PTT_LEAVE` 를 보낸 뒤 개설 경로가 이어서
`PTT_JOIN` 을 하면 **주인 없는 CMP 청취 멤버**가 남는다.

**정책 세대**(`CspAuthz::PolicyGeneration`)로 닫는다. 규칙 셋이다.

1. **판정보다 먼저 읽는다.** 판정 뒤에 읽으면 «허용 판정 → 정책 변경·스윕 → 세대 읽기» 순서에서 이미 오른
   값을 읽어 재검사를 건너뛴다. 먼저 읽으면 그 경우 세대가 달라 재판정이 돈다. 반대로 읽기와 판정 사이에
   올라가면 새 정책으로 판정했는데도 달라 보이지만, 재검사가 **재판정**이라 같은 답이 나온다(오탐 없음).
2. **등록 뒤에 한 번 더 본다.** 등록이 끝나면 이후의 어떤 스윕도 그것을 보므로, 남는 것은 «등록 전에 지나간
   스윕» 뿐이고 세대가 그 사실을 알려 준다. PTT 는 세션 맵에 **아직 있는지**도 함께 본다 — 없으면 스윕이
   이미 지나갔다는 뜻이다. 이 시점의 회수는 스윕이 했을 일과 같다(BYE + tap/멤버 회수).
3. **세대는 스윕보다 먼저 올린다** — `RevokeUnauthorized` 진입 시. 그래야 스윕이 못 본 것이 등록 때 걸린다.

적용 지점 = 감청 Join(`TasModule`), PTT 청취 합류(`GroupCallService`), dialog·conference SUBSCRIBE(`CscfModule`).
셋 다 «판정 전 세대 읽기 → (200 전 1차 재검사) → 등록 후 2차 재검사» 다.

**CMP 회수 실패를 성공으로 처리하지 않는다.** `RELAY_TAP_REMOVE` 가 유실되면 CMP 는 계속 RTP 를 복사하는데
CSP 는 맵에서 지운 뒤라 다음 스윕의 대상도 아니다 — 회수가 조용히 새는 자리다. 실패한 건은 재시도
대기열로 옮겨 1초 Tick 에서 지수 백오프(1·2·4·8·16·32초)로 다시 보내고, 그 뒤로는 30초 간격으로 **계속**
보낸다. 맵에는 되돌리지 않는다 — SIP leg 은 이미 끝났으므로 되돌리면 스윕이 죽은 호에 BYE 를 보낸다.

**포기하지 않는 근거**는 두 요청이 **자연 멱등**이라는 것이다 — `RELAY_TAP_REMOVE` 는 없는 tap 에,
`PTT_LEAVE` 는 없는 그룹·멤버에 각각 `OK` 로 답한다([cmp_media_api.md](../../api/cmp_media_api.md) §6.5·§7.5).
그래서 무한 재시도는 스스로 끝난다: 자원이 이미 사라졌으면 CMP 가 OK 로 답하고 대기열에서 빠진다. 남는 유일한
경우는 CMP 불통이고, 그때는 간격 상한이 비용을 묶는다. 반대로 상한을 두고 포기하면 «원 통화가 끝나면
`RELAY_REMOVE` 가 일괄 회수한다»(§5.9)는 안전망은 **자원** 만 지킨다 — 자격을 잃은 감청·청취는 그때까지 계속
들린다. «언젠가 자원이 회수된다» 와 «지금 안 들려야 한다» 는 다른 요구다.

대기열은 **같은 자원 키를 한 건으로 접고**(tap `(session_id, tap_id)`, PTT `(group, member)`) 상한 256 건을 둔다 —
접기 때문에 정상 운용에서는 살아 있는 leg 수를 넘지 않으므로, 상한에 닿았다면 회수가 아니라 상류가 고장난
상태다. 그 경우는 `LOG_ERROR` 로 **회수 유실**을 명시한다.

**tap_id 는 시도마다 새로 짓는다.** CMP 자원 키는 `(session_id, tap_id)` 이고 같은 키의 재요청은 멱등이라
(§6.5), Call-ID 만으로 지으면 같은 호에 다시 붙인 tap 과 앞선 시도가 **같은 자원**이 된다 — 대기열에 남은 옛
회수가 방금 연 감청을 걷는다. PTT 에서 같은 사고를 이미 겪었다(`PurgePendingLeave` — CMP 멤버 키가
`(group, user)` 라 재합류 뒤의 옛 `PTT_LEAVE` 가 새 멤버의 미디어를 걷었다). 그래서 `tap-<callid>-<일련번호>` 로
짓는다.

**응답을 못 받은 `RELAY_TAP_ADD` 는 «안 걸렸다» 가 아니다.** CMP 가 tap 을 만든 뒤 응답만 유실됐을 수 있는데,
CSP 는 그 호를 거절하므로 감청 leg 기록이 남지 않는다 → 어떤 스윕도 그 tap 을 찾지 못하고 **기록 없는 감청**이
원 통화가 끝날 때까지 CMP 안에 산다. 확정 거절(CMP 가 오류 코드로 답한 경우)은 만들어지지 않았으니 그대로
두고, 불명(`TIMEOUT`/`PARSE`)만 회수를 대기열에 건다.

**leg 회수는 BYE 만으로 끝나지 않는다.** psip 의 로컬 `StopCall` 은 dialog 를 지우고 BYE 를 보낼 뿐
`EventCallEnd` 를 올리지 않는다(`ext/psip/SipUserAgent/SipUserAgentCall.hpp`). 뒷정리를 직접 태우지 않으면
세션 맵·CMP 멤버 해제(`LeaveGroup`)·tap 회수·감사 `ended` 가 모두 남아 **단말만 끊기고 미디어는 계속
복사된다.** 그래서 감청 leg 는 `HandleMonitorLegEnd`, PTT 청취 leg 는 `OnCallTerminated` 를 BYE 직후 직접
부른다(`CheckMemberState` 의 강제 종료와 같은 순서).

**감사 `started` 는 등록 후 재검사보다 앞에 올린다.** 거기까지 왔다면 tap 이 붙었고(감청) CMP `JoinGroup` 이
끝났다(PTT 청취) — 미디어가 실제로 흘렀다. 재검사가 곧바로 회수하면 회수 경로가 `ended` 를 올리므로, `started`
를 뒤에 두면 **짝 없는 `ended`** 가 남는다(§9 M7 은 시작/종료 한 쌍을 요구한다).

**원 통화는 건드리지 않는다.** 걷는 것은 감청자·청취자의 leg 뿐이고 감시 대상의 통화와 다른 참가자는
그대로다 — 자격 회수와 업무 통화 차단은 다른 정책이다(§10 «자리/사람 분리» 의 같은 원칙).
이 원칙은 **정상 이탈에도 적용된다** — 사설콜·ad hoc 의 «한쪽이 끊으면 세션 종료» 규칙(§5.6a,
TS 24.379 §11.1)은 참가자 이탈에만 걸고 **청취 leg 이탈에는 걸지 않는다**. 관측자가 빠졌다고 당사자 통화를
끊으면 감청의 은닉성도 함께 깨진다.

**한계** — 회수는 CSP 인메모리 성립물만 본다. 이미 인도된 미디어(단말이 받은 RTP)나 앱이 받아 둔 화면
상태는 되돌릴 수 없다. **그리고 지금 단말은 종료 NOTIFY 를 화면까지 올리지 않는다** — 구독 수명은 pjsip
`evsub` 가 쥐고 있는데 CIMS 종료 콜백(`pjsua_pres.c` `cims_conf_on_evsub_state`)이 슬롯만 해제하고 앱에
알리지 않기 때문이다([ue_sdk.md §11](ue_sdk.md)). 그래서 `deactivated` 의 «즉시 재구독» 도 아직 성립하지
않는다. 회수 자체는 서버가 집행하므로 보안 구멍은 아니지만, **관제사 화면에는 끊긴 대상의 낡은 행이 남는다.**

---

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
| **CSC** `handlers/dispatch.py` · `services/authz.py` | `/api/v1/phone-groups` CRUD + `/members`(`directory.read`/`directory.write` — 콘솔 manager 전역, 관제 앱은 같은 코드 `dispatch_phone_group` 을 범위 안에서), `pickup_group` 파생 갱신(멤버 추가/제거/그룹 삭제 → 같은 person 의 volte·voip·ptt 전 회선 재계산 → 바뀐 회선마다 USER_CHANGED) + 가입자 API 직접 편집 409 `derived_from_phone_group`·새 회선 개설 시 파생값 상속, `PHONE_GROUP_CHANGED` 통지, pilot 충돌 409 · `/api/v1/roles`(전부 `authz.manage`: 내장 4행 읽기 전용 403 `builtin`, 커스텀에 위임 불가 능력 400 `not_delegable`, 배정 남은 삭제 409 `assigned`, 배정 PUT 사람당 하나 `moved_from`, 배정/해제 시 `allow_ambient_listening` 동기, `ROLE_CHANGED`) · 단일 판정 `can(principal, capability, target)`(§2.3 — roles 테이블 미적용이면 내장 4행) · `/provisioning/me` `phoneGroup`+`dispatch`(`dispatch_discovery`, 전환기 합성 필드) + `ETag` 304 · 이력·녹취 게이트 = 역할(`_dispatch_scope_sets`) · 감사 actor `console:<login>`/`user:<id>` | 구현 |
| **CSP `CCspPhoneGroupMap`** (`CspPhoneGroup.h/.cpp`) + **`CCspRoleMap`** (`CspRole.h/.cpp`) | 전화 그룹 맵 = 그룹 id·pilot·멤버 인덱스, `EffectiveGroupOf`(멤버 인덱스 → `pickup_group`, org 폴백 없음), `DbManager::LoadAllPhoneGroups/SelectPhoneGroup`·`PHONE_GROUP_CHANGED`(uri=그룹 id, DELETE/단건/전량)·JSON fallback `DataFolder.PhoneGroup`. 역할 맵 = 회선 → 역할 인덱스(`LoadAllRoles` 가 `role_assignments(user)` 를 person 의 volte·voip·ptt 전 회선으로 펼침), `CanWatch(watcherLine, targetGroup)`(규칙 1 전화 그룹 / 규칙 2 `monitor_call`)·`CanListenPtt(line, pttGroup)`·`ListenHidden(line)`·`RoleIdForLine`, `ROLE_CHANGED`·`USER_CHANGED`(POST/DELETE)·`CSC_RESTART` 전량 재적재·JSON fallback `DataFolder.Role`(`assignments[]`=회선). `DISPATCH_GROUP_CHANGED` 는 전환 전 이름 — 두 맵 재적재 | 구현 |
| **CSP `CTasModule` 포크 집합** | `CTasForkSet`(TAS 소유 — 대기 leg 는 승자 확정 전까지 `CCallMap` 밖) · `TryDispatchPilot`(§4.2, 미등록 착신 분기의 `TryPickupDial` 앞) · `ResolveForkTargets`(등록·`busy_members=skip` 비통화·발신자 제외·`alert_order` 순·`MaxForkTargets` 절삭) · `StartAlert`(`alert_mode` 분기 — parallel 전원 / sequential 큐+첫 순번) · `AdvanceSequential`(§4.4a 다음 순번·단계 시한 재설정) · `ForkAlert`(leg 전용 SDES 서버 키·`P-Called-Party-ID`=대표번호) · `OnForkRing`(첫 180 만 A 에, SDP 없이) · `OnForkStart`(승자 → (A,승자) 쌍 CallMap 삽입 후 디스패처 정상 answer 경로가 RELAY_MODIFY·A 200, 패자 CANCEL, 늦은 200 은 BYE) · `OnForkEnd`(패자 최종 응답 흡수, sequential 다음 순번, 전원 실패 486/480, A 취소 → 전원 CANCEL+relay 회수) · `Tick`(1초 — `no_answer_sec` 만료 → sequential 다음 순번 / `OverflowFork`(대표번호면 그 그룹원 재포크·내선이면 단일 leg, 1단계) 또는 480) · `FindForkForPickup`/`PickUpFork`(§4.4 링잉 대표번호 호 당겨받기 — `PickUp` 의 CallMap 후보 폴백) · 대표번호 AoR dialog 이벤트(§4.5 — early/confirmed/terminated) | 구현 |
| **CSP `CscfModule`** | dialog SUBSCRIBE 인가 → `gclsRoleMap.CanWatch(구독자 회선, 대상 전화 그룹)`(§5.2 — 규칙 1 전화 그룹 / 규칙 2 역할); 대상이 대표번호면 그 전화 그룹(§4.5) · conference SUBSCRIBE 인가(§5.6, TS 24.379 §10.1.3.4.1) → `CGroupCallService::CheckConferenceSubscribe`(멤버 = `allow_conference_state` / 비멤버 = 자격+`CanListenPtt`), 403 `Warning: 138`·480 `Warning: 105`(`SendResponseWithWarning`) | 구현 |
| **CSP `ModuleDispatcher`** | `OnCallRing`/`OnCallEnd` 훅을 소비형으로(포크 leg 흡수) — CallMap leg 의 dialog 통지는 종전대로 통과 | 구현 |
| **CSP 설정** | `Setup.Sip.Dispatch.{MaxForkTargets,ForkRingTimeoutSec,MaxTapsPerSession}`, `Setup.DataFolder.{PhoneGroup,Role}`(render 기본 `phone_group`/`role`). `Setup.Sip.CallPickupId` 는 없다(피처코드 = 접속서비스 필드만, [volte_supplementary_services.md §5.2](volte_supplementary_services.md)) | 구현 |
| **CSP `CCallMap`** | 감청 leg 는 CallMap 밖(TAS `m_mapMonitorLeg`)에 두어 dialog 이벤트·픽업 후보에서 자연 제외(별도 표식 불요). Join 대상 대조는 `MatchReplacesDialog` 재사용 | 구현 |
| **CSP `CTasModule` 감청** | `HandleIncomingJoin`(§5.3 — Join 파싱·역할 필수 + `CanWatch` 인가(같은 전화 그룹만으로는 BLF 까지, Join 은 역할이 있어야 한다)·recvonly·세션당 tap 상한·offer SDES→tap egress 서버 키·200 answer sendonly+`a=ssrc` 라벨) · `HandleMonitorLegEnd`/`ReleaseSessionMonitors`(M BYE·원 통화 종료 시 tap 회수) · `E-AUD-016` 발신(started/ended/denied, `role`=감청자 역할 id) | 구현 |
| **CSP `CGroupCallService` PTT 청취** | `ProcessGroupCall` 의 청취 leg 분기(§5.6 — `a=recvonly` 판정·비멤버 403·`SelectUserProfile` 자격 + `gclsRoleMap.CanListenPtt` 범위·`ListenHidden`(역할 `listen_visibility`)·활성 세션 없으면 480·answer sendonly·`PTT_JOIN recv_only=1`) · `CallSessionInfo.bListenOnly/bListenHidden`(세션 활성 판정 `HasActiveLeg`·로스터·조건 전파·참가자 DB/이력 제외) · `CanObserveEphemeral`(참가자 전화 그룹에 대한 관측자 역할 `CanWatch`) · `EmitPttListenAudit`(E-AUD-016 started/ended/denied, `role`) | 구현 |
| **CSP `CmpClient`** | `AddTap`(ssrc_a/ssrc_b 응답)/`ModifyTap`/`RemoveTap`, HEARTBEAT `resource.tap` 학습(`SupportsTap` — 미광고 CMP 는 Join 488) | 구현 |
| **CMP** | `PRtpTap`(청취 leg — SSRC 재매핑·SRTP egress·상향 폐기·RTCP SR 재매핑), `PRtpRelay::_taps` fan-out(복호 평문 ingress 복사), `RELAY_TAP_ADD/MODIFY/REMOVE` 핸들러, `resource.tap` 광고·STATS `taps[]`·풀(TapPoolSize/MaxTapsPerSession)·세션 회수 시 일괄 free(§6) | 구현 |
| **콘솔** | `구성 > 전화 그룹`(`/subscribers/phone-groups`, `PhoneGroupsPage` — 그룹 CRUD·멤버 transfer·`alert_order`·대표번호, 권한 열 없음) · `시스템 > 역할`(`/deploy/roles`, `RolesPage`, manager — 내장 프리셋 읽기 전용·관제 프리셋 생성·범위/대상·가입자·콘솔 계정 배정) · 계정 화면 역할 선택 = `GET /api/v1/roles` · 가입자 편집의 `pickup_group` 파생값 잠금(`derived_from_phone_group`) · **장애>감사 이력**(`/alerts/audit`, manager — `role` 열, 전환 전 이벤트의 `group` 폴백) | 구현 |
| **OAM** | `GET /api/v1/events` — `kind=audit` 열람 manager 게이트(미만은 결과 제외·명시 조회 403)·`code=` 필터 | 구현 |
| **OAM 게이트웨이** | csc `pkg.json` `gateway.routes` + `oam.json Gateway.Routes` 시드에 `/api/v1/phone-groups`·`/api/v1/roles`(기존 배포는 OAM 설정 PUT 으로 라우트 추가) · OAM `console_accounts.role` 은 커스텀 역할 id(`role-…`)도 수락(로컬 게이트에서는 monitor 등급) | 구현 |
| **단말 SDK `libcimsue`** ([ue_sdk.md](ue_sdk.md)) | `calledParty`(P-Called-Party-ID), `dialogWatch`(RFC 4235)·`join`(RFC 3911 recvonly, 200 OK a=ssrc 라벨 → `sources`), `pickup`, `transfer`, `joinGroupCall(listenOnly)` — `cimsue-cli` 로 dev 실측(Join 200·감청 RTP·caller/callee 라벨·픽업·REFER) | 구현 |
| **단말 앱(관제용 UI)** | dialog 목록·클릭→Join, SSRC 별 활성/레벨 표시(U10 관측 API 후속), PTT 청취 채널 UI(U6) — 화면 설계 정본 [dispatch_desktop_ui.md](dispatch_desktop_ui.md)(Windows WPF, 네 도킹 패널+감청 창·배너·핫키·응답 코드 문구) | Windows WPF 구현 완료 — 실기 시험은 서버 연결 후 일괄 |
| **cspsim** | `hunt`(`-pilot`, `-hunt_noanswer`, `-hunt_pickup` — D 의 `<code><pilot>` 지정 픽업, 마커 `pickup_status`/`t_answer_ms`) · `monitor`(dialog 구독→INVITE-Join 청취, 마커 `join_status`/`M_ssrc`/A·B·M RTP delta — SSRC 2개·은닉 판정) · `ptt_listen`(멤버 그룹콜 중 M 의 recvonly INVITE, `-listen_sendrecv` 비멤버 대조 — 마커 `join_status`/`M_recv`/`M_grant`/`M_deny`/`hidden`) · 수신 SSRC 집합·floor DENY/TAKEN 카운터·conference 로스터 누적 | 구현 |

②의 포크 집합이 절차상 유일한 구조 변경이고 나머지는 기존 훅·계약의 연장이다. 엔티티 분해(§3)는 인가 판정의
입력만 바꾸며 SIP 절차·CMP 계약은 그대로다.

**포크 집합의 위치(구현 결정)**: 대기 B-leg 는 `CCallMap`(leg 쌍 1:1 모델) 밖의 TAS 소유 맵에 두고, 승자 확정
시점에 (A, 승자) 쌍을 `CCallMap` 에 넣어 이후를 기존 1:1 경로(answer RELAY_MODIFY·re-INVITE·BYE·sweeper)에
넘긴다. 패자 leg 는 CANCEL 후 최종 응답(487)이 올 때까지 TAS 맵에 남아 이벤트를 흡수한다. B-leg INVITE 의
`To` 는 B2BUA 관례대로 그룹원 AoR 이고 `P-Called-Party-ID` 가 대표번호다(§4.3 — GroupCallService fan-out 과
동형). 포크는 공유 relay(peer1 포트 공용) 위에서만 성립하므로 RTP relay 비활성 노드에서는 503 이다.

---

## 8. 설정 정리 (운영 규약)

### 8.1 DB 스키마

목표 스키마(`sql/migrate_phone_groups_roles.sql`, 재실행 안전). 전환 전 스키마(`dispatch_groups` 계열 4 테이블,
`sql/migrate_dispatch_groups.sql`)와의 대응은 아래 전환 표.

```sql
-- 전화 그룹 (유선 전화 기능 — 관제 아님)
CREATE TABLE IF NOT EXISTS phone_groups (
    id              VARCHAR(64)  NOT NULL COMMENT '불변 키 (CSC 발급 pg-xxxxxxxx; 전환 전 dg- 값 유지) — pickup_group 값·상관 키',
    name            VARCHAR(128) NOT NULL DEFAULT '' COMMENT '표시 이름',
    pilot_id        VARCHAR(64)           DEFAULT NULL COMMENT '대표번호(AoR user part). NULL=대표번호 없음',
    service_ref     VARCHAR(64)           DEFAULT NULL COMMENT '대표번호 접속서비스 name (유선 VoIP)',
    alert_mode      ENUM('parallel','sequential') NOT NULL DEFAULT 'parallel' COMMENT 'TS 24.239 alerting mode',
    no_answer_sec   INT          NOT NULL DEFAULT 30,
    busy_members    ENUM('skip','alert') NOT NULL DEFAULT 'skip',
    overflow_target VARCHAR(64)           DEFAULT NULL COMMENT '무응답 넘김 대상(대표번호/가입 번호). NULL=480',
    org_id          INT                   DEFAULT NULL,
    created_at      DATETIME              DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_pilot (pilot_id),
    CONSTRAINT fk_pg_org FOREIGN KEY (org_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='전화 그룹';

CREATE TABLE IF NOT EXISTS phone_group_members (
    user_id     VARCHAR(64) NOT NULL COMMENT '가입자(회선) id — 가입자당 그룹 하나',
    group_id    VARCHAR(64) NOT NULL,
    alert_order INT         NOT NULL DEFAULT 0 COMMENT 'sequential 호출 순서',
    PRIMARY KEY (user_id),
    KEY idx_group (group_id),
    CONSTRAINT fk_pgm_group FOREIGN KEY (group_id) REFERENCES phone_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='전화 그룹 멤버';

-- 역할 (권한 = 능력 + 범위) — 정의·내장 프리셋은 mcptt_authorization.md §2·§3
CREATE TABLE IF NOT EXISTS roles (
    id                VARCHAR(64)  NOT NULL COMMENT '불변 키 — 내장 admin|manager|operator|monitor, 관제 role-xxxxxxxx',
    name              VARCHAR(128) NOT NULL DEFAULT '',
    builtin           TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '내장 프리셋(읽기 전용)',
    authz_manage      TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '역할·배정·범위 관리 — 내장 admin/manager 만',
    audit_read        TINYINT(1)   NOT NULL DEFAULT 0,
    directory_write   ENUM('none','own','all') NOT NULL DEFAULT 'none' COMMENT '조직/구성원/번호/전화 그룹 관리 범위 (own=org_id 하위)',
    directory_read    ENUM('none','own','all') NOT NULL DEFAULT 'none',
    ptt_group_manage  ENUM('none','own','scope','all') NOT NULL DEFAULT 'none' COMMENT 'own=본인 소유, scope=directory_write 범위',
    monitor_call      ENUM('none','own','listed','all') NOT NULL DEFAULT 'none' COMMENT '통화 감청·세션 관측·통화 이력/녹취 범위',
    ptt_listen        ENUM('none','listed','all')       NOT NULL DEFAULT 'none' COMMENT 'PTT 청취·conference 구독·PTT 이력/녹취 범위',
    listen_visibility ENUM('hidden','visible')          NOT NULL DEFAULT 'hidden' COMMENT 'PTT 청취 멤버 로스터 노출',
    history_read      ENUM('none','scope','all')        NOT NULL DEFAULT 'none' COMMENT 'scope=monitor_call/ptt_listen 범위',
    alarm_ack         TINYINT(1)   NOT NULL DEFAULT 0,
    mcptt_control     TINYINT(1)   NOT NULL DEFAULT 0,
    org_id            INT                   DEFAULT NULL COMMENT 'own 범위의 루트',
    created_at        DATETIME              DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    CONSTRAINT fk_role_org FOREIGN KEY (org_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='역할';

CREATE TABLE IF NOT EXISTS role_assignments (
    principal_type ENUM('console','user') NOT NULL COMMENT 'console=OAM 콘솔 계정(login_id), user=가입자 person(users.id)',
    principal_id   VARCHAR(64) NOT NULL,
    role_id        VARCHAR(64) NOT NULL,
    PRIMARY KEY (principal_type, principal_id),          -- 사람당 역할 하나
    KEY idx_role (role_id),
    CONSTRAINT fk_ra_role FOREIGN KEY (role_id) REFERENCES roles (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='역할 배정';

CREATE TABLE IF NOT EXISTS role_monitor_targets (
    role_id        VARCHAR(64) NOT NULL,
    phone_group_id VARCHAR(64) NOT NULL,
    PRIMARY KEY (role_id, phone_group_id),
    CONSTRAINT fk_rmt_role FOREIGN KEY (role_id)        REFERENCES roles (id)        ON DELETE CASCADE,
    CONSTRAINT fk_rmt_pg   FOREIGN KEY (phone_group_id) REFERENCES phone_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='monitor_call=listed 대상';

CREATE TABLE IF NOT EXISTS role_ptt_targets (
    role_id      VARCHAR(64) NOT NULL,
    ptt_group_id BIGINT      NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    PRIMARY KEY (role_id, ptt_group_id),
    CONSTRAINT fk_rpt_role FOREIGN KEY (role_id)      REFERENCES roles (id)      ON DELETE CASCADE,
    CONSTRAINT fk_rpt_ptt  FOREIGN KEY (ptt_group_id) REFERENCES ptt_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='ptt_listen=listed 대상';
```

콘솔 계정의 배정은 DB 가 아니라 OAM file_store `console_accounts[].role`(값 = `roles.id`)이다 — OAM 은 DB 없이
동작해야 하므로(내장 admin·부트스트랩) 콘솔 principal 의 배정은 JWT `role` 클레임으로 CSC 에 전달되고 CSC 가
`roles` 행으로 해석한다(내장 4행은 마이그레이션이 항상 시드한다).

**전환 표** (`migrate_phone_groups_roles.sql` — 재실행 안전, 데이터 보존):

| 전환 전 | 전환 후 |
|---|---|
| `dispatch_groups`(전화 열: id·name·pilot_id·service_ref·alert_mode·no_answer_sec·busy_members·overflow_target·org_id) | `phone_groups` 같은 열, **id 유지**(`dg-…` 그대로) |
| `dispatch_groups`(범위 열: monitor_scope·ptt_listen·listen_visibility·directory_admin·org_id) — 하나라도 `none` 이 아닌 그룹 | 그룹마다 역할 `role-<그룹 id>`(name = 그룹 name, `monitor_call`=monitor_scope, `ptt_listen`, `listen_visibility`, `directory_write`=directory_admin, `history_read=scope`, `org_id`) + 그 그룹 **전 멤버**의 `role_assignments('user', users.id, role)` |
| `dispatch_group_members` | `phone_group_members` |
| `dispatch_group_monitor_targets` / `dispatch_group_ptt_targets` | `role_monitor_targets` / `role_ptt_targets`(역할 `role-<그룹 id>`) |
| `ptt_user_profile.allow_ambient_listening` | 값 유지 + `ptt_listen≠none` 역할 배정자는 1 로 정합(백필) |
| `volte_subscriptions.pickup_group` / `voip_subscriptions.pickup_group` / `ptt_subscriptions.pickup_group` | 값 유지(= `phone_groups.id`). 유선 회선의 테이블 이동(`migrate_voip_subscriptions.sql`)도 값과 `phone_group_members` 행을 바꾸지 않는다. 컬럼 미적용 DB 에서는 CSP 가 부팅 프로브로 감지해 픽업·BLF 축을 비활성(INFO 로그) |

### 8.2 CSC 관리 API

- `/api/v1/phone-groups` — `GET`(목록) / `POST` / `GET|PUT|DELETE /{id}` / `POST /{id}/members` /
  `DELETE /{id}/members/{user_id}`. 검증: `pilot_id` 가 가입 번호(`volte_subscriptions`·`voip_subscriptions`·`ptt_subscriptions` 의
  `id`)/다른 pilot 과 충돌 → 409 `pilot_conflict`(역방향 — 가입 번호 개설이 대표번호와 겹치면 409 `number_exists`). 권한 = `directory.write`(콘솔 manager 전역 / 관제 `own|all` 범위 안 조직의 그룹).
- `/api/v1/roles` — `GET`(내장 4 + 커스텀) / `POST` / `GET|PUT|DELETE /{id}`(내장은 읽기 전용) / `PUT /{id}/monitor-targets`
  `{phone_group_ids:[…]}` / `PUT /{id}/ptt-targets` `{ptt_group_ids:[mcptt_group_id…]}` / `GET|PUT|DELETE /{id}/assignments`
  `{principal_type, principal_id}`. **전부 `authz.manage`**(콘솔 `manager` 이상). `authz_manage=1` 은 내장 행에만 허용(커스텀은
  400 `not_delegable`). `ptt_listen≠none` 배정/해제는 대상 person 의 `allow_ambient_listening` 을 동기하고 `ROLE_CHANGED` 를 낸다.
  콘솔 계정의 배정은 OAM `PUT /api/v1/console-accounts/{login_id}` `role`(값 = `roles.id`) 그대로.
- **관제 앱(가입자 토큰) 관리 API** — `/provisioning/directory/{admin,orgs,members,groups,phone-groups}`(§3.4) ·
  `/provisioning/recordings/{id}…`(§5.7b): MCPTT 서버(4430) 에 붙고 콘솔 관리 API 와 realm 이 다르지만 **판정은 같은
  `can()`** 을 지난다. 역할·배정 API 는 여기에 없다.
- 계약 상세 = [../../api/admin_api.md §6.7](../../api/admin_api.md).

### 8.3 접속서비스·csp.json

- **대표번호·관제 회선은 유선 VoIP 서비스(`kind=voip`)에 둔다** — `phone_groups.service_ref` 와 관제 회선 `service_ref`
  = 그 서비스 name. 피처코드(`pickup_feature_code`)·`transfer_allowed`·`media_srtp=required`·`sec_mechanisms=[tls]`·
  `media_nat_mode=off` 가 이 서비스의 필드다([sip_service_model.md §2-9](sip_service_model.md),
  [volte_supplementary_services.md §10.2](volte_supplementary_services.md)). 이동 VoLTE 서비스에는 피처코드를 두지 않는다.
- 신규 서비스 필드 없음. csp.json `sections.tas` 키:

| 키 | 기본 | 의미 |
|---|---|---|
| `Setup.Sip.Dispatch.MaxTapsPerSession` | 2 | 세션당 감청 leg 상한(§5.5) |
| `Setup.Sip.Dispatch.MaxForkTargets` | 32 | 대표번호 1건이 동시 포크하는 멤버 상한(제어평면 부하 방어 — 초과분은 `alert_order` 순 절삭). [../csp_control_plane_load_hardening.md](../csp_control_plane_load_hardening.md) |
| `Setup.Sip.Dispatch.ForkRingTimeoutSec` | 60 | `no_answer_sec` 상한(그룹 값이 이를 넘으면 clamp) |
| `Setup.DataFolder.PhoneGroup` / `Setup.DataFolder.Role` | `phone_group` / `role` | JSON fallback 디렉터리(§3.5) |

`Setup.Sip.CallPickupId`(전역 피처코드)는 없다 — 피처코드는 접속서비스 필드만 인정한다(기존 csp.json 의 값은 무시).

### 8.4 단말

> 관제용 앱의 구현 토대는 [ue_sdk.md](ue_sdk.md)(C++ 코어 `libcimsue` + Android/Windows SDK) 이며, 아래 요건과
> 코어 API 의 대응표는 그 문서 §7 이다.

관제용 앱은 `/provisioning/me` 의 두 블록으로 자기 데스크를 안다(계약 정본 [android_ue_provisioning.md §3](android_ue_provisioning.md)):
- **`phoneGroup`** — 전화 그룹 소속일 때. `groupId·groupName·pilotId·members[]`(같은 그룹원 = 그룹원 상태 띠·BLF 대상
  `userId·name·volteAor·pttId·extension`)·`etag`. 대표번호 대기열·픽업·"대표번호로 발신" 토글은 이 블록으로 켜진다.
- **`dispatch`** — 역할이 있을 때. `roleId·roleName·monitorCall·pttListen·listenVisibility·directoryWrite·orgCode·members[]·
  pttTargets[]·etag`. `members[]` = dialog 감시(§5.2 규칙 2) 대상 — CSC 가 `monitorCall` 을 CSP `CanWatch` 와 같은 규칙으로
  해석한 가입자(`listed` 대상 그룹원 / `all` 전 가입자 — `all` 에는 VoLTE 회선 없는 PTT 전용 가입자도 `volteAor=""` 로
  실린다). `pttTargets[]` = conference 구독·청취(§5.6) 대상. 감청 창·범위 채널·이력·관리 탭은 이 블록으로 켜진다.
- **전환기**: 현 `dispatch` 블록의 필드(`groupId/groupName/pilotId/monitorScope/pttListen/listenVisibility/directoryAdmin/
  orgCode/members[]/pttTargets[]`)를 두 블록에서 합성해 함께 내린다 — `monitorScope`=`monitorCall`, `directoryAdmin`=
  `directoryWrite`, `members[]` = 그룹원 ∪ 감시 대상. 구 앱은 그대로 동작한다.
- `extension` 은 가입 번호 끝자리(`Provisioning.ExtensionDigits`, 기본 4)로 망 주소가 아닌 표시 라벨. `etag`(블록) +
  응답 `ETag`/`If-None-Match` 304 — 주기 재조회로 편성 변경을 따라간다. 범위 enum 의 해석은 서버에만 있다(CSC 가 목록,
  CSP 가 게이트 — 두 규칙은 같다).

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
| | F7 dialog 정합 | 그룹원 B 가 대표번호·A·C dialog 구독(`-hunt_watch`), C 응답 뒤 **A(발신자) 선종료** → entity 별 자기 dialog(id 불변) confirmed→terminated 1회, local=entity·remote=상대·direction 불변(대표번호 recipient/A · A initiator/C · C recipient/A), entity 별 version 단조 |
| `S3-SCN-MONITOR` | M1 목록 | M 의 A dialog 구독(범위 안) → 200 + NOTIFY ≥1 |
| | M2 청취 | M Join INVITE → 200, M 수신 RTP delta>0 (SSRC 2개), A·B delta 변화 없음 |
| | M3 은닉 | A·B 에 re-INVITE/NOTIFY 0건, 같은 그룹 D 의 dialog NOTIFY 에 M leg 없음 |
| | M4 상향 차단 | M 송신 RTP → A·B 수신 delta 무변화 |
| | M5 인가 | M5a 범위 밖 역할(`own`, 다른 그룹)의 M' → 구독 403·Join 없음 / M5c 역할 없는 다른 그룹 M' → 구독 403 / **M5b 같은 전화 그룹원이지만 역할 없음** → 그룹원 BLF 구독 200·Join 403(미디어 무흐름); 미지 Call-ID → 481 |
| | M6 종료 | A BYE → M 에 BYE 수신 마커, CMP tap 회수(STATS `taps` 0) |
| | M7 감사 | `E-AUD-016` 시작·종료 2건 |
| | M8 회수 | 청취 확립 뒤 역할 범위 제거 + `ROLE_CHANGED` → M 에 BYE·수신 정지, **A↔B 는 통화 유지·수신 계속** (§5.10) |
| `S3-SCN-PTT-LISTEN` | L1 청취 합류 | 멤버 A·B 그룹콜 중 M(비멤버, `allow_ambient_listening=1`, 역할 `ptt_listen=all`) recvonly INVITE → 200, M 수신 RTP delta>0, M floor 요청 → DENY(GRANT 0), A 의 conference 로스터에 M 없음(hidden) |
| | L2 자격 없음 | `allow_ambient_listening=0` → 403 |
| | L3 범위 밖 | 역할 `ptt_listen=none` → 403 / L3c 전화 그룹원이지만 역할 없음 → Join 403 + conference 구독 403 `Warning: 138` |
| | L4 비멤버 일반 INVITE | sendrecv → 403 |
| | L5 공개 청취 | `listen_visibility=visible` → 200 + 로스터에 M(`roles` listener) |
| `S1` | CMP tap 단위(복사·PT 스탬프·상향 폐기·세션 종료 회수), CSP `Join` 파서·`MatchDialog` 단위 | gtest |

```bash
./cims-verify run --items S3-SEED,S3-SCN-FA          # F1·F3·F5·F6·F7 대표번호 호출 (parallel/sequential·overflow·픽업·dialog 정합)
./cims-verify run --items S3-SEED,S3-SCN-MONITOR     # M2·M5 감청
./cims-verify run --items S3-SEED,S3-SCN-PTT-LISTEN  # L1~L5 PTT 그룹콜 청취
```

`S3-SCN-PTT-LISTEN` 은 S3-SEED 의 PTT 자격 창(멤버 A·B)과 대상 그룹의 **비멤버** PTT 가입자(M)를 쓰고, M 의 관제
역할(`role-vfy-lsn-<group>`, `ptt_listen` 대상 = 그 그룹)과 `ptt_user_profile.allow_ambient_listening` 을 검사별로 시드·복원한다.

세 항목은 각자 공용 픽스처(`verify/lib/items/stage3/_dispatch_common.py` `DispatchFixture`)로 전화 그룹·역할을 DB 에
직접 시드하고 종료 시 복원한다(자기복원 — S3-SEED 는 관여하지 않는다): `S3-SCN-FA` = `pg-verify-a`(대표번호, 역할 없음 —
포크·픽업·그룹원 BLF 는 규칙 1) / `S3-SCN-MONITOR` = `pg-verify-a`(A·B) + `role-verify-mon`(`monitor_call=all`, M 의 person
배정) + M5 대조군(`pg-verify-b` 의 M' — `role-verify-out` own / 역할 없음 / 같은 그룹원이지만 역할 없음) /
`S3-SCN-PTT-LISTEN` = `role-vfy-lsn-<group>`(`ptt_listen=listed`+대상) 또는 역할 없는 `pg-vfy-lsn-<group>`. 배정은 회선의
person(`users.id`)에 하고, 통지는 `PHONE_GROUP_CHANGED`/`ROLE_CHANGED`/`USER_CHANGED`. 스키마 프로브: `phone_groups`+
`role_assignments` 둘 다 있으면 새 경로, 둘 다 없고 `dispatch_groups` 만 있으면 전환 전 경로(같은 의미를 관제 그룹으로
시드, M5b·L3c 는 SKIP), 어느 것도 없으면 SKIP.

---

## 10. 범위 외 / 향후 과제

- **끼어들기(barge-in)·3자 통화** — 관제사의 상향을 A/B 에 섞으려면 믹서가 필요하다. CMP MIX
  예약 기능(`(service, conf_id)`)의 실체화로 다룬다. tap 은 그 전 단계다.
- **TS 24.379 ambient listening**(remote-init 1:1) — 그룹콜 청취와 같은 `allow_ambient_listening`
  자격(§5.6)을 재사용하되, 단말 무표시 자동응답 + CSP `session-type=ambient-listening` 시그널링이
  추가로 필요하다. 단말 파트 선행.
- **History-Info(RFC 7044)** — 대표번호 재타게팅 이력의 표준 표현(§4.3, 현재 `P-Called-Party-ID` 로 대체).
- **3GPP LI 핸드오버(HI2/HI3·LEMF)** — 외부 사법기관 인도가 요구되면 별도 LI 게이트웨이(§5.8). 본 설계 범위 밖.
- **전화 그룹 겸임(N:M 멤버십)** — 채택하지 않는다(§3.2 확정). 겸임 요구는 `overflow_target`·지정 픽업으로
  흡수한다.
- **자리(관제석)와 사람의 분리 — 방향 확정, 미착수.** 지금은 관제석 = 가입자(회선 묶음 + 로그인)라 교대 근무에서
  계정을 나눠 쓴다. 그래서 감청·청취·녹취 열람의 감사 actor 가 **자리의 회선 번호**로 남고(§5.7b), 개인별 자격
  회수가 안 된다. 목표 구조는 **세 축**이다.
  - **사람** = IdMS 신원 + 역할 배정 + 감사 actor. 회선 없는 `users` 행은 지금도 합법이고(가입은 별도 테이블),
    로그인 신원은 `login:<login_id>` 로 파생된다.
  - **자리** = 회선 묶음(전화 그룹 멤버십·PTT 회선). 번호는 자리 수만큼만 둔다.
  - **사용 세션** = 사람↔자리 결박. 전환·만료·회수를 **서버가 집행**한다. 자리 전체의 통신 가능 여부와 특권
    (감청·청취) 사용 세션은 분리한다 — 리스 만료로 감청 자격을 거두는 것과 진행 중 업무 통화를 끊는 것은 다른 정책이다.

  **functional alias 는 이 구조의 기본 도구가 아니다.** TS 23.280 §10.13 은 MC service ID 를 가진 사용자의 별칭
  활성·인수이고 같은 별칭의 복수 사용자 활성도 정책에 따라 허용되므로, 별칭이 곧 자리의 배타 점유를 뜻하지 않는다.
  공유 단말의 사용자 교체는 TS 33.180 §5.1.3.2.1(로그아웃·재로그인 시 access token 으로 IMPU↔MC service ID 재결박)이
  더 직접적이다. 업무상 호출 정체성("당직 관제사")이 필요해지면 별칭을 그 위에 별도로 얹는다. 규격 표기 —
  활성 인가는 `allow-activate-*` 같은 권한 요소가 아니라 사용자 프로파일 `<FunctionalAliasList>` 의 `<entry>` 등재로
  표현되고, 타인 인수만 TS 24.484 §8.3.2.7 표 8.3.2.7-46 `<allow-takeover-functional-alias-other-user>`
  (TS 24.483 §5.2.48W9 대응)가 있다.

  **콘솔 계정의 IdMS 신원 통합은 선행 조건이 아니다** — [mcptt_authorization.md §2.1](mcptt_authorization.md) 이 이미
  principal 둘(`console:<login>` / `user:<users.id>`)과 토큰 realm 분리를 유효한 모델로 정의한다. 같은 정책을 쓰기 위해
  인증 저장소까지 먼저 합칠 필요는 없고, 합치면 CSC 장애 중 복구 콘솔 진입이 같이 막힌다(§9 는 별도 과제로 둔다).

  **순서는 회수 집행이 먼저, 역할 투영이 나중이다.**

  - **① 회수 집행 — 구현 완료([§5.10](#510-인가-회수--자격을-거두면-이미-선-것도-걷는다)).** 자격을 거두면 이미 선
    구독·감청 leg·PTT 청취 leg 이 실제로 걷힌다(능동 종료 + 갱신 시 재검사 두 겹). 점유 전환이 자격을 옮기는
    구조의 전제였다 — 이것이 없으면 "앉기/일어나기" 는 화면 상태일 뿐 서버가 집행하는 것이 아니다.
  - **② 사람 actor 병행 — 미착수.** 감사 actor 표기가 경로마다 다르다 — 관리 API 는 `user:<users.id>`,
    감청·청취·녹취(`E-AUD-016`)는 회선 번호. `monitor` 의 뜻을 바꾸기보다 사람 actor 필드를 **병행 추가**하고
    생산자·소비자(콘솔 감사 CSV 포함)를 같이 옮긴다.
  - **③ 사용 세션(점유) — 미착수.** 착수 전 확인해 둔 것:
    - `USER_CHANGED` 의 `PUT` 은 역할 맵을 재적재하지 않는다(`csp/CscInterface.cpp` — `POST`/`DELETE` 만). 점유
      전환을 역할에 반영하려면 `ROLE_CHANGED` 경로여야 한다.
    - 회선 없는 person 은 로그인과 주소록 조회까지는 되지만(`caller_identity` 가 `sub`→`users.id` 로 폴백),
      `/provisioning/me` 는 빈 `services` 를, `/provisioning/history` 는 `403 no_monitor_scope` 를 준다.
    - 토큰은 이미 사람과 회선을 나눠 싣는다 — `sub`=`users.login_id`(사람), `mcptt_id`=파생 회선
      (`csc/src/services/mcptt.py` `_load_login_accounts` — ptt→volte→voip 첫 가입, 회선이 없으면 `login:<login_id>`).
      점유는 이 파생을 **고정 파생이 아니라 점유 결과**로 바꾸는 일이다.
- **RFC 4662 RLS** 목록 구독(§5.2 표준형 — PTT 회선 dialog 구독(§5.6a)까지 더해 구독 수가 회선 ×2 로 늘어 우선순위가
  올라간다), **큐/ACD**(대기열·순번 안내).
- Android UE 의 Join 발신·SSRC 디먹스 UI — 서버 완성 후 단말 파트.

---

## 11. 문서 갱신 대상 (구현과 같은 변경에서)

- [volte_supplementary_services.md](volte_supplementary_services.md) §5 — 픽업 축 = `pickup_group` 만(org 폴백 없음), 전역 `CallPickupId` 제거.
- [mcptt_authorization.md](mcptt_authorization.md) — 역할·능력·범위 모델(정본), 내장 프리셋, `authz.manage` 불변 규칙.
- [sip_service_model.md](sip_service_model.md) §2-9 — `kind=voip`.
- [registration_binding_set.md](registration_binding_set.md) §2.2 — "병렬 포크 금지" 는 **한 사람의 멀티 디바이스** 범위임을 명시(전화 그룹 포크는 §4).
- [../../api/cmp_media_api.md](../../api/cmp_media_api.md) — §6.5 `RELAY_TAP_*`(분리 인도·`a=ssrc` 라벨링·RTCP SR), §5.1 `resource.tap`, §5.2 STATS `taps`, §9 `LIMIT`.
- [../db_schema.md](../db_schema.md) — `phone_groups`·`roles` 계열, `pickup_group` 값 의미, `ptt_user_profile.allow_ambient_listening`(역할 동기), `ptt_groups.allow_conference_state`.
- [../../api/admin_api.md](../../api/admin_api.md) — `/api/v1/phone-groups`·`/api/v1/roles`.
- [../alarm_catalog.csv](../alarm_catalog.csv) — `E-AUD-016 call_monitored` 정의·감지 행(`role` 필드).
- [recording.md](recording.md) — `call.json` `phone_group/pilot/alerted/answered_by/monitors[]`.
- [android_ue_provisioning.md](android_ue_provisioning.md) — `/provisioning/me` `phoneGroup`·`dispatch` 블록, §3-3 게이트.
- [../csp_control_plane_load_hardening.md](../csp_control_plane_load_hardening.md) — 포크 팬아웃 상한 `MaxForkTargets`(§8.3).
- [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §R1 — ambient listening 행에 본 문서 §5.6/§10 참조.
- [volte_supplementary_services.md](volte_supplementary_services.md) §6.2 — dialog 구독 인가가 **갱신에도** 걸린다(§5.10).
