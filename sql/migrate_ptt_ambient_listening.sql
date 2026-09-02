-- 사용자 MCPTT 프로파일 — 원격 청취 자격 (docs/design/features/dispatch_center.md §5.6)
--  allow_ambient_listening: allow-ambient-listening (TS 24.484 ruleset / TS 24.379 ambient listening
--  인가) — 이 사용자가 PTT 그룹콜 청취(recv_only 합류)·원격 청취를 수행할 자격. 관제사에게만 부여
--  (기본 0). 범위는 관제 그룹 ptt_listen, 편입 승인은 users.role manager 가 콘솔에서 한다(2단 인가).
--  재실행 안전(컬럼 존재 시 no-op). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_ambient_listening');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_ambient_listening TINYINT(1) NOT NULL DEFAULT 0
           COMMENT ''allow-ambient-listening (TS 24.484 ruleset) — 원격 청취 수행 자격 (관제사)''
           AFTER emergency_private_recipient',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
