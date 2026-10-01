# CIMS DB Schema — SSOT

> 외부 이중화 DB(MariaDB/MySQL 호환) 에 적재할 CIMS 테이블 인벤토리.
> 코드/마이그레이션 정합성의 단일 출처(SoT).
> 스키마 생성 = `sql/cims_schema.sql` + `sql/migrate_*.sql` 순차 적용.
>
> 가입자 정보/상태 외 모든 데이터는 파일 기반(file_store)이 SoT.
> file_store 도메인 상세: [runtime_store_design.md](runtime_store_design.md) §1.

## 1. 적용 순서 (신규 환경)

```bash
mysql -u root -p < sql/cims_schema.sql
# 이후 migrate_*.sql 을 파일명 정렬순으로 일괄 적용
for f in sql/migrate_*.sql; do mysql -u root -p cims < "$f"; done
```

`migrate_*.sql` 은 idempotent(`IF EXISTS` / `IF NOT EXISTS`) 로 작성. 이미 반영된 환경 재실행 시 NO-OP.

## 2. 테이블 인벤토리 (도메인별)

> **규칙:** 신규 데이터는 DB 테이블을 새로 만들지 않고 file-store(collection/jsonl)로 시작한다. DB 는 가입자(person/VoLTE/VoIP/PTT) 도메인 등 관계형이 본질적으로 필요한 데이터에 한정한다 → [runtime_store_design.md](runtime_store_design.md).
>
> 취소선(~~table~~) 항목은 DB 테이블 없이 파일 기반(file_store)으로 운영되는 도메인.

| 도메인 | 테이블 | 정의 파일 | 비고 |
|---|---|---|---|
| **가입자** | `users` | cims_schema.sql + migrate_add_email.sql + migrate_users_title.sql | 개인 (name/email/org_id/title/details) — `title`=직함, GMS 그룹문서 `cims:user-title` 로 UE 전달 |
| | `volte_subscriptions` | cims_schema.sql + migrate_voip_to_volte.sql + migrate_subscription_transport.sql + migrate_subscription_ha1.sql + migrate_subscription_aka.sql + migrate_icb_naming.sql | 이동 VoLTE MSISDN(kind=volte 접속서비스만 참조), SIP 인증(`ha1` SoT, `auth_scheme`/AKA 자료(`k_enc`/`opc_enc`/`sqn`/`amf`), `sip_transport` 채널 정책 — NULL=ANY 단말 선택), `icb_all`(착신 차단 — 전체, TS 24.611 ICB)/forward. FK 이름 `fk_voip_sub_user` |
| | `voip_subscriptions` | cims_schema.sql + migrate_voip_subscriptions.sql + migrate_icb_naming.sql | 유선 VoIP MSISDN — `volte_subscriptions` 와 컬럼 동일(pickup_group 포함), `service_ref` 는 kind=voip 접속서비스만(**테이블 = 접속환경 kind**, [sip_service_model.md §2-9](features/sip_service_model.md)). 마이그레이션이 `service_ref ∈ @voip_refs`(기본 `voip`) 인 volte 행을 옮긴다. FK `fk_voipsub_user` → users CASCADE. 번호는 세 가입 테이블·대표번호에 걸쳐 유일(CSC 게이트) |
| | `icb_identities` | cims_schema.sql + migrate_icb_naming.sql | 착신 차단 — 지정 번호(TS 24.611 ICB `cp:identity`) — (`user_id`, `identity`) PK, `user_id` → users. 사람 단위라 그 사람의 모든 전화 회선(volte·voip)에 적용 ([volte_supplementary_services.md §6B](features/volte_supplementary_services.md)) |
| | `ptt_subscriptions` | cims_schema.sql + migrate_auth.sql + migrate_auth_id_dropped.sql + migrate_subscription_transport.sql + migrate_subscription_ha1.sql + migrate_subscription_aka.sql + migrate_icb_naming.sql | MCPTT ID, IMPI 인증(`ha1` SoT, `auth_scheme`/AKA 자료, `sip_transport` 채널 정책). 착신 차단 컬럼(`icb_all`)은 없다 — MMTel ICB 는 MCPTT 대상이 아니다 |
| | `ptt_user_profile` | cims_schema.sql + migrate_ptt_user_profile_v2.sql + migrate_ptt_user_profile_v3.sql + migrate_ptt_ambient_listening.sql + migrate_ptt_allow_create_group.sql + migrate_ptt_non_ack_users_info.sql + migrate_ptt_user_profile_cancel_authz.sql | 사용자 MCPTT 프로파일(TS 24.484) — SOS 대상 결정 모드/전용 긴급그룹·개시 인가 3종 ([mcptt_emergency_modes.md](features/mcptt_emergency_modes.md) §2), 긴급 사설콜, `allow_ambient_listening`(원격 청취 자격 — [dispatch_center.md](features/dispatch_center.md) §5.6, 기본 0. 역할 `ptt_listen≠none` 배정 시 CSC 가 동기 — 직접 편집 없음, mcptt_authorization.md §2.4), `allow_create_group`(CIMS 확장 GMS 그룹 생성 자격 — mcptt_authorization.md §3, 기본 0), `allow_non_ack_users_info`(anyExt `allow-to-receive-non-acknowledged-users-information` — 그룹 호 개시자의 확인 통화 미응답 멤버 INFO 수신 자격, TS 24.379 §6.3.3.3, 기본 0), 해제 인가 셋(TS 24.484 ruleset) `allow_cancel_group_emergency`(그룹 진행 중 긴급 상태 해제 — local policy = 개시자 ∨ 이 값, TS 24.379 §6.3.3.1.13.4, 기본 0)·`allow_cancel_imminent_peril`(임박 위험 해제 §6.3.3.1.13.6, 기본 1)·`allow_cancel_emergency_alert`(긴급 경보 취소 §6.3.3.1.13.3, 기본 1 — 마이그레이션이 붙일 때 `allow_emergency_alert` 값을 옮긴다) |
| | `volte_subscriptions.forward_busy_id`·`forward_no_reply_id`·`forward_no_reply_sec`·`forward_not_logged_in_id`·`forward_not_reachable_id` / `voip_subscriptions.…` | migrate_subscription_cdiv.sql (재실행 안전 — 컬럼 5종, 적용 판정은 `forward_not_reachable_id`) | 조건부 착신전환 CFB/CFNR(+시한 초, 0=CSP 기본)/CFNL/CFNRc 대상 번호. 전화 가족(volte·voip) 테이블에만 있고 `ptt_subscriptions` 에는 없다 — CSP `DbManager` 는 `ringback_media`·이 컬럼들을 전화 테이블 SELECT 에만 넣는다(`RingbackCol/CdivCols(alias, bPhone)`)(TS 24.604 — volte_supplementary_services.md §6A.4). '' = 전환 없음. CSC 가 쓰고 CSP 가 읽는다 |
| | `volte_subscriptions.ringback_media` / `voip_subscriptions.ringback_media` | migrate_subscription_ringback.sql | 가입자 링백(컬러링) 음원 id(`sys:`/`op:`/`sub:` — 안내 라이브러리, announcements.md §6.3). NULL=접속서비스 프로파일의 ringback 그대로. CSC `ringback_media` 가 쓰고 CSP 가 LoadAllUsers/SelectUser 로 읽는다 |
| | `volte_subscriptions.pickup_group` / `voip_subscriptions.pickup_group` / `ptt_subscriptions.pickup_group` | migrate_subscription_pickup_group.sql (voip 는 생성 시점부터 보유) | 당겨받기 그룹 축 = **전화 그룹 id**(NULL = 어떤 픽업·BLF 축에도 속하지 않음 — org 폴백 없음). 전화 그룹 소속 가입자는 값이 그룹 id(`pg-…`, 전환 전 `dg-…`)로 **파생**된다(CSC 단일 쓰기 주체, 직접 편집 409). 파생은 **person 단위** — 멤버 행(유선 회선)과 같은 person 의 PTT 회선도 같은 값을 받는다(dispatch_center.md §3.2). 기존 데이터 백필 = 마이그레이션 끝 |
| **전화 그룹** | `phone_groups` | cims_schema.sql + migrate_phone_groups_roles.sql(전환 — `dispatch_groups` 계열에서 복사 후 DROP) | 유선 전화 그룹(픽업 그룹 + 대표번호, [dispatch_center.md](features/dispatch_center.md) §3.1·§8.1) — `id`(VARCHAR(64) 불변 키 `pg-…`, 전환 전 `dg-…` 유지), name, `pilot_id`(UNIQUE, 대표번호), `service_ref`(유선 VoIP 서비스), `alert_mode`(parallel/sequential), `no_answer_sec`, `busy_members`(skip/alert), `overflow_target`, `org_id`(FK organizations SET NULL). 관제 권한 열은 없다 |
| | `phone_group_members` | cims_schema.sql + migrate_phone_groups_roles.sql | `user_id` **PK**(가입자당 그룹 하나), `group_id`(FK CASCADE), `alert_order`(sequential 호출·포크 상한 절삭 순) |
| **역할** | `roles` | cims_schema.sql + migrate_phone_groups_roles.sql | 권한 = 능력 + 범위([mcptt_authorization.md](features/mcptt_authorization.md) §2) — `id`(내장 `admin/manager/operator/monitor` + `role-…`), name, `builtin`, `authz_manage`, `audit_read`, `directory_write`/`directory_read`(none/own/all), `ptt_group_manage`(none/own/scope/all), `monitor_call`(none/own/listed/all), `ptt_listen`(none/listed/all), `listen_visibility`, `history_read`(none/scope/all), `alarm_ack`, `mcptt_control`, `org_id`(own 루트). 내장 4행은 마이그레이션이 시드 |
| | `role_assignments` | cims_schema.sql + migrate_phone_groups_roles.sql | (`principal_type` console\|user, `principal_id`) **PK** — 사람당 역할 하나, `role_id`(FK CASCADE). 콘솔 계정의 배정은 OAM file_store `console_accounts[].role` 이 정본이라 여기엔 `user` 행만 쓴다 |
| | `role_monitor_targets` | cims_schema.sql + migrate_phone_groups_roles.sql | (`role_id`, `phone_group_id`) — `monitor_call=listed` 의 감청·감시 대상 전화 그룹 |
| | `role_ptt_targets` | cims_schema.sql + migrate_phone_groups_roles.sql | (`role_id`, `ptt_group_id`=**ptt_groups.id surrogate** FK CASCADE) — `ptt_listen=listed` 대상. CSP 는 적재 시 `mcptt_group_id` 로 해석 |
| | `mcptt_service_config` | cims_schema.sql + migrate_mcptt_service_config.sql | MCPTT **시스템 전역** 서비스 설정(TS 24.484 §8.4 service-config) — **단일 행 id=1**. N2(user-profile MaxAffiliationsN2 기본값) + broadcast-group 계층 수. 인가 컬럼 없음(인가 = ptt_user_profile·그룹). 편집=`PUT /api/v1/mcptt/service-config`(콘솔 구성>MCPTT 정책). 옛 스위치 컬럼 제거 = `migrate_service_config_drop_switches.sql`(같은 DB 의 CSC 전부 0.2.133 이상 뒤) |
| | `users.login_id/password/role` | migrate_auth.sql | 콘솔 인증(가입자와 동일 신원). `role` RBAC ([mcptt_authorization.md](features/mcptt_authorization.md)) |
| **PTT 그룹** | `ptt_groups` | cims_schema.sql + migrate_ptt_groups_v2.sql + migrate_ptt_groups_v3_3gpp.sql + migrate_ptt_group_conference_state.sql + migrate_ptt_groups_broadcast_call.sql + migrate_ptt_groups_ack_call_setup.sql | **id=surrogate BIGINT AI(PK, 디렉터리/FK 키)**, `mcptt_group_id`(UNIQUE 식별자), name/priority/encryption/emergency/org_code, **group_type(prearranged/chat — on-network-invite-members)/on_network/max_members/require_affiliation/alias/icon_url** (3GPP), `allow_conference_state`(on-network-allow-conference-state — 멤버의 conference 구독 허용, 기본 1), `hang_timer_sec`(on-network-hang-timer = 그룹 호 T4, 기본 30)·`max_duration_sec`(on-network-maximum-duration = TNG3, 기본 3600) — migrate_ptt_groups_broadcast_call.sql (일제 통화는 그룹 종류가 아니라 호 속성), 확인 통화 설정 `min_number_to_start`(on-network-minimum-number-to-start, 기본 0)·`ack_timeout_sec`(on-network-timeout-for-acknowledgement-of-required-members = TNG1, 기본 5)·`ack_action`(proceed/abandon, 기본 abandon) — migrate_ptt_groups_ack_call_setup.sql. `video_enabled` 열은 현행 코드가 읽지도 쓰지도 않는다(MCPTT 그룹 호는 음성만, 그룹 영상 = 아래 `mcvideo_group_attrs`) — DB 를 공유하는 전 사이트가 새 빌드가 된 뒤 DROP 한다([mcvideo.md](features/mcvideo.md) §8 3) |
| | `ptt_group_members` | cims_schema.sql + migrate_ptt_groups_v3_3gpp.sql + migrate_ptt_groups_ack_call_setup.sql + migrate_ptt_group_members_implicit_affiliation.sql | group_id=**surrogate ptt_groups.id(BIGINT FK)**, user_id, priority, **role(chair/participant), mcptt_id**, `on_network_required`(<on-network-required> 필수 멤버, 기본 0), `implicit_affiliation`(user profile <ImplicitAffiliations> 대상 — 등록 때 서버가 제휴, 기본 0, mcptt_standard_conformance.md C9) |
| | `ptt_affiliations` | migrate_ptt_groups_v3_3gpp.sql | MCPTT affiliation(TS 24.379 §9): (group_id, user_id, client_id) + affiliated_at/expires_at/status |
| | `ptt_session_seq` (시퀀스) | migrate_ptt_session_seq.sql | PTT 세션 ID 발급 시퀀스 |
| **MCVideo** | `mcvideo_group_attrs` | cims_schema.sql + migrate_mcvideo.sql | **행 = 그 그룹이 MCVideo 그룹**(한 그룹 id 에 서비스 집합 — [mcvideo.md](features/mcvideo.md) §5.1·§7 D4). `group_id` PK = ptt_groups.id(FK CASCADE). 열 = MCVideo `<list-service>` 속성(TS 24.481 §7.2.2): `invite_members`(0=chat 기본·1=prearranged)·`max_duration_sec`(TNG3)·`max_transmitters`(동시 송출 상한, 기본 2)·`audio_encodings`/`video_encodings`(선호 rtpmap 이름, 쉼표 구분 — 기본 AMR-WB/H264)·`video_resolutions`/`video_frame_rate`(NULL=생략)·`reception_hang_timer_sec`(T5)·`min_number_to_start`·`group_priority`(0..255 높을수록 높음, NULL=생략)·`protect_media`/`protect_transmission_control`(없으면 true 로 읽혀 늘 명시 — E2E 전까지 0, D7)·`allow_conference_state`. 송출 제어 T1 은 MCPTT 그룹의 `hang_timer_sec` 를 쓴다(TS 24.581 §11.1.3). 마이그레이션이 `video_enabled=1` 그룹마다 기본값 행을 만든다(§8) |
| | `mcvideo_user_profile` | cims_schema.sql + migrate_mcvideo.sql | **행 = PTT 회선의 MCVideo 이용 자격**(TS 24.484 §9.3) — `ptt_id` PK = ptt_subscriptions.id(FK CASCADE, MCVideo ID = MCPTT ID, D1), `max_video_streams`(`<MaxSimultaneousVideoStreams>` = 서버 카운터 C9, 기본 1), `max_calls_n6`(MCVideo 그룹 호 N6, 기본 1), `max_affiliations_n2`(`<MaxAffiliationsN2>` — 동시 MCVideo 제휴 그룹 N2, 기본 4, MCPTT N2 와 따로 — `migrate_mcvideo_n2.sql`). 우선순위는 MCPTT 와 같은 설정을 쓴다. 마이그레이션이 기존 PTT 회선 전부에 행을 만든다(현행 «PTT 영상» 이용 범위 보존) |
| | `mcvideo_affiliations` | cims_schema.sql + migrate_mcvideo.sql | MCVideo affiliation(TS 24.281 §8 — MCPTT 와 따로, TS 23.280 §5.2.5): `ptt_affiliations` 와 같은 열·PK. 서비스별 표라 옛 CSP 의 `ptt_affiliations` 정리 DELETE·옛 OAM 집계가 MCVideo 행을 건드리지 않는다 |
| **조직** | `organizations` | migrate_organizations.sql | code/name/parent_id 트리 — `users.org_id` FK 대상으로 가입자 도메인과 함께 DB 유지 |
| **인증** | ~~`auth_codes`~~ | — | **파일 기반** — `{CimsRuntimeDir}/auth_codes/<code>.json` |
| | ~~`refresh_tokens`~~ | — | **파일 기반** — `refresh_tokens/<token>.json` |
| **녹취** | ~~`recordings`~~ | — | **파일 기반** — call.json + recordings/ 디렉토리. CSP InsertRecording no-op, CSC `/api/v1/recordings` 가 파일 스캔. |
| | ~~`recording_segments`~~ | — | (call.d 내 segments.jsonl 임베드) |
| **모니터링** | ~~`stats_daily`~~ / ~~`stats_monthly`~~ / ~~`stats_yearly`~~ | — | 코드 미사용 unused tables (DROP 대상) |
| **CSP 런타임** | ~~`csp_listener`~~ | — | **파일 기반** — `{CimsRuntimeDir}/csp_listener/<id>.json` |
| | ~~`sip_trunk`~~ | — | **파일 기반** |
| | ~~`routing_rule (+match/transform)`~~ | — | **파일 기반** — match/transform 임베드 |
| | ~~`routing_access_list`~~ | — | **파일 기반** |
| | ~~`csp_config_audit`~~ | — | **파일 기반** — JSONL 시계열 (`csp_config_audit/audit/YYYY/MM/DD.jsonl`) |
| | ~~`sip_service (+sip_service_listener)`~~ | — | **파일 기반** — listeners 배열 임베드 |
| **구독↔서비스** | `volte_subscriptions.service_ref` / `voip_subscriptions.service_ref` / `ptt_subscriptions.service_ref` | migrate_subscriptions_service_ref.sql | VARCHAR(64) = `access_services.name`(file-store, FK 없음). 테이블 kind 와 같은 kind 의 레코드만(CSC 400 `service_kind_mismatch`) |
| **HA** | ~~`ha_groups`~~ | — | **파일 기반** — `{CimsRuntimeDir}/ha_groups/<id>.json` (members 배열 임베드) |
| | ~~`ha_group_members`~~ | — | (그룹 JSON 안에 임베드) |
| **에이전트/배포** | ~~`cims_agent`~~ | — | **파일 기반** — `{CimsRuntimeDir}/agents/<id>.json` |
| | ~~`cims_package`~~ | — | **파일 기반** — `{CimsRuntimeDir}/packages/<name>__<version>.json` |
| | ~~`agent_deployment`~~ | — | **파일 기반** — `{CimsRuntimeDir}/deployments/<id>.json` |
| | ~~`agent_job`~~ | — | **파일 기반** — `{CimsRuntimeDir}/jobs/<id>.json` |
| | ~~`agent_metric`~~ | — | **파일 기반** — `{CimsRuntimeDir}/metrics/<agent_id>/YYYY/MM/DD.jsonl` (시계열) |

### 주요 FK / 참조

- `users(id)` ← `volte_subscriptions.user_id`(`fk_voip_sub_user`), `voip_subscriptions.user_id`(`fk_voipsub_user`), `ptt_subscriptions.user_id`(`fk_ptt_sub_user`), `icb_identities.user_id`(`fk_icb_user`) (ON DELETE CASCADE)
- `ptt_subscriptions(id)` ← `ptt_user_profile.ptt_id`(`fk_pup_ptt_sub`), `ptt_user_profile.emergency_private_recipient`(`fk_pup_emg_priv`)
- `ptt_groups(id)` ← `ptt_group_members.group_id` (CASCADE) — **id=surrogate BIGINT**; `mcptt_group_id` 는 UNIQUE 식별자(키 아님)
- `ptt_groups(id)` ← `ptt_affiliations.group_id` (CASCADE)
- `ptt_groups(id)` ← `mcvideo_group_attrs.group_id`, `mcvideo_affiliations.group_id` (CASCADE) · `ptt_subscriptions(id)` ← `mcvideo_user_profile.ptt_id` (CASCADE)
- `phone_groups(id)` ← `phone_group_members.group_id`, `role_monitor_targets.phone_group_id` (CASCADE); `roles(id)` ← `role_assignments.role_id`, `role_monitor_targets.role_id`, `role_ptt_targets.role_id` (CASCADE); `ptt_groups(id)` ← `role_ptt_targets.ptt_group_id` (CASCADE); `organizations(id)` ← `phone_groups.org_id`, `roles.org_id` (SET NULL). `volte_subscriptions.pickup_group`·`voip_subscriptions.pickup_group`·`ptt_subscriptions.pickup_group` 은 FK 없이 값으로 `phone_groups.id` 를 담는다(person 단위 파생 — 멤버 제거·그룹 삭제 시 CSC 가 재계산해 NULL 로 되돌린다). 전환 전 이름 = `dispatch_groups` 계열(매핑은 dispatch_center.md §8.1 전환 표)
- 가입 테이블 `service_ref` 는 file-store `access_services.name` 을 값으로 가리킨다(FK 없음)
- `ha_groups(id)` ← `ha_group_members.group_id` (CASCADE)

## 3. 옛 테이블 / DROP 됨

| 테이블 | DROP 한 마이그레이션 | 대체 |
|---|---|---|
| `voip_call_logs` / `volte_call_logs` | migrate_drop_call_logs.sql | **파일 기반** — 녹취 영역 `volte/.../*.d/call.json`. `/api/v1/call/logs` 가 디렉토리 스캔 |
| `ptt_call_logs` | migrate_drop_call_logs.sql | 위와 동일 |
| `voip_call_participants` / `volte_call_participants` | migrate_drop_call_logs.sql | 파일 — `participants.jsonl` |
| `ptt_call_participants` | migrate_drop_call_logs.sql | 위와 동일 |
| `csp_listener_deprecated` | migrate_drop_deprecated_tables.sql | `sip_service_listener` |
| `sip_trunk_deprecated` | migrate_drop_deprecated_tables.sql | `sip_trunk` (rename) |
| `routing_rule_deprecated` | migrate_drop_deprecated_tables.sql | `routing_rule` (rename) |
| `routing_access_list_deprecated` | migrate_drop_deprecated_tables.sql | `routing_access_list` (rename) |
| `sip_service_deprecated` | migrate_drop_deprecated_tables.sql | `sip_service` (rename) |
| `verification_run` / `verification_run_item` | (마이그레이션 파일 없음 — 처음부터 미배포) | **파일 기반** — `verify_runs/YYYY/MM/<id>.json`. `verify.lib.run_store` |

## 4. 파일 기반 SOT (DB 미적재)

다음은 파일 시스템이 SoT 이며 DB 테이블이 **없음** — 이중화 DB 와 무관. 경로의 영역 루트(`Recording.Dir`·`ServiceLogging.Dir` …)는
사이트 디렉터리에서 유도된다([site_directory_layout.md](features/site_directory_layout.md)):

| 항목 | 경로 | 처리 |
|---|---|---|
| 통화 이력(VoLTE) | `{Recording.Dir}/volte/YYYY/MM/DD/HH/.../*.d/call.json` | 디렉토리 스캔 (OAM `flow_logger`, CSC 관제 이력 `dispatch_history`) |
| PTT 그룹 이력/녹취 | `{Recording.Dir}/ptt/{id}/{YYYY}/{MM}/{DD}/{HH}/` (id=ptt_groups.id surrogate, 시간버킷) — `group.json`(base) + `events/floor/segments.jsonl` + `seg/{NNN}/seg_NNNN_*`(100세그 shard) | 시간창 스캔. [recording.md](features/recording.md) |
| 참여자 | `.d/participants.jsonl` | call.json 와 동봉 |
| Session ↔ Call-ID 매핑 | `.d/session.json` | flow 재구성 |
| 그룹 SDS 메시지 | `{Recording.Dir}/message/{gid}/YYYY/MM/DD/HH/messages.jsonl` | 콘솔 oam-svc + 관제 `GET /provisioning/history?kind=message`(범위 게이트) |
| 1:1 SDS/SMS (관제 이력) | `{Recording.Dir}/message_direct/YYYY/MM/DD/HH/messages.jsonl` — `Setup.McData.StoreOneToOneSds` 시에만 | 관제 이력 조회 시 역할 `monitor_call` 게이트([mcdata_messaging.md §4.3](features/mcdata_messaging.md)) |
| SIP 메시지 | `{ServiceLogging.Dir}/sip/YYYY/MM/DD/HH/<sysid>_sip.msg.<mm5>.jsonl` (flow·타 인터페이스 msg 도 같은 시간 디렉터리) | call_id 별 grep, [flow_logging.md](features/flow_logging.md) |
| 검증 회차 | `verify_runs/YYYY/MM/<id>.json` | `verify.lib.run_store` |
| Alert 이력 | `{ServiceLogging.Dir}/alerts/YYYY/MM/DD.jsonl` | OAM `services/alert_log.py` |
| 녹취 데이터 | `.d/raw_*.rtp` / `seg_*.rtp` | recordings 테이블이 메타만 |

## 5. 알려진 정합성 이슈

(현재 미해결 없음)

## 6. 외부 이중화 DB 인계 체크리스트

- 문자셋: **utf8mb4 / utf8mb4_unicode_ci** (한글 가입자명 / 그룹명)
- 엔진: **InnoDB** (FK / 트랜잭션)
- 권한: CIMS CSC 가 사용하는 계정에 `SELECT/INSERT/UPDATE/DELETE/CREATE/ALTER/DROP/INDEX` 모두 필요 (마이그레이션 적용 위해 DDL 포함)
- 외부 DB 가 read replica 분리 운영하는 경우, CSC `CimsDatabase.Host` 는 **write endpoint** 를 향함 (현재 CSC 는 r/w 분리 미지원)
- 백업 권장: `users / *_subscriptions / ptt_group* / organizations / sip_service*` (런타임 설정), 나머지는 운영 이력
