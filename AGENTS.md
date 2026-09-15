# Codex 리뷰 지침

## 1. 역할과 범위

- 답변은 한국어로, 명령어·경로·코드·규격 이름은 원문으로 쓴다.
- Claude Code가 설계·구현의 주체이자 제안자다. Codex는 코드와 **설계 방향성**을 리뷰한다.
- Codex는 설계를 말없이 바꾸지 않는다. 문제와 대안을 제시하고, 채택 여부는 사람이 정한다.
- 관제 앱은 개발 중이며 요구·설계가 바뀔 수 있다. 정본 문서는 검토·갱신 대상이지 불변 명세가 아니다.
- 상세 설계는 `docs/`에 둔다. 이 파일에는 판정 기준과 계약별 탐색 경로만 둔다.

## 2. 판정축과 우선순위

`CLAUDE.md`의 「개발/기능보완/문서 현행화 원칙」에 따라 아래 순서로 판정한다.

1. **표준규격 준수가 최우선**이다. 관련 3GPP TS/RFC와 어긋나는 편의 구현은 채택하지 않는다.
2. **규격 안에서 체계성·일관성**을 중시한다. 자원 모델·명령·명명·계약에 일회성 예외를 늘리지 않는다.
3. **band-aid를 최소화**한다. 올바른 구조를 먼저 제시하고, 최소 보완안은 명시적 요청이나 호환 전환기 등 근거가 있을 때만 대안으로 든다.

- 위 우선순위 아래에서 **층 경계**(코어/파사드/플랫폼 접점/앱, 단말/서버 책임), **정본 문서와의 정합**, **생성물의 단일 정의**를 함께 검사한다.
- 규격·문서·코드가 충돌하면 아래 A~D로 원인을 판정한다. 기존 코드나 문서라는 이유만으로 옳다고 보지 않는다.

## 3. 계약 단위 리뷰와 계약 지도

- 리뷰 범위는 폴더 전체가 아니라 **계약 = 정본 문서 / 서버 구현 / 단말 구현**의 3점 세트다.
- 변경에 해당하는 행을 고르고 세 지점의 관련 절·함수를 읽는다. 호출·인가·저장 경계가 이어지면 필요한 경로만 추가 추적한다.
- 표는 탐색 진입점이다. 경로가 존재한다는 사실을 동작 검증으로 취급하지 않는다. 표 밖 계약도 같은 방식으로 세 지점을 찾는다.
- 단말 칸은 현재 코어·Windows 이식 원본이다. Android 구현이 생기면 대응 경로를 같은 변경에서 연결한다. 없는 파일을 지어내지 않는다.
- 표의 경로 접두: `D/` = `docs/design/features/`, `K/` = `sdk/core/src/`, `N/` = `sdk/windows/dotnet/CimsUe/`, `W/` = `windows/dispatch-desktop/`.
- 공통 API·파사드 경계는 `D/ue_sdk.md` §4.2·§5·§6.4·§7, `sdk/core/include/cimsue/`, `K/c_api.cpp`, `N/Engine.cs`, `N/CscClient.cs`를 해당 변경에 맞춰 확인한다.

| 계약 | 정본 문서·절 | 서버 구현 | 단말 구현 |
|---|---|---|---|
| 1. 로그인·프로비저닝·데스크/대상 발견 | `D/android_ue_provisioning.md` §1·§3·§4; `D/dispatch_center.md` §8.4 | `csc/src/services/mcptt.py` | `K/csc/csc_client.cpp`; `N/CscClient.cs`; `W/Services/DispatchSession.cs` |
| 2. 등록·접속·보안 프로파일 적용 | `D/ue_sdk.md` §4.2·§7; `D/android_ue_provisioning.md` §3 | `csp/CscfModule.cpp`; `csc/src/services/mcptt.py` | `K/engine.cpp`; `K/account_map.cpp`; `K/http/https_client.cpp`; `W/Services/DispatchSession.cs` |
| 3. 전화 그룹·역할 관리·대표번호 | `D/dispatch_center.md` §3·§4·§8.2 | `csc/src/handlers/dispatch.py`; `csc/src/services/authz.py`; `csp/CspPhoneGroup.cpp`; `csp/TasModule.cpp` | `K/engine.cpp`; `W/Services/ManagementClient.cs`; `W/ViewModels/CallDeskViewModel.cs` |
| 4. BLF·dialog 구독·통화 상태 | `D/dispatch_center.md` §4.5·§5.2·§5.6a; `D/ue_sdk.md` §7 | `csp/CscfModule.cpp`; `csp/CspServer.cpp`; `csp/GroupCallService.cpp`; `csp/TasModule.cpp` | `K/engine.cpp`; `W/Services/DispatchSession.cs`; `W/ViewModels/CallActivityViewModel.cs` |
| 5. 당겨받기·호 전달 | `D/ue_sdk.md` §7; `D/dispatch_center.md` §4.4; `D/dispatch_desktop_ui.md` §4.3·§9 | `csp/TasModule.cpp` | `K/engine.cpp`; `N/Account.cs`; `N/Call.cs`; `W/ViewModels/CallDeskViewModel.cs` |
| 6. 감청 Join·SSRC·소스 표시 | `D/dispatch_center.md` §5.3·§5.4·§6; `D/ue_sdk.md` §4.5·§11 | `csp/TasModule.cpp`; `cmp/PRtpTap.cpp` | `K/engine.cpp`; `W/ViewModels/MonitorWindowViewModel.cs` |
| 7. PTT 그룹콜·affiliation·구독·청취 | `D/dispatch_center.md` §5.6; `D/ue_sdk.md` §7 | `csp/GroupCallService.cpp`; `csp/CscfModule.cpp`; `cmp/PMcpttGroup.cpp` | `K/engine.cpp`; `K/mcptt/mcptt_xml.cpp`; `W/ViewModels/PttChannelsViewModel.cs`; `W/ViewModels/ScopedChannelsViewModel.cs` |
| 8. floor·다중 채널 발언 | `D/ue_sdk.md` §4.6; `D/dispatch_desktop_ui.md` §4.1·§13 | `cmp/PMcpttGroup.cpp`; `csp/GroupCallService.cpp` | `K/floor/floor_participant.cpp`; `K/floor/floor_codec.cpp`; `K/engine.cpp`; `W/ViewModels/TalkBarViewModel.cs` |
| 9. 타인 사설콜·애드혹 관측 | `D/dispatch_center.md` §5.6a; `D/dispatch_desktop_ui.md` §4.2·§13 | `csp/GroupCallService.cpp`; `csp/CspServer.cpp`; `csp/CscfModule.cpp` | `K/engine.cpp`; `W/ViewModels/ScopedChannelsViewModel.cs`; `W/Services/DispatchSession.cs` |
| 10. PTT 그룹 GMS XCAP CRUD | `D/ue_sdk.md` §7; `D/dispatch_center.md` §3.4; `D/dispatch_desktop_ui.md` §4.7 | `csc/src/services/mcptt.py`; `csc/src/handlers/dispatch_directory.py`; `csc/src/services/authz.py` | `K/csc/csc_client.cpp`; `K/csc/group_doc.cpp`; `N/CscClient.cs`; `W/ViewModels/GroupEditViewModel.cs`; `W/ViewModels/GroupAdminViewModel.cs` |
| 11. 전화번호부·조직·구성원·번호 관리 | `D/android_ue_provisioning.md` §3-1·§3-3; `D/dispatch_center.md` §3.4 | `csc/src/services/mcptt.py`; `csc/src/handlers/dispatch_directory.py`; `csc/src/services/authz.py` | `K/csc/csc_client.cpp`; `W/Services/DirectoryService.cs`; `W/Services/ManagementClient.cs`; `W/ViewModels/DirectoryAdminViewModel.cs` |
| 12. 통합 이력·메시지 모니터링·PTT 상세 | `D/android_ue_provisioning.md` §3-2·§3-2a; `D/dispatch_center.md` §5.7a | `csc/src/services/mcptt.py`; `csc/src/services/dispatch_history.py`; `csc/src/handlers/dispatch_recordings.py` | `K/csc/csc_client.cpp`; `W/Services/HistoryClient.cs`; `W/Services/ManagementClient.cs`; `W/ViewModels/SessionHistoryViewModel.cs` |
| 13. 녹취 생성·열람·재생 | `D/android_ue_provisioning.md` §3-4; `D/dispatch_center.md` §5.7b; `D/dispatch_desktop_ui.md` §4.6 | `csc/src/handlers/dispatch_recordings.py` → `ems/core/oam/src/handlers/recording.py` → `cmp/PSyncRtpRecorder.cpp` | `K/csc/csc_client.cpp`; `W/Services/ManagementClient.cs`; `W/ViewModels/SessionHistoryViewModel.cs`; `W/Views/HistoryView.xaml` |
| 14. floor 단일 정의·생성·대조 | `D/ue_sdk.md` §4.6·§9; `D/mcptt_floor_defs.yaml`; `scripts/gen_floor_defs.py` | `cmp/PMcpttGroup.h`; `scripts/mcptt_floor_policy_probe.py` | `K/floor/floor_defs.h`; `android/ptt-client/src/main/java/com/cims/ue/ptt/floor/FloorControl.kt` |
| 15. MCData 메시지(PTT SDS) | `D/mcdata_messaging.md` §1~§5·§7·§8; `D/dispatch_desktop_ui.md` §4.4 | `csp/McDataAsModule.cpp`; `csp/McDataCodec.cpp`; `csp/McDataMediaService.cpp`; `csp/CmdpClient.cpp`; `cmdp/PCmdpServer.cpp`; `cmdp/PMsrpConnection.cpp`; `cmdp/PFdStore.cpp`; `csc/src/services/mcdata_fd.py` | `K/mcdata/sds_codec.cpp`; `K/engine.cpp`; `N/Account.cs`; `W/ViewModels/McDataMessagesViewModel.cs` |
| 16. SMS·LMS 1:1 | `D/dispatch_desktop_ui.md` §4.3·§13; `D/mcdata_messaging.md` §4·§4.3 | `csp/ModuleDispatcher.cpp` | `K/engine.cpp`; `N/Account.cs`; `W/Services/DispatchSession.cs`; `W/ViewModels/SmsMessagesViewModel.cs` |

## 4. A~D 설계 판정

- 먼저 적용 규격·문서 계약·현재 구현을 대조하고 아래 유형을 구분한다. 겹치면 주 판정과 연관 판정을 함께 적는다.
- **A. 구현의 계약 위반**: 유효한 정본 계약을 구현이 어겼다면 해당 코드를 고치도록 제안한다. 근거는 문서 절 번호다.
- **B. 문서의 오류·노후화**: 규격 조항 또는 기존 계약과의 일관성 논거로 문서 변경을 제안한다.
  `CLAUDE.md`대로 문서와 코드가 다르면 현재 동작은 코드를 정본으로 확인하고 문서 갱신으로 차이를 해소한다. 단, 규격 위반이면 코드가 틀린 것이다.
  예: `D/ue_sdk.md` §4.1의 모듈 트리와 달리 실제 `sdk/core/src/`에는 `sip/`·`media/`·`domain/` 디렉터리가 없고 SIP·미디어는 `K/engine.cpp`에 있다.
  이 차이는 문서 갱신 검토의 근거이며, 문서 모양에 맞추기 위한 코드 재배치의 자동 근거가 아니다.
- **C. 책임 경계 오류**: 단말에서 풀 일을 서버에서 풀거나 그 반대라면 책임을 옮길 위치와 필요한 계약 변경을 제안한다.
  앱 수정으로 보이더라도 서버 계약 변경이 필요하면 반드시 이 판정으로 드러낸다. 먼저 기존 서버 구현으로 충족되는지 확인한다.
  선례: 다중 그룹 동시 발언은 단말 팬아웃(서버 변경 없음, `D/dispatch_desktop_ui.md` §13); 타인 사설콜·애드혹 관측은 서버 계약(`D/dispatch_center.md` §5.6a)이다.
  후자의 서버 경로는 이미 구현돼 있으며(`csp/GroupCallService.cpp:2091`·`:2120`, `CanObserveEphemeral`·`BuildPttDialogExt`), UI 문서 §13의 잔여는 **앱 과제**다.
- **D. 규격 공백**: 3GPP/RFC에 해당 절차가 없다면 조사한 범위를 밝히고 체계성·기존 계약과의 일관성으로 제안한다.
  미확인을 규격 공백으로 단정하지 않는다. 단말/서버 중 어느 쪽이 책임질지와 기존 계약에 미치는 영향을 명시한다.
- 모든 판정에서 **수정 대상 = 단말 코드 / 서버 코드 / 문서 / 이들의 조합**을 명시한다. 코드 대상은 SDK·앱 또는 서버 파일까지 좁힌다.

## 5. 리뷰 산출물

- 지적 하나마다 다음 형식을 쓴다. 심각도 높은 순으로 제시한다.
  **[심각도][A/B/C/D] 제목 — 판정축 / 근거 / 수정 대상 / 영향·수정 제안**
- 근거는 확인한 **3GPP TS·RFC 조항 번호, 문서 경로·절 번호, 또는 `파일:줄`**로 쓴다. 일관성 논거도 비교할 계약·구현을 특정한다.
- **근거를 못 대는 지적은 내지 않는다.** 취향·추측은 결함 판정으로 올리지 않는다.
- 서버 동작을 단정하려면 실제 구현을 읽고 `파일:줄`을 댄다. 확인하지 못했다면 **미확인**으로 분리하고 필요한 확인 위치를 적는다.
- 심각도: **치명**(보안·데이터 손실·서비스 중단), **높음**(핵심 계약·기능 손상), **보통**(제한된 조건의 오류), **낮음**(동작 영향 없는 정합 문제). 실제 영향으로 정한다.
- 끝에 확인한 계약·검증 결과·미확인 범위를 짧게 적는다. 미실행·SKIP·BLOCKED를 PASS로 쓰지 않는다.

## 6. 금지 사항

- “기존 코드 최소 변경”을 이유로 규격·체계성을 희생하는 band-aid를 우선 권고하지 않는다.
- 정본·시안에 없는 것을 일관성·정돈만을 이유로 지어내거나 지우지 않는다. 필요한 변경은 근거와 함께 제안한다.
- 문서에 변경 이력·작성일·버전 헤더·Phase 완료 로그를 추가하지 않는다. 최종 상태만 쓰며 미구현·향후 과제는 보존한다. 이력은 git에 둔다.
- 생성물은 손수정하지 않고 원천 정의와 생성기를 수정·실행한다. floor는 `docs/design/features/mcptt_floor_defs.yaml`을 고치고 `scripts/gen_floor_defs.py`로 `floor_defs.h`를 생성한다.
- CMP·Kotlin·probe는 생성물이 아니라 대조 대상이다. 정본과 정합시키고 `python scripts/gen_floor_defs.py --check`로 네 곳의 일치를 확인한다.

## 7. 이식·검증 원칙

- Windows `windows/dispatch-desktop/`(WPF/C#)에서 Android `android/dispatch-tablet`로 이식한다. 순서는 `D/ue_sdk.md` §10의 **B→C→D→E**다.
- `sdk/android`의 cimsue AAR(SWIG + Kotlin 파사드)를 먼저 채우고 앱을 올린다. 기존 Android 이전 경계는 같은 문서 §5.3을 따른다.
- 현재 `sdk/android/`에는 `build-native.sh`만 있고 태블릿 앱 경로는 아직 없다. 하위 지침은 해당 구현을 만들 때 별도로 둔다.
- 앱의 CIMS 연동은 SDK 파사드만 통한다. Windows는 `CimsUe.dll`, Android는 `com.cims.ue.sdk.*` 공개면만 사용하며 앱에서 `com.cims.ue.sdk.jni.*`·`org.pjsip.*`를 쓰지 않는다.
- **플랫폼 접점은 파사드와 분리된 별도 층**이다. `sdk/windows/dotnet/CimsUe/Platform/`의 장치·입력·수명주기·자격 저장 책임을 `D/ue_sdk.md` §5.1의 Android 접점으로 옮긴다. 이 층은 프로토콜을 모르며, 프로토콜 로직이 새면 층 경계 위반으로 판정 C에 연결한다.
- 오디오 이중 출력은 `D/ue_sdk.md` §6.3과 `D/dispatch_desktop_ui.md` §7을 함께 확인한다.
- PTT 입력원은 Windows 전역 핫키와 Android 측면 하드키로 다르지만 누름·해제의 동작 의미는 같다(`D/dispatch_desktop_ui.md` §8).
- WPF 도킹·별창 구조를 그대로 이식하지 않는다. 태블릿은 `D/dispatch_desktop_ui.md` §12, 공통 화면 의미론은 §3~§4를 따른다. **포커스 ≠ 발언 대상**은 플랫폼이 달라도 같다.
- 응답 코드의 화면 문구는 같은 문서 §9 사전을 따른다. 임의로 새로 짓지 않고 사전 보완이 필요하면 문서 변경을 제안한다.
- MCData SDS(행 15)와 SIP MESSAGE `text/plain` SMS·LMS(행 16)는 별도 계약으로 리뷰한다. 외부망 SMS/LMS 게이트웨이는 UI 문서 §13의 미구성·후속 범위다.
- 동작·인터페이스·설정 변경은 코드와 해당 정본 문서를 **같은 변경에서** 갱신한다. 이식 원본 구조는 `W/DispatchDesktop.csproj`와 UI 문서 §11을 함께 본다.
- 검증은 `docs/VERIFICATION_PROCESS.md` §0.1·§1·§2의 **S1~S6 게이트**에서 최소 관련 stage를 확인한다. SDK·실기기 검증은 `D/ue_sdk.md` §9, 서버 관제 검증은 `D/dispatch_center.md` §9를 따른다.
- 미해결 설계는 `D/ue_sdk.md` §11, `D/dispatch_desktop_ui.md` §13, `D/dispatch_center.md` §10을 확인한다. 미해결 항목을 완료된 기능으로 가정하지 않는다.
