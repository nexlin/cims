# CIMS DB Schema — SSOT

> 외부 이중화 DB(MariaDB/MySQL 호환) 에 적재할 CIMS 테이블 인벤토리.
> 코드/마이그레이션 정합성의 단일 출처(SoT).
> 스키마 생성 = `sql/cims_schema.sql` + `sql/migrate_*.sql` 순차 적용.

## 1. 적용 순서 (신규 환경)

```bash
mysql -u root -p < sql/cims_schema.sql
# 이후 migrate_*.sql 을 파일명 정렬순으로 일괄 적용
for f in sql/migrate_*.sql; do mysql -u root -p cims < "$f"; done
```

`migrate_*.sql` 은 idempotent(`IF EXISTS` / `IF NOT EXISTS`) 로 작성. 이미 반영된 환경 재실행 시 NO-OP.

## 2. 현재 활성 테이블 (33개, 도메인별)

| 도메인 | 테이블 | 정의 파일 | 비고 |
|---|---|---|---|
| **가입자** | `users` | cims_schema.sql + migrate_add_email.sql | 개인 (name/email/org_id/details) |
| | `voip_subscriptions` → `volte_subscriptions` | cims_schema.sql + **migrate_voip_to_volte.sql** | VoLTE MSISDN, SIP 인증, dnd/forward (table 명 v3 부터 `volte_*`) |
| | `user_rejects` | cims_schema.sql | VoLTE 착신거부 목록 |
| | `ptt_subscriptions` | cims_schema.sql + migrate_auth.sql + migrate_auth_id_dropped.sql | MCPTT ID, IMPI 인증 |
| **PTT 그룹** | `ptt_groups` | cims_schema.sql + migrate_group_media.sql + migrate_ptt_groups_v2.sql | name/priority/encryption/emergency/video_enabled/org_code |
| | `ptt_group_members` | cims_schema.sql | (group_id, user_id) UNIQUE + priority |
| | `ptt_session_seq` (시퀀스) | migrate_ptt_session_seq.sql | PTT 세션 ID 발급 시퀀스 |
| **조직** | `organizations` | migrate_organizations.sql | code/name/parent_id 트리 |
| **인증** | `auth_codes` | migrate_idms_tables.sql | IdMS OAuth2 인증 코드 |
| | `refresh_tokens` | migrate_idms_tables.sql | JWT refresh token |
| **녹취** | `recordings` | migrate_recordings.sql | 통화 단위 메타 (raw/transcoding/ready) |
| | `recording_segments` | migrate_recordings.sql | seg_*.rtp 세그먼트 메타 |
| **모니터링** | `stats_daily` | migrate_monitoring.sql | 일간 KPI 집계 |
| | `stats_monthly` | migrate_monitoring.sql | 월간 |
| | `stats_yearly` | migrate_monitoring.sql | 연간 |
| **CSP 런타임** | `csp_listener` | migrate_csp_runtime_config.sql | (옛 표 = sip_service 로 마이그레이션 중) |
| | `sip_trunk` | migrate_csp_runtime_config.sql | IP-PBX trunk |
| | `routing_rule` | migrate_csp_runtime_config.sql | SIP routing 규칙 헤더 |
| | `routing_rule_match` | migrate_csp_runtime_config.sql | 조건 |
| | `routing_rule_transform` | migrate_csp_runtime_config.sql | 변환 |
| | `routing_access_list` | migrate_csp_runtime_config.sql | ACL |
| | `csp_config_audit` | migrate_csp_runtime_config.sql | 변경 audit |
| | `sip_service` | migrate_sip_service.sql | 신규 SIP 서비스 인스턴스 추상 |
| | `sip_service_listener` | migrate_sip_service.sql | service ↔ listener N:M |
| **구독↔서비스** | `voip_subscriptions.service_id` / `ptt_subscriptions.service_id` | migrate_subscriptions_service_ref.sql | FK → sip_service |
| **HA** | `ha_groups` | migrate_ha_groups.sql + migrate_ha_groups_vip_nullable.sql + migrate_ha_services_wiring.sql | A/S / AA 그룹 + VIP (nullable) |
| | `ha_group_members` | migrate_ha_groups.sql | 그룹 멤버 (agent_id) |
| **에이전트/배포** | `cims_instance` | migrate_agent_deployment.sql | 인스턴스 등록 |
| | `cims_agent` | migrate_agent_deployment.sql + migrate_agent_mtls_columns.sql + migrate_agent_sync_port.sql + migrate_agent_upgrade.sql | 에이전트 등록 (mTLS, sync_port) |
| | `cims_package` | migrate_agent_deployment.sql + migrate_package_config_template.sql | 패키지 manifest + config template |
| | `agent_deployment` | migrate_agent_deployment.sql | 배포 이력 |
| | `agent_job` | migrate_agent_deployment.sql + migrate_agent_job_types.sql | 작업 큐 |
| | `agent_metric` | migrate_agent_deployment.sql | 에이전트 metric |

### 주요 FK / 참조

- `users(id)` ← `voip_subscriptions.user_id`, `ptt_subscriptions.user_id` (ON DELETE CASCADE)
- `voip_subscriptions(id)` ← `user_rejects.subscription_id` (CASCADE)
- `ptt_groups(id)` ← `ptt_group_members.group_id` (CASCADE)
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
| 통화 이력 | `{ServiceLogDir}/{volte\|ptt}/YYYY/MM/DD/HH/.../*.d/call.json` | 디렉토리 스캔 (csc/handlers/call.py) |
| 참여자 | `.d/participants.jsonl` | call.json 와 동봉 |
| Session ↔ Call-ID 매핑 | `.d/session.json` | flow 재구성 |
| SIP 메시지 | `{MsgLogDir}/csp/sip/YYYY/MM/DD/HH/sip.jsonl` | call_id 별 grep |
| 검증 회차 | `verify_runs/YYYY/MM/<id>.json` | `verify.lib.run_store` |
| Alert 이력 | `{ServiceLogDir}/alerts/YYYY/MM/DD.jsonl` | `csc/services/alert_log.py` |
| 녹취 데이터 | `.d/raw_*.rtp` / `seg_*.rtp` | recordings 테이블이 메타만 |

## 5. 알려진 정합성 이슈

| 위치 | 이슈 | 처리 필요 |
|---|---|---|
| `docs/design/modules/csc.md` §6 | `voip_*` 옛 이름 + call_logs 표 잔존 | 본 문서 참조하도록 갱신 (별도 작업) |
| `docs/design/modules/csp.md` / `features/volte_flows.md` / `features/monitoring.md` | `voip_call_logs` / `ptt_call_logs` 표기 잔존 | 파일 기반(call.json) SoT 로 갱신 |

## 6. 외부 이중화 DB 인계 체크리스트

- 문자셋: **utf8mb4 / utf8mb4_unicode_ci** (한글 가입자명 / 그룹명)
- 엔진: **InnoDB** (FK / 트랜잭션)
- 권한: CIMS CSC 가 사용하는 계정에 `SELECT/INSERT/UPDATE/DELETE/CREATE/ALTER/DROP/INDEX` 모두 필요 (마이그레이션 적용 위해 DDL 포함)
- 외부 DB 가 read replica 분리 운영하는 경우, CSC `CimsDatabase.Host` 는 **write endpoint** 를 향함 (현재 CSC 는 r/w 분리 미지원)
- 백업 권장: `users / *_subscriptions / ptt_group* / organizations / sip_service*` (런타임 설정), 나머지는 운영 이력
