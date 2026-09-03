# MCPTT 단말 동시 발언 미디어 평면 (U10)

단말이 **여러 화자의 음성을 동시에 재생**하려면 무엇을 해야 하는지, 선택지와 권고안을 정리한다.
[android_ue_client.md §5.4](android_ue_client.md#54-서버-규격-정합에-따른-단말-구현-요구사항-ts-24380) 의
**U10** 이 이 문서의 대상이며, 나머지 U 항목(floor 평면)은 모두 반영을 마쳤다.

구현은 엔진 소스 정본 `ext/pjproject` 의 `pjmedia/src/pjmedia/stream.c`·`stream_imp_common.c` 에 반영돼 있다(§5).
Android `.so` 는 WSL2 에서 `sdk/android/build-native.sh` 로 빌드해 투입한다([ue_sdk.md](ue_sdk.md) §3·§8) —
`android/` 자체에는 네이티브 빌드가 없다.

---

## 1. 현재 구조 — 두 평면이 갈려 있다

| 평면 | 소켓 주인 | 처리 |
|---|---|---|
| **Floor control** (`m=application`, RTCP-APP "MCPT") | **앱** — `ptt-client/floor/FloorClient.kt` 의 `DatagramChannel` | 자체 코덱(`FloorCodec`)으로 직접 인코드/디코드 |
| **음성·영상 RTP/RTCP** (`m=audio`/`m=video`) | pjsua2/pjmedia | `pjmedia_stream` + `transport_udp` → 지터버퍼 → 코덱 → conference bridge |

앱이 미디어 평면에서 하는 일은 패킷 처리가 아니라 **파이프라인 제어**다 — 코덱 협상(`CodecConfig`),
발언권에 따른 mic 슬롯 connect/disconnect(`setMicEnabled`), 캡처 게이트, 채널별 수신 음량
(`adjustRxLevel`), 출력 라우팅, RTP keepalive.

## 2. 문제 — 믹싱이 아니라 **디먹스**다

**믹싱은 이미 하고 있다.** pjmedia conference bridge 가 믹서이고, 멀티그룹 "전체듣기"에서
N개 그룹 통화의 오디오가 이미 브리지에서 섞여 나온다(`setCallListen`/`applyListenPolicy`).
브리지에 입력을 물릴 수만 있으면 믹싱은 공짜다.

막힌 곳은 그 앞이다. CMP 는 화자 슬롯마다 **다른 SSRC** 를 **같은 RTP 포트**로 보낸다
(`cmp/PMcpttGroup.cpp` `_egressSsrc`):

```
슬롯 0 : 0x10000000 + memberSsrc   (영상 0x20000000)   ← 단일 화자 시 종전과 동일한 고정 SSRC
슬롯 N : 0x40000000 + (N<<24) + memberSsrc (영상 0x50000000)
정원   : MCPTT_MAX_TALKER_SLOTS = 8
```

그런데 `pjmedia_stream` 은 **스트림당 SSRC 가 하나**다. 두 SSRC 가 번갈아 들어오면
`pjmedia_rtp_session_update2` 가 매번 SSRC 변경(`PJMEDIA_RTP_ESESSRESTART`)으로 판정해
지터버퍼를 리셋한다 — 두 화자 모두 끊긴다. 슬롯 1 이상의 스트림은 재생되지 않는다.

이 포트를 읽는 주체가 pjmedia 이므로 **앱 레이어에서는 그 패킷이 보이지 않는다.**

## 3. 이미 준비된 것 — floor 평면이 필요한 정보를 다 준다

미디어 작업에 필요한 입력은 floor 평면에서 이미 올라온다:

- **화자 집합**: `FloorClient.talkers: StateFlow<List<FloorTalker>>` — Floor Taken 이 전체 집합을,
  0x0F 가 한 명 이탈을, Idle 이 비움을 알린다(서버는 증분으로만 알린다).
- **화자별 RTP SSRC**: `FloorTalker.ssrc` / `PttController.Session.talkerSsrc`
  (Floor Taken 의 List of SSRCs(16), 단일 화자면 SSRC(14)).
- **정리 트리거**: `FloorEvent.TalkerLeft`(0x0F) · `FloorEvent.Idle`.

즉 "누가 말하는지 · 그 사람의 SSRC 가 무엇인지 · 언제 끝났는지"는 이미 안다.
남은 것은 **그 SSRC 별로 RTP 를 갈라 브리지에 물리는 일** 하나다.

## 4. 선택지

### A. pjmedia 에 SSRC 디먹스 추가 — **권고**

`pjmedia_stream` 안에 SSRC → 서브스트림 테이블을 두고, 첫 SSRC 는 기존 경로(primary)로 그대로
두고 두 번째 이후 SSRC 는 서브스트림(지터버퍼 + 디코더)으로 갈라 디코드한 뒤 `get_frame` 에서
PCM 을 합산(믹싱)한다. 스트림은 이미 믹싱된 PCM 한 포트를 conference bridge 에 내므로 브리지
포트·AudioTrack 은 1개로 유지된다. (구현 상세 = §5)

- 규격 그대로다 — TS 24.380 §6.2.4.3.4 NOTE: *"RTP media packets can be received from multiple
  sources … The MCPTT client can differentiate between the different sources using the **SSRC** …
  How the **media mixer in the MCPTT client** mixes the different RTP media stream sources is out
  of scope."* 하나의 스트림, SSRC 로 구분, 믹싱은 단말 몫.
- AEC·지터버퍼·브리지 믹싱·장치 라우팅·동시 캡처 중재 등 실기기에서 잡아 둔 것을 그대로 쓴다.
- 비용: pjproject 패치 1건(WSL2).

### B. 앱이 오디오 평면을 통째로 가져감

RTP 를 앱이 직접 수신 → SSRC 분리 → MediaCodec AMR-WB 디코더 N개 → `AudioTrack` N개
(AudioFlinger 가 자동 믹싱 — 별도 믹서 API 가 있는 게 아니라 트랙을 여러 개 열면 오디오 서버가 섞는다).

- 네이티브 패치가 필요 없다. floor 평면이 같은 방식으로 이미 동작 중이라 패턴도 검증돼 있다.
- **그러나 UDP 포트는 하나뿐이라 수신만 가져올 수 없다.** 수신을 앱이 가져오면 송신(AMR-WB 인코딩·
  RTP·RTCP·DTX)도 앱 몫이 되고, AEC·장치 라우팅·동시 캡처 중재를 다시 만들어야 한다.
  U10 하나를 위해 치르기엔 큰 비용이다.
- 지터버퍼를 아예 빼는 것은 안 된다(언더런으로 끊긴다). 다만 PTT 는 반이중이라 지연 예산이
  넉넉해(200~300ms 허용) **고정 100~200ms 버퍼로 충분**하고 적응형은 필요 없다.

### C. 화자 슬롯마다 별도 `m=audio`

pjsua 는 통화당 오디오 스트림을 여러 개 지원한다 — 각 스트림이 자기 지터버퍼·디코더를 갖고
브리지가 섞는다. **네이티브 코드 0줄.**

- 규격 이탈이다. 규격은 "하나의 스트림 + SSRC 구분"을 명시하므로 사설 편차가 되고, 타사 단말·서버
  상호운용을 포기하게 된다.
- CMP 도 슬롯별 포트 할당과 N-스트림 SDP 협상으로 바꿔야 한다(현재는 그룹당 RTP 포트 1개).

### 비교

| | 규격 정합 | 네이티브 작업 | 잃는 것 | CMP 변경 |
|---|---|---|---|---|
| **A** SSRC 디먹스 | ✅ | pjproject 패치 1건 | — | 없음 |
| **B** 앱이 오디오 평면 | ✅ | 없음 | AEC·라우팅·캡처 중재·송신 경로 재구현 | 없음 |
| **C** 다중 m=audio | ✗ 사설 편차 | 없음 | 상호운용 | 슬롯별 포트·SDP 협상 |

**권고 = A.** [CLAUDE.md](../../../CLAUDE.md) 의 설계 우선순위(①표준규격 준수 ②체계성 ③최소 보완)에
따른다. C 는 가장 싸지만 ①에 어긋나고, B 는 ①은 만족하나 U10 하나를 위해 검증된 미디어 스택을
버리는 대가가 크다.

## 5. 구현 — `pjmedia_stream` 내부 SSRC 디먹스

**패치**: 엔진 소스 정본 `ext/pjproject` 의 `pjmedia/src/pjmedia/stream.c` + `stream_imp_common.c`. 앱(Kotlin)·SWIG 는 바뀌지
않는다. 네이티브 상태는 `struct cims_mt_sub`(서브스트림 테이블)과 `cims_mt_*` 함수로 모인다.

- **디먹스 지점** — `on_rx_rtp`(RX/ioqueue 스레드). RTP 헤더 SSRC 를 보고:
  - **첫 SSRC = primary** 로 확정하고 기존 단일 화자 경로 그대로 흘려보낸다(지터버퍼·디코더·RTP
    세션 불변). CMP 의 슬롯0 고정 SSRC 가 여기에 해당한다.
  - 이후 **다른 SSRC = secondary 화자** → 그 화자의 서브스트림에 넣고 즉시 반환한다. primary RTP
    세션에는 넣지 않으므로 SSRC 변경(`PJMEDIA_RTP_ESESSRESTART`) 지터버퍼 리셋이 나지 않는다.
  - 상한 초과 SSRC 는 드롭한다.
- **서브스트림** — secondary 화자마다 **자체 지터버퍼 + 디코더**. 슬롯(지터버퍼·버퍼)은 스트림
  생성 시 `MCPTT_MAX_TALKER_SLOTS`(8) − primary = **7 개 선할당**(RX 경로에서 지터버퍼 생성/파괴
  회피)하고, 디코더(AMR-WB MediaCodec)는 **실제 화자가 나타날 때 지연 생성·재사용**한다 — 무거운
  코덱 인스턴스를 동시 화자 수만큼만 연다.
- **믹싱** — `get_frame`(오디오 스레드). primary PCM 을 만든 뒤 활성 서브스트림을 각각 디코드해
  PCM 을 **포화 합산**한다. 스트림은 **이미 믹싱된 PCM 한 포트**를 conference bridge 에 내므로
  브리지 포트·AudioTrack·`adjustRxLevel` 대상이 1개로 유지되고, 채널별 수신 음량은 합산 결과
  전체에 같은 값이 적용된다. 활성 서브스트림이 없으면 no-op — **단일 화자 동작이 바뀌지 않는다.**
- **정리(회수)** — **RTP 무활동** 기준. 어떤 secondary SSRC 로 `CIMS_MT_IDLE_FRAMES`(100 프레임,
  20ms 기준 ≈2s) 동안 재생할 프레임이 없으면 슬롯을 반납한다(코덱·지터버퍼는 재사용 위해 파괴하지
  않는다). floor 가 화자를 재통지하므로 다음 패킷에 즉시 재바인딩된다.
  - floor 이벤트(0x0F/Idle)를 정리 트리거로 쓰지 **않는다** — 미디어를 floor 로 게이팅하는 권위는
    **CMP(media distributor)에 있기 때문**이다. CMP 는 발언권 있는 화자만 egress 로 중계하므로
    (`PMcpttGroup.cpp` 중계 자격 판정) 화자가 발언권을 놓으면 그 화자의 egress 가 멈추고, 해제 후
    오류 RTP 도 단말에 도달하지 않는다. 단말의 RTP 무활동 회수는 **서버측 T1(End of RTP media)**
    이 무수신으로 발언 종료를 판정하는 것과 대칭이며, floor→네이티브 정리 훅(SWIG)·앱 연동이
    불필요하다. floor 평면은 발언 스트립 UI(`Session.talkerSsrc`)에만 쓴다.
- **상한** — `MCPTT_MAX_TALKER_SLOTS`(8) − primary = 7 secondary. 서버 정원과 일치한다.

### 검증

- **서버측 (완료)** — `scripts/mcptt_floor_policy_probe.py` 116/116 에 dual/multi 정책, 슬롯별
  SSRC, 0x0F 통지가 포함돼 있다([VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md)
  「floor 정책 시험」).
- **네이티브 정합 (완료)** — 호스트 `gcc -fsyntax-only`(pjmedia 헤더 대상) 로 `stream.c` 전체
  타입검사 통과. 실제 코덱 링크·동작 빌드는 WSL2(§0)에서 한다.
- **단말측 실호 (대기 — WSL2 빌드 + 실기기 3대)**: CMP 에 `floor_policy: "multi"`, `max_talkers: 2`
  그룹을 만들고 A·B 발언, C 청취로
  ① A 발언 중 B 승급 시 **A 의 마이크가 닫히지 않을 것**(floor 평면 기반영 — `meSpeaking`)
  ② C 에서 A·B 음성이 **둘 다** 들릴 것 ③ A 가 Release 하면 B 만 계속 들릴 것(A 서브스트림 무활동
  회수) ④ 발언 스트립에 두 화자가 모두 표시될 것.

## 6. 부록 — floor 코덱 공유 / 정의 단일화

"CMP 와 단말이 쓰는 RTP/RTCP/floor 를 라이브러리로 묶자"는 검토 결과. 공유 가능한 범위는 좁다.

| 덩어리 | 공유 | 이유 |
|---|---|---|
| **floor 메시지 코덱**(RTCP APP TLV) | 가능 | 순수 바이트 조작, 외부 의존 없음(`cmp/PFloorCodec.cpp` 는 `<cstring>/<string>/<vector>` 만) |
| **floor 상태머신** | 불가 | CMP=floor control **server**(중재·큐·타이머·멤버 정책, §6.3.4) / 단말=**participant**(버튼·마이크·톤, §6.2.4). 규격상 다른 역할 |
| **RTP/RTCP** | 실익 없음 | CMP=relay(SSRC/seq 재작성, 디코드·지터버퍼 없음) / 단말=endpoint. 겹치는 건 헤더 파싱뿐이고 단말은 그마저 pjmedia 가 한다 |

공유 후보는 사실상 **코덱 하나**(C++ 150줄 / Kotlin 250줄)다. 그런데 이를 공유하려면 앱 모듈에
NDK+JNI 를 새로 들여야 한다 — `android/` 에는 현재 네이티브 빌드가 없다(pjsip 은 WSL2 산출물 `.so`
투입). 붙인다면 pjsip 과 같은 패턴(WSL2 빌드 → `jniLibs`)이 되지만, 150줄짜리 순수 함수 대비
빌드 의존이 커진다.

**드리프트가 실제로 나는 곳은 알고리즘이 아니라 상수표다.** 4옥텟 정렬 규칙은 규격이 고정이라
변하지 않는 반면, opcode·field ID·cause·indicator 비트는 **네 곳**에 중복돼 있다:

- `cmp/PMcpttGroup.h` · `android/.../floor/FloorControl.kt`
- `scripts/mcptt_floor_policy_probe.py` · `tests/cmp_floor_codec_test.cpp`

따라서 **단일 정의 테이블 + 실행 가능한 계약 시험**으로 간다 ([ue_sdk.md](ue_sdk.md) §4.6):

1. 정본 테이블 `mcptt_floor_defs.yaml` → `scripts/gen_floor_defs.py` 가 코어 헤더(`sdk/core/src/floor/floor_defs.h`)를
   생성하고, `--check` 가 `cmp/PMcpttGroup.h`·`FloorControl.kt`·`mcptt_floor_policy_probe.py` 의 상수를 테이블과 대조한다
   — 네 곳의 상수 드리프트를 JNI 없이 없앤다.
2. 알고리즘 드리프트는 **교차 검증**으로 잡는다 — `cimsue_test` 의 `FloorXCheck` 가 CMP `BuildFloorMessage` 출력을
   코어 디코더로, 코어 빌더 출력을 `ParseFloorMessage` 로 파싱한다. 안드로이드 빌드가 없는 환경에서도 돌아간다.

단말 SDK 코어(`libcimsue`)의 floor 코덱·participant 는 Kotlin `FloorCodec`/`FloorClient` 를 C++ 로 이식한 것이고, Android
앱은 SDK 전환 후 이 코어를 쓴다(ue_sdk.md §5.3).
