-- agent_job.job_type ENUM 확장
--   - update_ha       : Phase 1.B 누락 (코드는 추가됐으나 ENUM 갱신 안 됨)
--   - apply_ip_config : Phase 2 신규 (HaServicesPage ServiceIpPanel [적용])
--
-- Usage: mysql -u cims -p cims < sql/migrate_agent_job_types.sql

USE cims;

ALTER TABLE agent_job
    MODIFY COLUMN job_type ENUM(
        'install','upgrade','upgrade_agent','uninstall',
        'start','stop','restart',
        'update_config','collect_log','exec','health_check',
        'update_ha','apply_ip_config'
    ) NOT NULL;
