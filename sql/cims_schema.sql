-- ============================================================
--  CIMS MariaDB Schema (consolidated)
--  최초 DB 생성용 단일 스키마 — base + 전 마이그레이션의 최종 상태.
--  적용: db-bootstrap 모듈(권장) 또는  mysql -u root -p < cims_schema.sql
--
--  설계 원칙 (2026-06-15):
--    - DB = 조직 / 가입자(person) / VoLTE·PTT 번호 / PTT 그룹.
--    - 콘솔 로그인 계정(OAM users)은 DB 가 아니라 OAM file_store 도메인
--      console_accounts 에서 관리 (HA 시 OAM 런타임 공유로 동기화).
--      → users 테이블은 가입자(person) 전용. login_id/password/role 없음.
--    - 운영 런타임 데이터(agents/ha_groups/packages/deployments/call_logs/
--      recordings/csp_runtime/idms tokens 등)는 전부 file_store 로 이관됨 → DB 없음.
--  최종 테이블: 8개 (organizations, users, volte_subscriptions,
--    ptt_subscriptions, user_rejects, ptt_groups, ptt_group_members,
--    ptt_affiliations).
-- ============================================================

CREATE DATABASE IF NOT EXISTS cims
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE cims;

-- ─────────────────────────────────────────────
--  조직 트리 (Organizations)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS organizations (
    id          INT          NOT NULL AUTO_INCREMENT,
    code        VARCHAR(32)  NOT NULL COMMENT '조직 코드',
    code_path   VARCHAR(512)          DEFAULT NULL COMMENT '루트→현재 코드 경로 (캐시)',
    name        VARCHAR(128) NOT NULL COMMENT '조직명',
    parent_id   INT                   DEFAULT NULL COMMENT '상위 조직 ID (NULL=최상위)',
    sort_order  INT                   DEFAULT 0 COMMENT '정렬 순서',
    created_at  DATETIME              DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME              DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY code (code),
    KEY idx_org_parent (parent_id),
    KEY idx_org_code (code),
    KEY idx_org_code_path (code_path),
    CONSTRAINT organizations_ibfk_1 FOREIGN KEY (parent_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='조직 트리';

-- ─────────────────────────────────────────────
--  가입자(person) 기본 정보 (Users) — 콘솔 로그인 필드 없음 (person 전용)
--  콘솔 로그인 계정은 OAM file_store 도메인 console_accounts 에서 관리.
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS users (
    id          INT          NOT NULL AUTO_INCREMENT COMMENT '개인 고유 ID (자동 발행)',
    name        VARCHAR(128) NOT NULL COMMENT '표시 이름',
    login_id    VARCHAR(64)           DEFAULT NULL COMMENT '단말/IdMS 로그인 ID (CIMS 로그인 자격 — MCPTT ID 와 별개)',
    passwd      VARCHAR(128)          DEFAULT NULL COMMENT '단말/IdMS 로그인 비밀번호',
    email       VARCHAR(255) NOT NULL DEFAULT '' COMMENT '이메일',
    org_id      VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '소속 조직 코드 (organizations.code)',
    title       VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '직함/직위 (그룹문서 cims:user-title 로 UE 전달)',
    details     TEXT                  DEFAULT NULL COMMENT '세부사항',
    create_time DATETIME              DEFAULT NULL,
    update_time DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_users_login_id (login_id),
    KEY idx_users_org (org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='가입자(person) — login_id/passwd=단말 IdMS 로그인(MCPTT ID 와 분리). 콘솔 admin 계정은 OAM console_accounts(file)';

-- ─────────────────────────────────────────────
--  VoLTE 가입자 인증 정보 (VoLTE Subscriptions)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS volte_subscriptions (
    id            VARCHAR(64)  NOT NULL COMMENT 'VoLTE MSISDN (E.164)',
    user_id       INT          NOT NULL COMMENT 'users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    imsi          VARCHAR(64)           DEFAULT NULL,
    service_ref   VARCHAR(64)           DEFAULT NULL COMMENT 'access_services.name',
    passwd        VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 패스워드',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_voip_sub_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='VoLTE 가입자 인증 정보';

-- ─────────────────────────────────────────────
--  PTT 가입자 인증 정보 (PTT Subscriptions)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_subscriptions (
    id            VARCHAR(64)  NOT NULL COMMENT 'PTT MSISDN (E.164)',
    user_id       INT          NOT NULL COMMENT 'users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    imsi          VARCHAR(64)           DEFAULT NULL,
    service_ref   VARCHAR(64)           DEFAULT NULL COMMENT 'access_services.name',
    passwd        VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 패스워드',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_ptt_sub_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 가입자 인증 정보';

-- ─────────────────────────────────────────────
--  가입자별 착신거부 목록 (User Rejects)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS user_rejects (
    user_id   INT         NOT NULL COMMENT '가입자 ID (users.id)',
    reject_id VARCHAR(64) NOT NULL COMMENT '거부할 발신자 MSISDN',
    PRIMARY KEY (user_id, reject_id),
    CONSTRAINT fk_reject_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='가입자별 착신거부 목록';

-- ─────────────────────────────────────────────
--  PTT 그룹 (PTT Groups) — 3GPP TS 24.379/24.380
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_groups (
    id                  BIGINT       NOT NULL AUTO_INCREMENT COMMENT 'surrogate 내부키',
    mcptt_group_id      VARCHAR(255) NOT NULL COMMENT 'MCPTT group ID (그룹 식별자, SIP 주소)',
    name                VARCHAR(128) NOT NULL DEFAULT '' COMMENT '그룹명',
    video_enabled       TINYINT(1)   NOT NULL DEFAULT 0 COMMENT 'H.264 비디오 릴레이 활성화',
    session_start       DATETIME              DEFAULT NULL COMMENT '그룹 세션 시작시간',
    session_end         DATETIME              DEFAULT NULL COMMENT '그룹 세션 종료시간 (NULL=무기한)',
    priority            INT                   DEFAULT 5 COMMENT '그룹 우선순위 (1=최고, 10=최저)',
    encryption          TINYINT(1)            DEFAULT 0 COMMENT '암호화 여부',
    emergency_call      TINYINT(1)            DEFAULT 0 COMMENT '긴급통화 허용',
    org_code            VARCHAR(32)           DEFAULT NULL COMMENT '소속 조직 코드',
    session_seq         INT          NOT NULL DEFAULT 1,
    group_type          ENUM('prearranged','chat','broadcast') NOT NULL DEFAULT 'prearranged' COMMENT '3GPP TS 24.379 session-type',
    on_network          TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'on-network 그룹 여부',
    max_members         INT          NOT NULL DEFAULT 0 COMMENT 'on-network-max-participant-count (0=무제한)',
    require_affiliation TINYINT(1)   NOT NULL DEFAULT 1 COMMENT '그룹콜 수신 전 affiliation 요구',
    alias               VARCHAR(128)          DEFAULT NULL COMMENT '그룹 별칭/단축명',
    icon_url            VARCHAR(512)          DEFAULT NULL COMMENT '그룹 아이콘 URL',
    authorized_user_id  INT                   DEFAULT NULL COMMENT '그룹 소유자 users.id (TS 23.280)',
    created_at          DATETIME              DEFAULT CURRENT_TIMESTAMP COMMENT '그룹 생성 시각',
    imminent_peril_call TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'allow-imminent-peril-call',
    emergency_alert     TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'allow-MCPTT-emergency-alert',
    adhoc_enabled       TINYINT(1)   NOT NULL DEFAULT 0 COMMENT 'ad hoc 그룹콜 허용 (TS 22.179 Rel-18)',
    allow_sds           TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'mcdata-allow-short-data-service (그룹 SDS 메시징 허용, TS 24.481)',
    allow_fd            TINYINT(1)   NOT NULL DEFAULT 0 COMMENT 'mcdata-allow-file-distribution (그룹 파일전송 허용, TS 24.481)',
    max_sds_size        INT          NOT NULL DEFAULT 10000 COMMENT 'mcdata-on-network-max-data-size-for-SDS (payload octets, 0=무제한)',
    max_auto_recv       INT          NOT NULL DEFAULT 1048576 COMMENT 'mcdata-on-network-max-data-size-auto-recv (파일 자동 다운로드 임계 octets)',
    PRIMARY KEY (id),
    UNIQUE KEY uk_mcptt_group_id (mcptt_group_id),
    KEY idx_authorized_user (authorized_user_id),
    CONSTRAINT fk_grp_authorized_user FOREIGN KEY (authorized_user_id) REFERENCES users (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='PTT 그룹 정보';

-- ─────────────────────────────────────────────
--  PTT 그룹 멤버 (PTT Group Members)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_group_members (
    group_id BIGINT      NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    user_id  VARCHAR(64) NOT NULL COMMENT '멤버 가입자 ID',
    priority INT         NOT NULL DEFAULT 0 COMMENT '발언권 우선순위 (낮을수록 높음)',
    role     ENUM('chair','participant') NOT NULL DEFAULT 'participant' COMMENT 'TS 24.380 participant type — chair 는 floor 우선 선점',
    mcptt_id VARCHAR(255)         DEFAULT NULL COMMENT '멤버 MCPTT ID URI (NULL=user_id 사용)',
    PRIMARY KEY (group_id, user_id),
    KEY idx_user (user_id),
    CONSTRAINT fk_gm_group FOREIGN KEY (group_id) REFERENCES ptt_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='그룹 멤버 목록';

-- ─────────────────────────────────────────────
--  MCPTT affiliation 상태 (TS 24.379 §9)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_affiliations (
    group_id      BIGINT       NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    user_id       VARCHAR(64)  NOT NULL COMMENT 'ptt_group_members.user_id',
    client_id     VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP instance (Contact +sip.instance)',
    affiliated_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'affiliation 시각',
    expires_at    DATETIME              DEFAULT NULL COMMENT 'affiliation 만료 (NULL=dereg 시까지)',
    status        ENUM('affiliated','deaffiliated') NOT NULL DEFAULT 'affiliated' COMMENT '상태',
    PRIMARY KEY (group_id, user_id, client_id),
    KEY idx_aff_user (user_id),
    KEY idx_aff_group_status (group_id, status),
    CONSTRAINT fk_aff_group FOREIGN KEY (group_id) REFERENCES ptt_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='MCPTT affiliation 상태 (TS 24.379 §9)';
