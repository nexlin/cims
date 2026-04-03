-- 모니터링·이력·통계 DB 마이그레이션
-- Usage: sudo mysql cims < sql/migrate_monitoring.sql

-- 1. call_logs 에 녹취 상태 컬럼 추가
ALTER TABLE voip_call_logs
    ADD COLUMN IF NOT EXISTS rec_status ENUM('none','raw','transcoding','ready','failed') DEFAULT 'none',
    ADD COLUMN IF NOT EXISTS rec_raw_path VARCHAR(512) DEFAULT NULL;

ALTER TABLE ptt_call_logs
    ADD COLUMN IF NOT EXISTS rec_status ENUM('none','raw','transcoding','ready','failed') DEFAULT 'none',
    ADD COLUMN IF NOT EXISTS rec_raw_path VARCHAR(512) DEFAULT NULL;

-- 2. 통계 집계 테이블

-- 일간 집계
CREATE TABLE IF NOT EXISTS stats_daily (
    date          DATE NOT NULL,
    category      VARCHAR(32) NOT NULL,
    metric        VARCHAR(64) NOT NULL,
    value         DOUBLE NOT NULL DEFAULT 0,
    detail_json   TEXT DEFAULT NULL,
    PRIMARY KEY (date, category, metric)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 월간 집계
CREATE TABLE IF NOT EXISTS stats_monthly (
    year_month    VARCHAR(7) NOT NULL,
    category      VARCHAR(32) NOT NULL,
    metric        VARCHAR(64) NOT NULL,
    value         DOUBLE NOT NULL DEFAULT 0,
    detail_json   TEXT DEFAULT NULL,
    PRIMARY KEY (year_month, category, metric)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 연간 집계
CREATE TABLE IF NOT EXISTS stats_yearly (
    year          SMALLINT NOT NULL,
    category      VARCHAR(32) NOT NULL,
    metric        VARCHAR(64) NOT NULL,
    value         DOUBLE NOT NULL DEFAULT 0,
    detail_json   TEXT DEFAULT NULL,
    PRIMARY KEY (year, category, metric)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
