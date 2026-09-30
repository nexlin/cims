-- 사용자 MCPTT 프로파일 — 미응답 멤버 알림 수신 자격 (TS 24.379 §6.3.3.3)
--  allow_non_ack_users_info: allow-to-receive-non-acknowledged-users-information (TS 24.484 §8.3.2.1 11)xxxviii)L),
--  표 8.3.2.7-49 — ruleset <actions> 의 <anyExt> 요소, TS 24.483 §5.2.48Z AuthorisedReceiveNonAcknowledged).
--  true = 이 사용자가 그룹 호 개시자일 때, 확인 통화 설정(그룹 문서 <on-network-required> 필수 멤버)이 충족되지 않은 채
--  진행된 호에 대해 controlling MCPTT function 이 응답하지 않은 멤버 목록을 SIP INFO(mcptt-info <non-acknowledged-user>)로
--  보낼 수 있다. 요소 부재 = false(규격 기본값) — 컬럼 기본 0. 부여는 OAM(콘솔·admin API)·관제 앱 관리 범위.
--  재실행 안전(컬럼 존재 시 no-op). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해.
--  위치는 allow_create_group 뒤(migrate_ptt_allow_create_group.sql) — 그 컬럼이 없는 DB 에서는 끝에 붙는다.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_non_ack_users_info');
SET @after := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_create_group') > 0, ' AFTER allow_create_group', '');
SET @sql := IF(@have = 0,
    CONCAT('ALTER TABLE ptt_user_profile
       ADD COLUMN allow_non_ack_users_info TINYINT(1) NOT NULL DEFAULT 0
           COMMENT ''allow-to-receive-non-acknowledged-users-information (TS 24.484 ruleset anyExt, TS 24.483 AuthorisedReceiveNonAcknowledged) — 그룹 호 개시자로서 미응답 멤버 목록(INFO) 수신 자격 (TS 24.379 §6.3.3.3)''',
           @after),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
