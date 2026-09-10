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

### 2-9. .45(ctrl01) 관리 창 회선 저장 실패 — 접속서비스 후보 0건 (앱 로그 2026-09-10 12:23~12:31)

- **증상**: 앱 관리 창에서 구성원 생성·편집 시 VoLTE/PTT 회선이 저장되지 않음. 앱 로그: `PUT …/members/6001/volte 400 service_ref required to derive ha1 (unknown service)`(관제3 회선 개설),
  `PUT …/members/5020|5021/volte 400 passwd required when imsi or service_ref changes (ha1 rebinding)`(기존 구성원 저장).
- **원인(서버 상태)**: .45 CSC 의 `GET /provisioning/directory/admin` 이 `services.volte/ptt` 를 **빈 배열**로 내려준다. CSC 가 `CimsRuntimeDir` 없이 자기 모듈 runtime
  (`modules/csc/runtime/collections/csp/access_services`, 존재하지 않음)을 보고, csc.json 폴백 `Provisioning.Services` 도 설정되지 않았다. CSP 의 정본은
  `/mnt/cims/runtime/collections/csp/access_services` 에 `volte`(volte.cims.example.kr)·`mcptt`(ptt.cims.example.kr) 두 건이 있다. 후보가 비어 앱이 `serviceRef` 없이 회선을 보내면
  `_service_realm` 이 None → 400.
- **조치(운영)**: CSC 가 관리 store 를 oam 에서 유도받도록(`CimsRuntimeDir`, [initial_install.md §4.2](../user-manual/initial_install.md)) 재구성·재기동하면 CSP 접속서비스 두 건이
  그대로 후보가 된다. 그 전까지는 csc.json `Provisioning.Services.volte = {name: "volte", domain: "volte.cims.example.kr", …}`, `.ptt = {name: "mcptt", domain: "ptt.cims.example.kr", …}`
  폴백으로 메운다(§2-6 규약).
- **2026-09-10 15:13 csc 0.2.113 재배포 뒤 상태**: agent 오버레이(`modules/csc/current/csc/config.json`)에 `CimsRuntimeDir=/opt/cims-agent/modules/oam/runtime` 이 주입됐다(OAM 0.2.112).
  그런데 그 아래 CSP 미러 `modules/csp/runtime/collections/access_services/` 가 비어 있어(`.schema_version` 만) 여전히 폴백 `Provisioning.Services` 를 쓰고, 후보는
  `volte`(volte.cims.example.kr)·**`ptt`**(ptt.cims.example.kr) 두 건이다. CSP 는 config 캐시 `config/access_services.jsonl` 로 `volte`·`mcptt` 를 로드했고 DB 의 PTT 회선 42건은 전부
  `mcptt` 다 → 앱이 만드는 PTT 회선은 `ptt` 로 저장돼 REGISTER 403. 즉시 조치 = 콘솔 csc 패키지 설정 `Provisioning.Services.ptt.name=mcptt` 저장+재기동. 근본 = §4.4(미러가 비는 원인).
  DB 실측: 관제3(6001) 은 회선 없이 남아 있고, 관제1·2 회선의 IMSI 는 `+821310001001` 처럼 `+` 를 포함해 저장돼 있어(콘솔 입력값) 앱 게이트의 숫자 채움과 어긋난다(§4.2 A1·§4.3 B1).
- **앱 쪽(같은 날 수정)**: 폼에 "접속서비스 목록이 비어 있습니다" 경고 + 회선 개설 저장 차단(서버 400 대신 앱 문구), 기존 회선 편집 시 저장된 접속서비스·IMSI 를 그대로 실어 서버가
  "IMSI/서비스 변경" 으로 오판하지 않게 함(종전에는 저장값이 후보에 없으면 첫 후보로 바꿔 넣어 PUT 마다 400), 서버 문장형 오류를 사전 문구로 번역.

## 3. 남은 서버 과제 (이번 변경 밖)

| # | 과제 | 배경 | 제안 |
|---|---|---|---|
| T3 | 이력 스캔 48 시간 버킷 상한 — 관리 창은 하루 단위로 나눠 묻지만 월 단위 조회·검색은 느리다 | PTT 창 조회·세션 상세는 OAM 세션 인덱스(`/api/v1/ptt/sessions`·`/ptt/history`)를 프록시한다(csc 0.2.112). **통화(volte) 쪽은 여전히 `dispatch_history.py` 파일 glob** | volte 도 oam-svc 의 `/api/v1/call/logs` 를 범위 게이트 뒤에서 프록시하거나, CSC 가 자기 일별 인덱스를 두는 안 |
| T5 | 회선 여러 개인 구성원 — 관리 API 는 종류당 첫 회선만 노출/편집 | `_members_in_scope` 첫 행 | 필요하면 `volte[]`/`ptt[]` 배열로 계약 확장(앱 폼도) |
| T6 | 관제 그룹 편성 자체(멤버·대표번호·감청/청취/관리 범위)는 여전히 콘솔 전용 | 설계상 승인 사항(manager) | 유지. 앱에서 필요해지면 별도 인가 축으로 검토 |
| T8 | Android 관제 태블릿·SDK Kotlin 파사드에 `CscClient.request`·`DispatchProfile.directoryAdmin/orgCode` 반영 | SWIG `cimsue.i` 는 `csc.h` 를 포함하지 않는다 | Android 관리 화면 착수 시 |
| T10~T13 | **관리 쓰기 경로 단일화** — 앱 게이트(`dispatch_directory.py`)가 만든 업무 규칙(IMSI 채움·접속서비스 후보·생성 부분 성공·문장형 오류)을 코어(`admin.py`)로 내린다 | 콘솔은 되고 앱은 안 되는 불일치의 뿌리(§2-9) | **구현 지시 = §4** |

## 4. 관리 쓰기 경로 단일화 — 구현 지시 (T10~T13)

### 4.1 문제의 구조

콘솔과 관제 앱은 **같은 코어**로 회선을 쓴다. 다른 것은 바깥 층 하나다.

| 경로 | 진입 | 층 |
|---|---|---|
| 콘솔 워크벤치 | OAM 게이트웨이 4419 `PUT/POST /api/v1/users/{id}/call\|ptt` | `csc/src/handlers/admin.py` `handle_users` → `_add_subscription` / `_update_subscription` / `_create_user` |
| 관제 앱 | CSC MCPTT 서버 4430 `PUT/POST /provisioning/directory/members/…` | `csc/src/handlers/dispatch_directory.py` `_member_write`(인가·범위·감사·필드명 변환) → **같은** `admin.py` 함수 |

바깥 층 `dispatch_directory.py` 가 **인가와 이름 변환만 해야 하는데 업무 규칙 네 가지를 스스로 만들었고**, 콘솔 경로에는 그 규칙이 없다. 그래서 콘솔은 되고 앱은 안 되는 불일치가 난다(§2-9). 규칙을 안쪽 층 `admin.py` 로 내리고 바깥 층을 얇게 만드는 것이 보완이다. 진입점을 하나로 합치지는 않는다 — 주체(관리자 JWT ↔ 가입자 토큰)와 인가 축(role ↔ 관제 그룹 `directory_admin`)이 다른 것은 설계 그대로다(dispatch_center.md §3.4).

### 4.2 `csc/src/handlers/admin.py` — 규칙을 여기로 모은다 (콘솔·앱 공용)

| # | 함수 | 지금 | 바꿀 것 |
|---|---|---|---|
| A1 | `_add_subscription` | `imsi` 없으면 400 `imsi required` | **개설 기본값**: `imsi` 가 없거나 비면 `id`(MSISDN) 의 숫자(`lstrip('+')`)로 채운다 — USIM 없는 관제 소프트폰 규약(volte_supplementary_services.md). 규약의 주인은 코어다. `dispatch_directory._sub_body` 의 같은 코드는 지운다 |
| A2 | `_add_subscription` | `service_ref` 없으면 `_service_realm` None → 400 `service_ref required to derive ha1 (unknown service)` | **기본 채택**: `service_ref` 가 없으면 그 종류(volte\|ptt) 접속서비스 후보를 하나의 조회 함수(4.4)로 읽어 **1건이면 그것**, 0건 `400 no_service`, 2건 이상 `400 service_ref_required {"choices":[…]}`. 이름이 후보에 없으면 `400 unknown_service {"serviceRef":…}`, 후보의 domain 이 비면 `400 service_domain_missing`. `passwd` 가 있을 때만 realm 을 요구한다(지금과 같음) |
| A3 | `_update_subscription` | `'imsi' in body` 면 변경으로 본다(지금 로직은 맞다) | 유지. 단 바깥 층이 값을 채워 넣지 않게 되므로(4.3 B1) "요청에 없으면 유지" 가 실제로 성립한다. `binding_changed` 판정과 `passwd` 요구는 그대로 |
| A4 | `_add_subscription`·`_update_subscription`·`_create_user` 오류 본문 | 문장형(`passwd required when imsi or service_ref changes (ha1 rebinding)`, `imsi required to derive ha1`, `sip_transport must be one of …`) | **토큰형** `{"error": "<token>", "detail": "<지금 문장>"}` 로 통일. 토큰: `ha1_rebinding_required`(imsi/service_ref 변경에 passwd 없음), `ha1_password_required_for_digest`(aka→digest), `imsi_required`, `unknown_service`, `no_service`, `service_ref_required`, `service_domain_missing`, `invalid_sip_transport`, `invalid_auth_scheme`. 콘솔(`ems/core/console` 가입자 화면 문구)·앱(`ResponseText.ForManagementError`)이 같은 토큰을 본다 |

### 4.3 `csc/src/handlers/dispatch_directory.py` — 얇게 만든다 (관제 앱 게이트)

| # | 함수 | 바꿀 것 |
|---|---|---|
| B1 | `_sub_body` | 이름만 바꾼다: `msisdn→id`, `imsi→imsi`(있을 때만), `serviceRef→service_ref`(있을 때만), `sipTransport→sip_transport`(대문자), `password→passwd`(있을 때만). **값을 만들지 않는다** — `imsi` 기본값 코드 삭제(A1 로 이동) |
| B2 | `_member_write` POST | `_create_user` 성공 뒤 회선 `_add_subscription` 이 하나라도 실패하면 `_delete_user(uid)` 로 되돌리고 `{…원 오류…, "rolledBack": true}` 를 그 상태코드로 반환 — **전부 성공 또는 전부 없음**. 감사는 성공한 경우에만 남긴다 |
| B3 | `_member_write` PUT `/volte\|ptt` 번호 변경 경로 | 종전 회선 service_ref·sip_transport 승계(지금 있음)는 유지. `service_ref` 미지정·미상 처리는 A2 에 맡긴다(여기서 선판정하지 않음) |
| B4 | `_services` | 삭제하고 4.4 의 공용 조회 함수를 부른다(`GET /provisioning/directory/admin` 의 `services` 응답 형태 `{volte:[{name,domain}],ptt:[…]}` 는 유지) |

### 4.4 접속서비스 후보 = 원천 하나 (`csc/src/services/` 신설 함수, 콘솔·앱·H(A1) 공용)

- 신설 `services/access_services.py`(CSC 쪽) 에 `list_services(config) -> {volte:[{name,domain,auth_realm}], ptt:[…]}` 하나를 두고, `admin._service_realm`·`dispatch_directory` 가 **둘 다 이것만** 쓴다. 지금은 `_service_realm`(admin.py)·`_services`(dispatch_directory.py)가 각자 컬렉션을 읽고 각자 폴백한다.
- 읽는 순서는 지금과 같다: ① `ha_lookup.collection_dir(config, 'access_services')`(OAM 이 주입한 `CimsRuntimeDir` 아래 CSP 미러) ② 비면 csc.json `Provisioning.Services.<kind>` 폴백. **폴백을 쓸 때 로그 경고 한 줄**(`access_services 컬렉션 비어 있음 — Provisioning.Services 폴백`)을 남긴다 — .45 처럼 미러가 비어 폴백 이름(`ptt`)이 나가는 상태가 조용히 지나가지 않게.
- **미러가 비는 원인**(운영·OAM 과제, §2-9): .45 는 `CimsRuntimeDir=/opt/cims-agent/modules/oam/runtime` 이 주입됐는데 그 아래 `modules/csp/runtime/collections/access_services/` 에 파일이 없다(`.schema_version` 만). CSP 자체는 OAM 이 준 config 캐시(`config/access_services.jsonl`, 2건)로 `volte`·`mcptt` 를 로드했다. OAM 의 미러 갱신(`ems/core/oam/src/services/access_services.py` `refresh_mirror`)이 이 노드에서 왜 채워지지 않는지 확인하고, 채워질 때까지 .45 csc 오버레이 `Provisioning.Services.ptt.name` 을 `mcptt` 로 맞춘다(지금 `ptt` — 앱이 만든 PTT 회선은 CSP 에 없는 service_ref 라 REGISTER 403).

### 4.5 정책 결정 — "새 회선은 SIP 비밀번호 필수" 를 어디에 둘 것인가

지금은 앱(`windows/dispatch-desktop/ViewModels/DirectoryAdminViewModel.cs`)만 강제하고, 콘솔은 비밀번호 없이 만들 수 있다(H(A1) 비움 → 등록 불가 상태). 두 경로가 한 규칙을 갖게 하려면 코어에서 정해야 한다.
- 안 ①(권장): `_add_subscription` 에서 `auth_scheme=digest` 이고 `passwd` 가 없으면 `400 ha1_password_required` — Digest 가입자는 H(A1) 없이는 어차피 쓸 수 없다(sip_access_security.md P1, `passwd` 컬럼은 읽지 않음). AKA 는 K/OPc 가 자격이므로 예외.
- 안 ②: 콘솔처럼 허용하고 앱의 강제만 푼다.
결정 전까지 앱 규칙은 그대로 둔다.

### 4.6 시험·문서 (같은 변경에서)

- `tests/test_csc_dispatch_management.py`: `SubBodyTests` — 이름 변환만 하는지(imsi 미포함); `MemberWriteTests` — 같은 번호 PUT 에 imsi 없을 때 `_update_subscription` 본문에 `imsi` 키가 없음 · POST 회선 실패 시 `del_user` 호출 + `rolledBack` · 후보 1건 기본 채택 · 미상 이름 `unknown_service`.
- admin 쪽 시험(`tests/test_csc_*admin*` 가 있으면 거기, 없으면 신설): A1 기본값, A2 후보 0/1/2건, A4 토큰.
- 계약 문서: [android_ue_provisioning.md §3-3](../design/features/android_ue_provisioning.md) 표의 `POST members`·`PUT …/volte|ptt` 행에 `serviceRef` 생략 규칙·`rolledBack`·오류 토큰을 적고, [admin_api.md](../api/admin_api.md) 가입 API 오류 표에 같은 토큰을 적는다. [dispatch_desktop_ui.md §4.5](../design/features/dispatch_desktop_ui.md) 는 앱 우회 설명을 "서버가 판정" 으로 줄인다.
- 앱: `ResponseText.ForManagementError` 에 새 토큰을 추가하고, 문장형 접두 매칭은 구 서버 호환으로 남긴다. 저장된 IMSI·서비스를 되돌려 보내는 우회는 무해하므로 유지.

### 4.7 완료 판정

1. 콘솔 워크벤치와 관제 앱에서 **같은 구성원**에 같은 입력으로 회선 개설·갱신·번호 변경을 하면 결과(성공/거절 코드)가 같다.
2. 앱에서 IMSI 를 보내지 않고 접속서비스·transport 만 바꿔도 비밀번호 요구가 나지 않는다(IMSI 유지).
3. 접속서비스 후보가 1건인 배포에서 앱이 `serviceRef` 를 생략해도 개설이 201 이다.
4. 회선 개설이 실패한 POST 뒤에 사용자가 남지 않는다.
5. .45 에서 `GET /provisioning/directory/admin` 의 `services.ptt[0].name` 이 CSP 와 같은 `mcptt` 다.
