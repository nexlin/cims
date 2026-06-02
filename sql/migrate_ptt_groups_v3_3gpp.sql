-- PTT 그룹 3GPP MCPTT 규격 정합 마이그레이션 (v3) — 식별 체계 재구성
-- Usage: mysql -u cims -pcims1234 cims < sql/migrate_ptt_groups_v3_3gpp.sql
--
-- ⚠ 구조 변경(1회성, 재실행 불가). 실행 전 백업 권장.
--
-- 변경 요지:
--  * ptt_groups.id : 기존 VARCHAR(번호) → 기존 번호값을 mcptt_group_id 로 이전,
--    id 는 BIGINT AUTO_INCREMENT surrogate 키로 신설(내부 관리/FK).
--  * mcptt_group_id : 그룹 식별자(UNIQUE). 키(PK/FK)로는 쓰지 않음.
--  * ptt_group_members.group_id / ptt_affiliations.group_id : surrogate id(BIGINT) 참조.
--  * 3GPP 속성(group_type/on_network/max_members/require_affiliation/alias/icon_url) +
--    멤버 role/mcptt_id + ptt_affiliations 신설.
--  * require_affiliation 기본=1: 모든 그룹(기존 포함)이 affiliation 필요 — 규격 기본 동작.

SET FOREIGN_KEY_CHECKS = 0;

-- ── 1) ptt_groups: 번호 id → mcptt_group_id, surrogate id 신설 ──────────────
ALTER TABLE ptt_groups CHANGE COLUMN id mcptt_group_id VARCHAR(255) NOT NULL
    COMMENT 'MCPTT group ID (그룹 식별자, SIP 주소)';
ALTER TABLE ptt_groups DROP PRIMARY KEY;
ALTER TABLE ptt_groups ADD UNIQUE KEY uk_mcptt_group_id (mcptt_group_id);
ALTER TABLE ptt_groups ADD COLUMN id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST;

-- 3GPP 속성
ALTER TABLE ptt_groups
    ADD COLUMN group_type ENUM('prearranged','chat','broadcast') NOT NULL DEFAULT 'prearranged'
        COMMENT '3GPP TS 24.379 session-type';
ALTER TABLE ptt_groups
    ADD COLUMN on_network TINYINT(1) NOT NULL DEFAULT 1 COMMENT 'on-network 그룹 여부';
ALTER TABLE ptt_groups
    ADD COLUMN max_members INT NOT NULL DEFAULT 0 COMMENT 'on-network-max-participant-count (0=무제한)';
ALTER TABLE ptt_groups
    ADD COLUMN require_affiliation TINYINT(1) NOT NULL DEFAULT 1 COMMENT '그룹콜 수신 전 affiliation 요구';
ALTER TABLE ptt_groups
    ADD COLUMN alias VARCHAR(128) DEFAULT NULL COMMENT '그룹 별칭/단축명';
ALTER TABLE ptt_groups
    ADD COLUMN icon_url VARCHAR(512) DEFAULT NULL COMMENT '그룹 아이콘 URL';

-- ── 2) ptt_group_members: group_id(번호) → surrogate id 로 repoint ──────────
ALTER TABLE ptt_group_members ADD COLUMN _gpk BIGINT NULL;
UPDATE ptt_group_members m JOIN ptt_groups g ON m.group_id = g.mcptt_group_id SET m._gpk = g.id;
ALTER TABLE ptt_group_members DROP PRIMARY KEY;
ALTER TABLE ptt_group_members DROP COLUMN group_id;
ALTER TABLE ptt_group_members CHANGE COLUMN _gpk group_id BIGINT NOT NULL COMMENT 'ptt_groups.id (surrogate)';
ALTER TABLE ptt_group_members ADD PRIMARY KEY (group_id, user_id);

ALTER TABLE ptt_group_members
    ADD COLUMN role ENUM('chair','participant') NOT NULL DEFAULT 'participant'
        COMMENT 'TS 24.380 participant type — chair 는 floor 우선 선점';
ALTER TABLE ptt_group_members
    ADD COLUMN mcptt_id VARCHAR(255) DEFAULT NULL COMMENT '멤버 MCPTT ID URI (NULL=user_id 사용)';
ALTER TABLE ptt_group_members
    ADD CONSTRAINT fk_gm_group FOREIGN KEY (group_id) REFERENCES ptt_groups(id) ON DELETE CASCADE;

-- ── 3) ptt_affiliations 신설 (surrogate id 참조) ────────────────────────────
CREATE TABLE IF NOT EXISTS ptt_affiliations (
    group_id      BIGINT       NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    user_id       VARCHAR(64)  NOT NULL COMMENT 'ptt_group_members.user_id',
    client_id     VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP instance (Contact +sip.instance)',
    affiliated_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'affiliation 시각',
    expires_at    DATETIME     DEFAULT NULL COMMENT 'affiliation 만료 (NULL=dereg 시까지)',
    status        ENUM('affiliated','deaffiliated') NOT NULL DEFAULT 'affiliated' COMMENT '상태',
    PRIMARY KEY (group_id, user_id, client_id),
    INDEX idx_aff_user (user_id),
    INDEX idx_aff_group_status (group_id, status),
    CONSTRAINT fk_aff_group FOREIGN KEY (group_id) REFERENCES ptt_groups(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCPTT affiliation 상태 (TS 24.379 §9)';

SET FOREIGN_KEY_CHECKS = 1;
