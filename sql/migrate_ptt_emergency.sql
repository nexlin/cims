-- ============================================================
-- MCPTT emergency / imminent-peril / alert / ad-hoc 능력(capability)
-- Phase 0 (능력/설정) — 런타임 호경로 무영향. 설계: docs/design/features/mcptt_emergency_modes.md
-- 실행: mysql -u root cims < migrate_ptt_emergency.sql   (또는 cims 계정)
-- MariaDB 10.0+ ADD COLUMN IF NOT EXISTS 사용(재실행 안전).
-- ============================================================
USE cims;

-- 그룹 능력 (TS 24.481 group config) — emergency_call 은 기존 컬럼 재사용.
ALTER TABLE ptt_groups
  ADD COLUMN IF NOT EXISTS imminent_peril_call TINYINT(1) NOT NULL DEFAULT 1
      COMMENT 'allow-imminent-peril-call (임박위험 그룹콜 허용)',
  ADD COLUMN IF NOT EXISTS emergency_alert     TINYINT(1) NOT NULL DEFAULT 1
      COMMENT 'allow-MCPTT-emergency-alert (긴급경보 허용)',
  ADD COLUMN IF NOT EXISTS adhoc_enabled       TINYINT(1) NOT NULL DEFAULT 0
      COMMENT 'ad hoc 그룹콜 허용 (TS 22.179 Rel-18)';

-- 사용자 MCPTT 프로파일 (TS 24.483) — Phase 0b 에서 admin/console 연동.
CREATE TABLE IF NOT EXISTS ptt_user_profile (
  user_id                    INT          NOT NULL PRIMARY KEY COMMENT 'users.id',
  allow_emergency_init       TINYINT(1)   NOT NULL DEFAULT 1 COMMENT '긴급 개시 권한',
  allow_alert_init           TINYINT(1)   NOT NULL DEFAULT 1 COMMENT '긴급경보 개시 권한',
  default_emergency_group_id BIGINT       NULL COMMENT '기본 긴급 그룹 (ptt_groups.id)',
  default_imminent_group_id  BIGINT       NULL COMMENT '기본 임박위험 그룹 (ptt_groups.id)',
  updated_at                 DATETIME     NULL,
  CONSTRAINT fk_pup_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='사용자 MCPTT 프로파일 (긴급/경보 권한·기본 긴급그룹)';
