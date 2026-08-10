-- 사용자 MCPTT 프로파일 v2 (TS 24.484 user profile / TS 24.379 §6.3.3.1.13.2)
--  - SOS(긴급 그룹콜) 대상 결정 모드 + 전용 긴급그룹 + 사용자 단위 개시 인가.
--  - 키 = ptt_subscriptions.id (PTT MSISDN) — CSP 게이트·XCAP user-profile 이 쓰는 운영 신원.
--    (구 ptt_user_profile(users.id 키)은 미배선으로 drop 됨 — migrate_drop_unused_tables.sql)
--  - emergency_group_id 는 ptt_groups.mcptt_group_id 참조 — 그룹 삭제 시 NULL(미지정=미인가→403).
--  적용 순서: 테이블 생성은 코드 배포와 독립적으로 선행 가능 (CSC/CSP 는 부재 row 를 기본값으로 취급).

CREATE TABLE IF NOT EXISTS ptt_user_profile (
    ptt_id                VARCHAR(64)  NOT NULL COMMENT 'ptt_subscriptions.id (PTT MSISDN)',
    allow_emergency_call  TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'allow-emergency-group-call (TS 24.484 ruleset) — 긴급 그룹콜 개시 인가',
    allow_emergency_alert TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'allow-activate-emergency-alert — 긴급경보 개시 인가',
    allow_adhoc_call      TINYINT(1)   NOT NULL DEFAULT 1 COMMENT 'ad hoc 그룹콜 개시 인가 (시스템 정책 Setup.PttAdhocEnabled 와 AND)',
    emergency_group_mode  ENUM('DedicatedGroup','UseCurrentlySelectedGroup') NOT NULL DEFAULT 'DedicatedGroup'
        COMMENT 'SOS 대상 결정 (MCPTTGroupInitiation entry-info, TS 24.484)',
    emergency_group_id    VARCHAR(255) DEFAULT NULL COMMENT '전용 긴급그룹 (ptt_groups.mcptt_group_id) — DedicatedGroup 모드의 콜·경보 공통 대상. NULL=미지정(긴급 미인가)',
    update_time           DATETIME     DEFAULT NULL,
    PRIMARY KEY (ptt_id),
    CONSTRAINT fk_pup_ptt_sub FOREIGN KEY (ptt_id) REFERENCES ptt_subscriptions (id) ON DELETE CASCADE,
    CONSTRAINT fk_pup_emg_group FOREIGN KEY (emergency_group_id) REFERENCES ptt_groups (mcptt_group_id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='사용자 MCPTT 프로파일 (SOS 대상 결정 모드·전용 긴급그룹·개시 인가)';
