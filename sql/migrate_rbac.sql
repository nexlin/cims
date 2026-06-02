-- ============================================================
-- CIMS RBAC + 그룹 소유(authorized user) 마이그레이션 (idempotent)
--   설계: docs/design/features/mcptt_authorization.md
--   · users.role ENUM 5종 확장 (admin/manager/operator/monitor/user)
--     기존 'admin'→'admin', 그 외(기존 'user' 포함)→'user' 로 보존.
--   · ptt_groups.authorized_user_id (그룹 생성자 = 3GPP authorized user = 관리주체)
--     + created_at.  FK→users.id ON DELETE SET NULL.
-- 실행: mysql -u root -p cims < sql/migrate_rbac.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
-- 1) users.role ENUM 5종 확장
--    MODIFY 이므로 기존 값('admin','user')은 새 enum 집합에 포함되어 그대로 보존된다.
-- ─────────────────────────────────────────────
ALTER TABLE users
    MODIFY COLUMN role ENUM('admin','manager','operator','monitor','user')
        NOT NULL DEFAULT 'user'
        COMMENT '관리 권한 역할 (admin/manager/operator/monitor/user). user=OAM 로그인 불가';

-- ─────────────────────────────────────────────
-- 2) ptt_groups.authorized_user_id + created_at
--    authorized_user_id = 그룹을 생성한 users.id (= administrator = 관리주체).
--    users.id 가 INT 이므로 FK 컬럼도 INT.
-- ─────────────────────────────────────────────
ALTER TABLE ptt_groups
    ADD COLUMN IF NOT EXISTS authorized_user_id INT DEFAULT NULL
        COMMENT '그룹 소유자(=생성자=authorized user) users.id (3GPP TS 23.280)'
        AFTER icon_url,
    ADD COLUMN IF NOT EXISTS created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        COMMENT '그룹 생성 시각'
        AFTER authorized_user_id;

-- FK (이미 있으면 무시) — INFORMATION_SCHEMA 로 존재 확인 후 추가
SET @has_fk := (
    SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups'
      AND CONSTRAINT_NAME = 'fk_grp_authorized_user'
);
SET @sql := IF(@has_fk = 0,
    "ALTER TABLE ptt_groups ADD CONSTRAINT fk_grp_authorized_user "
    "FOREIGN KEY (authorized_user_id) REFERENCES users(id) ON DELETE SET NULL",
    "DO 0");
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

ALTER TABLE ptt_groups ADD INDEX IF NOT EXISTS idx_authorized_user (authorized_user_id);

SELECT 'RBAC migration complete: users.role(5) + ptt_groups.authorized_user_id/created_at' AS status;
