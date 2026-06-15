-- ============================================================
--  migrate_users_person_only.sql  (2026-06-15)
--  users 테이블을 가입자(person) 전용으로 환원 — 콘솔 로그인 필드 제거.
--  콘솔 로그인 계정은 OAM file_store 도메인 console_accounts 로 이관됨.
--
--  ⚠️ 선행: 콘솔 로그인 가능 계정(role IN admin/manager/operator/monitor)이
--     DB users 에 있다면, 컬럼 삭제 전에 console_accounts(file) 로 내보내야 한다.
--     아래 SELECT 로 대상 확인 → 있으면 sql/export_console_accounts.py 로 이관.
--       SELECT id, login_id, name, role FROM users
--        WHERE role <> 'user' AND login_id <> '';
--     (가입자(role=user)의 login_id/password 는 콘솔 미사용이므로 그대로 폐기.)
--
--  적용:  mysql -u root -p cims < migrate_users_person_only.sql
--  (실행 전 백업 권장)
-- ============================================================

USE cims;

-- 컬럼 존재 시에만 DROP (재실행 안전).
SET @drop_login := IF(
  EXISTS(SELECT 1 FROM information_schema.columns
         WHERE table_schema = DATABASE() AND table_name = 'users' AND column_name = 'login_id'),
  'ALTER TABLE users DROP COLUMN login_id', 'SELECT 1');
PREPARE s FROM @drop_login; EXECUTE s; DEALLOCATE PREPARE s;

SET @drop_pw := IF(
  EXISTS(SELECT 1 FROM information_schema.columns
         WHERE table_schema = DATABASE() AND table_name = 'users' AND column_name = 'password'),
  'ALTER TABLE users DROP COLUMN password', 'SELECT 1');
PREPARE s FROM @drop_pw; EXECUTE s; DEALLOCATE PREPARE s;

SET @drop_role := IF(
  EXISTS(SELECT 1 FROM information_schema.columns
         WHERE table_schema = DATABASE() AND table_name = 'users' AND column_name = 'role'),
  'ALTER TABLE users DROP COLUMN role', 'SELECT 1');
PREPARE s FROM @drop_role; EXECUTE s; DEALLOCATE PREPARE s;

-- org_id 검색 인덱스 (없으면 추가).
SET @add_idx := IF(
  NOT EXISTS(SELECT 1 FROM information_schema.statistics
             WHERE table_schema = DATABASE() AND table_name = 'users' AND index_name = 'idx_users_org'),
  'ALTER TABLE users ADD KEY idx_users_org (org_id)', 'SELECT 1');
PREPARE s FROM @add_idx; EXECUTE s; DEALLOCATE PREPARE s;
