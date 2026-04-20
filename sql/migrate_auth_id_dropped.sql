-- ============================================================
-- auth_id 컬럼 제거 (P8 — IMSI 정규화 완료)
--
-- 전제:
--   - migrate_sip_service.sql 이 먼저 적용되어 imsi/service_id 가 backfill 됨
--   - 이 스크립트는 모든 가입자가 imsi 를 갖도록 최종 정렬 후 auth_id 제거
--
-- Usage: sudo mysql cims < sql/migrate_auth_id_dropped.sql
-- ============================================================

USE cims;

-- 1. imsi 가 NULL 인 row 중 auth_id 가 'user@domain' 형태면 user 부분을 imsi 로
UPDATE voip_subscriptions
   SET imsi = SUBSTRING_INDEX(auth_id, '@', 1)
 WHERE imsi IS NULL AND auth_id LIKE '%@%';

UPDATE ptt_subscriptions
   SET imsi = SUBSTRING_INDEX(auth_id, '@', 1)
 WHERE imsi IS NULL AND auth_id LIKE '%@%';

-- 2. 그래도 imsi 가 NULL 이면 auth_id 를 그대로 사용
UPDATE voip_subscriptions SET imsi = auth_id WHERE imsi IS NULL;
UPDATE ptt_subscriptions  SET imsi = auth_id WHERE imsi IS NULL;

-- 3. 확인
SELECT 'voip' AS tbl, COUNT(*) AS total,
       SUM(imsi IS NOT NULL) AS imsi_set,
       SUM(service_id IS NOT NULL) AS service_set
  FROM voip_subscriptions
UNION ALL
SELECT 'ptt', COUNT(*), SUM(imsi IS NOT NULL), SUM(service_id IS NOT NULL)
  FROM ptt_subscriptions;

-- 4. auth_id 컬럼 DROP
ALTER TABLE voip_subscriptions DROP COLUMN IF EXISTS auth_id;
ALTER TABLE ptt_subscriptions  DROP COLUMN IF EXISTS auth_id;

-- 5. imsi 에 NOT NULL 제약 (가입자 추가 시 반드시 지정)
ALTER TABLE voip_subscriptions MODIFY COLUMN imsi VARCHAR(32) NOT NULL;
ALTER TABLE ptt_subscriptions  MODIFY COLUMN imsi VARCHAR(32) NOT NULL;

SHOW COLUMNS FROM voip_subscriptions;
