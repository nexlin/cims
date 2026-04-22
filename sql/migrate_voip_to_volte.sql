-- v3 (2026-04-22): 명명 통일 — voip_* → volte_*
--
-- 대상:
--   voip_subscriptions      → volte_subscriptions
--   voip_call_logs          → volte_call_logs
--   voip_call_participants  → volte_call_participants
--
-- 적용:
--   sudo mysql cims < sql/migrate_voip_to_volte.sql
--
-- FK 확인: voip_subscriptions.user_id → users.id (CASCADE). RENAME 으로 FK 유지.

SET @db := DATABASE();

-- voip_subscriptions → volte_subscriptions
SET @has := (SELECT COUNT(*) FROM information_schema.TABLES
             WHERE TABLE_SCHEMA=@db AND TABLE_NAME='voip_subscriptions');
SET @sql := IF(@has > 0,
               'RENAME TABLE voip_subscriptions TO volte_subscriptions',
               'SELECT ''voip_subscriptions already renamed'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- voip_call_logs → volte_call_logs
SET @has := (SELECT COUNT(*) FROM information_schema.TABLES
             WHERE TABLE_SCHEMA=@db AND TABLE_NAME='voip_call_logs');
SET @sql := IF(@has > 0,
               'RENAME TABLE voip_call_logs TO volte_call_logs',
               'SELECT ''voip_call_logs already renamed'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- voip_call_participants → volte_call_participants
SET @has := (SELECT COUNT(*) FROM information_schema.TABLES
             WHERE TABLE_SCHEMA=@db AND TABLE_NAME='voip_call_participants');
SET @sql := IF(@has > 0,
               'RENAME TABLE voip_call_participants TO volte_call_participants',
               'SELECT ''voip_call_participants already renamed'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT 'migrate_voip_to_volte applied' AS status;
SELECT TABLE_NAME FROM information_schema.TABLES
 WHERE TABLE_SCHEMA=@db AND (TABLE_NAME LIKE 'voip%' OR TABLE_NAME LIKE 'volte%')
 ORDER BY TABLE_NAME;
