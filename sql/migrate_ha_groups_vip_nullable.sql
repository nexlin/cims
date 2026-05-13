-- ha_groups.vip 폐기 단계 1 — NULL 허용
--
-- Phase 2: vip_bindings_json 이 multi-slot VIP 를 표현하므로 단일 vip 필드 불필요.
-- 기존 운영 그룹의 vip 값은 유지 (legacy single-VIP 경로로 _render_ha_for_agent 가 처리).
-- 신규 그룹은 vip 없이도 생성 가능 (vip_bindings 만 사용).
--
-- 향후 단계 2 (모든 그룹이 vip_bindings 로 전환된 후): vip 컬럼 DROP.
--
-- Usage: mysql -u cims -p cims < sql/migrate_ha_groups_vip_nullable.sql

USE cims;

ALTER TABLE ha_groups
    MODIFY COLUMN vip VARCHAR(64) NULL DEFAULT NULL
        COMMENT 'legacy 단일 VIP (vip_bindings_json 이 대체) — 신규 그룹은 NULL 권장';
