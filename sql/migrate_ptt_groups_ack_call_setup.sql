-- 확인 통화 설정(acknowledged call setup) — docs/design/features/mcptt_standard_conformance.md §C4c
--  (TS 24.379 §6.3.3.3 TNG1·§10.1.1.4.2 · TS 24.481 §7.2.2 s)t)u)·§7.2.4.2 <on-network-required>).
--  ① ptt_groups: min_number_to_start = <on-network-minimum-number-to-start>(개시자 200 OK 전에 받아야 할 멤버 200 수),
--     ack_timeout_sec = <on-network-timeout-for-acknowledgement-of-required-members>(TNG1 초),
--     ack_action = <on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>(proceed|abandon)
--  ② ptt_group_members: on_network_required = <entry> 의 <on-network-required>(필수 멤버)
--  재실행 안전(information_schema 확인). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해. 기본값 = 필수 멤버 없음·최소 0 —
--  적용 전과 같은 동작(개시자 200 OK 즉시)이다.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups' AND COLUMN_NAME = 'min_number_to_start');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_groups
       ADD COLUMN min_number_to_start INT NOT NULL DEFAULT 0
           COMMENT ''on-network-minimum-number-to-start (TS 24.481 §7.2.2 s) — 개시자 200 OK 전 멤버 200 수 (TS 24.379 §10.1.1.4.2)''
           AFTER max_duration_sec',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups' AND COLUMN_NAME = 'ack_timeout_sec');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_groups
       ADD COLUMN ack_timeout_sec INT NOT NULL DEFAULT 5
           COMMENT ''on-network-timeout-for-acknowledgement-of-required-members (TS 24.481 §7.2.2 t) — TNG1 초 (TS 24.379 §6.3.3.3)''
           AFTER min_number_to_start',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups' AND COLUMN_NAME = 'ack_action');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_groups
       ADD COLUMN ack_action ENUM(''proceed'',''abandon'') NOT NULL DEFAULT ''abandon''
           COMMENT ''on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members (TS 24.481 §7.2.2 u)''
           AFTER ack_timeout_sec',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_group_members' AND COLUMN_NAME = 'on_network_required');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_group_members
       ADD COLUMN on_network_required TINYINT(1) NOT NULL DEFAULT 0
           COMMENT ''<on-network-required> (TS 24.481 §7.2.4.2) — 필수 멤버: 개시자 200 OK 전 응답을 기다린다(TNG1)''
           AFTER mcptt_id',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
