-- ============================================================
-- 그룹 floor 정책(동시 발언 수) — TS 24.380 dual/multi-talker 의 DB 원천.
-- 설계: docs/design/features/mcptt_csp_cmp_roadmap_contract.md §B.1
-- 계약: docs/api/cmp_media_api.md §7.1, §7.7
-- 실행: mysql -u root cims < migrate_ptt_floor_policy.sql   (또는 cims 계정)
-- MariaDB 10.0+ ADD COLUMN IF NOT EXISTS 사용(재실행 안전).
-- ⚠️ csp 신버전(SelectGroup 이 아래 컬럼 참조)보다 먼저 적용해야 한다.
--
-- max_talkers 기본 2 — floor_policy 만 'multi' 로 바꿔도 CMP 계약(2..8)을 만족한다.
--   CMP 는 multi 인데 범위 밖이면 BAD_REQUEST 로 그룹 생성 자체를 거절하므로(통화 불가),
--   ENUM + 유효 기본값으로 잘못된 조합이 DB 에 들어가지 못하게 막는다.
-- ============================================================
USE cims;

ALTER TABLE ptt_groups
  ADD COLUMN IF NOT EXISTS floor_policy ENUM('single','dual','multi') NOT NULL DEFAULT 'single'
      COMMENT 'TS 24.380 동시 발언 정책 (single=단일 화자, dual=2인째는 선점 자격자만, multi=max_talkers 명)',
  ADD COLUMN IF NOT EXISTS max_talkers  INT NOT NULL DEFAULT 2
      COMMENT 'floor_policy=multi 일 때 동시 발언 상한 (CMP 계약 범위 2..8)';
