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

> **규칙:** 신규 데이터는 DB 테이블을 새로 만들지 않고 file-store(collection/jsonl)로 시작한다. DB 는 가입자(person/VoLTE/PTT) 도메인 등 관계형이 본질적으로 필요한 데이터에 한정한다 → [runtime_store_design.md](runtime_store_design.md).
>
> 취소선(~~table~~) 항목은 DB 테이블 없이 파일 기반(file_store)으로 운영되는 도메인.

| 도메인 | 테이블 | 정의 파일 | 비고 |
|---|---|---|---|
| **가입자** | `users` | cims_schema.sql + migrate_add_email.sql + migrate_users_title.sql | 개인 (name/email/org_id/title/details) — `title`=직함, GMS 그룹문서 `cims:user-title` 로 UE 전달 |
| | `volte_subscriptions` | cims_schema.sql + migrate_voip_to_volte.sql + migrate_subscription_transport.sql + migrate_subscription_ha1.sql + migrate_subscription_aka.sql | VoLTE MSISDN, SIP 인증(`ha1` SoT, `auth_scheme`/AKA 자료(`k_enc`/`opc_enc`/`sqn`/`amf`), `sip_transport` 채널 정책), dnd/forward |
| | `user_rejects` | cims_schema.sql | VoLTE 착신거부 목록 |
| | `ptt_subscriptions` | cims_schema.sql + migrate_auth.sql + migrate_auth_id_dropped.sql + migrate_subscription_transport.sql + migrate_subscription_ha1.sql + migrate_subscription_aka.sql | MCPTT ID, IMPI 인증(`ha1` SoT, `auth_scheme`/AKA 자료, `sip_transport` 채널 정책) |
| | `ptt_user_profile` | cims_schema.sql + migrate_ptt_user_profile_v2.sql + migrate_ptt_user_profile_v3.sql + migrate_ptt_ambient_listening.sql | 사용자 MCPTT 프로파일(TS 24.484) — SOS 대상 결정 모드/전용 긴급그룹·개시 인가 3종 ([mcptt_emergency_modes.md](features/mcptt_emergency_modes.md) §2), 긴급 사설콜, `allow_ambient_listening`(원격 청취 자격 — [dispatch_center.md](features/dispatch_center.md) §5.6, 기본 0) |
| | `volte_subscriptions.pickup_group` / `ptt_subscriptions.pickup_group` | migrate_subscription_pickup_group.sql | 당겨받기 그룹 축(NULL=org 폴백). 관제 그룹 소속 가입자는 값이 `dispatch_groups.id`(`dg-…`)로 **파생**된다(CSC 단일 쓰기 주체, 직접 편집 409) |
| **관제 그룹** | `dispatch_groups` | cims_schema.sql + migrate_dispatch_groups.sql | 관제 그룹(픽업 그룹+대표번호+감청 범위, [dispatch_center.md](features/dispatch_center.md) §3·§8.1) — `id`(VARCHAR(64) 불변 키 `dg-xxxxxxxx`), name, `pilot_id`(UNIQUE, 대표번호), `service_ref`, `alert_mode`(parallel/sequential), `no_answer_sec`, `busy_members`(skip/alert), `overflow_target`, `monitor_scope`(none/own/listed/all), `ptt_listen`(none/listed/all), `listen_visibility`(hidden/visible), `org_id`(FK organizations SET NULL) |
| | `dispatch_group_members` | migrate_dispatch_groups.sql | `user_id` **PK**(가입자당 그룹 하나), `group_id`(FK CASCADE), `alert_order`(sequential 호출·포크 상한 절삭 순) |
| | `dispatch_group_monitor_targets` | migrate_dispatch_groups.sql | (`group_id`, `target_group_id`) — `monitor_scope=listed` 의 감청 대상 그룹 |
| | `dispatch_group_ptt_targets` | migrate_dispatch_groups.sql | (`group_id`, `ptt_group_id`=**ptt_groups.id surrogate** FK CASCADE) — `ptt_listen=listed` 대상. CSP 는 적재 시 `mcptt_group_id` 로 해석 |
| | `mcptt_service_config` | cims_schema.sql + migrate_mcptt_service_config.sql | MCPTT **시스템 전역** 서비스 설정(TS 24.484 service-config) — **단일 행 id=1**. 1:1/긴급/경보/발언요청/그룹생성 허용 + N2 상한. 편집=`PUT /api/v1/mcptt/service-config`(콘솔 구성>MCPTT 정책), 단말이 user-profile 인가와 AND 로 게이트 |
| | `users.login_id/password/role` | migrate_auth.sql | 콘솔 인증(가입자와 동일 신원). `role` RBAC ([mcptt_authorization.md](features/mcptt_authorization.md)) |
| **PTT 그룹** | `ptt_groups` | cims_schema.sql + migrate_ptt_groups_v2.sql + migrate_ptt_groups_v3_3gpp.sql + migrate_ptt_group_conference_state.sql | **id=surrogate BIGINT AI(PK, 디렉터리/FK 키)**, `mcptt_group_id`(UNIQUE 식별자), name/priority/encryption/emergency/video_enabled/org_code, **group_type(prearranged/chat/broadcast)/on_network/max_members/require_affiliation/alias/icon_url** (3GPP), `allow_conference_state`(on-network-allow-conference-state — 멤버의 conference 구독 허용, 기본 1) |
| | `ptt_group_members` | cims_schema.sql + migrate_ptt_groups_v3_3gpp.sql | group_id=**surrogate ptt_groups.id(BIGINT FK)**, user_id, priority, **role(chair/participant), mcptt_id** |
| | `ptt_affiliations` | migrate_ptt_groups_v3_3gpp.sql | MCPTT affiliation(TS 24.379 §9): (group_id, user_id, client_id) + affiliated_at/expires_at/status |
| | `ptt_session_seq` (시퀀스) | migrate_ptt_session_seq.sql | PTT 세션 ID 발급 시퀀스 |
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
| **구독↔서비스** | `voip_subscriptions.service_id` / `ptt_subscriptions.service_id` | migrate_subscriptions_service_ref.sql | FK → sip_service |
| **HA** | ~~`ha_groups`~~ | — | **파일 기반** — `{CimsRuntimeDir}/ha_groups/<id>.json` (members 배열 임베드) |
| | ~~`ha_group_members`~~ | — | (그룹 JSON 안에 임베드) |
| **에이전트/배포** | ~~`cims_agent`~~ | — | **파일 기반** — `{CimsRuntimeDir}/agents/<id>.json` |
| | ~~`cims_package`~~ | — | **파일 기반** — `{CimsRuntimeDir}/packages/<name>__<version>.json` |
| | ~~`agent_deployment`~~ | — | **파일 기반** — `{CimsRuntimeDir}/deployments/<id>.json` |
| | ~~`agent_job`~~ | — | **파일 기반** — `{CimsRuntimeDir}/jobs/<id>.json` |
| | ~~`agent_metric`~~ | — | **파일 기반** — `{CimsRuntimeDir}/metrics/<agent_id>/YYYY/MM/DD.jsonl` (시계열) |

### 주요 FK / 참조

- `users(id)` ← `voip_subscriptions.user_id`, `ptt_subscriptions.user_id` (ON DELETE CASCADE)
- `voip_subscriptions(id)` ← `user_rejects.subscription_id` (CASCADE)
- `ptt_groups(id)` ← `ptt_group_members.group_id` (CASCADE) — **id=surrogate BIGINT**; `mcptt_group_id` 는 UNIQUE 식별자(키 아님)
- `ptt_groups(id)` ← `ptt_affiliations.group_id` (CASCADE)
- `dispatch_groups(id)` ← `dispatch_group_members.group_id`, `dispatch_group_monitor_targets.{group_id,target_group_id}`, `dispatch_group_ptt_targets.group_id` (CASCADE); `ptt_groups(id)` ← `dispatch_group_ptt_targets.ptt_group_id` (CASCADE); `organizations(id)` ← `dispatch_groups.org_id` (SET NULL). `volte_subscriptions.pickup_group` 은 FK 없이 값으로 `dispatch_groups.id` 를 담는다(파생 — 삭제 시 CSC 가 NULL 로 되돌린다)
- `sip_service(id)` ← `voip_subscriptions.service_id`, `ptt_subscriptions.service_id` (NULLABLE)
- `ha_groups(id)` ← `ha_group_members.group_id` (CASCADE)

## 3. 옛 테이블 / DROP 됨

| 테이블 | DROP 한 마이그레이션 | 대체 |
|---|---|---|
| `voip_call_logs` / `volte_call_logs` | migrate_drop_call_logs.sql | **파일 기반** — `service_log/volte/.../*.d/call.json`. `/api/v1/call/logs` 가 디렉토리 스캔 |
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

다음은 파일 시스템이 SoT 이며 DB 테이블이 **없음** — 이중화 DB 와 무관:

| 항목 | 경로 | 처리 |
|---|---|---|
| 통화 이력(VoLTE) | `{ServiceLogDir}/volte/YYYY/MM/DD/HH/.../*.d/call.json` | 디렉토리 스캔 (csc/handlers/call.py) |
| PTT 그룹 이력/녹취 | `{ServiceLogDir}/ptt/{id}/{YYYY}/{MM}/{DD}/{HH}/` (id=ptt_groups.id surrogate, 시간버킷) — `group.json`(base) + `events/floor/segments.jsonl` + `seg/{NNN}/seg_NNNN_*`(100세그 shard) | 시간창 스캔. [recording.md](features/recording.md) |
| 참여자 | `.d/participants.jsonl` | call.json 와 동봉 |
| Session ↔ Call-ID 매핑 | `.d/session.json` | flow 재구성 |
| 그룹 SDS 메시지 | `{ServiceLogDir}/message/{gid}/YYYY/MM/DD/HH/messages.jsonl` | 콘솔 oam-svc + 관제 `GET /provisioning/history?kind=message`(범위 게이트) |
| 1:1 SDS/SMS (관제 이력) | `{ServiceLogDir}/message_direct/YYYY/MM/DD/HH/messages.jsonl` — `Setup.McData.StoreOneToOneSds` 시에만 | 관제 이력 조회 시 `monitor_scope` 게이트([mcdata_messaging.md §4.3](features/mcdata_messaging.md)) |
| SIP 메시지 | `{MsgLogDir}/csp/sip/YYYY/MM/DD/HH/sip.jsonl` | call_id 별 grep |
| 검증 회차 | `verify_runs/YYYY/MM/<id>.json` | `verify.lib.run_store` |
| Alert 이력 | `{ServiceLogDir}/alerts/YYYY/MM/DD.jsonl` | `csc/services/alert_log.py` |
| 녹취 데이터 | `.d/raw_*.rtp` / `seg_*.rtp` | recordings 테이블이 메타만 |

## 5. 알려진 정합성 이슈

(현재 미해결 없음)

## 6. 외부 이중화 DB 인계 체크리스트

- 문자셋: **utf8mb4 / utf8mb4_unicode_ci** (한글 가입자명 / 그룹명)
- 엔진: **InnoDB** (FK / 트랜잭션)
- 권한: CIMS CSC 가 사용하는 계정에 `SELECT/INSERT/UPDATE/DELETE/CREATE/ALTER/DROP/INDEX` 모두 필요 (마이그레이션 적용 위해 DDL 포함)
- 외부 DB 가 read replica 분리 운영하는 경우, CSC `CimsDatabase.Host` 는 **write endpoint** 를 향함 (현재 CSC 는 r/w 분리 미지원)
- 백업 권장: `users / *_subscriptions / ptt_group* / organizations / sip_service*` (런타임 설정), 나머지는 운영 이력
