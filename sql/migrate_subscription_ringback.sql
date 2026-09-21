-- 가입자 단위 링백(컬러링) 음원 (docs/design/features/announcements.md §6.3)
--  - 피착신 가입자가 고른 음원을 발신자가 링백으로 듣는다. 값 = 안내 라이브러리 음원 id(`sys:<name>` | `op:<name>` | `sub:<name>`).
--  - NULL(기본) = 접속서비스 프로파일의 ringback 동작 그대로. 접속서비스 프로파일 ringback.mode 가 media 여야 뜻이 있다
--    (스위치는 서비스, 음원은 가입자). 실패 안내·보류 음악은 사업자 정책이라 가입자 단위로 두지 않는다.
--  - CSC POST/PUT /users/{pid}/{volte|voip} 의 `ringback_media` 가 쓰고, CSP 가 LoadAllUsers/SelectUser 로 읽는다(USER_CHANGED 반영).
--  - 재실행 안전 (컬럼 존재 시 no-op). voip_subscriptions 는 테이블이 있을 때만.

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions'
      AND COLUMN_NAME = 'ringback_media');
SET @sql := IF(@col = 0,
    'ALTER TABLE volte_subscriptions ADD COLUMN ringback_media VARCHAR(64) DEFAULT NULL
        COMMENT ''가입자 링백 음원 id(sys:|op:|sub: — announcements.md §6.3, NULL=서비스 프로파일)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @tbl := (SELECT COUNT(*) FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'voip_subscriptions');
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'voip_subscriptions'
      AND COLUMN_NAME = 'ringback_media');
SET @sql := IF(@tbl = 1 AND @col = 0,
    'ALTER TABLE voip_subscriptions ADD COLUMN ringback_media VARCHAR(64) DEFAULT NULL
        COMMENT ''가입자 링백 음원 id(sys:|op:|sub: — announcements.md §6.3, NULL=서비스 프로파일)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
