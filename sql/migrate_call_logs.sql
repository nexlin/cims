-- ============================================================
-- Migration: add call logging tables
-- 실행: sudo mysql cims < migrate_call_logs.sql
-- ============================================================

USE cims;

-- 통화 세션 (1 Call-ID = 1 row, PTT 그룹은 그룹 세션 1 row)
CREATE TABLE IF NOT EXISTS cims_call_logs (
    id          BIGINT       NOT NULL AUTO_INCREMENT,
    call_id     VARCHAR(256) NOT NULL                COMMENT 'SIP Call-ID (PTT: 발신자 Call-ID)',
    call_type   ENUM('voip','ptt') NOT NULL DEFAULT 'voip',
    group_id    VARCHAR(64)  DEFAULT NULL            COMMENT 'PTT 그룹 ID',
    initiator   VARCHAR(64)  NOT NULL                COMMENT '발신 MSISDN',
    callee      VARCHAR(64)  NOT NULL                COMMENT '수신 MSISDN 또는 그룹 ID',
    state       ENUM('ringing','active','ended') NOT NULL DEFAULT 'ringing',
    invite_time DATETIME     NOT NULL                COMMENT 'INVITE 수신 시각',
    answer_time DATETIME     DEFAULT NULL            COMMENT '최초 응답(연결) 시각',
    end_time    DATETIME     DEFAULT NULL            COMMENT '종료 시각',
    duration    INT          DEFAULT NULL            COMMENT '통화 시간(초, answer→end)',
    sip_status  INT          DEFAULT NULL            COMMENT '종료 SIP 상태코드',
    end_reason  VARCHAR(32)  DEFAULT NULL            COMMENT 'normal/busy/cancel/timeout/error',
    PRIMARY KEY (id),
    UNIQUE KEY uq_call_id (call_id(128)),
    KEY idx_state       (state),
    KEY idx_initiator   (initiator),
    KEY idx_callee      (callee),
    KEY idx_invite_time (invite_time),
    KEY idx_group       (group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='통화 세션 로그';

-- 통화 참여자 (VoIP: 2명, PTT: N명, 멀티세션 지원)
CREATE TABLE IF NOT EXISTS cims_call_participants (
    log_id      BIGINT       NOT NULL                COMMENT 'cims_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL                COMMENT '참여자 MSISDN',
    role        ENUM('caller','callee','member') NOT NULL DEFAULT 'member',
    join_time   DATETIME     DEFAULT NULL            COMMENT 'NULL = 아직 연결 안됨 (ringing)',
    leave_time  DATETIME     DEFAULT NULL            COMMENT 'NULL = 현재 연결 중',
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn),
    KEY idx_active (msisdn, leave_time)             COMMENT '번호별 현재 참여 세션 조회'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='통화 참여자 상태';

SELECT 'Migration complete: cims_call_logs, cims_call_participants created.' AS result;
