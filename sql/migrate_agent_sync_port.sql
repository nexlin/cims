-- ============================================================
-- P10.2: Agent sync REST 포트
--   cims_agent 에 sync_port 컬럼 추가 (agent 가 heartbeat 시 보고).
--   CSC 가 이 포트로 agent 에 직접 요청 (jsonl collection read/write 등).
-- ============================================================

USE cims;

ALTER TABLE cims_agent
    ADD COLUMN IF NOT EXISTS sync_port INT DEFAULT NULL
        COMMENT 'Agent 가 노출하는 동기 REST 포트 (jsonl collection 접근)';

SELECT 'migrate_agent_sync_port done' AS status;
