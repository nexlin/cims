# 권한 모델 — 역할·능력·범위 (콘솔 관리 권한 · 관제 권한 · PTT 그룹 소유)

본 문서는 CIMS 의 **권한 모델 정본**이다. 콘솔(운영·관리) 권한과 관제사 권한을 **하나의 역할 모델**로 정의하고,
PTT 그룹 소유(authorized user)와 `group.json` 디스크립터를 함께 다룬다. 전화 그룹·감청·청취의 절차는
[dispatch_center.md](dispatch_center.md), 접속서비스는 [sip_service_model.md](sip_service_model.md).

> **구현 상태**: 역할 모델(§2·§3) 구현 반영 — CSC `services/authz.py` `can()`(콘솔 JWT·가입자 principal 둘을 같은 `roles`
> 행으로 판정), `/api/v1/roles`(`handlers/dispatch.py`), 관제 앱 관리 API·이력·녹취 게이트가 역할을 읽고, CSP 는
> `CCspRoleMap`([dispatch_center.md §3.5](dispatch_center.md)). `require_role`(콘솔 4단 계층)은 인증 + 내장 역할 계층 다리로
> 남아 있고 능력·범위는 `can()` 이 판정한다. DB = `sql/migrate_phone_groups_roles.sql`(전환 마이그레이션 포함). 라이브 배포는
> 정지창에서 마이그레이션·csp·csc·oam-svc 동시 적용.

---

## 1. Context (왜)

- CIMS 의 신원은 **두 저장소**다: 콘솔 로그인 계정은 OAM file_store `console_accounts`(+ 패키지 내장 `admin`)에,
  가입자(person)는 DB `users`(+`*_subscriptions` = telephony)에 있다([csc_standalone_module.md](csc_standalone_module.md)
  도메인 경계). `users` 에는 role 컬럼이 없고 `login_id/passwd` 는 단말 IdMS 로그인 자격이지 콘솔 인증이 아니다.
  OAM 은 DB 없이 동작해야 하므로(부트스트랩·내장 admin) 두 저장소는 유지한다.
- 그러나 **권한은 하나**다. 조직·구성원·번호·PTT 그룹·전화 그룹은 콘솔 manager 가 콘솔에서 고치든 관제사가 관제 앱에서
  고치든 같은 자원에 대한 같은 동작이고, 같은 쓰기 코드(`handlers/admin.py`·`org.py`)를 지난다. 게이트가 둘이면 규칙이
  어긋난다(관리 범위가 있는 관제사가 청취 자격을 스스로 부여할 수 있었던 경로가 그 예).
- 3GPP 기준: 그룹 생성·관리 주체는 **authorized user**(MC 가입자, TS 23.280 / TS 24.481)이고, 관제사(dispatcher)는 별도
  도메인이 아니라 **추가 권한을 가진 MC 사용자**다(TS 22.179). 권한은 TS 24.484 user profile 의 개별 `allow-*` 자격으로
  표현된다 — 역할 이름이 아니라 개별 필드로 판정하는 모델의 원형. 콘솔 운영 권한은 규격 밖(CIMS 정의).
- 따라서: ① **역할 = 능력(capability) 집합 + 범위**, 콘솔 프리셋과 관제 프리셋이 같은 엔티티 ② principal 은 둘(콘솔
  계정·가입자)이지만 **판정은 CSC 한 곳** ③ 권한을 부여하는 능력(`authz.manage`)은 전역 manager 이상에만 있고 위임되지
  않는다 ④ 그룹 소유는 `authorized_user_id` 로 명시, `group.json` 은 자기완결형 디스크립터.

## 2. 모델

### 2.1 principal 둘, 역할 하나

| principal | 식별 | 저장 | 역할 배정 | 토큰 |
|---|---|---|---|---|
| `console` | `login_id` | OAM file_store `console_accounts` + 내장 `admin` | `console_accounts[].role` = `roles.id`(내장 4 또는 커스텀) | OAM JWT — `sub`=login_id, `role`=roles.id. CSC `admin_auth` 가 공유 시크릿으로 검증 |
| `user` | `users.id`(person) | DB `users` | `role_assignments(principal_type='user', principal_id, role_id)` — 사람당 하나 | IdMS access token(PKCE, `3gpp:mc:*`·provisioning scope) — `sub`→msisdn→`users.id` |

토큰 realm 은 섞지 않는다(콘솔 토큰을 관제 API 에, PKCE 토큰을 콘솔 API 에 끼워 넣지 않는다). 섞이는 것은 **정책**이다 —
두 토큰이 같은 `roles` 행으로 해석되고 같은 판정을 지난다.

### 2.2 역할 = 능력 + 범위

`roles` 한 행이 역할이다(DDL = [dispatch_center.md §8.1](dispatch_center.md)). 내장 프리셋 4행(`admin / manager /
operator / monitor`, `builtin=1`, 읽기 전용)은 마이그레이션이 항상 시드하고, 관제 프리셋(`감독 / 관리 / 전체`)은 콘솔이
역할 생성 시 초깃값으로 제공한다. 저장은 개별 필드다 — 프리셋 밖 조합도 막지 않는다.

| 필드 | 값 | 의미 |
|---|---|---|
| `authz_manage` | bool | 역할 생성·범위 변경·배정·해제, 청취 자격 동기. **내장 admin/manager 만** — 커스텀 역할에 켜면 400 `not_delegable` |
| `audit_read` | bool | `kind=audit` 이벤트(E-AUD) 열람 — 감청 감사([dispatch_center.md §5.7](dispatch_center.md)) |
| `directory_write` | none / own / all | 조직·구성원(person)·VoLTE/PTT 번호·프로파일 비감청 자격·전화 그룹 쓰기. `own` = `org_id` 조직과 그 하위 |
| `directory_read` | none / own / all | 같은 자원 조회(콘솔). 관제 앱의 전화번호부(`/provisioning/directory`)는 provisioning scope 의 일반 읽기라 별개 |
| `ptt_group_manage` | none / own / scope / all | PTT 그룹 CRUD — `own` = `authorized_user_id` == principal(가입자일 때만 성립, §9), `scope` = `directory_write` 범위 안 `org_code`, `all` |
| `monitor_call` | none / own / listed / all | 통화 감청(Join tap)·타인 세션 관측(dialog)·통화 이력/녹취 범위. `own` = 배정자의 전화 그룹, `listed` = `role_monitor_targets`(전화 그룹 id) |
| `ptt_listen` | none / listed / all | PTT 그룹콜 청취·conference 구독·PTT 이력/녹취 범위. `listed` = `role_ptt_targets`(ptt_groups.id) |
| `listen_visibility` | hidden / visible | PTT 청취 멤버의 로스터 노출 |
| `history_read` | none / scope / all | 이력·녹취 열람. `scope` = `monitor_call`/`ptt_listen` 범위, `all` = 전역(콘솔) |
| `alarm_ack` | bool | 알람 ack |
| `mcptt_control` | bool | MCPTT 관제(floor·긴급 조작, 콘솔) |
| `org_id` | FK | `own` 범위의 루트 |

인프라·릴리스·배포·검증·계정 관리(OAM 소유 API)는 이 모델 밖이다 — OAM 이 DB 없이 판정해야 하므로 OAM 로컬의 `role` 계층
(admin 만)으로 남는다. 콘솔 계정에 커스텀 역할 id(`role-…`)가 배정되면 OAM 로컬 게이트와 콘솔 메뉴 노출은 그 계정을
**monitor 등급(읽기 전용)** 으로 보고(`services/admin_auth.role_rank`·콘솔 `permissions.roleRank`), 실제 능력은 CSC `can()`
이 `roles` 행으로 판정한다 — OAM 은 `roles` 행을 읽지 않는다.

### 2.3 단일 판정 — `can(principal, capability, target)`

CSC `services/authz.py` 하나가 판정한다.
1. **principal 해석** — 콘솔 JWT 면 `(console, sub)` 와 `role` 클레임 → `roles` 행; IdMS 토큰이면 `sub`→msisdn→`users.id` →
   `role_assignments` → `roles` 행. 배정이 없으면 능력 전부 `none`.
2. **능력 확인** — capability 이름 → 필드. bool 은 참/거짓, enum 은 `none` 이 아니어야 한다.
3. **범위 확인** — `target` 의 종류에 따라: 조직 코드(`own` = `org_id` 하위 집합) / 전화 그룹 id(`own` = 배정자의 전화 그룹,
   `listed` = 대상 집합) / PTT 그룹(`scope` = 그룹 `org_code` 가 조직 범위 안, `own` = 소유) / 없음(전역 능력).
4. 결과 403 본문 = `{"error": "forbidden", "capability": "…", "scope": "…"}` — 관제 앱 문구 사전이 읽는 토큰.

이 판정이 콘솔 관리 API(`/api/v1/*`, `require_role` 8곳)와 관제 앱 관리 API(`/provisioning/directory/*`,
`admin_scope/in_scope`)·이력/녹취 게이트(`services/dispatch_history.py`·`handlers/dispatch_recordings.py`)를 모두 대체한다.
CSP 는 SIP 경로에서 같은 역할 행을 인메모리로 든다(`CCspRoleMap` — 회선 → person → 역할,
[dispatch_center.md §3.5](dispatch_center.md)) — CSC 가 목록을 만들 때와 CSP 가 게이트를 걸 때의 규칙은 같아야 한다.

### 2.4 불변 규칙 — 권한 분리

- **`authz_manage` 는 위임되지 않는다.** 내장 `admin`·`manager` 에만 있고, 커스텀(한정 범위) 역할에는 켤 수 없다. 관제 앱에는
  역할·배정 API 가 없다. 그러므로 관리 범위(`directory_write`)가 있는 관제사도 감청·청취 권한을 만들거나 넓힐 수 없다.
- **청취 자격은 배정의 결과다.** `ptt_user_profile.allow_ambient_listening`(TS 24.484)은 `ptt_listen≠none` 역할에 배정될 때
  CSC 가 켜고 해제 시 끈다. 관제 앱 `PUT …/ptt/profile` 은 이 값의 변경을 400 `not_editable` 로 거절한다(현재값과 같은 값은
  무시 — 구 앱 호환; **구현 반영**, `dispatch_directory.py` `_LOCKED_PROFILE_KEYS`). 콘솔 프로파일 편집(manager)은 역할 모델
  도입 시 표시만으로 바뀐다. CSP 는 규격 자리(프로파일)에서 자격을 읽는다.
- **수행과 감사 열람은 분리한다.** 감청을 수행하는 능력(`monitor_call`·`ptt_listen`)과 감사를 읽는 능력(`audit_read`)은 같은
  역할에 함께 두지 않는다(관제 프리셋에는 `audit_read` 가 없다).
- **배정·범위 변경은 감사된다.** `E-AUD-006 config_change`(entity=`role` | `role_assignment`, actor=§2.5).

### 2.5 감사 actor

모든 쓰기의 actor 는 principal 표기로 통일한다 — `console:<login_id>` / `user:<users.id>`. 어느 UI(콘솔·관제 앱)로 했든
같은 E-AUD-006 행이며, 감청 감사 E-AUD-016 의 `monitor`(관제사 회선)·`role`(역할 id)도 같은 사람을 가리킨다.

## 3. 능력 어휘와 프리셋

| 능력 | 대상 · 범위 단위 | admin | manager | operator | monitor | 관제 · 감독 | 관제 · 관리 | 관제 · 전체 |
|---|---|---|---|---|---|---|---|---|
| 인프라·릴리스·배포·검증·계정(OAM 로컬) | OAM | ● | – | – | – | – | – | – |
| `authz_manage` | 역할·배정·범위 | ● | ● | – | – | – | – | – |
| `audit_read` | E-AUD 이벤트 | ● | ● | – | – | – | – | – |
| `directory_write` | 조직 범위 | all | all | – | – | – | own \| all | own \| all |
| `directory_read` | 조직 범위 | all | all | all | all | – | – | – |
| `ptt_group_manage` | own / scope / all | all | all | own(§9) | – | – | scope | scope |
| `monitor_call` | 전화 그룹 | – | – | – | – | own \| listed \| all | – | own \| listed \| all |
| `ptt_listen` · `listen_visibility` | PTT 그룹 | – | – | – | – | listed \| all | – | listed \| all |
| `history_read` | 범위 | all | all | all | all(R) | scope | – | scope |
| `alarm_ack` · `mcptt_control` | 운용 대응 | ● | ● | ● | – | – | – | – |
| **OAM 로그인** | | O | O | O | O | X | X | X |
| telephony | | (구독 시) | (구독 시) | (구독 시) | (구독 시) | O | O | O |

- 가입자(person)는 콘솔 계정이 아니므로 **OAM 로그인 불가** — 역할이 있어도 관제 앱(PKCE)에서만 쓴다.
- 전화 그룹 멤버십은 권한이 아니다 — 대표번호 착신·그룹/지정 픽업·그룹원 BLF·대표번호 발신 표시는 멤버십만으로 성립한다
  ([dispatch_center.md §3.1](dispatch_center.md)). 역할이 없는 유선 사용자도 전화 그룹원일 수 있다.
- 가입자의 **그룹 conference 이벤트 구독**(RFC 4575)은 역할이 아니라 그룹 문서 축이다 — 멤버는
  `ptt_groups.allow_conference_state`(`<on-network-allow-conference-state>`), 비멤버 관제사는 청취 2단 인가(자격
  `allow_ambient_listening` + 역할 `ptt_listen`). CSP 가 초기 SUBSCRIBE 에서 판정(TS 24.379 §10.1.3.4.1, 403 `Warning: 138`) —
  [dispatch_center.md §5.6](dispatch_center.md).
- `monitor` 는 ack 불가, ack 는 `operator` 이상.

### 3.1 패키지 내장 계정 + 개발자 모드

`admin` 은 **공급사 구축 계정**으로, 고객측 관리자/운용자(manager/operator/monitor —
OAM file_store `console_accounts`, 콘솔 `관리 > 계정`)와 분리한다. 가입자 DB 에 저장하지 않고 **OAM 패키지
설정에 내장**:

```json
"CimsAuth": {
  "BuiltinAccounts": [
    { "login_id": "admin", "name": "관리자", "role": "admin", "password_sha256": "<sha256hex>" }
  ]
}
```

**개발자 모드**: 개발 기능(빌드·모듈 검증·패키징·배포 검증 = 릴리스 메뉴)은
별도 developer 계정이 아니라 **admin 로그인 후 콘솔 헤더의 `</>` 토글**로
노출한다 (라우트 `devOnly` 플래그, localStorage 영속, 권한 분리가 아닌 화면
모드 분리 — 직접 URL 진입 시에도 안내 화면 + 켜기 버튼).

- **근거 (부트스트랩)**: 상용 구축은 base OAM 만 수동 배포 → admin 로그인 → 인프라
  구축 → 전 모듈 배포 순서. 이 시점에 DB 가 없으므로 로그인이 DB 에 의존하면 불가.
  내장 계정 로그인/`users/me` 는 **DB 를 일절 접근하지 않는다**.
- 같은 login_id 의 `console_accounts` 계정보다 내장 계정이 **항상 우선** (DB 는 보지 않는다).
- 미설정 시 코드 기본값(admin, 비밀번호 `1234`) 적용 — **상용 패키징 시
  password_sha256 교체 필수**. `BuiltinAccounts: []` 로 전체 비활성화 가능.
- 내장 계정 id 는 음수(-1000부터) — DB FK 로 사용 금지. 비밀번호 변경은 콘솔이 아닌
  설정 파일에서만 (PUT /auth/password → 403).
- 구현: `ems/core/oam/src/handlers/auth.py` `_builtin_accounts`/`_login`, `users.py` `_get_me`
  (builtin 클레임 합성). 개발자 모드: `utils/devMode.ts` + 라우트 `devOnly`.

## 4. 그룹 소유 — `authorized_user_id` (단일 필드)

- 3GPP 용어 **authorized user** = 그룹 생성자 = 관리 주체. 생성자가 곧 administrator (별도 `administrator`/`created_by` 컬럼 **두지 않음** — 중복).
- `ptt_groups.authorized_user_id BIGINT` = 생성한 `users.id`. (+ `created_at`)
- **읽을 때 파생**:
  - 편집 권한 스코프: `ptt_group_manage=own` 은 principal 이 `user` 이고 `users.id == group.authorized_user_id` 일 때 성립
  - 규격 표기(GMS/group.json) `authorized_user` = 그 user의 PTT MSISDN (`ptt_subscriptions.id WHERE user_id=authorized_user_id`)
  - 표시명 = `users.name/login_id`
- **편집 인가 규칙** = `can(principal, ptt_group.manage, group)`: 가입자 principal 이 **소유자**(`authorized_user_id`)면 역할 값과
  무관하게 허용(소유는 능력이 아니라 규격의 authorized user 권리 — `authz._owns_ptt_group`) / `all` → 모든 그룹 / `scope` → 그룹
  `org_code` 가 `directory_write` 범위 안 / `own` → 소유만. 생성 시 `authorized_user_id` = 생성자(가입자)이며, 콘솔 계정이 생성하면 owner 를 PTT 가입자 중 지정.
- 제약: authorized user 는 **PTT 가입자여야** 함(규격). 소유권은 관리 범위 편집으로 바뀌지 않는다.

### 4.1 가입자(관제사) 주체의 그룹 CRUD — GMS XCAP 경로

규격(TS 23.280 §10.2.5, TS 24.481)의 그룹 생성·수정·삭제 주체는 **authorized user(MC 가입자)** 이고 경로는
GMC→GMS **XCAP Ut PUT/DELETE** 다. 관제사는 콘솔 계정이 아니라 PTT 가입자(`users.id` 있음)이므로 이 경로에서 소유 판정이
성립한다. 관리 API(4421, 콘솔 토큰)는 콘솔 전용으로 두고 **PKCE 토큰을 관리 API 에 끼워 넣지 않는다**(토큰 realm 혼합 금지).

| 동작 | 인가 | 근거 |
|---|---|---|
| 생성 (PUT 신규 uri) | 프로파일 `ptt_user_profile.allow_create_group=1` **또는** 역할 `ptt_group_manage=scope\|all` | CIMS 확장 요소 `<cims:allow-create-group>` — TS 24.484 에는 일반 그룹 생성 요소가 없다(`allow-regroup` 은 임시 regroup, `allow-create-{group,user}-broadcast-group` 은 브로드캐스트 한정). 규격상 이 인가는 GMS 측 정책이라 프로파일 확장 자리(`anyExt` 계열, 기존 `cims:allow-adhoc-group-call` 과 같은 관례)에 둔다 |
| 수정·삭제 (PUT 기존 / DELETE) | `ptt_groups.authorized_user_id == 토큰 가입자 users.id` **또는** 역할 `ptt_group_manage=scope`(그룹 `org_code` 가 범위 안)\|`all` | §4 소유 규칙 + 관리 범위. 소유권(`authorized_user_id`)은 바뀌지 않는다 |

- 관리 범위 편집은 같은 범위로 조직·구성원·VoLTE/PTT 번호·전화 그룹·PTT 프로파일의 비감청 자격(`allow_create_group`·긴급 계열)도
  관제 앱에서 다룬다(`/provisioning/directory/*`, [dispatch_center.md §3.4](dispatch_center.md)). `allow_ambient_listening` 은 §2.4.
- `allow_create_group` 의 부여는 **OAM/관리 범위**가 한다(TS 23.280 authorized user = 조직 프로비저닝): 콘솔 가입자 편집의 PTT
  프로파일 토글, admin API `PUT /api/v1/users/{id}/ptt/{msisdn}/profile`, 관제 앱 관리 화면 — 셋 다 같은 플래그(인가 축은 하나).
- 단말에는 프로비저닝 `/provisioning/me` 의 ptt 서비스 `allowCreateGroup` 으로 노출([새 그룹] 표시 여부),
  수정·삭제 가능 여부는 GMS 목록의 `is_owner`/`canManage`. 계약 상세 = [mcptt_api.md §2](../../api/mcptt_api.md).
- 정본은 DB(`ptt_groups`·`ptt_group_members`)이고 관리 API·GMS 두 쓰기 경로가 같은 캐시 동기화
  (`sync_group_from_db`)와 CSP `GROUP_CHANGED` 통지를 공유한다.

## 5. `group.json` (자기완결형 디스크립터, CSP 기록)

```json
{
  "id": 1,                              // ptt_groups.id (surrogate, = 디렉터리 키)
  "mcptt_group_id": "g001",
  "name": "음성그룹1",
  "alias": null,
  "group_type": "prearranged",
  "priority": 5, "encryption": false, "emergency_call": false,
  "video_enabled": false, "on_network": true,
  "max_members": 0, "require_affiliation": true, "org_code": "",
  "authorized_user_id": 27,
  "authorized_user": "tel:+82500000027",   // 파생 MCPTT ID (규격 administrator)
  "created_at": "2026-06-02T20:42:01",
  "updated_at": "2026-06-02T20:42:01",
  "state": "active",
  "member_count": 40,
  "members": [ { "user_id": "+82500000001", "priority": 0, "role": "participant", "mcptt_id": null }, ... ]
}
```

## 6. 구현 범위 / 파일

### 저장소
- 콘솔 계정 = OAM file_store `console_accounts`(`ems/core/oam/src/handlers/console_accounts.py`, `role` = roles.id) + 내장 `admin`.
  DB `users` 에는 role 이 없다(`sql/migrate_users_person_only.sql`).
- 역할 = DB `roles`·`role_assignments`·`role_monitor_targets`·`role_ptt_targets`(`sql/cims_schema.sql` + 전환
  `sql/migrate_phone_groups_roles.sql`, DDL [dispatch_center.md §8.1](dispatch_center.md)). 내장 4행 시드.
- `ptt_groups.authorized_user_id BIGINT NULL` (FK→users.id, ON DELETE SET NULL) + `created_at` — `sql/cims_schema.sql`.

### 인가 (CSC/OAM)
- `ems/core/oam/src/handlers/auth.py` — 로그인(내장 → `console_accounts` 순), JWT 에 `role`(roles.id)·`sub`(login_id) 포함.
  `csc/src/services/admin_auth.py` 가 같은 시크릿으로 검증한다.
- `csc/src/services/authz.py` `can(principal, capability, target)`(§2.3) — 콘솔 관리 API 는 `require_role`(인증·내장 계층) 뒤에
  `can()` 으로 능력·범위를 판정하고, `dispatch_directory.admin_scope/in_scope`·`dispatch_history`/`dispatch_recordings` 범위
  게이트가 역할을 읽는다. 역할·배정 API = `handlers/dispatch.py` `/api/v1/roles`(`authz.manage`), 배정 시
  `allow_ambient_listening` 동기 + `ROLE_CHANGED` 통지. roles 테이블 미적용 DB 는 내장 4행만으로 판정(종전 계층과 같다).
- `csc/src/handlers/admin.py` `_create_group`: `authorized_user_id` 입력/기본=생성자, PTT 가입자 검증. `_list_groups`/`_get_group`: `authorized_user`(파생 MCPTT ID) 포함.
- `csc/src/services/mcptt.py` GMS: `<list-service>`/ruleset 에 authorized user 반영, PUT/DELETE 인가 §4.1.

### CSP
- `csp/GroupCallService.cpp` `BuildGroupDescriptor(clsGroup)` — §5 전체 필드 생성(멤버 role 포함, authorized_user_id).
- `csp/CallDir.h` `PttSessionStart` — 디스크립터 + `state/created_at/updated_at` 기록. initiator 는 state 파일용으로만 유지.
- `csp/CspPttGroup.{h,cpp}` + `csp/DbManager.cpp` `SelectGroup` — `authorized_user_id` 로드.
- `CCspRoleMap`(`csp/CspRole.{h,cpp}`, 회선 → 역할) — [dispatch_center.md §3.5](dispatch_center.md).

### 콘솔
- `관리 > 계정`: role 지정 = `roles` 목록에서 선택(내장 4 + 커스텀), 기본 default-deny.
- `시스템 > 역할`(`/deploy/roles`, `RolesPage`, manager): 내장 프리셋 읽기 전용, 관제 프리셋(감독/관리/전체)으로 생성, 범위·대상
  (전화 그룹·PTT 그룹)·`org_id`, 가입자 배정(`PUT /api/v1/roles/{id}/assignments`)과 콘솔 계정 배정(`PUT /api/v1/console-accounts/
  {login_id}` `role`, admin) 한 화면. 계정 화면의 역할 선택은 `GET /api/v1/roles`(내장 4 + 커스텀).
- 메뉴/버튼 게이팅: 능력별 노출/비활성. `monitor`=쓰기 숨김.
- PTT 그룹 페이지: `ptt_group_manage` 범위 밖 그룹은 읽기(잠금). 그룹 생성 시 authorized user 지정.
- group.json 표시: `authorized_user`/role 노출.

## 7. 검증
- 로그인: 가입자 콘솔 로그인 거부, `monitor` 쓰기 API 403, `operator` 타인 그룹 edit 403·생성 OK.
- 역할: 커스텀 역할에 `authz_manage=1` → 400 `not_delegable`; 관제 앱 `PUT …/ptt/profile` 에 `allowAmbientListening` → 400
  `not_editable`; `ptt_listen≠none` 배정 → 대상 person `allow_ambient_listening=1`, 해제 → 0; 관리 범위 역할이 `/api/v1/roles` → 403.
- 게이트 동치: 콘솔 manager 와 관제 관리(all) 가 같은 조직에 같은 쓰기 → 같은 결과·같은 감사 행(actor 만 다름).
- S3: `S3-SCN-MONITOR` M5 / `S3-SCN-PTT-LISTEN` L2·L3 — "전화 그룹원이지만 역할 없음 → 403"([dispatch_center.md §9](dispatch_center.md)).
- group.json: 신 디스크립터(authorized_user 파생) 생성. GMS XML 반영.

## 8. 직교 개념 (재확인)
- 역할(이 문서) = 콘솔 운영·관리 권한 + 관제 권한.
- 전화 그룹 멤버십([dispatch_center.md §3.1](dispatch_center.md)) = 유선 전화 기능(대표번호·픽업·BLF). 권한이 아니다.
- `ptt_group_members.role` (chair/participant) = **통화 중 floor 권한** (TS 24.380) — 별개. 한 가입자가 관제 감독(권한) + 어떤 그룹의
  `chair`(발언통제)일 수 있음.
- TS 24.484 프로파일 `allow-*` = 가입자 개인 자격. `allow_ambient_listening` 만 역할 배정에 종속(§2.4), 나머지는 관리 범위 편집 대상.

## 9. 미결/후속
- **신원 통합(후속)**: 콘솔 `manager/operator/monitor` 계정을 IdMS 사용자(`users`, 전화 가입 없는 person)로 옮기고 콘솔 로그인을
  IdMS OAuth(PKCE, 콘솔 scope)로 바꾼다 — 운영자이자 관제사인 사람이 계정 하나를 갖고, TS 33.180 IdMS 가 단일 IdP 가 된다.
  **내장 admin 은 OAM 로컬로 남긴다**(부트스트랩·break-glass). 조건: 콘솔 로그인이 CSC 가용성에 의존 → IdMS HA. 순서는 역할 모델
  통합(§2) 뒤.
- **콘솔 operator 의 그룹 소유(`ptt_group_manage=own`)**: 콘솔 계정에는 `users.id` 가 없어 소유 판정이 성립하지 않는다(현행과
  같다). 위 신원 통합에서 자연히 해소되며, 그 전에는 operator 에게 `scope` 를 쓴다.
- **자리(관제석)와 사람의 분리 — 보류**([dispatch_center.md §10](dispatch_center.md)).
- 사람당 역할 여러 개(합성)가 필요해지면 `role_assignments` PK 를 풀고 능력은 OR·범위는 합집합으로 판정한다(현재는 하나 + "전체"
  프리셋으로 충분).
