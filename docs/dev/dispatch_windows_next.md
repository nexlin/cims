# Windows PC — 관제 앱 이어서 할 일 (긴급·경보 서버 반영 짝 · MCVideo 준비)

Claude Code 터미널에서 이 문서를 읽고 §2 부터 순서대로 한다. **원칙: VoLTE·MCPTT 규격 절을 먼저 확인하고 그대로 따른다(CLAUDE.md 설계 우선순위 1).**
빌드·시험 명령은 [dispatch_desktop_handoff.md](dispatch_desktop_handoff.md) §4 그대로다. 서버(CSP·CMP·CSC)는 .48 이 소스 서버, 단말 SDK·Android 는 .45 가 맡는다
([mcvideo_dev_plan.md](mcvideo_dev_plan.md) §2).

## 1. 먼저

- `git pull --ff-only` — 커밋 `ea2b617d`(SDK 코어·바인딩: 해제 인가·disposition 통지 규격 본문·ue-init-config PSI·지시자 조합)가 들어 있어야 한다.
- 이 변경은 **C API 인자가 바뀌어** DLL 과 .NET 을 함께 다시 빌드해야 한다(§2). 옛 DLL 에 새 .NET 을 붙이면 `cimsue_engine_send_sds_notification` 호출이 깨진다.

## 2. W1 — SDK 변경분 Windows 빌드·시험

Linux 는 `cimsue_test` 98건·S1-UE 전 항목 PASS 다. Windows 는 이 PC 에서만 빌드된다.

| 바뀐 것 | C API | .NET |
|---|---|---|
| 해제 인가 요소(TS 24.484 §8.3.2.1 11)xiv)·xvii)) | `cimsue_user_profile_doc_t` 끝에 `allow_cancel_group_emergency`·`allow_cancel_imminent_peril`, `cimsue_capabilities_t` 끝에 `cancel_group_emergency`·`cancel_imminent_peril` | `UserProfileDoc`·`Capabilities` 끝 인자(기본 true) |
| UE initial configuration(TS 24.484 §7.2) | `cimsue_ue_init_config_doc_t` · `cimsue_csc_fetch_ue_init_config` · `cimsue_ue_init_config_parse` · struct id `UE_INIT_CONFIG_DOC` | `UeInitConfigDoc` · `CscClient.FetchUeInitConfig(mcsUeId)` |
| MCData PSI | `cimsue_account_config_t` 끝 `mcdata_server_uri` | `AccountConfig.McdataServerUri` |
| disposition 통지 규격 본문(TS 24.282 §12.2.1.1) | `cimsue_engine_send_sds_notification(…, msg_id, **group_id**, notif_type, …)` | `Account.SendSdsNotification(peer, convId, msgId, notifType, groupUri)` |

확인:
1. `cimsue_test.exe` — 새 시험 `CmsDoc.ParseUeInitConfig`·`SdsCodec.NotificationSpecForm`·`SdsCodec.CallingIdentitiesFromMcdataInfo`·`McpttXml.IndicatorOrderAndAlert`, `McpttCondition`(임박→긴급 와이어·규격형 통지 MESSAGE) 포함 전부.
2. `dotnet test sdk\windows\dotnet\CimsUe.Tests` — `AbiLayoutTests`(`UE_INIT_CONFIG_DOC` 크기 포함)·`CscTests.CmsDocsAndCapabilitiesFollowCore`(해제 인가·ue-init-config) 포함 전부.
   필드 순서는 Linux 에서 C 헤더와 대조만 했다 — 여기서 처음 실제로 돈다.
3. 관제 앱 빌드 — `DispatchSession.SendSdsNotification` 은 기본 인자라 그대로 컴파일돼야 한다(§3 ② 에서 고친다).

## 3. W2 — 긴급·경보 서버 반영의 관제 앱 짝

서버는 .45 에 csp 0.2.180·csc 0.2.138·oam 0.2.182 로 올라가 있다([server45_handoff.md](server45_handoff.md) §10). 관제 앱에 남은 넷:

| # | 할 일 | 위치 | 규격 |
|---|---|---|---|
| ① | **[긴급 해제] 자격 = `Capabilities.CancelGroupEmergency` ∨ 내가 올린 조건.** 지금은 `s.IsConditionMine` 만 본다. 임박 해제는 `CancelImminentPeril`(개시자 예외 없음) | `Services/DispatchSession.cs:1155` `CanCancelCondition` · 배너 `CanCancel`(:1141·:1147) | TS 24.379 §6.2.8.1.7·§6.2.8.1.10, 서버 판정 §6.3.3.1.13.4·.6 |
| ② | **SDS 전달 확인에 그룹을 넘긴다** — `m.GroupUri`(= `<mcdata-calling-group-id>`) | `ViewModels/McDataMessagesViewModel.cs:46` → `DispatchSession.SendSdsNotification`(:1673)에 `groupUri` 인자 추가 | TS 24.282 §12.2.1.1 3)·5) |
| ③ | **계정 만들기 전 ue-init-config 로 PSI 두 개** — `CscClient.FetchUeInitConfig(instanceId)` → `cfg.McpttServerUri`·`cfg.McdataServerUri`. MCS UE ID = 지금 쓰는 `instanceId`(MachineGuid urn:uuid). 못 받으면 비운 채(경보 = 그룹 URI, 통지 = 원 발신자 직행) | `Services/DispatchSession.cs:437~453` 계정 루프 앞 | TS 24.484 §7.2.1.1·§7.2.2.1 10)·14) |
| ④ | **경보 취소 403 이면 배너를 되살린다** — 지금은 취소 MESSAGE 를 보내자마자 배너를 내린다. 새 CSP 는 권한 없는 취소에 403 + `alert-ind` true(경보 유지)로 답한다 → `RequestCompleted` 의 token 으로 결과를 받아 403 이면 배너 복원·«경보 해제 권한 없음» | `Services/DispatchSession.cs:1186` `CancelBanner` | TS 24.379 §12.1.3.2 |

실서버 확인(.45):
- 관제 계정 PTT 회선의 «긴급 해제»(콘솔 가입자 → PTT 회선 카드 «긴급 (SOS)») 끔 → 남이 건 긴급에 [긴급 해제] 없음 / 켬 → 보이고 해제 200.
- 그룹 SDS 수신 → 전달 확인 발송(.45 CSC 는 MCData 를 광고하지 않아 옛 형식 — 정상).
- 권한 없는 계정으로 남의 경보 [경보 해제] → 403 → 배너 남음.

## 4. W3 — MCVideo 준비 (설계만, 코드 없음)

MCVideo 는 별도 MC 서비스다(설계 정본 [mcvideo.md](../design/features/mcvideo.md), 분담 [mcvideo_dev_plan.md](mcvideo_dev_plan.md)). 확정 결정: D5 chat · D6 음성 호·영상 호
둘 다 유지(소리가 겹치면 영상 호 우선) · D9 전환 기간 없음. 관제 앱 구현은 SDK API 선언(K7)과 .NET 바인딩(C7) 뒤라 지금은 설계와 리뷰만 한다.

1. **K7 리뷰 준비** — .45 가 SDK MCVideo API 선언을 올리면 관제 앱 요구로 검토한다: 스트림별 렌더 창(지금 `setVideoWindow` 는 창 하나), 송출·수신 이벤트에 송출자 ID·SSRC,
   두 호(MCPTT·MCVideo) 동시 참여 시 호 구분(`CallInfo.service`).
2. **그룹 편집 «영상» 토글 위치 파악(W4)** — `Views/GroupEditView.xaml:88`·`ViewModels/GroupEditViewModel.cs:78·151·222` 는 비규격 `<mcpttgi:mcptt-video>` 를 쓴다.
   1차 배포 때 MCVideo 서비스 켜기(그룹 문서 MCVideo `<service>` + `<mcvideo-*>` 속성)로 바뀐다 — 지금은 바꾸지 않는다(서버 A3·SDK C2 뒤).
3. **관제 앱 MCVideo 화면 설계 초안** — [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) 에 절로 적는다:
   - 채널 상세에 [영상 참여]/[영상 나가기] — MCPTT 채널 카드와 같은 그룹, 음성 호는 그대로(D6)
   - [영상 보내기](송출 요청 — 발언 바의 PTT 와 따로), «새 영상» 알림 → [받기]/[그만 보기](수신 manual)
   - 수신 영상 칸 — 1차는 한 개(R1), 다중 스트림 격자(W6)는 엔진 확장 뒤
   - 후속(V8) 자리만 표시: 원격 영상 보기(ambient viewing)·영상 가져오기(pull)·원격 송출 요청·긴급 영상 자동 표시
4. **태블릿** — 같은 의미론을 [android_dispatch_tablet.md](../design/features/android_dispatch_tablet.md) 에 한 줄 기록(코드는 Android 빌드 환경에서).

## 5. 결과

이 문서 끝에 항목별 결과 표를 붙이고 커밋·푸시한다. .45·.48 은 이 표를 보고 짝을 맞춘다.
