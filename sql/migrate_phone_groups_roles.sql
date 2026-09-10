-- 전화 그룹 / 역할 분해 — dispatch_groups(관제 그룹 = 픽업 그룹 + 대표번호 + 감청/관리 범위 한 엔티티) 를
--   phone_groups(유선 전화 기능: 픽업 축 + 대표번호) 와 roles/role_assignments(권한: 능력 + 범위) 로 나눈다.
--   설계 정본: docs/design/features/dispatch_center.md §3·§8.1(전환 표), docs/design/features/mcptt_authorization.md §2·§3.
--
-- 재실행 안전(IF NOT EXISTS / INSERT IGNORE / 존재 검사). 데이터 보존:
--   · phone_groups.id 는 dispatch_groups.id 를 그대로 쓴다(dg-… 유지 — pickup_group 값이자 상관 키, 재키잉 금지)
--   · 범위가 하나라도 none 이 아닌 관제 그룹마다 역할 role-<그룹 id> 를 만들고 그 그룹 전 멤버(person)를 배정한다
--   · ptt_listen≠none 역할 배정자의 ptt_user_profile.allow_ambient_listening 을 1 로 정합한다(컬럼이 있을 때)
--   · 복사가 끝난 뒤 dispatch_groups 계열 4 테이블을 DROP 한다 — 이 마이그레이션은 phone_groups/roles 를 읽는 릴리스와
--     같은 정지창에서 적용한다(구 코드는 테이블 부재를 프로브로 감지해 관제 기능을 비활성한다).
-- 콘솔 계정의 역할 배정은 DB 가 아니라 OAM file_store console_accounts[].role(= roles.id) 이다 — 여기에는 user 행만 쓴다.

-- ── 1. 전화 그룹 ────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS phone_groups (
    id              VARCHAR(64)  NOT NULL COMMENT '불변 키 (CSC 발급 pg-xxxxxxxx; 전환 전 dg- 값 유지) — pickup_group 값·상관 키',
    name            VARCHAR(128) NOT NULL DEFAULT '' COMMENT '표시 이름',
    pilot_id        VARCHAR(64)           DEFAULT NULL COMMENT '대표번호(AoR user part). NULL=대표번호 없음',
    service_ref     VARCHAR(64)           DEFAULT NULL COMMENT '대표번호 접속서비스 name (유선 VoIP)',
    alert_mode      ENUM('parallel','sequential') NOT NULL DEFAULT 'parallel' COMMENT 'TS 24.239 alerting mode',
    no_answer_sec   INT          NOT NULL DEFAULT 30,
    busy_members    ENUM('skip','alert') NOT NULL DEFAULT 'skip',
    overflow_target VARCHAR(64)           DEFAULT NULL COMMENT '무응답 넘김 대상(대표번호/가입 번호). NULL=480',
    org_id          INT                   DEFAULT NULL,
    created_at      DATETIME              DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_pilot (pilot_id),
    CONSTRAINT fk_pg_org FOREIGN KEY (org_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='전화 그룹';

CREATE TABLE IF NOT EXISTS phone_group_members (
    user_id     VARCHAR(64) NOT NULL COMMENT '가입자(회선) id — 가입자당 그룹 하나',
    group_id    VARCHAR(64) NOT NULL,
    alert_order INT         NOT NULL DEFAULT 0 COMMENT 'sequential 호출 순서',
    PRIMARY KEY (user_id),
    KEY idx_group (group_id),
    CONSTRAINT fk_pgm_group FOREIGN KEY (group_id) REFERENCES phone_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='전화 그룹 멤버';

-- ── 2. 역할 ────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS roles (
    id                VARCHAR(64)  NOT NULL COMMENT '불변 키 — 내장 admin|manager|operator|monitor, 관제 role-xxxxxxxx',
    name              VARCHAR(128) NOT NULL DEFAULT '',
    builtin           TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '내장 프리셋(읽기 전용)',
    authz_manage      TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '역할·배정·범위 관리 — 내장 admin/manager 만',
    audit_read        TINYINT(1)   NOT NULL DEFAULT 0,
    directory_write   ENUM('none','own','all') NOT NULL DEFAULT 'none' COMMENT '조직/구성원/번호/전화 그룹 관리 범위 (own=org_id 하위)',
    directory_read    ENUM('none','own','all') NOT NULL DEFAULT 'none',
    ptt_group_manage  ENUM('none','own','scope','all') NOT NULL DEFAULT 'none' COMMENT 'own=본인 소유, scope=directory_write 범위',
    monitor_call      ENUM('none','own','listed','all') NOT NULL DEFAULT 'none' COMMENT '통화 감청·세션 관측·통화 이력/녹취 범위',
    ptt_listen        ENUM('none','listed','all')       NOT NULL DEFAULT 'none' COMMENT 'PTT 청취·conference 구독·PTT 이력/녹취 범위',
    listen_visibility ENUM('hidden','visible')          NOT NULL DEFAULT 'hidden' COMMENT 'PTT 청취 멤버 로스터 노출',
    history_read      ENUM('none','scope','all')        NOT NULL DEFAULT 'none' COMMENT 'scope=monitor_call/ptt_listen 범위',
    alarm_ack         TINYINT(1)   NOT NULL DEFAULT 0,
    mcptt_control     TINYINT(1)   NOT NULL DEFAULT 0,
    org_id            INT                   DEFAULT NULL COMMENT 'own 범위의 루트',
    created_at        DATETIME              DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    CONSTRAINT fk_role_org FOREIGN KEY (org_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='역할';

CREATE TABLE IF NOT EXISTS role_assignments (
    principal_type ENUM('console','user') NOT NULL COMMENT 'console=OAM 콘솔 계정(login_id), user=가입자 person(users.id)',
    principal_id   VARCHAR(64) NOT NULL,
    role_id        VARCHAR(64) NOT NULL,
    PRIMARY KEY (principal_type, principal_id),
    KEY idx_role (role_id),
    CONSTRAINT fk_ra_role FOREIGN KEY (role_id) REFERENCES roles (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='역할 배정 (사람당 하나)';

CREATE TABLE IF NOT EXISTS role_monitor_targets (
    role_id        VARCHAR(64) NOT NULL,
    phone_group_id VARCHAR(64) NOT NULL,
    PRIMARY KEY (role_id, phone_group_id),
    CONSTRAINT fk_rmt_role FOREIGN KEY (role_id)        REFERENCES roles (id)        ON DELETE CASCADE,
    CONSTRAINT fk_rmt_pg   FOREIGN KEY (phone_group_id) REFERENCES phone_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='monitor_call=listed 대상';

CREATE TABLE IF NOT EXISTS role_ptt_targets (
    role_id      VARCHAR(64) NOT NULL,
    ptt_group_id BIGINT      NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    PRIMARY KEY (role_id, ptt_group_id),
    CONSTRAINT fk_rpt_role FOREIGN KEY (role_id)      REFERENCES roles (id)      ON DELETE CASCADE,
    CONSTRAINT fk_rpt_ptt  FOREIGN KEY (ptt_group_id) REFERENCES ptt_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='ptt_listen=listed 대상';

-- ── 3. 내장 프리셋 4행 (mcptt_authorization.md §3 매트릭스) — 항상 존재, 값은 코드가 정본 ──
INSERT INTO roles (id, name, builtin, authz_manage, audit_read, directory_write, directory_read, ptt_group_manage,
                   monitor_call, ptt_listen, listen_visibility, history_read, alarm_ack, mcptt_control) VALUES
    ('admin',    '관리자',      1, 1, 1, 'all',  'all', 'all',  'none', 'none', 'hidden', 'all', 1, 1),
    ('manager',  '운영 관리자', 1, 1, 1, 'all',  'all', 'all',  'none', 'none', 'hidden', 'all', 1, 1),
    ('operator', '운용자',      1, 0, 0, 'none', 'all', 'own',  'none', 'none', 'hidden', 'all', 1, 1),
    ('monitor',  '모니터',      1, 0, 0, 'none', 'all', 'none', 'none', 'none', 'hidden', 'all', 0, 0)
ON DUPLICATE KEY UPDATE
    name = VALUES(name), builtin = 1, authz_manage = VALUES(authz_manage), audit_read = VALUES(audit_read),
    directory_write = VALUES(directory_write), directory_read = VALUES(directory_read), ptt_group_manage = VALUES(ptt_group_manage),
    monitor_call = VALUES(monitor_call), ptt_listen = VALUES(ptt_listen), listen_visibility = VALUES(listen_visibility),
    history_read = VALUES(history_read), alarm_ack = VALUES(alarm_ack), mcptt_control = VALUES(mcptt_control);

-- ── 4. 전환 — dispatch_groups 계열이 있을 때만 ──────────────────────────────────
SET @have_dg := (SELECT COUNT(*) FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dispatch_groups');
SET @have_da := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dispatch_groups' AND COLUMN_NAME = 'directory_admin');
SET @have_amb := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile' AND COLUMN_NAME = 'allow_ambient_listening');
SET @have_voip := (SELECT COUNT(*) FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'voip_subscriptions');
-- 회선 → person 펼침 서브쿼리 — 가입 테이블 전부(voip_subscriptions 는 migrate_voip_subscriptions.sql 적용 뒤에만 있다)
SET @lines := CONCAT('(SELECT id, user_id FROM volte_subscriptions',
                     IF(@have_voip > 0, ' UNION ALL SELECT id, user_id FROM voip_subscriptions', ''),
                     ' UNION ALL SELECT id, user_id FROM ptt_subscriptions)');

-- 4a. 전화 그룹 (전화 열만, id 유지)
SET @sql := IF(@have_dg > 0,
    'INSERT IGNORE INTO phone_groups (id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, overflow_target, org_id, created_at)
     SELECT id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, overflow_target, org_id, created_at FROM dispatch_groups',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@have_dg > 0,
    'INSERT IGNORE INTO phone_group_members (user_id, group_id, alert_order)
     SELECT m.user_id, m.group_id, m.alert_order FROM dispatch_group_members m JOIN phone_groups g ON g.id = m.group_id',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4b. 역할 — 범위가 하나라도 있는 그룹마다 role-<그룹 id> (directory_admin 컬럼은 있을 때만 옮긴다)
SET @dw := IF(@have_da > 0, 'COALESCE(directory_admin, ''none'')', '''none''');
SET @sql := IF(@have_dg > 0, CONCAT(
    'INSERT IGNORE INTO roles (id, name, builtin, directory_write, monitor_call, ptt_listen, listen_visibility, history_read, org_id, created_at)
     SELECT CONCAT(''role-'', id), name, 0, ', @dw, ', monitor_scope, ptt_listen, listen_visibility, ''scope'', org_id, created_at
       FROM dispatch_groups
      WHERE monitor_scope <> ''none'' OR ptt_listen <> ''none'' OR ', @dw, ' <> ''none'''),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4c. 배정 — 그 그룹 멤버(회선)의 person 에게. 멤버 행은 유선 회선(voip — 전환 전엔 volte 행)이지만 어느 가입 테이블의
--     회선이 섞였어도 person 으로 해석한다.
SET @sql := IF(@have_dg > 0, CONCAT(
    'INSERT IGNORE INTO role_assignments (principal_type, principal_id, role_id)
     SELECT ''user'', CAST(p.user_id AS CHAR), r.id
       FROM dispatch_group_members m
       JOIN roles r ON r.id = CONCAT(''role-'', m.group_id)
       JOIN ', @lines, ' p ON p.id = m.user_id'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4d. 대상 목록
SET @sql := IF(@have_dg > 0,
    'INSERT IGNORE INTO role_monitor_targets (role_id, phone_group_id)
     SELECT CONCAT(''role-'', t.group_id), t.target_group_id FROM dispatch_group_monitor_targets t
       JOIN roles r ON r.id = CONCAT(''role-'', t.group_id) JOIN phone_groups g ON g.id = t.target_group_id',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@have_dg > 0,
    'INSERT IGNORE INTO role_ptt_targets (role_id, ptt_group_id)
     SELECT CONCAT(''role-'', t.group_id), t.ptt_group_id FROM dispatch_group_ptt_targets t
       JOIN roles r ON r.id = CONCAT(''role-'', t.group_id)',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4e. 청취 자격 정합 — ptt_listen≠none 역할 배정자의 PTT 프로파일 allow_ambient_listening=1 (mcptt_authorization.md §2.4)
SET @sql := IF(@have_amb > 0,
    'UPDATE ptt_user_profile pr
       JOIN ptt_subscriptions ps ON ps.id = pr.ptt_id
       JOIN role_assignments ra ON ra.principal_type = ''user'' AND ra.principal_id = CAST(ps.user_id AS CHAR)
       JOIN roles r ON r.id = ra.role_id AND r.ptt_listen <> ''none''
        SET pr.allow_ambient_listening = 1',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4f. 구 테이블 제거 (복사 완료 후). FK 순서: 대상 → 멤버 → 그룹
DROP TABLE IF EXISTS dispatch_group_ptt_targets;
DROP TABLE IF EXISTS dispatch_group_monitor_targets;
DROP TABLE IF EXISTS dispatch_group_members;
DROP TABLE IF EXISTS dispatch_groups;
