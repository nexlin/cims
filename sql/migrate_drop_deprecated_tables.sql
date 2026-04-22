-- ============================================================
-- Phase D: CSP 런타임 설정 deprecated 테이블 DROP
--
--   migrate_deprecate_csp_runtime_tables.sql 로 `_deprecated` 접미사
--   붙여둔 5 개 테이블을 실제로 제거합니다.
--
--   전제: 코드베이스 전체에서 해당 테이블 이름이 참조되지 않는 것을
--         확인했습니다 (Phase D 조사 결과).
--         롤백이 필요하면 migrate_deprecate_csp_runtime_tables.sql 의
--         원본 CREATE 문 (git history) 참고.
--
-- 실행: sudo mysql cims < sql/migrate_drop_deprecated_tables.sql
-- ============================================================

USE cims;

DROP TABLE IF EXISTS csp_listener_deprecated;
DROP TABLE IF EXISTS sip_trunk_deprecated;
DROP TABLE IF EXISTS routing_rule_deprecated;
DROP TABLE IF EXISTS routing_access_list_deprecated;
DROP TABLE IF EXISTS sip_service_deprecated;

SELECT 'Migration complete: deprecated tables dropped' AS status,
       (SELECT COUNT(*) FROM information_schema.tables
        WHERE table_schema='cims' AND table_name LIKE '%_deprecated') AS remaining_deprecated;
