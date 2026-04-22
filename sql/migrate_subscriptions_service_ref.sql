-- v3 (2026-04-22): subscriptions.service_id INT → service_ref VARCHAR(64)
--
-- 배경:
--   v3 재구조화로 sip_service 테이블 폐기, access_services.jsonl 이 SOT.
--   access_services 의 식별자는 UUID 문자열 (name 은 unique) — 기존 INT service_id 와
--   네임스페이스 충돌. subscriptions 가 jsonl 의 name 을 참조하도록 전환.
--
-- 적용:
--   sudo mysql cims < sql/migrate_subscriptions_service_ref.sql
--
-- 변경:
--   voip_subscriptions: DROP service_id, ADD service_ref VARCHAR(64)
--   ptt_subscriptions : DROP service_id, ADD service_ref VARCHAR(64)
--
-- 기존 데이터 보존:
--   service_id 를 쓰는 레코드의 service_ref 는 NULL 로 시작. 운영자가 access_services 생성 후
--   수동 UPDATE 또는 verify 스크립트/seed 가 기본값 (예: 'voip-default' / 'ptt-default') 주입.

SET @db := DATABASE();

-- 아래 drop 절차는 FK → 인덱스 → 컬럼 순:
--   1) FK 제약 drop (sip_service_deprecated 참조)
--   2) 서비스 인덱스 drop
--   3) service_id 컬럼 drop
--   4) service_ref 추가

-- v3 (2026-04-22): 테이블명이 voip_subscriptions 또는 volte_subscriptions 일 수 있음 (rename 순서 유동).
--   이 migration 은 volte_subscriptions 기준이지만, 구 이름에도 안전하도록 처리.
SET @volte_tbl := (SELECT IF(COUNT(*) > 0, 'volte_subscriptions', 'voip_subscriptions')
                   FROM information_schema.TABLES
                   WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'volte_subscriptions');

-- volte/voip_subscriptions
SET @has_fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = @volte_tbl
                  AND CONSTRAINT_NAME = 'fk_voip_service');
SET @sql := IF(@has_fk > 0,
               CONCAT('ALTER TABLE ', @volte_tbl, ' DROP FOREIGN KEY fk_voip_service'),
               'SELECT ''no fk_voip_service'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA = @db AND TABLE_NAME = @volte_tbl
                   AND INDEX_NAME = 'idx_voip_service');
SET @sql := IF(@has_idx > 0,
               CONCAT('ALTER TABLE ', @volte_tbl, ' DROP INDEX idx_voip_service'),
               'SELECT ''no idx_voip_service'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA = @db AND TABLE_NAME = @volte_tbl
                   AND COLUMN_NAME = 'service_id');
SET @sql := IF(@has_col > 0,
               CONCAT('ALTER TABLE ', @volte_tbl, ' DROP COLUMN service_id'),
               CONCAT('SELECT ''', @volte_tbl, '.service_id already removed'' AS note'));
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA = @db AND TABLE_NAME = @volte_tbl
                   AND COLUMN_NAME = 'service_ref');
SET @sql := IF(@has_col > 0,
               'SELECT ''service_ref already exists'' AS note',
               CONCAT('ALTER TABLE ', @volte_tbl, ' ADD COLUMN service_ref VARCHAR(64) DEFAULT NULL AFTER imsi'));
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ptt_subscriptions
SET @has_fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'ptt_subscriptions'
                  AND CONSTRAINT_NAME = 'fk_ptt_service');
SET @sql := IF(@has_fk > 0,
               'ALTER TABLE ptt_subscriptions DROP FOREIGN KEY fk_ptt_service',
               'SELECT ''no fk_ptt_service'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'ptt_subscriptions'
                   AND INDEX_NAME = 'idx_ptt_service');
SET @sql := IF(@has_idx > 0,
               'ALTER TABLE ptt_subscriptions DROP INDEX idx_ptt_service',
               'SELECT ''no idx_ptt_service'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'ptt_subscriptions'
                   AND COLUMN_NAME = 'service_id');
SET @sql := IF(@has_col > 0,
               'ALTER TABLE ptt_subscriptions DROP COLUMN service_id',
               'SELECT ''ptt_subscriptions.service_id already removed'' AS note');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

ALTER TABLE ptt_subscriptions
    ADD COLUMN IF NOT EXISTS service_ref VARCHAR(64) DEFAULT NULL AFTER imsi;

-- Seed: 가입자 전체에 기본 service_ref 할당 (access_services.jsonl 의 name 과 동일)
-- v3 (2026-04-22): volte / ptt 로 통일.
UPDATE volte_subscriptions SET service_ref = 'volte' WHERE service_ref IS NULL OR service_ref IN ('voip-default','volte-default');
UPDATE ptt_subscriptions   SET service_ref = 'ptt'   WHERE service_ref IS NULL OR service_ref = 'ptt-default';

SELECT 'migrate_subscriptions_service_ref applied' AS status;
SELECT COUNT(*) AS voip_rows, COUNT(service_ref) AS voip_bound FROM voip_subscriptions;
SELECT COUNT(*) AS ptt_rows,  COUNT(service_ref) AS ptt_bound  FROM ptt_subscriptions;
