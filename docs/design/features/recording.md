# CIMS 통화 녹취 설계서

> 이 문서가 녹취의 정본이다 — CMP 기록(RTP 덤프·세그먼트 메타), OAM 변환·재생, 콘솔 UI 규약.
> 관련: [monitoring.md](monitoring.md) · [../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.7(floor 정책)
>
> 재생 변환 런타임의 정본은 **[§3.6 재생 변환 런타임](#36-재생-변환-런타임)** 이다(트랜스코딩 주체=**OAM**,
> 공유 NAS, ffmpeg 번들, 변환 워커 풀, 믹스/슬롯 재생 단위, 콘솔 자동재생).
> 녹취 메타는 DB 미사용 — call.json/segments.jsonl 파일 SoT, OAM `recording.py` 가 파일 스캔.

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
                     │   OAM   │←────────────────────────┘
                     │ (Python)│
                     │         │── ffmpeg 변환 (on-demand, 워커 풀)
                     │         │── 변환 완료 → mp4 + peaks 캐시
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
| **CMP** | raw RTP 패킷을 파일에 덤프 (비동기 write) + 세그먼트 메타 기록 | 최소 (I/O only) |
| **CSP** | 녹취 경로(record_dir) 결정·전달 + 세션/그룹 디스크립터 기록 | 최소 |
| **OAM** | 재생 요청 시 ffmpeg 변환 + 캐싱 + 스트리밍 ([§3.6](#36-재생-변환-런타임)) | 변환 시에만 |

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

**파일 구조 (시간버킷):**
```
{ServiceLogDir}/ptt/{id}/                       # id = ptt_groups.id (surrogate, mcptt_group_id 아님)
  ├── group.json                                # 그룹 디스크립터 (CSP, base 1개) — 최신 편성 스냅샷
  └── {YYYY}/{MM}/{DD}/{HH}/                     # 시간버킷 (시간검색) — VoLTE 관례와 통일
      ├── session.json                           # 세션 디스크립터 (CSP) — 세션 시작 당시 스냅샷
      ├── events.jsonl                           # 멤버 join/leave 등 (CSP)
      ├── floor.jsonl                            # floor 이벤트 GRANT/DENY/QUEUE/QUEUE_CANCEL/RELEASE/REVOKE/REVOKE_END/IDLE (CMP)
      ├── segments.jsonl                         # 세그먼트 인덱스 (CMP)
      └── seg/{NNN}/                             # 100 세그먼트 단위 shard (000,001,…) — 디렉터리 엔트리수 상한
          ├── seg_NNNN_audio.rtp                 # 화자 턴 오디오 (동시 발언 슬롯 0)
          ├── seg_NNNN_audioK.rtp                # 동시 발언(dual/multi-talker) 슬롯 K 화자 오디오
          ├── seg_NNNN_video.rtp                 # 영상그룹 + 실제 영상 있을 때만 (빈 파일 미생성)
          ├── seg_NNNN.json                      # 세그먼트 메타 — tracks[] 가 정본 (아래 §3.3.1)
          ├── seg_NNNN.mp4                       # 믹스 변환본 (OAM 캐시 — 화자 전원 합성)
          ├── seg_NNNN_sK.mp4                    # 슬롯 K 화자 단독 변환본 (OAM 캐시)
          └── seg_NNNN[_sK].peaks.json           # 파형 피크 배열 (콘솔 전이중 플레이어 레인)
```
> 그룹 키 = surrogate `id`. 1:1 private call·ad-hoc 그룹은 DB 행이 없어 surrogate 가 없다 —
> 이때 키는 세션 식별자(`priv-<caller>-<callee>` 등)가 그대로 쓰인다.

**그룹/세션 디스크립터 — 2단 스냅샷.** CSP 가 `BuildGroupDescriptor`(편성·floor 축·멤버)에
`state`/`updated_at` 을 주입해 세션 시작마다 두 곳에 기록한다:

| 파일 | 위치 | 의미 | 갱신 |
|---|---|---|---|
| `group.json` | 그룹 base 루트 1개 | **최신** 편성 스냅샷 — 좌측 목록(요약)의 분류·이름·멤버 근거 | 매 세션 시작 시 전체 재작성, 종료 시 `state:"ended"`+`end_time` 마킹 |
| `session.json` | 세션 시작 시간버킷 | **세션 당시** 스냅샷 — 세션 이력의 반이중/전이중·동시 발언 정원 배지의 정본 | 세션 시작 시 기록(같은 버킷 재시작은 덮어쓰기), 종료 시 동일 마킹 |

세션 축을 분리하는 이유: floor 축은 소급되면 안 된다 — private call 의 `floor_control` 은
세션마다 SDP 협상으로 달라지고, 그룹의 `floor_policy`/`max_talkers` 도 편성 변경 전 세션은
당시 값으로 보여야 이력이 왜곡되지 않는다. `session.json` 이 없는 버킷(구 녹취, 시간 경계를
넘긴 세션의 후속 버킷)은 그룹 레벨(`group.json`)로 폴백한다. 종료 마킹은 `end_time` 이 이미
있으면 값만 교체한다(키 중복 누적 금지).

#### 3.3.1 세그먼트 메타 — `tracks[]` (정본)

한 세그먼트는 **슬롯 트랙 N개**를 가질 수 있다. 동시 발언(dual/multi-talker)은 화자마다,
floor 없는 private call(전이중)은 멤버마다 슬롯이 하나씩이다. 트랙 메타의 정본은 `tracks[]` 다:

```json
{
  "seq": 15, "type": "ptt", "speaker_id": "01011112222", "priority": 5,
  "start_time": "…", "end_time": "…", "duration_ms": 94000,
  "audio_file": "seg/000/seg_0015_audio.rtp", "audio_pt": 96,       // ← 구 소비자 호환 flat 키
  "audio1_file": "…", "speaker_id_audio1": "01033334444",           //   (슬롯 0 / 첫 화자만)
  "tracks": [
    { "prefix": "audio", "kind": "audio", "slot": 0,
      "file": "seg/000/seg_0015_audio.rtp", "pt": 96, "codec": "AMR-WB/16000",
      "speakers": [ { "id": "01011112222", "offset_ms": 0,     "dur_ms": 58000 },
                    { "id": "01099990000", "offset_ms": 58000, "dur_ms": 36000 } ] },
    { "prefix": "audio1", "kind": "audio", "slot": 1, "file": "…", "pt": 99, "codec": "AMR-WB/16000",
      "speakers": [ { "id": "01033334444", "offset_ms": 25000, "dur_ms": 44000 } ] }
  ],
  "has_video": false
}
```

| 필드 | 의미 |
|---|---|
| `kind`/`slot` | PTT 슬롯 트랙 — `audio`/`audio1`… = 슬롯 0..N, `video`/`videoK` 동일 |
| `side` | VoIP leg (`a`/`b`) — PTT 에는 없다 |
| `pt`/`codec` | **슬롯마다** 다를 수 있다(이종 단말 혼재) — 변환기의 PT 판별 근거 |
| `speakers[]` | 그 트랙을 점유한 화자 **구간** 목록. 선점 회수로 슬롯이 재사용되면 원소가 2개 이상이 된다 — 트랙당 화자를 한 값으로만 두면 뒤 화자만 남아 귀속이 소실된다 |

미디어(payload 있는 RTP)가 없는 트랙은 파일·`tracks[]` 양쪽에서 제외된다(keepalive-only 포함).
flat 키(`audio_file`/`audio_pt`/`speaker_id_audioK`)는 기존 녹취와의 호환을 위해 계속 기록되며,
슬롯 0 과 각 트랙의 **첫 화자**만 담는다. 소비자는 `tracks[]` 가 있으면 그것을 쓰고, 없으면
(그 이전 녹취) flat 키에서 트랙 1개를 합성한다.

### 3.4 변환 전략

DB 미사용 — 녹취 디렉터리에서 직접 raw RTP 를 트랜스코딩한다. 출력은 **MP4**(AAC, 영상 있으면
H.264) 로 통일하고 원본 옆에 캐시한다. 실행 주체·큐잉·상태 규약은 [§3.6](#36-재생-변환-런타임)
이 정본이다.

| 시나리오 | 입력 | 처리 | 출력 |
|----------|------|------|------|
| PTT 단일 화자 | `seg_NNNN_audio.rtp` | AMR-WB strip → AAC | `seg_NNNN.mp4` |
| PTT 동시 발언·전이중 (믹스) | 슬롯 트랙 N개 | AMR-WB strip → `amix` → AAC | `seg_NNNN.mp4` |
| PTT 슬롯 단독 | 슬롯 K 트랙 | AMR-WB strip → AAC | `seg_NNNN_sK.mp4` |
| PTT 영상 (슬롯 1개) | 음성 + `seg_NNNN_video.rtp` | H.264 `copy` mux | `seg_NNNN.mp4` |
| PTT 영상 (슬롯 2개 이상) | 음성 + 영상 트랙 N개 | 2열 격자 합성 + mux | `seg_NNNN.mp4` |
| VoIP 음성 | `_a.rtp` + `_b.rtp` | AMR-WB strip → `amix` → AAC | `seg_NNNN.mp4` |
| VoIP 영상 | `_a/_b` + `_va/_vb` | 발신=좌 / 착신=우 배치 + mux | `seg_NNNN.mp4` |

모든 음성 변환은 같은 ffmpeg 실행의 두 번째 출력으로 PCM 을 뽑아 파형 피크
(`seg_NNNN[_sK].peaks.json`)까지 만든다.

---

## 3.6 재생 변환 런타임

재생 변환 런타임은 다음과 같다. 이 절이 정본이다.

### 3.6.1 저장 위치 — 공유 NAS (분산 CMP 정합)

CMP는 보통 **원격 미디어 노드**(media01/02)에서 동작하고, 조회/변환 주체인 OAM은 관리 호스트에서 동작한다.
양쪽이 **동일한 공유 스토리지(NFS)** 를 같은 절대경로로 마운트해야 녹취가 한 곳에 모인다.

- 마운트: `NAS:/export → /mnt/cims` (4서버 공통, 동일 절대경로 필수)
- `ServiceLogDir = /mnt/cims/service_log` (csp/cmp/oam 공통). CSP가 `RELAY_ADD`/`PTT_GROUP_ADD` JSON에
  **절대경로 record_dir**(세션 `.d`)을 실어 보내고, 원격 CMP가 그 경로(=NAS)에 seg를 기록 →
  OAM이 같은 NAS 경로를 스캔해 조회·변환.
- 녹취 파일은 한 세션 `.d` 디렉토리에 공존:
  `call.json`/`participants.jsonl`/`session.json`(csp 작성) + `seg_NNNN_{a,b,va,vb}.rtp`·
  `seg_NNNN.json`·`segments.jsonl`(cmp 작성) + `seg_NNNN.mp4`(OAM 변환 캐시).

### 녹취 오디오 PT/코덱 메타 — 변환 결정론

녹취 raw RTP 는 **화자/leg 원본 wire PT** 로 기록된다(egress PT 재작성 전 탭). 협상 PT 는
leg 마다 다르므로(UE 동적 96, cspsim 99, 이종 단말 혼재) 변환기의 PT 판별 근거를 세그먼트
메타에 남긴다:

- **CSP → CMP**: PTT_JOIN `user_src_pt`/`user_codec`, RELAY_ADD/MODIFY `remote_src_pt`/
  `remote_codec` ([cmp_media_api.md](../../api/cmp_media_api.md) §6.1/§7.4). 코덱 문자열 =
  코덱 테이블 top 의 rtpmap prefix(예 `"AMR-WB/16000"`).
- **CMP → seg 메타**: PTT `seg_NNNN.json` 에 `audio_pt`/`audio_codec`(화자 leg — GRANT 시점
  화자별 갱신), VoIP 에 `audio_pt_a/b`/`audio_codec_a/b`(leg 별, 재협상 시 최신 반영).
- **동시 발언 세그먼트**(dual/multi-talker, [../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.7):
  세그먼트는 발언자 집합이 빌 때 닫히고, 화자마다 슬롯 트랙(`audio`/`audio1`…)에 분리 기록된다.
  귀속은 `tracks[].speakers[]` 구간이 정본이다(§3.3.1). 단일 화자 정책에서는 슬롯 0 만 쓰여
  파일명·메타가 종전과 같다.
- **floor 없는 세션**(`floor_control:"off"` — 1:1 private 멀티): 발언 경계가 없어 세그먼트는
  첫 미디어에서 열려 그룹 해제까지 유지되고, **멤버마다** 슬롯 트랙을 하나씩 쓴다(발언 정원이
  아니라 멤버 상향 스트림 슬롯 수가 트랙 수 기준). 슬롯 트랙 등록·화자 귀속은 **멤버 합류
  시점**에 이뤄지며, 세그먼트가 이미 열려 있으면 그 트랙 파일을 즉시 연다 — 개시자 미디어가
  상대 합류보다 먼저 도착하는 것이 정상 순서라, 세그먼트 시작 시점에만 파일을 열면 나중에
  합류한 상대의 음성이 통째로 유실된다.
- **OAM 변환기**: 트랙 메타 `pt`(구 녹취는 `audio_pt`)를 **우선** 사용해 해당 PT 패킷만 추출.
  메타 없는 녹취는 파일 내 최빈 PT 자동감지 fallback(payload ≥ 6바이트 표본 — telephone-event
  배제). AMR-WB 외 코덱 메타는 경고 로그 후 AMR-WB 로 시도(현행 디코더 = AMR-WB + H.264).

### 영상 유무(has_video) 판정 — keepalive-only 트랙 배제

음성 통화에서도 UE 가 영상 포트로 헤더-only RTP keepalive 를 보내므로, "바이트가 기록됨" 을
영상 있음으로 판정하면 음성 호가 콘솔에서 영상 플레이어(검은 화면)로 열린다.

- **CMP**: payload 있는 RTP 패킷만 미디어로 집계 — keepalive-only 트랙은 파일 미보존·
  세그먼트 메타 참조 미기록·`has_video` 제외 (빈 트랙과 동일 취급).
- **OAM**(기존 녹취 방어): 영상 raw 파일이 4KB 미만이면 payload 패킷 존재를 스캔해 판정 —
  keepalive 만 담긴 파일(레코드당 24B)은 영상 없음. 목록·세그먼트·변환·Content-Type 공통 적용.

### 3.6.2 트랜스코딩 주체 = OAM + 번들 ffmpeg

- 트랜스코딩은 **OAM** `ems/core/oam/src/handlers/recording.py` 가 수행. 출력은 **MP4**(H.264+AAC) `seg_NNNN.mp4`.
- ffmpeg/ffprobe는 **OAM 패키지에 동봉**(air-gapped 대응). 빌드 시 `cims.sh` 의 `_ensure_oam_vendor_ffmpeg`가
  정적 바이너리를 `ems/core/oam/vendor/bin/` 으로 다운로드(idempotent, `CIMS_SKIP_VENDOR_FETCH`/`CIMS_FFMPEG_URL`),
  `cims.sh pkg oam` 이 vendor를 패키지에 포함. 경로 해석: 명시인자 → `CIMS_FFMPEG` env → PATH → fallback.
- 메타데이터는 **DB 미사용** — 파일(call.json/segments.jsonl)이 SoT, `recording.py`가 디렉토리 스캔.

### 3.6.3 변환 실행 — 온디맨드 + 변환 워커 풀 (bounded)

**전략: 온디맨드 유지**(전수 사전변환 안 함 — 대부분 미재생, CPU·저장소 2배 낭비). 단 ffmpeg 실행을
**요청 처리 경로와 분리**하고 **동시 변환 수를 제한**한다.

```
[콘솔] GET …/segments/{seq}/audio[?slot=K]
   │
   ▼  (OAM 요청 스레드 — ffmpeg 직접 실행 안 함)
 _ensure_segment_ready(rec_dir, seg, slot)
   ├─ status 판정: ready(mp4 존재) / transcoding(마커·lock) / failed(.failed 마커
   │              또는 오디오 원본 없음) / recording / raw
   ├─ raw 면: dedup lock(lock_key="{rec_dir}:{seq}:{slot|mix}") 획득 후
   │          _transcode_executor.submit(_transcode_segment_file, rec_dir, seg, slot)
   └─ 즉시 202 transcoding 반환 (failed 면 500 + 사유 — 재큐잉 없음)
        │
        ▼  ThreadPoolExecutor(max_workers=N)  ← 내부 FIFO 큐 + 워커 스레드 N개
     [worker] 마커 생성 → 슬롯별 raw RTP strip(AMR-WB/H.264)
              → ffmpeg (amix + mux) → seg_NNNN[_sK].mp4
              → 같은 실행의 2번째 출력(PCM)으로 peaks.json
              → finally: 마커·lock 해제
```

**재생 단위 = 믹스 또는 슬롯**. 동시 발언·전이중 세션은 세그먼트 하나에 화자가 여럿이므로
재생본이 둘로 나뉜다.

| 재생 단위 | 출력 | 내용 |
|---|---|---|
| **믹스**(기본, `slot` 미지정) | `seg_NNNN.mp4` | 그 세그먼트 화자 **전원 합성**(`amix`) — 실제로 무전/통화에서 들린 소리 |
| **슬롯 단독**(`?slot=K`) | `seg_NNNN_sK.mp4` | 슬롯 K 화자만 — 화자 식별·증거용 |

슬롯이 1개뿐인 단일 화자 세그먼트는 콘솔이 `slot` 을 붙이지 않아 종전 `seg_NNNN.mp4` 경로·캐시를
그대로 쓴다(중복 변환 없음). 영상은 슬롯 1개면 재인코딩 없이 `-c:v copy` mux, 2개 이상이면
2열 격자(640×640 셀)로 합성한다.

| 항목 | 설명 |
|------|------|
| **워커 풀** | `ThreadPoolExecutor(max_workers=N, thread_name_prefix='rec-transcode')`. 내부 FIFO 큐 + 워커 스레드. OAM `init()`에서 1회 생성 |
| **동시 변환 수(N)** | 기본 **2**. `oam.json` 의 `RecordingTranscodeWorkers` 로 조정. 초과 작업은 큐에서 대기(FIFO), 빈 워커 생기면 시작 → **CPU 폭주 방지** |
| **동작 주기** | 폴링 타이머 **없음** — 이벤트 구동(`submit` 시 idle 워커가 즉시 pull, 큐 비면 블로킹 대기). 유일한 주기는 콘솔 폴링(아래) |
| **할당 단위** | **재생 단위 1개 = 작업 1개** — 세그먼트 믹스와 슬롯 단독본은 각각 별도 작업이다 |
| **중복 방지** | `_transcoding_locks[lock_key]` — 같은 재생 단위에 동시 요청이 와도 1회만 큐잉. 작업 종료 시(`finally`) 해제 |
| **캐시** | 결과 `seg_NNNN[_sK].mp4` 영속 → 재시청은 변환 없이 즉시 200 |

**파형 피크**(`seg_NNNN[_sK].peaks.json`): 변환 ffmpeg 의 **두 번째 출력**으로 s16le/8kHz PCM 을
같이 뽑아 그 자리에서 진폭 피크 배열(0..255, 600 버킷)로 요약한다 — 별도 프로세스·재디코딩이
없다. 콘솔 전이중 플레이어의 화자별 파형 레인이 이 값을 쓴다. 이 기능 이전에 변환된 캐시에는
피크가 없어 `/peaks` 가 404 를 반환하고, 콘솔은 레인을 비운 채 재생만 제공한다.

### 3.6.4 콘솔 자동재생 (폴링)

`ems/core/console/src/components/SegmentPlayer.tsx` — raw 세그먼트 재생 시 닫았다 다시 열 필요 없이 자동 재생.

- 재생 클릭 → 미변환이면 `waitSegmentReady(url)` 가 같은 audio/video URL을 폴링
  (첫 **0.7초**, 이후 **1.5초** 간격, 최대 **120초**): 202면 대기, 200이면 src 지정 후 **자동 재생**.
- 진행 중 "변환중" 배지 표시, 실패 시 "다시 시도" 버튼. 변환 완료 세그먼트는 즉시 재생.

### 3.6.5 튜닝 / 확장

- 동시 시청 많아 변환 대기가 길면 `RecordingTranscodeWorkers` ↑ (호스트 CPU 코어·영상변환 비용 고려). 적용은 OAM 재기동.
- 더 강한 격리(ffmpeg OOM/크래시가 OAM 무영향)가 필요하면 변환 워커를 **별도 OS 프로세스**로 승격 가능
  (현재는 ffmpeg가 subprocess라 GIL 해제 → in-OAM 워커 풀로 응답성·동시성 제어 충족).

---

## 4. 파일 저장 구조

> **실제 on-disk 구조는 §3.3 참조**:
> VoLTE=`volte/YYYY/MM/DD/HH/.../*.d/`, PTT=`ptt/{id}/{YYYY}/{MM}/{DD}/{HH}/seg/{NNN}/` (시간버킷+shard).
> 변환 mp4(`seg_NNNN.mp4`)는 원본 옆(.d/window 디렉터리)에 캐시된다. 아래는 raw/converted 분리의 개념 레이아웃이다.

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

## 5. DB 스키마 (미사용 — 참고)

> 녹취 메타는 **파일이 SoT** 다(`group.json`/`call.json`/`segments.jsonl`/`seg_NNNN.json`).
> 아래 스키마는 조회 인덱스가 필요해질 때를 위한 참고안이며, 현재 조회·변환 경로는 DB 를 쓰지
> 않는다. 특히 세그먼트의 슬롯 트랙·화자 구간([§3.3.1](#331-세그먼트-메타--tracks-정본))은
> 아래 평면 스키마로 표현되지 않는다.

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
GET    /api/v1/recordings/{id}/segments/{seq}/audio[?slot=K]  세그먼트 음성 (믹스 / 슬롯 단독)
GET    /api/v1/recordings/{id}/segments/{seq}/video[?slot=K]  세그먼트 영상
GET    /api/v1/recordings/{id}/segments/{seq}/peaks[?slot=K]  파형 피크 배열
DELETE /api/v1/recordings/{id}                     삭제 (raw + converted 모두)
```

`slot` 미지정 = **믹스**(화자 전원 합성), `slot=K` = 슬롯 K 화자 단독본 (§3.6.3).

세그먼트 응답 필드 — 슬롯 트랙과 집계:

| 필드 | 의미 |
|---|---|
| `tracks[]` | `{slot, kind, side, pt, codec, speakers[], has_video, status}` — 재생 가능한 단위와 화자 귀속 |
| `speaker_ids[]` | 그 세그먼트의 화자 (등장 순서) |
| `talker_count` | 음성 트랙 수 |
| `max_concurrent` | 동시에 열려 있던 화자 구간의 최대 수 (동시 발언 인원) |

녹취 목록(`/recordings`)·PTT 세션 목록(`/ptt/history`)에는 `turn_count`(발언 턴 = 화자 구간 수),
`speaker_count`(슬롯 화자 포함), `max_concurrent`, `talk_ms`(화자별 발화 누적)가 함께 실린다 —
세그먼트 수만 세면 동시 발언이 과소 집계된다(3명이 겹쳐 말해도 세그먼트는 1개).
PTT 세션 행에는 세션 당시 floor 축(`floor_control`/`floor_policy`/`max_talkers` — 시간버킷
`session.json`, 없으면 ``''``/0)도 실린다 (§3.3, §7).

### 응답 상태

| status | HTTP | 설명 |
|--------|------|------|
| `raw` | 202 Accepted + 변환 시작 | 첫 요청 시 변환 트리거 |
| `transcoding` | 202 Accepted | 변환 진행 중, 클라이언트 재시도 |
| `ready` | 200 OK + 파일 스트리밍 | 캐싱된 파일 즉시 응답 |
| `failed` | 500 + `reason` | 영구 재생불가 — `seg_NNNN.mp4.failed` 마커(사유 JSON). 원본 없음·음성 프레임 0(빈/극소 녹취)·ffmpeg 실패 시 확정되어 **재큐잉하지 않는다**(무한 '변환중' 방지). 세그먼트 목록에는 `status:'failed'`+`status_reason` 으로 노출, 콘솔은 "재생불가" 배지 표시. `?retry=1` 로 마커 해제 후 1회 재변환 시도 가능 |

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

### PTT 세션 이력 (`/service/history/ptt`)

**좌측 목록의 출처는 DB(`ptt_groups`)가 아니라 녹취 디렉터리 요약**(`/ptt/history?summary=1`)
이다. DB 만 보면 1:1 private call(`priv-<caller>-<callee>`)·ad-hoc 그룹은 행이 없어 이력에서
통째로 사라진다. 아직 통화가 없는 DB 그룹도 계속 보이도록 **DB 그룹 ∪ 녹취 요약**으로 합치고,
`kind` 로 섹션을 나눈다.

| kind | 판정 | 섹션 |
|---|---|---|
| `group` | `group.json` 의 surrogate `id` > 0 | 그룹 |
| `private` | `group_type=private` 또는 키가 `priv-` 로 시작 | 1:1 private call |
| `adhoc` | `group.json` 은 있으나 surrogate `id` 없음 | 임시 / ad-hoc |
| `unknown` | `group.json` 유실 | 분류 미상 |

세션 헤더 배지의 floor 축은 **세션 시간버킷의 `session.json`(세션 당시 스냅샷) 이 정본**이고,
없으면 그룹 루트 `group.json`(최신 스냅샷) 으로 폴백한다 (§3.3) — `floor_control:"off"` =
**전이중·통화**, `"on"` = **반이중·무전**, `floor_policy`/`max_talkers` = 동시 발언 정원.
세션 목록 API(`/ptt/history?group_id=`)가 각 세션 행에 이 세 값을 실어 준다.

**반이중(무전형)** — 화자 레인 타임바 + 발언 턴 목록:
```
발언권 타임라인      14:20:03 ~ 14:31:47 · 최대 동시 발언 3명
                          ╎동시 2╎ ╎동시 3╎
  01011112222   ██████▌      ▐███████
  01033334444        ▐████████▌
  01055556666            ▐███▌      ▐██▌
  14:20          14:26          14:31

타임라인 (발언 / 발언권 / 이벤트 레이어 토글):
 14:23:11  ▶  ▌▌ 동시 2명  01011112222, 01033334444   [믹스]        0:59
 14:23:11  ▶  01011112222  발언  슬롯 0                            0:47
 14:23:39  ▶  01033334444  발언  슬롯 1                            0:31
 14:23:39  ◆  발언권 부여   01033334444  슬롯 1 · 동시 2명 · 정책 multi
```
- 화자마다 **레인 1줄**. 겹친 구간은 음영 + `동시 N` 라벨 — 단일 화자 세션은 레인이 1개라
  종전 미니 타임바와 같은 모습이다.
- 동시 발언 세그먼트만 **믹스 행**이 앞에 붙는다(전원 합성 = 실제로 들린 소리). 그 아래
  화자별 턴 행은 각자 슬롯 단독본을 재생한다.
- floor 레이어는 CMP 가 기록하는 op 8종을 사유와 함께 편다:
  `GRANT`(슬롯·동시 인원·정책) · `RELEASE`(잔여 인원, T1 무RTP 회수) · `IDLE` ·
  `REVOKE`(cause·유예) · `REVOKE_END`(유예 만료 강제 회수) · `QUEUE`(선점 대기/대기 위치) ·
  `QUEUE_CANCEL` · `DENY`(수신전용·1인·broadcast·점유 중 등 사유).

**전이중(통화형)** — floor 가 없어 발언 턴이 성립하지 않으므로 통화형 플레이어:
```
▶ ━━━━━━●━━━━━━━━━  1:30 / 2:52   [믹스(양측) ▾]
   01011112222 ▁▃▅▂▁▁▃▅▇▅▃▁▁▁▂▄▂▁     ← 화자별 파형 레인 (클릭 = 탐색)
   01055556666 ▁▁▁▄▆▃▁▁▁▁▁▂▅▃▁▁▁▁
```
기본은 믹스, 드롭다운으로 화자 단독 전환. 파형은 `/segments/{seq}/peaks?slot=K` 를 쓴다.

**지표** — 동시 발언에서 뜻이 갈리는 값은 분리해 표기한다:

| 지표 | 정의 |
|---|---|
| 발언 턴 | 화자 구간 수 (동시 발언 세그먼트는 턴이 여럿) |
| 녹취 세그먼트 | 파일 단위 — 발언자 집합이 빌 때 닫힌다 |
| 발화 구간 | 세그먼트 길이 합 — 겹침을 1회로 센 실제 무전 점유 시간 |
| 발화 누적 | 화자별 발언 시간 합 — 겹치면 각각 더해진다 |

> PTT 통계(`/stats/ptt`)는 아직 세그먼트 기준 집계를 쓴다 — 통계 평면 전면 개선 시 함께 맞춘다.

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
