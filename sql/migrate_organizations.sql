-- 조직 트리 테이블 마이그레이션
-- Usage: sudo mysql cims < sql/migrate_organizations.sql

CREATE TABLE IF NOT EXISTS organizations (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    code        VARCHAR(32)  NOT NULL UNIQUE   COMMENT '조직 코드',
    name        VARCHAR(128) NOT NULL          COMMENT '조직명',
    parent_id   INT          DEFAULT NULL      COMMENT '상위 조직 ID (NULL=최상위)',
    sort_order  INT          DEFAULT 0         COMMENT '정렬 순서',
    created_at  DATETIME     DEFAULT NOW(),
    updated_at  DATETIME     DEFAULT NOW() ON UPDATE NOW(),
    FOREIGN KEY (parent_id) REFERENCES organizations(id) ON DELETE SET NULL,
    INDEX idx_org_parent (parent_id),
    INDEX idx_org_code (code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='조직 트리';
