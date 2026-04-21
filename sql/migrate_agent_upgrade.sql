-- Agent 자기 바이너리 업그레이드 job 지원 — agent_job.job_type ENUM 확장
ALTER TABLE agent_job
    MODIFY COLUMN job_type ENUM(
        'install','upgrade','upgrade_agent','uninstall',
        'start','stop','restart',
        'update_config','collect_log','exec','health_check'
    ) NOT NULL;
