# 관제조작반 앱 UI — Windows 데스크톱(WPF) 화면 설계

> 관제 센터 기능([dispatch_center.md](dispatch_center.md))과 보조 서비스([volte_supplementary_services.md](volte_supplementary_services.md))의
> **관제사 화면**을 정의한다. 앱은 `windows/dispatch-desktop`(WPF, MVVM, `net10.0-windows`)이며 SDK 는
> [ue_sdk.md](ue_sdk.md) 의 .NET 파사드 `CimsUe.dll`(→ C API `cimsue_c.h` → `libcimsue`) 만 참조한다. 요구↔코어 API 대응표는
> ue_sdk.md §7 이 정본이고, 이 문서는 그 위의 **화면·조작·표시 규약**을 정한다. Android 태블릿 관제 앱은 같은 패널을
> 다른 밀도로 배치한다(§12).

## 1. 범위와 결론

- **관제사가 참여한 PTT 채널이 중심이다.** 화면은 두 세계 — **PTT(무전)** 와 **일반통화(VoLTE·외부망)** — 를 같은 폭의 좌우
  두 열로 놓고, 각 세계는 위 **운영 패널** · 아래 **실시간 내역 패널** 의 짝이다. 운영 패널 안은 다시 **왼쪽 운영 / 오른쓰 발신+주소록 /
  그 아래 메시지** 로 나뉜다(§3.1).
- **도킹 패널 여덟 개 + 그룹원 띠.** 열마다 네 패널 — PTT 열: PTT 발신(주소록 포함) · PTT 채널 · PTT 메시지 · PTT 내역, 일반통화 열:
  일반통화 발신(주소록·다이얼패드) · 일반통화(그룹원 띠·대기열·내 통화) · 메시지(SMS·LMS) · 일반통화 내역. 기본 배치는 열마다 위 행
  [발신 | 채널/운영], 아래 행 [메시지 | 내역] 의 2×2 로, 왼쪽(발신·메시지) : 오른쪽(채널·운영·내역) 기본 폭 = 1 : 2 (§3.1 의 ①~④ 구획 번호는 이 여덟 패널의
  묶음 이름으로 계속 쓴다: ① = PTT 발신+채널+메시지, ② = PTT 내역, ③ = 일반통화 발신+운영+메시지, ④ = 일반통화 내역). 관제 그룹원(2~4명,
  최대 10)의 통화 상태는 격자 보드가 아니라 일반통화 패널 첫 줄의 **상태 띠**다.
- **패널은 도킹 패널이다.** 머리를 끌어 위치를 바꾸고, 경계를 끌어 크기를 바꾸고, 별창으로 떼어 두 번째 모니터에 둔다. 기본 배치가
  이 문서의 캔버스이고, 배치는 프리셋으로 저장·잠금한다(§3.3).
- **화면 한 장, 스크롤 없음.** 콘솔 관제 캔버스([../console_platform.md](../console_platform.md) §3.0)와 같은 규율 — 목록이 넘치면
  그 패널 안에서만 스크롤한다. 설계 캔버스 1920×1080, 창이 더 크면 비율 유지, 더 작으면 재배열 없이 그대로.
- **듣기만 하는 세션은 전부 감청 창(팝업), 발언하는 세션은 전부 ① 카드.** 감청·청취의 진입은 내역 행과 그룹원 띠의 [청취]다(§5).
- **관제석은 발신 주체다.** PTT 발신(사설콜·애드혹, PTT 주소록)과 일반통화 발신(다이얼패드·주소록, 외부망 포함)이 각 운영 패널의
  오른쪽 위에 있고, 그 아래에 각 세계의 메시지(MCData SDS / SMS·LMS)가 있다.
- **앱은 화면·장치·수명주기만 안다.** SIP/RTP/floor/SDS/CSC 는 전부 SDK — 앱 코드에 프로토콜 상수(§9 사전 제외)가 나타나면 경계
  위반이다(ue_sdk.md §1 경계 규칙).
- **합법감청 UI 원칙**: 감청·청취는 관제사에게 항상 명시(창·상단 바 칩·시간·대상), 당사자 은닉은 서버 몫. 앱은 `listenVisibility` 를
  표시만 한다.
- **오디오 배치**(ue_sdk.md §6.3): 통화·VoLTE 감청·사설콜은 헤드셋(라우트 0), PTT 그룹·애드혹·PTT 청취는 데스크 스피커(라우트 ≥1)
  기본. 세션마다 바꿀 수 있다.

## 2. 관제사 작업 모델 (화면이 지원해야 하는 일)

| 작업 | 트리거 | 화면 위치 | 코어 API (ue_sdk.md §7) |
|---|---|---|---|
| PTT 그룹 발언 | 채널 선택 + PTT 키 | ① 채널 카드 PTT 버튼 / 핫키 | `joinGroupCall`·`floorRequest/Release` |
| PTT 사설콜 발신 | PTT 사용자 1명 | ① PTT 발신 [사설콜] · PTT 주소록 | `startPrivateCall(peer, {fullDuplex, emergency})` |
| PTT 애드혹 그룹콜 발신 | PTT 사용자 N명 | ① PTT 발신 [애드혹] · 주소록 다중 선택 | `joinGroupCall("adhoc-<나>-<epoch>", {members[]})` |
| MCData 문자(그룹·1:1·첨부) | SDS | ① MCData 메시지 | `sendGroupSds`·`onSds`·`sendSdsNotification` |
| PTT 세션 상황 인지·청취 | conference 로스터·floor | ② PTT 내역 — 진행 중 행 [채널]/[청취] | `subscribeConference`→`onRoster`, `joinGroupCall(listenOnly)` |
| 대표번호 착신 응대 | INVITE `P-Called-Party-ID`=pilot | ③ 대기열 + 착신 배너 | `CallInfo.calledParty` → `answer` |
| 링 중인 대표번호 호 당겨받기 | 대기열 링잉 | ③ 대기열 [당겨받기] | `pickup(code, pilot)` |
| 그룹원 링잉 지정 픽업 | 그룹원 띠 / ④ 행 ringing | [픽업] | `pickup(code, ext)` |
| 일반통화 발신(내선·외부망) | 다이얼패드·주소록 | ③ 일반통화 발신 | `dial` |
| 호 전달 blind / attended | 통화 중 | ③ 내 통화 카드 [전달]·[상담 전달] | `transfer` / `dial`+`transferAttended` |
| 진행 중 통화 청취(감청) | ④ 행 / 그룹원 띠 confirmed | [청취] → 감청 창 | `dialogWatch` → `join(dlg)` |
| 문자(SMS·LMS) | 내선·가입자 | ③ SMS·LMS | `sendRequest(MESSAGE, text/plain)`·`onMessage` |
| 긴급 상황 인지 | emergency/imminent/alert | 전역 배너 + ①②행 배지 | `CallInfo.mcptt.emergency/imminentPeril`, `onMessage(alert-ind)` |
| 장치·핫키·배치 | 설정 | 상단 바 → 설정 창 / 🔒 프리셋 | `audioDevices`·`setAudioDevices`·`addPlaybackRoute`·`setCallRoute` |

## 3. 화면 구성

### 3.1 캔버스 (1920×1080) — 기본 배치

```
┌──────────────────────────────────────────────────────────────────────────────────────────────┐ 56
│ CIMS 관제  홍길동 · 1002 · PTT +8250…0002 · 관제1 · 대표 7000  ●PTT ●VoLTE [감청 중 1▾] … 🔒배치 프리셋▾ 🎧 🔊 ⌨ 14:32 ⚙ │
├──────────────────────────────────────────────┬───────────────────────────────────────────────┤
│ ⋮⋮ ① PTT 채널          참여 4   [카드|타일] ↗▾ │ ⋮⋮ ③ 일반통화     대표 7000 · VoLTE·외부망     ↗▾ │
│ ┌ 순찰1 (멤버 12) ─────────┐┃┌ PTT 발신 [사설콜|애드혹] ┐│ ┌ 관제 그룹원 ─────────────┐┃┌ 일반통화 발신 ──────────┐│
│ │ 발언 김순경 ▮▮▮▮  [PTT] ││┃│ 대상 [윤순경 ……] [발신]  ││ │ 1001● 1002나 1003●통화  │┃│ [02-120……] [발신][픽업**]││
│ │ 🎧/🔊 ▬▬▬ 로스터 이탈    ││┃│ (●)반이중 ( )전이중 ☐긴급││ │ [청취] 1004● 1005●통화   │┃│ [다이얼|주소록|최근]     ││
│ └────────────────────────┘│┃│ PTT 주소록 ─────────────││ └────────────────────────┘┃│ 교통상황실 02-120 [통화] ││
│ ┌ 상황실 (멤버 8)  대기 ────┐│┃│ 윤순경 …0008  [사설][☐] ││ ┌ 대기열 ─────────────────┐┃│ 이순경 1003 통화중 [통화]││
│ │                   [PTT] ││┃│ 최순경 …0004  [사설][☐] ││ │ 🔔 010-9876 → 7000 00:12│┃│ 소방상황실 02-119 [통화] ││
│ └────────────────────────┘│┃│ 박경장 …2003  [사설][☐] ││ │    울림 1001,1004 [당겨받기]│┃│ 정경장 1006 보류  [통화] ││
│ ┌ 교통1 (멤버 5) ──────────┐│┃└────────────────────────┘│ └────────────────────────┘┃└────────────────────────┘│
│ │ 발언 박경장 ▮▮     [PTT] ││┃┌ MCData 메시지  SDS ─────┐│ ┌ 내 통화 ────────────────┐┃┌ SMS · LMS  SIP MESSAGE ─┐│
│ └────────────────────────┘│┃│ [순찰1][상황실 2][교통1]  ││ │ ▶ 02-333-4444 대표 착신 ││┃│ [이순경 1003][정경장]     ││
│ ┌ 애드혹 · 3명 ───────────┐│┃│ 김순경: 현장 도착 …      ││ │   02:15 [보류][음소거]  ││┃│ 이순경: 민원인 연락처 …  ││
│ │ 발언 최순경       [PTT] ││┃│        나: 추가 인원 … ✓✓││ │   [전달▾][상담][종료]   ││┃│        나: 확인했습니다 ✓││
│ └────────────────────────┘│┃│ 📎 현장사진_01.jpg       ││ │ ⏸ 1006 정경장 보류 05:02││┃│                          ││
│ ┌ 윤순경 사설콜 · 전이중 ───┐│┃│ [📎][순찰1 에 메시지…][전송]││ │   [재개][전달▾][종료]   ││┃│ [문자… 0/70 SMS] [전송]  ││
│ │ 01:12 🎧      [음소거][종료]│┃└────────────────────────┘│ └────────────────────────┘┃└────────────────────────┘│
│ └────────────────────────┘│                            │                            │                          │
├──────────────────────────────────────────────┼───────────────────────────────────────────────┤
│ ⋮⋮ ② PTT 내역  실시간  [전체|내 채널|긴급] [검색] ↗▾ │ ⋮⋮ ④ 일반통화 내역  실시간  [전체|대표번호|부재] [검색] ↗▾│
│ 진행 중                                        │ 진행 중                                          │
│ 02:10 순찰1  12명 · 발언 김순경   멤버  ● [채널]   │ 03:41 1003 이순경 ↔ 010-1234-5678  은닉  ● [청취]  │
│ 18:03 야간   5명 · 발언 박경장  청취범위 ● [청취]  │ 11:08 1005 김순경 ↔ 02-555-0001  대표7000 ● [청취] │
│ 00:25 애드혹 최순경·윤순경·임순경 임시  ● [채널]   │ 02:15 1002 나 ↔ 02-333-4444     대표7000 ●        │
│ 최근                                           │ 최근                                             │
│ 14:31 순찰1 김순경 발언 12초                     │ 14:29 착신 7000 ← 010-2222-3333 응답 1004 · 03:12 │
│ 14:30 순찰1 긴급 개시 → 해제  ⚠                 │ 14:21 발신 1002 → 02-120 교통상황실 · 01:05       │
│ 14:28 교통1 세션 종료 05:12 · 참가 6             │ 14:15 부재 7000 ← 010-7777-8888 → 넘김 7100 [재발신]│
│ 14:27 상황실 SDS 박경장 "교대 인원 2명 도착"       │ 14:02 전달 1002 → 1003 blind                     │
└──────────────────────────────────────────────┴───────────────────────────────────────────────┘
        951 (좌 PTT)                                       951 (우 일반통화)          ┃ = 안쪽 스플리터
```

세로 예산: 상단 바 56 + 운영 600 + 내역 406(+간격 6×3) = 1080. 가로: 두 열 951 씩. 운영 패널 안은 왼쓰 545 / 오른쪽 400
(발신·주소록 위, 메시지 아래 — 각 ½). 이 수치는 **기본 프리셋**이고, 운영자가 스플리터로 바꾼 값이 저장된다(§3.3).
감청 창은 캔버스 예산 밖(별창)이다.

### 3.2 전역 요소

| 요소 | 내용 |
|---|---|
| 상단 바 | 데스크 신원(`Profile.displayName`·내선(volte msisdn)·PTT 번호(`effectiveMcpttId`)·`dispatch.groupName(groupId)`·`pilotId`), 계정 등록 점등 2개(PTT/VoLTE — `RegState` 색: 회색 미등록·노랑 등록중·녹색 등록·빨강 실패, 툴팁에 코드·사유), **감청 중 N 칩**(보라 — 열린 감청 창 목록, 클릭 → 창 복원, §5), **배치 🔒/🔓 + 프리셋 ▾**(§3.3), 오디오 요약(헤드셋/스피커 장치명, 클릭 → 설정), PTT 핫키 표시, 시각, 설정 ⚙ |
| 착신 배너 | 상단 바 아래 슬라이드 — "대표번호 7000 착신 · 010-9876-5432 · [응답 F9] [거절]". 대표번호 착신(`calledParty`=pilot) 주황, 내선 직접 착신 파랑, PTT 사설콜 착신 청록. 여러 착신은 스택(최신 위). 응답 핫키는 최상단 호 |
| 긴급 배너 | 빨강(emergency) / 주황(imminent peril) / 자주(alert) 풀폭 — 그룹명·개시자·경과, [채널로 이동]. ①카드·②행 배지와 동기. 취소(`emergency-ind=false` re-INVITE / `alert-ind=false`) 수신 시 해제 |
| 토스트 | 명령 실패의 사유(§9 사전) — 우하단, 6초, 오류는 수동 닫기. 원문 코드는 ▸상세 |
| 상태 색상 | 대기 회색 · 링잉 주황(점멸) · 통화/발언 녹색 · 보류 파랑 · 감청 보라 · 청취/사설콜 청록 · 긴급 빨강. 아이콘·텍스트 병기(색맹 대비) |
| 시간 | 진행 항목은 `mm:ss` 경과(1초 갱신) — 링잉·통화·감청·PTT 세션·발언 |
| 신원 표시 | 내선 → 표시 이름(`users.name`) 병기 "1003 이순경"; PTT 번호는 뒷 4자리 축약 + 툴팁 전체; 외부 번호는 국내 표기. 원 값(URI user part)은 툴팁 |

### 3.3 도킹 — 위치·크기 조정

- **패널 = 도킹 단위** 네 개(①②③④). 머리의 ⋮⋮ 를 끌어 다른 패널 자리로 옮기면 두 패널이 교환되고, 열 경계·행 경계의 스플리터로 크기를
  바꾼다. 운영 패널 **안쪽**(왼쪽 운영 | 오른쪽 발신·주소록 / 메시지)도 스플리터가 있다.
- **↗ 별창**: 패널을 주 창 밖으로 떼어 두 번째 모니터에 둔다(내역 패널·메시지 영역이 주 사용례). 떼어낸 자리는 나머지 패널이 채운다.
  **▾ 접기**: 머리만 남기고 접는다(접힌 패널의 착신·미읽음은 머리 배지로).
- **🔒 배치 잠금**: 오조작 방지 — 잠긴 동안 ⋮⋮·스플리터가 비활성. 기본 잠금.
- **프리셋**: 배치·크기·별창 위치·모니터를 이름으로 저장(기본/야간/2모니터 …), 상단 바에서 전환. `%APPDATA%\CIMS\dispatch-desktop\layout.json`.
  "기본 배치" 는 §3.1 로 복원.
- 최소 크기: 운영 패널 폭 560, 내역 패널 높이 200, 운영 패널 안 오른쪽 열 320. 창 크기가 1920×1080 보다 작으면 재배열하지 않고 그대로
  (작은 화면은 OS 배율이 fit 역할).
- 구현: WPF `Grid`+`GridSplitter` 로 기본 4분할, 도킹 이동·별창·프리셋은 AvalonDock(Xceed, MS-PL) 류 도킹 라이브러리 — 앱 층 결정(§11).

## 4. 패널 상세

### 4.1 ① PTT 채널 — 운영 (좌 열 위)

**왼쪽: 채널 카드** — 관제사가 **발언하는** 세션만: 멤버 그룹 채널 + 진행 중 사설콜·애드혹.
- **소스**: GMS `listGroups(userUri)`(멤버 그룹). 카드는 운영자가 고른 채널(설정 → 채널 선택, 기본 = 멤버 그룹 전부) + 세션 동안의
  사설콜·애드혹 카드. 표시 모드 [카드|타일] — 타일은 큰 PTT 버튼·큰 발언자 표시(터치 모니터·태블릿과 같은 언어).
- **멤버 채널 카드**: 그룹명 · 배지 "멤버" · 멤버 수·affiliation · 세션 상태(대기 / 진행 — 발언자 이름·레벨·경과) · 긴급/임박 배지 ·
  **PTT 버튼**(누르는 동안 `floorRequest`, 떼면 `floorRelease`; 대기 회색·요청 노랑·**발언 중 녹색**·대기열 "n번째" 파랑·거부 빨강 1초) ·
  라우트 토글(기본 🔊) · 남은 발언 시간 게이지(Granted Duration) · [로스터 ▾](`onRoster` — 청취 멤버는 `listenVisibility=visible` 일 때만 `listener` 로 보임) ·
  [이탈](`leaveGroupCall`) / 미참여면 [참여].
- **사설콜 카드**(`CallInfo.mcptt.privateCall`): 상대 이름·번호 · 배지 "사설콜" + "전이중"(`noFloorCtrl`)/"반이중" · 경과 · 라우트(기본 🎧) ·
  반이중이면 PTT 버튼, 전이중이면 [음소거] · [종료]. 착신 사설콜은 착신 배너(청록)에서 응답(§13 자동 수락 분리).
- **애드혹 카드**(`groupId` 가 `adhoc-` 접두): "애드혹 · N명" · 멤버 칩(응답 상태는 in-dialog NOTIFY 로스터) · 발언자·레벨 · PTT 버튼 · 긴급 배지 ·
  [종료]. 마지막 멤버 이탈로 서버가 세션을 걷으면 카드 소멸.
- **선택 채널**: 카드 하나가 "선택"(테두리) — 전역 PTT 핫키는 선택 채널에. 진행 중 애드혹/반이중 사설콜이 생기면 자동 선택(ptt-client 의
  "애드혹 우선" 규칙), 끝나면 이전 선택으로.
- **floor 이벤트 표시**: Granted → 버튼 녹색 + "발언 중 mm:ss"(게이지) · Taken → 발언자(로스터 매칭) · Denied/Revoked → `causeText` 카드 하단 한 줄 ·
  QueuePosition → "대기 n" · TalkLimit → 게이지 빨강 · RequestTimeout → 회색.
- **긴급**: `mcptt.emergency`/`imminentPeril` 세션은 카드 테두리 빨강/주황 + 전역 배너. 관제사 긴급 개시는 카드 메뉴 [긴급 호출](확인) —
  `GroupCallOptions.emergency`, 자격 없으면 403 사전 문구.
- 카드 세로 예산: 카드 ~100px → 600 높이에 5개 무스크롤(멤버 3~4 + 사설콜/애드혹 1~2). 넘치면 카드 열 안 스크롤.

**오른쪽 위: PTT 발신 + PTT 주소록**
| 모드 | 대상 | 옵션 | 개시 |
|---|---|---|---|
| 사설콜 | PTT 사용자 1명(주소록 [사설] 또는 입력) | (●)반이중 floor / ( )전이중 `mc_no_floor_ctrl` · ☐긴급 | `startPrivateCall(acc_ptt, peer, {fullDuplex, emergency})` → 카드 |
| 애드혹 | PTT 사용자 N명(주소록 ☐ 체크 → 칩, 최소 1) | ☐긴급 | `joinGroupCall(acc_ptt, "adhoc-<내 PTT 번호>-<epoch초>", {members: tel: URI[]})` → 카드 |

- **PTT 주소록**: 세그먼트 [사용자|그룹]. 사용자 행 = 이름·PTT 번호·현재 상태(어느 채널에서 발언/참여 중 — 로스터에서 파생)·[사설]·[☐ 애드혹].
  그룹 행 = 이름·멤버 수·[채널에 추가]·[메시지] + 내 소유 그룹(`is_owner`)이면 "내 그룹" 배지·[편집]·[삭제]. 탭 머리 = [↻ 새로고침]·[새 그룹](자격
  `ptt.allowCreateGroup` 일 때만). [새 그룹]/[편집] → `GroupEditWindow`(이름·id·세션 종류·우선순위·최대 참가자·긴급/SDS/FD/영상/affiliation/암호화 +
  멤버 = PTT 주소록에서 추가·의장 토글) → GMS XCAP PUT(편집은 `If-Match`, 412 = 재편집 안내) → 목록 재조회(`RefreshGroupsAsync` — 새 그룹
  affiliation·conference 구독, 사라진 그룹 해제). [삭제] = 확인 후 XCAP DELETE. 서버발 변경은 `xcap-diff`(`sip:gms_psi@…`) NOTIFY 로 자동 재조회.
  검색 공통. 소스는 §13(GMS 그룹 문서 멤버 + 프로비저닝 → 사용자 목록 API 후속).
- 애드혹 임시 그룹 id 는 앱이 만든다(mcptt_emergency_modes.md §6 규약; `adhoc-`·`priv-` 는 편성 그룹 예약어). 채널 영속·affiliation·로스터 구독 대상이
  아니다.
- 자격 선차단: CMS user-profile(`allow_adhoc_call`·`allow_emergency_private_call`·`PrivateCall/EmergencyCall/MCPTTPrivateRecipient`) 에 없으면 해당
  모드/옵션 비활성 + 툴팁. 긴급 사설콜 `UsePreConfigured` 면 대상을 사전 지정 수신자로 잠금(mcptt_emergency_modes.md §7). 파싱 API 는 §13.

**오른쪽 아래: MCData 메시지** — [mcdata_messaging.md](mcdata_messaging.md) §5 의 앱 동작:
- 스레드 칩(그룹 = `groupUri`, 1:1 = 발신자 — `threadKeyOf` 규칙, 미읽음 수) → 선택 스레드의 말풍선(발신 상태 🕓→✓→✓✓/⚠ 재전송, 그룹 수신은
  발신자 라벨) → 입력 + [📎](FD) + [전송]. 채널 카드를 선택하면 그 그룹 스레드로 따라간다(설정으로 끌 수 있음).
- 발신 `sendGroupSds(acc_ptt, groupId, text, requestDelivery)` → `onRequestResult`(token, 2xx=SENT) · disposition 요청 수신은 `sendSdsNotification(delivered)`
  자동 회신 · `onSds(notification)` → ✓✓. 1:1 SDS 는 사설콜 상대·주소록 사용자에게.
- 보관: 로컬 SQLite(`%APPDATA%\CIMS\dispatch-desktop\messages.db`) 최근 30일(설정). 미읽음은 패널이 접혀 있어도 머리 배지.

### 4.2 ② PTT 내역 — 실시간 (좌 열 아래)

**목적**: PTT 세계에서 **지금 일어나는 것과 방금 일어난 것**을 한 목록으로. 진행 중은 위에 고정, 아래로 시각순.

- **진행 중 행**(범위 안 그룹 = 멤버 그룹 + `pttListen` 대상): 그룹명 · 배지(멤버/청취 범위/임시) · 참가자 수 · 발언자 · 경과 · 긴급 배지 · 조작 —
  멤버 그룹 [채널](① 카드로 포커스·미참여면 `joinGroupCall`), 청취 그룹 **[청취]**(`joinGroupCall(listenOnly)` → 감청 창 §5), 이미 듣고
  있으면 [청취 중 · 창]. 세션 없는 청취 그룹은 "대기" + [청취] 비활성.
- **상태 소스**: RFC 4575 conference 구독(`subscribeConference` → `onRoster`) — 멤버 그룹은 현행, **청취 범위 그룹은 서버 인가 확장이
  전제**(§13, dispatch_center.md §10). 발언자는 참여/청취 중인 세션만(floor 이벤트).
- **최근 행**(이벤트 종류): 발언(누가·몇 초 — `onFloor` Granted→Idle) · 긴급/임박 개시·해제 · 세션 시작/종료(참가·길이) · 멤버 합류/이탈(로스터 diff) ·
  SDS(발신자·요약) · 사설콜/애드혹 시작·종료 · 청취 시작/종료(관제사 자신). 필터 [전체|내 채널|긴급], 검색.
- 앱 로컬 링 버퍼(세션당 200 행, 하루)·CSV 내보내기. 서버 정본(PTT 세션 이력·감사)과 별개 — 앱 내역은 관제사의 작업 메모리다.
  범위 안 **타인**의 세션·발언·SDS 는 서버 통합 이력 폴링(`HistoryClient`, §13)이 수초 지연으로 최근 행에 합친다(내가 당사자인 항목은 로컬 행이
  이미 있어 건너뜀).

### 4.3 ③ 일반통화 — 운영 (우 열 위)

**왼쪽 위: 관제 그룹원 상태 띠** — BLF 의 축소형(2~4명, 최대 10):
- 칩 = 내선 · 이름 · 상태(dialog `state`: 대기/링잉/통화/보류 + 상대·경과). 자기 내선은 "나". 링잉 → [픽업](`pickup(code, ext)`), 통화 중이고
  `monitorScope` 안 → [청취](감청 창), 대기 → 클릭 → 발신 필드에 채움.
- 소스: 그룹원마다 `dialogWatch(acc, ext, true)`(RFC 4235) → `onDialogInfo`. 대표번호 AoR 도 구독(대기열). 그룹원 목록 공급은 §13.
- 10명을 넘으면 띠가 두 줄이 되고 그 이상은 "+n" 로 접는다(운영 권고 10 이내).

**왼쪽 중: 대기열** — 대표번호 AoR dialog(dispatch_center.md §4.5): 발신자 · `→ 대표번호` · 링 경과 · 울리는 그룹원(포크 대기 leg — 각 내선
dialog 의 early 로 추정, RLS 전) · [당겨받기]=`pickup(code, pilotId)`. 자기 단말도 울리면 [응답]. 응답되면 "응답: 1004 최순경" 3초 후 제거.
sequential 모드는 한 명만 울린다. 빈 상태: "대기 호 없음" + 오늘 응대·부재 건수.

**왼쪽 아래: 내 통화** — `calls()` 중 VoLTE 통화(`!isMcptt && !listenOnly`) 카드(활성 1 + 보류 n, 스크롤):
- 카드: 상대 · **착신 경로 배지**(`calledParty`=pilot → "대표 7000 착신") · 상태 · 경과 · 🎧/🔊 · [응답]/[거절 486]/[보류]/[재개]/[음소거]/[DTMF]/[전달 ▾]/
  [상담 전달]/[종료].
- blind 전달: 대상(주소록 선택 가능) → `transfer` — "전달 중 → 1003", 서버 BYE 로 카드 소멸·토스트. 실패면 원 통화 유지.
- attended: [상담 전달] → 원 통화 자동 보류 + `dial` 상담 카드(배지 "상담") → [전달 완결]=`transferAttended(원, 상담)` / [취소].
- 활성 통화 중 새 착신 응답 → 기존 통화 자동 보류(설정: 자동 보류/거절).

**오른쪽 위: 일반통화 발신 — [다이얼패드 | 주소록 | 최근] 중 하나만 표시**
- 번호 필드(내선·번호·이름 자동완성, `sip:` URI 허용) · [발신]=`dial(acc_volte, target)` · [픽업 **]=`pickup(code)` 는 공통 첫 줄. 그 아래는
  세그먼트 **[다이얼패드 | 주소록 | 최근]** 으로 한 가지만 보인다 — 기본은 **주소록**, 마지막 선택을 기억한다. 세 가지가 동시에 나오지 않으므로
  각각 오른쓰 열 폭(400)을 다 쓴다.
- **다이얼패드** 3×4 — 번호 필드에 채우고, 통화 중에는 같은 패드가 DTMF 로 동작한다(활성 통화가 있고 필드가 비어 있으면 DTMF). 내 통화 카드의
  [DTMF] 를 누르면 이 세그먼트로 자동 전환.
- **주소록**: 행 = 이름·번호·종류 배지(내선/외부)·현재 상태(그룹원은 BLF)·[통화]·[문자](외부 번호는 §"SMS·LMS" 게이트웨이 유무에 따라 비활성). 검색 공통.
  외부망 번호는 접속서비스의 트렁크 라우팅에 맡긴다(앱은 그냥 `dial`).
- **최근**: 착신·발신·부재·전달 — 시각·상대·경로(대표/직접)·[재발신]·[문자].
- 주소록 소스: 관제 그룹원(프로비저닝) + 조직 연락처(외부망 포함 — 로컬 CSV, CSC 연락처 API 는 §13).

**오른쪽 아래: SMS · LMS** — 일반통화 세계의 문자:
- 전송 = SIP `MESSAGE` `text/plain` (`sendRequest(acc_volte, "MESSAGE", target, "text/plain", body)`, 결과 `onRequestResult`), 수신 = `onMessage(from,
  "text/plain", body)`. 서버는 1:1 MESSAGE 를 상대 등록 바인딩으로 그대로 전달한다([mcdata_messaging.md](mcdata_messaging.md) §4 ③ — 게이트·보관 없음).
- 스레드 칩(1:1 = 상대) → 말풍선(발신 ✓ = MESSAGE 2xx, 실패 ⚠ 재전송 — 4xx/5xx 사전) → 입력(글자 수 "n/70 SMS", 70자 초과 시 "LMS" 표시 —
  하나의 MESSAGE 로 보낸다; 길이 한도는 서버 `max_sds_size`, 초과 413 → 사전 문구) + [전송].
- **대상은 내선·가입자(등록 단말)다.** 외부망 휴대전화 SMS/LMS 는 서버에 문자 게이트웨이(IBCF→SMSC, TS 24.341 SMS over IMS 또는 SMPP)가
  없어 **미지원** — 외부 번호의 [문자] 는 비활성 + 툴팁 "문자 게이트웨이 미구성"(§13 서버 과제). 패널 머리에 게이트웨이 상태 배지.
- 보관: MCData 와 같은 SQLite, 스레드 종류 구분. 미읽음은 머리 배지.

### 4.4 ④ 일반통화 내역 — 실시간 (우 열 아래)

- **진행 중 행**: dialog 이벤트를 **세션 행**으로 결합 — `A ↔ B`(내선은 이름 병기) · 상태(링잉/통화/보류) · 경과 · 배지(대표번호 경로 — 대표번호 AoR
  dialog 와 Call-ID·시각 상관; 은닉/투명) · 조작: 링잉 → [지정 픽업], 통화(confirmed) & 범위 안 → **[청취]**(`join(dlg)` → 감청 창), early/범위 밖 →
  비활성(툴팁). 자기 통화도 행으로 보인다(조작 없음).
- **결합 규칙**: 감시 대상 두 내선이 서로 통화하면 dialog 가 두 개(각 leg — Call-ID 가 다름) 온다. `remoteIdentity` 가 서로를 가리키고 전이 시각이
  근접하면 한 행(어느 leg 로도 Join 가능 — dispatch_center.md §5.3). 한쪽만 감시 대상이면 그 leg 하나.
- **최근 행**: 착신(응답자·길이) · 발신(길이) · 부재(대표번호 전원 무응답 → 넘김 대상, [재발신][문자]) · 전달(blind/attended, 대상) · 픽업(누가 어느 호를) ·
  문자(SMS 요약) · 청취 시작/종료(관제사 자신). 필터 [전체|대표번호|부재], 검색. 정렬: 링잉 → 진행 시작 역순 → 최근 시각 역순.
- 로컬 링 버퍼·CSV 내보내기(②와 동일). 서버 정본은 통화 기록·녹취 이력. 범위 안 타인의 끝난 통화·SMS 는 서버 통합 이력 폴링(§13)이 최근 행에 합친다.

## 5. 감청 창 (팝업)

듣기만 하는 세션 하나 = 창 하나. 주 창과 별개의 비모달 `Window`(기본 440×260, 크기 조절·이동 가능, 두 번째 모니터에 두는 것이 기본 사용례).

| 종류 | 진입 | 내용 | 종료 |
|---|---|---|---|
| **VoLTE 감청** | ④ 행·③ 그룹원 띠 [청취] → `join(dlg)` → `CallInfo.listenOnly && joinedDialog≠""` | 제목 "감청 — A ↔ B" · 은닉/투명 배지(`listenVisibility`) · 경과 · 두 줄 `caller`/`callee`(RFC 5576 `label`) 각각 이름·**레벨 미터**·활성 점(`MediaSource.active/level` — U10 관측 API 후속 시 실시간, 그 전엔 `active`) · 라우트(기본 🎧) · [양쪽 ▾](표시용 — tap 모드는 서버 운용값) · [청취 종료] | [청취 종료]·창 닫기 = `hangup`. 원 통화 종료 → 서버 BYE → "통화 종료됨" 3초 후 자동 닫힘 |
| **PTT 청취** | ② 행 [청취] → `joinGroupCall(listenOnly)` | 제목 "청취 — 그룹명" · 배지 "청취 전용" · 발언자 이름·레벨·경과(`onFloor` Taken) · 참가자 수(`onRoster`) · 긴급 배지 · "발언 요청 불가(Permission=0)" 고정 문구 · 라우트(기본 🔊) · 음량 · [청취 종료] | [청취 종료]·창 닫기 = `leaveGroupCall`. 480 이면 창을 띄우지 않고 토스트 |

- **창을 최소화해도 청취는 계속된다** — 상단 바 "감청 중 N" 칩이 열린 창을 나열하고 클릭 시 복원. 창 닫기(×)는 종료(설정 "닫기 전 확인").
- 감청 창은 포커스를 훔치지 않는다. 주 창의 착신·긴급 배너가 우선.
- 여러 창의 소리는 라우트별로 섞인다 — 기본값이 VoLTE 감청 🎧 / PTT 청취 🔊 인 이유. 동시 청취 상한 기본 4(설정).
- 앱은 감청 이력을 로컬 내역(②④ "청취 시작/종료" 행)에만 남기고 감사 정본은 서버(`E-AUD-016`).
- 영상(F3): VoLTE 감청 창 아래 격자(caller | callee)가 붙어 창이 커진다(§10).

## 6. 시작·로그인·프로비저닝

```
[시작] → 단일 인스턴스 확인(명명 Mutex, 둘째 실행은 기존 창 활성화)
      → 저장 토큰(DPAPI) 있으면 refresh → 없으면 로그인 창(아이디/비밀번호 → CscClient.login PKCE)
      → fetchProfile(/provisioning/me) → services[volte, ptt] → Engine.start → addAccount×2 → register×2
      → dispatch 블록 → 데스크 신원·범위 적용 → dialogWatch(그룹원·대표번호) · affiliate(멤버 그룹) · subscribeConference(멤버·청취 그룹)
      → 배치 프리셋 적용 → 메인 화면
```

- 로그인 창: 아이디·비밀번호·CSC 주소(기본 마지막 값, 고급 접힘)·"자동 로그인"(refresh token 만 DPAPI 저장 — 비밀번호·H(A1) 는 저장하지 않는다;
  `sipHa1` 은 매 로그인 프로파일에서 받는다).
- `dispatch.present=false` → 메인은 뜨되 ③ 그룹원 띠·대기열과 ④ 의 청취 조작을 "관제 데스크 미배정" 으로 접고 PTT·통화·문자·발신은 동작
  (일반 소프트폰 모드). 콘솔 `관리 > 관제 그룹` 배정 안내.
- 등록 실패(401/403/타임아웃) → 상단 점등 빨강 + 토스트, 자동 재시도(백오프 5→60초). `refreshRegistration` 은 네트워크 복귀 이벤트(Windows
  `NetworkChange`)에서 즉시.
- 로그아웃: 등록 해제 → 토큰 폐기 → 로그인 창. 창 닫기는 트레이 최소화(설정), 완전 종료는 메뉴 — 감청 창·진행 세션이 있으면 확인.

## 7. 오디오 배치 UI (ue_sdk.md §6.3)

설정 → **오디오**:

| 항목 | UI | API |
|---|---|---|
| 마이크 | 캡처 장치 콤보 + 레벨 미터(테스트) | `setAudioDevices(capture, -2)` |
| 헤드셋(라우트 0) | 재생 장치 콤보 — 통화·VoLTE 감청·사설콜 기본 | `setAudioDevices(-1, playback)` |
| 데스크 스피커(라우트 1) | 재생 장치 콤보(없음 가능) — PTT 그룹·애드혹·PTT 청취 기본 | `addPlaybackRoute(dev)` / `removePlaybackRoute` |
| 기본 라우트 정책 | 세션 종류별 기본값(위) — 스피커 없으면 헤드셋 | 세션 생성 시 `setCallRoute` |
| 장치 테스트 | 각 장치로 톤 재생 | 앱 로컬(WASAPI 톤) |

- 핫플러그: 파사드 `AudioEndpoints`(`IMMNotificationClient`) → `refreshAudioDevices()` → 선택 장치가 사라졌으면 기본 장치 폴백 + 토스트, 다시 붙으면
  자동 복귀(설정).
- 카드·감청 창의 🎧/🔊 토글은 세션별 `setCallRoute(callId, 0|routeId)`. 라우트 1 이 없으면 비활성.
- 두 장치 동시 출력의 지연·에코는 실기 검증 항목(ue_sdk.md §11) — 보정 설정은 두지 않는다(엔진 과제).

## 8. 핫키·입력

| 기능 | 기본 | 동작 | 접점 |
|---|---|---|---|
| PTT | `Ctrl+Space`(hold) | 선택 채널 `floorRequest` / 떼면 `floorRelease` | `RegisterHotKey` + 메시지 전용 HWND, key-up 은 `GetAsyncKeyState` 폴링 |
| 응답 | `F9` | 착신 배너 최상단 호 `answer` | 전역 |
| 종료 | `F10` | 활성 통화 `hangup`(감청 창·PTT 채널 제외) | 전역 |
| 그룹 픽업 | `F8` | `pickup(code)` | 전역 |
| 보류/재개 | `F11` | 활성 통화 토글 | 앱 포커스 시 |
| 음소거 | `F12` | 활성 통화·전이중 사설콜 `setMuted` 토글 | 앱 포커스 시 |
| 채널 선택 | `Ctrl+1..9` | ① 카드 n 선택 | 앱 포커스 시 |

- 설정에서 재배치, 충돌(`RegisterHotKey` 실패)은 빨강 표시. 게임패드/풋스위치는 HID 키 매핑으로 같은 경로.
- PTT 키 hold 중 포커스가 바뀌어도 release 를 놓치지 않도록 key-up 폴링 20ms + 안전장치(Granted 후 `TalkLimit` 는 코어가 자동 Release).

## 9. 응답 코드 → 화면 문구 (사전)

앱이 프로토콜을 해석하는 유일한 지점. 코드 원문은 토스트 ▸상세에.

| 기능 | 코드 | 문구 | 근거 |
|---|---|---|---|
| 픽업 | 404 | "당겨받을 호가 없습니다" | volte_supplementary_services §5.3 |
| 픽업 | 403 | "다른 그룹의 호입니다" | 〃 |
| 픽업 | 489 | "구독 이벤트 미지원(서버)" | 〃 P2 |
| 전달 | 403 | "이 서비스는 호 전달이 허용되지 않습니다" | `transfer_allowed` §6.3 |
| 전달 | 4xx(대상) | "전달 대상이 응답하지 않아 원 통화를 유지합니다" | §6.1 |
| 감청 Join | 403 | "청취 권한이 없는 대상입니다" | dispatch_center §5.3 |
| 감청 Join | 481 | "통화가 이미 종료되었거나 아직 연결 전입니다" | 〃 |
| 감청 Join | 488 | "미디어 조건 불일치(코덱/SRTP) — 관리자 문의" | 〃 |
| 감청 Join | 486 | "이 통화의 청취 인원이 찼습니다" | §5.5 |
| PTT 청취 | 403 | "청취 자격이 없거나 범위 밖 그룹입니다" | §5.6 |
| PTT 청취 | 480 | "진행 중인 그룹 통화가 없습니다" | 〃 |
| PTT 참여 | 403 | "그룹 멤버가 아닙니다" | TS 24.379 §10.1.1 |
| 사설콜 | 403 | "사설콜 자격이 없거나 상대가 허용하지 않습니다" | TS 24.379 §11 인가 |
| 사설콜 | 404 / 480 / 486 | "상대를 찾을 수 없음 / 응답 없음 / 통화 중" | — |
| 애드혹 | 403 | "애드혹 그룹통화 자격이 없거나 시스템에서 꺼져 있습니다" | mcptt_emergency_modes §6 |
| 긴급 개시 | 403 | "긴급 호출 자격이 없습니다" | mcptt_emergency_modes §4.2·§7 |
| MCData SDS | 403 / 413 / 404·408·503 | "그룹 문자 권한 없음 / 너무 긺(서버 한도) / 전송 실패 — 재전송" | mcdata §4·§5 |
| SMS·LMS | 404 / 480 | "상대가 등록되어 있지 않습니다 / 응답 없음" | MESSAGE 1:1 전달 |
| SMS·LMS | 413 | "문자가 너무 깁니다(서버 한도)" | `max_sds_size` |
| 등록 | 401/403 | "인증 실패 — 다시 로그인" | sip_access_security |
| 등록 | 408/503 | "서버 응답 없음 — 재시도 중" | — |

## 10. 영상 (F3 — 예약)

- VoLTE 감청 창 아래 격자: 양측 영상 SSRC 2개를 2분할(caller | callee), `onVideoFrame` 콜백 렌더(WPF `WriteableBitmap`/D3DImage). 창은 이미
  별창이라 주 캔버스 예산은 유지.
- 1:1 영상 통화는 ③ 내 통화 카드 확장(또는 별창).
- F3 전까지 감청 창에 "영상 없음(음성 감청)" 고정 문구.

## 11. 구현 구조 (WPF, MVVM)

```
windows/dispatch-desktop/                 DispatchDesktop.csproj — net10.0-windows, CommunityToolkit.Mvvm · Dirkster.AvalonDock · Microsoft.Data.Sqlite
  App.xaml(.cs)                 단일 인스턴스·전역 예외·SynchronizationContext 캡처·테마·로그인→메인·1초 틱·네트워크 복귀 재등록. `--ui-preview` = 로그인 없이 메인(개발)
  Shell/MainWindow.xaml         상단 바(드롭다운은 Popup — 시스템 메뉴는 테마 색을 못 입힌다) · 배너 레이어 · AvalonDock 도킹 호스트(패널 8개 LayoutAnchorable,
                                ContentId pttcall/ptt/pttmsg/pttlog · callorig/call/sms/calllog, 열마다 [발신 | 채널·운영] / [메시지 | 내역]) · 토스트 레이어
  Themes/Controls.xaml          기본 컨트롤(ScrollBar·ComboBox·CheckBox·RadioButton·TabControl·ToolTip·TextBox)의 테마 템플릿 — Light/Dark 브러시로만 그린다.
                                도킹 크롬은 AvalonDock VS2013 Light/Dark 테마를 앱 테마와 함께 전환
                                코드비하인드: 배치 잠금(CanMove/CanFloat)·프리셋(XmlLayoutSerializer → layout.json)·감청 창 관리·앱 포커스 핫키·트레이 최소화·종료 확인
  Shell/MonitorWindow.xaml      감청 창(§5) — VoLTE/PTT 두 본문, 위치 기억, 닫기 = 종료(확인), 세션 종료 → 3초 후 자동 닫힘
  Shell/LoginWindow · SettingsWindow · PromptWindow
  ViewModels/
    MainViewModel               패널 VM 조립 · 패널 간 연동(발신 필드 채움·스레드 따라가기·[채널] 포커스) · 전역 핫키 → 동작 · 감청 창 열기/닫기 요청
    DeskViewModel               Profile·dispatch·등록 상태·오디오 요약·감청 중 N 칩·배치 잠금/프리셋 (상단 바)
    PttChannelsViewModel        ① 왼쪽 — ChannelCard(멤버/사설콜/애드혹) · FloorState · 선택 채널(애드혹 우선 자동 선택) · 카드/타일 모드
    PttOriginateViewModel       ① 오른쪽 위 — 사설콜/애드혹 모드 · PTT 주소록(사용자/그룹) · 애드혹 선택 칩
    McDataMessagesViewModel     ① 오른쪽 아래 — 스레드·말풍선·disposition 자동 회신·발신 순서 큐로 MESSAGE 최종 응답 상관 (MessagesViewModelBase)
    PttActivityViewModel        ② — 진행 중 행(conference 로스터·floor) + 최근 이벤트 링 버퍼
    CallDeskViewModel           ③ 왼쪽 — MemberChip(DialogInfo) · QueueItem(대표번호 dialog) · CallCard(전달 blind/attended 포함)
    CallOriginateViewModel      ③ 오른쪽 위 — 표시 모드 [다이얼패드|주소록|최근] 하나 · 다이얼패드(DTMF 겸용) · 주소록 · 최근
    SmsMessagesViewModel        ③ 오른쪽 아래 — text/plain MESSAGE 스레드 · token 상관 · 외부망 비활성
    CallActivityViewModel       ④ — 세션 행(dialog 쌍 결합) + 최근 기록
    MonitorWindowViewModel      감청 창 하나(join 호 또는 listenOnly 그룹콜) · MediaSource 미터
    LoginViewModel · SettingsViewModel
  Models/  SessionKind: isMcptt&&listenOnly→PTT 청취(창) · isMcptt&&privateCall→사설콜(①) · groupId adhoc-→애드혹(①) · isMcptt→멤버 채널(①) ·
           listenOnly&&joinedDialog→VoLTE 감청(창) · 그 외 VoLTE 통화(③). SessionItem·GroupInfo·DialogRow·Message/MessageThread·ActivityRow·Contact
  Services/ DispatchSession(코어 투영 + 관제 동작 진입점 — Engine·CscClient 소유, Sessions/Groups/Dialogs, 등록 백오프, 오디오 적용) ·
            Notifications(토스트·배너) · SettingsStore(json) · LayoutStore(프리셋) · MessageStore(SQLite: mcdata/sms) · ActivityLog(링 버퍼·CSV) ·
            HotKeyMap · AudioPolicy(라우트 기본값) · AdhocIdFactory(adhoc-<나>-<epoch>) · DirectoryService(그룹원·PTT 사용자·연락처 CSV) ·
            ResponseText(§9 사전) · AppLog(%APPDATA% logs, 7일)
  Views/    PttChannelsPanel · PttOriginateView · MessagesView(MCData/SMS 공용) · PttActivityPanel · CallDeskPanel · CallOriginateView · CallActivityPanel
  Themes/   Light/Dark(같은 키) · Styles(패널·카드·버튼·배지·칩·미터)  Converters/  표시 규약 변환기
```

- 도킹 라이브러리 = **AvalonDock(Dirkster, MS-PL)**. 잠금은 각 패널의 `CanMove`/`CanFloat` 를 끄고 안쪽 `GridSplitter` 를 비활성, 접기는 AvalonDock
  auto-hide, 별창은 float, 프리셋은 `XmlLayoutSerializer` XML 을 `layout.json` 에 이름별로 보관("기본 배치" 는 XAML 기본).

- 파사드 이벤트(`CimsUe.Engine` 의 `CallStateChanged`·`FloorChanged`·`DialogInfo`…)는 UI 스레드로 마샬링돼 온다(ue_sdk.md §6.4) — ViewModel 은
  `ObservableCollection` 직접 갱신.
- **UI 는 코어 상태의 투영**: 카드·행·창은 `calls()`/`callInfo` 스냅샷과 구독 이벤트에서 파생하고 앱이 별도 상태 기계를 갖지 않는다(재접속·재기동 후
  화면 재구성 = 스냅샷 재조회, 열려 있던 감청 창도 `calls()` 의 listenOnly 호에서 복원). 내역(②④)만 앱이 축적한다.
- 접근성: 모든 조작은 키보드 도달 가능, 상태는 색+아이콘+텍스트 삼중.

## 12. Android 태블릿 밀도

같은 네 패널을 가로 태블릿(1280×800)에 **탭 2개**로: [PTT](① 위 · ② 아래) / [일반통화](③ 위 · ④ 아래). 감청 창은 전면 시트(bottom sheet), 착신·긴급
배너와 PTT 하드키(UNIWA 측면 키)는 공통, 카드 모드는 "타일". 상세는 `android/dispatch-tablet` 구현 시 이 절을 확장한다.

## 13. 미해결 / 향후 과제

- **주소록 소스 = 서버 회사 전화번호부** `GET /provisioning/directory?service=volte|ptt`([android_ue_provisioning.md](android_ue_provisioning.md) §3-1 — 조직 트리 +
  가입자, ETag/304, Android 연락처 탭과 같은 소스·동선: 조직 범위 선택 + 조직별 섹션 + 검색 + 홈 국가 로컬 표기). 앱은 `directory-cache.json` 에 캐시한다.
  **아직 서버가 주지 않는 것**: 관제 그룹원 내선 목록·청취 대상 그룹 목록 — `dispatch` 블록 `members[]`/`pttTargets[]` 확장을 서버에 요청
  ([../../dev/server_request_dispatch_group_monitoring.md](../../dev/server_request_dispatch_group_monitoring.md) §2). 앱은 두 배열이 오면 그것을
  dialog watch·conference 구독 대상으로 쓰고, 없으면 로컬 CSV `member` 태그로 폴백한다. 외부망 연락처(CSV `external`).
- **PTT 그룹 생성·편집·삭제를 관제 앱에서** — 경로는 **GMS XCAP**(TS 24.481, 생성 주체 = 권한 있는 가입자 = 관제사, PKCE 토큰)로 확정.
  관리 API `/api/v1/ptt/groups`([admin_api.md](../../api/admin_api.md) §6)는 콘솔 토큰 전용으로 그대로 둔다. 자격 = `ptt_user_profile.allow_group_creation`
  (프로비저닝 `ptt.allowCreateGroup`), 편집·삭제 = 본인 소유(`authorized_user_id`) 그룹만 — 서버 구현 요청은 위 요청서 §1.
  앱: PTT 주소록 [그룹] 탭 [새 그룹]·행 [편집]·[삭제] → `GroupEditWindow` → `CscClient.PutGroup/DeleteGroup` → `RefreshGroupsAsync`
  (GMS 목록 재조회 + 신규 그룹 affiliation·conference 구독, 삭제 그룹 해제).
- **조직 구성 관리는 OAM 콘솔 몫** — 조직 트리(`organizations` 계층)·가입자 소속·관제 그룹 편성은 콘솔 `관리 > 조직/가입자/관제 그룹` 에서 편집하고
  앱은 `/provisioning/directory`·`dispatch` 블록으로 결과만 받는다(콘솔 화면 과제, [../console_platform.md](../console_platform.md)).
- **서버 전제 — 청취 범위 그룹의 conference 이벤트 구독 인가**(dispatch_center.md §10): ② 진행 중 행의 "진행/참가자 수" 소스. 그 전까지 청취 그룹 행은
  "미상". 서버가 인가를 켜면 범위 밖 그룹은 403 + `Warning: 138` 이 온다 — 앱은 `Area.PttListen` 403 문구로 흡수하고 재시도하지 않는다(구독은 그룹 목록
  재조회 때 1회).
- **서버 통합 이력 조회(메시지 모니터링 포함) — 앱 `Services/HistoryClient`**: 관제 범위 안에서 **끝난** 통화·PTT 세션·메시지를 수초 지연으로 ②④ 최근 행에
  합친다. 진행 중 상태는 dialog/conference 구독이 정본이라 폴링이 live 를 대체하지 않는다. 메시지 모니터링은 **실시간 사본 없이 이력 조회만**으로 결정
  (요청서 [../../dev/server_request_dispatch_group_monitoring.md](../../dev/server_request_dispatch_group_monitoring.md) §4).
  - 요청: `GET /provisioning/history?kind=call|ptt|message&since=<cursor>&limit=200`(CSC 4430, PKCE Bearer, `If-None-Match` → 304). kind 별로 커서·ETag 독립,
    2.5 초 주기. 로그인 직후 `kind=call&limit=1` 탐침 — 404/501 이면 서버 미구현으로 조용히 꺼지고, 403 이면 범위 밖으로 꺼진다(재시도 없음).
  - 응답(앱이 읽는 것 — **서버 확정 대기**, 필드 추가는 무시): `{ "items": [ { "id", "time"(ISO 8601), "kind", "event", "from", "to", "group", "duration"(초),
    "emergency", "text" } ], "next": "<다음 since 커서>", "etag" }`. `id` 가 중복 제거 키, `items` 는 시각순. `event` 는 앱이 아는 값만 종류로 옮기고 나머지는
    메모 행: call = `call.answered|call.ended|call.missed|call.noanswer|call.transferred|call.pickup`, ptt = `ptt.talk|ptt.session.start|ptt.session.end|
    ptt.emergency|ptt.private|ptt.adhoc`, message = `message.sds|message.sms`(sms 는 ④, sds 는 ②).
  - 내가 당사자(`from`/`to` 가 내 PTT·VoLTE 번호)인 항목은 로컬 행이 이미 있어 건너뛴다. 이름은 주소록으로, 그룹은 GMS 목록 이름으로 표시.
- **서버 과제 — 외부망 SMS/LMS 게이트웨이**: 현재 MESSAGE 는 등록 가입자 간 전달만. 외부망 휴대전화 문자는 IBCF→SMSC(TS 24.341 SMS over IMS) 또는 SMPP
  게이트웨이가 필요하다. 앱은 게이트웨이 유무를 접속서비스 능력으로 받아 [문자] 활성/비활성을 결정한다(능력 키 신설 필요).
- **U10 관측 API** — `MediaSource.level/active` 실시간 갱신 확정 후 감청 창 레벨 미터 활성.
- **경보(alert-ind) 파싱** — `onMessage` 의 `mcptt-info` 를 코어가 `McpttInfo` 로 해석해 이벤트로.
- **CMS user-profile 파싱 API** — `allow_adhoc_call`·`allow_emergency_private_call`·수신자 모드를 구조로.
- **자동 수락 분리** — `AccountConfig.autoAnswerMcptt` 는 그룹콜·사설콜 공통. 관제석은 그룹콜 자동 + 사설콜 수동이 맞아 코어 플래그 분리 필요.
- **대표번호 발신 표시**(`P-Preferred-Identity`=pilot — 서버 과제) 확정 시 ③ 발신에 "대표번호로 발신" 토글.
- **큐/ACD**, **영상 F3**, **ambient listening**, **끼어들기**(CMP 믹서) — 서버 과제.
- **도킹 라이브러리 선정** — AvalonDock(MS-PL) vs 자체 `Grid` 이동 구현; 별창·프리셋 요구가 있어 라이브러리 채택이 유력, 라이선스 검토 후 확정.

## 14. 실기 시험 환경 — 개발 서버(.45) 등록 계획

앱 실기 시험(§2 표의 작업 전부)을 위해 `.45` 개발 서버(`121.161.164.45` — CSC·CSP·CMP 동거)에 아래를 등록한다. 서버 쪽 등록이 끝나면
앱은 로그인 창에 CSC 주소·관제석 계정만 넣어 붙는다. 스키마 전제 = `sql/migrate_dispatch_groups.sql` ·
`sql/migrate_ptt_ambient_listening.sql` 적용([dispatch_center.md](dispatch_center.md) §8.1), CSC 관리 API 는 [admin_api.md](../../api/admin_api.md) §6.7.
콘솔에서는 `구성 > 사용자`(가입자·번호)·`구성 > 관제 그룹`(대표번호·호출 방식·감청/청취 범위·멤버) 화면이 같은 API 를 쓴다.

**번호 체계** — 망 신원은 가입자당 글로벌 E.164 하나다(관제석 `+82131000100x`, 대표번호 `+821310001000` — 대표번호는 TS 24.239
Flexible Alerting pilot 로 가상·미등록·포크). **내선 4자리는 망 주소가 아니라 관제 앱 주소록의 표시 라벨**이다(아래 directory.csv —
서버·콘솔에 내선 엔티티는 없다). 일반번호와 내선을 둘 다 망 주소로 두는 것(TS 23.228 implicit registration set / 별칭)은
[volte_supplementary_services.md](volte_supplementary_services.md) §9 향후 과제라, 시험은 이 규약으로 간다.

### 14.1 등록 대상

| 대상 | 값 | 비고 |
|---|---|---|
| **관제석 가입자 ①** | person `disp01`(표시명 `관제1석`, 조직 TEAM01) · VoLTE 번호(msisdn=가입 id=imsi) `+821310001001` · `sip_transport=TLS` · 내선 라벨 `1001`(앱 주소록) · PTT 번호 **미등록**(PTT 채널 시험 전 `ptt` 가입 추가) | Digest+TLS 관제 소프트폰 규약([volte_supplementary_services.md](volte_supplementary_services.md)) — `sipHa1` 발급. 등록 완료(TLS 200 OK 실측). 콘솔 역할은 가입자에 없다(역할 = 콘솔 계정, [mcptt_authorization.md](mcptt_authorization.md) §2) |
| **관제석 가입자 ②** | person `disp02`(`관제2석`) · VoLTE 번호 `+821310001002` · TLS · 내선 라벨 `1002` · PTT 번호 **미등록** | 같은 관제 그룹의 두 번째 자리 — 그룹원 띠·지정 픽업·대표번호 병렬 호출·상호 감청의 상대 |
| **관제 그룹** | `id=dg-dispatch01` · `name=관제1` · **`pilot_id=+821310001000`**(대표번호) · `service_ref=volte` · `alert_mode=parallel` · `no_answer_sec=30` · `busy_members=skip` · `overflow_target=NULL` · `listen_visibility=hidden` · members `[{+821310001001, alert_order 0}, {+821310001002, 1}]` · **`monitor_scope=none` · `ptt_listen=none`(등록 시점)** — 감청·청취 시험 단계에서 콘솔 `구성 > 관제 그룹` 에서 manager 계정으로 `all` 로 올린다 | 등록 완료 — 대표번호 병렬 포크 실측(두 관제석 동시 링·한쪽 응답·다른 쪽 CANCEL). 멤버 편입으로 두 가입자의 `pickup_group=dg-dispatch01` 이 설정돼 있다 |
| **PTT 청취 자격** | 두 가입자의 `ptt_user_profile.allow_ambient_listening=1` | `PUT /api/v1/users/{pid}/ptt/{msisdn}/profile`(콘솔 `구성 > 사용자` PTT 프로파일) — 없으면 PTT 청취 403. PTT 가입 추가 후 |
| **PTT 그룹(멤버)** | 사내 시험 그룹 `g002` 에 두 PTT 번호를 멤버로 추가(`g001` 은 협력업체 단말이 포함돼 시험에 쓰지 않는다) | ① 채널 카드·MCData 그룹 스레드·affiliation·conference 구독 대상 |
| **PTT 그룹(청취 범위)** | 두 관제석이 **멤버가 아닌** 그룹(예: `g003`) — 다른 PTT 단말 2대(`+82500000001/2` 등 기존 시험 계정) 멤버 | ② 진행 중 행 [청취] → PTT 청취 창(`ptt_listen=all`). 로스터 표시는 서버의 청취 범위 구독 인가(§13) 전까지 "미상" |
| **감청·픽업 대상 통화 가입자** | 기존 VoLTE 시험 가입자 `+821300000001/2`(사내 단말) — 관제 그룹 **밖** | ④ 진행 중 행 [청취](Join, `monitor_scope=all`)·전달 대상. 대표번호 착신은 이들 중 하나가 `+821310001000` 으로 발신 |
| **접속서비스** | VoLTE `sip_transport=TLS`+`media_srtp=optional` 서비스 하나(대표번호 `service_ref` 가 이것), PTT 서비스 하나. 인증서는 개발 자가서명 → 앱 로그인 창 "서버 인증서 검증" 끔 또는 CA PEM 지정 | `pickup_feature_code`(기본 `**`)·`transfer_allowed=1` 확인 — 앱 설정의 피처코드와 일치해야 한다 |

### 14.2 앱이 서버에서 기대하는 것

- `POST /oauth/token`(PKCE) 로그인 → `GET /provisioning/me` 응답에 `services[volte, ptt]`(각 `sipHost/sipPort/transport/domain/msisdn/imsi/authId/sipHa1`,
  PTT 는 `mcpttId`) + **`dispatch` 블록** `{present:true, groupId:"dg-dispatch01", groupName:"관제1", pilotId:"+821310001000", monitorScope:"none"|"all",
  pttListen:"none"|"all", listenVisibility:"hidden"}`. `present=false` 면 앱은 소프트폰 모드(§6).
- GMS `listGroups(<PTT 번호 tel: URI>)` 가 `g002` 를 돌려준다(카드 소스). 대표번호 `+821310001000` 과 그룹원 번호는 앱이 `dialogWatch` 로 구독하므로
  CSP 의 dialog 이벤트 인가(`monitor_scope`)가 두 관제석 모두에 적용돼야 한다.
- 그룹원 목록은 서버 API 가 없어(§13) 앱 로컬 `%APPDATA%\CIMS\dispatch-desktop\directory.csv` 에 둔다. `number` 는 실제로 다이얼되는
  망 주소(E.164)이고 내선 4자리는 `name` 에 병기하는 표시 라벨이다(단축 다이얼 열은 §13 후속) — 시험용 내용:
  ```
  kind,number,name,tags
  ext,+821310001001,관제1석 1001,member
  ext,+821310001002,관제2석 1002,member
  ext,+821300000001,시험단말A,
  ext,+821300000002,시험단말B,
  external,02-120,교통상황실,
  ptt,+82500000001,PTT단말1,
  ptt,+82500000002,PTT단말2,
  ```

### 14.3 시험 순서(일괄)

1. 두 관제석 로그인·등록(PTT·VoLTE 점등 녹색) → ① 카드 `g002` 참여·PTT 발언·로스터 → MCData 그룹 SDS 왕복(✓✓).
2. 시험단말 → `+821310001000` 발신 → 두 관제석 착신 배너(주황)·③ 대기열 → 한쪽 응답, 다른 쪽 [당겨받기]/무응답 부재 행.
3. 시험단말A ↔ B 통화 → ④ 진행 중 행 [청취] → 감청 창(caller/callee 두 줄) → 종료 시 자동 닫힘. 그룹원 띠에서 상대 관제석 통화 [청취].
4. 관제석 ① 착신 링 중 ②가 [픽업](지정)·`**`(그룹) → 픽업 행. 통화 중 [전달▾] blind / [상담 전달] attended.
5. 청취 범위 그룹(예: `g003`) 세션(PTT 단말 2대) → ② 행 [청취] → PTT 청취 창(발언자·참가자). 사설콜(반이중·전이중)·애드혹(주소록 ☐) 카드.
6. SMS·LMS(`+821310001001`↔`+821310001002`, 70자 초과) · 오디오 라우트 🎧/🔊 · 핫키(Ctrl+Space PTT, F9 응답, F8 픽업, F10 종료) · 배치 프리셋 저장/복원.

