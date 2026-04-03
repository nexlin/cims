-- 녹취 테이블 마이그레이션
-- Usage: sudo mysql cims < sql/migrate_recordings.sql

CREATE TABLE IF NOT EXISTS recordings (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    call_id         VARCHAR(128) NOT NULL,
    call_type       ENUM('voip','ptt') NOT NULL,
    group_id        VARCHAR(64) DEFAULT NULL,
    caller          VARCHAR(64) NOT NULL,
    callee          VARCHAR(64) DEFAULT NULL,
    start_time      DATETIME NOT NULL,
    end_time        DATETIME DEFAULT NULL,
    duration        INT DEFAULT 0,

    -- CMP raw 파일 경로
    raw_path_a      VARCHAR(512) DEFAULT NULL,
    raw_path_b      VARCHAR(512) DEFAULT NULL,
    raw_path_va     VARCHAR(512) DEFAULT NULL,
    raw_path_vb     VARCHAR(512) DEFAULT NULL,

    -- CSC 변환 후 파일 경로
    audio_path      VARCHAR(512) DEFAULT NULL,
    audio_path_a    VARCHAR(512) DEFAULT NULL,
    video_path_a    VARCHAR(512) DEFAULT NULL,
    audio_path_b    VARCHAR(512) DEFAULT NULL,
    video_path_b    VARCHAR(512) DEFAULT NULL,

    -- PTT 집계
    segment_count   INT DEFAULT 0,
    total_speech_ms BIGINT DEFAULT 0,

    -- 메타
    has_video       TINYINT(1) DEFAULT 0,
    file_size       BIGINT DEFAULT 0,
    status          ENUM('raw','transcoding','ready','failed') DEFAULT 'raw',

    INDEX idx_rec_call_id (call_id),
    INDEX idx_rec_start_time (start_time),
    INDEX idx_rec_group_id (group_id),
    INDEX idx_rec_caller (caller)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS recording_segments (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    recording_id    BIGINT NOT NULL,
    seq             INT NOT NULL,
    speaker_id      VARCHAR(64) NOT NULL,
    start_time      DATETIME(3) NOT NULL,
    end_time        DATETIME(3) DEFAULT NULL,
    duration_ms     INT DEFAULT 0,

    -- CMP raw 파일 경로
    raw_audio_path  VARCHAR(512) NOT NULL,
    raw_video_path  VARCHAR(512) DEFAULT NULL,

    -- CSC 변환 후 파일 경로
    audio_path      VARCHAR(512) DEFAULT NULL,
    video_path      VARCHAR(512) DEFAULT NULL,

    has_video       TINYINT(1) DEFAULT 0,
    file_size       INT DEFAULT 0,
    status          ENUM('raw','transcoding','ready','failed') DEFAULT 'raw',

    INDEX idx_seg_recording (recording_id),
    INDEX idx_seg_speaker (speaker_id),
    INDEX idx_seg_time (start_time),
    FOREIGN KEY (recording_id) REFERENCES recordings(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
