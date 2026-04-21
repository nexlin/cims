-- ============================================================
-- P10: 분산 배포 — Agent/Package/Deployment 스키마
--
--   cims_agent       : 각 서버에 설치된 에이전트 레지스트리
--   cims_package     : CSC 에 업로드된 배포 패키지
--   agent_deployment : (agent × package × role × instance) 배치
--   agent_job        : 작업 큐 (install/start/stop/config_update/uninstall)
--   agent_metric     : 리소스 메트릭 (선택적, 로테이션)
--
-- Usage: sudo mysql cims < sql/migrate_agent_deployment.sql
-- ============================================================

USE cims;

-- ─────────────────────────────────────────────
--  cims_instance (P9 에서 도입) — P10 은 이 테이블도 같이 생성
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_instance (
    id                INT PRIMARY KEY AUTO_INCREMENT,
    name              VARCHAR(64) UNIQUE NOT NULL,
    role              ENUM('volte','ptt','ibcf','multi') NOT NULL DEFAULT 'multi',
    description       VARCHAR(255),
    host              VARCHAR(64) NOT NULL DEFAULT '127.0.0.1',
    csp_notify_port   INT NOT NULL DEFAULT 4421,
    cmp_control_port  INT NOT NULL DEFAULT 9000,
    cmp_rtp_port_start INT NOT NULL DEFAULT 50000,
    enabled           TINYINT(1) NOT NULL DEFAULT 1,
    last_seen         DATETIME DEFAULT NULL,
    last_health       ENUM('unknown','alive','degraded','dead') NOT NULL DEFAULT 'unknown',
    note              VARCHAR(255),
    etag              VARCHAR(32) NOT NULL DEFAULT '',
    create_time       DATETIME DEFAULT CURRENT_TIMESTAMP,
    update_time       DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='CIMS 논리 인스턴스';

-- 기존 설정 엔티티에 instance_id 컬럼 추가 (NULL = multi 인스턴스 공유 설정)
ALTER TABLE sip_service   ADD COLUMN IF NOT EXISTS instance_id INT DEFAULT NULL,
                          ADD KEY IF NOT EXISTS idx_svc_instance (instance_id);
ALTER TABLE csp_listener  ADD COLUMN IF NOT EXISTS instance_id INT DEFAULT NULL,
                          ADD KEY IF NOT EXISTS idx_lsn_instance (instance_id);
ALTER TABLE sip_trunk     ADD COLUMN IF NOT EXISTS instance_id INT DEFAULT NULL,
                          ADD KEY IF NOT EXISTS idx_trk_instance (instance_id);
ALTER TABLE routing_rule  ADD COLUMN IF NOT EXISTS instance_id INT DEFAULT NULL,
                          ADD KEY IF NOT EXISTS idx_rte_instance (instance_id);
ALTER TABLE routing_access_list ADD COLUMN IF NOT EXISTS instance_id INT DEFAULT NULL,
                                ADD KEY IF NOT EXISTS idx_acc_instance (instance_id);

-- 기본 multi 인스턴스 (기존 단일 박스 구성 호환)
INSERT IGNORE INTO cims_instance (id, name, role, description, host, note) VALUES
    (1, 'default-multi', 'multi', '기본 통합 인스턴스 (하위호환)',
     '127.0.0.1', '단일 박스 배포 — 모든 서비스 귀속');

-- ─────────────────────────────────────────────
--  cims_agent — 등록된 서버 에이전트
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_agent (
    id              INT PRIMARY KEY AUTO_INCREMENT,
    agent_token     VARCHAR(64) UNIQUE NOT NULL
                      COMMENT '세션 토큰 (enroll 시 발급, 주기 rotation 가능)',
    enrollment_token VARCHAR(64) DEFAULT NULL
                      COMMENT '최초 등록용 1회용 토큰 (enroll 완료 시 NULL)',
    name            VARCHAR(64) NOT NULL
                      COMMENT '관리용 식별명 (hostname 기본)',
    hostname        VARCHAR(128),
    ip_address      VARCHAR(64),
    os_info         VARCHAR(255),
    cpu_cores       INT,
    memory_mb       INT,
    disk_gb         INT,
    agent_version   VARCHAR(32),
    status          ENUM('pending','approved','online','offline','error','revoked')
                      NOT NULL DEFAULT 'pending'
                      COMMENT 'pending=enroll 전, approved=승인됨, online/offline=heartbeat 기반',
    last_heartbeat  DATETIME DEFAULT NULL,
    last_metric     DATETIME DEFAULT NULL,
    enrolled_at     DATETIME DEFAULT NULL,
    approved_at     DATETIME DEFAULT NULL,
    note            VARCHAR(255),
    create_time     DATETIME DEFAULT CURRENT_TIMESTAMP,
    update_time     DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    KEY idx_agent_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='서버 에이전트 레지스트리';

-- cims_instance 에 agent 바인딩 (한 인스턴스는 하나의 agent 에서 실행)
ALTER TABLE cims_instance
    ADD COLUMN IF NOT EXISTS agent_id INT DEFAULT NULL
        COMMENT '이 인스턴스를 호스팅하는 agent (NULL = 미배치)',
    ADD FOREIGN KEY IF NOT EXISTS fk_inst_agent (agent_id)
        REFERENCES cims_agent(id) ON DELETE SET NULL;

-- ─────────────────────────────────────────────
--  cims_package — 배포 패키지 (tarball)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS cims_package (
    id              INT PRIMARY KEY AUTO_INCREMENT,
    name            VARCHAR(64) NOT NULL
                      COMMENT '패키지명: csp, cmp, csc, cwrtc, phone, console, agent',
    version         VARCHAR(32) NOT NULL,
    file_path       VARCHAR(512) NOT NULL COMMENT 'CSC 서버 내 저장 경로',
    file_size       BIGINT NOT NULL,
    sha256          CHAR(64) NOT NULL,
    description     VARCHAR(255),
    uploaded_by     VARCHAR(128),
    uploaded_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_pkg (name, version),
    KEY idx_pkg_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='배포 패키지 레지스트리';

-- ─────────────────────────────────────────────
--  agent_deployment — (agent × package × instance) 배치
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS agent_deployment (
    id              INT PRIMARY KEY AUTO_INCREMENT,
    agent_id        INT NOT NULL,
    package_id      INT NOT NULL,
    instance_id     INT DEFAULT NULL COMMENT '이 배치가 속한 논리 인스턴스',
    service_kind    VARCHAR(32) COMMENT 'csp/cmp/csc/cwrtc 등',
    status          ENUM('pending','deploying','running','stopped','failed','removed')
                      NOT NULL DEFAULT 'pending',
    install_path    VARCHAR(512) COMMENT '대상 서버 내 설치 경로',
    deployed_at     DATETIME,
    last_job_id     INT,
    note            VARCHAR(255),
    create_time     DATETIME DEFAULT CURRENT_TIMESTAMP,
    update_time     DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (agent_id)    REFERENCES cims_agent(id)    ON DELETE CASCADE,
    FOREIGN KEY (package_id)  REFERENCES cims_package(id)  ON DELETE RESTRICT,
    FOREIGN KEY (instance_id) REFERENCES cims_instance(id) ON DELETE SET NULL,
    KEY idx_dep_agent (agent_id, status),
    KEY idx_dep_instance (instance_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='에이전트 배포 배치';

-- ─────────────────────────────────────────────
--  agent_job — 작업 큐
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS agent_job (
    id              INT PRIMARY KEY AUTO_INCREMENT,
    agent_id        INT NOT NULL,
    job_type        ENUM('install','upgrade','upgrade_agent','uninstall',
                         'start','stop','restart',
                         'update_config','collect_log','exec','health_check')
                      NOT NULL,
    params          TEXT COMMENT 'JSON 파라미터 (package_id, service_kind, config 등)',
    status          ENUM('queued','running','succeeded','failed','cancelled')
                      NOT NULL DEFAULT 'queued',
    result_code     INT,
    result_stdout   MEDIUMTEXT,
    result_stderr   MEDIUMTEXT,
    dispatched_at   DATETIME,
    completed_at    DATETIME,
    create_time     DATETIME DEFAULT CURRENT_TIMESTAMP,
    update_time     DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (agent_id) REFERENCES cims_agent(id) ON DELETE CASCADE,
    KEY idx_job_agent_status (agent_id, status, create_time),
    KEY idx_job_dispatched (status, create_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='에이전트 작업 큐';

-- ─────────────────────────────────────────────
--  agent_metric — 리소스 메트릭 (간단히)
--    작업 큐가 아니므로 PK=agent_id+ts
--    주기적으로 오래된 row 삭제 (운영에서 cron 으로 정리)
-- ─────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS agent_metric (
    agent_id        INT NOT NULL,
    ts              DATETIME NOT NULL,
    cpu_pct         DOUBLE DEFAULT NULL,
    mem_pct         DOUBLE DEFAULT NULL,
    disk_pct        DOUBLE DEFAULT NULL,
    load_avg        VARCHAR(32),
    processes_json  MEDIUMTEXT COMMENT 'JSON: [{name,status,pid,cpu,mem_mb}]',
    PRIMARY KEY (agent_id, ts),
    FOREIGN KEY (agent_id) REFERENCES cims_agent(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='에이전트 리소스 메트릭';

-- ─────────────────────────────────────────────
--  csp_config_audit entity 에 agent/package/deployment 추가 가능
--  (엔티티 필드가 VARCHAR 이라 변경 불필요)
-- ─────────────────────────────────────────────

-- 기존 bootstrap 시드 연결: default-multi 인스턴스 가 단일 agent 없이 운영되는 경우도 허용
SELECT 'Migration complete' AS status,
       (SELECT COUNT(*) FROM cims_instance) AS instances,
       (SELECT COUNT(*) FROM cims_agent)    AS agents;
