# 단말 음성 레벨 — 마이크 AGC · 리미터 · 음량 배선

단말(Android PTT/VoLTE/관제 태블릿, Windows 관제 앱, `cimsue-cli`)이 **보내는 음성의 크기**와 **듣는 음성의
크기**를 정하는 구조의 정본이다. 엔진은 `ext/pjproject` 하나이므로 레벨 처리도 엔진(conference bridge)에
두고, 앱은 사용자 조작을 엔진의 두 축(스피커 배율·마이크 목표)에 옮기기만 한다.

관련: [android_ue_client.md](android_ue_client.md)(PTT 설정 화면·채널 음량), [ue_sdk.md](ue_sdk.md)(SDK
`setRxLevel`), [recording.md](recording.md)(녹취 트랙 형식 — 측정 원천), [test_instrument.md](test_instrument.md)
(P.56 샘플·MOS).

## 1. 기준

| 항목 | 값 | 근거 |
|---|---|---|
| 레벨 척도 | 활성 음성 레벨(dBov, 0 dBov = 32768 RMS) | ITU-T P.56 method B |
| 송신 목표 | **-26 dBov** | P.56 시험 관례 레벨 — 계측기 동봉 샘플(`gen_samples.py`)과 같은 자 |
| 풀스케일 한계 | 피크 **-1 dBFS** | 리미터 한계. 인코더 입력에서 포화가 생기면 복구할 수 없다 |
| AGC 요건 | 음성에서만 학습·잡음 비증폭·원단 재생 중 학습 정지·게인 범위와 변화율 상한 | ITU-T G.169 (AGC 장치) 요건 방향 |

단말 음향 특성(SLR/RLR, 3GPP TS 26.131)은 단말 하드웨어의 몫이라 여기서 다루지 않는다. 이 문서의 대상은
**디지털 레벨** — 같은 앱을 쓰는 단말끼리도 마이크 디지털 레벨이 34 dB 넘게 벌어진다(서버 녹취 P.56 측정:
활성 레벨 -46 dBov 단말 ↔ -12 dBov 단말). 고정 배율 하나로는 작은 단말을 키우면 큰 단말이 잘리고 그 반대도
같으므로, 크기 맞추기는 측정값에 따라 게인을 정하는 AGC 가 한다.

## 2. 방향 규약 (pjsua2 AudioMedia)

pjsua2 `AudioMedia` 의 방향은 **미디어 포트 관점**이다. conference bridge 관점(pjmedia/pjsua C API)과 이름이
반대라 이름만 보고 걸면 축이 뒤바뀐다.

| pjsua2 호출 | bridge 관점 | slot 0(장치) 에서 | 통화 포트에서 |
|---|---|---|---|
| `adjustRxLevel(x)` | tx — bridge → 포트 | **스피커**로 나가는 크기 | **상대에게 보내는** 내 음성 크기 |
| `adjustTxLevel(x)` | rx — 포트 → bridge | **마이크**에서 들어오는 크기 | 상대에게서 **받아 듣는** 크기 |

`AudDevManager::getCaptureDevMedia()` 와 `getPlaybackDevMedia()` 는 **같은 slot 0** 객체를 돌려준다. 이름이
"캡처" 라고 해서 그 객체의 `adjustRxLevel` 이 마이크를 바꾸지 않는다. 앱·SDK 가 쓰는 곳은 셋뿐이다.

| 조작 | 호출 | 위치 |
|---|---|---|
| 스피커 크기 | `playbackDevMedia.adjustRxLevel(spk)` | `SipController.applyDeviceAudioBoost`, libcimsue `Engine::setDeviceAudioLevels` |
| 마이크 크기 | `AudDevManager.setCaptureAgc(true, 목표)` (배율 `adjustTxLevel` 은 1 고정) | 〃 |
| 호별 듣는 크기 | 통화 `AudioMedia.adjustTxLevel(level)` | `CimsCall.setRxLevel`, libcimsue `Engine::setRxLevel` |

## 3. 신호 경로

```
 송신  마이크 ─ 단말 HW 처리(VOICE_COMMUNICATION: AEC/NS/AGC 는 제조사 몫)
        └─ SW AEC (Android = WebRTC AEC, Windows/Linux = Speex AEC — 전처리 AGC 끔)
            └─ bridge slot 0 rx : [AGC → 목표 -26 dBov] × 마이크 배율(1) → [리미터]
                └─ 통화 포트로 믹스 → tx : × 통화 포트 배율 → [리미터] → 인코더(AMR-WB) → RTP
                                                                           └─ CMP 녹취 = 이 지점

 수신  RTP → 디코더 → 통화 포트 rx : × 듣는 크기(채널 음량) → [리미터]
        └─ slot 0 으로 믹스(다중 호 합산 넘침 = pjmedia mix_adj 정규화) → tx : × 스피커 배율 → [리미터] → 스피커
```

- **송화 마이크 선택(PTT).** 통화 입력(VOICE_COMMUNICATION)은 출력이 스피커면 안드로이드 정책이 **후면 마이크**
  (`AUDIO_DEVICE_IN_BACK_MIC`)를 고른다 — 탁자 위 스피커폰 설계라, 입에 대고 말하는 무전은 반대쪽 마이크로 받는다
  (MF52 실측 — 마이크가 하나뿐인 W999 는 HAL 이 기본 마이크로 강제해 해당 없음). PTT 는 단말 스피커·수화기로 들을 때
  입력 라우트 EARPIECE 로 **내장 기본(하단) 마이크를 고정**하고(`SipController.setAudioRoute` → pjsua2 `setInputRoute`
  keep → `android_jni_dev.c` `AudioRecord.setPreferredDevice`, 주소 `back` 은 건너뜀), 이어폰·헤드셋이면 고정을 푼다.
  고정 뒤 MF52 스피커 모드 HAL 입력은 약 +4 dB — 나머지 차이(수화기 모드 대비 ~20 dB)는 HAL 의 핸즈프리 튜닝(스피커
  출력 시 마이크 게인)이라 앱이 바꿀 수 없고 AGC 가 메운다. VoLTE 스피커폰(전이중)은 에코 때문에 정책 선택(후면)을 둔다.
  ⚠ pjmedia 의 `INPUT_ROUTE` 와 `INPUT_SOURCE` 는 같은 캡 비트다 — 입력 소스는 값에 `ROUTE_CUSTOM` 을 섞어 구분한다.
- **AGC 는 하나다.** Speex AEC 전처리의 AGC(목표 ≈ -12 dBov)는 `sdk/engine/config_site/common.h`
  `PJMEDIA_SPEEX_AEC_USE_AGC 0` 으로 끈다(Windows·Linux). Android 는 Speex AEC 가 빠져 있고 WebRTC AEC
  경로에는 AGC 가 없다. 단말 HW AGC 는 앱이 제어하지 않는다 — 켜져 있어도 bridge AGC 가 결과 레벨을 맞춘다.
- **리미터는 게인을 곱하는 포트 경로(rx·tx)의 끝**에 있다. 배율이 1 이고 리미터가 풀려 있으면 경로를
  건너뛴다(추가 비용 없음). 연결 단위 배율(`pjmedia_conf_connect_port` 의 `adj_level`, pjsua2
  `startTransmit2`)은 pjmedia 하드 클립 그대로다 — 앱·SDK 는 쓰지 않는다.
- 직렬 conference 백엔드(`conference.c`, 기본)만 해당한다. 병렬 백엔드(`conf_thread.c`,
  `PJMEDIA_CONF_THREADS`)는 쓰지 않으며 레벨 처리도 없다.

## 4. 마이크 AGC

구현 = `pjmedia/src/pjmedia/cims_level.h`(`cims_agc_*`), `conference.c` 가 slot 0 rx 에서 부른다. slot 0
은 bridge 생성 때 켜지고(`PJMEDIA_CONF_CIMS_MIC_AGC`), 학습 상태는 사운드 장치 재오픈·캡처 게이트 전환을
넘어 유지된다(slot 0 은 endpoint 수명 동안 하나).

| 단계 | 동작 | 값 |
|---|---|---|
| 잡음 바닥 | 프레임 파워가 바닥보다 작으면 빠르게, 크면 +1 dB/s 로 따라간다 | — |
| 음성 판정 | 프레임 레벨 > 잡음 바닥 + 9 dB **그리고** > -62 dBov | `CIMS_AGC_REL_GATE_DB`·`ABS_GATE_DB` |
| 원단 게이트 | slot 0 tx(스피커) 프레임이 -50 dBov 초과면 이후 0.5 s 동안 학습·게인 갱신 정지 | `CIMS_AGC_FAR_*` |
| 초기 수렴 | 음성 누적 1 s 동안은 누적 평균 + 게인 변화 ≤ 60 dB/s — 앱 기동 뒤 첫 발언 안에 수렴 | `CIMS_AGC_WARM_*` |
| 레벨 학습 | 음성 프레임 파워의 지수 평활(P.56 활성 레벨과 같은 척도) | τ = 1 s (`CIMS_AGC_TAU_S`) |
| 게인 | `목표 − 학습 레벨`, 범위 -20..+30 dB, 올림 ≤ 10 dB/s · 내림 ≤ 30 dB/s | `PJMEDIA_CONF_CIMS_AGC_*`, `CIMS_AGC_UP/DOWN_*` |
| 무음 구간 | 게인 유지(잡음 펌핑 방지) | — |
| 적용 | 직전 게인에서 새 게인으로 프레임 앞 2 ms 선형 램프 | `CIMS_LVL_RAMP` |

API: pjmedia `pjmedia_conf_set_rx_agc(conf, slot, enable, target_dbov)` · pjsua `pjsua_conf_set_rx_agc` ·
pjsua2 `AudDevManager::setCaptureAgc(enable, targetDbov)`. 목표는 -40..-10 dBov 로 제한한다. 끄고 켜도 학습
상태는 남는다. 다른 포트에도 켤 수 있으나(수신 화자 정규화 용도) 원단 게이트는 slot 0 에만 있다.

수렴·흩어짐(녹취 세션 비교 — 화자 4명 × 6 세션 × 6 발언, 세션마다 AGC 새로 시작):

| 지표 | 값 |
|---|---|
| 둘째 발언부터 중앙값 / 목표와의 편차 중앙값 | -25.9 dBov / 1.1 dB |
| 둘째 발언부터 p10–p90 흩어짐 | 7.8 dB (게인을 완벽히 고정했을 때 8.8 dB — 화자 자신의 말 크기 변화) |
| 첫 발언(앱 기동 직후) | 조용한 단말 -31 · 큰 단말 -22 dBov |

- 발언마다 오르내리는 폭은 대부분 **화자의 말 크기 변화**다. 수렴 뒤를 더 느리게(τ 3~5 s) 하면 그 변화가 그대로
  지나가 흩어짐이 오히려 커진다(9.7~11.5 dB) — 느린 설정은 쓰지 않는다.
- 큰 단말의 첫 발언 값이 큰 것은 게인이 내려가기 전 말머리(약 0.3 s)가 에너지 평균을 끌어올리기 때문이다.
  들리는 것은 짧은 말머리이고 리미터가 풀스케일을 막는다. 첫 음성 몇 프레임을 기다렸다 움직이는 방식은
  개선이 없었다.
- 게인 상한 +30 dB — 실측상 가장 작은 단말(MF52 스피커 모드, Qualcomm Fluence 음성 경로 — 단말 AGC 없음, 핸즈프리
  튜닝으로 마이크 게인이 수화기 모드보다 ~20 dB 낮음)이 -44~-53 dBov 로 보내 +27 dB 까지 필요하다. 그 단말의 잡음 바닥은 -85 dBFS(단말 잡음 억제)라 +30 dB 에서도 -55 dBov 다.
  AGC 는 음성 레벨로 게인을 정하므로 신호 대 잡음비는 바뀌지 않는다. 이보다 작은 단말은 상한에서 멈춘다.
- 레벨 추정은 에너지(파워) 평균이다. 큰 발언 뒤 몇 초간 게인이 낮게 머무는 것은 이 방식의 성질이다. dB 평균
  (보정 포함)이나 오름·내림 시간상수를 달리한 비대칭 평균은 녹취 비교에서 흩어짐이 12~16 dB 로 더 나빴다.
- 진단: pjsip 로그 수준 5 에서 slot 0 AGC 상태를 1 초마다 `CIMS-AGC slot0 frame= noise= level= gain= far_hang=`
  로 남긴다(마이크가 bridge 에 연결된 동안만).

## 5. 리미터

구현 = `cims_level.h`(`cims_limiter_run`). 포트마다 rx·tx 리미터 상태가 따로 있다.

- 프레임 피크가 한계(-1 dBFS)를 넘으면 그만큼만 줄인다. bridge 는 프레임 전체를 먼저 보므로 **현재 게인으로
  한계를 처음 넘는 샘플 앞에서 램프가 끝나게** 해(프레임 안 look-ahead) 초과 샘플이 원리적으로 없다.
- 풀림은 시간상수 200 ms(`PJMEDIA_CONF_CIMS_LIMIT_RELEASE_MS`)로 1 쪽으로 가되, 그 프레임 피크를 넘기지
  않는 선까지.
- 최종 하드 클립은 부동소수 반올림용 안전망이다. `PJMEDIA_CONF_CIMS_LIMITER 0` 이면 원래 pjmedia 하드 클립
  경로로 돌아간다.

## 6. 앱 조작

| 앱 | 조작 | 엔진에 걸리는 것 | 기본 |
|---|---|---|---|
| PTT 설정 | 스피커 게인 ×1.0~×3.0 | 스피커 배율 | ×1.5 |
| PTT 설정 | 마이크 게인 ×1.0~×3.0 | 마이크 AGC 목표 = `-26 + 20·log10(값)` dBov (×2 = -20 dBov) | ×1.0 |
| PTT 채널 상세 | 수신 음량 0~2 | 그 그룹 통화의 듣는 크기 | 신규 그룹 2 |
| PTT | 전 통화 종료 | 스피커 ×1.0 · 마이크 목표 -26 으로 원복 | — |
| VoLTE | (조작 없음) | 마이크 AGC 기본 -26 dBov | — |
| SDK `setRxLevel` | 관제 감청 창 음량 등 | 그 호의 듣는 크기 | 1 |
| SDK `setDeviceAudioLevels` | 앱이 PTT 설정 슬라이더를 옮길 때 | 스피커 배율 · 마이크 AGC 목표(dBov — 슬라이더 값 → 목표 환산 `-26 + 20·log10(값)` 은 앱) | 부르기 전까지 엔진 기본(×1 · -26) |

- 스피커 배율 × 채널 음량은 곱으로 적용되고(최대 ×6), 초과는 리미터가 막는다.
- 저장값 판(`gain_wiring` = 2): PTT 의 `audio_route`(스피커·마이크 값)와 `group_volume`(채널 음량)은 판이
  2 보다 낮으면 한 번 비우고 기본값에서 시작한다. 판 1 의 값은 §2 방향이 다르게 걸린 상태에서 맞춘 것이라
  새 배선에서 의미가 없다.

## 7. 측정·검증

**녹취 측정기** `cims-rec-level`(`tester/reclevel/`, `build/bin/`) — CMP 녹취 트랙(.rtp)을 디코드(AMR-WB
octet-align · PCMU · PCMA)해 P.56 활성 레벨과 포화 지표를 낸다. 녹취는 단말 인코더 출력이므로 **송신** 쪽
판정 기준이다. 수신(스피커) 쪽은 단말에서 따로 잰다.

```
cims-rec-level <seg_NNNN_audio.rtp>                      # 트랙 하나 → JSON
cims-rec-level --dir <녹취 루트>/ptt --since 2026-09-01 --summary   # 화자별 p10/중앙/p90·포화 턴 비율
```

| 지표 | 뜻 | 판정 |
|---|---|---|
| `active_dbov` | P.56 활성 레벨 | 둘째 발언부터 -26 ± 3 dBov (마이크 게인 ×1.0) |
| `flat_runs` / 요약 `sat%` | 2샘플 이상 이어진 포화(≥ 32700) 구간 / 그런 구간이 있는 턴 비율 | 0 / 0 % |
| `peak_dbfs` | 트랙 피크 | ≤ -1 dBFS (리미터 한계) |
| `hot_frames` | 피크 ≥ -1 dBFS 인 20 ms 프레임 수 | 참고 |

P.56 계산은 계측기 샘플 생성기(`gen_samples.py`)와 같고, 동봉 샘플 메타값과 일치한다(-16.0 / -26.2 dBov).

**엔진 시험** — `cims_level.h` 는 헤더 하나라 호스트에서 단독 컴파일해 녹취 PCM 을 흘릴 수 있고, conference
통합은 Linux 로 빌드한 pjmedia 에 `put_frame`(마이크)·`get_frame`(스피커)을 20 ms 단위로 흘려 확인한다.
확인 항목: 조용한/큰 단말의 수렴, 옛 배율(×1.5·×2)이 남아도 포화 0, 스피커 ×3 에서 포화 0, 원단 에코만 있을
때 AGC 켬/끔 송신 레벨 차 0 dB(게이트를 끄면 에코가 음성으로 학습되어 +7 dB).

## 8. 미해결 / 향후

- **실기 검증** — W999·MF52·협력업체 단말에 새 APK 설치 뒤 `cims-rec-level --summary` 로 화자별 중앙값
  -26 ± 3 dBov·`sat%` 0 확인, 귀 판정(첫 발언 수렴 체감·잡음 증폭·통화 중 에코), 스피커 ×3·채널 2 청취.
- **Windows 관제 앱** — Speex AGC 를 끈 엔진으로 재빌드 뒤 감청 창 음량 슬라이더가 청취 크기를 바꾸는지 실기
  확인(ue_sdk.md §11 Windows 오디오 실측과 함께).
- **이중 AEC** — 단말 HW AEC(VOICE_COMMUNICATION) 위에 SW AEC(WebRTC/Speex, tail 200 ms)가 또 돈다. 음질
  영향(말끝 잘림·잔향)은 별도 과제로 실측한다.
- **수신 화자 정규화** — 옛 앱이 남은 단말(판 1)은 -12 dBov 로 보내므로 청취 단말에서 크게 들린다(리미터가
  포화는 막음). 모든 단말이 새 엔진이면 불필요하다. 필요하면 통화 포트 rx AGC(§4 API)로 켤 수 있다.
- PTT 스피커 모드의 핸즈프리 튜닝 우회 — 통신 장치를 수화기로 두고 재생만 트랙 단위로 스피커에 고정하면 HAL 이
  수화기 튜닝(높은 마이크 게인)을 쓸 수 있다. 다만 MF52 는 통신 장치를 스피커로 지정해야 스피커 출력이 되는 실측이 있어
  (android_ue_client.md 통화 오디오 모드) 회귀 위험이 커, 필요성이 확인되면 별도로 시험한다.
