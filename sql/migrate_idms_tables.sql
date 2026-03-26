-- ============================================================
-- Migration: MCPTT IdMs OAuth 테이블을 cims DB에 추가
-- (기존에 csc_idms DB를 사용하던 auth_codes, refresh_tokens)
--
-- 실행: mysql -u root -p cims < migrate_idms_tables.sql
-- ============================================================

USE cims;

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
