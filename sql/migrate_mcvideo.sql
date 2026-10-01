-- MCVideo 서비스 표 — docs/design/features/mcvideo.md §5.1 · §8 (계약 K1, docs/dev/mcvideo_dev_plan.md §3)
--  한 그룹 = 서비스 집합(TS 23.280 §3). MCVideo 는 MCPTT 의 확장이 아니라 나란한 서비스라 서비스별 표를 둔다(mcvideo.md §7 D4):
--    · mcvideo_group_attrs   — 행이 있으면 그 그룹은 MCVideo 그룹이다(TS 24.481 §7.2.2 MCVideo <service>). 열 = MCVideo <list-service> 속성.
--    · mcvideo_user_profile  — 행이 그 PTT 회선의 MCVideo 이용 자격이다(TS 24.484 §9.3 MCVideo user profile). MCVideo ID = MCPTT ID(§7 D1).
--    · mcvideo_affiliations  — MCVideo 서비스의 affiliation(TS 24.281 §8 — 서비스별, TS 23.280 §5.2.5). ptt_affiliations 와 같은 모양.
--  **표 추가만** 한다 — 공유 DB(.45·.48·.135)의 옛 CSP·CSC·OAM 은 새 표를 읽지 않고, 옛 표(ptt_affiliations 등)를 지우는 옛 코드가
--  MCVideo 행을 건드리지 못한다. ptt_groups.video_enabled 는 전환기 열로 남기고 전 사이트가 새 빌드가 된 뒤 DROP 한다(mcvideo.md §8 3).
--  재실행 안전(CREATE IF NOT EXISTS · INSERT IGNORE). 새 CSC·CSP 가 이 표를 SELECT 하므로 배포 전에 적용한다.

CREATE TABLE IF NOT EXISTS mcvideo_group_attrs (
    group_id                     BIGINT       NOT NULL COMMENT 'ptt_groups.id (surrogate) — 행 = 이 그룹이 MCVideo 그룹 (TS 24.481 §7.2.2)',
    invite_members               TINYINT(1)   NOT NULL DEFAULT 0
        COMMENT 'mcvideo-on-network-invite-members — 1=prearranged, 0=chat (TS 24.481 §7.2.8, 없음=chat). 기본 chat (mcvideo.md §7 D5)',
    max_duration_sec             INT          NOT NULL DEFAULT 3600
        COMMENT 'mcvideo-on-network-maximum-duration — 그룹 호 최대 시간 TNG3 초 (TS 24.281 §6.3.3.5, 0=무제한 → 요소 생략)',
    max_transmitters             INT          NOT NULL DEFAULT 2
        COMMENT 'mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members — 동시 송출 상한 (TS 24.581 §4.1.1.1·§6.3.4)',
    audio_encodings              VARCHAR(128) NOT NULL DEFAULT 'AMR-WB'
        COMMENT 'mcvideo-preferred-audio-encodings — rtpmap encoding name, 쉼표 구분 선호순 (TS 24.481 §7.2.2 e)',
    video_encodings              VARCHAR(128) NOT NULL DEFAULT 'H264'
        COMMENT 'mcvideo-preferred-video-encodings — rtpmap encoding name, 쉼표 구분 선호순 (TS 24.481 §7.2.2 f)',
    video_resolutions            VARCHAR(128)          DEFAULT NULL
        COMMENT 'mcvideo-preferred-video-resolutions — 가로x세로 선호순 문자열 (예 1280x720,640x480). NULL=요소 생략',
    video_frame_rate             VARCHAR(64)           DEFAULT NULL
        COMMENT 'mcvideo-preferred-video-frame-rate — 초당 프레임 선호순 문자열 (예 30,15). NULL=요소 생략',
    reception_hang_timer_sec     INT          NOT NULL DEFAULT 30
        COMMENT 'on-network-reception-hang-timer — 수신 비활성 T5 초 (TS 24.581 §11.1.3, 0=요소 생략)',
    min_number_to_start          INT          NOT NULL DEFAULT 0
        COMMENT 'mcvideo-on-network-minimum-number-to-start (TS 24.481 §7.2.2 n)',
    group_priority               SMALLINT              DEFAULT NULL
        COMMENT 'mcvideo-on-network-group-priority 0..255 — 높을수록 높다 (TS 24.481 §7.2.8). NULL=요소 생략(가장 낮음)',
    protect_media                TINYINT(1)   NOT NULL DEFAULT 0
        COMMENT 'mcvideo-protect-media — 요소가 없으면 true(GMK 보호) 로 읽혀 늘 명시한다. E2E 전까지 0 (mcvideo.md §7 D7)',
    protect_transmission_control TINYINT(1)   NOT NULL DEFAULT 0
        COMMENT 'mcvideo-protect-transmission-control — 없으면 true 로 읽혀 늘 명시. E2E 전까지 0 (mcvideo.md §7 D7)',
    allow_conference_state       TINYINT(1)   NOT NULL DEFAULT 1
        COMMENT 'mcvideo-on-network-allow-conference-state — conference 이벤트 구독 허용 (TS 24.481 §7.2.8, 없음=false)',
    update_time                  DATETIME              DEFAULT NULL,
    PRIMARY KEY (group_id),
    CONSTRAINT fk_mvga_group FOREIGN KEY (group_id) REFERENCES ptt_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCVideo 그룹 속성 — 행 = 그 그룹이 MCVideo 서비스용 (TS 24.481 §7.2.2)';

CREATE TABLE IF NOT EXISTS mcvideo_user_profile (
    ptt_id            VARCHAR(64) NOT NULL COMMENT 'ptt_subscriptions.id — 행 = 이 회선의 MCVideo 이용 자격. MCVideo ID = MCPTT ID (mcvideo.md §7 D1)',
    max_video_streams TINYINT     NOT NULL DEFAULT 1
        COMMENT '<OnNetwork><MaxSimultaneousVideoStreams> (TS 24.484 §9.3.2.1 9e) — 수신 동시 스트림 상한, 서버 카운터 C9 (TS 24.581 §11.2.3). 1차 단말 = 1',
    max_calls_n6      TINYINT     NOT NULL DEFAULT 1
        COMMENT '<Common><MCVideo-group-call><MaxSimultaneousCallsN6> (TS 24.484 §9.3.2.1 8e i) — 동시 MCVideo 그룹 호 상한 (TS 24.281 §9.2.2.3.1.1 5)',
    max_affiliations_n2 SMALLINT  NOT NULL DEFAULT 4
        COMMENT '<OnNetwork><MaxAffiliationsN2> (TS 24.484 §9.3.2.1) — 동시 MCVideo 제휴 그룹 상한 N2 (TS 24.281 §8.2.2.2.3 14)c)·§9.2.2.3.1.1 7) 486 102). 기존 DB 는 migrate_mcvideo_n2.sql',
    update_time       DATETIME             DEFAULT NULL,
    PRIMARY KEY (ptt_id),
    CONSTRAINT fk_mvup_ptt_sub FOREIGN KEY (ptt_id) REFERENCES ptt_subscriptions (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCVideo user profile — 행 = MCVideo 이용 자격 (TS 24.484 §9.3)';

CREATE TABLE IF NOT EXISTS mcvideo_affiliations (
    group_id      BIGINT       NOT NULL COMMENT 'ptt_groups.id (surrogate) — MCVideo 그룹',
    user_id       VARCHAR(64)  NOT NULL COMMENT 'ptt_group_members.user_id',
    client_id     VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP instance (Contact +sip.instance)',
    affiliated_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'affiliation 시각',
    expires_at    DATETIME              DEFAULT NULL COMMENT 'affiliation 만료 (NULL=dereg 시까지)',
    status        ENUM('affiliated','deaffiliated') NOT NULL DEFAULT 'affiliated' COMMENT '상태',
    PRIMARY KEY (group_id, user_id, client_id),
    KEY idx_mvaff_user (user_id),
    KEY idx_mvaff_group_status (group_id, status),
    CONSTRAINT fk_mvaff_group FOREIGN KEY (group_id) REFERENCES ptt_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='MCVideo affiliation 상태 (TS 24.281 §8 — MCPTT 와 따로, TS 23.280 §5.2.5)';

-- 전환(mcvideo.md §8 1) — 현행 «PTT 영상» 그룹(video_enabled=1)은 MCPTT + MCVideo 그룹이 된다. 속성은 기본값(chat·송출 상한 2).
INSERT IGNORE INTO mcvideo_group_attrs (group_id, update_time)
    SELECT id, NOW() FROM ptt_groups WHERE video_enabled = 1;

-- 현행 동작 보존 — 지금은 PTT 회선이면 영상 그룹에서 누구나 영상을 쓴다. 그래서 기존 PTT 회선 전부에 MCVideo 자격 행을 둔다.
--  이후 새 회선의 자격은 관리 API·콘솔(가입자 PTT 회선 «MCVideo»)이 정한다.
INSERT IGNORE INTO mcvideo_user_profile (ptt_id, update_time)
    SELECT id, NOW() FROM ptt_subscriptions;
