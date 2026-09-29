# 관제 앱(Windows) 이어서 할 일 — 개인/임시 발신 분리 · 명칭 · 일제 통화 한 버튼

사용자 요청(관제 앱 실사용 피드백)과 지금까지 확인한 사실·설계를 모은 인계 문서다. Claude Code 터미널에서 이 문서를 읽고
§3 순서대로 이어서 한다. **원칙: VoLTE·MCPTT 규격 절을 먼저 확인하고 그대로 따른다(CLAUDE.md 설계 우선순위 1).**

## 1. 사용자 요청

1. "PTT 메시지 파일 전송 · 여러 그룹 지정 그룹통화(다중 채널 동시 발언) · 일제 통화가 관제 앱에 아직 반영되지 않은 것 같다" — 확인해 달라.
2. "사설콜과 애드혹이 모두 애드혹으로 동작한다" — 고쳐 달라.
3. 명칭: **사설콜 → 개인**, **애드혹 → 임시**.
4. 일제 통화를 **버튼 하나로 한 번에**(누르면 개시와 동시에 말하고, 놓으면 끝).

## 2. 확인 결과

### 2.1 요청 1 — 기능은 들어가 있다, 실행 중인 앱이 옛 빌드다

- 실행 중인 관제 앱 = `build-win\dist\CimsDispatch-0.1.0-win-x64`(오후 1:19 빌드, f54a5968 시점) — 파일 전송·다중 채널(d5198040)·
  일제 통화 U6(349fac97)·단말 결함 수정(bc2e499d)이 모두 **그 뒤**다. 새 패키지는 `build-win\dist-next\CimsDispatch-0.1.0-win-x64`(오후 3:09).
  앱이 `dist` 를 잡고 있어 덮어쓰지 못했다 → **앱을 닫고** `publish.ps1`(기본 OutDir = `build-win\dist`)로 다시 게시하거나 `dist-next` 의
  `CimsDispatch-run.cmd` 로 실행.
- 새 빌드에서 기능 위치(화면에서 찾기 어렵다는 피드백이면 UI 과제로 추가):
  - 파일: ④ PTT 메시지 입력 줄 왼쪽 [📎](다중 선택) · 말풍선 영역에 끌어 놓기 · 받은 파일 [↓ 받기](다운로드\CIMS).
    그룹 FD 는 그룹의 `allow_fd` 가 켜져 있어야 한다(아니면 403 "이 그룹은 파일 전송이 꺼져 있습니다").
  - 다중 채널: ① 카드 왼쪽 **체크**를 여러 개 → PTT 한 번에 체크된 채널 전부 floor 요청(칩별 승인/대기/거부).
  - 일제 통화: ① **포커스 카드(3줄)** 의 [일제 통화] — 멤버 편성 그룹·진행 중 세션 없음일 때만 활성. 누르면 세션만 열리고,
    PTT 를 눌러 말한 뒤 놓으면 호가 끝난다(두 단계 — 요청 4 가 이것을 한 버튼으로 바꾸는 것).

### 2.2 요청 2 — 원인: 발신 팝오버가 두 모드에서 같은 목록·같은 조작을 쓴다

- `windows/dispatch-desktop/Views/PttOriginateView.xaml` 사용자 행(~129-151)에 모드와 무관하게 [싱글][멀티][문자] + **애드혹 체크박스**가 함께 있다.
- 체크박스 → `PttOriginateViewModel.ToggleAdhoc`(~198)가 **`Mode = "adhoc"` 로 조용히 바꾼다** → 상단 버튼이 "애드혹 발신 (1명)"이 되고
  `StartAdhoc` 로 나간다. [사설콜 ▾]로 열어 사람을 체크해 고르면 임시 그룹 통화가 된다.
- "싱글/멀티" 표기는 반이중/전이중인데 뜻이 드러나지 않는다.
- 앱의 발신 분기 자체는 맞다(`Start` → `StartPrivateCall` / `StartAdhoc`, `DispatchSession.cs` ~1296·~1303), 코어도 사설콜에 `privateCall` 을 싣는다
  (`sdk/core/src/engine.cpp` `projectMcptt`). 세션 판정 `SessionKinds.Of`(`Models/Sessions.cs`)도 맞다.
- 관련 사람 메뉴·Ctrl+K: `Shell/MainWindow.xaml` ~292·~337([사설콜][애드혹에 추가]), `PersonActionsViewModel`.

### 2.3 요청 4 — 규격 근거(TS 24.380 V18.8.0 · TS 24.379 V18.14.0 원문 재확인)

- **§14.3.5 `mc_implicit_request`** — 클라이언트가 SDP **offer** 에 넣는다. MCPTT 서버는 최초 INVITE 를 **암묵적 발언 요청**으로 받는다
  — 단, chat 그룹 호 합류·진행 중 prearranged/adhoc 호 합류는 제외. 서버는 응답에도 이 속성을 싣는다.
  → 일제 통화 개시(= 새 prearranged 호)는 암묵적 요청이 적용된다.
- **§14.3.4 `mc_granted`** — controlling function 이 암묵적 요청을 200 OK 로 승인할 때 SDP **answer** 에 싣는다(선택 "may"). 승인하지 않으면 싣지 않는다.
  temporary group 세션에서는 200 OK 로 승인을 알리지 않는다. 승인해도 Floor Granted 메시지는 따로 보낸다(§6.3.4.4.2 1. — 예외 없음).
- **offer 의 `mc_granted` 는 요청이 아니라 능력**이다 — §14.2.4 "*shall include … in the SDP offer of an initial SIP INVITE request when it is acceptable for
  the MCPTT client to receive a granted indication in the SIP 200 (OK) response*", §12.1.2.2 NOTE 2 "*does not indicate an actual request for the floor*".
  그래서 "옛 단말의 offer `mc_granted` 를 한 릴리스 요청으로 받는다"는 전환기는 두지 않는다(규격대로 싣는 단말이 요청 없이 발언권을 받는다).
- 단말 절차: 호 성립 때 암묵 요청이면 T101 + 'U: pending Request'(§6.2.4.2.2 4.), answer 의 `mc_granted` 또는 Floor Granted 로 'U: has permission'(§6.2.4.4.2),
  그 뒤 Floor Granted 가 또 와도 머문다(§6.2.4.5.5). 이어지는 offer 에는 `mc_granted` 금지(§14.5).
- **진행 중 그룹 호는 일제 통화로 바꿀 수 없다** — TS 24.379 §10.1.1.3.1.1 15)(진행 중)는 합류이고 broadcast 단계가 없다(긴급은 15)f) 로 격상 가능).
- **임시 그룹(ad hoc) 일제 통화**는 규격에 있다(TS 24.379 §17.2.2.1.1 9)) — CIMS 미구현(CSP 가 ad hoc 의 broadcast-ind 무시).
- 구현 상태: **SDK 반영**(`GroupCallOptions.implicitFloorRequest` — offer `mc_queueing;mc_implicit_request;mc_granted`, answer 판정, 호 성립 전 놓음 처리 —
  정본 `mcptt_broadcast_group_call.md` R14·U7). **CSP 편차는 .48 로 넘김**(`server45_handoff.md` §8 — offer `mc_granted` 를 요청으로 읽음,
  `mc_implicit_request` 미해석, answer 에 되돌리지 않음). SDK 가 두 속성을 함께 실어 지금 CSP 에서도 동작한다. CMP 는 바꿀 것 없음.
- 발언 해제 뒤 호 종료는 이미 구현(개시 단말: pending Release 중 B-bit Floor Idle → BYE, §6.2.4.6.4 · 짧은 탭의 늦은 Granted 무시 §6.2.4.6.8 — bc2e499d).

## 3. 할 일(권장 순서)

### 3.1 발신 팝오버 모드 분리 + 명칭(개인/임시) — 앱만 · **반영함**

- 반영: 모드 = 세그먼트·① 버튼·사람 메뉴로만(행 조작·패드·빠른 발신 Enter 는 안 바꿈), 개인 행 [반이중][전이중][문자]·행 클릭 = 대상 선택·[개인 통화 발신],
  임시 행 ☐+[문자]·행 클릭 = 체크, 다이얼패드는 개인 모드만, 행 [전이중] 이 팝오버 선택을 바꾸던 부작용 제거. 화면 문자열 전부 개인/임시, 문서
  `dispatch_desktop_ui.md`·CLAUDE.md 개요. 렌더 점검 스위치 `--ui-preview-popover=private|adhoc`. 남은 것 = 태블릿 명칭(사용자 확인), 실기.

- **모드는 명시 조작으로만 바뀐다**: 세그먼트 [개인|임시]·① 버튼 [개인 ▾]/[임시 ▾]·사람 메뉴 [임시 그룹에 추가]. 행 조작이 모드를 바꾸지 않는다.
- 개인 모드: 사용자 행 = [반이중 발신][전이중 발신](또는 발신 1개 + 위 반이중/전이중 선택) + [문자], **체크박스 숨김**. 행 클릭 = 대상 선택.
- 임시 모드: 사용자 행 = 체크박스(행 클릭 = 토글) + [문자], 반이중/전이중 버튼 숨김. 상단 [임시 그룹 발신 (n명)].
- 명칭(화면 문자열만 — 내부 식별자 `PttPrivate`/`PttAdhoc`·`adhoc-` 그룹 id·`Operation` 은 그대로): 사설콜 → **개인**(개인 통화, TS 24.379 private call),
  애드혹 → **임시**(임시 그룹 통화, ad hoc group call). 대상 파일(grep "사설콜|애드혹"):
  `Views/PttChannelsPanel.xaml`(빠른 발신 줄 버튼·툴팁·힌트·카드 배지 ~166) · `Views/PttOriginateView.xaml` · `ViewModels/PttOriginateViewModel.cs`(StartText 등) ·
  `ViewModels/PttChannelsViewModel.cs`(Title "애드혹 · …", Badge "사설콜") · `Services/DispatchSession.cs`(⑤ 활동 문구 ~910·~969, 배너 "PTT 사설콜 착신" ~853,
  이력 라벨 ~523·~925) · `Models/Activity.cs` · `Services/ResponseText.cs`(Area.PttPrivate/PttAdhoc 문구) · `Shell/MainWindow.xaml`(Ctrl+K·사람 메뉴) ·
  `Shell/SettingsWindow.xaml` · `ViewModels/PersonActionsViewModel.cs` · `ViewModels/ScopedChannelsViewModel.cs`(OthersHint) · `Views/ScopedChannelsPanel.xaml`.
- 문서: `docs/design/features/dispatch_desktop_ui.md`(§2 작업표·§4.1 발신 팝오버·사람 메뉴·§9 문구 표) — 태블릿(`android_dispatch_tablet.md`)도 같은 명칭으로 맞출지 사용자 확인.

### 3.2 일제 통화 한 버튼(누르고 있는 동안 개시+발언, 놓으면 종료) — SDK·CSP·앱 · **SDK·앱 반영 · CSP 인계**

- 사용자 결정: 알림 = 그룹 이름(그룹 문서에 일제 표시 요소 없음) · 버튼 = 해당 그룹 카드의 [일제 통화](진행 중 통화 없는 멤버 편성 그룹만 활성) · 대상 = 그 그룹 하나.
- 반영: `PttChannelsViewModel.BroadcastDown/Up`(잠금 발언 = 클릭 토글, GMS 조회 중 놓으면 개시 직후 끝) · `DispatchSession.BroadcastCallAsync`(ImplicitFloorRequest)·
  `ReleaseBroadcast`(성립 전 CANCEL / 뒤 Floor Release) · 버튼 `IsBroadcastHeld` 로 활성 유지 · 오경고 판정에서 200 OK 승인(RawType -1) 제외. 남은 것 = 실기, 태블릿.

1. **SDK**: `GroupCallOptions.implicitFloorRequest`(types.h) → `floorSdp` 에 `mc_implicit_request` 추가 · answer 의 `mc_granted`(또는 이어 오는 Floor Granted)로
   Speaking·마이크 결선 · 호가 서기 전에 놓으면(200 전) CANCEL/BYE, 선 뒤 승인 전이면 Floor Release(§6.2.4.6.8 경로가 늦은 Granted 를 흡수) ·
   C API(`cimsue_group_call_options_t` 필드 추가 → struct id·ABI 시험)·.NET(`GroupCallOptions.ImplicitFloorRequest`)·SWIG. 시험: SDP 조립·answer 파싱.
2. **CSP**: offer 의 `mc_implicit_request` 를 PTT_JOIN `granted` 로(§14.3.5 — 새 호 개시일 때만, 합류·chat 제외), 전환기로 offer `mc_granted` 도 받음,
   승인 시 200 OK answer 에 `mc_granted`(§14.3.4). `GroupCallService.cpp` fmtp 파싱·answer 조립. 정본 `mcptt_standard_conformance.md`·`ptt_flows.md` 갱신.
3. **CMP**: 변경 없을 가능성(개시자 `grantInitialFloor` 통과) — 확인만.
4. **관제 앱**: 포커스 카드 [일제 통화]를 **누르고 있는 버튼**(press = `JoinGroupCall(Broadcast, ImplicitFloorRequest)` + 발언 대상 단일화,
   release = Floor Release → Idle(B) → 코어 BYE). 잠금 발언 설정이면 클릭 토글. 전역 핫키 후보(설정 `HotKeys` 에 "broadcast"). 진행 중 세션·chat 그룹은
   지금처럼 비활성(`BroadcastCallAsync` 의 판정 재사용). 문서 `dispatch_desktop_ui.md` §4.1 일제 통화 절·`mcptt_broadcast_group_call.md` §4.4 U6.
5. **확인 필요(사용자)**: 버튼 위치(포커스 카드 3줄 유지 vs ① 발언 바 옆 [일제] 상시) · 대상 그룹 고르는 방법(포커스 카드 = 대상).

### 3.3 요청 1 후속

- 앱을 닫고 `dist` 재게시 후 실사용 재확인. 기능이 있는데 찾기 어렵다면 UI 과제로(예: ④ 머리에 📎 안내, 체크 다중 선택 안내 문구).

## 4. 빌드·시험 절차(이 PC)

```
cmake --build build-win --config Release --parallel 8          # C:\Program Files\CMake\bin\cmake.exe (PATH 에 없을 수 있음)
build-win\bin\Release\cimsue_test.exe --gtest_brief=1          # 기준 46건
dotnet test sdk\windows\dotnet\CimsUe.Tests -c Release -p:Deterministic=false   # 기준 63건 (SAC 가 결정적 빌드 해시를 막으면 Deterministic=false)
dotnet build windows\dispatch-desktop\DispatchDesktop.csproj -c Release -o <scratch>\app -p:Deterministic=false
dotnet <scratch>\app\CimsDispatch.dll --ui-preview --ui-preview-canvas --ui-preview-shot=<png>   # 렌더 확인(끝나면 새로 뜬 dotnet 프로세스 정리)
powershell -ExecutionPolicy Bypass -File windows\dispatch-desktop\publish.ps1 [-OutDir build-win\dist-next]
```

- `ext/pjproject` 가 바뀌면 `build-win\pjproject-prefix\src\pjproject-stamp\Release\pjproject-{build,install,done}` 을 지우고 빌드(EP 가 스스로 재빌드하지 않음).
- Android 코드(`android/ptt-client` `FloorClient.kt`·`android/core` `SipController.kt`, bc2e499d)는 이 PC 에서 빌드 불가 — Android 빌드 환경에서 확인.
- 서버(CSP·CMP·CSC)는 .48 이 소스 서버 — 서버 몫은 .48 에서 반영·배포(인계는 `docs/dev/server45_handoff.md` 형식).

## 5. Android 관제 태블릿 — Android 빌드 환경에서 확인할 것

Windows 와 같은 변경을 `android/dispatch-tablet` 에 코드로 반영했다. 이 PC 는 Android 를 빌드하지 못해 **컴파일·JVM 시험·실기가 안 됐다.**

| # | 확인 | 파일 |
|---|---|---|
| T1 | 컴파일 — `startBroadcast`·`startAdhocBroadcast`·`releaseBroadcast`·`joinAdhoc`(`GroupCallOptions.copy`)·`BROADCAST_JUDGE_OPS` | `session/PttPlane.kt` |
| T2 | `SessionItem.isBroadcast/isBroadcastInitiator`·`FLOOR_IND_BROADCAST`·`GroupInfo.sessionType`·`broadcastPending` | `session/Models.kt`·`DispatchSession.kt` |
| T3 | VM 일제 통화 상태(`_broadcastHeld`·`bcCallId` 는 `cards` 의 Eagerly onEach 가 읽으므로 **그보다 먼저 선언**해 두었다)·`canBroadcast`·`canCheck`(Permission 0 제외) | `ui/ptt/PttChannelsViewModel.kt` |
| T4 | 한 버튼 제스처(`BroadcastHoldButton` — `rememberUpdatedState`, 개시 뒤 `joined` 가 돼도 뗄 때까지 남는지)·발신 시트 [임시] 의 [일제 통화](누르는 동안 시트·탭·대상 잠김, 놓으면 닫힘, 실패면 남음)·씨앗(`LaunchedEffect(seed)`) | `ui/ptt/ChannelScreen.kt`·`OriginateSheet.kt` |
| T5 | JVM 시험 `PttChannelTest` — 일제 통화 3건(`canBroadcast` 조건·수신 멤버 발언 대상 제외·B-bit 판정) + 기존 전부 | `src/test/.../PttChannelTest.kt` |
| T6 | 실기 — 채널 머리 [일제 통화] 누름·뗌(성립 전 CANCEL / 뒤 BYE), 잠금 발언 토글, 진행 중 통화·채팅 그룹 비활성, 수신 단말 «일제»·발언 대상 불가, [임시] 일제 통화(서버 §9 전에는 «일제 통화로 열리지 않았습니다» 가 정상) | — |

명칭(개인/임시)은 Windows 와 같다. 명칭이 바뀌면 두 앱·두 문서를 함께 바꾼다.
