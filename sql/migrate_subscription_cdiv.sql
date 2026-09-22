-- 조건부 착신전환 (TS 24.604 CDIV — volte_supplementary_services.md §6A.4)
--  - forward_id(CFU, 기존) 옆에 조건부 전환 대상을 둔다:
--      forward_busy_id           CFB  — 착신 leg 486/600(Q.850 17) 때
--      forward_no_reply_id       CFNR — 착신 leg 가 forward_no_reply_sec(0 = Setup.Sip.Cdiv.NoReplySec) 안에 응답하지 않을 때, 단말 480/408 도 포함
--      forward_not_logged_in_id  CFNL — 착신 가입자가 미등록일 때(INVITE 시점)
--    값 = 번호(선행 + 허용 — CSP 가 가입자 접속서비스 다이얼 플랜으로 +E.164 번역), '' = 전환 없음.
--  - CSC POST/PUT /users/{pid}/{volte|voip} 의 같은 이름 필드가 쓰고, CSP 가 LoadAllUsers/SelectUser 로 읽는다(USER_CHANGED 반영).
--  - 재실행 안전 (컬럼 존재 시 no-op). voip_subscriptions 는 테이블이 있을 때만. ptt 회선은 전화 경로가 아니라 대상 아님.

DELIMITER //
DROP PROCEDURE IF EXISTS cims_migrate_cdiv //
CREATE PROCEDURE cims_migrate_cdiv(IN tbl VARCHAR(64))
BEGIN
    DECLARE has_tbl INT;
    SELECT COUNT(*) INTO has_tbl FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl;
    IF has_tbl = 1 THEN
        IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = 'forward_busy_id') THEN
            SET @sql := CONCAT('ALTER TABLE ', tbl, ' ADD COLUMN forward_busy_id VARCHAR(64) NOT NULL DEFAULT '''' COMMENT ''CFB 전환 대상(TS 24.604)''');
            PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        END IF;
        IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = 'forward_no_reply_id') THEN
            SET @sql := CONCAT('ALTER TABLE ', tbl, ' ADD COLUMN forward_no_reply_id VARCHAR(64) NOT NULL DEFAULT '''' COMMENT ''CFNR 전환 대상(TS 24.604)''');
            PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        END IF;
        IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = 'forward_no_reply_sec') THEN
            SET @sql := CONCAT('ALTER TABLE ', tbl, ' ADD COLUMN forward_no_reply_sec INT NOT NULL DEFAULT 0 COMMENT ''CFNR 무응답 시한(초, 0 = Setup.Sip.Cdiv.NoReplySec)''');
            PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        END IF;
        IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = 'forward_not_logged_in_id') THEN
            SET @sql := CONCAT('ALTER TABLE ', tbl, ' ADD COLUMN forward_not_logged_in_id VARCHAR(64) NOT NULL DEFAULT '''' COMMENT ''CFNL 전환 대상(미등록, TS 24.604)''');
            PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        END IF;
    END IF;
END //
DELIMITER ;
CALL cims_migrate_cdiv('volte_subscriptions');
CALL cims_migrate_cdiv('voip_subscriptions');
DROP PROCEDURE IF EXISTS cims_migrate_cdiv;
