-- ============================================================
-- 그룹 capability 축 단순화 — imminent_peril_call/adhoc_enabled 컬럼 제거
--   · condition(긴급/임박위험) 허용은 emergency_call 단일 게이트로 통합
--     (XCAP allow-imminent-peril-call 은 emergency_call 미러로 산출)
--   · adhoc_enabled 는 어느 경로에서도 집행되지 않는 죽은 플래그 —
--     ad hoc 인가는 그룹 속성이 아니라 사용자/시스템 정책 소관
--     (docs/design/features/mcptt_emergency_modes.md §6)
-- 실행: mysql -u root cims < migrate_drop_group_imminent_adhoc.sql   (또는 cims 계정)
-- ⚠️ 이 컬럼을 SELECT 하지 않는 csp/csc 배포 후에 적용한다.
-- MariaDB 10.0+ DROP COLUMN IF EXISTS 사용(재실행 안전).
-- ============================================================
USE cims;

ALTER TABLE ptt_groups
  DROP COLUMN IF EXISTS imminent_peril_call,
  DROP COLUMN IF EXISTS adhoc_enabled;
