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
| 앱 실기 e2e(그룹 생성→disp02 자동 갱신→편집/삭제/오류 문구) | **미실시 — 사용자 실기** | 후속 요청서 §3 체크리스트 |
| 활성 세션 발견(서버 P2 — `/provisioning/me` `dispatch.members[]{…,groupId}/pttTargets[]/etag`, 응답 `ETag`/304) | **서버 구현(csc 0.2.104)**. 앱 반영: `DispatchMember.GroupId`(SDK·C API·파사드), `Directory.WatchTargets`(감시 = 전원) / `Members`(③ 띠 = `groupId == dispatch.groupId`), 60초 재조회 `RefreshDispatchAsync`(If-None-Match 304, 변경 시 dialog watch·conference 구독 집합 재적용) — **실기 미확인** | [android_ue_provisioning.md §3](../design/features/android_ue_provisioning.md), `DispatchSession.RefreshDispatchAsync` |
| 청취 범위 conference 구독 인가(서버 P3 — 403 + Warning 138, 브로드캐스트 480 + 105) | **서버 구현(csp, 다음 릴리스 + DB 마이그레이션)**. 앱: 구독 1회·재시도 없음, 범위에서 빠진 청취 그룹은 구독 해제 — 403/480 문구 = `Area.PttListen` | [dispatch_center.md §5.6](../design/features/dispatch_center.md) |
| 통합 이력·메시지 모니터링(서버 P3b — `GET /provisioning/history?kind=call\|ptt\|message&since&limit`, ETag/304, 403 `no_monitor_scope`) | **서버 구현(csc 0.2.104)**. 앱 `Services/HistoryClient`(탐침→404/501/403 이면 꺼짐, kind 별 `next` 커서·ETag, 2.5초) + `DispatchSession.OnHistory`(②④ 내역 행) — 계약 §3-2 와 대조 완료(items/next/event 이름표 일치), **실기 미확인**. 1:1 SDS 는 CSP `Setup.McData.StoreOneToOneSds` 필요 | [android_ue_provisioning.md §3-2](../design/features/android_ue_provisioning.md) |
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

## 3. 남은 일 (우선순위 순)

1. **앱 실기 e2e(그룹 CRUD)** — 후속 요청서 §3 의 7단계. 앱 층만 본다(서버는 CLI 로 같은 사이클 통과). 실패 시 로그 줄로 층 판정 → 수정 → 재빌드.
   비밀번호 입력이 필요해 Claude 가 대신 못 한다 — 사용자가 로그인한 뒤 로그 판정은 Claude 에게 맡겨도 된다.
2. **P2 실기 확인** — 로그인 로그 `members=N pttTargets=M` 이 0 이 아닌지(관제 그룹 `dg-dispatch01` 의 `monitor_scope`/`ptt_listen` 이 `none` 이면 빈 배열 —
   콘솔 `구성 > 관제 그룹` 에서 manager 로 `all`/`listed` 로 올려야 한다), ③ 띠 = 자기 그룹원만·감시(dialog)는 전원, ② PTT 내역에 "청취 범위" 행이 로스터 NOTIFY 로 뜨는지,
   콘솔에서 편성을 바꾸면 60초 안에 `dispatch discovery changed` 로그 + 토스트가 뜨는지. `directory.sample.csv` 의 `member` 태그 행은 서버 목록이 확인되면 정리.
3. **P3 실기 확인** — csp 다음 릴리스 배포 후: 범위 밖 그룹 conference 구독이 403(Warning 138)/480(105) 이면 문구만 뜨고 재시도 루프가 없는지.
4. **P3b 실기 확인** — 로그 `history: …` 탐침 결과. 403 `no_monitor_scope` 면 관제 그룹 범위부터(2번). 항목이 ②④ 내역에 붙는지, 메시지(kind=message)는 CSP
   `Setup.McData.StoreOneToOneSds` 설정 여부에 따라 1:1 유무가 갈린다. 메시지 모니터링 전용 UI(별도 패널)는 아직 없다 — 내역 행으로만 표시(설계 §13 후속).
5. (정리) C API `allow_group_creation` → `allow_create_group` rename 은 ABI 변경 — 파사드·`AbiLayoutTests`·문서 동시 변경일 때만.
6. (정리) 후속/요청서 `docs/dev/*_request_*.md` 는 양쪽 반영이 끝나면 삭제하고 설계 정본에만 최종 상태를 남긴다. 이 파일도 같다.
7. 미해결(이전부터): ③ 대기열에 "내게 온 전화"가 뜨는 현상 — 서버가 대표번호 dialog 를 포크 leg 마다 내던 것(요청서 §6, 서버 `e3d08d8c` 보완) + 앱 leg 병합(`30feccda`) 이후 재현 여부 확인.

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
  · 서버 `d725ae73`(P2·P3·P3b)·`e3d08d8c`(요청서 §6 결함 3건) · (다음) 앱 P2 반영 — `DispatchMember.GroupId`·`WatchTargets`·`RefreshDispatchAsync`.
