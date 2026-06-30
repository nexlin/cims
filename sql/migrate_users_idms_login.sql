-- 단말/IdMS 로그인 자격을 users(person)에 추가 — CIMS 로그인 ID 와 MCPTT ID 분리.
--   login_id  : 사용자가 입력하는 로그인 ID (예: test001). IdMS 인증 키.
--   passwd    : 로그인 비밀번호.
-- MCPTT ID(규격 신원)는 별도 컬럼 없이 가입자 msisdn 에서 tel:+<msisdn> 으로 파생한다.
-- (콘솔 admin 계정과는 무관 — 그건 OAM file_store console_accounts.)
--
-- 멱등: 컬럼/인덱스 존재 시 건너뛰도록 information_schema 가드.

SET @has_login := (SELECT COUNT(*) FROM information_schema.columns
                   WHERE table_schema=DATABASE() AND table_name='users' AND column_name='login_id');
SET @sql := IF(@has_login=0,
  'ALTER TABLE users ADD COLUMN login_id VARCHAR(64) NULL AFTER name, ADD COLUMN passwd VARCHAR(128) NULL AFTER login_id',
  'SELECT 1');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- 기존 가입자 마이그레이션: login_id = name(테스트NNN→testNNN), 기본 비번 1234.
UPDATE users
   SET login_id = REPLACE(name, '테스트', 'test'),
       passwd   = COALESCE(NULLIF(passwd, ''), '1234')
 WHERE login_id IS NULL OR login_id = '';

SET @has_uk := (SELECT COUNT(*) FROM information_schema.statistics
                WHERE table_schema=DATABASE() AND table_name='users' AND index_name='uk_users_login_id');
SET @sql := IF(@has_uk=0, 'ALTER TABLE users ADD UNIQUE KEY uk_users_login_id (login_id)', 'SELECT 1');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;
