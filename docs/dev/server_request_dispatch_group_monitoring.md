> 작업 요청서(단말 세션 → 서버 세션). 서버 구현·문서 반영이 끝나면 이 파일은 삭제하고 내용은 각 설계 정본(dispatch_center.md·mcptt_authorization.md·mcptt_api.md·csc.md)에 최종 상태로만 남긴다.

# 서버 보완 요청 — 관제조작반 PTT 그룹 CRUD · 활성 세션 모니터링 · 메시지 모니터링

> **진행 상태(서버 세션)** — §1 PTT 그룹 CRUD: **구현 완료**(csc 0.2.103, 계약 = [mcptt_api.md §2](../api/mcptt_api.md),
> 인가 = [mcptt_authorization.md §4.1](../design/features/mcptt_authorization.md)). 요청서와 다른 확정 사항 셋:
> ① 플래그명 `allow_create_group` / `<cims:allow-create-group>` / 프로비저닝 `ptt.allowCreateGroup`(TS 24.484 에 일반 그룹
> 생성 요소가 없어 CIMS 확장, 규격 동사형 관례) ② 그룹 uri 는 시스템 관례 **`tel:g-<8hex>`**(`sip:g-…@<PTT 도메인>` 도
> 수용, GROUPS 키·CSP·제휴가 전부 tel: 형) ③ 타인 소유 id 는 409 `uri_taken`, 소유자 없는 콘솔 그룹은 403 `not_group_owner`.
> §1.4 admin CRUD 캐시 동기화·API 문서 DELETE 도 완료. 부수 발견: HTTP 계층이 XML media type 을 415 로 거부하고 있었음(수정).
> §2 발견·§3 conference 인가(규격형: `<on-network-allow-conference-state>` + Warning 138)·§4 메시지(**이력 조회만**으로
> 결정, 통합 `GET /provisioning/history?kind=call|ptt|message`) 는 후속.
> **09-06 라이브 배포 완료(csc 0.2.103, 관제석 자격 부여, e2e PASS).** 단말 쪽 후속(재빌드·오류 문구·실기 e2e·번호 재키잉·다음 과제 대비) =
> [windows_request_dispatch_group_crud_followup.md](windows_request_dispatch_group_crud_followup.md).

작성 주체: Windows 관제조작반(`windows/dispatch-desktop`) + 단말 SDK(`sdk/`) 쪽 세션.
대상: 서버(CSC/CSP/CMP/OAM) 쪽 Claude Code 세션. 아래 계약대로 서버가 구현되면 단말 쪽은 같은 계약으로 SDK·앱을 붙인다.
원칙: CLAUDE.md 설계 우선순위(규격 → 체계성 → 최소보완 지양). 문서 현행화는 최종 상태만.

---

## 0. 현재 상태 요약 (조사 결과, 파일:줄 기준)

| 요구 | 서버 | SDK | 앱 | 막힌 층 |
|---|---|---|---|---|
| 관제사가 PTT 그룹 생성/편집/삭제 | 관리 API `/api/v1/ptt/groups` 는 콘솔 토큰(`CimsAuth.JwtSecret`+`role`) 전용 → PKCE 토큰 401. GMS XCAP `PUT/DELETE /org.openmobilealliance.groups/users/{xui}/{group}` 는 PKCE 로 200 이지만 **본문 무시·DB 미기록·재기동 시 소멸** (`csc/src/services/mcptt.py:1941-1971`) | 읽기(`listGroups`/`xcapGet`)만 (`sdk/core/include/cimsue/csc.h:81-84`) | [그룹] 탭에 버튼 없음 | **서버(권한 모델·GMS PUT 본체)** |
| 감청 동작 자체(VoLTE Join tap · PTT recv_only · SSRC 분리 · 은닉 · 감사) | 구현 | 구현 | 구현 | 없음 — 대상만 알면 즉시 동작 |
| 활성 PTT 그룹 전체 모니터링(비멤버 청취 범위 그룹) | `/provisioning/me` `dispatch` 블록이 범위 enum 만 주고 대상 그룹 목록을 안 줌 (`mcptt.py:2355-2363`). conference SUBSCRIBE 는 실제로 게이트 없음(문서 `dispatch_center.md:621` 과 불일치) | `subscribeConference` 있음 | 구독 대상을 열거할 수 없어 GMS 멤버 그룹만 구독 (`DispatchSession.cs:241-255`) | **서버(프로비저닝)** |
| 활성 VoLTE 통화 전체 모니터링(관제 그룹원 내선) | `members[]` 미제공 (`dispatch_desktop_ui.md:380`) | `dialogWatch` 있음 | 로컬 CSV `member` 태그에 의존 (`DispatchSession.cs:237`) | **서버(프로비저닝)** |
| 메시지(SDS/SMS) 모니터링 | **관제 축 설계 없음**. 그룹 SDS 는 CSP jsonl 보관 + OAM-svc `GET /api/v1/messages`(콘솔용). 1:1 SDS 는 보관 없음 (`mcdata_messaging.md:100-111, 265`) | 없음 | 자기 수신분만 | **서버(설계부터)** |

---

## 1. PTT 그룹 CRUD — 관제사(가입자) 주체, GMS XCAP 경로로 확정 요청

### 1.1 결정 근거
- TS 23.280 §10.2.5 / TS 24.481: 그룹 생성·수정·삭제 주체는 **authorized user(MC 가입자)** 이고 경로는 GMC→GMS **XCAP(Ut) PUT/DELETE**. `mcptt_authorization.md:10, 86-93` 도 같은 모델(authorized user = 생성자 = `ptt_groups.authorized_user_id` = `users.id`).
- 관제사는 콘솔 계정이 아니라 **PTT 가입자**(disp01 = users.id 있음) 이므로, `mcptt_authorization.md:152-155` §9 의 "콘솔 operator 는 users.id 가 없어 소유 판정이 항상 403" 문제가 GMS 경로에서는 발생하지 않는다.
- 관리 API(4421, 콘솔 토큰)는 콘솔용으로 그대로 두고, **PKCE 토큰을 관리 API 에 끼워 넣는 안은 채택하지 않는다**(토큰 realm 혼합, 체계성 위반).

### 1.2 인가
- `ptt_user_profile` 에 **`allow_group_creation TINYINT(1) DEFAULT 0`** 추가 (TS 24.484 사용자 프로파일 인가 플래그 관습 — `allow_ambient_listening` 과 같은 축. TS 24.484 §7.2.2 의 그룹 생성/regroup 인가 요소명에 맞춰 명명 확정 부탁). 마이그레이션 `sql/migrate_*.sql` + `cims_schema.sql`. CSP 는 이 컬럼을 읽지 않아도 됨(CSC 만).
- 콘솔 가입자 편집·admin API `/api/v1/users` 에서 편집 가능하게.
- GMS 게이트:
  - `PUT` 신규: 토큰 `mcptt_id` == XCAP 트리 xui(기존) **+ `allow_group_creation=1`** 아니면 403 `group_creation_not_allowed`.
  - `PUT` 기존 / `DELETE`: `ptt_groups.authorized_user_id == 토큰 sub(users.id)` 아니면 403 `not_group_owner`. (콘솔 admin/manager 가 만든 소유자 없는 그룹은 관제사가 편집 불가 — 의도.)
  - `adhoc-`/`priv-` 접두사 거부(관리 API 와 동일 규칙 `admin.py:1314-1316`).
- `/provisioning/me` 응답에 **`ptt.allowGroupCreation: bool`** 추가 — 앱이 [새 그룹] 버튼 노출 여부를 결정.

### 1.3 GMS 계약 (4430, `Authorization: Bearer <IdMS PKCE access token>`)
- `PUT /org.openmobilealliance.groups/users/{xui}/{group_uri}`
  - `Content-Type: application/vnd.oma.poc.groups+xml`, 본문 = **GET 이 돌려주는 그룹 문서와 동일 포맷**을 파싱 (현재 PUT 은 본문을 버림 — `mcptt.py:1945-1955`). 최소: `display-name`, `list/entry@uri`(멤버, 각 entry 에 CIMS 확장 속성으로 `role=chair|participant`, `priority`), 그룹 속성 확장 요소(group_type, floor_policy/max_talkers, allow_sds/allow_fd, emergency_*, priority). **정확한 XML 스키마는 서버가 확정하고 `docs/api/mcptt_api.md` §2 에 샘플 문서(PUT 본문·GET 응답)를 넣어 달라 — 단말은 그 샘플을 정본으로 직렬화/파싱 구현.**
  - 선택: `If-Match: "<etag>"` 지원(RFC 4825 조건부 — 편집 충돌 412).
  - 처리: `ptt_groups` INSERT/UPDATE(`authorized_user_id` = 토큰 sub), `ptt_group_members` 교체, **in-memory `GROUPS` 갱신**, `notify_csp("GROUP_CHANGED")`, 멤버에게 xcap-diff NOTIFY.
  - 응답: 201(신규)/200(갱신) + `ETag`. 409 = 같은 uri 가 타인 소유. 400 = 스키마 위반/접두사.
- `DELETE …/{group_uri}` → 200/204, 403/404. DB 삭제(CASCADE 멤버) + `GROUPS` 제거 + notify + xcap-diff.
- `GET /org.openmobilealliance.groups/users/{xui}` 목록 JSON 각 항목에 **`is_owner: bool`** 추가(기존 `uri, display_name, etag, member_count` 유지). 소유자면 멤버가 아니어도 목록에 포함.
- `GET …/{group_uri}` 문서 응답은 편집 폼 채우기용 — 멤버 uri/role/priority 와 속성이 모두 실려야 함.
- 그룹 uri 채번: 클라이언트가 `sip:g-<8hex>@<ptt domain>` 형태로 지정(XCAP 관습). 서버는 도메인·형식 검증. (서버 채번을 원하면 대안 제시 바람 — 단, XCAP 은 클라이언트 명명이 표준.)

### 1.4 부수 버그 (같이 수정 요청)
- 관리 API `_create_group`/`_update_group`/`_delete_group` 이 in-memory `GROUPS` 를 갱신하지 않아 **CSC 재기동 전까지 GMS 목록에 안 나온다** (`admin.py:1361-1387`, `refresh_group_members` `mcptt.py:577-586` 는 미지 그룹에 무효). CRUD 후 `GROUPS` 동기화 공통 함수로 통일.
- `docs/api/mcptt_api.md` §2 표에 DELETE 누락(코드엔 있음).

### 1.5 문서
- `mcptt_authorization.md` §3/§4 매트릭스에 "가입자(allow_group_creation) — GMS 경로 생성 + 본인 소유 CRUD" 행 추가, §9 항목은 "콘솔 operator 소유 판정" 으로 범위 축소 또는 종결.
- `csc.md` §4.2 GMS 에 PUT/DELETE 인가 규정 추가. `dispatch_center.md` 에 관제사 그룹 생성은 GMS 경로라는 한 줄.

---

## 2. 활성 세션 발견(discovery) — `/provisioning/me` `dispatch` 블록 확장

현재(`mcptt.py:2355-2363`): `dispatch{groupId, groupName, pilotId, monitorScope, pttListen, listenVisibility}`.

추가 요청:
```json
"dispatch": {
  ...기존...,
  "members": [ { "userId": 12, "name": "관제2석", "volteAor": "tel:+821310001002", "pttId": "sip:+82510001002@ptt.…", "extension": "1002" } ],
  "pttTargets": [ { "id": "g002", "uri": "sip:g002@ptt.…", "name": "음성그룹2" } ],
  "etag": "…"
}
```
- `members[]` = `dispatch_group_members` 전원(자기 포함) — dialog watch 대상 + ③ 그룹원 상태 띠 표시용. `monitor_scope=all` 이면 조직 전체 VoLTE 가입자로 확장(서버가 해석해 목록으로 내려줌 — 앱은 enum 을 해석하지 않는다).
- `pttTargets[]` = `ptt_listen=listed` → `dispatch_group_ptt_targets`; `all` → 서버가 해석한 전체 목록. `none` → `[]`.
- `etag` + `If-None-Match` 304 지원(앱이 주기 재조회·수동 새로고침).
- (선택, 장기) RFC 4662 RLS 목록 구독 — `dispatch_center.md:618`. 지금은 불필요.

---

## 3. 비멤버 그룹 conference 구독 인가 — 정식화

- 실측: `csp/CscfModule.cpp` SUBSCRIBE 경로에 conference 이벤트 게이트가 **없다**(403 지점은 dialog `:1179` 와 affiliation `:1316` 뿐). `dispatch_center.md:620-623` 은 "멤버 기준" 이라 적어 문서와 코드 불일치.
- 요청: conference SUBSCRIBE 인가 = **그룹 멤버 OR `CanListenPtt`(청취 범위 + `allow_ambient_listening`)**, 아니면 403. 청취 범위 구독자에게 나가는 roster NOTIFY 는 `listen_visibility=hidden` 규칙과 동일하게 청취 leg 를 숨긴 채 유지.
- `dispatch_center.md` §10 해당 항목을 구현 상태로, §7 표 `:454` "앱 파트 구현 전" 도 현행화(WPF 앱 구현·실측 완료).

---

## 4. 메시지 모니터링 — 설계 없음, 결정 필요 (제안)

규격 관점: TS 24.282 는 관제사의 타인 SDS 열람을 정의하지 않고, MC 서비스 LI 는 TS 33.127/33.128 의 LI_X2 이벤트 인도 영역이다. 따라서 **CIMS 확장 기능**으로 설계하되 VoLTE/PTT 감청과 같은 축(관제 그룹 범위 + 자격 + 감사 E-AUD-016)에 얹는 것을 제안.

제안 계약:
- 자격: `ptt_user_profile.allow_ambient_listening` 재사용 또는 별도 `allow_message_monitoring`. 범위: 관제 그룹 `ptt_listen`(그룹 SDS) + `monitor_scope`(그룹원 간 1:1 SDS·SMS).
- **실시간 사본**: CSP 가 범위 내 그룹 SDS / 그룹원 1:1 MESSAGE 를 관제사 AoR 로 SIP MESSAGE 사본 전달. 원본 헤더 보존 + CIMS 확장 헤더로 사본 표시(예 `X-CIMS-Monitor: group=<gid>;from=<aor>;to=<aor>`), 원 발신자·수신자에게는 은닉(disposition 미발생). 감사 `E-AUD-016 tap_mode=sds`.
- **이력 조회**: CSC 4430 에 `GET /provisioning/messages?group=&from=&to=&since=&limit=` (PKCE 토큰, 범위 밖 403) — 백엔드는 기존 CSP jsonl 보관(`mcdata_messaging.md:102-106`) 재사용. **1:1 SDS 보관을 켜야 함**(현재 미보관 `mcdata_messaging.md:265`) — 범위 대상만 보관하는 정책 결정 필요.
- 문서: `dispatch_center.md` 신규 §(메시지 모니터링), `mcdata_messaging.md` 보관 범위 갱신, `alarm_catalog.csv` 감사 tap_mode 값 추가.

→ 이 항목은 **실시간 사본 vs 이력 조회만** 중 어느 범위로 갈지 사용자 결정 후 진행.

---

## 5. 단말 쪽에서 맡는 것 (참고 — 서버 계약 확정 후 착수)
- SDK: `csc.h` 에 `GroupDetail`/`GroupMember` + `putGroup`/`deleteGroup`/`getGroup`, XML 직렬화, C API·.NET 파사드·ABI 테스트 등록.
- 앱: `DispatchSession.RefreshGroupsAsync()` 추출, [그룹] 탭 [새 그룹]/[편집]/[삭제] + `GroupEditWindow`, 생성 후 affiliation·conference 재적용, 삭제 시 해제, `SubscribeXcapDiff` 로 서버발 변경 자동 반영.
- 앱: `dispatch.members[]`/`pttTargets[]` 로 dialog watch·conference 구독 대상 전환(로컬 CSV `member` 태그 폐지), ② PTT 내역에 청취 범위 그룹 행 표시.
- 문서: `ue_sdk.md` §7 매핑 행, `dispatch_desktop_ui.md` §13 갱신.

---

## 6. 단말 세션 실측 발견 (2026-09-06, 라이브 .45 — cimsue-cli 로 CRUD 사이클 통과 후)

서버가 다음 단계(P2→P3→P3b)를 진행하면서 같이 봐 달라는 것. 우선순위 순.

1. **P3b 응답 계약 확정** — 앱 폴링 클라이언트 `HistoryClient` 뼈대가 들어갔다. 앱이 읽는 형태는
   [dispatch_desktop_ui.md §13](../design/features/dispatch_desktop_ui.md) "서버 통합 이력 조회" 항목(요청 `kind/since/limit` + `If-None-Match`,
   응답 `items[]{id,time,kind,event,from,to,group,duration,emergency,text}` + `next` + ETag). 그 형태로 가도 되는지, 다르면 확정안을 같은 절에 적어
   달라 — 앱은 `Parse` 와 event 이름표만 맞춘다. **구현 전에는 404 를 유지**해 달라(200 빈 목록이면 앱이 폴링을 계속 돈다). 범위 밖은 403.
2. **관제 그룹 `dg-dispatch01` 범위가 `monitor_scope=none`·`ptt_listen=none`** 이다(프로비저닝 실측). 감청·PTT 청취·이력 시험을 위해 콘솔
   `구성 > 관제 그룹`(manager)에서 `all` 로 올려 달라(설계 §14.1 의 "시험 단계에서 올린다" 시점).
3. **시험 그룹 편성이 설계 §14.1 과 다르다** — disp01/disp02 의 GMS 목록에 `tel:g003`(음성그룹3, 멤버 3, owner=false)만 있고 `g002` 는 없다.
   설계는 "g002 = 멤버 그룹, g003 = 청취 범위 비멤버 그룹"이었다. 편성을 문서대로 되돌리거나, 문서를 현재 편성으로 고쳐 달라(청취 범위 시험엔
   관제석이 멤버가 아닌 그룹이 하나 필요하다).
4. **P2 는 아직 안 내려온다**(`dispatch.members[]/ptt_targets[]` 빈 배열 — `cimsue-cli … login` 이 이제 두 배열을 그대로 찍으니 반영 확인에 쓰면 된다).
   `extension` 은 E.164 끝자리 4(1001/1002)로 부탁.
5. (문서) 본 요청서 §2 예시 JSON 의 `volteAor` 가 옛 번호 `tel:+82310001002` — 재키잉 값 `tel:+821310001002` 로.
6. (확인) GMS PUT 에 `role` 없이 멤버를 보내면 생성자(authorized user)까지 `participant` 로 저장된다. 앱은 자기 자신을 `chair` 로 명시해 보내니
   동작엔 문제없지만, role 생략 시 생성자 기본값을 chair 로 둘지는 서버 규칙으로 확정해 mcptt_api.md §2 에 한 줄 적어 달라.
7. **대표번호 dialog 이벤트가 포크 leg 마다 따로 나간다 — 설계 §4.5 와 다르다.** `CTasModule::NotifyPilotDialog`(`csp/TasModule.cpp:993`) 가
   `entity=<pilot>` NOTIFY 를 **그룹원 B-leg 마다** `id=<그 leg 의 Call-ID>` 로 보내므로, 대표번호 착신 한 건이 앱 대기열에 N 행(그룹원 수)으로
   뜨고 그중 하나가 관제사 자신의 착신 leg 다("대기열에 내게 온 전화가 뜬다" 현상의 근본 원인). [dispatch_center.md §4.5](../design/features/dispatch_center.md)
   는 "대표번호에 걸려온 **호**의 early/confirmed/terminated 와 응답자(remote)"라 적었고 RFC 4235 도 dialog 는 pilot 이 참여자인 dialog 단위다 —
   **포크 집합(`CTasForkSet`)당 dialog 하나**(`id` = A-leg Call-ID 또는 포크 집합 id 고정, early → confirmed 시 `remote` 는 발신자 유지 + 응답자는
   `<local>` 쪽 또는 CIMS 확장 요소로, terminated 1회)로 정리해 달라. 앱은 그때까지 발신자 기준으로 행을 병합해 방어한다.
8. **dialog 구독 초기 NOTIFY 가 빈 `state=full`** (`csp/CspServer.cpp:1049` — 진행 중 호를 역인덱싱하지 않음). 앱은 코어가 올리는 빈 자리표시자
   (`id`·`state` 빈 값)를 무시하도록 고쳤지만, RFC 4235 §3.2 대로 **구독 시점의 진행 중 dialog 를 full 스냅샷에 담아** 주면 관제석 재로그인·
   재구독 때 이미 울리고 있는 대표번호 호·통화 중인 그룹원이 즉시 보인다(지금은 다음 상태 변화까지 "대기"로 보인다).
