-- MCPTT 동시 그룹 호 상한 N6 (TS 24.484 §8.3.2.1 8)e)i) MCPTT user profile <MaxSimultaneousCallsN6> · TS 24.379 §10.1.1.3.1.1 5) 486 103)
--  - mcptt_service_config 에 열 둘을 더한다 — N6 는 사용자마다의 값이고, 값 = 그 사용자가 «관제» 인지(그 PTT 회선의 사람에게
--    역할 배정 role_assignments 가 있는지)로 고른다: 관제 = max_calls_n6_dispatch(기본 10) · 그 밖 = max_calls_n6(기본 5).
--    CSC 는 user profile 문서에 싣고(services.mcptt.user_max_calls_n6), CSP 는 같은 두 값·같은 판정으로 집행한다.
--    사이트 값은 콘솔 «MCPTT 정책»(관리 API PUT /api/v1/mcptt/service-config)에서 바꾼다.
--  - **열 추가만** 한다: 공유 DB(.45·.48·.135)의 옛 CSP·CSC 는 이 열을 읽지 않는다. 새 CSC 는 열이 없으면 기본 10·5 로
--    문서를 내고(바꿀 수만 없다 — 관리 API 의 N6 변경은 400), 새 CSP 도 같은 기본값으로 집행한다.
--  - 재실행 안전 — 열이 이미 있으면 아무것도 하지 않는다.
SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mcptt_service_config' AND COLUMN_NAME = 'max_calls_n6');
SET @ddl := IF(@has = 0,
    'ALTER TABLE mcptt_service_config ADD COLUMN max_calls_n6 SMALLINT NOT NULL DEFAULT 5 COMMENT ''N6 — 동시 그룹 호 상한(관제가 아닌 사용자). user-profile <MaxSimultaneousCallsN6>(TS 24.484 §8.3.2.1), 넘으면 486 103(TS 24.379 §10.1.1.3.1.1)'' AFTER max_affiliations_n2',
    'SELECT 1');
PREPARE s FROM @ddl;
EXECUTE s;
DEALLOCATE PREPARE s;

SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mcptt_service_config' AND COLUMN_NAME = 'max_calls_n6_dispatch');
SET @ddl := IF(@has = 0,
    'ALTER TABLE mcptt_service_config ADD COLUMN max_calls_n6_dispatch SMALLINT NOT NULL DEFAULT 10 COMMENT ''N6 — 동시 그룹 호 상한(관제 = 역할 배정 사용자, mcptt_authorization.md). user-profile <MaxSimultaneousCallsN6>'' AFTER max_calls_n6',
    'SELECT 1');
PREPARE s FROM @ddl;
EXECUTE s;
DEALLOCATE PREPARE s;
