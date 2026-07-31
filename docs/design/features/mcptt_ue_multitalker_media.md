# MCPTT 단말 동시 발언 미디어 평면 (U10)

단말이 **여러 화자의 음성을 동시에 재생**하려면 무엇을 해야 하는지, 선택지와 권고안을 정리한다.
[android_ue_client.md §5.4](android_ue_client.md#54-서버-규격-정합에-따른-단말-구현-요구사항-ts-24380) 의
**U10** 이 이 문서의 대상이며, 나머지 U 항목(floor 평면)은 모두 반영을 마쳤다.

작업에는 **pjproject 소스와 안드로이드 빌드 환경**이 필요하다 — 개발 서버(media01)에는 둘 다 없다
(pjsip `.so` 는 WSL2 에서 빌드해 투입하는 구조, `android/` 에는 네이티브 빌드가 없다).

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

하나의 `pjmedia_transport` 아래에 SSRC → 서브스트림 테이블을 두고, 새 SSRC 가 오면 서브스트림
(지터버퍼 + 디코더)을 만들어 conference bridge 슬롯에 붙인다. 믹싱은 브리지가 한다.

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

## 5. A 안 구현 설계

- **패치 위치**: `android/docs/scripts/m1_build_pjsip.sh` 에 다른 네이티브 수정과 같은 형식으로
  번호를 붙여 넣는다(그 스크립트가 pjproject 패치의 정본).
- **디먹스 지점**: `pjmedia_transport` 어댑터로 RX 를 가로채 SSRC 별로 서브스트림에 배달한다.
  기존 스트림(슬롯 0 = 종전 고정 SSRC)은 지금 경로 그대로 두어 **단일 화자 동작이 바뀌지 않게**
  한다 — 슬롯 1 이상이 나타날 때만 서브스트림을 만든다.
- **브리지 결선**: 서브스트림마다 conference bridge 포트를 만들어 스피커 슬롯에 connect.
  채널별 수신 음량(`adjustRxLevel`)은 서브스트림 전체에 같은 값을 적용한다.
- **서브스트림 정리**: RTP 타임아웃이 아니라 **floor 이벤트 기준**으로 건다 —
  `FloorEvent.TalkerLeft`(0x0F)에서 그 화자의 SSRC, `FloorEvent.Idle` 에서 전부.
  floor 평면이 이미 정확한 시점을 주므로 타임아웃 추정이 불필요하다.
- **상한**: 서버 정원과 맞춰 `MCPTT_MAX_TALKER_SLOTS`(8) 를 넘지 않게 한다.
- **SWIG**: 앱이 화자별 SSRC 를 네이티브로 내려보내야 하면 인터페이스 추가가 필요하다.
  가능하면 **SSRC 를 앱이 알려주지 않고 네이티브가 도착 SSRC 로 자동 생성**하는 형태로 두어
  SWIG 변경을 피한다(floor 이벤트는 정리 트리거로만 쓴다).

### 검증

- **서버측은 이미 검증됨** — `scripts/mcptt_floor_policy_probe.py` 116/116 에 dual/multi 정책,
  슬롯별 SSRC, 0x0F 통지가 포함돼 있다([VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md)
  「floor 정책 시험」).
- **단말측 실호 시험**(이번 작업으로 처음 가능해진다): CMP 에 `floor_policy: "multi"`,
  `max_talkers: 2` 그룹을 만들고 실기기 3대(A·B 발언, C 청취)로
  ① A 발언 중 B 승급 시 **A 의 마이크가 닫히지 않을 것**(floor 평면은 이미 반영 — `meSpeaking`)
  ② C 에서 A·B 음성이 **둘 다** 들릴 것 ③ A 가 Release 하면 B 만 계속 들릴 것(0x0F 경로)
  ④ 발언 스트립에 두 화자가 모두 표시될 것.

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

따라서 권고는 **단일 정의 테이블 + 실행 가능한 계약 시험**이다:

1. 하나의 테이블(YAML/JSON)에서 C++ 헤더 · Kotlin object · Python 상수를 생성 — JNI 없이 상수
   드리프트를 없앤다.
2. 알고리즘 드리프트는 **교차 검증**으로 잡는다 — CMP 인코더가 만든 패킷을 단말 디코더 규칙으로
   파싱하고, 역방향은 단말 빌더 출력을 `ParseFloorMessage` 로 파싱한다. 안드로이드 빌드가 없는
   환경에서도 돌아가므로 CI 에 고정할 수 있다.

JNI 공유 라이브러리로 가더라도 1·2 는 그대로 필요하다 — 먼저 하는 편이 낫다.
