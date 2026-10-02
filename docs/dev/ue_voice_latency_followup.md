# 단말 음성 지연 — 후속 보완

단말 간 VoLTE 통화의 입에서 귀 지연을 줄이는 남은 작업. 반영된 결정의 정본은 [ue_sdk.md](../design/features/ue_sdk.md) §4.5 «음성 지연»,
남은 항목의 요약은 같은 문서 §11. 이 문서는 항목마다 근거·방법·위험·확인 방법을 담는다. 원칙은 CLAUDE.md 설계 우선순위(규격 먼저).

## 1. 반영된 것 (ca8b85c8)

| 결정 | 위치 | 근거 |
|---|---|---|
| 오디오 SDP `a=ptime:20`·`a=maxptime:240`, AMR-WB 송신 패킷당 1프레임 | pjmedia `endpoint.c`(config_site `PJMEDIA_CIMS_SDP_PTIME`/`MAXPTIME`), 코어 `applyCodecPolicy` `frmPerPkt = 1` | GSMA IR.92 §3.2.5 Note 1 |
| MediaCodec 같은 프레임 출력 — 입력 뒤 그 프레임 출력을 최대 8 ms 기다림, 늦은 출력은 버림 | `and_aud_mediacodec.cpp` `and_med_take_output` | 비동기 출력 큐를 곧바로 보면 이전 프레임을 꺼내 밀림이 쌓였다 |
| 재생 트랙 `AudioTrack.Builder` + `PERFORMANCE_MODE_LOW_LATENCY`, 채우는 양 2프레임 | `android_jni_dev.c` `cims_build_track`·`cims_trim_track_buffer` | 블로킹 write 가 최소 버퍼를 늘 채워 전부가 지연이었다 |

실측(.45, W999↔MF52 VoLTE, 같은 사내 Wi-Fi·CMP 중계):

| 항목 | 반영 전(10-02 09:00, 84 s) | 반영 후(10-02 09:27, 20 s, W999 쪽) |
|---|---|---|
| 송신 패킷 | ptime 40 | ptime 20 |
| 수신 지터 버퍼 지연 현재/최대 | W999 99/500 ms · MF52 84/280 ms | W999 40/60 ms |
| 코덱 출력 놓침 | 시작 때 인코더·디코더 각 2회 → 각 +40 ms 가 통화 끝까지 | 인코더 0 · 디코더 1회(다음 프레임에서 따라잡음) |
| 재생 트랙 버퍼 | MF52 1288 프레임 = 80 ms(늘 가득) | W999 640/690 프레임 = 40 ms, **성능 모드 0(FAST 미허가)** |
| 재생 끊김 | — | 1 |
| 망 | 지터 평균 7~8 ms·최대 47~79 ms, 손실 0, RTT 25 ms | 지터 평균 3 ms·최대 9 ms, 손실 0.5 %, RTT 12 ms |

사용자 체감 = «조금 줄었다». **MF52 수신 쪽은 반영 후 통화 기록이 아직 없다** — 다음 측정에서 먼저 본다.

## 2. 남은 지연 구성 (한 방향, 반영 후 — 코드 사실 / 추정 구분)

| 단계 | 지연 | 근거 | 구분 |
|---|---|---|---|
| 녹음(AudioRecord VOICE_COMMUNICATION, 단말 전처리) | 20~40 ms | `android_jni_dev.c` 녹음 생성 | 추정(단말마다 다름) |
| 녹음 지연 버퍼 `cap_dbuf` | 10~40 ms, W999 는 통화 중 105회 비었다 | `sound_port.c`·`delaybuf.c` | 구조 = 사실, ms = 추정 |
| 믹서 0번 슬롯(클록 순서) | +20 ms 고정 | §3 L1 | 사실 |
| 프레임·AMR-WB 알고리즘 | 20 + 5 ms | AMR-WB 규격 | 일반 지식 |
| 망 + CMP 중계 | RTT/2 ≈ 6~12 ms, CMP < 1 ms | 통화 통계, `PRtpRelay.cpp` 같은 스레드 즉시 전달 | 실측·사실 |
| 지터 버퍼 | 40 ms(최대 60) | 통화 통계 XR | 실측 |
| 재생 지연 버퍼 `play_dbuf` | 10~60 ms | `sound_port.c` | 추정 |
| AudioTrack 버퍼 + 일반 믹서 + HAL(VOIP_RX 20 ms) | 40 + 20~40 ms | 트랙 로그·`dumpsys media.audio_flinger` | 트랙 = 실측, 믹서 = 추정 |

통화 통계의 «end system delay» 는 측정값이 아니다 — `rtcp_xr.c:350` 이 RTT/2 + 지터 버퍼 + 상수(녹음 100 + 재생 140~160 ms)로 계산한다. 판단에 쓰지 않는다.

## 3. 남은 후보 (권장 순서)

### L1. 소프트웨어 클록 순서 — +20 ms 고정 (작은 엔진 패치)

- **문제** — Android 는 `PJSUA_DEFAULT_SND_USE_SW_CLOCK = TRUE` 라 믹서를 소프트웨어 클록이 돌린다. 클록 콜백(`sound_port.c` `clock_callback`, 268~296행)이
  재생 `get_frame`(믹서 출력) → 녹음 `put_frame`(믹서 0번 슬롯 입력) 순서라, 그 틱에 들어온 마이크 프레임은 다음 틱의 믹서 출력에서 나간다.
- **방법** — 녹음 `delay_buf_get` → `put_frame` 을 재생 `get_frame` 앞으로. 믹서가 슬롯 0 입력을 같은 틱에 쓰는지 먼저 확인(`conference.c` put_frame
  보관 → get_frame 송출 경로).
- **위험** — 재생·녹음 순서에 기대는 AEC 기준 신호 정렬(Speex AEC 는 `sound_port.c` 에서 따로 맞춘다 — 같이 확인).
- **확인** — 송신 지연을 직접 재는 방법이 없으므로 루프백(같은 단말 두 계정 또는 계측기 실기기 풀)으로 왕복 지연 비교.

### L2. 통화 중 Wi-Fi 저지연 잠금 (앱·SDK 몇 줄)

- **문제** — 단말이 Wi-Fi 절전 상태면 공유기가 하향 패킷을 비컨 주기마다 몰아 보낼 수 있다. 반영 전 지터 최대 47~79 ms·지터 버퍼 최대 280~500 ms 가
  이런 몰림으로 보인다(추정). 코드에 WifiLock 이 없다(`android/`·`sdk/android` 검색 0).
- **방법** — 통화(VoLTE 호·PTT 세션) 동안 `WifiManager.createWifiLock(WIFI_MODE_FULL_LOW_LATENCY)`(API 29+) 를 잡고 끝나면 놓는다. 두 앱이 함께 쓰도록
  SDK 플랫폼 층(`sdk/android/cimsue` `platform/` — `UeForegroundService` 의 wake lock 옆)에 둔다.
- **위험** — Android 문서상 저지연 잠금은 앱이 전경이고 화면이 켜졌을 때만 효과가 있다. 화면을 끈 PTT 수신에는 효과가 없을 수 있다(그때 쓸 모드는 실측 후 결정).
  배터리.
- **확인** — 같은 조건에서 통화 통계 RX 지터 최대·지터 버퍼 최대 비교.

### L3. 재생을 FAST 경로로 — 수십 ms (큰 변경)

- **문제** — 반영 후에도 성능 모드 0. 장치를 16 kHz 로 열어 단말 기본 출력(48 kHz)과 달라 AudioFlinger 가 일반 믹서 경로로 보낸다(추정 — FAST 트랙은
  출력 속도와 같아야 한다).
- **방법 A** — pjsua `media_cfg.snd_clock_rate = 48000`(엔진이 16 ↔ 48 kHz 변환). 에코 제거도 48 kHz 로 돌아 CPU 가 는다.
- **방법 B** — Oboe/AAudio 백엔드(pjmedia `oboe_dev.cpp`, configure `--with-oboe`). 저지연·전용 모드를 쓸 수 있지만 **CIMS 무전/통화 분리 라우팅·송화 마이크
  고정 패치(`android_jni_dev.c`, README.CIMS.md)를 새 백엔드로 옮겨야 한다.**
- **확인** — `Audio track buffer … performance mode` 로그(1 = LOW_LATENCY), 통화 중 `dumpsys media.audio_flinger` 의 트랙 플래그(FAST)·출력 스레드.

### L4. 녹음 경로·녹음 지연 버퍼

- **문제** — W999(MediaTek)는 녹음 스레드가 22~25 ms 마다 읽는다는 HAL 경고(`AudioALSACaptureDataProviderNormal readThread … TIMEOUT`)와 함께
  `capdbuf Underflow` 가 84 s 통화에 105회였다. 비면 프레임을 만들어 채우고(음질), 몰려 오면 버퍼가 커진다(지연).
- **방법** — L3 과 함께(AAudio 입력 저지연) 또는 녹음 버퍼 크기·스레드 우선순위 조정. 먼저 MF52(Qualcomm)와 비교 측정.
- **확인** — `capdbuf Underflow` 횟수, 녹음 지연 버퍼 크기 조정 로그(`Buffer size adjusted`, 레벨 4).

### L5. 지터 버퍼 조정 · TS 26.114 대조

- **현재** — pjmedia 적응형(초기 0, 선행 채움 1~20 프레임, 상한 500 ms, 점진 버림 2~10 s). 반영 후 40/60 ms 라 급하지 않다.
- **방법** — 몰림이 남으면 pjsua `MediaConfig` `jbMaxPre`·`jbMax` 로 상한을 낮춘다(손실과 맞바꿈). GSMA IR.92 §3.2.6 은 TS 26.114(§8) 지터 버퍼 최소
  성능 요건을 요구한다 — 요건 표(지연·손실 대 기준 지연 프로파일)로 대조하는 시험이 없다. 계측기 망 지연 프로파일로 만들 수 있는지 검토.

### L6. 에코 제거 이중

- **문제** — 녹음 입력이 VOICE_COMMUNICATION(단말 AEC·NS)인데 엔진 Speex AEC(꼬리 200 ms — 로그 `Speex AEC created … tail length=200 ms`)도 돈다. 지연은
  더하지 않지만 CPU·음질(이중 처리) 영향. `sdk/engine/config_site/common.h` 의 «Android 는 Speex AEC 가 빠져 있다» 주석은 사실과 다르다.
- **방법** — 단말 AEC 가 있으면(`AcousticEchoCanceler.isAvailable`) 엔진 AEC 를 끄는(ec_tail 0) 선택지 — 스피커 출력 에코 실측이 전제. 주석 정정.

### L7. AMR-WB 대역 효율 형식 (규격 — 지연과 무관)

- GSMA IR.92 §3.2.5 — 단말은 대역 효율·octet-aligned 둘 다 지원하고, 발신 때 **대역 효율을 요청**해야 한다. 지금은 코어가 `octet-align=1` 만 제안한다
  (`engine.cpp` `applyCodecPolicy`). 서버(CSP 코덱 표·CMP 녹취·트랜스코딩)의 AMR 해석과 함께 바꿔야 하는 교차 변경 — 별건.

### L8. PTT 앱 재생 경로 확인

- PTT 는 출력 라우트 캡을 쓰면 STREAM_MUSIC 으로 재생한다(분리 라우팅). 저지연 지정으로 어느 출력(FAST·일반·deep buffer)을 탔는지, 분리 라우팅이 그대로인지
  PTT 그룹 호 한 번으로 확인 — `Audio track buffer … performance mode` 로그와 출력 장치.

### L9. Windows·Linux 엔진 재빌드

- ptime 20 은 세 플랫폼 공통(config_site `common.h` + `endpoint.c` + 코어)이다. Windows 관제 앱(cimsue.dll — API 변화 없음, 엔진만)과 Linux `cimsue-cli`·
  `pkg/pjproject` 는 다시 빌드해야 반영된다. Linux 빌드는 `ext/pjproject` 를 제자리에서 다시 구성하므로 Android 구성 상태를 덮는다 — Linux 를 빌드한 뒤
  Android 는 `sdk/android/build-native.sh` 를 다시 돌린다.

## 4. 측정 방법

통화를 끝내면 엔진이 미디어 통계를 로그로 남긴다(`[DISCONNECTED]` 아래). 읽을 것:

| 항목 | 로그 |
|---|---|
| 송신 패킷 크기 | `TX pt=96, ptime=20` |
| 수신 지터·손실·RTT | `RX … jitter`, `pkt loss`, `RTT msec` |
| 지터 버퍼 | VoIP Metrics `JB delay : cur=…, max=…` — **`TX` 블록 = 자기 수신 쪽**(상대에게 보낸 보고), `RX` 블록 = 상대 수신 쪽(상대가 보낸 보고) |
| 코덱 출력 | `Encoder/Decoder failed to get output Buffer`, `dropped … late output(s)`, `always later than` |
| 재생 트랙 | `Audio track buffer N/M frames (asked K), performance mode P`, 정지 때 `Audio track underruns: n` |
| 지연 버퍼 | `capdbuf`/`playdbuf` `Underflow`·`Buffer size adjusted` |
| 출력 경로 | 통화 중 `adb shell dumpsys media.audio_flinger`(트랙 플래그·출력 스레드·HAL 버퍼) |

체감 비교는 같은 두 단말·같은 위치에서 «하나 둘 셋» 따라 말하기 같은 단순한 방법으로 하고, 가능하면 계측기 실기기 풀([ue_voice_quality.md](../design/features/ue_voice_quality.md))
로 입에서 귀 지연을 직접 잰다.
