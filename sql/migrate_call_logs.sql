-- ============================================================
-- Migration: add call logging tables (split voip / ptt)
-- 실행: sudo mysql cims < migrate_call_logs.sql
-- ============================================================

USE cims;

-- VoIP 통화 세션 (1 Call-ID = 1 row)
CREATE TABLE IF NOT EXISTS voip_call_logs (
    id          BIGINT       NOT NULL AUTO_INCREMENT,
    call_id     VARCHAR(256) NOT NULL COMMENT 'SIP Call-ID',
    initiator   VARCHAR(64)  NOT NULL COMMENT '발신 MSISDN',
    callee      VARCHAR(64)  NOT NULL COMMENT '수신 MSISDN',
    state       ENUM('ringing','active','ended') NOT NULL DEFAULT 'ringing',
    invite_time DATETIME     NOT NULL,
    answer_time DATETIME     DEFAULT NULL,
    end_time    DATETIME     DEFAULT NULL,
    duration    INT          DEFAULT NULL,
    sip_status  INT          DEFAULT NULL,
    end_reason  VARCHAR(32)  DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_call_id (call_id(128)),
    KEY idx_state (state), KEY idx_initiator (initiator), KEY idx_callee (callee), KEY idx_invite_time (invite_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoIP 통화 로그';

-- PTT 그룹 통화 세션 (1 Call-ID = 1 row, 그룹 세션)
CREATE TABLE IF NOT EXISTS ptt_call_logs (
    id          BIGINT       NOT NULL AUTO_INCREMENT,
    call_id     VARCHAR(256) NOT NULL COMMENT 'SIP Call-ID (발신자)',
    group_id    VARCHAR(64)  NOT NULL COMMENT 'PTT 그룹 ID',
    initiator   VARCHAR(64)  NOT NULL COMMENT '최초 발신 MSISDN',
    state       ENUM('ringing','active','ended') NOT NULL DEFAULT 'ringing',
    invite_time DATETIME     NOT NULL,
    answer_time DATETIME     DEFAULT NULL,
    end_time    DATETIME     DEFAULT NULL,
    duration    INT          DEFAULT NULL,
    end_reason  VARCHAR(32)  DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_call_id (call_id(128)),
    KEY idx_state (state), KEY idx_group (group_id), KEY idx_invite_time (invite_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 그룹 통화 로그';

-- VoIP 통화 참여자 (2명)
CREATE TABLE IF NOT EXISTS voip_call_participants (
    log_id      BIGINT       NOT NULL COMMENT 'voip_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL COMMENT '참여자 MSISDN',
    role        ENUM('caller','callee') NOT NULL DEFAULT 'callee',
    join_time   DATETIME     DEFAULT NULL COMMENT 'NULL = 아직 연결 안됨 (ringing)',
    leave_time  DATETIME     DEFAULT NULL COMMENT 'NULL = 현재 연결 중',
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoIP 통화 참여자';

-- PTT 그룹 통화 참여자 (N명)
CREATE TABLE IF NOT EXISTS ptt_call_participants (
    log_id      BIGINT       NOT NULL COMMENT 'ptt_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL COMMENT '참여자 MSISDN',
    role        ENUM('member') NOT NULL DEFAULT 'member',
    join_time   DATETIME     DEFAULT NULL COMMENT 'NULL = 아직 연결 안됨 (ringing)',
    leave_time  DATETIME     DEFAULT NULL COMMENT 'NULL = 현재 연결 중',
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 그룹 통화 참여자';

SELECT 'Migration complete: voip_call_logs, ptt_call_logs, voip_call_participants, ptt_call_participants created.' AS result;
