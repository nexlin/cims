---
name: project-db-external
description: DB 이중화는 외부 DB 인프라에 위임 (Galera / Master-Master 직접 셋업 안 함). 우리는 사용 테이블 스키마만 깔끔히 유지하고 외부 DB 에 인계.
metadata: 
  node_type: memory
  type: project
  originSessionId: f5599863-8541-4e36-b24c-99a39bb2497f
---

# DB 이중화 — 외부 위임 + 가입자 외 파일 이전 결정 (2026-05-13)

## 결정
- CIMS 자체 DB 이중화 (Galera / Master-Master) **하지 않음**.
- 운영 환경에서 이미 이중화 처리된 외부 DB (MariaDB/MySQL 호환) 사용 예정.
- **2026-05-13 후속 결정**: 가입자 정보/상태 외 모든 데이터는 **파일 기반**으로 이전.
- 외부 DB 에는 가입자 도메인(users / *_subscriptions / user_rejects / ptt_group* / ptt_session_seq) 만 남도록 단계 마이그레이션.

**Why**: 외부 DB 인계 부담 최소화 + 배포/HA/런타임 설정은 컨테이너/볼륨 패러다임 친화적인 파일 단위가 더 자연스럽다는 사용자 판단.

**How to apply**:
- 신규 테이블 추가 금지 — 가입자 도메인이 아니면 file-store 로 시작.
- 마이그레이션 plan: [[../../work/cims/docs/design/runtime_store_design.md]] (`docs/design/runtime_store_design.md`) §1 Phase 1~9.
- 파일 레이아웃/Atomic 쓰기/ID 할당 규칙 = 같은 design doc §2~§5.
- handler 변경 패턴 = §8 (DB SELECT/INSERT/UPDATE/DELETE → file_store.load_all/load/save/delete).
- 마이그레이션 완료 도메인은 `_legacy` 로 rename 후 1 릴리스 뒤 DROP.
- 외부 DB 인계 시 본 문서 §6 체크리스트 (utf8mb4 / InnoDB / 권한) 전달.
- 마이그레이션 적용은 `for f in sql/migrate_*.sql; do mysql ... < $f; done` 순서.
- CSC 는 r/w 분리 미지원 — 외부 DB 가 read replica 운영하면 write endpoint 만 `CimsDatabase.Host` 에 지정.

## SoT
- 코드/마이그레이션 매핑 + 외부 위임 체크리스트: `docs/design/db_schema.md`
- 옛 csc.md §6 (`docs/design/modules/csc.md`) 는 stale — voip_* 옛 이름 + dropped call_logs 표 잔존. 시간 될 때 db_schema.md 참조하도록 갱신 필요.

## 현재 활성 테이블 (33개) — 도메인
- 가입자: users / volte_subscriptions / user_rejects / ptt_subscriptions (4)
- PTT: ptt_groups / ptt_group_members / ptt_session_seq (3)
- 조직: organizations (1)
- 인증: auth_codes / refresh_tokens (2)
- 녹취: recordings / recording_segments (2)
- 모니터링: stats_daily / stats_monthly / stats_yearly (3)
- CSP 런타임: csp_listener / sip_trunk / routing_rule(+match/transform) / routing_access_list / csp_config_audit / sip_service / sip_service_listener (9)
- HA: ha_groups / ha_group_members (2)
- 에이전트/배포: cims_instance / cims_agent / cims_package / agent_deployment / agent_job / agent_metric (6)

## 파일 기반 SoT (DB 미적재)
- 통화 이력 (call.json) / SIP 메시지 (sip.jsonl) / 검증 회차 (verify_runs/) / Alert 이력 (alerts/YYYY/MM/DD.jsonl) / 녹취 데이터 (raw_*.rtp)

## 알려진 정합성 이슈
- (해결됨 2026-05-13 — `2ac93c0` stats.py call_logs 의존 제거 → 파일 기반(call.json 스캔). `9dfadec` 잔존 stale docs 4건 정합화. db_schema.md §5 비어있음.)
