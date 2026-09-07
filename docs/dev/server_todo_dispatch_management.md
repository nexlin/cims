> 서버 쪽 보완 목록 — 관제조작반 관리 기능(조직/구성원/번호 · PTT 그룹 · 세션 이력/녹취 재생) 추가에 따른 서버·운영 작업.
> 앱·SDK·CSC 코드는 같은 변경(csc 0.2.108)에 들어 있다. 아래 §2 는 코드가 아니라 **배포·운영 절차**, §3 은 **아직 남은 서버 과제**다.
> 정본: [dispatch_center.md §3.4·§5.7a·§5.7b](../design/features/dispatch_center.md) · [android_ue_provisioning.md §3-2~§3-4](../design/features/android_ue_provisioning.md) ·
> [dispatch_desktop_ui.md §4.5](../design/features/dispatch_desktop_ui.md). 전부 반영되면 이 문서는 삭제한다.

# 관제조작반 관리 기능 — 서버 보완 목록

## 1. 이번 변경에 들어간 서버 파트 (csc 0.2.108 — 배포만 하면 된다)

| # | 항목 | 코드 | 상태 |
|---|---|---|---|
| S1 | 관제 그룹 속성 `directory_admin`(none\|own\|all) — 관제 앱 관리 범위 인가 축 | `sql/migrate_dispatch_directory_admin.sql`, `sql/cims_schema.sql`, `csc/src/handlers/dispatch.py`(프로브·manager 게이트·400 schema_not_migrated), 콘솔 `구성 > 관제 그룹` 폼(`ems/service/console/src/pages/DispatchGroupsPage.tsx`, `ems/core/console/src/api/dispatch.ts`) | 구현 · 단위시험 통과. **콘솔 tsc/vite 빌드는 이 PC 에 node 가 없어 미실행**(§2-5) |
| S2 | `/provisioning/me` `dispatch.directoryAdmin`·`orgCode` | `services/mcptt.py dispatch_discovery` | 구현 · 시험 통과 |
| S3 | 관제 앱 관리 API `/provisioning/directory/{admin,orgs,members,groups}` (MCPTT 서버 4430, PKCE) — admin/org 핸들러의 같은 쓰기 코드 호출, 범위 게이트, E-AUD-006 감사 | `csc/src/handlers/dispatch_directory.py` (+ `csc_app.py` 등록) | 구현 · 시험 통과(`tests/test_csc_dispatch_management.py` 29건) |
| S4 | GMS XCAP GET/PUT/DELETE 의 관리 범위 확장(소유자 아닌 그룹도 범위 안이면 허용, 생성은 `allow_create_group` 없이도) | `services/mcptt.py _admin_manages_group` | 구현 · 시험 통과 |
| S5 | `/provisioning/history` `until` 창 조회 + 항목 `recordingId`/`hasRecording` | `services/dispatch_history.py`, `services/mcptt.py` | 구현 · 시험 통과(기존 17건 + 신규) |
| S6 | 녹취 조회·재생 `/provisioning/recordings/{id}[/segments/{seq}/audio\|peaks]` — 범위 게이트 + oam-svc 프록시, E-AUD-016 `tap_mode=recording` | `csc/src/handlers/dispatch_recordings.py`, csc.json `Recording.OamUrl`/`Recording.VerifyTls`(`config_template.json`) | 구현 · 시험 통과(프록시는 가짜 HTTP) |
| S7 | 알람 카탈로그 — E-AUD-016 CSC 감지 행(history/recording) | `docs/design/alarm_catalog.csv` | 반영 |

검증 실행(Windows 로컬): `tests.test_csc_dispatch_management · test_csc_provisioning_history · test_csc_gms_group_crud · test_csc_provisioning_dispatch · test_csc_dispatch_rbac · test_csc_build_handler` = 118건 OK.
S1-UNIT-CSC(`verify/lib/items/stage1/unit_csc.py`)에 `tests/test_csc_dispatch_management.py` 등록 완료.

## 2. 배포·운영 절차 (개발 서버 .45 → 실기 시험 전)

1. **DB 마이그레이션**: `mysql cims < sql/migrate_dispatch_directory_admin.sql` (재실행 안전). 미적용이면 앱 관리 탭이 잠기고(`403 no_directory_admin`), 콘솔에서 관리 범위를 `none` 외로 바꾸면 400.
2. **csc.json**: `Recording.OamUrl` = OAM 게이트웨이(`https://<OAM_IP>:4419`, 이중화면 VIP). 비우면 `https://{Fm.OamIp}:4419`. 자가서명이면 `Recording.VerifyTls=false`(기본). configure 재생성(`deploy_value` 있음).
3. **CSC 배포·재기동**(csc 0.2.108) — MCPTT 서버(4430)에 라우트 4+1 개가 추가된다(`/provisioning/directory/{admin,orgs,members,groups}`, `/provisioning/recordings`). OAM 게이트웨이 라우트 변경 없음(4430 은 단말 직결).
4. **관제 그룹 설정**(콘솔 `구성 > 관제 그룹`, manager): 시험 그룹 `dg-dispatch01` 의 **관리 범위** = `소속 조직 하위`(조직 `TEAM01` 지정) 또는 `전체 조직`. 관리 범위 `own` 은 `org_id` 가 없으면 범위가 비어 관리 불가 — 조직을 먼저 지정한다.
5. **콘솔 빌드 확인**: `ems/core/console` 에서 `tsc -b && vite build` — `directory_admin` 필드·`DirectoryAdmin` 타입 추가분. 이 PC 는 node 가 없어 타입 검사를 못 돌렸다(수정 범위는 필드 하나·셀렉트 하나).
6. **접속서비스 목록**: 앱의 번호 개설 폼은 `access_services` 런타임 스토어(없으면 csc.json `Provisioning.Services`)에서 `service_ref` 후보를 받는다 — 개발 서버에 VoLTE/PTT 서비스 이름이 실제 CSP 서비스와 같은지 확인(H(A1) realm 결박 재료).
7. **실기 시험 항목**(앱 관리 창): ① 조직 생성/이동/삭제(범위 밖 403·not_empty 409) ② 구성원 생성(VoLTE+PTT 회선 동시)·회선 번호 변경(비밀번호 없이 400)·회선 삭제·자격 토글 → 단말 재등록 확인 ③ PTT 그룹 탭에서 비소유 그룹 편집/삭제 ④ 이력 창 조회(전날 포함)·녹취 행 재생(첫 재생 202 → 변환 대기 → 재생, `failed` 는 [다시 변환]) ⑤ 감사 이벤트(E-AUD-006 config_change · E-AUD-016 tap_mode=recording) 콘솔 `장애 > 감사 이력` 확인.

## 3. 남은 서버 과제 (이번 변경 밖)

| # | 과제 | 배경 | 제안 |
|---|---|---|---|
| T1 | **OAM 녹취·이력 API 인증 부재** — `handlers/recording.py`, `flow_logger.py` 의 `/api/v1/recordings·/call/logs·/ptt/sessions·/ptt/history·/messages` 는 `require_role` 이 없고 게이트웨이도 검증하지 않는다(api_docs 는 monitor 로 선언). `DELETE /api/v1/recordings/{id}` 는 무인증 rmtree | 지금은 CSC 프록시가 가입자 게이트를 대신하지만 4419 에 닿는 누구나 원 API 를 부를 수 있다 | 콘솔은 `<audio src>` 라 헤더를 못 붙이므로 **단기 서명 URL(쿼리 토큰)** 또는 쿠키 인증 + 나머지 경로 `require_role(monitor)`, DELETE 는 manager. CSC 프록시는 서버 간 토큰(`InternalApi.Token` 결)으로 호출 |
| T2 | 세션 레벨 `GET /api/v1/recordings/{id}/video?side=` 미구현(문서만) | 콘솔 `recordings.ts:115` 가 URL 을 만든다 | 구현하거나 문서·클라이언트에서 제거 |
| T3 | 이력 스캔 48 시간 버킷 상한 — 관리 창은 하루 단위로 나눠 묻지만 월 단위 조회·검색은 느리다 | `dispatch_history.py` 가 파일 glob | oam-svc `ptt_index`(일별 jsonl 읽기 모델)를 CSC 도 읽거나, CSC 가 자기 일별 인덱스를 두는 안. 통화(volte) 쪽은 인덱스가 없다 |
| T4 | 구성원 일괄 가져오기(CSV) 를 관제 앱 관리 API 에도 — 콘솔 `POST /api/v1/users/import` 와 같은 형식 | 앱은 건별 입력만 | `POST /provisioning/directory/members/import`(같은 파서 재사용, 범위 게이트) |
| T5 | 회선 여러 개인 구성원 — 관리 API 는 종류당 첫 회선만 노출/편집 | `_members_in_scope` 첫 행 | 필요하면 `volte[]`/`ptt[]` 배열로 계약 확장(앱 폼도) |
| T6 | 관제 그룹 편성 자체(멤버·대표번호·감청/청취/관리 범위)는 여전히 콘솔 전용 | 설계상 승인 사항(manager) | 유지. 앱에서 필요해지면 별도 인가 축으로 검토 |
| T7 | `directory_admin` 을 CSP 도 알아야 하는가 | CSP 는 관제 그룹 속성을 인메모리 맵으로 든다(§3.3) — 관리 범위는 CSC 만 판정하므로 **불필요**. `DISPATCH_GROUP_CHANGED` 재적재 시 모르는 컬럼은 무시됨 | 없음(확인만) |
| T8 | Android 관제 태블릿·SDK Kotlin 파사드에 `CscClient.request`·`DispatchProfile.directoryAdmin/orgCode` 반영 | SWIG `cimsue.i` 는 `csc.h` 를 포함하지 않는다 | Android 관리 화면 착수 시 |
