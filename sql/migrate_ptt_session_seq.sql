-- PTT 그룹 세션 시퀀스 추가
-- 그룹 재시작마다 session_seq 증가, Flow의 subid로 사용
ALTER TABLE ptt_groups ADD COLUMN IF NOT EXISTS session_seq INT NOT NULL DEFAULT 1;
