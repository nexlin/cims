-- 당겨받기(call pickup) 그룹 축 (docs/design/features/volte_supplementary_services.md §5.1)
--  - pickup_group : 같은 값을 가진 가입자끼리 당겨받기 가능. 조직(users.org_id)과 독립인
--                   관제 픽업 그룹 키. NULL/빈 값 = 미지정 — CSP 는 org_id 로 폴백한다
--                   (기존 현장 무변경 전환기 호환).
--  - 반영 시점 : CSP 는 REGISTER 시 바인딩에 그룹을 스냅샷한다 — 변경은 다음 등록 갱신부터.
--  - 재실행 안전 (컬럼 존재 시 no-op). ha1/auth_scheme 과 같은 방식.

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions'
      AND COLUMN_NAME = 'pickup_group');
SET @sql := IF(@col = 0,
    'ALTER TABLE volte_subscriptions
        ADD COLUMN pickup_group VARCHAR(64) NULL DEFAULT NULL
            COMMENT ''당겨받기 그룹 키 — 같은 값끼리 픽업 가능. NULL=미지정(org_id 폴백)'' AFTER forward_id',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_subscriptions'
      AND COLUMN_NAME = 'pickup_group');
SET @sql := IF(@col = 0,
    'ALTER TABLE ptt_subscriptions
        ADD COLUMN pickup_group VARCHAR(64) NULL DEFAULT NULL
            COMMENT ''당겨받기 그룹 키 — 같은 값끼리 픽업 가능. NULL=미지정(org_id 폴백)'' AFTER forward_id',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
