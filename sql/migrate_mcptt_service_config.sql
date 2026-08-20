-- MCPTT 시스템 서비스 설정 (TS 24.484 service-config)
--  - service-config 은 **시스템 전역 문서 1건**이다(사용자별 문서는 user-profile). 따라서 단일 행
--    (id=1)으로 둔다 — 가입자별 오버라이드는 규격 근거가 없어 두지 않는다. 사용자 단위 인가는
--    ptt_user_profile(ruleset)이 정본이며, 단말은 두 축을 AND 로 게이트한다.
--  - 단말 소비 지점: docs/design/features/android_ue_client.md §7 "CMS 문서 소비".
--  - 부재/미적용 DB 는 CSC 가 코드 기본값(전부 허용·N2 10)으로 취급하므로 적용은 코드 배포와
--    독립적으로 선행 가능.

CREATE TABLE IF NOT EXISTS mcptt_service_config (
    id                         TINYINT     NOT NULL DEFAULT 1
        COMMENT '단일 행 고정(1) — service-config 은 시스템 전역 문서 1건이다',
    allow_private_call         TINYINT(1)  NOT NULL DEFAULT 1 COMMENT 'allow-private-call — 1:1 통화 발신 허용',
    allow_emergency_call       TINYINT(1)  NOT NULL DEFAULT 1 COMMENT 'allow-emergency-call — 긴급통화 허용(사용자 인가와 AND)',
    allow_alert                TINYINT(1)  NOT NULL DEFAULT 1 COMMENT 'allow-alert — 긴급경보 허용(사용자 인가와 AND)',
    allow_transmit_request     TINYINT(1)  NOT NULL DEFAULT 1 COMMENT 'on-network allow-transmit-request — floor(발언권) 요청 허용',
    allow_create_delete_group  TINYINT(1)  NOT NULL DEFAULT 1 COMMENT 'allow-create-delete-group — 사용자 그룹 생성/삭제 허용',
    max_affiliations_n2        SMALLINT    NOT NULL DEFAULT 10
        COMMENT 'N2 — 동시 제휴(편성) 채널 상한. XML 의 max-affiliations-N2 와 on-network 하위 max-on-network-affiliations-N2 에 같은 값으로 실린다',
    num_levels_group_hierarchy TINYINT     NOT NULL DEFAULT 3 COMMENT 'num-levels-group-hierarchy',
    num_levels_user_hierarchy  TINYINT     NOT NULL DEFAULT 3 COMMENT 'num-levels-user-hierarchy',
    update_time                DATETIME    DEFAULT NULL,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCPTT 시스템 서비스 설정 (TS 24.484 service-config — 시스템 전역 1건)';

-- 초기 행 — 전부 허용(현행 동작 유지). 재실행 안전.
INSERT IGNORE INTO mcptt_service_config (id, update_time) VALUES (1, NOW());
