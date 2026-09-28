-- 착신 차단(ICB, Incoming Communication Barring — TS 24.611) 명칭 정렬 — docs/design/features/volte_supplementary_services.md §6B
--  CIMS 의 "DND(모든 착신 거절)"·"착신거부(지정 발신 번호 거절)"는 TS 24.611 ICB 의 두 규칙(무조건 / cp:identity)이다.
--  ① volte_subscriptions·voip_subscriptions: dnd → icb_all (ICB 전체 — 회선 단위)
--  ② ptt_subscriptions.dnd 제거 — MMTel ICB 는 MCPTT 대상이 아니다(쓰는 경로가 없었다)
--  ③ user_rejects(user_id, reject_id) → icb_identities(user_id, identity) + FK fk_reject_user → fk_icb_user (ICB 지정 번호 — 사람 단위)
--  재실행 안전(information_schema 확인 + PREPARE/EXECUTE). **이름 변경은 구 코드와 호환되지 않는다** — CSP·CSC·OAM·콘솔을 같은 정지창에서 올린다.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions' AND COLUMN_NAME = 'dnd');
SET @sql := IF(@have = 1,
    'ALTER TABLE volte_subscriptions CHANGE COLUMN dnd icb_all TINYINT(1) NOT NULL DEFAULT 0 COMMENT ''착신 차단 — 전체 (TS 24.611 ICB)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'voip_subscriptions' AND COLUMN_NAME = 'dnd');
SET @sql := IF(@have = 1,
    'ALTER TABLE voip_subscriptions CHANGE COLUMN dnd icb_all TINYINT(1) NOT NULL DEFAULT 0 COMMENT ''착신 차단 — 전체 (TS 24.611 ICB)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_subscriptions' AND COLUMN_NAME = 'dnd');
SET @sql := IF(@have = 1, 'ALTER TABLE ptt_subscriptions DROP COLUMN dnd', 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'user_rejects');
SET @sql := IF(@have = 1, 'ALTER TABLE user_rejects DROP FOREIGN KEY fk_reject_user', 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
SET @sql := IF(@have = 1, 'RENAME TABLE user_rejects TO icb_identities', 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
SET @sql := IF(@have = 1,
    'ALTER TABLE icb_identities
       CHANGE COLUMN reject_id `identity` VARCHAR(64) NOT NULL COMMENT ''차단할 발신 번호 (ICB cp:identity)'',
       ADD CONSTRAINT fk_icb_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,
       COMMENT = ''착신 차단 — 지정 번호 (TS 24.611 ICB cp:identity, 사람 단위)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
