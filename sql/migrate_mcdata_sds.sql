-- ============================================================
-- MCData 그룹 메시징(SDS) 게이트 — TS 24.481 그룹문서 요소의 DB 원천.
-- 설계: docs/design/features/mcdata_messaging.md
-- 실행: mysql -u root cims < migrate_mcdata_sds.sql   (또는 cims 계정)
-- MariaDB 10.0+ ADD COLUMN IF NOT EXISTS 사용(재실행 안전).
-- ⚠️ csp 신버전(SelectGroup 이 아래 컬럼 참조)보다 먼저 적용해야 한다.
-- ============================================================
USE cims;

ALTER TABLE ptt_groups
  ADD COLUMN IF NOT EXISTS allow_sds    TINYINT(1) NOT NULL DEFAULT 1
      COMMENT 'mcdata-allow-short-data-service (그룹 SDS 메시징 허용, TS 24.481)',
  ADD COLUMN IF NOT EXISTS allow_fd     TINYINT(1) NOT NULL DEFAULT 0
      COMMENT 'mcdata-allow-file-distribution (그룹 파일전송 허용, TS 24.481 — FD 기능 도입 전 기본 OFF)',
  ADD COLUMN IF NOT EXISTS max_sds_size INT        NOT NULL DEFAULT 10000
      COMMENT 'mcdata-on-network-max-data-size-for-SDS (payload octets, 0=무제한)',
  ADD COLUMN IF NOT EXISTS max_auto_recv INT       NOT NULL DEFAULT 1048576
      COMMENT 'mcdata-on-network-max-data-size-auto-recv (파일 자동 다운로드 임계 octets)';
