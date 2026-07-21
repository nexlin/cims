# PTT 조인 크래시 + 발언 무음 — 미해결 이슈 (조사 문서)

> **성격**: 실기기 PTT 호시험 중 발견된 2대 미해결 이슈의 조사·근본원인 분석·수정 후보를 담는
> **진행 중 문서**다. 해소되면 해당 사실을 정본 설계 문서(특히
> [ue_nat_traversal.md](ue_nat_traversal.md), [ptt_flows.md](ptt_flows.md))로 흡수하고 이 파일은 제거한다.
> NAT traversal 설계 자체는 [ue_nat_traversal.md](ue_nat_traversal.md) 가 정본이며, 본 문서는 그 위에서
> 발생한 UE(pjsip) 측 결함을 다룬다.

## 요약

실기기 2대(W999=`+82500000001` chair / MF52=`+82500000002` participant, 그룹 `g001`)로 PTT 호시험 중
발견된 2대 이슈:

1. **조인 크래시** — 그룹콜 SDP offer/answer 처리 중 pjsua 네이티브 크래시(SIGABRT/SIGSEGV).
   크래시→앱 재기동→자동 REGISTER+PUBLISH→CSP 재-fanout→재크래시 무한 루프까지 갔던 문제.
   **세 겹의 결함**(m= 개수 assert / PT 불일치 NULL / 비 RTP 슬롯 text 채널화)으로 규명됐고
   전부 수정됨 — 상세는 아래 "크래시 구조" 절. **실기기 최종 확정 잔여.**
2. **PTT 발언 무음** — 발언권 GRANT 정상인데 양방향 음성 안 들림(마이크 캡처 단계 무음).
   `AudioManager.mode` 미설정이 근본 — `AudioRouter.setInCall()`(MODE_IN_COMMUNICATION) 도입으로
   해소, 개시자 경로 양방향 음성 실기기 확인됨.

크래시 1·3겹은 UE(pjsua) 결함, 2겹은 UE 무가드 + 서버(psip) PT 하드코딩 결함의 복합이었다.

---

## 이슈 1: 조인 크래시

### 증상 / 크래시 스택 (실기기 2대 동일, KA 무관)

```
../src/pjsua-lib/pjsua_media.c:4422:
  pj_status_t pjsua_media_channel_update(...):
  assertion "call->med_prov_cnt >= local_sdp->media_count" failed
Fatal signal 6 (SIGABRT)  →  pjsua_media_channel_update+368 / +1820
```

- `PJMEDIA_STREAM_ENABLE_KA=0` 격리 빌드에서도 **동일하게 재현** → **RTP keepalive 와 무관**.
- 의미: pjsua 가 이 호에 **provisioning 한 미디어 transport 수(`med_prov_cnt`)** 보다 **UE 가 만든
  로컬 SDP 의 m= 라인 수(`local_sdp->media_count`)** 가 더 많다.

### 와이어 증거 (dumpcap 65초, CSP↔W999 SIP)

- 캡처된 SIP 패킷이 **전부 `CSP(121.161.164.45:15060) → W999`** 방향 (INVITE `CSeq: 1` 재전송 +
  conference NOTIFY). **W999 가 INVITE 에 200 OK 를 단 한 번도 응답하지 않음.**
- UE→CSP 방향 패킷은 NOTIFY 에 대한 `SIP/2.0 500 Unhandled by dialog usages` 하나뿐.
- → **UE 는 offer 를 받아 answer(200 OK) 를 생성/미디어 업데이트하는 도중 200 OK 송신 전에 크래시**한다.
  (media_channel_update 가 answer SDP 확정 직후·200 OK 송신 전에 호출되어 assert→abort)

### CSP fan-out INVITE offer (정상 — 이전 추정 "m=application 0" 은 오류)

CSP 로그·와이어에서 확인한 실제 offer 본문 (multipart/mixed):

```
Content-Type: multipart/mixed;boundary=mcptt_...
--...
Content-Type: application/vnd.3gpp.mcptt-info+xml      ← session-type=prearranged
--...
Content-Type: application/resource-lists+xml           ← 멤버 로스터(크기 안전 시)
--...
Content-Type: application/sdp
Content-Disposition: render

v=0
o=CSS 4 1 IN IP4 121.161.164.45
s=hak
c=IN IP4 121.161.164.45
t=0 0
m=audio 52072 RTP/AVP 99 101       ← AMR-WB(99)+telephone-event(101), 유효 포트
a=rtpmap:99 AMR-WB/16000/1
...
m=application 54018 UDP MCPTT      ← floor 포트 54018 (유효! 0 아님)
c=IN IP4 121.161.164.45
a=floorid:0 mstrm:audio
a=fmtp:MCPTT mc_queueing;mc_priority=3
a=mcptt-floor-request-uri:sip:g001@ptt.mnc033.mcc450.3gppnetwork.org
```

→ offer 는 **2개 m= 라인, 둘 다 유효 포트**로 well-formed. **CSP 측 SDP 결함 없음.**
CSP floor port 유도는 항상 >0 이다 (`GroupCallService.cpp:737`
`iFloorPort = iSharedFloorPortIM>0 ? … : iMemberAudioPort+1`).

### CSP 로그가 보여주는 크래시 루프

- `InviteMember(+82500000001)` 가 **약 40초마다 반복**되는데 대응하는 `OnCallStarted: Joined Group`
  로그가 **한 번도 없음** = W999 조인 실패 루프. 재-INVITE 는 매번 앱 재기동 직후 자동
  REGISTER+PUBLISH(→즉시 fan-out 대상) 로 촉발됨.
- MF52(`+82500000002`)는 한 번 `OnCallStarted: Joined Group(g001) Peer(192.168.0.113:4000
  floor=4001)` 로 **조인 성공한 사례 있음**. `floor=4001 = audioPort(4000)+1` 는 **CSP fallback**
  (`GroupCallService.cpp:1166`) = MF52 answer 의 `m=application` 포트가 0/미파싱이었다는 뜻
  → MF52 의 pjsua 가 미지원 미디어를 **port 0 으로 reject** 하여 answer 에 유지 → UE injection 이
  스킵되어 살아남았을 가능성.

### 근본원인 분석 (pjsip)

- pjsua 는 `m=application`(proto `UDP MCPTT`, 미지원 미디어)에 대해 네이티브 미디어 transport 를
  이해하지 못한다. UE 는 이를 우회하려고 **`onCallSdpCreated` 훅에서 `m=application` floor 섹션을
  로컬 SDP 에 수동 주입**한다 ([CimsCall.kt:73-96](../../../android/core/src/main/java/com/cims/ue/core/sip/CimsCall.kt#L73) `appendMediaSection`, `pendingAppSdp`).
- 주입 조건은 `if (!whole.contains("m=application"))`. **pjsua 가 answer 에 `m=application` 을
  포함하지 않는 경우** 주입이 발동 → 로컬 SDP m= 라인 수가 `med_prov_cnt` 를 초과 → assert 실패.
- `answerGroupCall`/`makeGroupCall` 은 `opt.audioCount=1, opt.videoCount=0` 로 pjsua 에 **미디어
  1개만 provisioning** 지시 ([SipController.kt:207-219, 293-312](../../../android/core/src/main/java/com/cims/ue/core/sip/SipController.kt#L207)).
- pjsua 는 answer 시 `med_prov_cnt` 를 원격 offer 의 `media_count` 로 올려주지만
  (`pjsua_media.c:2539-2541`), 이 주입/미러링 상호작용에서 최종 로컬 SDP media_count 가
  provisioning 수를 넘는 조합이 발생한다.

### 미해결 질문 (다음 세션에 device 진단으로 확정)

1. answer 시점 `onCallSdpCreated` 의 `whole` 이 `m=application` 을 포함하는가? (= injection 발동 여부)
2. injection 후 로컬 SDP 의 최종 m= 라인 수 vs `med_prov_cnt` 실제 값.
3. **W999 는 매번 크래시하는데 MF52 는 한 번 조인 성공**한 차이의 원인(role=chair/offerer vs
   participant/answerer? pjsua 의 미지원 미디어 port-0 처리 차이? 타이밍?).
4. 이 주입 구조는 **07-03 PTT E2E 에선 동작**했다 → 회귀 변수(빌드 assertion(NDEBUG) 활성 여부 /
   자동 affiliation 도입(`5025c511`)으로 answerer·재협상 경로가 상시화된 것) 규명.

### 수정 후보

- **A. UE injection 정합화 (유력)**: answer(UAS)에서 pjsua 가 이미 `m=application`(port 0 포함)을
  넣었으면 **append 하지 말고 그 섹션의 포트만 in-place 교체**(media_count 불변). offer 에만
  없을 때(UE=offerer) 신규 주입. → media_count ↔ med_prov_cnt 항상 정합.
- **B. pjsua provisioning 정합**: 주입하는 미디어 수만큼 pjsua 슬롯을 미리 확보(어려움 — pjsua 가
  application 미디어를 모름).
- **C. 빌드 assertion 확인**: 이전 동작 빌드가 release(NDEBUG=assert 무력화)였는지 확인. 단
  assert 비활성 의존은 취약 — 근본은 media_count 불일치이므로 A 를 우선.

---

## 이슈 2: PTT 발언 무음 (크래시 해결 후 재검증 필요)

증상: 발언권(floor GRANT) 정상인데 **양방향 음성 안 들림**.

- **pcap 확정**: 발언 중에도 상향 audio 가 대부분 **AMR-WB SID(6B, 무음/DTX 프레임)**, 실제 음성
  프레임(~42B) 극소량(25초 발언에 <10 패킷, 정상은 ~50pps). = **마이크→pjsua 로 음성이 안 들어옴.**
- **서버 결백**: floor 회전·CMP `sendAudioToAll` relay·NAT latch·녹취·SDP 협상(AMR-WB sendrecv)
  전부 정상. speaker RTP 도착 시 녹취됨.
- **UE 진단**: `setMic(true) → captureDevMedia.startTransmit(audioMedia)` **정상 호출**, `audioMedia()`
  null 아님(미디어 ACTIVE) — conference bridge 결선은 됨.
- **audio mode = MODE_NORMAL** (VoIP 권장은 MODE_IN_COMMUNICATION). 단 코드에 setMode 전무라
  VoLTE·PTT 동일하고 **VoLTE(Phone 앱)는 양방향 정상** → **mode 는 무죄**.
- **KA 무죄**: KA 는 이론상 음성 억제 안 함(SID 는 캡처 무음의 결과). KA=0 빌드로도 무음 예상.
- **남은 용의자**: PTT 특유 반이중 캡처 경로 — `halfDuplex` mic 토글 / `AudioRouter` /
  grantTone 지연. VoLTE(전이중, AudioRouter 미사용)와의 차이. **크래시가 클린 조인을 막아 KA=0
  무음 재검증 미완.**

---

## 관련 코드 위치

| 위치 | 내용 |
|---|---|
| [CimsCall.kt:73-96](../../../android/core/src/main/java/com/cims/ue/core/sip/CimsCall.kt#L73) | `onCallSdpCreated` — `m=application` 주입(`appendMediaSection`), remote floor 파싱 |
| [CimsCall.kt:165-173](../../../android/core/src/main/java/com/cims/ue/core/sip/CimsCall.kt#L165) | `onCallMediaState` — conference bridge 결선(halfDuplex spk만/mic는 GRANT시) |
| [SipController.kt:207-233](../../../android/core/src/main/java/com/cims/ue/core/sip/SipController.kt#L207) | `answerGroupCall`/`answer` — `audioCount=1`, `pendingAppSdp` 설정 |
| [SipController.kt:293-312](../../../android/core/src/main/java/com/cims/ue/core/sip/SipController.kt#L293) | `makeGroupCall` — offerer 경로, `audioCount=1`, `pendingAppSdp` |
| [PttController.kt:222](../../../android/ptt-client/src/main/java/com/cims/ue/ptt/PttController.kt#L222) | REGISTER Contact 에 MCData SDS ICSI 광고(`MCDATA_ICSI`) |
| [PttService.kt:259](../../../android/ptt-client/src/main/java/com/cims/ue/ptt/PttService.kt#L259) | `injectSsoToken` — 로그인만으로 REGISTER+affiliation PUBLISH(크래시 루프 촉발) |
| `csp/GroupCallService.cpp:737,1166,1502-1566` | fan-out INVITE SDP 빌더(`WrapMultipartBody`), floor port 유도 |
| `pjproject/pjsip/src/pjsua-lib/pjsua_media.c:4422,2493-2665` | assert 지점, `med_prov_cnt` 설정 로직 |

## 재현 방법

1. g001 에 진행 중 세션이 있는 상태(또는 두 단말이 affiliation).
2. ptt-client 앱 실행 → `injectSsoToken` 이 자동 REGISTER+affiliation PUBLISH → CSP 가 즉시
   fan-out INVITE → 단말이 answer 생성 중 크래시. **PTT 물리키 누름 불요** — 실행만으로 재현.
3. 크래시 확인: `adb -s <dev> logcat -b crash -d | grep med_prov_cnt`.

## 크래시 구조 — 세 겹 (순차 규명·수정)

조인 크래시는 **세 겹**이 겹쳐 있었고, 하나를 걷어낼 때마다 다음 층이 드러났다:

1. **assert SIGABRT** `med_prov_cnt >= media_count` (`pjsua_media.c:4422`) — UE 가 floor
   (`m=application`)를 **append** 해서 로컬 SDP m= 라인이 provisioning 수(audio+text=2)를 초과.
   **수정(UE)**: `CimsCall.onCallSdpCreated` 가 append 대신 **기존 `m=text`/`m=application` 슬롯을
   in-place 교체**(media_count 불변). MSRP(`m=message`) 동일.
2. **on_stream_precreate SIGSEGV** — **PT 협상 불일치**. pjsua 는 AMR-WB 를 **dynamic PT 96** 으로
   쓰는데 구 CSP 는 **99 고정** 응답/오퍼 → 협상 PT ≠ 96 이면 pjsua2 `StreamInfo::fromPj` 가
   NULL codec param 을 무가드 역참조(upstream 주석 스스로 "param can be NULL if the stream is
   rejected or disabled" 라 인정하면서 가드 없음).
   **수정(양측)**: ⓐ psip answer PT echo + PT 96 인식(아래 서버 절) ⓑ UE `StreamInfo::fromPj`
   NULL param 가드 + `stream_info.c` `si->param` ZALLOC(비초기화 잔존 방지). `pjmedia_stream_create`
   는 원래 NULL param 폴백이 있어 가드만으로 안전.
3. **pj_sockaddr_print assert SIGABRT** (`sock_common.c:315`, 2번 가드 후 드러난 3번째) —
   pjsua 가 **floor(`m=application`) 슬롯을 text 스트림으로 채널 업데이트**하는 경로.
   `pjmedia_txt_stream_info_from_sdp` 가 비 RTP transport(`UDP MCPTT`)면 stream info 를 비운 채
   (!active) **성공 반환**하는데, `pjsua_txt_channel_update` 게이트는 **포트≠0 만 확인** → 빈 info
   (주소 family=0)로 `on_stream_precreate` → `fromPj` 의 `pj_sockaddr_print` assert. 개시자/응답자
   무관, 200 OK 수신(UAC)·answer 생성(UAS) 양쪽에서 발생 가능.
   **수정(UE)**: ⓐ `pjsua_txt_channel_update` 게이트에 **RTP 협상 조건 추가**
   (`PJMEDIA_TP_PROTO_HAS_FLAG(si->proto, RTP_AVP)`) — 비 RTP application 슬롯은 text 스트림 생성
   자체를 스킵 ⓑ `fromPj` 의 rem_addr/rem_rtcp print 6곳에 address-family 가드.

UE 쪽 pjproject 패치는 전부 `android/docs/scripts/m1_build_pjsip.sh` §2-6~2-9 에 멱등 스크립트로
정본화돼 있다(재빌드 시 자동 적용).

### 규격 확인 (PT 정렬 근거)
- AMR-WB 를 98/99 로 고정하라는 규격 **없음**. PT 96–127=dynamic(RFC 3551), 코덱↔PT 는 `a=rtpmap`
  로만 바인딩(RFC 4566), AMR/AMR-WB 는 dynamic 협상(RFC 4867), answer 는 **rtpmap 이름으로 식별**
  (RFC 3264). psip `IsUseCodec{0,3,4,8,18,98,99}`·`AddSdp case 99` 하드코딩이 결함이었다.

### 서버 측 최종 상태 (라이브)
- **psip RFC 3264 answer PT echo**: `CSipDialog::FindRemotePayloadType()` — 오퍼
  (`m_clsRemoteMediaList`)의 rtpmap 에서 코덱 PT 조회, `AddSdp` answer 가 AMR-WB/AMR/telephone-event
  PT 를 **오퍼값 echo**(오퍼에 없으면 하드코딩 fallback=VoLTE 무영향). PT 96 을 AMR-WB 로
  인식/방출(`IsUseCodec`/offer rtpmap/answer switch case 96).
- **CSP fan-out 오퍼 AMR-WB PT 96** (`GroupCallService.cpp` InviteMember): fan-out 은 CSP 가
  오퍼러라 이 값이 wire PT — **relay 정합** 때문에 96 이 필수다. CMP `sendAudioToAll` 은 발언자
  RTP 를 PT 재작성 없이 relay(SSRC/seq 만 재작성)하고, 개시자 leg 는 answer echo 로 96 협상이므로
  fan-out leg 도 96 이어야 그룹 전 leg 의 PT 가 일치한다(불일치 시 청취자가 패킷 폐기→무음).
- **CSP self-heal**: `OnCallStarted` JoinGroup NOT_FOUND 시 AddGroup 재수립+재시도(CMP 그룹 소실 대비).
- **CMP floor latch IP guard**(0.2.16): floor 패킷 User-ID latch·port-only IP 학습에 sigIp guard +
  no-NAT 멤버 latch 자격 제외.

### UE(앱) 측 최종 상태
- 크래시 수정 3겹(위) + **오디오 라우팅/음량**: `AudioRouter.setInCall()` — 통화 시
  `MODE_IN_COMMUNICATION` + voice-call 스트림 최대, 수화기 아이콘 귀 모양(`ic_earpiece`).
  스피커폰은 pjsua `setOutputRoute` + **AudioManager 직접 제어 병행**(`setSpeakerphone` — 일부
  단말(MF52)은 pjsua 라우팅 무시 실측). **무전 게인 설정화**: 장치단 bridge gain
  (`SipController.setDeviceAudioBoost`, 스피커/마이크 ×1.0~×3.0, 기본 ×1.5) — 설정 탭 슬라이더,
  영속+통화 중 즉시 반영. 상세는 [android_ue_client.md](android_ue_client.md) §5 설정 탭.
- `config_site.h` **KA=1**(RTP keepalive, 청취 전용 구간 NAT 매핑 유지) + floor Ack 주기송신
  (`FloorClient` 15s).

### 실기기 검증 결과
- **조인 크래시 해소 확정**: 개시자(W999 참여→chair 조인) + 응답자(MF52 fan-out
  `OnCallStarted: Joined Group`) 양 경로 crash-free, 크래시 버퍼 0. floor
  REQUEST→GRANT→RELEASE→IDLE 회전 + 상태 브로드캐스트 2명 도달 + Floor ACK keepalive 양 단말
  15s 주기 확인. 양방향 음성 흐름 확인(체감 음량 이슈는 게인 설정화로 해소 — 기본 ×1.5).
- 시스템 오디오 실측: 양 단말 `MODE_IN_COMMUNICATION`+voice-call 스트림 최대+스피커 라우팅에서도
  체감 음량 부족 → 디지털 레벨 보정(장치단 gain)이 올바른 레버임을 확인.

### 잔여 — 사람 확인(귀 테스트)
①게인 기본 ×1.5 적정성(슬라이더로 미세조정) ②MF52 스피커폰 실동작(AudioManager 병행 제어 후)
③이어폰 3택 라우팅·그룹 음량 영속. 확인 완료되면 본 문서는 제거하고 확정 사실을
[ue_nat_traversal.md](ue_nat_traversal.md)·[android_ue_client.md](android_ue_client.md)·
[android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md) 로 흡수한다
(이미 각 문서에 반영됨 — 본 문서의 크래시 근본 분석만 남음).

## 환경 상태

- 라이브: **csp 0.2.36**(PT echo+96 인식+fan-out 96+self-heal) / **cmp 0.2.16**(floor IP guard) /
  cmdp 0.2.0 / oam 0.2.20 / oam-svc 0.2.16 / csc 0.2.9.
- OAM API 배포(사용자 허용): login `admin/1234` https 4419 → `POST /packages`(멀티파트) →
  `PUT /deployments/{2=csp,3=cmp} {package_id}` → `POST /deployments/N/job {job_type:upgrade}` → poll.
- 단말: W999=`192.168.0.83:35659`, MF52=`192.168.0.113:45433`(화면 꺼지면 무선포트 변동).
  adb=`/home/cims/android-sdk/platform-tools/adb`. ptt-client 는 크래시 3겹 수정+KA=1 빌드 설치.
  참여는 주채널 선택 후 자동(개시), affiliation 은 로그인 PUBLISH 자동.
