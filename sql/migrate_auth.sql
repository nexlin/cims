-- ============================================================
-- CIMS Auth Migration (idempotent)
--   · users 에 login_id / password / role 컬럼 보장
--   · 레거시 email 컬럼이 남아 있으면 login_id 로 복사 후 DROP
--   · login_id UNIQUE 인덱스 추가
--   · 기본 admin 계정 삽입 (login_id='admin', password=SHA2('1234'))
--     → tests/test_env.json 의 admin/1234 와 일치
-- 실행: mysql -u root -p cims < sql/migrate_auth.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
-- 1) 필수 컬럼 보장
-- ─────────────────────────────────────────────
ALTER TABLE users
    ADD COLUMN IF NOT EXISTS login_id VARCHAR(255) NOT NULL DEFAULT '' COMMENT '로그인 ID' AFTER name,
    ADD COLUMN IF NOT EXISTS password VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'SHA-256 해시' AFTER login_id,
    ADD COLUMN IF NOT EXISTS role     ENUM('admin','user') NOT NULL DEFAULT 'user' COMMENT '권한' AFTER password;

-- ─────────────────────────────────────────────
-- 2) 레거시 email 컬럼 정리 (존재 시에만)
--    · email 값이 있고 login_id 가 비어 있으면 이관
--    · 이관 후 email 컬럼 DROP
-- ─────────────────────────────────────────────
SET @has_email := (
    SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'users' AND COLUMN_NAME = 'email'
);

SET @sql := IF(@has_email > 0,
    "UPDATE users SET login_id = email WHERE (login_id = '' OR login_id IS NULL) AND email <> ''",
    "DO 0");
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @sql := IF(@has_email > 0,
    "ALTER TABLE users DROP INDEX uq_email",
    "DO 0");
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @sql := IF(@has_email > 0,
    "ALTER TABLE users DROP COLUMN email",
    "DO 0");
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ─────────────────────────────────────────────
-- 3) login_id UNIQUE 인덱스
-- ─────────────────────────────────────────────
ALTER TABLE users ADD UNIQUE INDEX IF NOT EXISTS uq_login_id (login_id);

-- ─────────────────────────────────────────────
-- 4) 기본 admin 계정 (tests/test_env.json 과 일치)
--    비밀번호: 1234 (운영에선 즉시 교체)
-- ─────────────────────────────────────────────
INSERT INTO users (name, login_id, password, role, org_id, create_time, update_time)
VALUES ('관리자', 'admin', SHA2('1234', 256), 'admin', '', NOW(), NOW())
ON DUPLICATE KEY UPDATE
    role     = 'admin',
    password = SHA2('1234', 256);

SELECT 'Auth migration complete: login_id + password + admin(admin/1234)' AS status;
