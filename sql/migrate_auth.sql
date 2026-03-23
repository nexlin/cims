-- ============================================================
-- CIMS Auth Migration
-- cims_users 에 password, role 컬럼 추가
-- 실행: mysql -u root -p cims < sql/migrate_auth.sql
-- ============================================================

USE cims;

ALTER TABLE cims_users
    ADD COLUMN IF NOT EXISTS password VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'SHA-256 해시' AFTER email,
    ADD COLUMN IF NOT EXISTS role ENUM('admin','user') NOT NULL DEFAULT 'user' COMMENT '권한' AFTER password;

ALTER TABLE cims_users
    MODIFY COLUMN email VARCHAR(255) NOT NULL DEFAULT '' COMMENT '로그인 이메일(아이디)';

-- email 고유 인덱스 (이미 있으면 스킵)
ALTER TABLE cims_users ADD UNIQUE INDEX IF NOT EXISTS uq_email (email);

-- 기본 admin 계정 (비밀번호: admin1234)
INSERT INTO cims_users (name, email, password, role, org_id, create_time, update_time)
VALUES ('관리자', 'admin', SHA2('admin1234', 256), 'admin', '', NOW(), NOW())
ON DUPLICATE KEY UPDATE role='admin';
