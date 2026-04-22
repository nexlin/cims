-- ============================================================
-- P10.3: CSP 런타임 설정 테이블 deprecation
--
--   CSP 는 이제 agent 가 관리하는 install_path/config/*.jsonl 에서
--   listener/trunk/route/acl/service 를 읽습니다 (jsonl 모드).
--
--   아래 테이블들은 더 이상 CSP 에 의해 소비되지 않습니다.
--   안전을 위해 즉시 DROP 하지 않고 `_deprecated` 접미사로 rename 하여
--   롤백 여지를 남깁니다. 충분한 운영 기간 후 DROP 하세요.
--
--   실행 시점: 모든 CSP 가 ConfigJsonlDir 설정으로 전환된 후
--
-- 실행: sudo mysql cims < sql/migrate_deprecate_csp_runtime_tables.sql
-- ============================================================

USE cims;

-- 각 테이블을 _deprecated 로 rename (IF EXISTS 를 위해 조건부)
DROP PROCEDURE IF EXISTS _deprecate_table;
DELIMITER $$
CREATE PROCEDURE _deprecate_table(IN tbl VARCHAR(128))
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.tables
               WHERE table_schema='cims' AND table_name=tbl) THEN
        SET @sql = CONCAT('RENAME TABLE `', tbl, '` TO `', tbl, '_deprecated`');
        PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;
    END IF;
END$$
DELIMITER ;

CALL _deprecate_table('csp_listener');
CALL _deprecate_table('sip_trunk');
CALL _deprecate_table('routing_rule');
CALL _deprecate_table('routing_access_list');
CALL _deprecate_table('sip_service');

DROP PROCEDURE _deprecate_table;

-- CSC 도 해당 테이블 참조 엔드포인트(/api/v1/csp/listeners 등) 를
-- 이미 제거했다면 CscInterface(UDP 4421) 도 사용 빈도가 0 일 것.
-- 해당 경로는 CSP 의 jsonl 모드에서 동작하지 않음 — 추후 완전 제거 예정.

SELECT 'Migration complete: 5 tables renamed to *_deprecated' AS status,
       (SELECT COUNT(*) FROM information_schema.tables
        WHERE table_schema='cims' AND table_name LIKE '%_deprecated') AS deprecated_count;

-- ──────────────────────────────────────────────────────────────
-- 최종 DROP 은 별도로 (충분한 기간 경과 후):
--
--   DROP TABLE IF EXISTS csp_listener_deprecated;
--   DROP TABLE IF EXISTS sip_trunk_deprecated;
--   DROP TABLE IF EXISTS routing_rule_deprecated;
--   DROP TABLE IF EXISTS routing_access_list_deprecated;
--   DROP TABLE IF EXISTS sip_service_deprecated;
-- ──────────────────────────────────────────────────────────────
