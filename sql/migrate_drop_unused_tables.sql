-- ============================================================
--  migrate_drop_unused_tables.sql  (2026-06-15)
--  전수 조사 결과 코드에서 SQL 로 사용되지 않는(file_store 이관 / 폐기 / 미사용)
--  테이블 16개 정리. 모두 0 rows 확인 후 작성 — 데이터 손실 없음.
--
--  분류:
--    file_store 이관: auth_codes, refresh_tokens(IDMS→idms_storage),
--      ha_groups, sip_service, routing_rule_match, routing_rule_transform(csp_runtime),
--      volte_call_logs, volte_call_participants(→call.json/participants.jsonl),
--      recordings, recording_segments(→NFS .d 녹취), csp_config_audit(→JSONL)
--    _deprecated 표식: routing_access_list_deprecated, routing_rule_deprecated, sip_trunk_deprecated
--    미사용: cims_instance, ptt_user_profile
--
--  적용:  mysql -u root -p cims < migrate_drop_unused_tables.sql
--  (실행 전 백업 권장:  mysqldump cims > cims_backup.sql)
-- ============================================================

USE cims;

SET FOREIGN_KEY_CHECKS = 0;

DROP TABLE IF EXISTS auth_codes;
DROP TABLE IF EXISTS refresh_tokens;
DROP TABLE IF EXISTS ha_groups;
DROP TABLE IF EXISTS sip_service;
DROP TABLE IF EXISTS routing_rule_match;
DROP TABLE IF EXISTS routing_rule_transform;
DROP TABLE IF EXISTS volte_call_logs;
DROP TABLE IF EXISTS volte_call_participants;
DROP TABLE IF EXISTS recordings;
DROP TABLE IF EXISTS recording_segments;
DROP TABLE IF EXISTS csp_config_audit;
DROP TABLE IF EXISTS routing_access_list_deprecated;
DROP TABLE IF EXISTS routing_rule_deprecated;
DROP TABLE IF EXISTS sip_trunk_deprecated;
DROP TABLE IF EXISTS cims_instance;
DROP TABLE IF EXISTS ptt_user_profile;

SET FOREIGN_KEY_CHECKS = 1;
