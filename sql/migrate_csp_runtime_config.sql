-- ============================================================
-- CSP 런타임 설정 DB 마이그레이션
--   - SIP 리스너, 트렁크(원격 서버), 라우팅 규칙, 접근제어, 감사로그
--   - CSC 가 DB primary + 메모리 캐시 + 파일 스냅샷을 관리하고
--     CSP 는 CSC 로부터만 설정을 받는다. DB 는 CSP 가 직접 조회하지 않음.
--
-- Usage: sudo mysql cims < sql/migrate_csp_runtime_config.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
--  SIP 리스너 정의 (csp_listener)
--    - CSP 가 bind 하는 SIP 수신 endpoint
--    - protocol 별 분리, service 는 Realm 과 연결
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS csp_listener (
    id              INT          NOT NULL AUTO_INCREMENT,
    name            VARCHAR(64)  NOT NULL COMMENT '관리용 식별명',
    enabled         TINYINT(1)   NOT NULL DEFAULT 1,
    bind_ip         VARCHAR(64)  NOT NULL DEFAULT '0.0.0.0',
    bind_port       INT          NOT NULL,
    protocol        ENUM('UDP','TCP','TLS','WS','WSS') NOT NULL DEFAULT 'UDP',
    domain          VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'SIP From/To authority',
    service         ENUM('volte','mcptt','system','console') NOT NULL DEFAULT 'volte',
    tls_cert_path   VARCHAR(512)          DEFAULT NULL,
    tls_key_path    VARCHAR(512)          DEFAULT NULL,
    tls_ca_path     VARCHAR(512)          DEFAULT NULL,
    tls_verify_peer TINYINT(1)   NOT NULL DEFAULT 0,
    max_connections INT          NOT NULL DEFAULT 0  COMMENT '0 = 무제한 (TCP/TLS/WS*)',
    thread_count    INT          NOT NULL DEFAULT 2,
    note            VARCHAR(255)          DEFAULT NULL,
    etag            VARCHAR(32)  NOT NULL DEFAULT '',
    create_time     DATETIME              DEFAULT CURRENT_TIMESTAMP,
    update_time     DATETIME              DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_bind (bind_ip, bind_port, protocol)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='CSP SIP 리스너 정의';

-- ─────────────────────────────────────────────
--  외부 SIP 서버/트렁크 (sip_trunk)
--    - CSP 가 호 전달을 위해 접속하는 원격 서버
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS sip_trunk (
    id                   INT          NOT NULL AUTO_INCREMENT,
    name                 VARCHAR(64)  NOT NULL,
    enabled              TINYINT(1)   NOT NULL DEFAULT 1,
    remote_ip            VARCHAR(64)  NOT NULL,
    remote_port          INT          NOT NULL DEFAULT 5060,
    remote_domain        VARCHAR(255) NOT NULL DEFAULT '',
    protocol             ENUM('UDP','TCP','TLS') NOT NULL DEFAULT 'UDP',
    outbound_proxy_ip    VARCHAR(64)           DEFAULT NULL,
    outbound_proxy_port  INT                   DEFAULT NULL,
    -- 트렁크에 등록(REGISTER)을 보낼지 여부와 자격증명
    register_to_remote   TINYINT(1)   NOT NULL DEFAULT 0,
    auth_user            VARCHAR(128)          DEFAULT NULL,
    auth_password        VARCHAR(128)          DEFAULT NULL,
    auth_realm           VARCHAR(255)          DEFAULT NULL,
    register_expires     INT          NOT NULL DEFAULT 3600,
    -- 헬스체크
    options_ping_sec     INT          NOT NULL DEFAULT 60  COMMENT '0 = 비활성',
    options_dead_threshold INT        NOT NULL DEFAULT 3   COMMENT '연속 실패 N회 시 dead',
    -- 전송 안정화
    srv_lookup           TINYINT(1)   NOT NULL DEFAULT 0,
    dns_fallback         TINYINT(1)   NOT NULL DEFAULT 1,
    -- 용량/제한
    max_concurrent_calls INT          NOT NULL DEFAULT 0   COMMENT '0 = 무제한',
    cps_limit            INT          NOT NULL DEFAULT 0   COMMENT 'calls/sec, 0 = 무제한',
    -- 운영 메타
    note                 VARCHAR(255)          DEFAULT NULL,
    etag                 VARCHAR(32)  NOT NULL DEFAULT '',
    create_time          DATETIME              DEFAULT CURRENT_TIMESTAMP,
    update_time          DATETIME              DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_trunk_name (name),
    KEY idx_enabled (enabled)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='원격 SIP 서버/트렁크';

-- ─────────────────────────────────────────────
--  라우팅 규칙 헤더 (routing_rule)
--    - 규칙별 우선순위, 활성화, 히트 카운터, 실패 동작
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS routing_rule (
    id               INT           NOT NULL AUTO_INCREMENT,
    name             VARCHAR(128)  NOT NULL,
    enabled          TINYINT(1)    NOT NULL DEFAULT 1,
    priority         INT           NOT NULL DEFAULT 100 COMMENT '낮을수록 먼저 평가 (first-match-wins)',
    description      VARCHAR(512)           DEFAULT NULL,
    -- 타겟 선택 전략
    target_mode      ENUM('trunk','priority_list','round_robin','weighted','reject') NOT NULL DEFAULT 'trunk',
    target_trunk_id  INT                    DEFAULT NULL,
    target_json      TEXT                   DEFAULT NULL COMMENT '복합 타겟 정의 JSON',
    -- 실패 동작
    fail_action      ENUM('reject','fallback','next_rule') NOT NULL DEFAULT 'reject',
    fail_code        INT           NOT NULL DEFAULT 404,
    fail_reason      VARCHAR(128)  NOT NULL DEFAULT 'Not Found',
    fallback_trunk_id INT                   DEFAULT NULL,
    timeout_ms       INT           NOT NULL DEFAULT 4000,
    retry_count      INT           NOT NULL DEFAULT 0,
    -- 통계 (실시간 카운터는 메모리, 여기는 누적 스냅샷)
    hit_count        BIGINT        NOT NULL DEFAULT 0,
    last_hit_time    DATETIME               DEFAULT NULL,
    -- 운영
    etag             VARCHAR(32)   NOT NULL DEFAULT '',
    create_time      DATETIME               DEFAULT CURRENT_TIMESTAMP,
    update_time      DATETIME               DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_rule_name (name),
    KEY idx_priority (enabled, priority),
    CONSTRAINT fk_rule_trunk FOREIGN KEY (target_trunk_id) REFERENCES sip_trunk(id) ON DELETE SET NULL,
    CONSTRAINT fk_rule_fallback FOREIGN KEY (fallback_trunk_id) REFERENCES sip_trunk(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='SIP 라우팅 규칙';

-- ─────────────────────────────────────────────
--  라우팅 매칭 조건 (routing_rule_match)
--    - 하나의 rule 에 N개 조건 (AND 조합)
--    - match_type 에 따라 해석 방식이 달라짐
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS routing_rule_match (
    id         INT          NOT NULL AUTO_INCREMENT,
    rule_id    INT          NOT NULL,
    field      VARCHAR(64)  NOT NULL COMMENT 'req_uri_user | req_uri_host | from_uri | to_uri | method | source_ip | source_trunk | header:<NAME> | sdp_codec | time_of_day | day_of_week',
    op         ENUM('equals','prefix','suffix','contains','regex','cidr','in_range','not_equals') NOT NULL DEFAULT 'equals',
    value      VARCHAR(512) NOT NULL,
    invert     TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '매칭 결과 반전 (NOT)',
    seq        INT          NOT NULL DEFAULT 0,
    PRIMARY KEY (id),
    KEY idx_rule (rule_id, seq),
    CONSTRAINT fk_match_rule FOREIGN KEY (rule_id) REFERENCES routing_rule(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='라우팅 규칙 매칭 조건';

-- ─────────────────────────────────────────────
--  라우팅 변환 규칙 (routing_rule_transform)
--    - forward 직전 SIP 메시지 변조
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS routing_rule_transform (
    id         INT          NOT NULL AUTO_INCREMENT,
    rule_id    INT          NOT NULL,
    action     ENUM('set_req_uri_user','set_req_uri_host','set_from_host',
                    'add_header','remove_header','replace_header',
                    'strip_prefix','add_prefix',
                    'set_transport','set_privacy','anonymize_from') NOT NULL,
    target     VARCHAR(128)          DEFAULT NULL COMMENT 'header 이름 등 액션별 타겟',
    value      VARCHAR(1024)         DEFAULT NULL,
    seq        INT          NOT NULL DEFAULT 0,
    PRIMARY KEY (id),
    KEY idx_rule (rule_id, seq),
    CONSTRAINT fk_transform_rule FOREIGN KEY (rule_id) REFERENCES routing_rule(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='라우팅 규칙 변환 액션';

-- ─────────────────────────────────────────────
--  접근제어 목록 (routing_access_list)
--    - IP allow/deny, UA allow/deny. 리스너별 또는 글로벌.
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS routing_access_list (
    id            INT          NOT NULL AUTO_INCREMENT,
    scope         ENUM('global','listener','trunk') NOT NULL DEFAULT 'global',
    scope_ref_id  INT                   DEFAULT NULL COMMENT 'listener_id 또는 trunk_id',
    kind          ENUM('allow','deny')  NOT NULL,
    match_type    ENUM('ip','cidr','ua_regex') NOT NULL DEFAULT 'ip',
    value         VARCHAR(255)  NOT NULL,
    enabled       TINYINT(1)    NOT NULL DEFAULT 1,
    note          VARCHAR(255)          DEFAULT NULL,
    priority      INT           NOT NULL DEFAULT 100,
    etag          VARCHAR(32)   NOT NULL DEFAULT '',
    create_time   DATETIME               DEFAULT CURRENT_TIMESTAMP,
    update_time   DATETIME               DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_scope (scope, scope_ref_id, enabled, priority)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='SIP 접근제어 리스트';

-- ─────────────────────────────────────────────
--  설정 변경 감사 로그 (csp_config_audit)
--    - 누가 언제 어떤 설정을 바꿨는지 추적
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS csp_config_audit (
    id           BIGINT       NOT NULL AUTO_INCREMENT,
    ts           DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    actor        VARCHAR(128) NOT NULL COMMENT '변경을 수행한 관리자 (JWT sub)',
    actor_ip     VARCHAR(64)           DEFAULT NULL,
    entity       VARCHAR(64)  NOT NULL COMMENT 'listener | trunk | route | access',
    entity_id    VARCHAR(64)           DEFAULT NULL,
    action       ENUM('CREATE','UPDATE','DELETE','APPLY','ROLLBACK') NOT NULL,
    before_json  TEXT                  DEFAULT NULL,
    after_json   TEXT                  DEFAULT NULL,
    etag_before  VARCHAR(32)           DEFAULT NULL,
    etag_after   VARCHAR(32)           DEFAULT NULL,
    reason       VARCHAR(512)          DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_ts (ts),
    KEY idx_entity (entity, entity_id),
    KEY idx_actor (actor)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='CSP 설정 변경 감사 로그';

-- ─────────────────────────────────────────────
--  초기 bootstrap 시드 (선택)
--    - 기존 csp.json 의 5060/UDP 리스너를 관리 대상으로 등록
--    - 실제 bind 는 CSP 가 수행, 이 테이블은 관리/표시용 시드
-- ─────────────────────────────────────────────
INSERT IGNORE INTO csp_listener (name, enabled, bind_ip, bind_port, protocol, domain, service, note)
VALUES
    ('default-udp-5060', 1, '0.0.0.0', 5060, 'UDP', 'csp', 'system', '기본 bootstrap 리스너');
