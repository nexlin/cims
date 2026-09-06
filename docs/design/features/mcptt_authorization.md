# MCPTT 권한 모델 & 그룹 소유(authorized user) 설계

본 문서는 CIMS 의 콘솔/운영 권한 모델(역할)과 PTT 그룹 소유(authorized user) 모델을 정의한다.

---

## 1. Context (왜)

- `group.json` 은 그룹을 식별·관리할 핵심 정보(소유자/관리자, 식별자, 유형)를 담는 자기완결형 디스크립터다.
- 3GPP MCPTT(TS 23.280 / 24.481)에서 그룹 생성·관리 주체는 **authorized user**(MCPTT 사용자)이다. 별도의 "운영자 계정"이 아니라 **권한을 가진 가입자**다.
- CIMS 의 신원은 **두 저장소**로 나뉜다: 콘솔 로그인 계정은 OAM file_store `console_accounts`(+ 패키지 내장 `admin`)에, 가입자(person)는 DB `users`(+`*_subscriptions` = telephony)에 있다([csc_standalone_module.md](csc_standalone_module.md) 도메인 경계). `users` 에는 role 컬럼이 없고(`sql/migrate_users_person_only.sql`) `login_id/passwd` 는 단말 IdMS 로그인 자격이지 콘솔 인증이 아니다. **역할(role)은 콘솔 계정의 속성**이며 가입자에게는 역할이 없다.
- 따라서: ① 콘솔/운영 권한을 **역할(role)** 로 정리, ② 그룹 소유를 **authorized_user_id** 로 명시, ③ `group.json` 을 자기완결형 디스크립터로 재설계.

## 2. 신원·권한 두 축 (혼동 방지)

| 축 | 무엇 | 누구 | 저장 |
|---|---|---|---|
| **A. 서비스 이용 권한 (telephony)** | VoLTE/PTT 가입, 영상·긴급·우선순위 등 | 모든 단말 사용자 | `*_subscriptions` + service_ref (+향후 feature 플래그) |
| **B. 관리 권한 (role)** | 콘솔/운영/그룹관리 | 운영자/관리자 (콘솔 계정) | OAM file_store `console_accounts[].role` + 내장 `admin` |

- 일반 단말 사용자 = A축만(가입자, 콘솔 계정 없음 → 콘솔 로그인 불가).
- 두 축은 저장소부터 분리된다 — 콘솔 계정과 가입자를 잇는 키는 없다(운용자가 관제석 단말도 쓰면 콘솔 계정과 가입자를 따로 가진다). 그래서 "가입자의 역할" 을 전제로 한 게이트는 두지 않는다(예: 관제 그룹 편입 — [dispatch_center.md](dispatch_center.md) §5.3).

## 3. 역할 모델 (콘솔 계정 `role`, 계층적 4종 + 가입자)

`console_accounts[].role ∈ {admin, manager, operator, monitor}` — 계층적. 가입자(person)는 콘솔 계정이
아니므로 아래 표의 `user` 행은 "계정 없음" 을 뜻한다.

| role | 한글 | 요약 |
|---|---|---|
| `admin` | 관리자 | **전체** — 시스템·인프라·릴리스·배포·검증 + 가입자·그룹·조직 + 모니터링 + 계정/권한 |
| `manager` | 운영 관리자 | **구성 CRUD 전체** (가입자·조직·PTT그룹) + 모니터링/장애. **인프라/배포/검증/계정 제외** |
| `operator` | 운용자(관제) | 구성 **조회만** + 운용 대응(알람 ack, MCPTT 관제) + **PTT그룹 생성 / 본인 소유 그룹만 관리** |
| `monitor` | 모니터 | **조회 전용** (대시보드/성능/이력/녹취 보기, ack 불가) |
| (`user`) | 일반 단말 사용자 = 가입자 | 콘솔 계정 없음 — **OAM 로그인 불가**, telephony만 |

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

### 권한 매트릭스
| 도메인 | admin | manager | operator | monitor | user |
|---|---|---|---|---|---|
| 가입자/조직 | CRUD | CRUD | R | R | – |
| PTT 그룹 | CRUD(all) | CRUD(all) | **생성 + 본인소유 CRUD / 타인 R** | R | **GMS XCAP: 생성(`allow_create_group`) + 본인소유 CRUD** (§4.1) |
| 모니터링/성능 | ● | ● | ● | R | – |
| 알람 ack | ● | ● | ● | – | – |
| MCPTT 관제(floor/긴급) | ● | ● | ● | – | – |
| 통화이력/녹취 | R·재생 | R·재생 | R·재생 | R | – |
| 검증 S1~S6 | ● | – | – | – | – |
| 시스템/인프라(HA·agent) | ● | – | – | – | – |
| 릴리스/배포/패키지 | ● | – | – | – | – |
| 계정/권한 관리 | ● | – | – | – | – |
| **OAM 로그인** | O | O | O | O | **X** |
| telephony | (구독 시) | (구독 시) | (구독 시) | (구독 시) | **O** |

- 계층적이라 게이팅은 "필요 등급 이상" + (PTT그룹은) "소유 스코프" 추가 체크.
- `monitor` ack 불가, ack는 `operator` 이상 (논의 확정).

## 4. 그룹 소유 — `authorized_user_id` (단일 필드)

- 3GPP 용어 **authorized user** = 그룹 생성자 = 관리 주체. 생성자가 곧 administrator (별도 `administrator`/`created_by` 컬럼 **두지 않음** — 중복).
- `ptt_groups.authorized_user_id BIGINT` = 생성한 `users.id`. (+ `created_at`)
- **읽을 때 파생**:
  - 편집 권한 스코프: 세션 `user_id == group.authorized_user_id` (operator 본인 그룹 판정)
  - 규격 표기(GMS/group.json) `authorized_user` = 그 user의 PTT MSISDN (`ptt_subscriptions.id WHERE user_id=authorized_user_id`)
  - 표시명 = `users.name/login_id`
- **편집 인가 규칙**: `admin`·`manager` → 모든 그룹 / `operator` → `authorized_user_id == 본인` 만 / 생성 시 `authorized_user_id=본인` 자동.
- 제약: authorized user 는 **PTT 가입자여야** 함(규격). admin/manager 대리 생성 시 owner를 PTT 가입자 중 지정.

### 4.1 가입자(관제사) 주체의 그룹 CRUD — GMS XCAP 경로

규격(TS 23.280 §10.2.5, TS 24.481)의 그룹 생성·수정·삭제 주체는 **authorized user(MC 가입자)** 이고 경로는
GMC→GMS **XCAP Ut PUT/DELETE** 다. 관제사는 콘솔 계정이 아니라 PTT 가입자(`users.id` 있음)이므로 이 경로에서는
§9 의 "콘솔 operator 소유 판정" 문제가 없다. 관리 API(4421, 콘솔 토큰)는 콘솔 전용으로 두고 **PKCE 토큰을
관리 API 에 끼워 넣지 않는다**(토큰 realm 혼합 금지).

| 동작 | 인가 | 근거 |
|---|---|---|
| 생성 (PUT 신규 uri) | 프로파일 `ptt_user_profile.allow_create_group=1` | CIMS 확장 요소 `<cims:allow-create-group>` — TS 24.484 에는 일반 그룹 생성 요소가 없다(`allow-regroup` 은 임시 regroup, `allow-create-{group,user}-broadcast-group` 은 브로드캐스트 한정). 규격상 이 인가는 GMS 측 정책이라 프로파일 확장 자리(`anyExt` 계열, 기존 `cims:allow-adhoc-group-call` 과 같은 관례)에 둔다 |
| 수정·삭제 (PUT 기존 / DELETE) | `ptt_groups.authorized_user_id == 토큰 가입자 users.id` | §4 소유 규칙 그대로. 콘솔 admin/manager 가 만든 소유자 없는 그룹은 관제사가 편집 불가(의도) |

- 부여는 **OAM** 이 한다(TS 23.280 authorized user = 조직 프로비저닝): 콘솔 가입자 편집의 PTT 프로파일 토글,
  admin API `PUT /api/v1/users/{id}/ptt/{msisdn}/profile` 의 `allow_create_group`, 관제 그룹 멤버 화면의 일괄 부여 —
  셋 다 같은 플래그(인가 축은 하나). `allow_ambient_listening` 과 같은 결.
- 단말에는 프로비저닝 `/provisioning/me` 의 ptt 서비스 `allowCreateGroup` 으로 노출([새 그룹] 표시 여부),
  수정·삭제 가능 여부는 GMS 목록의 `is_owner`. 계약 상세 = [mcptt_api.md §2](../../api/mcptt_api.md).
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
- 콘솔 계정 = OAM file_store `console_accounts`(`ems/core/oam/src/handlers/console_accounts.py`, role 포함) + 내장 `admin`.
  DB `users` 에는 role 이 없다(`sql/migrate_users_person_only.sql`).
- `ptt_groups.authorized_user_id BIGINT NULL` (FK→users.id, ON DELETE SET NULL) + `created_at DATETIME DEFAULT CURRENT_TIMESTAMP` — `sql/cims_schema.sql`.

### 인가 (CSC/OAM)
- `ems/core/oam/src/handlers/auth.py` — 로그인(내장 → `console_accounts` 순), JWT 에 `role`·`sub`(콘솔 계정은 login_id) 포함.
  `csc/src/services/admin_auth.py` 가 같은 시크릿으로 검증하고 등급 게이트(`require_role`)를 건다.
- 인가 미들웨어/데코레이터: 핸들러별 **요구 등급** 선언 + (그룹은) 소유 스코프 체크. `csc/src/handlers/admin.py`(그룹 CRUD: operator 생성 허용 + edit/delete `authorized_user_id==self` 게이트), org/users 핸들러 등급 게이팅.
- `csc/src/handlers/admin.py` `_create_group`: `authorized_user_id` 입력/기본=생성자, PTT 가입자 검증. `_list_groups`/`_get_group`: `authorized_user`(파생 MCPTT ID) 포함.
- `csc/src/services/mcptt.py` GMS: `<list-service>`/ruleset 에 authorized user 반영.

### CSP (group.json/GMS)
- `csp/GroupCallService.cpp` `BuildGroupDescriptor(clsGroup)` — §5 전체 필드 생성(멤버 role 포함, authorized_user_id).
- `csp/CallDir.h` `PttSessionStart` — 디스크립터 + `state/created_at/updated_at` 기록. initiator 는 state 파일용으로만 유지.
- `csp/CspPttGroup.{h,cpp}` + `csp/DbManager.cpp` `SelectGroup` — `authorized_user_id` 로드.

### 콘솔
- 계정 관리 페이지: role 지정(5종) UI + 기본 default-deny.
- 메뉴/버튼 게이팅: role 등급별 노출/비활성. `monitor`=쓰기 숨김.
- PTT 그룹 페이지: operator 에게 **본인 그룹=편집 / 타인 그룹=읽기(잠금)**. 그룹 생성 시 authorized user 지정.
- group.json 표시: `authorized_user`/role 노출.

## 7. 검증
- DB: enum/컬럼 확인, 기존 admin→admin·가입자→user.
- 로그인: `user` 거부, `monitor` 쓰기 API 403, `operator` 타인 그룹 edit 403·본인 그룹 OK·생성 OK.
- group.json: 신 디스크립터(authorized_user 파생) 생성. GMS XML 반영.
- 콘솔: role별 메뉴/버튼, 그룹 소유 잠금 동작.

## 8. 직교 개념 (재확인)
- 콘솔 계정 `role` (이 문서) = 콘솔/운영/그룹관리 권한.
- `ptt_group_members.role` (chair/participant) = **통화 중 floor 권한** (TS 24.380) — 별개. 한 가입자가 `operator`(관리) + 어떤 그룹의 `chair`(발언통제)일 수 있음.

## 9. 미결/후속
- **콘솔 operator** 계정에는 `users.id` 가 없어(§2) 본인 소유 그룹 판정(`sub`=login_id ≠ `authorized_user_id`)이 항상 403 이다 —
  `authorized_user_id` 를 콘솔 계정 login_id 로 재키잉하거나 소유 스코프를 org 로 옮기는 것은 후속 과제. **가입자(관제사)** 경로는
  §4.1 GMS 로 해소됨(토큰 sub→users.id 해석).
- A축 telephony feature 플래그 중 그룹 생성은 §4.1 `allow_create_group` 으로 구현. 나머지(can_emergency/can_video/max_priority)는 별도 트랙.
- 다중 역할·세밀 scope(org 단위)가 필요해지면 `user_permissions(user_id,capability,scope)` 테이블로 확장(현재는 단일 role + 그룹 소유 스코프로 충분).
