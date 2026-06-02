# MCPTT 권한 모델 & 그룹 소유(authorized user) 설계 — 구현 계획서

> 상태: **설계 확정 / 구현 대기(다음 세션)**. 본 문서는 다음 세션에서 한 번에 구현할 스펙이다.
> 작성 배경: PTT 그룹 `group.json`의 placeholder 필드(`session_id="permanent"`, `call_id="autojoin"`) 정리 논의에서 출발해,
> "그룹 owner = 누구인가 / OAM 운영자와 가입자 신원 관계 / 권한 모델"로 확장되어 합의된 결과.

---

## 1. Context (왜)

- 현재 `group.json` 에는 옛 "세션=1통화" 모델의 placeholder(`session_id`,`call_id`,`initiator`)만 있고, 그룹을 식별·관리할 핵심 정보(소유자/관리자, 식별자, 유형)가 빠져 있다.
- 3GPP MCPTT(TS 23.280 / 24.481)에서 그룹 생성·관리 주체는 **authorized user**(MCPTT 사용자)이다. 별도의 "운영자 계정"이 아니라 **권한을 가진 가입자**다.
- CIMS는 이미 **단일 신원 모델**: 콘솔 로그인 계정과 가입자가 같은 `users` 테이블의 한 person이다(`users.login_id/password/role` = 콘솔 인증, `*_subscriptions` = telephony). → 이 위에 **세분 권한(역할)** 을 얹으면 "가입자 = 운영자 = MCPTT 관리자"가 하나로 통합된다.
- 따라서: ① 콘솔/운영 권한을 **역할(role)** 로 정리, ② 그룹 소유를 **authorized_user_id** 로 명시, ③ `group.json` 을 자기완결형 디스크립터로 재설계.

## 2. 신원·권한 두 축 (혼동 방지)

| 축 | 무엇 | 누구 | 저장 |
|---|---|---|---|
| **A. 서비스 이용 권한 (telephony)** | VoLTE/PTT 가입, 영상·긴급·우선순위 등 | 모든 단말 사용자 | `*_subscriptions` + service_ref (+향후 feature 플래그) |
| **B. 관리 권한 (role)** | 콘솔/운영/그룹관리 | 운영자/관리자 (선택) | `users.role` |

- 일반 단말 사용자 = A축만(구독), B축 `user`(콘솔 로그인 불가).
- 같은 `users` person 위에 두 축이 얹히되 **의미·네임스페이스 분리**.

## 3. 역할 모델 (단일 `users.role`, 계층적 5종)

`users.role ENUM('admin','manager','operator','monitor','user') DEFAULT 'user'`

| role | 한글 | 요약 |
|---|---|---|
| `admin` | 관리자 | **전체** — 시스템·인프라·릴리스·배포·검증 + 가입자·그룹·조직 + 모니터링 + 계정/권한 |
| `manager` | 운영 관리자 | **구성 CRUD 전체** (가입자·조직·PTT그룹) + 모니터링/장애. **인프라/배포/검증/계정 제외** |
| `operator` | 운용자(관제) | 구성 **조회만** + 운용 대응(알람 ack, MCPTT 관제) + **PTT그룹 생성 / 본인 소유 그룹만 관리** |
| `monitor` | 모니터 | **조회 전용** (대시보드/성능/이력/녹취 보기, ack 불가) |
| `user` | 일반 단말 사용자 | **OAM 로그인 불가**, telephony만 |

### 권한 매트릭스
| 도메인 | admin | manager | operator | monitor | user |
|---|---|---|---|---|---|
| 가입자/조직 | CRUD | CRUD | R | R | – |
| PTT 그룹 | CRUD(all) | CRUD(all) | **생성 + 본인소유 CRUD / 타인 R** | R | – |
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

## 5. `group.json` 재설계 (자기완결형, CSP 기록)

placeholder(`session_id`,`call_id`,`initiator`) **제거** → 디스크립터:
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

## 6. 구현 범위 / 파일 (다음 세션)

### Phase A — DB
- `sql/migrate_*_rbac.sql` (신규):
  - `users.role` → `ENUM('admin','manager','operator','monitor','user') DEFAULT 'user'` (기존 'admin'→'admin', 그 외/가입자→'user').
  - `ptt_groups` 에 `authorized_user_id BIGINT NULL` (FK→users.id, ON DELETE SET NULL) + `created_at DATETIME DEFAULT CURRENT_TIMESTAMP`.
- `sql/cims_schema.sql` inline 반영.

### Phase B — 인가 (CSC/OAM)
- `csc/src/services/admin_auth.py` — 로그인: `role=='user'` 거부. JWT 에 `role` 포함.
- 인가 미들웨어/데코레이터: 핸들러별 **요구 등급** 선언 + (그룹은) 소유 스코프 체크. `csc/src/handlers/admin.py`(그룹 CRUD: operator 생성 허용 + edit/delete `authorized_user_id==self` 게이트), org/users 핸들러 등급 게이팅.
- `csc/src/handlers/admin.py` `_create_group`: `authorized_user_id` 입력/기본=생성자, PTT 가입자 검증. `_list_groups`/`_get_group`: `authorized_user`(파생 MCPTT ID) 포함.
- `csc/src/services/mcptt.py` GMS: `<list-service>`/ruleset 에 authorized user 반영.

### Phase C — CSP (group.json/GMS)
- `csp/GroupCallService.cpp` `BuildGroupDescriptor(clsGroup)` — §5 전체 필드 생성(멤버 role 포함, authorized_user_id).
- `csp/CallDir.h` `PttSessionStart` — wrapper 를 디스크립터 + `state/created_at/updated_at` 로 (session_id/call_id/initiator 제거). initiator 는 state 파일용으로만 유지.
- `csp/CspPttGroup.{h,cpp}` + `csp/DbManager.cpp` `SelectGroup` — `authorized_user_id` 로드.

### Phase D — 콘솔
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
- `users.role` (이 문서) = 콘솔/운영/그룹관리 권한.
- `ptt_group_members.role` (chair/participant) = **통화 중 floor 권한** (TS 24.380) — 별개. 한 가입자가 `operator`(관리) + 어떤 그룹의 `chair`(발언통제)일 수 있음.

## 9. 미결/후속
- A축 telephony feature 플래그(can_create_group/can_emergency/can_video/max_priority)는 별도 트랙(이번 범위 외).
- 다중 역할·세밀 scope(org 단위)가 필요해지면 `user_permissions(user_id,capability,scope)` 테이블로 확장(현재는 단일 role + 그룹 소유 스코프로 충분).
