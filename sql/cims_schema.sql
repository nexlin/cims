-- ============================================================
-- CIMS MariaDB Schema
-- 실행: mysql -u root -p < cims_schema.sql
-- ============================================================

CREATE DATABASE IF NOT EXISTS cims
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE cims;

-- ─────────────────────────────────────────────
--  가입자 기본 정보 (Users)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS users (
    id          INT          NOT NULL AUTO_INCREMENT COMMENT '개인 고유 ID (자동 발행)',
    name        VARCHAR(128) NOT NULL COMMENT '표시 이름',
    email       VARCHAR(255) NOT NULL DEFAULT '' COMMENT '이메일',
    org_id      VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '소속 조직 ID',
    details     TEXT                  DEFAULT NULL COMMENT '세부사항',
    create_time DATETIME              DEFAULT NULL,
    update_time DATETIME              DEFAULT NULL,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='사용자 기본 정보';

-- ─────────────────────────────────────────────
--  VoLTE 가입자 인증 정보 (VoIP Subscriptions)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS voip_subscriptions (
    id            VARCHAR(64)  NOT NULL COMMENT 'VoLTE MSISDN (E.164)',
    user_id       INT          NOT NULL COMMENT 'users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    passwd        VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 패스워드',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_voip_sub_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoLTE 가입자 인증 정보';

-- ─────────────────────────────────────────────
--  PTT 가입자 인증 정보 (PTT Subscriptions)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_subscriptions (
    id            VARCHAR(64)  NOT NULL COMMENT 'PTT MSISDN (E.164)',
    user_id       INT          NOT NULL COMMENT 'users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    passwd        VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 패스워드',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_ptt_sub_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 가입자 인증 정보';

-- ─────────────────────────────────────────────
--  착신거부 목록 (User Reject List)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS user_rejects (
    user_id   INT         NOT NULL COMMENT '가입자 ID (users.id)',
    reject_id VARCHAR(64) NOT NULL COMMENT '거부할 발신자 MSISDN',
    PRIMARY KEY (user_id, reject_id),
    CONSTRAINT fk_reject_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='가입자별 착신거부 목록';

-- ─────────────────────────────────────────────
--  PTT 그룹 (Groups)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_groups (
    id            VARCHAR(64)  NOT NULL COMMENT '그룹 번호 (E.164)',
    name          VARCHAR(128) NOT NULL DEFAULT '' COMMENT '그룹명',
    video_enabled TINYINT(1)   NOT NULL DEFAULT 0 COMMENT 'H.264 비디오 릴레이 활성화',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='PTT 그룹 정보';

-- ─────────────────────────────────────────────
--  그룹 멤버 (Group Members)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_group_members (
    group_id VARCHAR(64) NOT NULL COMMENT '그룹 ID',
    user_id  VARCHAR(64) NOT NULL COMMENT '멤버 가입자 ID',
    priority INT         NOT NULL DEFAULT 0 COMMENT '발언권 우선순위 (낮을수록 높음)',
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='그룹 멤버 목록';

-- ─────────────────────────────────────────────
--  VoIP 통화 로그
-- ─────────────────────────────────────────────
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

-- ─────────────────────────────────────────────
--  PTT 그룹 통화 로그
-- ─────────────────────────────────────────────
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

-- ─────────────────────────────────────────────
--  VoIP 통화 참여자
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS voip_call_participants (
    log_id      BIGINT       NOT NULL COMMENT 'voip_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL,
    role        ENUM('caller','callee') NOT NULL DEFAULT 'callee',
    join_time   DATETIME     DEFAULT NULL,
    leave_time  DATETIME     DEFAULT NULL,
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoIP 통화 참여자';

-- ─────────────────────────────────────────────
--  PTT 그룹 통화 참여자
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_call_participants (
    log_id      BIGINT       NOT NULL COMMENT 'ptt_call_logs.id',
    msisdn      VARCHAR(64)  NOT NULL,
    role        ENUM('member') NOT NULL DEFAULT 'member',
    join_time   DATETIME     DEFAULT NULL,
    leave_time  DATETIME     DEFAULT NULL,
    PRIMARY KEY (log_id, msisdn),
    KEY idx_msisdn (msisdn)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 그룹 통화 참여자';

-- ─────────────────────────────────────────────
--  MCPTT IdMs OAuth 2.0 인증 코드
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS auth_codes (
    code                   VARCHAR(128)  NOT NULL COMMENT '인증 코드',
    user_id                VARCHAR(128)  NOT NULL COMMENT '사용자 ID',
    client_id              VARCHAR(128)  NOT NULL DEFAULT '' COMMENT '클라이언트 ID',
    redirect_uri           VARCHAR(512)           DEFAULT NULL COMMENT '리다이렉트 URI',
    scope                  VARCHAR(256)           DEFAULT NULL COMMENT '스코프',
    state                  VARCHAR(256)           DEFAULT NULL COMMENT '상태 값',
    issued_at              BIGINT        NOT NULL DEFAULT 0  COMMENT '발급 시각 (Unix timestamp)',
    expires_at             BIGINT        NOT NULL DEFAULT 0  COMMENT '만료 시각 (Unix timestamp)',
    used                   TINYINT(1)    NOT NULL DEFAULT 0  COMMENT '사용 여부',
    code_challenge         VARCHAR(256)           DEFAULT NULL COMMENT 'PKCE code_challenge',
    code_challenge_method  VARCHAR(16)            DEFAULT NULL COMMENT 'PKCE method (S256 등)',
    PRIMARY KEY (code),
    KEY idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCPTT IdMs OAuth 2.0 인증 코드';

-- ─────────────────────────────────────────────
--  MCPTT IdMs OAuth 2.0 리프레시 토큰
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS refresh_tokens (
    token_id    VARCHAR(256)  NOT NULL COMMENT '리프레시 토큰 ID',
    user_id     VARCHAR(128)  NOT NULL COMMENT '사용자 ID',
    client_id   VARCHAR(128)  NOT NULL DEFAULT '' COMMENT '클라이언트 ID',
    scope       VARCHAR(256)           DEFAULT NULL COMMENT '스코프',
    issued_at   BIGINT        NOT NULL DEFAULT 0  COMMENT '발급 시각 (Unix timestamp)',
    expires_at  BIGINT        NOT NULL DEFAULT 0  COMMENT '만료 시각 (Unix timestamp)',
    revoked     TINYINT(1)    NOT NULL DEFAULT 0  COMMENT '폐기 여부',
    rotated_to  VARCHAR(256)           DEFAULT NULL COMMENT '교체된 토큰 ID',
    PRIMARY KEY (token_id),
    KEY idx_expires (expires_at),
    CONSTRAINT fk_rotated_to FOREIGN KEY (rotated_to)
        REFERENCES refresh_tokens (token_id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCPTT IdMs OAuth 2.0 리프레시 토큰';

-- ─────────────────────────────────────────────
--  DB 접속 계정 생성 (root 로 실행)
-- ─────────────────────────────────────────────
-- CREATE USER IF NOT EXISTS 'cims'@'localhost' IDENTIFIED BY 'cims1234';
-- GRANT SELECT, INSERT, UPDATE, DELETE ON cims.* TO 'cims'@'localhost';
-- FLUSH PRIVILEGES;
