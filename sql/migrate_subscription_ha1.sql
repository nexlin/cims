-- SIP Digest 인증 자료 경계 재편 (docs/design/features/sip_access_security.md §4.2)
--  - ha1 = MD5(imsi@domain:realm:password) 를 인증 자료 SoT 로 둔다. CSP 는 ha1 우선,
--    비어 있으면 passwd 로 종전 계산(과도기 fallback).
--  - 기존 행의 ha1 일괄 계산은 realm(access_services.auth_realm ?? domain)이 OAM 스토어에
--    있어 SQL 만으로 못 한다 → sql/migrate_subscription_ha1.py 가 수행한다(멱등, ha1='' 행만).
--  - passwd 컬럼의 값 소거·DROP 은 전 조합 등록 회귀 후 후속 마이그레이션에서 한다.
--  - 재실행 안전 (컬럼 존재 시 no-op).

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions'
      AND COLUMN_NAME = 'ha1');
SET @sql := IF(@col = 0,
    'ALTER TABLE volte_subscriptions ADD COLUMN ha1 CHAR(32) NOT NULL DEFAULT ''''
        COMMENT ''SIP Digest H(A1)=MD5(imsi@domain:realm:password) — 인증 자료 SoT'' AFTER passwd',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_subscriptions'
      AND COLUMN_NAME = 'ha1');
SET @sql := IF(@col = 0,
    'ALTER TABLE ptt_subscriptions ADD COLUMN ha1 CHAR(32) NOT NULL DEFAULT ''''
        COMMENT ''SIP Digest H(A1)=MD5(imsi@domain:realm:password) — 인증 자료 SoT'' AFTER passwd',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
