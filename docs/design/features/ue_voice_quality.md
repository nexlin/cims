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
| 보고·제어 경로 | **단말 → 계측기 워커 직접 TLS 링크**(`Device.Listen`). CSP·CSC·CMP 를 거치지 않는다 | 계측 경로를 시험 대상에서 분리 |
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
│ CSP · CMP · CSC   │  (계측 경로 없음)  │ DeviceHub(Device.Listen)            │─────►│ device 풀   │
└───────────────────┘                  │  └ 연결 단말 = K_DEVICE Endpoint      │ 스트림 │ 지표·판정    │
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
| 단말 지연 ESD | XR `end_sys_delay` | 지터버퍼 + 코덱 프레임·lookahead + 패킷화 |
| 신호·잡음 레벨 | XR `signal_lvl`·`noise_lvl`(dBm0) | 기준 음원 시험(§4.2) 때 도달 레벨 확인 — 목표 -26 dBov(P.56) |
| 무음 leg | 수신 0 이 일정 시간 지속 | 단방향 무음 판정. DTX(AMR-WB SID)와 floor 게이트는 무음으로 세지 않는다 |
| R · MOS-LQ · MOS-CQ | §3.2 E-model | LQ = 지연 손상(Id) 제외 청취 품질, CQ = 지연 포함 대화 품질 |
| 코덱 | 협상 결과(PT·rtpmap·fmtp) | E-model 코덱 상수 선택 |
| PTT floor | floor 참가자 상태머신 | 요청→허가 시간(TS 22.179 KPI 1 MCPTT access time). 그룹 미디어는 RTCP 가 없어 RTD·원격 지표 없음 |

### 3.2 E-model

단일 정의는 코어 `sdk/core/src/quality/emodel.h`(헤더 전용·의존 없음)에 두고, 계측기 워커(`tester/worker/src/EModel.h`)도 이 헤더를 쓰도록
옮긴다 — floor 정의 단일화([ue_sdk.md §4.6](ue_sdk.md))와 같은 방식이다.

- **R = Ro − Is − Id − Ie,eff + A**(G.107 §7). 기본값 Ro − Is = 93.2, A = 0.
- **Ie,eff = Ie + (95 − Ie) · Ppl / (Ppl / BurstR + Bpl)**. Ppl = 망 손실 + 지터버퍼 폐기(사용자가 겪는 손실은 둘의 합).
  BurstR 은 XR 버스트/갭 지표에서 2 상태 마르코프 근사로 구하고, 없으면 1(무작위 손실)로 둔다.
- **Id** 는 단방향 입→귀 지연 **Ta ≈ RTD/2 + ESD(자기) + ESD(상대)** 로 계산한다. 상대 XR 이 없으면 ESD(상대) = 코덱 프레임 + lookahead 고정값.
  MOS-LQ 는 Id = 0 으로 계산한다.
- **광대역**(AMR-WB·G.722)은 G.107.1 — Ro 는 129 척도, 코덱 상수 Ie,wb·Bpl 은 G.113 광대역 표에서 옮긴다. MOS 는 R_WB / 1.29 를
  협대역 척도로 옮긴 뒤 G.107 Annex B 식으로 구한다. 그래야 협대역 호와 한 척도로 비교할 수 있다. R 원값은 따로 보존한다.
- MOS = 1 + 0.035R + R(R − 60)(100 − R) · 7·10⁻⁶ (0 < R < 100), R ≤ 0 → 1, R ≥ 100 → 4.5.

### 3.3 API

```cpp
struct CallQuality {                          // quality.h — 스냅샷(동기 조회)
    bool valid; std::string codec; int ptime;
    struct Dir { unsigned pkts, lost, discarded; double lossPct, discardPct, jitterMs, jitterMaxMs;
                 double burstDensity, gapDensity; double signalDbm, noiseDbm; bool silent; } rx, remote;  // remote = 상대 XR
    double rtdMs, esdMs;                      // -1 = 없음
    double rLq, rCq, mosLq, mosCq;            // -1 = 계산 불가
    int64_t startEpochMs, stopEpochMs;
};
class Engine { … CallQuality callQuality(int callId) const; … };
```

- 호 종료 뒤에도 마지막 통계를 보존한다(현행 `onStreamDestroyed` 보존 규칙 승계). 전달·재협상으로 스트림이 재생성되면 **누적 합산**한다.
- Android `:cimsue`(Kotlin 파사드)·C API `cimsue_call_quality`·.NET `Call.Quality` 로 같은 구조체를 낸다.

### 3.4 엔진 빌드

`ext/pjproject` 의 플랫폼별 `config_site.h`(Linux·NDK·MSVC)에 `PJMEDIA_HAS_RTCP_XR 1`·`PJMEDIA_STREAM_ENABLE_XR 1` 을 켠다. RTCP-XR 은
RTCP compound 에 실려 나가므로 SRTP 호에서는 SRTCP 로 보호된다. CMP 는 relay leg 마다 SRTCP 를 풀고 다시 보호하므로 XR 도 그대로 건너간다.

## 4. 시험 모드 (앱)

### 4.1 진입·설정·해제

- **진입** — 앱 자체의 [정보] 화면(Android 3종: 설정 › 정보, 관제 태블릿: 더보기 › 설정 › 정보, Windows 관제 앱: 정보 창)에서 **버전 줄을 7회 연속**
  누르면 "시험 모드" 메뉴가 나타난다(남은 횟수를 3회부터 토스트로 알림). 안드로이드 시스템 설정의 개발자 옵션과는 무관하다.
- **설정** — 시험 모드 메뉴:

  | 항목 | 내용 |
  |---|---|
  | 시험 모드 | 켜기/끄기. 켜면 오버레이·요약·이력이 보이고 계측기 연결을 쓸 수 있다 |
  | 계측기 주소 | 워커 호스트(IP 또는 이름)·포트(기본 7110) |
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

- TCP + **TLS 1.2 이상**, 워커가 서버(`Device.Listen`, 기본 `0.0.0.0:7110`), 단말이 클라이언트.
- 워커 인증서 = `Device.CertFile/KeyFile`(비면 기동 때 자체 서명을 만들어 `DataDir` 에 둔다 — 단말은 TOFU 로 지문을 고정).
- 줄 단위(UTF-8, `\n`), 한 줄 최대 64 KiB. 양방향 15 초 무통신이면 `ping`/`pong`, 45 초면 끊는다.

### 5.2 프로토콜

`drive` 구동 모드([ue_sdk.md §4.7](ue_sdk.md))의 명령·이벤트를 **그대로** 쓴다. 링크가 더하는 것만 적는다.

| 방향 | 줄 | 뜻 |
|---|---|---|
| 단말 → 워커 | `hello{proto:1, device_id, app, version, platform, model, pair_key, accounts:[{service, aor, msisdn, registered}], test_mode:true}` | 연결 직후 한 번. `device_id` = 설치 고유 id(`+sip.instance` 와 같은 원천) |
| 워커 → 단말 | `welcome{worker, accepted}` / `bye{reason}` | 연결 키가 틀리면 `bye{pair_key}` 후 닫음 |
| 워커 → 단말 | `use <service>` | 이후 명령이 쓸 회선. 호 명령은 이 회선의 계정으로 |
| 워커 → 단말 | `media mic\|sample [<id>]` | 다음 호부터 송출 원천. `sample` 기본 = 동봉 기준 음원 |
| 워커 → 단말 | `quality <call>` | 즉시 `quality` 이벤트 |
| 단말 → 워커 | `quality{call, kind: interval\|callTerm, …CallQuality}` | `callTerm` = 호 종료 직후(최종 통계), `interval` = `stats` 와 같은 1 초 주기 |
| 단말 → 워커 | `reg{service, state, …}` | 앱이 가진 등록의 변화(앱이 등록을 소유하므로 알림만) |

- **등록은 앱 소유** — 링크에서 `register`/`unregister` 는 거절한다(`result{ok:false, reason:"app_owned"}`). 워커는 `hello.accounts[].registered`
  와 `reg` 이벤트로 상태를 본다.
- **자동 응답** — 링크가 연결된 동안 앱은 착신을 사람에게 알리되, 계측기가 `answer <call>` 을 보내면 그 호를 받는다. 계측기가 모르는 착신
  (예: 실제 사람이 건 호)은 계측기 단계와 맞지 않으므로 워커가 응답하지 않는다 — 사람이 받는다.
- `stats`·`call(disconnected)` 이벤트는 `rtd_ms`·`esd_ms`·`discard`·`mos_lq`·`mos_cq`·`r_lq` 필드를 더한다(§3.3 과 같은 이름). `cimsue-cli drive` 도 같다.

### 5.3 코어 `drive/`

`cimsue-cli` 의 drive 루프를 코어 `sdk/core/src/drive/` 의 **구동 세션**(`DriveSession` — 명령 해석·이벤트 직렬화)으로 옮기고, 줄 입출력만
인터페이스(`ILineIo`)로 둔다 — `cimsue-cli` = stdin/stdout, 앱 = TLS 소켓(`drive/tls_link` — 코어의 OpenSSL). 앱은 `DeviceLink.connect(host, port,
pairKey, verify)` 한 번과 상태 콜백만 쓴다. 프로토콜 정의가 한 곳이라 `real-ue` 와 `device` 가 어긋나지 않는다.

## 6. 계측기 — `device` 풀

[test_instrument.md](test_instrument.md) 에 풀 종류 `device` 를 더한다.

- **워커 `DeviceHub`** — `Device.Listen`·`CertFile`·`KeyFile`·`PairKey`·`MaxDevices`(기본 32). 연결된 단말 목록 = `hello` 요약 + 연결 시각·주소.
  `GET /devices`(워커 API) · health `devices{connected, max}`. 링크 추상은 `RealUeProcess` 와 같은 모양(`request`·`send`·이벤트 콜백 — 리더 스레드는 큐에만)
  으로 두고, 전송만 파이프 대신 소켓이다.
- **컨트롤러** — `GET /devices`(모든 워커 합산 — 편집기 '연결된 단말' 목록, 서비스·번호·앱·워커). 풀 정의 `RealUePool` 과 같은 층의
  `DevicePool{service, identities(번호 목록), media: sample|mic}`. 계획 미리보기가 연결 여부를 검산하고, 풀은 **그 단말이 붙어 있는 워커**에
  배치된다(없으면 400 `device_not_connected`). 신원은 다른 풀과 겹치면 안 된다.
- **Endpoint** — 워커 `Endpoint::Kind` 에 `K_DEVICE` 를 더한다. 이벤트를 가상 단말과 같은 Event 종류로 다시 풀어 처리하는 것은 `K_REAL` 과 같은
  경로(ep* 헬퍼의 실단말 분기를 공유)라 단계 실행기·지표·판정은 Endpoint 종류를 모른다.
- **단계 게이트** — `REAL_UE_STEPS` 에서 `register`/`deregister` 를 뺀 것(앱 소유). 콘솔 편집기 행위자 칩·`vocab.steps[*].device`.
- **지표** — `device_*` 시리즈(`device_legs`·`device_srd_ms`·`device_rtp_loss_pct`·`device_jitter_ms`·`device_rtd_ms`·`device_mos`(min)·
  `device_mos_lq`·`device_link_lost`) + 전체 지표 동시 기록(`real_*` 와 같은 규칙). 원천 = `quality{callTerm}`. 링크가 끊기면 진행 중 인스턴스는 실패
  (`device_link_lost` + event).
- **KPI 요약** — 결과 보고서에 ETSI TS 102 250-2 / ITU-T E.804 이름으로: Telephony Service Accessibility(= 확립 성공률)·Setup Time(= SRD)·
  Cut-off Call Ratio(확립 뒤 비정상 종료)·Speech Quality on Call Basis(= 호별 MOS-CQ, E-model 추정임을 표기).
- **가상 단말 RTCP** — libcsim `CRtpThread` 가 RTCP SR/RR 을 **보낸다**(RFC 3550 §6.4, 5 초 간격 — 지금은 받기만 함). 그래야 실단말이 가상 단말
  상대로도 RTD 를 재고, 워커도 RTT 를 실측해 E-model 망 지연을 0 대신 실측값으로 넣는다.
- **동봉 시나리오** — `VOLTE-CALL-DEVICE-MO`(실단말 발신 → 가상 착신) · `VOLTE-CALL-DEVICE-MT`(가상 발신 → 실단말 응답) ·
  `VOLTE-CALL-DEVICE-E2E`(실단말 ↔ 실단말 — 두 `device` 역할) · `PTT-GROUP-DEVICE-FLOOR`.

## 7. 검증

| 항목 | 내용 |
|---|---|
| `S1-UNIT-UE`(코어) | E-model 기준값(기본값 R 93.2 → MOS 4.41, 손실·지연 표 벡터) · 광대역 척도 변환 · `DriveSession` 명령/이벤트 왕복 · 링크 `hello`/연결 키/`app_owned` |
| `S1-UNIT` 계측기 | `tester_emodel_test` 가 공용 헤더로 · libcsim RTCP 송신 · `DeviceHub`(파이썬 스텁 단말이 TLS 로 붙어 hello·명령·이벤트·끊김) · 컨트롤러 `test_device_pool`(컴파일·배치·게이트) |
| 계측기 | `VOLTE-CALL-DEVICE-*` 를 링크 모드 `cimsue-cli`(`--link host:port` — 앱과 같은 TLS 링크)로 먼저, 이어 실단말로 |

## 8. 이행

| WP | 내용 | 걸리는 곳 |
|---|---|---|
| Q1 | 코어 측정 — RTCP-XR 빌드 켬(3 플랫폼) · `quality/` · 공용 E-model · `callQuality` · `cimsue-cli` stats 확장 · 바인딩 | `ext/pjproject` config_site · `sdk/core` · `sdk/android` · `sdk/windows` · `tester/worker`(EModel 공용화) |
| Q2 | 코어 `drive/` — `DriveSession`·`ILineIo` 로 drive 루프 이전 · TLS 링크 · `hello`/`use`/`media`/`quality` · `cimsue-cli --link` · 기준 음원 송출 | `sdk/core` |
| Q3 | 계측기 — `DeviceHub`·`K_DEVICE` · 컨트롤러 `DevicePool`·`GET /devices`·배치·게이트·`device_*`·KPI 요약 · libcsim RTCP 송신 · 동봉 시나리오 · 콘솔(연결된 단말 목록·풀 속성) | `tester/worker` · `cspsim` · `ems/tester` |
| Q4 | 앱 시험 모드 — Android core 공통(진입·설정·링크 서비스·오버레이·요약·이력·내보내기) + 앱 3종 · Windows 관제 앱 | `android/core` · 앱 · `windows/dispatch-desktop` |

Q1 만으로도 계측기 `real-ue` 가 RTT 실측 MOS 를 얻는다. Q2·Q3 뒤에는 앱 없이 링크 모드 `cimsue-cli` 로 `device` 풀을 끝까지 검증할 수 있다.

## 9. 미해결 / 향후

- **대상 쪽 구간 관측** — 실단말 두 대의 보고만으로는 상향·하향 어느 구간이 나빴는지 가르기 어렵다. 계측기의 대상 관측(대상 OAM·SSH, test_instrument.md
  대상 관측)으로 CMP relay leg 별 수신 카운터를 읽는 경로를 둘지 결정한다 — 보고·제어 경로가 아니라 **관측 증거**로만.
- **PTT 입→귀 지연**(TS 22.179 KPI 3) — 발언자 첫 RTP 송출 시각·수신자 재생 시각을 단말이 NTP 시각으로 `quality` 에 싣고 워커가 맞춘다.
  단말 시각 동기 품질이 전제라 별건.
- **RFC 6849 미디어 루프백** — 한쪽 단말만으로 왕복 품질을 재는 시험. 객관 음질 비교 없이도 왕복 손실·지연 확인에 쓸 수 있다.
- **영상 품질** — RTCP-XR 에 영상 지표가 없어 손실·지터·프레임률만 별도 필드로.
- **운영 상시 품질 관측** — 시험이 아닌 운영 중 전 단말의 품질 수집(RFC 6035 `vq-rtcpxr` 또는 TS 26.114 §16 MTSI QoE)은 계측과 목적이 달라 이 문서
  범위 밖이다. 필요해지면 측정 코어(`quality/`)를 그대로 쓰고 보고 경로만 따로 설계한다.
