# Windows PC — 관제 앱 MCVideo (W1'·W4·W5·F3 · 뒤에 W6)

Claude Code 터미널에서 이 문서를 읽고 §1 부터 순서대로 한다. **원칙: VoLTE·MCPTT·MCVideo 규격 절을 먼저 확인하고 그대로 따른다(CLAUDE.md 설계 우선순위 1).**
빌드·시험 명령은 [dispatch_desktop_handoff.md](dispatch_desktop_handoff.md) §4 그대로다. 서버(CSP·CMP·CSC)·SDK 코어·PTT 단말은 .45, 관제 앱 두 벌(Windows
데스크톱·Android 태블릿)은 이 PC 가 맡는다([mcvideo_dev_plan.md](mcvideo_dev_plan.md) §2). 설계 정본 = [mcvideo.md](../design/features/mcvideo.md),
화면 = 검토 캔버스 «MCVideo 단말 UX 검토»(claude.ai 비공개 artifact — 같은 계정이면 열린다. 이 문서만으로 작업할 수 있게 필요한 것은 아래에 옮겼다).

## 0. 먼저 — 확정된 화면 규칙 (사용자, 10-01)

| # | 규칙 | 근거 |
|---|---|---|
| D10 | **«영상 참여» 단계가 없다.** MCVideo 그룹(그룹 문서에 MCVideo 몫이 있는 그룹)은 채널 화면에 처음부터 영상 자리가 있고, 앱이 채널에 들어갈 때 MCVideo 호도 함께 합류한다(chat = 합류, prearranged = MCVideo affiliation + 멤버 초대 자동 수락). 채널을 나가면 함께 나간다 | TS 22.280 R-8.4.2-002 (여러 서비스를 한 번의 논리적 제휴로) |
| D8 | **수신 = manual · 골라 보기** — 영상 칸은 **볼 영상이 있을 때만** 둔다(영상 없는 채널·보내는 사람이 없을 때는 자리도 없다). 누가 보내면 «영상 n» 목록(이름·기능 별칭·경과 + [보기])이 나타나고, 고른 송출만 [보기] → 그때 영상 칸이 생긴다 → [그만 보기]·[바꿔 보기]. 고르기 전에는 영상 RTP 가 오지 않는다. 버튼 문구는 단말과 같게 [보기] | TS 24.581 §6.2.5.3.2·§6.2.5.3.3 · TS 22.281 R-5.2.6.2.3-001·-002 |
| D11 | **음성과 영상의 송출은 따로** — 발언 바·PTT = MCPTT 음성만, 영상 = [영상 보내기](MCVideo 송출 요청·해제). MCVideo 송출은 오디오+영상이라 한 버튼으로 묶으면 음성이 두 호로 겹친다 | TS 22.280 R-8.2.2-001~-004 · TS 22.281 §4.2·§4.4 · TS 24.581 §6.2.5.3.2 |
| D12 | **마이크 경합** — 영상을 보내는 중에 발언하면 그동안 마이크는 음성 무전으로, 영상 호 오디오는 멈춘다(영상은 계속). 설정으로 바꿀 수 있게, 긴급·임박은 늘 우선 | TS 22.280 R-8.3-003·-004 |
| — | 1차 수신 = 한 번에 1개(엔진 렌더 창이 전역 하나). 다른 송출을 보면 보던 것은 그만 본다([바꿔 보기]) | ue_sdk.md §11 «호별 수신 창» |
| — | **기존 화면 틀은 그대로** — 레일·탭 줄·2×2·오른쪽 패널·발언 바(데스크톱), 레일·탭 줄·사이드 패널·발언 바(태블릿)를 바꾸지 않는다. 영상은 그 안의 채널 상세에만 들어간다(단말도 하단 탭을 유지하고 전체화면 대신 주채널 안 [크게]) | 사용자 결정(10-01) |

`git pull --ff-only` — 아래 커밋이 들어 있어야 한다: `cdc8cd95`·`9d3de02d`(C7 — MCVideo C API·.NET 바인딩) · `09e7a6e4`(수동 수락 영상) · `eece42b4` ·
`b2a45e36`(D6 — MCPTT 착신 refresher=uas, 세션 갱신 fmtp) 와 이 문서.

## 1. W1' — SDK 다시 빌드·시험 (C7 이후)

지난 W1(10c62fcb — cimsue_test 93·CimsUe.Tests 76)은 C7 이전 트리일 수 있다. C7 은 **C 구조체가 커졌다**(`cimsue_account_config_t` 끝 `mcvideo_*`,
`cimsue_group_member_t` 등 — struct_size 자기검사 id 8개 추가) → **cimsue.dll 과 CimsUe.dll 을 함께** 바꾼다. 옛 DLL 에 새 .NET 을 붙이면 계정 생성부터 깨진다.

1. `cimsue_test.exe` 전부 — Linux 기준 149건(MCVideo `McvSip`·`McvCall`·`McvParticipant`·`McvCodec`·`McvConfig`, `McpttInvite.MemberInvitationRefresherUas`,
   `FloorSdp.SubsequentOfferDropsInitialOnlyFmtp` 포함). Windows 에서 빠지는 시험이 있으면 이름과 이유를 결과 표에.
2. `dotnet test sdk\windows\dotnet\CimsUe.Tests` — `AbiLayoutTests`(새 struct id 포함)가 **여기서 처음 실제로 돈다**(Linux 는 C 헤더 대조·81/85, 플랫폼 4건은
   Windows 전용).
3. 관제 앱 빌드 0 경고 0 오류.

## 2. W4 — 그룹 편집 «서비스» 절 (데스크톱 · 태블릿)

지금 «영상» 스위치(`Views/GroupEditView.xaml:88` · `ViewModels/GroupEditViewModel.cs:78·151·222`, 태블릿 `ui/groups/NewGroupPanel.kt:146` ·
`PttGroupsScreen.kt:316` · `PttGroupsViewModel.kt:120·376·458`)는 비규격 `<mcpttgi:mcptt-video>`(현행 PTT 영상)다. 콘솔 D3 도안과 같은 의미로 바꾼다.

| 칸 | 값 | 규격 |
|---|---|---|
| 서비스 — MCPTT 음성 | 늘 켬(기존 속성 그대로) | TS 24.481 §7.2.2 |
| 서비스 — MCVideo 영상 | 켜면 `GroupDoc.Mcvideo = McVideoGroupAttrs`(XCAP PUT 에 MCVideo `<service>`) | TS 24.481 §7.2.2·§7.2.8 |
| 호 방식 | chat / prearranged → `InviteMembers` false/true | TS 24.281 §6.3.5.2 |
| 동시 송출 상한 | `MaxTransmitters` | TS 24.481 §7.2.2 |
| 최대 통화 시간 · 수신 유지(T5) · 시작 최소 응답 · 그룹 우선순위 · 참가자 정보 구독 | `MaxDurationSec` · `ReceptionHangTimerSec` · `MinNumberToStart` · `GroupPriority` · `AllowConferenceState` | TS 24.481 §7.2.2 · TS 24.581 §11.1.3 |
| 보호(E2E) | `ProtectMedia`·`ProtectTransmissionControl` = **false 명시**, 칸은 비활성 + «종단간 보호는 아직 지원하지 않습니다» | mcvideo.md D7 |

- **끄기는 관제 앱에서 하지 않는다.** CSC 전환기 규칙 — MCVideo `<service>` 가 없는 PUT 은 MCVideo 상태를 건드리지 않는다(mcvideo.md §5.1 — 옛 단말 PUT
  보호). 켜진 그룹은 체크를 잠그고 «끄기는 운영 콘솔에서» 안내.
- **저장 때 읽은 값을 되싣는다** — 지금 저장은 `GroupDoc.Mcvideo` 를 비운 새 객체를 만든다(null = 서버 값 유지라 해는 없지만, 속성 편집을 넣으면 읽은 객체를
  고쳐 실어야 한다). 목록 행에 서비스 칩 «음성»·«영상».

## 3. W5 — MCVideo 채널 (데스크톱 · 태블릿)

### 3.1 연결 (둘 다)

- **계정** — ue-init-config(W2 ③ 에서 이미 받는다)의 `McvideoServerUri` 가 있고 user profile 자격이 있으면(`CscClient.FetchMcVideoUserProfile` 200)
  `AccountConfig.McvideoEnabled = true`·`McvideoServerUri`·`AutoAnswerMcvideo = true`(prearranged 멤버 초대 자동 수락 — 자동 개시, 수신은 manual).
- **채널 진입·이탈(D10)** — «내 채널»에 들어오는 그룹이 MCVideo 그룹이면 `Account.Affiliate(g, true, McService.McVideo)`(관심 그룹 전부를 한 PUBLISH —
  코어가 모은다) + chat 이면 `Account.JoinVideoGroupCall(g, new VideoGroupCallOptions { Prearranged = false, Queueing = true })`. prearranged 는 초대를
  기다린다. 채널을 나가면 영상 호 `Hangup` + `Affiliate(g, false, McService.McVideo)`. 관제는 «내 채널» 전부에 합류한다(알림을 받으려면 합류해야 한다 —
  chat). 영상 RTP 는 [보기] 한 1개만 온다.
- **«채널» = 관제사가 내 채널에 둔 의도이지 무전 세션 수명이 아니다** — MCPTT 그룹 호는 T4(무발언 hang timer)·TNG3(최대 통화 시간)로 서버가 해제한다
  (TS 24.379 §6.3.8.1). 그것은 호가 끝난 것이지 채널을 떠난 것이 아니므로 영상 호·MCVideo affiliation 은 그대로 둔다. 영상 호를 끝내는 것은 내 채널에서 빼기·로그아웃뿐.
  (단말 실기에서 드러났다 — 무전 세션 종료를 따라 영상 호까지 나갔다.)
- **이벤트** — `Engine.TransmissionChanged`(내 송출: 요청·허가·거절·대기·회수·끝) · `Engine.ReceptionChanged`(새 송출 알림·받는 중·수신 거절·서버 종료·송출 끝).
  채널마다 «보내는 중» 목록 = 알림(Notified)으로 더하고 송출 끝(End Notify)으로 뺀다. 스냅샷 `Call.TransmissionInfo`.
- **조작** — [보기] = `Call.AcceptReception(transmitterId)` · [그만 보기] = `EndReception` · [바꿔 보기] = 보던 것 `EndReception` 뒤 `AcceptReception` ·
  [영상 보내기] = `RequestTransmission()` / 끄면 `ReleaseTransmission()`.
- **발언 바·PTT = 음성만**(D11). 영상 송출 중 발언(D12) — 코어 공개 API 가 필요하다(호별 오디오 송신 멈춤, .45 에 요청 — 지금은 엔진 내부 `setAudioTx`).
  1차 관제 앱은 [영상 보내기]를 비활성으로 두므로(아래) 당장은 필요 없다.

### 3.2 화면 — Windows 데스크톱 (캔버스 W1)

- **배너 층** — «새 영상 · {그룹} — {이름}({기능 별칭})이 영상을 보냅니다 [보기] [닫기]». 토스트(명령 실패 전용 — dispatch_desktop_ui.md)가 아니라 착신 배너와
  같은 층. [보기] = 그 채널의 상세 패널을 열고 영상 칸을 띄운다. [닫기] 해도 채널 «영상 n» 목록에서 다시 [보기] 할 수 있다(TS 22.281 R-5.2.6.2.2-009 NOTE 3).
- **채널 카드** — 1줄 태그 «영상 n»(보내는 중 수). 카드의 «조작 하나» 규칙은 그대로.
- **오른쪽 패널 채널 상세 «영상» 절**(MCVideo 채널만 — 아니면 절 자체가 없다) — 상태에 따라 셋:
  - 보내는 사람 없음 = 머리 한 줄 «영상 · 보내는 사람 없음» + [영상 보내기](1차 비활성 «카메라 없음»)만. 영상 칸 없음.
  - 누가 보냄 = «영상 n» 목록 행(이름 · 기능 별칭 · 경과 · 보는 사람 n + [보기]). 아직 영상 칸 없음.
  - [보기] 뒤 = 목록 위에 영상 칸 — **기본 세로 480×640(3:4)**, 패널 안 270×360 · [↺][↻] = 90° 씩 회전(가로면 4:3 360×270, 보내는 사람마다 기억 — 사용자 10-01)
    (캡션 «이름 · 기능 별칭 · 경과», [창으로 ↗]) · [그만 보기] · [영상 소리](영상 호 음량 — R-8.3-002), 목록의 그 행은 «보는 중», 다른 행은 [바꿔 보기].
- **영상 칸 = F3**(§4) — 엔진 프레임을 그린다. 첫 장 전 «영상 기다리는 중…», 영상 미디어가 없는 호는 «영상 미디어가 열리지 않았습니다 — 영상 호 소리만 들립니다».
- **이벤트 칸** — «{그룹} · {이름} 영상 보내기 시작/끝».

### 3.3 화면 — Android 태블릿 (캔버스 T1)

- 같은 의미론·같은 세 상태. 사이드 패널 400 의 «영상» 절 — «영상 n» 목록 [보기] → 세로 3:4 칸(기본 480×640, [↺][↻] 90° 회전 — 데스크톱과 같다) + [그만 보기]·[크게 보기 ↗](본문 자리를 영상 한 장으로 —
  레일·탭 줄·발언 바는 그대로)·[영상 소리]. 채널 카드 태그 «영상 n»(보내는 중 수, 없으면 태그 없음).
  렌더 = SDK `setVideoSurface`(엔진 전역 하나 — Android 엔진은 영상이 있다).
- CAMERA 권한은 [영상 보내기]를 둘 때(후속) 추가한다. 전역 배너(MainActivity 184-194)에 «새 영상» 한 종류를 더한다.

### 3.4 문구 (단말과 같은 사전 — 캔버스 P5)

| 상황 | 문구 | 근거 |
|---|---|---|
| 수신 거절 #7 | 더 볼 수 없습니다 — 동시에 볼 수 있는 영상(1)이 찼습니다 [바꿔 보기] | TS 24.581 §6.2.5.4.2 |
| 서버 수신 종료 | {이름} 영상 보기가 끝났습니다 | §6.2.5.5.5 |
| 송출 끝 | {이름}이 영상 보내기를 멈췄습니다 | §6.2.5.3.4 |
| 송출 요청 중 / 허가 / 대기 | 영상 보내기 요청 중… / 내 영상 송출 중 · 보는 사람 n / 대기 n번째 [대기 취소] | §6.2.4.3.2 · §6.2.4.4.6 · §6.2.4.5.6 · §6.2.4.4.5 |
| 송출 거절 #1 · #5 | 보내지 못했습니다 — 동시에 보낼 수 있는 수(n)가 찼습니다 · 이 그룹에서는 영상을 받기만 할 수 있습니다 | §9.2.6.2 |
| 송출 회수 #2 · #4 | 보내기가 멈췄습니다 — 한 번에 보낼 수 있는 시간을 넘었습니다 · 우선순위가 높은 송출이 들어왔습니다 | §9.2.10.2 |

## 4. F3 — Windows 영상 엔진 (구현 — 10-01)

Windows 엔진이 영상으로 빌드된다 — 정본 = [ue_sdk.md](../design/features/ue_sdk.md) §4.5 «Windows 영상 = 창 없는 프레임 렌더»·§6.1 «영상» 행, 앱 = [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §10.3.

- **엔진(ext/pjproject — README.CIMS.md 인벤토리)** — `PJMEDIA_HAS_VIDEO 1` · OpenH264(vcpkg `openh264:x64-windows`, DLL `openh264-7.dll`) · DirectShow 캡처 · 동봉 libyuv ·
  **CIMS 프레임 렌더 장치** `pjmedia-videodev/cims_frame_dev.{h,c}`(BGRA 프레임 → 콜백, 창 핸들 = 토큰). 엔진 CMake 결함 둘을 함께 고쳤다 — `config_auto.h.cm` 에 영상 코덱 칸이 없어
  OpenH264 가 등록되지 않던 것, DirectShow 백엔드가 BaseClasses 를 빌드하지 않던 것(streams.h min/max 보정 포함). OpenH264 IDR 주기 2 초(upstream 0 = 요청에만).
- **코어** — `Listener::onVideoFrame(VideoFrame)`(영상 스레드, 이벤트 잠금 밖) · `Engine::setVideoPreview`(송출 중 셀프뷰 callId -1) · `setVideoCaptureDevice` · Windows 인코딩 640×480 ·
  `setVideoWindow` 는 프레임 렌더 빌드에서 실패. C API `on_video_frame`·`cimsue_video_frame_t`(struct id `VIDEO_FRAME`)·`set_video_preview`·`set_video_capture_device`·`set_video_send`·
  `call_info.video_send`, .NET `Engine.VideoFrameReceived`(ref struct `VideoFrame`)·`SetVideoPreview`·`SetVideoCaptureDevice`·`Call.SetVideoSend`·`CallInfo.VideoSend`·
  `VideoDeviceInfo.IsCamera`. SWIG(Android)는 프레임 표면을 내지 않는다(Surface 직결). **.45 SDK 코어와 계약 맞춤 필요** — 공개 헤더가 바뀌었다(아래 §7).
- **앱** — `Services/VideoFrames`(우편함 — 보는 칸만 최신 한 장) · `Views/VideoView`(WriteableBitmap) · [영상 보내기] 요청·대기·송출·끝 · 셀프뷰 160×120(거울상) ·
  «보는 사람 n»(서버 Media Reception Notification 이 올 때) · D12 설정 «영상 보내는 중 무전»(음성 우선 기본 / 영상 우선) · 설정 [영상] 카메라 · 패키지에 `openh264-7.dll`.
- **시험** — 단위 `McvCall.VideoTransmitSelfViewAndReceiveFrames`(합성 캡처 → H.264 RTP → 셀프뷰 → 가짜 CMP 가 되돌린 RTP 디코드 프레임, `CIMSUE_TEST_CAMERA=1` = 이 PC 의
  실카메라 OV02C10 으로도 통과) · `CimsUe.Tests.VideoEngineIsFrameSink`. 남은 실기: 현장 앱(C9 빌드 W999·MF52)이 보내는 영상을 관제가 [보기] → 그림 · 회전 · [바꿔 보기],
  관제 [영상 보내기] → 현장 앱이 [보기](.48/.45 — §6).

## 5. W6 — 영상 벽 별창 (후속)

여러 송출을 한 화면에 — 2×2·3×3 격자, 칸마다 [그만 보기], 보지 않는 송출 목록 [보기], 배치는 `layout.json` 에 기억(감청 창처럼 별창). SDK «송출자별 렌더
창»(ue_sdk.md §11 — 지금 렌더 창은 엔진 전역 하나) 뒤. 관제사 «새 영상 자동으로 보기»(TS 22.281 §5.2.7.1)도 그때.

## 6. 시험 — 대상 .48 (MCVideo 켜짐)

- .48: csp 0.2.182(`Setup.Roles.MCVIDEO=true`) · cmp 0.2.106 · csc 0.2.139(`UeInitConfig.ServiceDetails.McVideo.Enable=true`). 시험 그룹 gmv1(chat · 동시 송출
  상한 1) · gmv2(prearranged · T1 10 s), 멤버 test023~025(자격 = NAS `test48/tester/scenarios/creds/ptt.jsonl` 자리). 절차·결과 = server45_handoff.md §12.5 ·
  mcvideo_m2_runbook.md.
- 관제 계정을 gmv1/gmv2 멤버로 넣는 것은 .48 관리 API(A6)로 — **사용자 확인 뒤**(.45 에 요청).
- 송출 쪽: Linux cimsue-cli 는 영상 없는 빌드라 영상 호 오디오·송출 제어만 보낸다(`cimsue-cli video-call gmv1 --transmit-at 2 --transmit-len 20`, .45 에
  요청). 실제 영상은 C9 빌드 PTT 앱(MF52·W999) — .45 와 일정을 맞춘다.
- 확인: ① 채널 진입만으로 영상 호 합류(INVITE·affiliation PUBLISH, «영상 참여» 버튼 없음) ② 남이 보내면 배너 «새 영상» → [보기] → 수신(태블릿 = 그림, Windows =
  자리 표시 + 소리) → [그만 보기] ③ 두 사람이 보내면 [바꿔 보기] ④ 발언 바 PTT 는 음성만(MCPTT floor) ⑤ 채널 나가기 = 영상 호도 BYE ⑥ 보내는 사람이 없거나
  영상 없는 채널이면 채널 상세에 영상 칸이 없다.

## 7. 결과

이 문서 끝에 항목별 결과 표(W1'·W4·W5 데스크톱·W5 태블릿, 시험 ①~⑥)를 붙이고 커밋·푸시한다. 막히면 dev_share 에 `_win_` 메시지로.

| 항목 | 결과 | 비고 |
|---|---|---|
| W1' | 끝 — d1685a78 까지 Windows 재빌드: `cimsue_test` 144/144 · `CimsUe.Tests` 85/85(`AbiLayoutTests` 포함) · 관제 앱 Release 경고 0 | 리눅스 149 와의 차 5건 = `FloorXCheck.*`(floor_xcheck_test.cpp — CMP `PFloorCodec` 이 pasf(pthread·semaphore)를 끌어 리눅스 전용) |
| W4 데스크톱 | 끝 — 그룹 편집 «서비스» 절(MCPTT 음성 늘 켬 · MCVideo 영상 켜기·호 방식·송출 상한·TNG3·T5·최소 응답·우선순위·참가자 정보, 종단간 보호 비활성 false 명시, 읽은 객체 되싣기, 켜진 그룹 잠금 «끄기는 운영 콘솔에서») · 목록 행 칩 «음성»·«영상» · 상세 능력 줄 | «영상» 칩은 관리 목록에 서비스가 없어 내 영상 채널·열어 본 문서로만(서버 과제 X7) |
| W5 데스크톱 | 끝(영상 그림·송출 제외 — F3, 다음 Windows 세션) — 게이트(ue-init-config MCVideo PSI ∧ user profile) · 계정 태그 · 영상 채널 MCVideo affiliation · D10 자동 합류(chat `Queueing`, 편성 = 초대 자동 수락, N6 안에서 카드 순서, 무전 T4 와 무관, 끝나면 3 s / 실패 10 s 배수 → 2 분 재합류, 그룹이 빠지면·로그아웃 때만 나감) · 채널 상세 «영상» 절 세 상태 · 영상 칸 기본 세로 480×640(3:4) + [↺][↻] 90° 회전(보내는 사람마다 기억) · [보기]/[바꿔 보기]/[그만 보기]/[영상 소리] · [영상 보내기 — 카메라 없음] 비활성 · 카드 «영상 n» · «새 영상» 배너 [보기][닫기] · «이벤트» [영상] · §3.4 문구 · D6 무전 음량 ×0.3 | 영상 칸 = «이 PC 에서는 영상을 표시할 수 없습니다(영상 엔진 준비 중)» 자리(F3). 미리보기 `--ui-preview-banner=video`·`--ui-preview-rotate=90` |
| W5 태블릿 · W4 태블릿 | 안 함 — Windows 안정화 뒤(사용자 결정 09-29) | 의미론은 [android_dispatch_tablet.md](../design/features/android_dispatch_tablet.md) 한 줄 |
| F3 | 끝(실기 전) — 엔진 영상(OpenH264·DirectShow·libyuv·CIMS 프레임 렌더 장치)·코어/C API/.NET 프레임·셀프뷰·카메라 선택·앱 영상 칸 그림·[영상 보내기]·셀프뷰·D12 설정·카메라 설정·패키지. `cimsue_test` 145/145(실카메라 `CIMSUE_TEST_CAMERA=1` 도 145/145) · `CimsUe.Tests` 87/87 · 앱 경고 0 · 패키지 `build-win/dist-f3`(63.4 MB, 패키지 dotnet.exe 로 렌더 확인) | 공개 헤더 변경 = `listener.h onVideoFrame`·`engine.h setVideoPreview/setVideoCaptureDevice`·`types.h VideoFrame`·`cimsue_c.h`(끝에 덧붙임) — .45 SDK 코어와 맞춘다. 미리보기 `--ui-preview-videotx` |
| 오른쪽 패널 겹침 | 끝 — 패널이 본문 위에 겹쳐 오른쪽에서 밀려 들어오고 나간다(220/180 ms, 시스템 애니메이션 끔 = 즉시), 본문 배치 그대로(패널 때문에 좁은 배치로 바꾸지 않는다), 토스트는 패널 왼쪽 | 사용자 요청(10-01). 태블릿(400dp 밀어내기)은 그대로 — 태블릿 작업 때 정한다 |
| 시험 ①~⑥ | 실기 전 | .45 가 MCVideo 를 켰다(41100484) — 관제 계정 PTT 회선의 MCVideo 자격·N6·영상 채널 멤버 편성은 사용자 확인 뒤 |
| 서버 과제 | [server45_handoff.md](server45_handoff.md) §13 X1~X9 | X1 관제석 N6 · X2 상시 합류 자원 · X3 T11 · X4 이력 서비스 축(앱 반영 — CSC 패치 대기)·X5 이력 피드·X9 통화 이력의 영상 통화 누락(CSC 패치 대기) · X6 N2 · X7 목록 서비스 표시 · X8 관리 화면 자격 |
