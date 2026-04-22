-- v3 (2026-04-22): call log 는 파일 기반이 SOT.
--   service_log/{volte|ptt}/YYYY/MM/DD/HH/.../*.d/call.json 이 원천이고,
--   CSC `/api/v1/call/logs` 가 디렉토리 스캔으로 조회.
--   아래 테이블은 레거시이며 DROP.

DROP TABLE IF EXISTS volte_call_participants;
DROP TABLE IF EXISTS ptt_call_participants;
DROP TABLE IF EXISTS volte_call_logs;
DROP TABLE IF EXISTS ptt_call_logs;

SELECT 'drop_call_logs applied' AS status;
