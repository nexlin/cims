# Windows PC — 관제 앱 MCVideo 편성(prearranged) 그룹 영상 열기 · 새 그룹 지연

Claude Code 터미널에서 이 문서를 읽고 §2 부터 순서대로 한다. 원칙은 [dispatch_windows_next.md](dispatch_windows_next.md) 와 같다 — VoLTE·MCPTT·MCVideo
규격 절을 먼저 확인하고 그대로 따른다(CLAUDE.md 설계 우선순위 1). 빌드·시험 명령은 [dispatch_desktop_handoff.md](dispatch_desktop_handoff.md) §4.
설계 정본 = [mcvideo.md](../design/features/mcvideo.md) §5.5, 화면 규약 = [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §10.
SDK·C API·.NET 파사드는 바뀌지 않았다 — DLL 다시 빌드 없이 앱만 고친다.

## 1. 무엇이 문제였나 (.45 실측·코드 확인)

**증상** — 그룹에 MCVideo 를 켰는데 단말의 [영상 보내기] 가 보이기만 하고 눌리지 않는다. 그룹을 만들고 한참 지나야 영상이 된다.

| # | 원인 | 근거 |
|---|---|---|
| 1 | 그룹 «영상테스트»(`g-bb255e56`)의 MCVideo 호 방식이 **편성(prearranged, `mcvideo_group_attrs.invite_members = 1`)**. 편성 그룹은 앱이 호를 열지 않고 초대만 기다리는데, 단말·관제 앱 어느 쪽에도 호를 여는 곳이 없었다 → 영상 호가 생기지 않아 [영상 보내기](호 성립이 조건)가 영원히 꺼져 있다 | DB 조회(10-01) · `PttVideo.kt` `sync` · `DispatchSession.McVideo.cs:128` |
| 2 | CSP 그룹 재적재가 맵을 **비우고 하나씩 다시 채웠다** — 재적재(60초 주기·GROUP_CHANGED 마다, 콘솔은 멤버 추가 한 건마다 GROUP_CHANGED) 동안 MCVideo INVITE 가 그룹을 못 찾아 404 Warning 113, affiliation PUBLISH 는 200 인데 기록이 빠진다 | `csp/DbManager.cpp` `LoadAllGroups` (수정됨 — §4) |
| 3 | 한 번 거절되면 앱이 10 s → 20 → 40 → 80 → 최대 120 s 물러나고, 그룹 문서가 바뀌어도(서버가 그룹을 다시 적재한 뒤 xcap-diff 가 온다) 물러남을 풀지 않았다. 편성 그룹 MCVideo affiliation 은 한 번 보내고 다시 보내지 않았다(PUBLISH 는 200 — 결과는 NOTIFY 로만 온다, TS 24.281 §8.2.2.2.3) | 단말 `PttVideo.kt` · 데스크톱 `_videoRetryAt`·`McVideoAffiliated`·`McVideoType` 한 번만 받음 |

## 2. 단말(ptt-client)에 반영한 것 — 관제 앱이 따라 할 동작

이 문서와 같은 커밋의 `android/ptt-client/src/main/java/com/cims/ue/ptt/PttVideo.kt`(`VideoPlane`)·`ui/VideoViews.kt` 가 기준 구현이다.

- **편성 그룹 영상 열기**(TS 24.281 §9.2.1.2.1.1) — 편성 영상 채널에 영상 호가 없으면 [영상 보내기] 가 눌리고, 누르면
  `joinVideoGroupCall(g, {prearranged = true, implicitTransmissionRequest = true})` = 개시 INVITE 에 송출 요청을 싣는다(16) · §6.4 —
  TS 24.581 §14.2.4 `mc_implicit_request`). 서버(제어 기능)는 MCVideo 로 affiliate 한 멤버를 초대하고 **첫 멤버가 붙으면 200 OK**, 10 s 안에
  아무도 안 붙으면 **480**(§9.2.1.4.2 — mcvideo.md §5.2.1 «INVITE — prearranged 새 세션»). 그사이 다른 멤버가 먼저 열었으면 서버가 합류로 받고
  암묵 요청은 받지 않는다(§14.3.5) — 코어가 곧바로 명시 Transmission Request 로 잇는다(`tc_participant.cpp` `onEstablished`).
- **여는 중 표시** — 코어는 호 성립 전에는 송출 이벤트를 내지 않는다(`Participant::armImplicitRequest` 가 상태만 'U: pending request' 로 둔다).
  앱이 «여는 중…»(카드 «영상 호를 여는 중 — 멤버가 받기를 기다립니다»)을 스스로 그리고, 성립 뒤에는 코어 송출 이벤트(Granted 등)를 따른다.
- **여는 중에 다시 누름** = 개시를 거둔다(hangup → CANCEL).
- **실패는 다시 열지 않는다** — 사용자가 다시 누른다. 480 = «영상 호를 열지 못했습니다 — 영상을 받을 멤버가 없습니다», 그 밖은 응답 코드·사유.
  chat 합류의 자동 재시도(물러남)와 섞지 않는다.
- **그룹 문서가 바뀌면 다시 맞춘다** — 영상 채널이 바뀌었거나 그 그룹 문서가 바뀌었으면(내용 비교 — 같은 문서 재조회로는 풀지 않는다) 그 그룹의
  합류 물러남(실패 횟수·재시도 시각)을 지우고, 편성 그룹이면 MCVideo affiliation PUBLISH 를 다시 보낸다.

## 3. 관제 앱 Windows 데스크톱 — 할 일

### P1. 편성 그룹 영상 열기 (D11 [영상 보내기])

| 자리 | 지금 | 바꿀 것 |
|---|---|---|
| `Services/DispatchSession.McVideo.cs:128`·`:350` `VideoNote` | «편성 영상 그룹 — 멤버가 영상 호를 열면 함께 합류합니다» | 영상 호가 없을 때 «편성 영상 그룹 — [영상 보내기] 로 영상 호를 엽니다»(초대가 오면 지금처럼 자동 수락) |
| `RequestVideoTx(SessionItem s)`(`:209`) | 영상 세션이 있어야 부른다 | 세션 없는 편성 채널용 `OpenVideoTx(GroupInfo g)` — `HasCamera` 확인 → `ptt.JoinVideoGroupCall(g.Id, new VideoGroupCallOptions { Prearranged = true, ImplicitTransmissionRequest = true, Queueing = true })` → `_pendingOps[id] = Operation.VideoJoin`·`_videoJoining` 에 넣되 **«내가 연 호» 표시**(아래 두 줄이 이 표시를 본다) |
| `ViewModels/SidePanelViewModel.cs:181` `CanVideoSend` | `VideoConnected && …` | 편성 채널이고 영상 세션이 없고 `McVideoAffiliated && HasCamera` 이면 참, 내가 연 호가 성립 전이면 참(누르면 `Engine.GetCall(id).Hangup()` = CANCEL) |
| `VideoSendText` | 상태별 문구 | 내가 연 호 성립 전 = «여는 중…» |
| `:349` 영상 호 종료 → `VideoBackoff` | 성립 전 끝나면 물러나 다시 합류 | **내가 연 편성 호가 성립 전 끝나면 물러남 없이** `ResponseText`(Area.Video) 토스트만 — 480 문구는 이미 `ResponseText.cs:91` 에 있다 |

규격 확인: TS 24.281 §9.2.1.2.1.1(개시 절차 — `<preconfigured-group-use-only>` 가 true 면 개시 금지, 그룹 문서에 있으면 따른다) · TS 24.581 §6.2.4.2.2·§14.2.4.

### P2. 그룹 문서가 바뀌면 영상 채널을 다시 맞춘다

| 자리 | 지금 | 바꿀 것 |
|---|---|---|
| `EnsureMcVideoAttrsAsync`(`:87`) | `g.McVideoType.Length > 0` 이면 다시 안 받는다 → 콘솔에서 편성 ↔ chat 을 바꿔도 로그인 전까지 옛 방식 | xcap-diff(`DispatchSession.cs:731` `OnSipMessage` → `RefreshGroupsAsync`) 뒤 MCVideo 그룹 문서를 다시 받아 **호 방식·동시 송출 상한이 바뀌었으면** 갱신 |
| `_videoRetryAt`·`_videoFailures` | 성립(`:158`)·로그아웃(`:175`)에서만 지운다 | 그 그룹 문서가 바뀌었으면 그 그룹 몫을 지우고 `EnsureVideoChannels()` |
| `ApplyMcVideoGroups`(`:77-80`) `McVideoAffiliated` | 한 번 보내면 다시 안 보낸다 | 그 그룹 문서가 바뀌었으면 affiliation 을 다시 싣는다(코어가 관심 그룹 집합을 들고 있으므로 `Affiliate(g, true, McVideo)` 재호출이 곧 집합 재PUBLISH — TS 24.281 §8.2.1.2) |

### P3. 문서

같은 변경에서 [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §10(편성 = 초대 대기 문구 `:740`·`:753`)과 [mcvideo.md](../design/features/mcvideo.md)
§5.5 «관제 앱» 줄(«편성 = MCVideo affiliation + 멤버 초대 자동 수락» 뒤에 «영상 호가 없으면 [영상 보내기] 가 연다»)을 현재 동작으로 고친다.

### P4. Android 태블릿

태블릿은 MCVideo 를 아직 넣지 않았다(Windows 안정화 뒤). 넣을 때 §2 동작을 처음부터 같게 둔다.

## 4. 서버 — CSP 그룹 맵 교체 (이 문서와 같은 커밋)

`CDbManager::LoadAllGroups` 가 그룹을 다 읽은 뒤 `CGroupMap::ReplaceDbGroups` 로 맵을 한 번에 바꾼다(즉석 세션 ephemeral 그룹은 같은 락 안에서 보존 —
옛 `CollectEphemeral` 대체). 빌드·`S1-UNIT-CSP`·`S1-CPP-FORMAT` 통과. **.45 배포는 사용자 지시 뒤**(CSP 재기동 = 단말 재등록) — 배포 전에는 §1 #2 가
남아 있으므로 새 그룹 직후 첫 시도가 404 113 을 받을 수 있다(앱 쪽 P2 가 그 뒤 문서 변경 때 다시 맞춘다).

남은 서버 과제(이번에 고치지 않음):
- `ReloadGroupMap` 의 문서 변경 판정 지문(`ComputeGroupConfigHash` — 멤버·floor·T4)에 MCVideo 몫이 없다 → GROUP_CHANGED 를 놓친 경우 60초 재적재가 MCVideo
  켜기·호 방식 변경을 xcap-diff 로 알리지 않는다(이름 붙은 GROUP_CHANGED 는 늘 알린다).
- chat 합류의 암묵적 MCVideo affiliation 은 영상 호를 나가도 남는다(규격에 나갈 때의 암묵적 de-affiliation 이 없다 — 단말이 풀어야 한다, TS 24.281 §8.2.1.2).
  주채널을 MCVideo chat 그룹 5곳 이상으로 옮겨 다니면 N2(기본 4)에 걸려 486 Warning 102 — 단말·관제 앱이 영상 채널을 떠날 때 MCVideo de-affiliation 을
  보낼지 결정이 필요하다(사용자 결정 대기).

## 5. 시험 (.45)

그룹 «영상테스트» `g-bb255e56` — 편성(prearranged), 멤버 3명: `+82500000001`(W999, 새 APK)·`+82500000002`(MF52, 새 APK)·`+82510001001`. 자격 전원 있음(N2 4·N6 1).

1. 관제 앱에서 «영상테스트» 를 내 채널로 — 카드·채널 상세에 «편성 영상 그룹 — [영상 보내기] 로 영상 호를 엽니다».
2. 단말 둘은 주채널을 «영상테스트» 로 고른다(MCVideo affiliation 이 서야 초대 대상이 된다).
3. 관제 앱 [영상 보내기] → «여는 중…» → 단말 둘이 자동 수락 → 200 OK → 송출 허가(«보내는 중») → 단말 «영상 n» 목록 → [보기] 로 관제 영상이 보이는지.
4. 반대로 W999 가 [영상 보내기] 로 열고 관제 앱이 초대를 자동 수락하는지, «영상 n» 에 W999 가 나오는지.
5. 다른 멤버가 아무도 그 채널에 없을 때 [영상 보내기] → 약 10 s 뒤 480 문구, 자동 재시도 없음.
6. 콘솔에서 호 방식을 chat 으로 바꾸고(저장) 로그인 유지한 채 — 관제 앱이 문서 변경 뒤 chat 합류로 바뀌는지(P2).
