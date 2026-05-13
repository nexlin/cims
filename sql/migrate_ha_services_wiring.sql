-- ============================================================
-- HaServicesPage wiring 용 스키마 확장
--
-- 추가:
--   cims_agent.interfaces_json       — agent 가 heartbeat 시 보고하는 인터페이스 list
--   cims_agent.service_ip_rows_json  — 운영자가 설정한 iface→slot 매핑 (서버 단위)
--   ha_groups.vip_bindings_json      — VIP slot 정의 + 멤버별 iface override (그룹 단위)
--
-- standalone 서비스는 ha_groups 미등록 agent 로 표현 (스키마 변경 없음).
-- service IP rows / interfaces 는 그룹 소속과 무관하게 cims_agent 단위로 저장.
--
-- Usage: sudo mysql cims < sql/migrate_ha_services_wiring.sql
-- ============================================================

USE cims;

ALTER TABLE cims_agent
    ADD COLUMN IF NOT EXISTS interfaces_json TEXT NULL
        COMMENT 'agent heartbeat 보고 인터페이스 list (JSON: [{name,ip,mask,hint?}])',
    ADD COLUMN IF NOT EXISTS service_ip_rows_json TEXT NULL
        COMMENT '운영자 설정 iface→slot 매핑 (JSON: [{iface,ip,mask,slot,status?}])';

ALTER TABLE ha_groups
    ADD COLUMN IF NOT EXISTS vip_bindings_json TEXT NULL
        COMMENT 'VIP slot bindings (JSON: [{bid,slot,ip,mask?,status?,memberIfaces?}])';
