-- MCVideo 동시 제휴 상한 N2 (TS 24.484 §9.3 MCVideo user profile <MaxAffiliationsN2> · TS 24.281 §8.2.2.2.3 14)c))
--  - mcvideo_user_profile 에 열 하나를 더한다 — 회선마다 운용 중 바꾼다(콘솔 가입자 › PTT 회선 › MCVideo 이용 자격,
--    관리 API PUT /api/v1/users/{pid}/ptt/{msisdn}/mcvideo). 기본 4 — 기존 행도 4 가 된다.
--  - MCPTT 의 N2(mcptt_service_config.max_affiliations_n2)와 따로다 — 서비스마다 제휴가 따로다(mcvideo.md §5.1).
--  - **열 추가만** 한다: 공유 DB(.45·.48·.135)의 옛 CSP·CSC 는 이 열을 읽지 않는다. 새 CSP·CSC 는 열이 없으면 기본 4 로
--    집행하고(바꿀 수만 없다), 관리 API 의 N2 변경은 400 schema_not_migrated 다.
--  - 재실행 안전 — 열이 이미 있으면 아무것도 하지 않는다.
SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mcvideo_user_profile' AND COLUMN_NAME = 'max_affiliations_n2');
SET @ddl := IF(@has = 0,
    'ALTER TABLE mcvideo_user_profile ADD COLUMN max_affiliations_n2 SMALLINT NOT NULL DEFAULT 4 COMMENT ''<OnNetwork><MaxAffiliationsN2> (TS 24.484 §9.3.2.1) — 동시 MCVideo 제휴 그룹 상한 N2 (TS 24.281 §8.2.2.2.3 14)c)·§8.2.2.2.15 9)c)·§9.2.2.3.1.1 7) 486 102). 기본 4'' AFTER max_calls_n6',
    'SELECT 1');
PREPARE s FROM @ddl;
EXECUTE s;
DEALLOCATE PREPARE s;
