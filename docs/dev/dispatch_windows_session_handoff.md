> 세션 인수인계(단말/Windows 작업 — 2026-09-06 기준). 다른 Claude Code 세션(CLI 포함)이 이 파일만 읽고 이어서 작업한다.
> 설계 정본은 [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md)·[ue_sdk.md](../design/features/ue_sdk.md)·
> [mcptt_api.md](../api/mcptt_api.md) §2 — 여기엔 **현재 상태·남은 일·절차**만 둔다. 작업이 끝나면 이 파일은 삭제한다.

# 관제조작반(Windows) 세션 인수인계 — PTT 그룹 CRUD · 활성 세션 모니터링 · 감청

## 0. 이어서 시작하는 법

```bash
cd C:\work\cims
git pull --ff-only
claude
```

첫 지시 예: "docs/dev/dispatch_windows_session_handoff.md 읽고 §3 남은 일부터 이어서 진행". 자동 메모리(`~/.claude/projects/C--work-cims/memory/`)는
데스크톱 앱과 CLI 가 같은 것을 쓴다 — `dispatch-desktop-ui-status.md`·`windows-dev-environment.md` 가 같은 내용을 요약한다.
이전 세션 대화 자체를 잇고 싶으면 `claude --resume` 로 "PTT 그룹 CRUD" 세션을 고른다.

## 1. 지금 상태 (한눈에)

| 층 | 상태 | 근거 |
|---|---|---|
| 서버 CSC — GMS 그룹 CRUD(XCAP PUT/DELETE, 자격 `allow_create_group`, `is_owner`, 오류 본문 `error`) | **구현·배포·실측 완료** (csc 0.2.103, 커밋 `9bf59d26`) | [mcptt_api.md §2](../api/mcptt_api.md), [windows_request_dispatch_group_crud_followup.md](windows_request_dispatch_group_crud_followup.md) §0 |
| SDK 코어 `sdk/core` — `GroupDoc`(XML 직렬화/파서), `CscClient.getGroup/putGroup/deleteGroup`, `Profile.allowGroupCreation`, `dispatch.members[]/pttTargets[]` 파싱 | 완료, 단위시험 통과 | `sdk/core/include/cimsue/csc.h`, `src/csc/group_doc.cpp`, `test/csc_test.cpp` |
| C API / .NET 파사드 — 구조체 4개(`dispatch_member/target`, `group_member/doc`)·`cimsue_csc_get/put/delete_group`, `CscClient.GetGroup/PutGroup/DeleteGroup` | 완료, ABI 시험 통과(56건) | `cimsue_c.h`, `c_api.cpp`, `sdk/windows/dotnet/CimsUe/CscClient.cs` |
| 앱 `windows/dispatch-desktop` — [그룹] 탭 [새 그룹]/[↻]/[편집]/[삭제], `GroupEditWindow`, `RefreshGroupsAsync`(차분 갱신·청취 범위 그룹 구독), xcap-diff 자동 재조회, 오류 문구·409 재시도·412 재조회, 서버 그룹원 우선(`Directory.SetMembers`) | 완료, Release 빌드 통과 | `Services/DispatchSession.cs`, `ViewModels/GroupEditViewModel.cs`, `Shell/GroupEditWindow.xaml`, `Services/ResponseText.cs` |
| 앱 실기 e2e(그룹 생성→disp02 자동 갱신→삭제, 대표번호 착신/응답/부재/동료 응답, PTT 청취, SDS 양방향) | **UI 자동화로 실기 완료(09-06 22시)** — §3-2. 남은 실기 = 오디오 실청취·핫키·콘솔 편성 변경(사용자) | §3-2 |
| 활성 세션 발견(서버 P2 — `/provisioning/me` `dispatch.members[]{…,groupId}/pttTargets[]/etag`, 응답 `ETag`/304) | **서버 구현(csc 0.2.104)**. 앱 반영: `DispatchMember.GroupId`(SDK·C API·파사드), `Directory.WatchTargets`(감시 = 전원) / `Members`(③ 띠 = `groupId == dispatch.groupId`), 60초 재조회 `RefreshDispatchAsync`(If-None-Match 304, 변경 시 dialog watch·conference 구독 집합 재적용) — 실기 통과(304 조용·그룹 수 변동 감지) | [android_ue_provisioning.md §3](../design/features/android_ue_provisioning.md), `DispatchSession.RefreshDispatchAsync` |
| 청취 범위 conference 구독 인가(서버 P3 — 403 + Warning 138, 브로드캐스트 480 + 105) | **서버 구현(csp, 다음 릴리스 + DB 마이그레이션)**. 앱: 구독 1회·재시도 없음, 범위에서 빠진 청취 그룹은 구독 해제 — 403/480 문구 = `Area.PttListen` | [dispatch_center.md §5.6](../design/features/dispatch_center.md) |
| 통합 이력·메시지 모니터링(서버 P3b — `GET /provisioning/history?kind=call\|ptt\|message&since&limit`, ETag/304, 403 `no_monitor_scope`) | **서버 구현(csc 0.2.104)**. 앱 `Services/HistoryClient`(탐침→404/501/403 이면 꺼짐, kind 별 `next` 커서·ETag, 2.5초) + `DispatchSession.OnHistory`(②④ 내역 행) — 계약 §3-2 와 대조 완료(items/next/event 이름표 일치), 실기 통과(타인 세션·통화 행 유입, 대표번호 항목은 dialog 정본이라 건너뜀). 1:1 SDS 는 CSP `Setup.McData.StoreOneToOneSds` 필요 | [android_ue_provisioning.md §3-2](../design/features/android_ue_provisioning.md) |
| 감청 창 화자 레벨 미터(U10 관측 API) | SDK/엔진 과제, 미착수 | ue_sdk.md §11 |

관제사 계정: disp01/disp02 (pw 1234, 개발 서버 121.161.164.45, CSC 4430 — 앱은 인증서 검증 끔). VoLTE `+821310001001/2`(내선 1001/1002), 대표번호 `+821310001000`,
PTT `+82510001001/2`, 관제 그룹 `dg-dispatch01`, 멤버 그룹 `g002`. 두 계정 모두 `allowCreateGroup=true` 부여됨.

## 2. 빌드·실행·시험 절차 (Windows, 이 PC)

```bash
# 1) 네이티브 SDK (cimsue.dll·cimsue-cli·cimsue_test) + sdk/bin 스테이징 — PowerShell 에선 -m:8 이 MSB1031 → /m:8
& "C:\Program Files\CMake\bin\cmake.exe" --build C:\work\cims\build-win --config Release --target cimsue cimsue-cli cimsue_test sdk-layout -- /m:8 /v:m /nologo
C:\work\cims\build-win\bin\Release\cimsue_test.exe --gtest_filter="Csc.*:GroupDoc.*:CApi.*"
# 2) .NET 파사드 + 시험 (ABI 레이아웃 대조 포함)
dotnet test C:\work\cims\sdk\windows\dotnet\CimsUe.Tests\CimsUe.Tests.csproj -c Release
# 3) 앱 — 실행 중이면 먼저 종료(exe 잠금 + SingleInstance)
Get-Process CimsDispatch -ErrorAction SilentlyContinue | Stop-Process -Force
dotnet build C:\work\cims\windows\dispatch-desktop\DispatchDesktop.csproj -c Release
C:\work\cims\windows\dispatch-desktop\bin\Release\net10.0-windows\CimsDispatch.exe        # 로그인 창. --ui-preview = 서버 없이 화면만
```

- 최초 configure 는 `cmake -S sdk/windows -B build-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake`(이미 돼 있음).
- Antigravity IDE 로 앱 편집: `windows/dispatch-desktop` 폴더 열기, `.vscode/tasks.json`(Ctrl+Shift+B)·`launch.json`(F5, C# 확장 `muhammad-sammy.csharp`).
- 앱 로그 `%APPDATA%\CIMS\dispatch-desktop\logs\app-YYYYMMDD.log` — 판정 줄: `groups N member (M owned), K listen-scope`, `xcap-diff: group document changed`, `roster …`, `dialog watched=…`.
- 서버 쪽 확인용 CLI(로그인 필요): `cimsue-cli --csc-host 121.161.164.45 --csc-port 4430 --no-tls-verify --user disp01 --pw <pw> groups | group-get URI | group-put URI --name N --members tel:..,tel:.. | group-delete URI`.
- Smart App Control 이 새 exe 를 간헐 차단할 수 있다(재빌드·재시도로 풀림, 보안 설정은 사용자 결정).

## 3. 남은 일 (우선순위 순) — 2026-09-06 22시 실측 반영

서버(csc 0.2.104 / csp 0.2.112)가 배포된 뒤 헤드리스 UE(`cimsue-cli`)와 PKCE 스크립트로 앱이 쓰는 경로를 실측했다. **앱 코드 변경 없이 전부 통과**:

| 경로 | 실측 |
|---|---|
| `/provisioning/me` | `dispatch` members 42(범위 all)·ptt_targets g001~g005·관제석 두 명 `group_id=dg-dispatch01`, 응답 `etag`(소문자 헤더) + `If-None-Match` → **304** |
| `/provisioning/history?kind=call\|ptt\|message` | 200 `{items:[],next:"…"}` + etag, `If-None-Match` → 304, 미지 kind 400 `invalid_kind` — 앱 `HistoryClient` 계약과 일치(이력은 아직 0건) |
| VoLTE·PTT 등록(TLS 15061) | 둘 다 200 |
| 대표번호 dialog 감시 | 초기 빈 full 스냅샷 1건(앱은 무시) → disp02 발신 시 **호당 dialog 하나**(early → terminated, 같은 id) — §6-7·8 반영 확인 |
| GMS 그룹 CRUD | 생성 201 → disp02 목록 반영(owner=false) → disp02 삭제 403 `not_group_owner` → 미가입 번호 400 `unknown_member` → 삭제 |
| 청취 범위 그룹 합류(g003 listen-only) | **403** — 원인은 disp01 프로파일 `allow-ambient-listening=false`(서버 계정 설정, 설계 §14.1 "PTT 청취 자격" 미부여). 앱 결함 아님 |

1. ✅ **서버 쪽 처리 완료(09-06 21:30, 서버 세션)** — 원인은 둘이었다. ① 자격: disp01/disp02 `ptt_user_profile.allow_ambient_listening=1`
   부여(프로파일 PUT 200, CSP 사용자 캐시 갱신 확인). ② 범위: CSP 는 청취 INVITE 의 SIP 신원(**PTT 회선** `+82510001001`)으로
   관제 그룹을 찾는데 `dg-dispatch01` 멤버 행은 VoLTE 회선뿐이라 org(`TEAM01`)로 폴백 → 자격을 켜도 `ptt_listen scope` 403 이
   났을 것. 근본 수정 = 관제 그룹을 person 귀속으로 보고 같은 person 의 PTT 회선 `pickup_group` 도 파생(CSC `effective_dispatch_group`
   + `migrate_dispatch_groups.sql` 백필, dispatch_center.md §3.2·§5.6). 라이브 DB 백필 적용·CSP 캐시 재적재 완료. **서버 실측**:
   `cimsue-cli --from-profile ptt group-call g003 --listen-only` → 종전 `denied (allow_ambient_listening=0) → 403` 이
   `no active session → 480`(자격·범위 통과 뒤 활성 세션 없음 = 정상). g003 에 실단말 세션이 있을 때 다시 합류하면 200 이어야 한다.
   CSC 코드(파생 자동화)는 **csc 0.2.105 로 라이브 배포 완료**(21:4x, 정지창 11초, 커밋 `3a10fff0`) — 이후 콘솔에서 관제 그룹 멤버를
   바꾸면 같은 person 의 PTT 회선 `pickup_group` 도 자동 파생된다(파생 회선 직접 편집은 409 실측).
2. ✅ **앱 GUI e2e(2026-09-06 22시, CLI 세션 — UI 자동화로 실기)** — 화면 잠금 상태라 스크린샷 대신 UIA 트리·앱 로그로 판정. 상대 단말 = `cimsue-cli`(disp02).
   - 로그인·프로파일 `members=42 pttTargets=5`, VoLTE/PTT 등록 200, ③ 띠 = 관제1석·관제2석, `groups 1 member (0 owned), 4 listen-scope`, 로스터 5개, `history: available` — 통과
   - **결함 1(엔진, 수정)**: 구독 슬롯 24개(`pjsua_pres.c CIMS_CONF_MAX_SUB`)에 dialog 42 + 대표번호 + conference 5 + xcap-diff 가 넘쳐 **대표번호·conference 구독 전부 실패**
     (`Too many CIMS subscriptions`). → `PJSUA_CIMS_MAX_SUB` 256(config_site 재정의 가능) + 앱은 대표번호를 먼저 구독하고 실패 건수를 토스트. 함께 있던 기동 오류
     "Unable to register dialog event package: already exist" 는 upstream mod-dlg-event 가 먼저 등록한 정상 상황 → 레벨 4 로.
   - 대표번호 착신(disp02 → +821310001000): 대기열 1행, [응답] → 양방향 RTP, 정상 종료 — 통과. 전원 무응답 → 부재 1건 — 통과(아래 결함 2 수정 뒤).
   - **결함 2(앱, 수정)**: 대표번호 부재가 dialog(로컬)와 서버 이력 `call.missed` 양쪽에서 행이 생겨 **부재 2**. 내가 받은 호도 이력 `call.answered/ended`(from=발신자, to=대표번호)
     와 겹쳐 응대가 부풀었다. → 대표번호 호는 dialog 가 정본(`RecordPilotOutcome`: 부재·동료 응답 행), 이력의 대표번호 `call.*` 는 건너뜀, 이력 행 `IsOthers` 는 응대·부재 집계 제외.
   - **결함 3(서버 CSP, 앱 완화)**: 발신자(A-leg)가 BYE 하면 종료 dialog NOTIFY 의 entity/direction/remote 가 confirmed 때와 어긋나고 1001 에는 종료가 안 와 **③ 띠·④ 진행 중에
     "통화 중" 잔류**. version 도 단조 증가 아님(→ 스테일 구독의 NOTIFY 가 섞인 것, §3-3). 서버 수정 csp 0.2.113(§3-3). 앱은 같은 dialog id 의 행 전부를 종료해 잔류를 없앰.
   - 동료 응답(앱이 대표번호 발신 → disp02 `answer`): ④ "착신 대표 ← 관제1석 · 응답 관제2석 · 00:12" 1행, 내 쪽 부재 없음 — 통과.
   - PTT 청취(g003, 활성 세션 2명): [청취] → 200, 감청 창 "청취 전용·참가자 2", [청취 종료] 200 — 통과(서버 §3-1 수정 확인).
   - SDS: 앱 → g002 → disp02 수신(✓✓ disposition), disp02 → g002 → 앱 수신 표시 — 통과.
   - 그룹 CRUD(GUI): [새 그룹] → 이름·멤버 2명 → 생성 201 → disp02 목록에 `owner=false` → [삭제] → xcap-diff 재조회로 목록에서 제거 — 통과. 삭제 직후 서버 `pttTargets`(ptt_listen=all)에
     그 그룹이 60초 남아 청취 범위로 재구독되던 창은 그룹 소멸 시 발견 재조회를 당겨서 없앰.
   - 60초 `/provisioning/me` 재조회: 변경 없으면 조용(304), 그룹 수 변동 시 `dispatch discovery changed … pttTargets 6→5` + 토스트 — 통과.
   - 미실시: 콘솔에서 관제 편성 변경(콘솔 접근 없음), 오디오 실청취(잠금 세션), 핫키.
3. ✅ **서버(CSP) — 완료(csp 0.2.113 라이브, S3-SCN-FA F7 PASS)**: A-leg BYE 종료 dialog NOTIFY 의 entity/direction/remote 오귀속은 TAS 가 psip dialog From/To(요청 송신
   입장 저장 — 수신 leg 에서 뒤집힘)를 caller/callee 로 읽은 것이 원인. 당사자 해석을 `CallLegParty`(`CCallMap::ResolveLegParties`)로 단일화(NotifyDialogState·대표번호 종료
   `PilotCall` 고정·초기 full 스냅샷·Replaces/Join 인가) — 규칙은 [dispatch_center.md §4.5](../design/features/dispatch_center.md). **version 비단조는 서버 결함이 아니었다**: 라이브 CSP
   로그(22:15:28 `subs=2` → `Subscription Reaped … notify-failure`)상 세 entity 모두 이전 앱 세션의 스테일 구독(포트 33841)이 하나씩 남아 있었고, 그쪽 NOTIFY(v5/v2/v3)가 현행 등록
   바인딩(34226)으로 배달돼 앱 로그에 섞인 것 — 앱이 481 로 거절해 즉시 회수됐다. 현행 구독의 version 은 1·2·3·4 로 단조. **앱은 version 을 구독(Call-ID) 단위로 비교해야 한다.**
   요청서 `server_request_dispatch_dialog_notify.md` 는 반영 완료로 삭제. 검증 = `S3-SCN-FA` F7(cspsim `-hunt_watch`).
4. (정리) 후속/요청서 `docs/dev/*_request_*.md` 중 양쪽 반영이 끝난 것(group_crud_followup·group_monitoring)은 삭제하고 설계 정본만 남긴다. 이 파일도 3 이 끝나면 같다.
5. (정리) C API `allow_group_creation` → `allow_create_group` rename 은 ABI 변경 — 파사드·`AbiLayoutTests`·문서 동시 변경일 때만.

## 4. 이번에 확정한 계약·규칙 (앱이 지키는 것)

- 그룹 생성 주체 = 관제사(가입자, PKCE 토큰) → GMS XCAP. 관리 API `/api/v1/ptt/groups` 는 콘솔 토큰 전용(앱은 안 씀).
- 새 그룹 uri = `tel:g-<소문자 hex 8>`(클라이언트 명명, `adhoc-`/`priv-` 금지). 목록·응답 문서의 uri 가 정본 — 앱 id 는 uri user part.
- PUT 본문 = GET 문서 포맷(`GroupDoc.toXml`). `<list>` 있으면 멤버 전체 교체. `authorized-user`·floor 정책은 서버/관리 API 몫.
- 편집은 연 시점 ETag 를 `If-Match` 로 → 412 `etag_mismatch` 면 문서 재조회. 409 `uri_taken` 은 id 재생성 1회 재시도. 400 `unknown_member` 는 번호 표시.
- [새 그룹] 노출 = `Profile.AllowGroupCreation`(와이어 `ptt.allowCreateGroup`), [편집]/[삭제] = `GroupSummary.IsOwner`.
- 서버발 변경 = `SubscribeXcapDiff("sip:gms_psi@<PTT 도메인>")` → `MessageReceived(application/xcap-diff+xml)` 에 `org.openmobilealliance.groups` 포함 시 0.5초 합쳐 재조회.
- 그룹원(dialog watch·띠) = 프로비저닝 `dispatch.members[]` 우선, 없으면 CSV `member` 태그. 청취 범위 그룹 = `dispatch.pttTargets[]`(비멤버, conference 구독만).

## 5. 관련 커밋 (main)

- `bdf72d6c` 서버 요청서 + UI §13 결정 · `05bb385b` 단말 파트(SDK·파사드·앱 그룹 CRUD) · 서버 `9bf59d26`·`718b42a0`·`d2cb2ab4` · `93b1aef8` 계약 접점 반영·번호 재키잉
  · `16b4c8a6` 409 재시도 세션 이동·샘플 CSV 재키잉 · `2d50ebc8` HistoryClient 뼈대 · `30feccda`·`e90ff616`·`34264ac8` 앱 결함 검토 보완
  · 서버 `d725ae73`(P2·P3·P3b)·`e3d08d8c`(요청서 §6 결함 3건) · `0c17951e` 앱 P2 반영 · 서버 `3a10fff0`(person 단위 pickup_group 파생, csc 0.2.105)
  · (이번) 엔진 구독 슬롯 256·dialog 패키지 EPKGEXISTS·앱 대표번호 결과 행 정본화·dialog id 종료·이력 IsOthers·그룹 소멸 시 발견 재조회.
