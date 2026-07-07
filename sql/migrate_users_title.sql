-- ============================================================
-- Migration: add title (직함/직위) column to users
-- 실행: sudo mysql cims < migrate_users_title.sql
-- ============================================================

ALTER TABLE users
  ADD COLUMN title VARCHAR(64) NOT NULL DEFAULT '' COMMENT '직함/직위 (그룹문서 cims:user-title 로 UE 전달)' AFTER org_id;

SELECT 'Migration complete. title column added to users.' AS result;
