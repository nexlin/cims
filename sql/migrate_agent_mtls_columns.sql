-- ============================================================
-- Phase D: cims_agent 에 per-agent mTLS 관련 컬럼 추가
--
--   Phase C 에서 CSC 전역 Agent.MtlsEnabled=true 일 때 모든 enroll 된
--   agent 에게 cert 를 발급했습니다. 그러나:
--     1. MtlsEnabled=true 활성화 전에 enroll 된 agent 는 cert 없음
--     2. 향후 agent 별로 점진적 전환/롤백이 필요할 수 있음
--
--   → agent 레코드별 mtls_enabled 플래그 + cert 만료일 트래킹 도입.
--
--   컬럼:
--     mtls_enabled       — 이 agent 에 mTLS 적용 여부 (0/1)
--     cert_issued_at     — 최신 cert 발급 시각
--     cert_expires_at    — 최신 cert 만료 시각 (rotation 모니터링용)
--
-- 실행: sudo mysql cims < sql/migrate_agent_mtls_columns.sql
-- ============================================================

USE cims;

ALTER TABLE cims_agent
    ADD COLUMN IF NOT EXISTS mtls_enabled TINYINT(1) NOT NULL DEFAULT 0
        COMMENT 'Per-agent mTLS 활성 플래그 — enroll 시 CSC 전역 설정과 함께 AND 됨',
    ADD COLUMN IF NOT EXISTS cert_issued_at  DATETIME DEFAULT NULL
        COMMENT 'mTLS agent server cert 최신 발급 시각',
    ADD COLUMN IF NOT EXISTS cert_expires_at DATETIME DEFAULT NULL
        COMMENT 'mTLS agent server cert 만료 시각 (rotation 감시용)',
    ADD COLUMN IF NOT EXISTS cert_rotate_pending TINYINT(1) NOT NULL DEFAULT 0
        COMMENT '1 이면 다음 heartbeat 응답에 cert_rotate=true 를 내려줘야 함',
    ADD KEY IF NOT EXISTS idx_agent_cert_expires (cert_expires_at);

SELECT 'Migration complete: cims_agent mTLS columns added' AS status;
