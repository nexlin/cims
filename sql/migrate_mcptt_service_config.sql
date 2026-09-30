-- MCPTT 시스템 서비스 설정 (TS 24.484 service-config)
--  - service-config 은 **시스템 전역 문서 1건**이다(사용자별 문서는 user-profile). 따라서 단일 행
--    (id=1)으로 둔다 — 가입자별 오버라이드는 규격 근거가 없어 두지 않는다. 인가(1:1·긴급·경보·그룹 생성)는
--    이 문서가 아니라 ptt_user_profile(ruleset)·그룹 문서가 정본이다(TS 24.484 §8.4 에 그런 요소가 없다).
--  - 단말 소비 지점: docs/design/features/android_ue_client.md §7 "CMS 문서 소비".
--  - 부재/미적용 DB 는 CSC 가 코드 기본값(N2 10·계층 3)으로 취급하므로 적용은 코드 배포와
--    독립적으로 선행 가능.

CREATE TABLE IF NOT EXISTS mcptt_service_config (
    id                         TINYINT     NOT NULL DEFAULT 1
        COMMENT '단일 행 고정(1) — service-config 은 시스템 전역 문서 1건이다',
    max_affiliations_n2        SMALLINT    NOT NULL DEFAULT 10
        COMMENT 'N2 — 동시 제휴(편성) 채널 상한. user-profile <MaxAffiliationsN2>(TS 24.484 §8.3.2.1) 의 기본값',
    num_levels_group_hierarchy TINYINT     NOT NULL DEFAULT 3 COMMENT 'common/broadcast-group/num-levels-group-hierarchy (TS 24.484 §8.4.2.1)',
    num_levels_user_hierarchy  TINYINT     NOT NULL DEFAULT 3 COMMENT 'common/broadcast-group/num-levels-user-hierarchy (TS 24.484 §8.4.2.1)',
    update_time                DATETIME    DEFAULT NULL,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCPTT 시스템 서비스 설정 (TS 24.484 service-config — 시스템 전역 1건)';

-- 초기 행. 재실행 안전.
INSERT IGNORE INTO mcptt_service_config (id, update_time) VALUES (1, NOW());
