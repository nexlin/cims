-- mcptt_service_config 의 시스템 스위치 컬럼 제거 (TS 24.484 §8.4 정렬)
--  - service-config 문서(§8.4)에는 allow-private-call·allow-emergency-call·allow-alert·allow-transmit-request·
--    allow-create-delete-group 요소가 없다. 인가 정본은 user profile ruleset(§8.3.2.7)·그룹 문서(TS 24.481)다.
--  - CSC 0.2.133 이상은 이 컬럼을 읽지도 쓰지도 않는다. 옛 CSC 는 SELECT 에 이 컬럼을 넣으므로, 같은 DB 를 쓰는
--    모든 CSC 가 0.2.133 이상이 된 뒤 적용한다(적용 전에도 새 CSC 는 정상 동작).
--  - 재실행 안전(IF EXISTS).
ALTER TABLE mcptt_service_config
    DROP COLUMN IF EXISTS allow_private_call,
    DROP COLUMN IF EXISTS allow_emergency_call,
    DROP COLUMN IF EXISTS allow_alert,
    DROP COLUMN IF EXISTS allow_transmit_request,
    DROP COLUMN IF EXISTS allow_create_delete_group;
