-- 사용자 MCPTT 프로파일 — 개별 호 인가 (TS 24.484 §8.3.2.1 11) ruleset, 표 8.3.2.7-7·-27·-29)
--  allow_private_call               : <allow-private-call> (TS 24.483 PrivateCall/Authorised) — 개별 호 발신 인가. 요소가 없으면
--      false 로 읽히므로 문서에 늘 싣는다. 참여 기능은 false 면 403 + 107 로 거절한다(TS 24.379 §11.1.1.3.1.1 10)).
--  allow_private_call_to_any_user   : <allow-private-call-to-any-user> (AuthorisedAny) — 상대를 <PrivateCallList> 로 한정하지 않는다.
--      0 이면 목록 밖 상대는 403 + 144(§11.1.1.3.1.1 11)).
--  allow_private_call_participation : <allow-private-call-participation> (EnabledParticipation) — 초대받은 개별 호 참가(착신) 인가.
--      0 이면 착신 참여 기능이 403 + 127(§11.1.1.3.2). 문서의 <allow-to-receive-private-call-from-any-user> 도 같은 값이다.
--  셋 다 기본 1 — 종전 동작(누구나 누구에게나 건다)을 지킨다. 사용자별로 끄는 것은 콘솔 가입자 › PTT 프로파일·관리 API.
--  **열 추가만** 한다: 공유 DB(.45·.48·.135)의 옛 CSP·CSC 는 이 열을 읽지 않는다. 새 CSC 는 열이 없으면 1 로 문서를 내고
--  (바꿀 수만 없다 — 관리 API 400 schema_not_migrated), 새 CSP 도 열이 없으면 허용으로 판정한다. 재실행 안전.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile' AND COLUMN_NAME = 'allow_private_call');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_private_call TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''allow-private-call (TS 24.484 ruleset) — 개별 호 발신 인가 (TS 24.379 §11.1.1.3.1.1 — 0 이면 403 107)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile' AND COLUMN_NAME = 'allow_private_call_to_any_user');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_private_call_to_any_user TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''allow-private-call-to-any-user (TS 24.484 ruleset) — 개별 호 상대를 PrivateCallList 로 한정하지 않는다 (0 이면 목록 밖 403 144)''
           AFTER allow_private_call',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile' AND COLUMN_NAME = 'allow_private_call_participation');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_private_call_participation TINYINT(1) NOT NULL DEFAULT 1
           COMMENT ''allow-private-call-participation (TS 24.484 ruleset) — 개별 호 착신 참가 인가 (TS 24.379 §11.1.1.3.2 — 0 이면 403 127)''
           AFTER allow_private_call_to_any_user',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
