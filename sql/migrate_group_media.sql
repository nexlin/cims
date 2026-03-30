-- Migration: ptt_groups 테이블 미디어 설정 정리
-- audio_codec 컬럼 제거 (서버 설정으로 분리 예정)
-- video_enabled 컬럼 추가 (그룹별 H.264 비디오 릴레이 활성화)

ALTER TABLE ptt_groups
    DROP COLUMN IF EXISTS audio_codec,
    ADD COLUMN IF NOT EXISTS video_enabled TINYINT(1) NOT NULL DEFAULT 0
        COMMENT 'H.264 비디오 릴레이 활성화';
