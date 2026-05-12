-- ============================================================
-- HA groups — 노드(agent) 묶음 + 모드 (A/S, All Active)
--
--   ha_groups         : 그룹 정의 (이름/모드/VIP/VRID/auth_pass)
--   ha_group_members  : 그룹 멤버 (1 agent = 1 group UNIQUE 강제)
--
-- 정책:
--   - 1 노드 = 1 그룹 (uk_agent UNIQUE)
--   - A/S 노드 = active_standby 가능 모듈만 install (csc/csp/psp 등)
--   - AA 노드 = all_active 가능 모듈만 install (cmp/pmp 등)
--   - standalone 모듈은 어느 그룹이든 OK (cwrtc/cspsim/agent 등)
--   - ha_group 미정의 agent = standalone 만 install
--
-- VRID 는 51-255 range 에서 자동 할당 (uk_vrid UNIQUE), VIP 는 운영자 수동 입력.
--
-- Usage: sudo mysql cims < sql/migrate_ha_groups.sql
-- ============================================================

USE cims;

CREATE TABLE IF NOT EXISTS ha_groups (
    id           INT PRIMARY KEY AUTO_INCREMENT,
    name         VARCHAR(128) NOT NULL,
    mode         ENUM('active_standby', 'all_active') NOT NULL,
    vip          VARCHAR(64) NOT NULL,
    vrid         TINYINT UNSIGNED NOT NULL,
    vip_mask     TINYINT UNSIGNED NOT NULL DEFAULT 24,
    auth_pass    VARCHAR(8) NOT NULL,
    note         VARCHAR(255),
    create_time  DATETIME DEFAULT CURRENT_TIMESTAMP,
    update_time  DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_name (name),
    UNIQUE KEY uk_vrid (vrid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='HA 그룹 — 노드 묶음의 모드/VIP/VRID';

CREATE TABLE IF NOT EXISTS ha_group_members (
    group_id   INT NOT NULL,
    agent_id   INT NOT NULL,
    priority   INT NOT NULL DEFAULT 100,
    role       ENUM('master','backup') NOT NULL DEFAULT 'backup',
    PRIMARY KEY (group_id, agent_id),
    UNIQUE KEY uk_agent (agent_id),
    FOREIGN KEY (group_id) REFERENCES ha_groups(id) ON DELETE CASCADE,
    FOREIGN KEY (agent_id) REFERENCES cims_agent(id) ON DELETE CASCADE,
    INDEX idx_agent (agent_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='HA 그룹 멤버 — 1 agent = 1 group (uk_agent)';
