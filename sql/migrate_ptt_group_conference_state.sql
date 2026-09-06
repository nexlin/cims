-- PTT 그룹 — conference 이벤트 구독 허용 (docs/design/features/dispatch_center.md §5.6, TS 24.379 §10.1.3.4.1)
--  allow_conference_state: <on-network-allow-conference-state> (TS 24.481 §7.2.4.2, 그룹 문서 <cp:actions> 요소) —
--  그룹 멤버가 그룹 세션의 conference 이벤트 패키지(RFC 4575)를 구독할 수 있는가. CSP 가 초기 SUBSCRIBE 에서
--  판정하고 불허 시 403 + Warning 138. 관제사(비멤버)의 구독은 청취 범위(allow_ambient_listening + ptt_listen)
--  로 별도 인가되므로 이 값과 무관. 기본 1 = 종전 동작(멤버 구독 허용) 유지.
--  재실행 안전(컬럼 존재 시 no-op). ⚠ CSP/CSC 가 SELECT 컬럼으로 읽으므로 신 버전 기동 전에 적용한다.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups'
      AND COLUMN_NAME = 'allow_conference_state');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_groups
       ADD COLUMN allow_conference_state TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''on-network-allow-conference-state (TS 24.481 §7.2.4.2) — 멤버의 conference 이벤트(RFC 4575) 구독 허용''
           AFTER emergency_alert',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
