-- 사용자 MCPTT 프로파일 v3 — 긴급 사설콜 (TS 24.379 §11 emergency private call / TS 24.484
--  PrivateCall > EmergencyCall > MCPTTPrivateRecipient + ruleset allow-emergency-private-call)
--  - allow_emergency_private_call: 사용자 단위 개시 인가 (사설콜은 그룹문서가 없어 capability
--    축이 공허 — 사용자 축이 유일 게이트).
--  - private_emergency_mode: 긴급 사설콜 대상 결정 — LocallyDetermined(기본, 발신자가 상대 지정)
--    | UsePreConfigured(사전 지정 수신자 고정).
--  - emergency_private_recipient: UsePreConfigured 모드의 수신자 (ptt_subscriptions.id).
--    NULL=미지정 — 그 모드에서는 긴급 사설콜 미인가(403). 가입 해지 시 NULL(FK SET NULL).
--  적용 순서: 본 마이그레이션을 CSP/CSC 코드 배포보다 **선행** 적용한다 (구 코드는 신 컬럼을
--  읽지 않아 무해, 신 코드는 컬럼 부재 시 SELECT 실패 → 프로파일 fail-open 으로 기존 게이트까지
--  풀린다). 재실행 안전(idempotent).

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_emergency_private_call');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_emergency_private_call TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''allow-emergency-private-call (TS 24.484 ruleset) — 긴급 사설콜 개시 인가''
           AFTER emergency_group_id,
       ADD COLUMN private_emergency_mode ENUM(''LocallyDetermined'',''UsePreConfigured'') NOT NULL DEFAULT ''LocallyDetermined''
           COMMENT ''긴급 사설콜 대상 결정 (MCPTTPrivateRecipient entry-info, TS 24.484)''
           AFTER allow_emergency_private_call,
       ADD COLUMN emergency_private_recipient VARCHAR(64) DEFAULT NULL
           COMMENT ''사전 지정 긴급 수신자 (ptt_subscriptions.id) — UsePreConfigured 모드 대상. NULL=미지정(그 모드에선 미인가)''
           AFTER private_emergency_mode,
       ADD CONSTRAINT fk_pup_emg_priv FOREIGN KEY (emergency_private_recipient)
           REFERENCES ptt_subscriptions (id) ON DELETE SET NULL',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
