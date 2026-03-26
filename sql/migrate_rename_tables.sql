-- ============================================================
-- Migration: rename all cims_* tables to new names
-- 기존 배포 DB에 적용하는 마이그레이션 스크립트
-- 실행: sudo mysql cims < migrate_rename_tables.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
--  단순 이름 변경
-- ─────────────────────────────────────────────
RENAME TABLE
    cims_users            TO users,
    cims_call_users       TO voip_subscriptions,
    cims_ptt_users        TO ptt_subscriptions,
    cims_user_rejects     TO user_rejects,
    cims_ptt_groups       TO ptt_groups,
    cims_ptt_group_members TO ptt_group_members;

-- FK 참조 대상은 RENAME TABLE 시 MariaDB 가 자동으로 갱신하므로 별도 ALTER 불필요

-- ─────────────────────────────────────────────
--  call_logs 분리: voip + ptt
-- ─────────────────────────────────────────────

-- 1. 새 테이블 생성
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

CREATE TABLE IF NOT EXISTS voip_call_participants (
    log_id      BIGINT       NOT NULL COMMENT 'voip_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL,
    role        ENUM('caller','callee') NOT NULL DEFAULT 'callee',
    join_time   DATETIME     DEFAULT NULL,
    leave_time  DATETIME     DEFAULT NULL,
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoIP 통화 참여자';

CREATE TABLE IF NOT EXISTS ptt_call_participants (
    log_id      BIGINT       NOT NULL COMMENT 'ptt_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL,
    role        ENUM('member') NOT NULL DEFAULT 'member',
    join_time   DATETIME     DEFAULT NULL,
    leave_time  DATETIME     DEFAULT NULL,
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 그룹 통화 참여자';

-- 2. VoIP 데이터 이전 (call_type='voip')
INSERT INTO voip_call_logs
    (id, call_id, initiator, callee, state, invite_time, answer_time, end_time, duration, sip_status, end_reason)
SELECT id, call_id, initiator, callee, state, invite_time, answer_time, end_time, duration, sip_status, end_reason
FROM cims_call_logs
WHERE call_type = 'voip';

-- 3. PTT 데이터 이전 (call_type='ptt')
INSERT INTO ptt_call_logs
    (id, call_id, group_id, initiator, state, invite_time, answer_time, end_time, duration, end_reason)
SELECT id, call_id, COALESCE(group_id, ''), initiator, state, invite_time, answer_time, end_time, duration, end_reason
FROM cims_call_logs
WHERE call_type = 'ptt';

-- 4. participants 이전
--    VoIP 참여자 (role IN ('caller','callee'))
INSERT INTO voip_call_participants (log_id, msisdn, role, join_time, leave_time)
SELECT cp.log_id, cp.msisdn, cp.role, cp.join_time, cp.leave_time
FROM cims_call_participants cp
JOIN cims_call_logs cl ON cl.id = cp.log_id
WHERE cl.call_type = 'voip'
  AND cp.role IN ('caller','callee');

--    PTT 참여자 (role='member')
INSERT INTO ptt_call_participants (log_id, msisdn, role, join_time, leave_time)
SELECT cp.log_id, cp.msisdn, 'member', cp.join_time, cp.leave_time
FROM cims_call_participants cp
JOIN cims_call_logs cl ON cl.id = cp.log_id
WHERE cl.call_type = 'ptt';

-- 5. 기존 테이블 삭제
DROP TABLE IF EXISTS cims_call_participants;
DROP TABLE IF EXISTS cims_call_logs;

SELECT 'Migration complete: tables renamed, call_logs split into voip_call_logs + ptt_call_logs.' AS result;
