# CIMS 통화 녹취 설계서

> 버전: 1.3 (2026-06-01)
> 상위 문서: [04_Monitoring_Statistics.md](04_Monitoring_Statistics.md) Part 2 참조
> 이 문서는 녹취의 CMP 구현 상세(RTP 덤프, 파일 형식)를 다룸. UI/API는 상위 문서 Part 2에 통합.
>
> ⚠️ **§2~§3.5 및 §5(DB)는 초기 설계(2026-04) 기준.** 현행 런타임 구현(트랜스코딩 주체=**OAM**,
> 공유 NAS, ffmpeg 번들, 변환 워커 풀, 콘솔 자동재생)은 **[§3.6 재생 변환 런타임(현행)](#36-재생-변환-런타임-현행-구현-2026-06)** 을 정본으로 본다.
> (녹취 메타는 DB 미사용 — call.json/segments.jsonl 파일 SoT, OAM `recording.py` 가 파일 스캔.)

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

**파일 구조 (2026-06 시간버킷 재구조화):**
```
{ServiceLogDir}/ptt/{id}/                       # id = ptt_groups.id (surrogate, mcptt_group_id 아님)
  ├── group.json                                # 그룹 디스크립터 (CSP, base 1개) — session.json 대체
  └── {YYYY}/{MM}/{DD}/{HH}/                     # 시간버킷 (시간검색) — VoLTE 관례와 통일
      ├── events.jsonl                           # 멤버 join/leave 등 (CSP)
      ├── floor.jsonl                            # floor 이벤트 GRANT/REVOKE/REJECT/RELEASE/IDLE (CMP)
      ├── segments.jsonl                         # 세그먼트 인덱스 (CMP)
      └── seg/{NNN}/                             # 100 세그먼트 단위 shard (000,001,…) — 디렉터리 엔트리수 상한
          ├── seg_NNNN_audio.rtp                 # 화자 턴 오디오
          ├── seg_NNNN_video.rtp                 # 영상그룹 + 실제 영상 있을 때만 (빈 파일 미생성)
          └── seg_NNNN.json                      # speaker_id/priority/preempt, audio_file=상대경로(seg/NNN/…)
```
> 옛 구조 `ptt/{group}/sessions/{key}.d` (상시그룹 세그먼트 단일 디렉터리 무한 누적) 폐지.
> `recordings/`·`daily/`·`sessions/`·placeholder(session_id/call_id) 제거. 그룹 키 = surrogate `id`.

### 3.4 CSC: on-demand 트랜스코딩 (파일시스템 기반)

DB 미사용. 세션 디렉토리에서 직접 raw RTP를 트랜스코딩.

```
Console UI: GET /api/v1/ptt/history/{gid}/{session}/audio
  ↓
CSC (services/flow_logger.py):
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

## 3.6 재생 변환 런타임 (현행 구현, 2026-06)

초기 설계(§2~§3.5)와 달리 현행 런타임은 다음과 같다. 이 절이 정본이다.

### 3.6.1 저장 위치 — 공유 NAS (분산 CMP 정합)

CMP는 보통 **원격 미디어 노드**(media01/02)에서 동작하고, 조회/변환 주체인 OAM은 관리 호스트에서 동작한다.
양쪽이 **동일한 공유 스토리지(NFS)** 를 같은 절대경로로 마운트해야 녹취가 한 곳에 모인다.

- 마운트: `NAS:/export → /mnt/cims` (4서버 공통, 동일 절대경로 필수)
- `ServiceLogDir = /mnt/cims/service_log` (csp/cmp/oam 공통). CSP가 `add`/`addGroup` JSON에
  **절대경로 record_dir**(세션 `.d`)을 실어 보내고, 원격 CMP가 그 경로(=NAS)에 seg를 기록 →
  OAM이 같은 NAS 경로를 스캔해 조회·변환.
- 녹취 파일은 한 세션 `.d` 디렉토리에 공존:
  `call.json`/`participants.jsonl`/`session.json`(csp 작성) + `seg_NNNN_{a,b,va,vb}.rtp`·
  `seg_NNNN.json`·`segments.jsonl`(cmp 작성) + `seg_NNNN.mp4`(OAM 변환 캐시).

### 3.6.2 트랜스코딩 주체 = OAM + 번들 ffmpeg

- 트랜스코딩은 **OAM** `oam/src/handlers/recording.py` 가 수행(과거 문서의 CSC 아님). 출력은 **MP4**(H.264+AAC) `seg_NNNN.mp4`.
- ffmpeg/ffprobe는 **OAM 패키지에 동봉**(air-gapped 대응). 빌드 시 `cims.sh` 의 `_ensure_oam_vendor_ffmpeg`가
  정적 바이너리를 `oam/vendor/bin/` 으로 다운로드(idempotent, `CIMS_SKIP_VENDOR_FETCH`/`CIMS_FFMPEG_URL`),
  `cims.sh pkg oam` 이 vendor를 패키지에 포함. 경로 해석: 명시인자 → `CIMS_FFMPEG` env → PATH → fallback.
- 메타데이터는 **DB 미사용** — 파일(call.json/segments.jsonl)이 SoT, `recording.py`가 디렉토리 스캔.

### 3.6.3 변환 실행 — 온디맨드 + 변환 워커 풀 (bounded)

**전략: 온디맨드 유지**(전수 사전변환 안 함 — 대부분 미재생, CPU·저장소 2배 낭비). 단 ffmpeg 실행을
**요청 처리 경로와 분리**하고 **동시 변환 수를 제한**한다.

```
[콘솔] GET …/segments/{seq}/audio
   │
   ▼  (OAM 요청 스레드 — ffmpeg 직접 실행 안 함)
 _ensure_segment_ready(rec_dir, seg)
   ├─ status 판정: ready(mp4 존재) / transcoding(마커·lock) / recording / raw
   ├─ raw 면: dedup lock(lock_key="{rec_dir}:{seq}") 획득 후
   │          _transcode_executor.submit(_transcode_segment_file, rec_dir, seg)
   └─ 즉시 202 transcoding 반환
        │
        ▼  ThreadPoolExecutor(max_workers=N)  ← 내부 FIFO 큐 + 워커 스레드 N개
     [worker] _transcode_segment_file: 마커 생성 → raw RTP strip(AMR-WB/H.264)
              → ffmpeg mux → seg_NNNN.mp4 → finally: 마커·lock 해제
```

| 항목 | 설명 |
|------|------|
| **워커 풀** | `ThreadPoolExecutor(max_workers=N, thread_name_prefix='rec-transcode')`. 내부 FIFO 큐 + 워커 스레드. OAM `init()`에서 1회 생성 |
| **동시 변환 수(N)** | 기본 **2**. `oam.json` 의 `RecordingTranscodeWorkers` 로 조정. 초과 작업은 큐에서 대기(FIFO), 빈 워커 생기면 시작 → **CPU 폭주 방지** |
| **동작 주기** | 폴링 타이머 **없음** — 이벤트 구동(`submit` 시 idle 워커가 즉시 pull, 큐 비면 블로킹 대기). 유일한 주기는 콘솔 폴링(아래) |
| **할당 단위** | **세그먼트(seq) 1개 = 작업 1개** (녹취 1건이 아님). 어느 워커가 받을지는 풀이 FIFO 선착순 결정 |
| **중복 방지** | `_transcoding_locks[lock_key]` — 같은 세그먼트에 동시 요청이 와도 1회만 큐잉. 작업 종료 시(`finally`) 해제 |
| **캐시** | 결과 `seg_NNNN.mp4` 영속 → 재시청은 변환 없이 즉시 200 |

### 3.6.4 콘솔 자동재생 (폴링)

`cims-console/src/components/SegmentPlayer.tsx` — raw 세그먼트 재생 시 닫았다 다시 열 필요 없이 자동 재생.

- 재생 클릭 → 미변환이면 `waitSegmentReady(url)` 가 같은 audio/video URL을 폴링
  (첫 **0.7초**, 이후 **1.5초** 간격, 최대 **120초**): 202면 대기, 200이면 src 지정 후 **자동 재생**.
- 진행 중 "변환중" 배지 표시, 실패 시 "다시 시도" 버튼. 변환 완료 세그먼트는 즉시 재생.

### 3.6.5 튜닝 / 확장

- 동시 시청 많아 변환 대기가 길면 `RecordingTranscodeWorkers` ↑ (호스트 CPU 코어·영상변환 비용 고려). 적용은 OAM 재기동.
- 더 강한 격리(ffmpeg OOM/크래시가 OAM 무영향)가 필요하면 변환 워커를 **별도 OS 프로세스**로 승격 가능
  (현재는 ffmpeg가 subprocess라 GIL 해제 → in-OAM 워커 풀로 응답성·동시성 제어 충족).

---

## 4. 파일 저장 구조

> ⚠️ 아래는 옛 개념 레이아웃(raw/converted 분리)이다. **실제 on-disk 구조는 §3.3 참조**:
> VoLTE=`volte/YYYY/MM/DD/HH/.../*.d/`, PTT=`ptt/{id}/{YYYY}/{MM}/{DD}/{HH}/seg/{NNN}/` (시간버킷+shard).
> 변환 mp4(`seg_NNNN.mp4`)는 원본 옆(.d/window 디렉터리)에 캐시된다.

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
| 2026-06-01 | 1.3 | §3.6 현행 런타임 추가 — 트랜스코딩 주체 OAM, 공유 NAS(/mnt/cims) 정합, ffmpeg 패키지 번들, **온디맨드 + 변환 워커 풀(bounded)**, 콘솔 자동재생 폴링. (메타 DB 미사용=파일 SoT) |
