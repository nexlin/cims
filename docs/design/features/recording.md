# CIMS 통화 녹취 설계서

> 버전: 1.2 (2026-04-03)
> 상위 문서: [04_Monitoring_Statistics.md](04_Monitoring_Statistics.md) Part 2 참조
> 이 문서는 녹취의 CMP 구현 상세(RTP 덤프, 파일 형식)를 다룸. UI/API는 상위 문서 Part 2에 통합.

---

## 1. 개요

VoIP 1:1 통화 및 PTT 그룹콜의 음성·영상을 녹취하고, Console UI를 통해 조회·재생할 수 있는 기능.

### 설계 원칙
- **CMP는 raw RTP 저장만 담당** — 비동기 파일 I/O로 서비스 부하 최소화
- **트랜스코딩은 CSC에서 on-demand** — API 요청 시 미변환 파일이면 그때 변환
- **발언 단위 세그먼트 (PTT)** — Floor 이벤트 기준으로 분리, 무음 구간 미저장
- **음성+영상 동기화** — 하나의 MP4 컨테이너에 mux

---

## 2. 아키텍처

```
┌─────────┐          ┌─────────┐                ┌─────────────────┐
│  단말    │──RTP───→│   CSP   │── add ────────→│      CMP        │
│ (A, B)  │          │         │  (record=true)  │                 │
└─────────┘          └────┬────┘                 │  raw RTP 저장    │
                          │                      │  (비동기 I/O)    │
                          │                      └────────┬────────┘
                          │                               │
                          │                               ▼
                          │                      ┌───────────────┐
                          │                      │  raw 파일      │
                          │                      │  /recordings/  │
                          │                      │    raw/        │
                          ▼                      └───────┬───────┘
                     ┌─────────┐                         │
                     │   DB    │  메타데이터              │
                     │ MariaDB │  (status=raw)            │
                     └────┬────┘                         │
                          │                              │
                     ┌────┴────┐   재생 요청 시           │
                     │   CSC   │←────────────────────────┘
                     │ (Python)│
                     │         │── ffmpeg 변환 (on-demand)
                     │         │── 변환 완료 → status=ready
                     │         │── 스트리밍 응답
                     └────┬────┘
                          │
                     ┌────┴────┐
                     │Console  │  재생 플레이어
                     │  UI     │
                     └─────────┘
```

### 역할 분리

| 컴포넌트 | 역할 | CPU 부하 |
|----------|------|----------|
| **CMP** | raw RTP 패킷을 파일에 덤프 (비동기 write) | 최소 (I/O only) |
| **CSP** | DB에 녹취 메타데이터 생성 (통화 시작/종료) | 최소 |
| **CSC** | 재생 요청 시 ffmpeg 변환 + 캐싱 + 스트리밍 | 변환 시에만 |

---

## 3. 녹취 흐름

### 3.1 CMP: raw RTP 저장 (비동기)

```
RTP 패킷 수신
  ↓
포워딩 (기존 로직)
  ↓
비동기 write: [4-byte len][RTP packet] → .rtp 파일
  (별도 I/O 스레드 또는 non-blocking write)
```

CMP는 트랜스코딩을 **절대 하지 않음**. raw 파일만 저장하고 서비스 루프에 영향 없음.

### 3.2 VoIP 1:1 통화

```
1. CSP → CMP: add(session_id, record=true)
2. CMP: 양방향 RTP를 raw 파일로 저장
   → raw/{session_id}_a.rtp  (발신 음성)
   → raw/{session_id}_b.rtp  (착신 음성)
   → raw/{session_id}_va.rtp (발신 영상, 있을 때만)
   → raw/{session_id}_vb.rtp (착신 영상, 있을 때만)
3. 통화 종료 → CMP: 파일 close
4. CSP → DB: recordings 레코드 생성 (status='raw')
```

### 3.3 PTT 그룹콜 (세션 단위)

PTT 녹취는 세션 단위 단일 파일로 기록 (화자 변경과 무관하게 연속 기록).

```
1. CSP → CMP: addGroup(group_id, record_dir=세션디렉토리)
2. CMP: McpttGroup::setRecording(true, record_dir)
   → {record_dir}/raw_audio.rtp 생성
3. Floor GRANT → 해당 화자의 RTP 수신 시 raw_audio.rtp에 연속 기록
4. Floor RELEASE → 다음 GRANT까지 기록 일시 중지 (파일은 유지)
5. 세션 종료 → McpttGroup::stopRecording() → 파일 close
```

**파일 구조:**
```
{ServiceLogDir}/ptt/{group_id}/sessions/{session_key}.d/
  ├── raw_audio.rtp          # 세션 전체 음성 (RTP 원본)
  ├── recording_mixed.wav    # 트랜스코딩 캐시 (최초 재생 시 생성)
  ├── session.json           # 세션 메타데이터
  ├── events.jsonl           # Floor/참가 이벤트
  └── cmp.jsonl              # CMP 내부 이벤트
```

### 3.4 CSC: on-demand 트랜스코딩 (파일시스템 기반)

DB 미사용. 세션 디렉토리에서 직접 raw RTP를 트랜스코딩.

```
Console UI: GET /api/v1/ptt/history/{gid}/{session}/audio
  ↓
CSC (csc_flow.py):
  1. 세션 디렉토리 탐색 → .d 폴더
  2. recording_mixed.wav 캐시 확인
     ├─ 캐시 존재 → 즉시 스트리밍 (audio/wav)
     └─ 캐시 없음 → _transcode_audio(d_dir)
          │
          ├─ PTT: raw_audio.rtp → AMR-WB strip → ffmpeg PCM → recording_mixed.wav
          └─ VoIP: raw_a.rtp + raw_b.rtp → AMR-WB strip → ffmpeg → amix → recording_mixed.wav
  3. recording_mixed.wav 스트리밍

VoIP 녹취: GET /api/v1/recordings/{call_id}/audio
  → 동일한 _transcode_audio() 로직 적용
       ↓
     변환된 파일 스트리밍 (또는 202 → 클라이언트 재시도)
```

### 3.5 변환 전략

| 시나리오 | 입력 | ffmpeg 명령 | 출력 |
|----------|------|------------|------|
| VoIP 음성 | _a.rtp + _b.rtp | raw → PCM → 믹스 → MP3 | {call_id}.mp3 |
| VoIP 영상 (발신) | _a.rtp + _va.rtp | audio+video mux | {call_id}_a.mp4 |
| VoIP 영상 (착신) | _b.rtp + _vb.rtp | audio+video mux | {call_id}_b.mp4 |
| PTT 음성 세그먼트 | seg_audio.rtp | raw → MP3 | seg_{seq}.mp3 |
| PTT 영상 세그먼트 | seg_audio.rtp + seg_video.rtp | mux | seg_{seq}.mp4 |

---

## 4. 파일 저장 구조

```
/recordings/
  ├─ raw/                            ← CMP가 저장하는 원본
  │   ├─ {session_id}_a.rtp
  │   ├─ {session_id}_b.rtp
  │   ├─ {session_id}_va.rtp
  │   ├─ {session_id}_vb.rtp
  │   └─ {group_id}/
  │       ├─ seg_0001_audio.rtp
  │       ├─ seg_0001_video.rtp
  │       └─ ...
  │
  └─ converted/                      ← CSC가 변환 후 저장하는 캐시
      ├─ voip/{YYYY}/{MM}/{DD}/
      │   ├─ {call_id}.mp3
      │   ├─ {call_id}_a.mp4
      │   └─ {call_id}_b.mp4
      └─ ptt/{group_id}/{YYYY}/{MM}/{DD}/
          ├─ seg_0001.mp3
          ├─ seg_0002.mp4
          └─ ...
```

---

## 5. DB 스키마

```sql
CREATE TABLE recordings (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    call_id         VARCHAR(128) NOT NULL,
    call_type       ENUM('voip','ptt') NOT NULL,
    group_id        VARCHAR(64) DEFAULT NULL,
    caller          VARCHAR(64) NOT NULL,
    callee          VARCHAR(64) DEFAULT NULL,
    start_time      DATETIME NOT NULL,
    end_time        DATETIME DEFAULT NULL,
    duration        INT DEFAULT 0,

    -- VoIP: 음성만
    audio_path      VARCHAR(512) DEFAULT NULL,

    -- VoIP: 영상통화 (양측 분리)
    audio_path_a    VARCHAR(512) DEFAULT NULL,
    video_path_a    VARCHAR(512) DEFAULT NULL,
    audio_path_b    VARCHAR(512) DEFAULT NULL,
    video_path_b    VARCHAR(512) DEFAULT NULL,

    -- raw 파일 경로 (CMP가 저장한 원본)
    raw_path_a      VARCHAR(512) DEFAULT NULL,
    raw_path_b      VARCHAR(512) DEFAULT NULL,
    raw_path_va     VARCHAR(512) DEFAULT NULL,
    raw_path_vb     VARCHAR(512) DEFAULT NULL,

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
);

CREATE TABLE recording_segments (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    recording_id    BIGINT NOT NULL,
    seq             INT NOT NULL,
    speaker_id      VARCHAR(64) NOT NULL,
    start_time      DATETIME(3) NOT NULL,
    end_time        DATETIME(3) DEFAULT NULL,
    duration_ms     INT DEFAULT 0,
    audio_path      VARCHAR(512) DEFAULT NULL,
    raw_audio_path  VARCHAR(512) NOT NULL,
    has_video       TINYINT(1) DEFAULT 0,
    video_path      VARCHAR(512) DEFAULT NULL,
    raw_video_path  VARCHAR(512) DEFAULT NULL,
    file_size       INT DEFAULT 0,
    status          ENUM('raw','transcoding','ready','failed') DEFAULT 'raw',

    INDEX idx_seg_recording (recording_id),
    INDEX idx_seg_speaker (speaker_id),
    INDEX idx_seg_time (start_time),
    FOREIGN KEY (recording_id) REFERENCES recordings(id) ON DELETE CASCADE
);
```

---

## 6. API

```
GET    /api/v1/recordings                          목록 조회
GET    /api/v1/recordings/{id}                     상세 (메타 + 세그먼트)
GET    /api/v1/recordings/{id}/audio               음성 (on-demand 변환 후 스트리밍)
GET    /api/v1/recordings/{id}/video?side=a|b       영상 (on-demand 변환 후 스트리밍)
GET    /api/v1/recordings/{id}/segments            PTT 세그먼트 목록
GET    /api/v1/recordings/{id}/segments/{seq}/audio 세그먼트 (on-demand 변환 후 스트리밍)
DELETE /api/v1/recordings/{id}                     삭제 (raw + converted 모두)
```

### 응답 상태

| status | HTTP | 설명 |
|--------|------|------|
| `raw` | 202 Accepted + 변환 시작 | 첫 요청 시 변환 트리거 |
| `transcoding` | 202 Accepted | 변환 진행 중, 클라이언트 재시도 |
| `ready` | 200 OK + 파일 스트리밍 | 캐싱된 파일 즉시 응답 |
| `failed` | 500 | 변환 실패 |

---

## 7. Console UI 재생

### VoIP 음성만
```
▶  ━━━━━━━━●━━━━━━━━━━━━  1:30 / 3:20
```

### VoIP 영상통화
```
┌─────────────────┐ ┌─────────────────┐
│  발신 (1001)    │ │  착신 (1002)    │
│   [영상+음성]    │ │   [영상+음성]    │
└─────────────────┘ └─────────────────┘
▶  ━━━━━━━━●━━━━━━━━━━━━  1:30 / 3:20
[발신측▾]
```

### PTT 그룹콜
```
타임라인 (클릭하면 해당 세그먼트 재생):
09:00          10:00          11:00
├──┤           ├┤             ├──┤
1001           1003           1001

발언 목록:
# │화자   │시간      │길이  │영상│재생
1 │김철수 │09:00:12 │4:51 │ ○ │ ▶    ← .mp4 (영상+음성)
2 │박영희 │09:30:45 │0:38 │   │ ▶    ← .mp3 (음성만)
```

---

## 8. 용량 산정

| 항목 | 값 |
|------|-----|
| raw RTP (AMR-WB) | ~24 kbps, 1분 ≈ 180 KB |
| 변환 후 MP3 | ~32 kbps, 1분 ≈ 240 KB |
| H.264 영상 (320p) | ~500 kbps, 1분 ≈ 3.7 MB |
| VoIP 음성 1000건/일 × 2분 | raw ~350 MB, converted ~470 MB |
| PTT 그룹 10개 × 발언 30분/일 | raw ~53 MB, converted ~70 MB |
| 100GB 디스크 (raw+converted) | 음성만: ~3개월, 영상포함: ~1개월 |

### 보관 정책
- raw 파일: 변환 완료 후 보관 기간 설정 (예: 7일 후 삭제)
- converted 파일: 장기 보관 (예: 90일)
- 정기 삭제 크론잡으로 관리

---

## 변경 이력

| 날짜 | 버전 | 내용 |
|------|------|------|
| 2026-04-02 | 1.0 | 초기 설계 |
| 2026-04-02 | 1.1 | CMP/CSC 역할 분리 — CMP는 raw 저장만, 트랜스코딩은 CSC on-demand |
