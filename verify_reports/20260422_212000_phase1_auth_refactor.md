# Auth / Users 엔드포인트 분리 리팩토링 (Phase 1 후속)

**일시**: 2026-04-22 21:20
**근거**: Phase 1 검증 중 "로그인 API 가 가입자 정보까지 함께 내리는 구조" 의 지적.

---

## 설계 변경 요약

| 이전 | 현재 |
|---|---|
| `POST /api/v1/auth/login` 응답에 `user.call_subscriptions[]`, `user.ptt_subscriptions[]` 포함 | `POST /api/v1/auth/login` 은 **토큰 + 최소 user 만** 반환 |
| `GET /api/v1/auth/me` — 프로파일 + subscriptions 혼합 | **제거** |
|  — | `GET /api/v1/users/me` — 본인 프로파일 (role, org_id, created) |
|  — | `GET /api/v1/users/me/subscriptions` — 본인 VoIP/PTT 가입자 배열 (Phone UE 용) |

### 엔드포인트 이름 결정 근거
- `/auth/me` 는 "인증 주체" 의미로 모호 (로그인 상태 확인인지 프로파일 조회인지).
- `/users/me` = "유저 리소스의 내 자신" — RESTful + 직관적.
- subscriptions 는 하위 리소스 (`/users/me/subscriptions`) 로 노출.

### 호출자별 필요 차이
- **Console (admin)**: `/users/me` 만 필요 (role 확인, 관리자 본인에게는 subscription 없음)
- **Phone (UE)**: `/users/me/subscriptions` 호출해서 `auth_id` / `passwd` / `domain` 받아 SIP REGISTER

---

## 구현 내역

### CSC
| 파일 | 변경 |
|---|---|
| `csc/src/handlers/auth.py` | `_login` 응답에서 `_user_with_subs` 호출 제거. `/auth/me` 라우트 + `_me` 함수 삭제. 주석 갱신. |
| `csc/src/handlers/users.py` | **신규** — `/api/v1/users/me`, `/api/v1/users/me/subscriptions`. 본인 JWT 기반 조회. subscription 응답에 `domain`, `auth_id`(=imsi@domain) 자동 조립 (access_services.jsonl 을 CSC 가 읽어 계산). |
| `csc/src/handlers/admin.py` | 경로 충돌 해소 — `handle_users` 에서 `person_id=='me'` 이면 `users.handle_users` 로 위임. |
| `csc/src/csc_app.py` | `CIMS_USERS_HANDLER_LIST` import + 라우트 등록 (`AUTH + USERS + ADMIN`). |

### Console (`cims-console/src/api/auth.ts`)
- `AuthUser` 에서 subscription 필드 제거.
- `UserProfile extends AuthUser`, `MySubscriptions` 타입 분리.
- `authApi.me()` → `GET /users/me`, `authApi.mySubscriptions()` 신규.

### Phone (`cims-phone/`)
| 파일 | 변경 |
|---|---|
| `src/api/auth.ts` | `CimsUser` 에서 subscription 필드 제거. `Subscription` 에 `service_ref`/`domain`/`imsi` 추가 (기존 `auth_id` 는 그대로). `authApi.me()` → `/users/me`, `mySubscriptions()` 신규. |
| `src/contexts/AuthContext.tsx` | user + subscriptions 2개 state. 로그인 시 `/auth/login` + `/users/me/subscriptions` 병행. `useAuth()` 반환에 `callSubscriptions`, `pttSubscriptions` 추가. |
| `src/pages/PhonePage.tsx` | `user.call_subscriptions` → `callSubscriptions` (context 에서). |

---

## 검증

### 3 엔드포인트 응답

```bash
$ curl -X POST /api/v1/auth/login -d '{"login_id":"admin","password":"1234"}'
{
  "token": "eyJ...",
  "user": {"id":33,"name":"관리자","login_id":"admin","role":"admin"}
}                                            # ← subscriptions 없음 (설계대로)

$ curl /api/v1/users/me -H "Authorization: Bearer <token>"
{
  "id":33,"name":"관리자","login_id":"admin","role":"admin",
  "org_id":"cims","create_time":"2026-03-23T19:05:59",
  "update_time":"2026-04-09T20:40:03"
}                                            # ← 프로파일 상세

$ curl /api/v1/users/me/subscriptions -H "Authorization: Bearer <token>"
{
  "call_subscriptions": [],
  "ptt_subscriptions": [{
    "id":"+821030432632",
    "service_ref":"ptt-default",
    "imsi":"jcryu74",
    "domain":"csp",                          # ← CSC 가 access_services.jsonl 로부터 조립
    "auth_id":"jcryu74@csp",                 # ← imsi@domain 완성형
    "passwd":"1234","dnd":false,...
  }]
}

$ curl /api/v1/auth/me   # 제거 확인
→ 404 Not Found
```

### 타입 체크
- CSC Python AST: OK
- Console TypeScript: exit 0
- Phone TypeScript: exit 0

### 경로 충돌 해소 확인
- admin.py 의 `/api/v1/users/:pid` 와 신규 `/api/v1/users/me/*` 가 같은 base path. admin.handle_users 가 person_id='me' 조기 분기로 users.handle_users 에 위임 → 두 경로 모두 정상 동작.

---

## 구조적 이점

1. **단일 책임**: auth 는 인증만. users 는 본인 리소스만. admin 은 관리자 CRUD.
2. **Phone/Console 요구 분리**: 호출자가 필요한 것만 가져감.
3. **스키마 변경 영향 최소**: subscriptions 테이블 변경 시 users.py 만 수정하면 됨 (이전에는 auth.py 도 깨짐).
4. **확장 가능**: `/users/{id}` (관리자용) 이후 추가 시 자연스러운 리소스 URI 구조.

---

## 잔존 TODO

- Console admin 페이지에서 `/users/me/subscriptions` 를 별도 호출할지는 향후 결정 (현재는 로그인 후 토큰만 있으면 동작; 개별 user 의 subscriptions 는 admin.py 의 `/users/:pid/call`, `/users/:pid/ptt` 로 관리).
- CSC `_access_service_domain_map` 은 파일 I/O 매 요청 — 향후 캐시 + SIGUSR1 기반 무효화 고려. 현재는 Phase 1 환경에서 성능 이슈 없음.

---

## 참조
- `verify_reports/20260422_210300_phase1_full_summary.md` — 전 모듈 기동 + 로그인 500 이슈 발견
- `verify_reports/20260422_205500_phase1_retest_summary.md` — v3 Blocker 수정
- 이 리포트 — 구조 개선 (auth/users 분리)
