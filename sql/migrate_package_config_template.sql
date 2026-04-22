-- ============================================================
-- P10.1: 모듈 설정 템플릿 + process/functions 분리
--
--   cims_package:
--     + config_template_json  (tarball 내 config_template.json)
--     + meta_json             (tarball 내 meta.json 전체: service.functions/processes 포함)
--
--   agent_deployment:
--     * service_kind     → process_name         (프로세스/바이너리명)
--     + service_functions                       (활성 기능 머신명, 콤마구분)
--     + config_json                             (해당 배포의 현재 설정 값)
--     + config_applied_at                       (타겟에 마지막 적용 시각)
--
-- Usage: sudo mysql cims < sql/migrate_package_config_template.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
--  cims_package: 템플릿/메타 저장 컬럼
-- ─────────────────────────────────────────────
ALTER TABLE cims_package
    ADD COLUMN IF NOT EXISTS config_template_json MEDIUMTEXT NULL
        COMMENT 'tarball 의 config_template.json (sections/fields 스키마)',
    ADD COLUMN IF NOT EXISTS meta_json MEDIUMTEXT NULL
        COMMENT 'tarball 의 meta.json 전체 (service.functions / service.processes 포함)';

-- ─────────────────────────────────────────────
--  agent_deployment: process_name / service_functions / config
--   service_kind → process_name 으로 rename (기존 값 그대로 유지)
-- ─────────────────────────────────────────────
ALTER TABLE agent_deployment
    CHANGE COLUMN service_kind process_name VARCHAR(32)
        COMMENT '프로세스(바이너리) 이름: CSP, PSP, ISP, CMP ...',
    ADD COLUMN IF NOT EXISTS service_functions VARCHAR(255) DEFAULT NULL
        COMMENT '활성 기능 머신명들(콤마구분): 예) "volte,ptt" 또는 "volte,ptt,ibcf"',
    ADD COLUMN IF NOT EXISTS config_json MEDIUMTEXT NULL
        COMMENT '해당 배포의 현재 설정 값 (JSON, 키는 template field.key)',
    ADD COLUMN IF NOT EXISTS config_applied_at DATETIME DEFAULT NULL
        COMMENT '타겟에 마지막 적용된 시각';

SELECT 'Migration complete' AS status,
       (SELECT COUNT(*) FROM cims_package)     AS packages,
       (SELECT COUNT(*) FROM agent_deployment) AS deployments;
