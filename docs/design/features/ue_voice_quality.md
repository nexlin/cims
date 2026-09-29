# 단말 음성 품질 측정 + 시험 모드 계측기 연동

> 실단말(VoLTE·VoIP·PTT 앱, 관제조작반 앱)이 **자기 호의 미디어 품질(손실·지터·지연·MOS)을 스스로 재고**, **시험 모드**에서
> 운영자가 설정한 **계측기 워커(ip/port)에 직접 접속**해 계측기의 구동을 받고 품질을 보고하는 구조의 정본.
>
> - **시험 대상 서버(CSP·CSC·CMP)를 보고·제어 경로로 쓰지 않는다.** 대상이 흔들리면 계측 경로도 흔들리고, 계측 트래픽이 대상의 부하·통계를
>   오염시키기 때문이다. 단말 ↔ 계측기 링크는 시험 대상 밖(계측기 워커 호스트)에서 끝난다 — 워커를 시험 대상 밖 호스트에 두는
>   계측기 원칙([test_instrument.md](test_instrument.md) §1)과 같다.
> - 측정은 **코어 `libcimsue` 한 곳**에 둔다 — 모든 앱·`cimsue-cli`(계측기 `real-ue`)가 같은 측정 코드와 같은 E-model 을 쓴다.
> - 링크 프로토콜은 **`cimsue-cli drive` 구동 모드 그대로**(한 줄 명령 / 한 줄 JSON 이벤트, [ue_sdk.md §4.7](ue_sdk.md))를 TLS 소켓 위에 싣는다.
>   계측기 워커는 실단말을 `real-ue` 와 같은 링크로 다루므로 **단계 실행기·지표·판정이 그대로** 실단말에도 적용된다.
> - 시험 모드는 **앱 안의 숨김 진입**으로 켠다. 서버 쪽 허용 플래그·전용 권한은 두지 않는다 — 사용자가 계측기 주소를 직접 넣고 연결하는 것이 동의다.
> - 객관 음질(POLQA P.863·PESQ P.862)은 범위 밖 — 전송 지표 기반 E-model(G.107/G.107.1) 추정만 한다.
>
> 관련: [ue_sdk.md](ue_sdk.md)(코어·바인딩·`cimsue-cli`), [test_instrument.md](test_instrument.md)(풀·지표·워커 E-model §5·`real-ue` §3.3),
> [media_security.md](media_security.md)(SRTP — RTCP 도 SRTCP), [../modules/cmp.md](../modules/cmp.md)(1:1 relay 의 RTCP 중계).

---

## 1. 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 측정 위치 | 코어 `libcimsue` `quality/` 모듈. 앱은 표시만 | 앱 4종 + `cimsue-cli` 가 같은 값을 낸다. 계측기 `real_*`·`device_*` 지표가 같은 식 |
| 측정 원천 | pjmedia RTCP 통계 + **RTCP-XR**(RFC 3611, 빌드에서 켬) | pjmedia 가 손실·폐기·버스트/갭·RTT·단말 지연을 이미 계산한다. R/MOS 칸만 비어 있다(`rtcp_xr.c` 127 = 없음) |
| 품질 추정 | ITU-T **G.107** E-model(협대역) · **G.107.1**(광대역, AMR-WB·G.722) | 전송 지표만으로 추정. 기준 음원 비교(POLQA/PESQ)는 제외 |
| 보고·제어 경로 | **단말 → 계측기 워커 직접 TLS 링크**(`Device.Port`). CSP·CSC·CMP 를 거치지 않는다 | 계측 경로를 시험 대상에서 분리 |
| 링크 프로토콜 | `drive` 구동 모드(명령 줄 / JSON 이벤트 줄) + 링크 전용 `hello`·`use`·`quality`·`media` | `real-ue` 와 한 프로토콜 — 워커는 전송(파이프 / 소켓)만 다르다 |
| 접속 방향 | **단말이 먼저** 워커에 연결(아웃바운드), 끊기면 재접속 | NAT·방화벽 뒤 단말도 붙는다. 워커가 단말 주소를 알 필요가 없다 |
| 시험 모드 진입 | 앱 **정보 화면의 버전 줄 7회 연속 탭**(Windows 는 클릭) → 시험 모드 메뉴 → 계측기 주소·연결 | 안드로이드 시스템 설정의 개발자 옵션을 쓰지 않는다. 일반 사용자에게 숨긴다 |
| 권한 | 서버 허용 플래그·전용 capability 없음. 단말 쪽 게이트 = 시험 모드 + 사용자가 넣은 계측기 주소. 워커 쪽 선택 게이트 = 연결 키 | 시험 대상 서버는 이 기능을 모른다 |
| 계측기 | 풀 종류 **`device`** — 연결된 실단말을 번호로 골라 `real-ue` 와 같은 단계로 구동. 지표 `device_*` | USB·adb 없이 현장 단말 그대로 |
| 시험 KPI 용어 | ETSI TS 102 250-2 / ITU-T E.804 의 전화 서비스 KPI 에 맞춘다 | 시험 호 방법론의 표준 용어로 보고 |

### 1.1 규격 관계

| 부분 | 근거 |
|---|---|
| 지표 정의 | RFC 3550(A.3 손실·A.8 지터·§6.4.1 RTT) · RFC 3611 §4.7(VoIP Metrics) |
| 품질 추정 | ITU-T G.107 · G.107.1 · 코덱 상수 G.113 |
| 시험 KPI | ETSI TS 102 250-2 / ITU-T E.804 — 접속 성공률(service accessibility)·설정 시간(setup time)·절단율(cut-off ratio)·호별 음성 품질 |
| 단말 ↔ 계측기 링크 | 규격 없음 — 계측기 내부 계약(워커 ↔ 컨트롤러 계약과 같은 층). 시험 대상의 SIP·미디어 절차에는 아무것도 더하지 않는다 |

## 2. 구조

```
┌──────────── 단말 (Android 앱 3종 · Windows 관제 앱) ────────────┐
│ 앱: 시험 모드 화면(계측기 주소·연결 상태·오버레이·요약·이력·기준 음원) │
│ ─────────────────────────────────────────────────────────────── │
│ libcimsue  quality/ (RTCP-XR·E-model)   drive/ (구동 세션)   Engine │
└──────┬──────────────────────────────────────────┬────────────────┘
       │ SIP · RTP/RTCP(+XR)  — 평소 그대로          │ 계측 링크 TLS (drive 줄 프로토콜)
       ▼                                           ▼
┌──── 시험 대상 ────┐                  ┌──── 계측기 워커 (대상 밖 호스트) ────┐      ┌── 컨트롤러 ──┐
│ CSP · CMP · CSC   │  (계측 경로 없음)  │ DeviceHub(Device.Port 7120)         │─────►│ device 풀   │
└───────────────────┘                  │  └ 연결 단말 = 실스택 Endpoint(device)│ 스트림 │ 지표·판정    │
                                       │ ue / peer / real-ue 풀 (상대 역할)    │      │ 콘솔         │
                                       └──────────────────────────────────────┘      └─────────────┘
```

`cimsue-cli drive`(`real-ue`)는 같은 `drive/` 세션을 stdin/stdout 으로, 앱은 TLS 소켓으로 연결한다 — 코어의 구동 세션은 하나다.

## 3. 측정 (코어 `quality/`)

### 3.1 지표

방향은 **수신 기준**(내가 받은 스트림)이 원칙이다. 상대가 RTCP-XR 을 보내면 상대가 받은 품질(내가 보낸 스트림)도 원격 지표로 함께 싣는다.

| 지표 | 원천 | 정의 |
|---|---|---|
| 수신·송신 패킷/바이트 | pjmedia `rx/tx` 통계 | 누적 |
| 망 손실률 | RTCP 통계(RFC 3550 A.3) | 기대 패킷 대비 도달하지 않은 비율 |
| 지터버퍼 폐기율 | XR VoIP Metrics(RFC 3611 §4.7.1) | 늦거나 넘쳐 버린 비율 — 망 손실과 따로 센다 |
| 버스트/갭 밀도·길이 | XR `burst_den`·`gap_den`·`burst_dur`·`gap_dur`(Gmin 16) | 평균이 같아도 몰린 손실을 구분 |
| 지터 | RFC 3550 A.8 | 평균·최대(ms) |
| 왕복 지연 RTD | RTCP SR/RR LSR·DLSR(RFC 3550 §6.4.1), pjmedia `rtcp.stat.rtt` | CMP 가 1:1 relay 에서 RTCP 를 중계하므로 **단말↔상대 종단 RTT** |
| 단말 지연 ESD | 지터버퍼 평균 지연(pjmedia jbuf `avg_delay`) + 코덱 프레임·lookahead + 장치 추정 30 ms | 자기 단말 안의 지연. pjmedia XR 의 `end_sys_delay` 는 RTD/2 를 포함해 계산하므로(자체 추정) 망 지연과 겹치지 않게 쓰지 않는다 |
| 신호·잡음 레벨 | XR `signal_lvl`·`noise_lvl`(dBm0) | 필드는 싣지만 pjmedia 가 계산하지 않아 127(없음) — 기준 음원 도달 레벨 확인(목표 -26 dBov, P.56)은 §9 과제 |
| 무음 leg | 받은 패킷 0 | `rx.valid=false`, MOS 는 -1(추정하지 않음). DTX(AMR-WB SID)·floor 게이트 구간의 무수신은 계측기가 따로 가린다 |
| R · MOS-LQ · MOS-CQ | §3.2 E-model | LQ = 지연 손상(Id) 제외 청취 품질, CQ = 지연 포함 대화 품질 |
| 코덱 | 협상 결과(PT·rtpmap·fmtp) | E-model 코덱 상수 선택 |
| PTT floor | floor 참가자 상태머신 | 요청→허가 시간(TS 22.179 KPI 1 MCPTT access time). 그룹 미디어는 RTCP 가 없어 RTD·원격 지표 없음 |

### 3.2 E-model

단일 정의는 코어 `sdk/core/src/quality/emodel.h`(헤더 전용·의존 없음, 네임스페이스 `cimsue::emodel`)이고, 계측기 워커의
`tester/worker/src/EModel.h` 는 이 헤더 위의 얇은 층(워커 입력 손실·지터·망 지연 → Ta)이다 — floor 정의 단일화([ue_sdk.md §4.6](ue_sdk.md))와 같은 방식.
품질 계산(pjmedia 통계 → `CallQuality`)은 pj 타입 없는 평문 입력을 받는 `sdk/core/src/quality/call_quality.{h,cpp}` 가 하고, 엔진은 pjsua2
`StreamStat`·`pjsua_call_get_stream_stat_xr`(CIMS 패치) 결과를 그 입력으로 옮긴다.

- **R = Rmax − Id − Ie,eff**(G.107 §7 의 기본 입력 — Ro − Is 와 A = 0 을 접은 값). 협대역 Rmax = 93.2.
- **Ie,eff = Ie + (95 − Ie) · Ppl / (Ppl / BurstR + Bpl)**. Ppl = 망 손실 + 지터버퍼 폐기(사용자가 겪는 손실은 둘의 합). BurstR = 1(무작위 손실) —
  XR 버스트/갭 지표는 보고 필드로만 싣고 BurstR 에는 아직 넣지 않는다(§9).
- **Id = 0.024·Ta + 0.11·(Ta − 177.3)·H(Ta − 177.3)**(G.107 Id 의 한 구간 근사). 단방향 입→귀 지연 **Ta = RTD/2 + ESD(자기) + ESD(상대 추정)**,
  ESD(상대 추정) = 코덱 프레임·lookahead + 장치 30 ms. RTD 를 모르면(RTCP 없음) 망 지연 0. MOS-LQ 는 Ta = 0 으로 계산한다.
- **광대역**(AMR-WB·G.722)은 G.107.1 — Rmax = 129, 코덱 상수는 Ie,wb. MOS 는 R_WB / 1.29 를 협대역 척도로 옮긴 뒤 G.107 Annex B 식으로 구한다.
  그래야 협대역 호와 한 척도로 비교할 수 있다. R 은 원 척도 그대로 보존한다(`rLq`·`rCq`).
- MOS = 1 + 0.035R + R(R − 60)(100 − R) · 7·10⁻⁶ (0 < R < 100), R ≤ 0 → 1, R ≥ 100 → 4.5.

| 코덱(rtpmap) | 대역 | Ie(,wb) | Bpl | 프레임·lookahead | 출처 |
|---|---|---|---|---|---|
| PCMU·PCMA | 협대역 | 0 | 25.1 | 20 ms | G.113 App. I(G.711 + PLC) |
| G729 | 협대역 | 11 | 19 | 25 ms | G.113 App. I |
| AMR | 협대역 | 5 | 10 | 25 ms | G.113 App. I(12.2 kbit/s) |
| AMR-WB | 광대역 | 13 | 13 | 25 ms | 대표 모드 12.65 kbit/s — G.113 광대역 표 대조 과제(§9) |
| G722 | 광대역 | 13 | 10 | 20 ms | 64 kbit/s — 같은 대조 과제 |
| 그 밖 | 협대역 | 0 | 25.1 | 20 ms | G.711 값 |

### 3.3 API

```cpp
struct QualityDirection {                     // types.h — rx = 내가 받은 스트림, remote = 상대가 받은 내 스트림(상대 RR + XR)
    bool valid; unsigned packets, lost, discarded;
    double lossPct, discardPct, jitterMs, jitterMaxMs;       // %, ms — 없으면 -1
    double burstDensityPct, gapDensityPct; int burstMs, gapMs; // XR(RFC 3611 §4.7)
    int signalDbm, noiseDbm;                                 // XR 레벨, 127 = 없음
};
struct CallQuality {
    bool valid; std::string codec; unsigned clockRate; bool wideband;
    QualityDirection rx, remote;
    double rtdMs, esdMs, oneWayMs;            // -1 = 없음
    double rLq, rCq, mosLq, mosCq;            // -1 = 계산 불가(받은 패킷 없음)
    int64_t startEpochMs, durationMs;
};
class Engine { … CallQuality callQuality(int callId) const; … };
```

- 원천: rx = RTCP rx stat(손실·폐기·지터) + 자기 XR 계산값, remote = RTCP tx stat(상대 RR 의 누적 손실·지터, `updateCount` > 0 일 때) + 상대 XR
  (폐기율·버스트/갭), RTD = RTCP `rtt` 평균(없으면 XR DLRR 의 `rtt`).
- 호 종료 뒤에도 마지막 값을 보존한다(`onStreamDestroyed` 에서 오디오 스트림을 측정). 전달·재협상으로 스트림이 재생성되면 패킷·손실·폐기를
  **누적 합산**하고(`quality::merge`) 비율·MOS 를 다시 계산한다. 받은 패킷이 없으면 MOS 는 -1.
- 바인딩: C API `cimsue_engine_call_quality`(`cimsue_call_quality_t`·`cimsue_quality_direction_t`, 구조체 id `CIMSUE_STRUCT_CALL_QUALITY`·
  `CIMSUE_STRUCT_QUALITY_DIRECTION`) · .NET `Call.Quality`(`CallQuality`·`QualityDirection` record) · Android `CimsUe.callQuality(callId)`·`Call.quality()`.
- `cimsue-cli` 는 결과 JSON·`drive` 의 `call(disconnected)`·`stats` 이벤트에 `codec`·`discard`·`loss_pct`·`discard_pct`·`jitter_max_ms`·
  `remote_loss_pct`·`remote_jitter_ms`·`rtd_ms`·`esd_ms`·`one_way_ms`·`r_lq`·`r_cq`·`mos_lq`·`mos_cq` 를 싣는다(값 없음 = -1).

### 3.4 엔진 빌드

세 플랫폼 공통 `sdk/engine/config_site/common.h` 에 `PJMEDIA_HAS_RTCP_XR 1`·`PJMEDIA_STREAM_ENABLE_XR 1` 을 켠다. XR 통계는 pjsua API 에 없어
CIMS 패치 `pjsua_call_get_stream_stat_xr`(`pjsip/src/pjsua-lib/pjsua_call.c`)로 읽는다. RTCP-XR 은
RTCP compound 에 실려 나가므로 SRTP 호에서는 SRTCP 로 보호된다. CMP 는 relay leg 마다 SRTCP 를 풀고 다시 보호하므로 XR 도 그대로 건너간다.

## 4. 시험 모드 (앱)

### 4.1 진입·설정·해제

- **진입** — 앱 자체의 [정보] 화면(Android 3종: 설정 › 정보, 관제 태블릿: 더보기 › 설정 › 정보, Windows 관제 앱: 정보 창)에서 **버전 줄을 7회 연속**
  누르면 "시험 모드" 메뉴가 나타난다(남은 횟수를 3회부터 토스트로 알림). 안드로이드 시스템 설정의 개발자 옵션과는 무관하다.
- **설정** — 시험 모드 메뉴:

  | 항목 | 내용 |
  |---|---|
  | 시험 모드 | 켜기/끄기. 켜면 오버레이·요약·이력이 보이고 계측기 연결을 쓸 수 있다 |
  | 계측기 주소 | 워커 호스트(IP 또는 이름)·포트(기본 7120) |
  | 연결 키 | 선택. 워커 `Device.PairKey` 와 같아야 한다(비우면 워커가 검사하지 않을 때만 붙는다) |
  | 인증서 확인 | 켜면 워커 인증서를 검증(앵커 = 앱 동봉 루트 또는 사용자가 넣은 PEM), 끄면 최초 지문 고정(TOFU) |
  | 연결 | 켜기/끄기 + 상태(연결 중 / 연결됨 워커 이름 / 끊김 사유) |
  | 서비스 | 링크로 내보낼 회선(VoLTE·VoIP·PTT 중 앱이 가진 것) |

  설정은 앱 설치 단위 로컬 저장, 재기동 뒤에도 유지된다. 연결이 켜져 있으면 끊겨도 백오프(1·2·4 … 최대 30 초)로 다시 붙는다.
- **표시** — 시험 모드가 켜진 동안 상단 바(태블릿은 하단 내비 위 띠)와 Android 상시 알림에 "시험 모드" 배지, 계측기에 연결된 동안은
  "계측기 연결됨 · <워커>" 를 둔다. 계측기가 구동한 호는 호 화면에 "계측기" 표지를 단다.
- **해제** — 시험 모드를 끄면 링크를 닫고 오버레이를 감춘다. 메뉴는 7회 탭을 다시 할 때까지 숨긴다.

### 4.2 화면

| 화면 | 내용 |
|---|---|
| 통화 중 오버레이 | 호 화면 위 반투명 한 줄: 코덱 · 손실 % · 지터 ms · RTD ms · MOS-CQ. 누르면 방향별 상세(수신/원격)로 펼친다. 1 초 갱신 |
| 호 종료 요약 | 호가 끝나면 카드 한 장(§3.1 지표 전부 + 판정 색: MOS-CQ ≥ 4.0 양호 · ≥ 3.6 보통 · 미만 나쁨) |
| 이력 | 최근 50 호의 요약. 행을 누르면 상세 |
| 기준 음원 | 켜면 다음 호부터 마이크 대신 동봉 음원(P.59 활동률 50 % 두 화자 대화, 16 kHz, P.56 -26 dBov — 계측기 샘플 마스터와 같은 원본)을 송출. 계측기가 `media` 명령으로도 바꾼다 |
| 내보내기 | 이력을 JSON 파일로 — Android 공유 시트, Windows 저장 대화 상자 |

측정·오버레이는 계측기 연결 없이도 동작한다(단독 현장 점검).

### 4.3 앱 공통화

Android 는 `android/core` 에 시험 모드 진입 제스처·설정·화면 조각·링크 서비스를 한 번 두고 VoLTE·PTT·관제 태블릿이 같이 쓴다. 링크는 앱의
Foreground Service 가 세션과 함께 든다(화면이 꺼져도 유지). Windows 관제 앱은 .NET 파사드로 같은 API 를 쓴다. 기준 음원 송출은 코어가 한다
(pjsua2 `AudioMediaPlayer` → 호 포트, 반복 재생).

## 5. 계측 링크

### 5.1 전송

- TCP + **TLS 1.2 이상**, 워커가 서버(`Device.Ip:Port`, 기본 `0.0.0.0:7120` — 7110 은 컨트롤러 관측 수신 `Tester.WorkerStreamPort`), 단말이 클라이언트(먼저 연결).
- 워커 인증서 = `Device.CertFile/KeyFile`(비면 기동 때 자체 서명을 만들어 `DataDir` 에 둔다). 단말은 두 방식 중 하나로 확인한다 —
  **검증**(`verifyServer` + 앵커 PEM — 체인과 접속 주소의 IP/DNS SAN) 또는 **최초 지문 고정**(TOFU — 첫 연결의 leaf SHA-256 을 `pinFile` 에
  적고 이후 다르면 `Refused(pin_mismatch)`). 둘 다 끄면 암호화만 한다.
- 줄 단위(UTF-8, `\n`), 한 줄 최대 64 KiB. 15 초 동안 받은 것이 없으면 단말이 `{"event":"ping"}` 을 보내고(워커 응답 = 줄 `pong`), 워커가
  보낸 줄 `ping` 에는 단말이 `{"event":"pong"}` 으로 답한다. 45 초 동안 받은 것이 없으면 끊는다.
- 끊기면 단말은 이 링크가 만들거나 받은 호만 끊고 1·2·4 … 초(상한 `reconnectMaxSec`, 기본 30) 백오프로 다시 붙는다. 60 초 넘게 붙어 있었으면
  백오프를 처음부터. 연결 키 거절·지문 불일치(`Refused`)는 다시 붙지 않는다.

### 5.2 프로토콜

`drive` 구동 모드([ue_sdk.md §4.7](ue_sdk.md))의 명령·이벤트를 **그대로** 쓴다 — 정의는 공개 헤더 `cimsue/drive.h` 한 곳. 링크가 더하는 것:

| 방향 | 줄 | 뜻 |
|---|---|---|
| 단말 → 워커 | `hello{proto:1, device_id, app, version, platform, model, pair_key, accounts:[{service, aor, msisdn, registered}], test_mode:true, engine}` | 연결 직후 한 번. `device_id` = 설치 고유 id(`+sip.instance` 와 같은 원천) |
| 워커 → 단말 | `welcome{worker, accepted:true}` / `bye{reason}` | 10 초 안에 와야 한다. 연결 키가 틀리면 `bye{reason:"pair_key"}` → 단말 `Refused` |
| 워커 → 단말 | `use <service>` | 이후 명령이 쓸 회선(기본 = hello 의 첫 회선). 없으면 `no_account` |
| 워커 → 단말 | `media mic\|sample [<wav>]` | 송출 원천 — 마이크 / WAV 반복 재생(`Engine::setTxSource`, 진행 중 호에도 즉시). `sample` 기본 = 앱 동봉 기준 음원 |
| 워커 → 단말 | `quality <call>` | 즉시 `quality{kind:"snapshot"}` |
| 단말 → 워커 | `quality{call, kind:"callTerm"\|"snapshot", 품질}` | `callTerm` = 호 종료 직후(`call disconnected` 바로 뒤, 최종 값). 진행 중 품질은 1 초 `stats` 에 같은 필드로 실린다 |
| 단말 → 워커 | `reg{service, state, …}` | 등록 변화(앱이 등록을 소유하므로 알림만) |
| 워커 → 단말 | `quit` | 워커가 단말을 놓는다 — 단말은 링크를 유지한 채 구동 호를 정리하고 새 세션으로 대기 |

- **등록은 앱 소유** — 링크에서 `register`/`unregister` 는 거절한다(`result{ok:false, reason:"app_owned"}`). 워커는 `hello.accounts[].registered`
  와 `reg` 이벤트로 상태를 본다.
- **자동 응답** — 링크가 연결된 동안 앱은 착신을 사람에게 알리되, 계측기가 `answer <call>` 을 보내면 그 호를 받는다. 계측기가 모르는 착신
  (예: 실제 사람이 건 호)은 계측기 단계와 맞지 않으므로 워커가 응답하지 않는다 — 사람이 받는다.
- `request` 이벤트는 이 세션이 낸 요청(`affiliate`)의 결과만 싣는다 — 앱이 스스로 낸 PUBLISH 등은 링크로 나가지 않는다.

### 5.3 코어 구성

| 부분 | 위치 | 역할 |
|---|---|---|
| `DriveSession` | 공개 `cimsue/drive.h` · `src/drive/drive_session.cpp` | 명령 해석·이벤트 직렬화. Engine **관찰자**(`Engine::addObserver`)로 이벤트를 받는다 — 앱의 주 리스너와 나란히. 줄 출력은 `LineSink` |
| `DeviceLink` | 공개 `cimsue/drive.h` · `src/drive/device_link.cpp` | 링크 스레드 하나가 연결·hello·읽기·쓰기 큐·ping·재접속을 한다(OpenSSL SSL 객체는 동시 읽기·쓰기에 안전하지 않아 다른 스레드의 줄은 큐로) |
| TCP/TLS 스트림 | `src/net/tls_stream.{h,cpp}` | 소켓·핸드셰이크·검증(IP/DNS SAN)·leaf 지문 — HTTPS 전송(`http/https_client`)과 공용 |
| JSON 파서 | `src/util/json_lite.h` | CSC 클라이언트와 공용(welcome/bye 해석) |
| 송출 원천 | `Engine::setTxSource(wav)` | `wireMedia` 에서 마이크 대신 `AudioMediaPlayer`(반복)를 호로 결선. 음소거·floor 게이트는 그대로 |

- `cimsue-cli drive` 는 `DriveSession` + stdout(`ready` 이벤트 포함, stop 때 모든 호 정리), `cimsue-cli link HOST[:PORT] [--pair-key K] [--link-ca PEM |
  --link-pin FILE] [--sample-file WAV] [--service S] [--duration S]` 는 등록 뒤 `DeviceLink`(stdout = `link{state,detail}` 줄) — 앱 시험 모드와 같은 경로라
  앱 없이 워커 쪽(Q3)을 끝까지 검증한다.
- 앱 바인딩(C API·.NET·SWIG 의 `DeviceLink`)은 앱 시험 모드(Q4)와 함께 낸다.

## 6. 계측기 — `device` 풀

[test_instrument.md](test_instrument.md) 의 풀 종류 `device`. 시험 모드 단말을 **번호로 골라** `real-ue` 와 같은 단계로 구동한다.

### 6.1 워커

- **`DeviceHub`**(`tester/worker/src/DeviceHub.{h,cpp}`) — 설정 `Device.Ip`(기본 `0.0.0.0`)·`Device.Port`(기본 **7120**, 0 = 끔)·`CertFile`/`KeyFile`
  (비면 `<모듈>/config/device.{crt,key}` — 없으면 기동 때 자체 서명 EC P-256 을 만들어 두고 재기동에도 같은 지문)·`PairKey`·`MaxDevices`(32).
  포트를 못 열어도 워커는 뜨고 health 가 알린다. 연결마다 입출력 스레드 하나(읽기·쓰기 큐·15 s ping·45 s 끊김), 핸드셰이크·hello 는 연결마다
  스레드라 느린 단말이 다른 단말의 수락을 막지 않는다. hello 검사 = `proto`·`pair_key`(틀리면 `bye{pair_key}`)·상한(`bye{full}`). **같은
  `device_id` 가 다시 붙으면 옛 연결을 닫고** 그 연결을 쓰던 풀에는 링크 끊김을 알린다.
- **링크 추상 `DriveLink`**(`RealUe.h`) — `request`·`send`·`alive`. 구현 = `RealUeProcess`(cimsue-cli 파이프, `real-ue`) · `DeviceConn`(TLS 소켓,
  `device`). 워커의 실스택 분기(`K_REAL` Endpoint·`onRealEvent`·ep* 헬퍼)는 이 인터페이스만 본다 — `device` 는 `K_REAL` + `device` 표지.
- **풀 생성** `POST /pools {kind: device, service, media, identities[{user, domain, ptt_group}]}` — 번호·서비스로 연결을 찾아 bind(없으면 400
  `device_not_connected`, 다른 풀이 쓰면 `device_busy`, 수신점 없음 `device_link_off`), 링크에 `use <service>`·`media <sample|mic>`. 풀을 내리면
  bind 를 풀고 `quit`(단말은 링크를 유지한 채 이 풀이 구동한 호를 정리하고 새 세션으로 대기).
- **등록 의미** — 등록은 앱 소유라 `register` 단계는 **앱의 등록 상태 확인**(hello·`reg` 이벤트로 추적 — 미등록이면 480 `device not registered
  (app)`, `rrd_ms` 는 재지 않는다), `deregister` 는 아무것도 하지 않는다.
- **API** — `GET /devices`(연결 목록: `device_id`·앱·버전·모델·주소·연결 시각·`pool`·`accounts[{service, aor, msisdn, registered}]` + 수신점 지문) ·
  health `devices{listening, port, connected, max, pair_key, fingerprint}`.

### 6.2 컨트롤러

- **모델** `DevicePool{worker, group?, access, listener?, transport, service, identities[{user, domain?, ptt_group?}], media: sample|mic}` — `access` 는
  단말 앱이 등록한 접속점(도메인·번호계획 파생용 — 계측기는 단말 등록을 바꾸지 않는다). 신원은 다른 풀과 겹치면 컴파일 오류.
- **단계 게이트** — `real-ue` 와 같은 `REAL_UE_STEPS`(콘솔 행위자 칩·`vocab.steps[*].real`), 실스택 역할이 든 시나리오의 호는 `media.rtp: auto` 만.
- **배치** — 풀의 `worker` 는 토폴로지가 정한다. 계획 미리보기가 그 워커의 `GET /devices` 로 번호·서비스 연결을 확인한다 — 없으면 오류(다른 워커에
  붙어 있으면 그 이름, 아니면 단말에 넣을 계측기 주소 `<워커 호스트>:<Device.Port>`), 앱 미등록·다른 풀 사용은 경고, 용량 행 `devices{need, connected,
  port, fingerprint}`.
- **API** `GET /api/v1/tester/devices[?topology=]` — 토폴로지 워커들의 `GET /devices` 합산(편집기 '연결된 단말').
- **지표** — `device_*` 시리즈(`device_legs`·`device_srd_ms`·`device_rtp_tx/rx/lost`·`device_rtp_loss_pct`·`device_jitter_ms`·`device_rtd_ms`·
  `device_mos`(단말이 잰 MOS-CQ — min 이 판정)·`device_rtp_silent_legs`·`device_rtp_nosample`·`device_link_lost`) + 전체 지표 동시 기록(`real_*` 와 같은
  규칙). 링크가 끊기면 진행 중 인스턴스는 실패. 공통 `rtd_ms`(가상 단말·피어가 상대 RR 로 잰 RTT).
- **보고서 용어** — ETSI TS 102 250-2 / ITU-T E.804 이름을 지표 라벨에 붙인다: `device_srd_ms` = Telephony Setup Time, `device_mos` = Speech Quality on
  Call Basis(E-model 추정임을 표기), `ser_pct` = 확립 성공률(Telephony Service Accessibility 에 대응).

### 6.3 콘솔

토폴로지 편집기 팔레트 **실기기 풀**(워커 위에) — 카드 = 접속점·회선·번호 수·송출. 속성 = 접속점(노드·수신점·transport)·service·송출 원천·번호 목록
(PTT 면 번호마다 `ptt_group`)·**연결된 단말**(워커별 수신점 포트·지문·단말(앱·모델·주소·사용 중인 풀)·회선별 등록 상태, [추가] 로 번호를 넣고 그
워커로 풀을 옮긴다, 선택한 번호가 이 워커에 없으면 단말에 넣을 계측기 주소를 보여 준다). 시나리오 편집기는 `device` 역할을 `real-ue` 와 같은 행위자
게이트로 막는다. 결과 요약에 `device_*` 행.

### 6.4 가상 단말 RTCP

libcsim `CRtpThread` 가 RTCP compound(SR — 보낸 RTP 가 없으면 RR — + 수신 보고 블록 + SDES CNAME, RFC 3550 §6.1·§6.4)를 5 초마다 RTP 포트+1 로
보내고, 상대 RR 의 LSR/DLSR 로 RTT(§6.4.1)를 잰다(`m_llRtcpRttUs` — 워커 `rtd_ms` 표본·E-model 망 지연 RTT/2). SRTP 세션이면 같은 libsrtp 컨텍스트로
SRTCP 보호/해제(RFC 3711 §3.4 — 수신 보고 블록도 해제해서 읽는다). PTT 는 보내지 않는다(그룹 미디어 RTCP 미사용). 피어 엔진(CsimPeer)·미디어 전담
워커(에이전트 `rtt_us`)도 같은 값을 낸다.

### 6.5 동봉 시나리오

`VOLTE-CALL-DEVICE-MO`(실기기 발신 → 가상 착신) · `VOLTE-CALL-DEVICE-MT`(가상 발신 → 실기기 응답) · `VOLTE-CALL-DEVICE-E2E`(실기기 ↔ 실기기 — 같은
`device` 풀의 번호 둘, 두 역할은 `disjoint_from`).

## 7. 검증

| 항목 | 내용 |
|---|---|
| `S1-UE-UNIT`(코어) | E-model 기준값(기본값 R 93.2 → MOS 4.41, 손실·지연 벡터)·광대역 척도 · `CallQuality` 계산·누적 · `DriveSession` 명령/결과 줄·`app_owned`·`use`·`media`·`quality` · `DeviceLink` 설정 거절 |
| `S1-UNIT-TESTER` | `tester_emodel_test`(공용 헤더) · `csim_rtp_media_test`(RTCP SR/RR·RTT 루프백·PTT 끔) · `tester_device_hub_test`(TLS hello/welcome·연결 키 거절·명령 result·reg 추적·bind 이벤트·재접속 교체·지문 유지) · 컨트롤러 `test_device_pool`(PoolCreate·게이트·계획 미리보기의 연결 확인) |
| 계측기 실측 | 실제 워커 + `cimsue-cli link`(앱과 같은 TLS 링크) + 컨트롤러 run 드라이버, .48 CSP 경유: `VOLTE-CALL-DEVICE-MO`·`-MT`·`-E2E` pass — SRD ≈ 1.08 s, 손실 0, RTD(RTCP 종단) 0.6~1.2 ms, MOS-CQ 4.27. 다음은 실기기 앱(Q4) |

## 8. 이행

| WP | 내용 | 걸리는 곳 |
|---|---|---|
| Q1 | 코어 측정 — RTCP-XR 빌드 켬(3 플랫폼) · `quality/` · 공용 E-model · `callQuality` · `cimsue-cli` stats 확장 · 바인딩 | `ext/pjproject` config_site · `sdk/core` · `sdk/android` · `sdk/windows` · `tester/worker`(EModel 공용화) |
| Q2 | 코어 `drive/` — `DriveSession`(관찰자)·`LineSink` 로 drive 루프 이전 · `DeviceLink` TLS 링크(TOFU·ping·재접속) · `hello`/`use`/`media`/`quality` · `cimsue-cli link` · `Engine::setTxSource` 기준 음원 | `sdk/core` |
| Q3 | 계측기 — `DeviceHub`·`DriveLink` · 컨트롤러 `DevicePool`·`GET /devices`·계획 미리보기 연결 확인·게이트·`device_*` · libcsim RTCP SR/RR·RTT·SRTCP · 동봉 시나리오 3종 · 콘솔(실기기 풀·연결된 단말) | `tester/worker` · `cspsim` · `ems/tester` |
| Q4 | 앱 시험 모드 — Android core 공통(진입·설정·링크 서비스·오버레이·요약·이력·내보내기) + 앱 3종 · Windows 관제 앱 | `android/core` · 앱 · `windows/dispatch-desktop` |

Q1·Q2·Q3 는 구현 반영 — 계측기 `real-ue` 는 cli 가 낸 `mos_cq`(RTT 실측·지터버퍼 폐기 포함)를 그대로 쓰고 `rtd_ms`·`real_rtd_ms` 를 기록한다. 링크는 워커 대역(파이썬 TLS 서버)으로 hello·welcome·`app_owned`·`media sample`·.48 경유 발신·ping·`callTerm`·quit·재접속·연결 키 거절·지문 불일치·CA 검증까지 확인했다.

## 9. 미해결 / 향후

- **E-model 입력 보강** — XR 버스트/갭에서 BurstR(G.107 §7.5, 2 상태 마르코프) 추정 · 광대역 코덱 상수(Ie,wb·Bpl)를 G.113 광대역 표와 대조하고
  AMR-WB 모드(fmtp `mode-set`·실제 수신 모드)별 값으로 · 신호·잡음 레벨(XR `signal_lvl`/`noise_lvl`)은 pjmedia 가 채우지 않아 127 — 레벨 계측
  (ue_audio_level.md 의 P.56 측정기)을 XR 에 넣을지 결정.

- **대상 쪽 구간 관측** — 실단말 두 대의 보고만으로는 상향·하향 어느 구간이 나빴는지 가르기 어렵다. 계측기의 대상 관측(대상 OAM·SSH, test_instrument.md
  대상 관측)으로 CMP relay leg 별 수신 카운터를 읽는 경로를 둘지 결정한다 — 보고·제어 경로가 아니라 **관측 증거**로만.
- **PTT 입→귀 지연**(TS 22.179 KPI 3) — 발언자 첫 RTP 송출 시각·수신자 재생 시각을 단말이 NTP 시각으로 `quality` 에 싣고 워커가 맞춘다.
  단말 시각 동기 품질이 전제라 별건.
- **실기기 PTT 그룹 세션** — 그룹 세션은 멤버 역할이 같은 풀이어야 해서 실기기 하나와 가상 멤버를 한 그룹에 섞지 못한다. 실기기 talker + 가상
  listener 시나리오(`PTT-GROUP-DEVICE-FLOOR`)는 그룹 세션의 풀 규칙을 넓힐 때.
- **링크 재접속 뒤 재바인딩** — run 중 단말이 다시 붙으면 새 연결은 풀에 묶이지 않는다(그 run 에서 그 단말은 끊김으로 끝난다). 같은 `device_id` 의
  재접속을 진행 중 풀에 다시 묶을지 결정.
- **RFC 6849 미디어 루프백** — 한쪽 단말만으로 왕복 품질을 재는 시험. 객관 음질 비교 없이도 왕복 손실·지연 확인에 쓸 수 있다.
- **영상 품질** — RTCP-XR 에 영상 지표가 없어 손실·지터·프레임률만 별도 필드로.
- **운영 상시 품질 관측** — 시험이 아닌 운영 중 전 단말의 품질 수집(RFC 6035 `vq-rtcpxr` 또는 TS 26.114 §16 MTSI QoE)은 계측과 목적이 달라 이 문서
  범위 밖이다. 필요해지면 측정 코어(`quality/`)를 그대로 쓰고 보고 경로만 따로 설계한다.
