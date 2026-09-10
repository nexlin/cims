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
--
-- 주의: 유선 VoIP 가입 테이블 분리 뒤 `voip_subscriptions` 는 **별개의 현행 테이블**이다(sql/migrate_voip_subscriptions.sql,
--   sip_service_model.md §2-9). 그래서 RENAME 은 volte_subscriptions 가 아직 없는(옛 이름만 있는) DB 에서만 실행되고,
--   volte_subscriptions 가 있으면 no-op 이다 — 재실행이 새 voip 테이블을 건드리지 않는다.

SET @db := DATABASE();

-- voip_subscriptions → volte_subscriptions (volte_subscriptions 가 없을 때만 — 있으면 voip_subscriptions 는 유선 VoIP 테이블)
SET @has := (SELECT COUNT(*) FROM information_schema.TABLES
             WHERE TABLE_SCHEMA=@db AND TABLE_NAME='voip_subscriptions');
SET @has_volte := (SELECT COUNT(*) FROM information_schema.TABLES
                   WHERE TABLE_SCHEMA=@db AND TABLE_NAME='volte_subscriptions');
SET @sql := IF(@has > 0 AND @has_volte = 0,
               'RENAME TABLE voip_subscriptions TO volte_subscriptions',
               'SELECT ''volte_subscriptions present — no rename (voip_subscriptions, if any, is the 유선 VoIP table)'' AS note');
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
