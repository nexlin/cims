-- ============================================================
-- Migration: add email column to cims_users
-- 실행: sudo mysql cims < migrate_add_email.sql
-- ============================================================

ALTER TABLE cims_users
  ADD COLUMN email VARCHAR(255) NOT NULL DEFAULT '' COMMENT '이메일' AFTER name,
  MODIFY COLUMN name VARCHAR(128) NOT NULL COMMENT '표시 이름';

-- cims 계정에 ALTER 권한 부여 (향후 마이그레이션 편의를 위해)
GRANT ALTER ON cims.* TO 'cims'@'localhost';
FLUSH PRIVILEGES;

SELECT 'Migration complete. email column added to cims_users.' AS result;
