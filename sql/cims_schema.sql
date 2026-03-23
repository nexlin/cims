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
CREATE TABLE IF NOT EXISTS cims_users (
    id          VARCHAR(64)  NOT NULL COMMENT 'MSISDN (E.164)',
    name        VARCHAR(128) NOT NULL DEFAULT '' COMMENT '표시 이름',
    org_id      VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '소속 조직 ID',
    details     TEXT                  DEFAULT NULL COMMENT '세부사항',
    create_time DATETIME              DEFAULT NULL,
    update_time DATETIME              DEFAULT NULL,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='사용자 기본 정보';

-- ─────────────────────────────────────────────
--  VoLTE 가입자 인증 정보 (Call Users)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_call_users (
    id            VARCHAR(64)  NOT NULL COMMENT 'VoLTE MSISDN (E.164)',
    user_id       VARCHAR(64)  NOT NULL COMMENT 'cims_users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    passwd        VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 패스워드',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_call_user2 FOREIGN KEY (user_id) REFERENCES cims_users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoLTE 가입자 인증 정보';

-- ─────────────────────────────────────────────
--  PTT 가입자 인증 정보 (PTT Users)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_ptt_users (
    id            VARCHAR(64)  NOT NULL COMMENT 'PTT MSISDN (E.164)',
    user_id       VARCHAR(64)  NOT NULL COMMENT 'cims_users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    passwd        VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 패스워드',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_ptt_user2 FOREIGN KEY (user_id) REFERENCES cims_users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 가입자 인증 정보';

-- ─────────────────────────────────────────────
--  착신거부 목록 (User Reject List)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_user_rejects (
    user_id   VARCHAR(64) NOT NULL COMMENT '가입자 ID',
    reject_id VARCHAR(64) NOT NULL COMMENT '거부할 발신자 ID',
    PRIMARY KEY (user_id, reject_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='가입자별 착신거부 목록';

-- ─────────────────────────────────────────────
--  PTT 그룹 (Groups)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_ptt_groups (
    id   VARCHAR(64)  NOT NULL COMMENT '그룹 번호 (E.164)',
    name VARCHAR(128) NOT NULL DEFAULT '' COMMENT '그룹명',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='PTT 그룹 정보';

-- ─────────────────────────────────────────────
--  그룹 멤버 (Group Members)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_ptt_group_members (
    group_id VARCHAR(64) NOT NULL COMMENT '그룹 ID',
    user_id  VARCHAR(64) NOT NULL COMMENT '멤버 가입자 ID',
    priority INT         NOT NULL DEFAULT 0 COMMENT '발언권 우선순위 (낮을수록 높음)',
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='그룹 멤버 목록';

-- ─────────────────────────────────────────────
--  DB 접속 계정 생성 (root 로 실행)
-- ─────────────────────────────────────────────
-- CREATE USER IF NOT EXISTS 'cims'@'localhost' IDENTIFIED BY 'cims1234';
-- GRANT SELECT, INSERT, UPDATE, DELETE ON cims.* TO 'cims'@'localhost';
-- FLUSH PRIVILEGES;
