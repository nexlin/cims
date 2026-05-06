-- ============================================================
-- 검증 회차 이력 — verification_run + verification_run_item
--
--   verification_run      : 한 번의 verify 실행 (stage<N> 또는 임의 묶음)
--   verification_run_item : 그 회차에서 실행된 항목 1건당 결과
--
-- Console UI 의 검증 이력 페이지 (/testbed/verify-history) 가 이 테이블을 조회.
-- 검증 종료 시점에 backend (csc/src/handlers/verification.py) 가 자동 기록.
--
-- Usage: sudo mysql cims < sql/migrate_verification_runs.sql
-- ============================================================

USE cims;

CREATE TABLE IF NOT EXISTS verification_run (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    started_at        DATETIME(3) NOT NULL,
    finished_at       DATETIME(3) DEFAULT NULL,
    elapsed_ms        INT DEFAULT 0,

    trigger_type      ENUM('user','cli','ci') NOT NULL DEFAULT 'user',
    -- scope: 'stage1' ~ 'stage6' / 'preset:<name>' / 'items'
    scope             VARCHAR(64) NOT NULL,
    selected_ids      TEXT,                              -- JSON array (선택된 ID list)
    resume_stage      TINYINT DEFAULT NULL,              -- 재개 지점 (1~6 또는 NULL)

    verdict           ENUM('PASS','FAIL','UNKNOWN') NOT NULL DEFAULT 'UNKNOWN',
    totals            JSON,                              -- {total,pass,fail,skip,blocked}

    -- 패키지 immutability gate (S4-PKG-MANIFEST 산출과 매칭)
    pkg_manifest_hash VARCHAR(128) DEFAULT '',

    git_branch        VARCHAR(255) DEFAULT '',
    git_sha           VARCHAR(40)  DEFAULT '',
    host              VARCHAR(64)  DEFAULT '',
    ens_ip            VARCHAR(45)  DEFAULT '',

    report_path       TEXT,                              -- verify_reports/<ts>_stageN.md
    job_id            VARCHAR(32)  DEFAULT '',           -- /tmp/cims_verify_jobs/<job_id>.log

    note              TEXT,
    create_time       DATETIME DEFAULT CURRENT_TIMESTAMP,

    KEY idx_run_started   (started_at DESC),
    KEY idx_run_scope     (scope, started_at DESC),
    KEY idx_run_verdict   (verdict, started_at DESC),
    KEY idx_run_pkg_hash  (pkg_manifest_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='검증 회차 이력';


CREATE TABLE IF NOT EXISTS verification_run_item (
    run_id            BIGINT NOT NULL,
    item_id           VARCHAR(64) NOT NULL,
    stage             TINYINT NOT NULL,
    parent_id         VARCHAR(64) DEFAULT NULL,          -- 자식이면 부모 ID
    is_group          TINYINT(1) NOT NULL DEFAULT 0,
    name              VARCHAR(255) DEFAULT '',
    status            ENUM('PASS','FAIL','SKIP','BLOCKED','RUNNING','UNKNOWN')
                          NOT NULL DEFAULT 'UNKNOWN',
    elapsed_ms        INT DEFAULT 0,
    detail            TEXT,                              -- 결과 요약 (긴 stdout 은 별도)
    idx               INT DEFAULT 0,                     -- 실행 순서

    PRIMARY KEY (run_id, item_id),
    KEY idx_run_item_status (run_id, status),
    KEY idx_run_item_parent (parent_id),
    CONSTRAINT fk_run_item_run FOREIGN KEY (run_id)
        REFERENCES verification_run(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='검증 회차 항목별 결과';
