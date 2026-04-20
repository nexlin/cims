-- ============================================================
-- SIP 서비스 엔티티 도입 (P7)
--   - Service = 비즈니스 경계 (domain, kind, auth_realm)
--   - subscribers, trunks, listeners 가 서비스에 귀속
--   - 기존 Realm 배열은 bootstrap/fallback 용으로 유지
--
-- Usage: sudo mysql cims < sql/migrate_sip_service.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
--  sip_service: 서비스(비즈니스 경계) 정의
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS sip_service (
    id              INT           NOT NULL AUTO_INCREMENT,
    name            VARCHAR(64)   NOT NULL COMMENT '관리용 식별명 (volte-main, mcptt-kt 등)',
    kind            ENUM('voip','ptt','ibcf','system','console') NOT NULL,
    domain          VARCHAR(255)  NOT NULL COMMENT 'URI 매칭 + full IMPI 조립용 도메인',
    auth_realm      VARCHAR(255)           DEFAULT NULL
                     COMMENT 'Digest challenge realm (NULL = domain 그대로 사용)',
    inbound_policy  ENUM('any','restricted') NOT NULL DEFAULT 'any'
                     COMMENT 'any=모든 리스너, restricted=sip_service_listener 에 매핑된 리스너만',
    priority        INT           NOT NULL DEFAULT 100
                     COMMENT '같은 도메인 다중 서비스 구분 순서 (낮을수록 먼저 평가)',
    enabled         TINYINT(1)    NOT NULL DEFAULT 1,
    note            VARCHAR(255)           DEFAULT NULL,
    etag            VARCHAR(32)   NOT NULL DEFAULT '',
    create_time     DATETIME               DEFAULT CURRENT_TIMESTAMP,
    update_time     DATETIME               DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_service_name (name),
    KEY idx_domain (domain)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='SIP 서비스 (가입자+트렁크의 비즈니스 경계)';

-- ─────────────────────────────────────────────
--  sip_service_listener: 서비스-리스너 링크 (inbound_policy=restricted 용)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS sip_service_listener (
    service_id   INT NOT NULL,
    listener_id  INT NOT NULL,
    PRIMARY KEY (service_id, listener_id),
    CONSTRAINT fk_svcl_svc  FOREIGN KEY (service_id)  REFERENCES sip_service(id)  ON DELETE CASCADE,
    CONSTRAINT fk_svcl_lsn  FOREIGN KEY (listener_id) REFERENCES csp_listener(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='서비스-리스너 링크 (restricted 정책 시 수신 허용 리스너)';

-- ─────────────────────────────────────────────
--  sip_trunk: 서비스 귀속 + failover priority
-- ─────────────────────────────────────────────
ALTER TABLE sip_trunk
    ADD COLUMN IF NOT EXISTS service_id INT DEFAULT NULL
        COMMENT '소속 서비스 (NULL=미지정, 라우팅 규칙에서 직접 trunk_id 로만 참조)',
    ADD COLUMN IF NOT EXISTS failover_priority INT NOT NULL DEFAULT 100
        COMMENT '같은 서비스 내 트렁크 선택 우선순위 (낮을수록 먼저)',
    ADD FOREIGN KEY IF NOT EXISTS fk_trunk_service (service_id)
        REFERENCES sip_service(id) ON DELETE SET NULL,
    ADD KEY IF NOT EXISTS idx_trunk_service (service_id, enabled, failover_priority);

-- ─────────────────────────────────────────────
--  voip_subscriptions: service_id + imsi 추가
--    auth_id 는 하위호환 유지 (IMSI 정규화 완료 후 P8 에서 제거 예정)
-- ─────────────────────────────────────────────
ALTER TABLE voip_subscriptions
    ADD COLUMN IF NOT EXISTS service_id INT DEFAULT NULL
        COMMENT '소속 서비스 (NULL 이면 REGISTER 거부)',
    ADD COLUMN IF NOT EXISTS imsi VARCHAR(32) DEFAULT NULL
        COMMENT 'IMSI 등 user 파트. service.domain 과 결합되어 full IMPI 구성',
    ADD FOREIGN KEY IF NOT EXISTS fk_voip_service (service_id)
        REFERENCES sip_service(id) ON DELETE SET NULL,
    ADD KEY IF NOT EXISTS idx_voip_service (service_id);

-- ─────────────────────────────────────────────
--  ptt_subscriptions: 동일 패턴
-- ─────────────────────────────────────────────
ALTER TABLE ptt_subscriptions
    ADD COLUMN IF NOT EXISTS service_id INT DEFAULT NULL,
    ADD COLUMN IF NOT EXISTS imsi VARCHAR(32) DEFAULT NULL
        COMMENT 'IMSI 등 user 파트',
    ADD FOREIGN KEY IF NOT EXISTS fk_ptt_service (service_id)
        REFERENCES sip_service(id) ON DELETE SET NULL,
    ADD KEY IF NOT EXISTS idx_ptt_service (service_id);

-- ─────────────────────────────────────────────
--  routing_rule: target.mode="service" 지원 + service_id FK
-- ─────────────────────────────────────────────
ALTER TABLE routing_rule
    MODIFY COLUMN target_mode
        ENUM('trunk','service','priority_list','round_robin','weighted','reject') NOT NULL DEFAULT 'trunk',
    ADD COLUMN IF NOT EXISTS target_service_id INT DEFAULT NULL
        COMMENT 'target_mode=service 일 때 참조할 sip_service.id',
    ADD FOREIGN KEY IF NOT EXISTS fk_rule_service (target_service_id)
        REFERENCES sip_service(id) ON DELETE SET NULL;

-- ─────────────────────────────────────────────
--  csp_config_audit entity 확장 (service 추가)
-- ─────────────────────────────────────────────
-- entity 필드는 VARCHAR 이라 변경 불필요

-- ─────────────────────────────────────────────
--  초기 seed: 현재 csp.json Realm 기준 기본 서비스 생성
--    id=1 → volte-main, id=2 → mcptt-main
-- ─────────────────────────────────────────────
INSERT IGNORE INTO sip_service (id, name, kind, domain, note) VALUES
    (1, 'volte-main', 'voip', 'ims.mnc001.mcc001.3gppnetwork.org', '기본 VoLTE 서비스 (자동 생성)'),
    (2, 'mcptt-main', 'ptt',  'ptt.mnc001.mcc001.3gppnetwork.org', '기본 MCPTT 서비스 (자동 생성)');

-- ─────────────────────────────────────────────
--  기존 가입자 backfill: auth_id 패턴에서 IMSI 추출 + 서비스 매핑
-- ─────────────────────────────────────────────

-- VoLTE: auth_id = "<숫자>@ims.xxx" 패턴 → IMSI 추출 + volte-main 귀속
UPDATE voip_subscriptions
   SET imsi       = SUBSTRING_INDEX(auth_id, '@', 1),
       service_id = 1
 WHERE auth_id REGEXP '^[0-9]+@'
   AND auth_id LIKE '%@ims.%'
   AND service_id IS NULL;

-- PTT: auth_id = "<숫자>@ptt.xxx" 패턴 → IMSI 추출 + mcptt-main 귀속
UPDATE ptt_subscriptions
   SET imsi       = SUBSTRING_INDEX(auth_id, '@', 1),
       service_id = 2
 WHERE auth_id REGEXP '^[0-9]+@'
   AND auth_id LIKE '%@ptt.%'
   AND service_id IS NULL;

-- ─────────────────────────────────────────────
--  기존 트렁크 backfill: 생성 시점 protocol 기준 voip/ptt 추정은 부정확 →
--    수동 매핑 필요. 여기서는 일단 service_id=NULL 로 두고 Console 에서 수동 연결
-- ─────────────────────────────────────────────

-- 확인용 SELECT (마이그레이션 검증)
SELECT id, name, kind, domain FROM sip_service ORDER BY id;
SELECT COUNT(*) AS voip_aligned_count
  FROM voip_subscriptions WHERE service_id IS NOT NULL AND imsi IS NOT NULL;
SELECT COUNT(*) AS ptt_aligned_count
  FROM ptt_subscriptions  WHERE service_id IS NOT NULL AND imsi IS NOT NULL;
