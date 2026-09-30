-- 사용자 MCPTT 프로파일 — 긴급·임박·경보 **해제** 인가 (TS 24.484 §8.3.2.1 11) ruleset, 표 8.3.2.7-14·-17·-19)
--  allow_cancel_group_emergency : <allow-cancel-group-emergency> (TS 24.483 CancelMCPTTGroup) — 그룹의 진행 중 긴급 상태
--      해제 인가. 서버 판정은 local policy(TS 24.379 §6.3.3.1.13.4 — 예시: 관제사·개시자)로, CIMS 정책 = 개시자 ∨ 이 값.
--      기본 0 — 개시자만 해제한다(관제사에게 켠다).
--  allow_cancel_imminent_peril  : <allow-cancel-imminent-peril> (TS 24.483 ImminentPerilCall/Cancel) — 임박 위험 해제 인가
--      (§6.3.3.1.13.6 — 이 값만으로 판정, 개시자 예외 없음). 기본 1 — 0 이면 개시자도 풀지 못한다.
--  allow_cancel_emergency_alert : <allow-cancel-emergency-alert> (TS 24.483 EmergencyAlert/Cancel) — 긴급 경보 취소 인가
--      (§6.3.3.1.13.3). 새로 붙일 때 allow_emergency_alert 값을 옮긴다 — 그 전 문서는 발령 값을 취소 값으로 냈다.
--  재실행 안전(컬럼 존재 시 no-op). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해.
--  위치는 allow_emergency_alert 뒤 — 규격 목록 순서(해제 인가는 발령 인가 옆).

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_cancel_group_emergency');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_cancel_group_emergency TINYINT(1) NOT NULL DEFAULT 0
           COMMENT ''allow-cancel-group-emergency (TS 24.484 ruleset) — 그룹 진행 중 긴급 상태 해제 인가 (TS 24.379 §6.3.3.1.13.4 local policy = 개시자 ∨ 이 값)''
           AFTER allow_emergency_alert',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_cancel_imminent_peril');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_cancel_imminent_peril TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''allow-cancel-imminent-peril (TS 24.484 ruleset) — 임박 위험 해제 인가 (TS 24.379 §6.3.3.1.13.6)''
           AFTER allow_cancel_group_emergency',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_cancel_emergency_alert');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_cancel_emergency_alert TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''allow-cancel-emergency-alert (TS 24.484 ruleset) — 긴급 경보 취소 인가 (TS 24.379 §6.3.3.1.13.3)''
           AFTER allow_cancel_imminent_peril',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
-- 새로 붙인 경우만 — 그 전 문서의 <allow-cancel-emergency-alert> 값(= allow_emergency_alert)을 잇는다
SET @sql := IF(@have = 0,
    'UPDATE ptt_user_profile SET allow_cancel_emergency_alert = allow_emergency_alert',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
