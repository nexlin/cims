-- 일제 통화(broadcast group call)는 그룹 종류가 아니라 호 속성이다 — docs/design/features/mcptt_broadcast_group_call.md §4.3 G2·G3
--  (TS 24.379 §4.12·Annex F.1: 일제 통화 = prearranged + <broadcast-ind>, TS 24.481 §7.2.2 그룹 종류 = on-network-invite-members).
--  ① 그룹별 호 타이머 컬럼 추가 — hang_timer_sec = <on-network-hang-timer>(T4 Inactivity, TS 24.380 §6.3.4.3.5),
--     max_duration_sec = <on-network-maximum-duration>(TNG3 그룹 호 최대 시간, TS 24.379 §6.3.8.1)
--  ② group_type='broadcast' 행 → 'prearranged'(hang_timer_sec=3 — 옛 broadcast 그룹의 고정 hang time 을 이어받음)
--  ③ group_type ENUM 을 ('prearranged','chat') 로 축소
--  재실행 안전(information_schema 확인). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해 — 단 ③ 뒤 구 코드로 broadcast 그룹 생성은 실패한다.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups' AND COLUMN_NAME = 'hang_timer_sec');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_groups
       ADD COLUMN hang_timer_sec INT NOT NULL DEFAULT 30
           COMMENT ''on-network-hang-timer (TS 24.481 §7.2.2 o) — 그룹 호 T4 Inactivity 초 (TS 24.380 §6.3.4.3.5, 0=미사용)''
           AFTER max_talkers',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups' AND COLUMN_NAME = 'max_duration_sec');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_groups
       ADD COLUMN max_duration_sec INT NOT NULL DEFAULT 3600
           COMMENT ''on-network-maximum-duration (TS 24.481 §7.2.7) — 그룹 호 최대 시간 TNG3 초 (0=무제한)''
           AFTER hang_timer_sec',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_groups' AND COLUMN_NAME = 'group_type'
      AND COLUMN_TYPE LIKE '%broadcast%');
SET @sql := IF(@have = 1,
    'UPDATE ptt_groups SET group_type = ''prearranged'', hang_timer_sec = 3 WHERE group_type = ''broadcast''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
SET @sql := IF(@have = 1,
    'ALTER TABLE ptt_groups
       MODIFY COLUMN group_type ENUM(''prearranged'',''chat'') NOT NULL DEFAULT ''prearranged''
           COMMENT ''그룹 종류 — on-network-invite-members (TS 24.481 §7.2.2 a): prearranged=true, chat=false. 일제 통화는 호 속성''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
