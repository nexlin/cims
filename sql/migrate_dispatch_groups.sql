-- 관제 그룹 (dispatch group) — docs/design/features/dispatch_center.md §3·§8.1
--  관제 그룹 = 픽업 그룹 + (선택) 대표번호 + (선택) 감청 범위. 기존 volte_subscriptions.pickup_group
--  축을 이 엔티티의 불변 id 로 채워 당겨받기·BLF 인가·대표번호 병렬 호출·감청 범위가 한 축을 공유한다.
--  - dispatch_groups            : 그룹 본체. id 는 CSC 발급 불변 키(dg-xxxxxxxx) — 이름은 어떤 키에도 쓰지 않는다.
--  - dispatch_group_members     : 가입자당 그룹 하나(user_id PK). CSC 가 멤버 변경 시 pickup_group 을 함께 갱신(SoT=멤버십).
--  - dispatch_group_monitor_targets : monitor_scope=listed 의 감청 대상 그룹.
--  - dispatch_group_ptt_targets     : ptt_listen=listed 의 청취 대상 PTT 그룹.
--  기존 pickup_group 자유 문자열 값은 같은 id 의 관제 그룹(대표번호 없음)으로 승격해 축을 맞춘다(하단).
--  재실행 안전(idempotent). CSP 는 부팅 프로브로 테이블 부재를 감지해 관제 기능을 비활성(INFO)한다.

CREATE TABLE IF NOT EXISTS dispatch_groups (
    id              VARCHAR(64)  NOT NULL COMMENT '불변 키 (CSC 발급 dg-xxxxxxxx) — pickup_group 값·상관 키',
    name            VARCHAR(128) NOT NULL DEFAULT '' COMMENT '표시 이름',
    pilot_id        VARCHAR(64)           DEFAULT NULL COMMENT '대표번호(AoR user part). NULL=대표번호 없음',
    service_ref     VARCHAR(64)           DEFAULT NULL COMMENT '대표번호 접속서비스 name',
    alert_mode      ENUM('parallel','sequential') NOT NULL DEFAULT 'parallel' COMMENT 'TS 24.239 alerting mode',
    no_answer_sec   INT          NOT NULL DEFAULT 30 COMMENT '전원 무응답 판정 초',
    busy_members    ENUM('skip','alert') NOT NULL DEFAULT 'skip' COMMENT '통화 중 그룹원 호출 여부',
    overflow_target VARCHAR(64)           DEFAULT NULL COMMENT '무응답 넘김 대상(대표번호/내선). NULL=480',
    monitor_scope   ENUM('none','own','listed','all') NOT NULL DEFAULT 'none' COMMENT '합법감청(dialog 감시·Join) 범위',
    ptt_listen      ENUM('none','listed','all')       NOT NULL DEFAULT 'none' COMMENT 'PTT 그룹콜 청취 범위',
    listen_visibility ENUM('hidden','visible')        NOT NULL DEFAULT 'hidden' COMMENT 'PTT 청취 멤버 로스터 노출',
    org_id          INT                   DEFAULT NULL COMMENT '소속 조직 (콘솔 필터·RBAC 스코프)',
    created_at      DATETIME              DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME              DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_dg_pilot (pilot_id),
    KEY idx_dg_org (org_id),
    CONSTRAINT fk_dg_org FOREIGN KEY (org_id) REFERENCES organizations (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='관제 그룹 (픽업 그룹+대표번호+감청 범위)';

CREATE TABLE IF NOT EXISTS dispatch_group_members (
    user_id     VARCHAR(64) NOT NULL COMMENT '가입자 id (volte_subscriptions.id) — 가입자당 그룹 하나',
    group_id    VARCHAR(64) NOT NULL COMMENT 'dispatch_groups.id',
    alert_order INT         NOT NULL DEFAULT 0 COMMENT 'sequential 호출 순서 (MaxForkTargets 절삭 순서)',
    PRIMARY KEY (user_id),
    KEY idx_dgm_group (group_id),
    CONSTRAINT fk_dgm_group FOREIGN KEY (group_id) REFERENCES dispatch_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='관제 그룹 멤버';

CREATE TABLE IF NOT EXISTS dispatch_group_monitor_targets (
    group_id        VARCHAR(64) NOT NULL COMMENT '감청 주체 그룹',
    target_group_id VARCHAR(64) NOT NULL COMMENT '감청 대상 그룹',
    PRIMARY KEY (group_id, target_group_id),
    CONSTRAINT fk_dgt_group  FOREIGN KEY (group_id)        REFERENCES dispatch_groups (id) ON DELETE CASCADE,
    CONSTRAINT fk_dgt_target FOREIGN KEY (target_group_id) REFERENCES dispatch_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='monitor_scope=listed 대상';

CREATE TABLE IF NOT EXISTS dispatch_group_ptt_targets (
    group_id     VARCHAR(64) NOT NULL COMMENT '청취 주체 관제 그룹',
    ptt_group_id BIGINT      NOT NULL COMMENT 'ptt_groups.id (surrogate)',
    PRIMARY KEY (group_id, ptt_group_id),
    CONSTRAINT fk_dgp_group FOREIGN KEY (group_id)     REFERENCES dispatch_groups (id) ON DELETE CASCADE,
    CONSTRAINT fk_dgp_ptt   FOREIGN KEY (ptt_group_id) REFERENCES ptt_groups (id)      ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='ptt_listen=listed 대상';

-- 기존 pickup_group 자유 문자열 → 같은 id 의 관제 그룹(대표번호 없음)으로 승격 + 멤버십 파생.
--  volte 가입자만 대상(관제 소프트폰 축). 이미 존재하는 그룹/멤버는 건드리지 않는다(재실행 안전).
INSERT IGNORE INTO dispatch_groups (id, name, org_id)
    SELECT DISTINCT s.pickup_group, s.pickup_group, u.org_id
      FROM volte_subscriptions s
      LEFT JOIN users u ON u.id = s.user_id
     WHERE s.pickup_group IS NOT NULL AND s.pickup_group <> '';

INSERT IGNORE INTO dispatch_group_members (user_id, group_id)
    SELECT s.id, s.pickup_group
      FROM volte_subscriptions s
     WHERE s.pickup_group IS NOT NULL AND s.pickup_group <> '';
