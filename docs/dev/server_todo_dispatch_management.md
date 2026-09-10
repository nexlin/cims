> 서버 쪽 보완 목록 — 관제조작반 관리 기능(조직/구성원/번호 · PTT 그룹 · 세션 이력/녹취 재생) 추가에 따른 서버·운영 작업.
> 앱·SDK·CSC 코드는 같은 변경(csc 0.2.108)에 들어 있다. 아래 §2 는 코드가 아니라 **배포·운영 절차**, §3 은 **아직 남은 서버 과제**다.
> 반영이 끝난 과제(OAM 녹취·이력 API 인증 게이트 + CSC 서비스 토큰, 세션 레벨 video URL 정리, 구성원 CSV/JSON 일괄 가져오기, CSC 의 `access_services` 읽기 =
> `CimsRuntimeDir` 전제([initial_install.md §4.2](../user-manual/initial_install.md)), `directory_admin` 은 CSC 전용)는 표에서 뺐다.
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
6. **접속서비스 이름**: 앱의 번호 개설 폼은 `GET /provisioning/directory/admin` 의 `services.<kind>[].name` 을 `service_ref` 후보로 받는다. CSC 는 CSP 의 `access_services` 컬렉션을 읽지 못하므로(관리 store 가 다르다 — android_ue_provisioning.md §3-2) 이 이름은 csc.json `Provisioning.Services.<kind>.name` 이 정본이다. 개발 서버는 PTT 접속서비스가 `mcptt` 라 configure `--ptt-service mcptt`(.cims `configure.ptt_service`)로 맞춘다 — 안 맞으면 앱이 개설한 회선의 service_ref 가 CSP 에 없어 REGISTER 403.
7. **실기 시험 항목**(앱 관리 창): ① 조직 생성/이동/삭제(범위 밖 403·not_empty 409) ② 구성원 생성(VoLTE+PTT 회선 동시)·회선 번호 변경(비밀번호 없이 400)·회선 삭제·자격 토글 → 단말 재등록 확인 ③ PTT 그룹 탭에서 비소유 그룹 편집/삭제 ④ 이력 창 조회(전날 포함)·녹취 행 재생(첫 재생 202 → 변환 대기 → 재생, `failed` 는 [다시 변환]) ⑤ 감사 이벤트(E-AUD-006 config_change · E-AUD-016 tap_mode=recording) 콘솔 `장애 > 감사 이력` 확인.

### 2-8. .48 실측(2026-09-07 18시, csc 0.2.108 라이브)과 재배포 필요분

- **실측 OK**: `/provisioning/me` `directoryAdmin=own/orgCode=TEAM01` · `/provisioning/directory/admin`(22명, services volte/`mcptt`) · 조직 생성/이름 변경/삭제·범위 밖 403·루트 이동 403·중복 409·not_empty 409 ·
  구성원 생성(VoLTE+PTT)·수정·삭제, 번호 변경(비밀번호 없이 400 / 있으면 201), 타인 번호 409, PTT 회선 삭제 · GMS 그룹 생성 → `/provisioning/directory/groups` isOwner 노출 → 삭제 ·
  `/provisioning/history` 창 조회(9/1~9/3 통화·PTT 항목에 `recordingId`/`hasRecording`) · 앱 관리 창 조직/구성원 탭 렌더.
- **재배포 필요(코드 수정됨, 커밋 참조)**: ① `Recording.VerifyTls` 를 문자열 `"false"` 로 렌더한 csc.json 을 `bool("false")=True` 로 읽어 OAM 프록시가 인증서 검증 실패(`502 oam_unreachable`) —
  fm_reporter 와 같은 문자열 bool 해석으로 정정. 재배포 전까지 녹취 메타/오디오는 502. ② `PUT …/ptt/profile` 이 저장 뒤 감사 페이로드에서 KeyError(500 — 값은 이미 반영됨). ③ 번호 변경 시 요청에 없는
  접속서비스·transport 를 종전 회선에서 승계(종전엔 UDP 기본값으로 개설됨).
- **SIP 등록 408 — 원인 확정(.48 csc.json 프로비저닝 ↔ CSP 리스너 불일치)**: `/provisioning/me` 가 두 서비스 모두 `15060/UDP, enforced=false`(템플릿 기본값)를 내리는데 .48 CSP 는
  15060/15061 을 열지 않고 **5060/UDP · 5061/TLS · 25061/TCP** 를 연다. 헤드리스 UE 실측: TLS 5061 → VoLTE·PTT 둘 다 200 등록, UDP 5060 → 403(`sip_transport=TLS` 집행), UDP 15060 → 무응답.
  조치 = csc.json `Provisioning.Services.{volte,ptt}.tls_port=5061`, `.port=5060`, `.tcp_port=25061`, `.transport=TLS` 로 맞추고 CSC 리로드 → 앱 재로그인. 앱 수정 없음.

## 3. 남은 서버 과제 (이번 변경 밖)

| # | 과제 | 배경 | 제안 |
|---|---|---|---|
| T3 | 이력 스캔 48 시간 버킷 상한 — 관리 창은 하루 단위로 나눠 묻지만 월 단위 조회·검색은 느리다 | PTT 창 조회·세션 상세는 OAM 세션 인덱스(`/api/v1/ptt/sessions`·`/ptt/history`)를 프록시한다(csc 0.2.112). **통화(volte) 쪽은 여전히 `dispatch_history.py` 파일 glob** | volte 도 oam-svc 의 `/api/v1/call/logs` 를 범위 게이트 뒤에서 프록시하거나, CSC 가 자기 일별 인덱스를 두는 안 |
| T5 | 회선 여러 개인 구성원 — 관리 API 는 종류당 첫 회선만 노출/편집 | `_members_in_scope` 첫 행 | 필요하면 `volte[]`/`ptt[]` 배열로 계약 확장(앱 폼도) |
| T6 | 관제 그룹 편성 자체(멤버·대표번호·감청/청취/관리 범위)는 여전히 콘솔 전용 | 설계상 승인 사항(manager) | 유지. 앱에서 필요해지면 별도 인가 축으로 검토 |
| T8 | Android 관제 태블릿·SDK Kotlin 파사드에 `CscClient.request`·`DispatchProfile.directoryAdmin/orgCode` 반영 | SWIG `cimsue.i` 는 `csc.h` 를 포함하지 않는다 | Android 관리 화면 착수 시 |
