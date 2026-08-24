-- SIP Digest 평문 passwd 컬럼 DROP (docs/design/features/sip_access_security.md §4.7 ⑥)
--  - 전제(배포 순서 계약 §4.7 ①~⑤): 전 행 ha1 채움 + 전 조합 등록 회귀 + passwd 값 소거가 끝난 뒤,
--    passwd 컬럼을 읽지 않는 코드(CSP SELECT·cspsim -db·CSC)가 배포된 **후속 릴리스**에서 적용한다.
--    이 릴리스의 CSP 는 아직 SELECT 목록에 passwd 가 있어 DROP 하면 기동 질의가 깨진다.
--  - 값 소거(⑤-c)는 되돌릴 수 없으므로 별도 절차로 선행한다:
--      UPDATE volte_subscriptions SET passwd='' WHERE passwd<>'';
--      UPDATE ptt_subscriptions   SET passwd='' WHERE passwd<>'';
--  - 재실행 안전 (컬럼 부재 시 no-op).

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions'
      AND COLUMN_NAME = 'passwd');
SET @sql := IF(@col = 1,
    'ALTER TABLE volte_subscriptions DROP COLUMN passwd',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_subscriptions'
      AND COLUMN_NAME = 'passwd');
SET @sql := IF(@col = 1,
    'ALTER TABLE ptt_subscriptions DROP COLUMN passwd',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
