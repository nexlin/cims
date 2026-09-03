# 관제조작반 앱 UI — Windows 데스크톱(WPF) 화면 설계

> 관제 센터 기능([dispatch_center.md](dispatch_center.md))과 보조 서비스([volte_supplementary_services.md](volte_supplementary_services.md))의
> **관제사 화면**을 정의한다. 앱은 `windows/dispatch-desktop`(WPF, MVVM, `net10.0-windows`)이며 SDK 는
> [ue_sdk.md](ue_sdk.md) 의 .NET 파사드 `CimsUe.dll`(→ C API `cimsue_c.h` → `libcimsue`) 만 참조한다. 요구↔코어 API 대응표는
> ue_sdk.md §7 이 정본이고, 이 문서는 그 위의 **화면·조작·표시 규약**을 정한다. Android 태블릿 관제 앱은 같은 구획을
> 다른 밀도로 배치한다(§11).

## 1. 범위와 결론

- **화면 한 장, 스크롤 없음.** 콘솔 관제 캔버스([../console_platform.md](../console_platform.md) §3.0)와 같은 규율 — 관제사는
  위치를 기억하고 쓰며, 알람·착신·발언자가 화면 밖으로 밀려나면 결함이다. 설계 캔버스 1920×1080, 창이 더 크면 비율
  유지로 채우고 더 작으면 재배열 없이 그대로(스크롤은 창 전체가 아니라 목록 구획 안에서만).
- **다섯 구획 고정**: ① 통화 보드(BLF) · ② 통화 데스크(대표번호 대기열 + 내 통화 + 발신) · ③ 활성 세션(VoLTE 통화 목록 +
  PTT 세션 목록) · ④ PTT 채널(내가 발언하는 세션) · ⑤ SDS·디렉터리. 구획 위치는 고정이고 크기만 운영자가 조정한다(§3.3).
- **감청·청취는 구획이 아니라 팝업 창이다.** 진행 중 세션은 ③ 목록(또는 ① 타일)에 보이고, 행 안의 [청취] 가
  **감청 창**(§5)을 띄운다. 듣기만 하는 세션은 전부 창으로, 발언하는 세션은 전부 ④ 카드로 — "듣는 것"과 "말하는 것"의
  자리를 나눈다.
- **관제석은 발신 주체다.** ② 발신 패널이 VoLTE 통화·PTT 사설콜·PTT 애드혹 그룹콜 세 모드로 다른 단말을 호출한다(§4.2).
- **앱은 화면·장치·수명주기만 안다.** SIP/RTP/floor/SDS/CSC 는 전부 SDK — 앱 코드에 프로토콜 상수(응답 코드 의미 해석 표
  §8 제외)가 나타나면 경계 위반이다(ue_sdk.md §1 경계 규칙).
- **합법감청 UI 원칙**: 감청·PTT 청취는 관제사에게는 **항상 명시**(창·상단 바 칩·시간·대상), 당사자 은닉은 서버 몫이다.
  앱은 `listenVisibility` 를 표시만 하고 동작을 바꾸지 않는다.
- **오디오 배치**(ue_sdk.md §6.3): 통화·감청 창은 헤드셋(라우트 0), PTT 는 데스크 스피커(라우트 ≥1) 기본. 세션마다
  라우트를 바꿀 수 있다.

## 2. 관제사 작업 모델 (화면이 지원해야 하는 일)

| 작업 | 트리거 | 화면 위치 | 코어 API (ue_sdk.md §7) |
|---|---|---|---|
| 대표번호 착신 응대 | INVITE `P-Called-Party-ID`=pilot | ② 대기열 + 착신 배너 | `CallInfo.calledParty` → `answer` |
| 링 중인 대표번호 호 당겨받기 | 대기열의 다른 관제사 링잉 호 | ② 대기열 [당겨받기] | `pickup(code, pilot)` |
| 그룹원 내선 링잉 지정 픽업 | ① 타일 / ③ VoLTE 행 ringing | [지정 픽업] | `pickup(code, ext)` |
| VoLTE 발신 | 내선·번호 | ② 발신 [VoLTE 통화] | `dial` |
| PTT 사설콜 발신 | PTT 사용자 1명 | ② 발신 [PTT 사설콜] → ④ 카드 | `startPrivateCall(peer, {fullDuplex, emergency})` |
| PTT 애드혹 그룹콜 발신 | PTT 사용자 N명 | ② 발신 [PTT 애드혹] → ④ 카드 | `joinGroupCall("adhoc-<나>-<epoch>", {members[]})` |
| 호 전달 blind / attended | 통화 중 | ② 통화 카드 [전달]·[상담 전달] | `transfer` / `dial`+`transferAttended` |
| 진행 중 VoLTE 통화 청취(감청) | ③ VoLTE 행 / ① 타일 confirmed | [청취] → 감청 창 | `dialogWatch` → `join(dlg)` |
| PTT 그룹 청취(타 그룹) | ③ PTT 행, `pttListen` 범위 | [청취] → 감청 창 | `joinGroupCall(listenOnly)` |
| PTT 그룹 발언(자기 그룹) | 채널 선택 + PTT 키 | ④ 카드 PTT 버튼 / 핫키 | `joinGroupCall`·`floorRequest/Release` |
| 긴급 상황 인지 | emergency/imminent/alert | 전역 배너 + ③ 행·④ 카드 배지 | `CallInfo.mcptt.emergency/imminentPeril`, `onMessage(alert-ind)` |
| 그룹·1:1 문자 | SDS 수신/발신 | ⑤ 메시지 탭 | `sendGroupSds`·`onSds`·`sendSdsNotification` |
| 내선·PTT 사용자·그룹 찾기 | 디렉터리 검색 | ⑤ 디렉터리 탭·② 발신 대상 선택 | — |
| 장치·핫키 | 설정 | 상단 바 → 설정 창 | `audioDevices`·`setAudioDevices`·`addPlaybackRoute`·`setCallRoute` |

## 3. 화면 구성

### 3.1 캔버스 (1920×1080)

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐ 56
│ 상단 바  [CIMS 관제]  홍길동 · 내선 1002 · 관제1(dg-1) · 대표 7000   ●VoLTE ●PTT  [감청 중 1 ▾]  🎧 🔊  ⌨PTT  14:32 ⚙ │
├────────────────────────┬─────────────────────────────────────┬──────────────────────────┤
│ ① 통화 보드 (BLF)       │ ② 통화 데스크 (대표 7000)             │ ③ 활성 세션              │
│ [내 그룹 ▾] [검색…]     │ ┌ 대기열 ───────────────────────────┐ │ VoLTE 통화 (범위 all)     │
│ ┌────┐┌────┐┌────┐┌────┐│ │ 🔔 010-9876-5432 → 7000  00:12   │ │ 1003 ↔ 010-1234  03:41   │
│ │1001││1002││1003││1004││ │    링잉: 1001,1004    [당겨받기] │ │        통화 · 은닉  [청취]│
│ │대기 ││ 나 ││통화││링잉││ └─────────────────────────────────┘ │ 1005 ↔ 02-555   11:08    │
│ └────┘└────┘└────┘└────┘│ ┌ 내 통화 ──────────────────────────┐ │        대표 7000    [청취]│
│ ┌────┐┌────┐┌────┐┌────┐│ │ ▶ 010-9876-5432 (대표 7000 착신)  │ │ 1011 ↔ 1007      00:48   │
│ │1005││1006││1007││1008││ │   02:15 🎧 [보류][음소거][DTMF]    │ │        통화         [청취]│
│ │통화││보류││대기││대기││ │   [전달 ▾][상담 전달]      [종료]  │ │ PTT 세션 (범위 listed)    │
│ └────┘└────┘└────┘└────┘│ │ ⏸ 1006 (보류) 05:02 [재개][종료]  │ │ 순찰1  멤버 진행 12명 ⚠  │
│  …                     │ └─────────────────────────────────┘ │                     [채널]│
│                        │ ┌ 발신 [VoLTE 통화|PTT 사설콜|PTT 애드혹]│ 야간   청취 진행 5명       │
│                        │ │ 대상 [1005 김순경 ……] [발신]        │ │                     [청취]│
│ 범례 ▢대기 ▣통화 ◐보류  │ │ 사설콜: (●)반이중 ( )전이중 [ ]긴급  │ ├──────────────────────────┤
│      ◉링잉 ✚감청가능    │ │ 애드혹: 선택 3명 [1004][1008][1010]│ │ ④ PTT 채널               │
│                        │ └─────────────────────────────────┘ │ ┌ 순찰1 (멤버) ─────────┐ │
│                        │                                     │ │ 발언: 김순경 ▮▮▮  02:10 │ │
│                        │                                     │ │ [  PTT  ] 🔊 [이탈]    │ │
│                        │                                     │ └──────────────────────┘ │
│                        │                                     │ ┌ 사설콜 · 윤순경 전이중 ┐ │
│                        │                                     │ │ 01:12 🎧        [종료] │ │
│                        │                                     │ └──────────────────────┘ │
│                        │                                     │ ┌ 애드혹 · 3명 ─────────┐ │
│                        │                                     │ │ 발언: 최순경  [ PTT ]  │ │
│                        │                                     │ └──────────────────────┘ │
├────────────────────────┴─────────────────────────────────────┴──────────────────────────┤
│ ⑤ [메시지] [디렉터리]                                                                     │ 280
│ 스레드: 순찰1 · 야간 · 1003     │ 순찰1 — 김순경: 현장 도착 ✓✓ 14:20 … [입력………………] [📎][전송]        │
└──────────────────────────────────────────────────────────────────────────────────────────┘
   560                       760                                 600

  ┌ 감청 창 (팝업, 별창 · 세션당 1개 · 두 번째 모니터 가능) ───────────────┐ 440×260
  │ 감청 — 1003 이순경 ↔ 010-1234-5678         은닉  03:41   [_][×]    │
  │ caller ▮▮▮▮▮▮▮▯▯▯ 발신자 010-1234                                    │
  │ callee ▮▮▯▯▯▯▯▯▯▯ 착신자 1003 이순경                                  │
  │ 🎧/🔊  [양쪽 ▾]                                        [청취 종료]    │
  └──────────────────────────────────────────────────────────────────────┘
```

세로 예산: 상단 바 56 + 본문 744 + 하단 ⑤ 280 = 1080. 가로: ① 560 · ② 760 · ③/④ 600(③ 상단 300 · ④ 하단 444).
구획은 WPF `Grid` 의 `*` 비율이며 `GridSplitter` 로 경계만 움직인다(§3.3). 어떤 구획도 창 밖으로 자라지 않는다 —
목록이 넘치면 **그 구획 안에서** 스크롤한다(①·②대기열·③·④·⑤). 감청 창은 캔버스 예산 밖(별창)이다.

### 3.2 전역 요소

| 요소 | 내용 |
|---|---|
| 상단 바 | 데스크 신원(`Profile.displayName`·내선(volte msisdn)·`dispatch.groupName(groupId)`·`pilotId`), 계정 등록 점등 2개(volte/ptt — `RegState` 색: 회색 미등록·노랑 등록중·녹색 등록·빨강 실패, 툴팁에 코드·사유), **감청 중 N 칩**(보라 — 열린 감청 창 목록 드롭다운, 클릭 → 창 복원, §5), 오디오 요약(헤드셋/스피커 장치명, 클릭 → 설정), PTT 핫키 표시, 시각, 설정 ⚙ |
| 착신 배너 | 화면 상단에 슬라이드 — "대표번호 7000 착신 · 010-9876-5432 · [응답 F9] [거절]". 대표번호 착신(`calledParty`=pilot)은 주황, 내선 직접 착신은 파랑, PTT 사설콜 착신은 청록. 여러 착신은 스택(최신 위). 응답 핫키는 배너 최상단 호 |
| 긴급 배너 | 빨강(emergency) / 주황(imminent peril) / 자주(alert) 풀폭 배너 — 그룹명·개시자·경과. ③ 행·④ 카드 배지와 동기. 취소(`emergency-ind=false` re-INVITE / `alert-ind=false`) 수신 시 해제 |
| 토스트 | 명령 실패의 사유(§8 사전) — 우하단, 6초, 오류는 수동 닫기. 원문 코드는 접힘(▸ 상세) |
| 상태 색상 | 대기 회색 · 링잉 주황(점멸) · 통화/발언 녹색 · 보류 파랑 · 감청 보라 · 청취 청록 · 긴급 빨강. 색맹 대비를 위해 아이콘 병기(▢ ◉ ▣ ◐ ✚ 🔊 ⚠) |
| 시간 | 모든 진행 항목은 `mm:ss` 경과 시간(1초 갱신) — 링잉·통화·감청·PTT 세션 |
| 신원 표시 | 내선 → 디렉터리의 표시 이름(`users.name`) 병기 "1003 이순경"; 외부 번호는 E.164 → 국내 표기. 항상 원 값(URI user part)을 툴팁으로 |

### 3.3 크기 조정과 저장

- `GridSplitter` 4개(①|②, ②|③④, ③/④, 본문/⑤) — 최소 크기: ① 400, ② 560, ③④ 480, ⑤ 200, ③ 200, ④ 240.
- 위치 잠금 없음(구획 순서 고정), 비율만 `%APPDATA%\CIMS\dispatch-desktop\settings.json` 에 저장. "기본 배치" 복원 버튼(설정).
- 감청 창의 마지막 위치·크기·모니터를 세션 종류(VoLTE/PTT)별로 기억한다 — 두 번째 모니터에 놓아둔 관제석이 기본 사용례.

## 4. 구획 상세

### 4.1 ① 통화 보드 (BLF)

**목적**: 범위 안 내선의 통화 상태를 한눈에, 링잉이면 픽업·통화 중이면 청취로 이어지는 진입점. ③ VoLTE 목록과 소스는 같고
(내선 중심 격자 vs 세션 중심 행) 조작은 동일하다.

- **소스**: 감시 대상 내선마다 `dialogWatch(acc, ext, true)`(RFC 4235) → `onDialogInfo`. 대표번호 AoR 도 구독한다(§4.2 대기열).
  구독 대상 목록(내 그룹 멤버 + `monitorScope=listed/all` 의 타 그룹 멤버)의 공급은 §12 — 초기형은 디렉터리(⑤)에
  있는 내선 전부를 구독하고 403 인 대상은 타일에 "범위 밖" 표시로 접는다.
- **타일**: 내선 · 표시 이름 · 상태(dialog `state`: 없음/terminated=대기, early=링잉, confirmed=통화) · 상대(`remoteIdentity`) ·
  경과. 자기 내선은 "나" 로 고정 표시(조작 없음). 통화 중이고 `monitorScope` 안이면 ✚ 감청 가능 배지.
- **조작**(타일 클릭 → 팝오버): 대기 → [VoLTE 발신]·[PTT 사설콜](PTT 번호가 있으면) · 링잉 → [지정 픽업 `<code><ext>`] ·
  통화 → [청취](감청 창 §5) · 어느 상태든 [문자].
- **필터**: [내 그룹 / 범위 전체] 토글, 검색(내선·이름). 정렬은 내선 순 고정(위치 기억).
- **밀도**: 타일 128×72 → 560px 폭에 4열, 744−크롬 높이에 8행 = 32 타일 무스크롤. 그 이상은 구획 안 스크롤(운영 권고: 한
  데스크 감시 대상 32 이내).
- `state=terminated` 는 즉시 대기로 바꾸되 3초간 "종료" 잔상(누가 끊었는지 확인용).

### 4.2 ② 통화 데스크

**대기열 (상단)** — 대표번호 AoR dialog 구독(dispatch_center.md §4.5)으로 얻은 **링잉·응답 상태**:
- 행: 발신자 · `→ 대표번호` · 링 경과 · 현재 링잉 중 그룹원(포크 대기 leg — 표준형 RLS 전까지는 각 내선 dialog 의 early 로 추정) ·
  [당겨받기]. 응답되면 "응답: 1004 최순경" 으로 3초 표시 후 목록에서 제거(또는 내 통화로 승격).
- [당겨받기] = `pickup(code, pilotId)`(지정 픽업 — 대표번호 링잉 호, dispatch_center.md §4.4 `PickUpFork`). 자기 단말에도 링잉 중이면
  그냥 [응답].
- sequential 모드(`alert_mode=sequential`)는 서버가 한 명씩 울리므로 "링잉: 1001" 한 명만 보인다 — 화면 규칙은 같다.
- 큐 비어 있음 상태: "대기 호 없음" + 오늘 응대 건수(로컬 집계).

**내 통화 (중단)** — `calls()` 중 VoLTE 통화(`!isMcptt && !listenOnly`) 카드. 최대 4개(넘으면 스크롤, 운영상 1 활성 + 보류 n):
- 카드: 방향 아이콘 · 상대 · **착신 경로 배지**(`calledParty`=pilot 이면 "대표 7000 착신", 내선 직접이면 없음) · 상태 · 경과 · 🎧/🔊 라우트 토글.
- 조작: [응답](Incoming) · [거절 486] · [보류]/[재개] · [음소거] · [DTMF 패드] · [전달 ▾ blind / 상담] · [종료].
- **blind 전달**: [전달] → 대상 입력(디렉터리 선택 가능) → `transfer(callId, target)`. 진행은 카드에 "전달 중 → 1003" 표시, 서버가
  전달 완결 후 BYE 하면 카드 소멸·토스트 "전달 완료". 실패(대상 거절)면 원 통화 유지·토스트.
- **attended 전달**: [상담 전달] → 원 통화 자동 보류 + `dial(target)` 상담 통화 카드 생성(배지 "상담") → 상담 중 [전달 완결] =
  `transferAttended(원 callId, 상담 callId)` / [취소] = 상담 종료 + 원 통화 재개.
- 활성 통화가 있는데 새 착신을 응답하면 기존 통화는 자동 보류(설정 가능: 자동 보류 / 거절).

**발신 (하단)** — 세 모드의 세그먼트 컨트롤. 대상 입력은 디렉터리 자동완성(내선·번호·이름·PTT 번호) 공통, 최근 발신 5개 칩.

| 모드 | 대상 | 옵션 | 개시 | 결과 위치 |
|---|---|---|---|---|
| VoLTE 통화 | 내선/번호 1개 | — (영상은 F3) | `dial(acc_volte, target)` | ② 내 통화 카드 |
| PTT 사설콜 | PTT 사용자 1명 | (●)반이중 floor / ( )전이중 `mc_no_floor_ctrl` · [ ]긴급 | `startPrivateCall(acc_ptt, peer, {fullDuplex, emergency})` | ④ 사설콜 카드 |
| PTT 애드혹 | PTT 사용자 N명(칩, 최소 1) | [ ]긴급 | `joinGroupCall(acc_ptt, "adhoc-<내 PTT 번호>-<epoch초>", {members: tel: URI[]})` | ④ 애드혹 카드 |

- 애드혹 임시 그룹 id 는 앱이 만든다(mcptt_emergency_modes.md §6 규약 `adhoc-<발신자번호>-<epoch초>`; `adhoc-`·`priv-` 는 편성 그룹
  예약어). 세션은 채널 영속·affiliation·로스터 구독 대상이 아니다 — 카드는 세션과 함께 사라진다.
- 자격 선차단: 사용자 프로파일(CMS user-profile — `allow_adhoc_call`·`allow_emergency_private_call`·`PrivateCall/EmergencyCall/
  MCPTTPrivateRecipient`)에 없으면 해당 모드/옵션을 비활성 + 툴팁 사유. 미인가 403 은 §8 사전. 프로파일 파싱은 §12(코어 API 후속) —
  그 전엔 옵션을 열어 두고 403 문구로만 대응한다.
- 긴급 사설콜은 `UsePreConfigured` 모드면 대상이 사전 지정 수신자로 고정된다(mcptt_emergency_modes.md §7) — 대상 입력을 잠그고 수신자를 표시.

### 4.3 ③ 활성 세션

**목적**: 범위 안에서 **지금 진행 중인 것**을 세션 단위로 한 목록에 — 무엇을 들을지(감청/청취) 고르는 자리. 두 목록을 위아래로 둔다
(높이 배분은 내용 비율, 각 목록은 자체 스크롤).

**VoLTE 통화 목록** — dialog 이벤트를 **세션 행**으로 결합:
- 행: `A ↔ B`(내선은 이름 병기, 외부는 번호) · 상태(링잉/통화/보류) · 경과 · 배지(대표 7000 경로 — B leg 의 `P-Called-Party-ID` 를 dialog
  `remote`/`local` 정보로는 알 수 없으므로 대표번호 AoR dialog 와 Call-ID·시각으로 상관; 없으면 생략) · 은닉/투명 · 조작.
- **결합 규칙**: 감시 대상 두 내선이 서로 통화하면 dialog 가 두 개(각 AoR 의 leg — Call-ID 가 다르다) 온다. `remoteIdentity` 가 서로를
  가리키고 상태 전이 시각이 근접하면 한 행으로 합친다(어느 leg 의 `DialogInfo` 로도 Join 가능 — dispatch_center.md §5.3). 한쪽만
  감시 대상이면 행은 그 leg 하나다.
- 조작: 링잉 → [지정 픽업](`pickup(code, ringingExt)`) · 통화(confirmed) & 범위 안 → **[청취]** = `join(acc, targetUri, dlg)` → 감청 창(§5) ·
  early/범위 밖 → [청취] 비활성(툴팁 "연결 전" / "감청 범위 밖"). 이미 듣고 있는 세션은 [청취 중 — 창 열기].
- 정렬: 링잉 먼저, 그다음 시작 시각 역순. 종료 행은 3초 잔상.

**PTT 세션 목록** — 범위 안 그룹(멤버 그룹 + `pttListen` 대상 그룹) 한 행씩:
- 행: 그룹명 · 배지(멤버 / 청취) · 세션 상태(진행 n명 / 대기 / 미상) · 긴급/임박 배지 · 경과 · 조작.
- **상태 소스**: 멤버 그룹은 `subscribeConference`(RFC 4575) 의 `onRoster` (참가자 수·진행 여부). 청취 대상 그룹도 같은 conference
  구독을 쓴다 — 서버가 비멤버 관제사의 구독을 `CanListenPtt` 범위로 인가해야 한다(§12 서버 전제, dispatch_center.md §10). 그 전까지
  청취 그룹은 "미상" 으로 두고 [청취] 시 480 이면 "진행 중 통화 없음" 으로 알려 준다.
- 조작: 멤버 그룹 → **[채널]**(④ 카드로 이동·미참여면 `joinGroupCall`) · 청취 그룹 → **[청취]** = `joinGroupCall(listenOnly)` → 감청 창(§5)
  (`floor` 요청 없음, Taken `Permission=0`) · 이미 듣고 있으면 [청취 중 — 창 열기].
- 정렬: 긴급 먼저, 진행 중, 대기/미상.

### 4.4 ④ PTT 채널

**목적**: 관제사가 **발언하는** 세션만 — 멤버 그룹 채널, 사설콜, 애드혹. 듣기만 하는 세션은 여기 없다(감청 창).

- **채널 목록 소스**: GMS `listGroups(userUri)`(멤버 그룹, ⑤ 디렉터리와 공유). 카드는 운영자가 고른 채널만 표시(설정 → 채널 선택,
  기본 = 멤버 그룹 전부) + 진행 중 사설콜·애드혹 카드(세션 동안만).
- **멤버 채널 카드**: 그룹명 · 배지 "멤버" · affiliation 상태 · 세션 상태(대기 / 진행 — 현재 발언자 이름·레벨·경과) · 긴급/임박 배지 ·
  **PTT 버튼**(누르는 동안 `floorRequest`, 떼면 `floorRelease`; 상태 색: 대기 회색·요청 노랑·**발언 중 녹색**·대기열 "n번째" 파랑·거부 빨강 1초) ·
  라우트 토글(기본 🔊 스피커) · 음량 · [참여]/[이탈](`joinGroupCall`/`leaveGroupCall`) · [로스터 ▾](`onRoster` 참가자 — 청취 멤버는
  `listenVisibility=visible` 일 때만 `listener` 역할로 보인다, 앱은 그대로 표시).
- **사설콜 카드**(`CallInfo.mcptt.privateCall`): 상대 이름·번호 · 배지 "사설콜" + "전이중"(`noFloorCtrl`) 또는 "반이중" · 경과 · 라우트(기본 🎧
  헤드셋 — 1:1 이라 통화에 가깝다) · 반이중이면 PTT 버튼, 전이중이면 [음소거] · [종료]. 착신 사설콜은 착신 배너(청록)에서 응답
  (`autoAnswerMcptt` 는 관제석에서 기본 꺼 둔다 — 사설콜은 관제사가 받는다; 그룹콜 자동 수락과 분리하는 설정 항목은 §12).
- **애드혹 카드**(`groupId` 가 `adhoc-` 접두): "애드혹 · N명" · 멤버 칩(응답 상태는 in-dialog NOTIFY 로스터로) · 발언자·레벨 · PTT 버튼 ·
  긴급 배지 · [종료]. 마지막 멤버 이탈로 서버가 세션을 걷으면 카드 소멸.
- **선택 채널**: 카드 하나가 "선택" 상태(테두리) — 전역 PTT 핫키는 선택 채널에 적용. 진행 중 애드혹/사설콜(반이중)이 생기면 자동 선택
  (ptt-client 의 "애드혹 우선" 과 같은 규칙), 끝나면 이전 선택으로 복귀.
- **floor 이벤트 표시**: Granted → 버튼 녹색 + "발언 중 mm:ss"(Duration 남은 시간 게이지) · Taken → 발언자 이름(로스터 매칭) ·
  Denied/Revoked → 사유(`causeText`) 카드 하단 한 줄 + 버튼 색 · QueuePosition → "대기 n" · TalkLimit → 게이지 빨강 ·
  RequestTimeout → 회색 복귀.
- **긴급**: `mcptt.emergency`/`imminentPeril` 인 세션은 카드 테두리 빨강/주황 + 전역 배너. 관제사의 긴급 개시(`GroupCallOptions.emergency`)는
  카드 메뉴 [긴급 호출](확인 대화상자) — 프로파일 자격 없으면 403 → 사전 문구.

### 4.5 ⑤ SDS · 디렉터리

**메시지 탭** — [mcdata_messaging.md](mcdata_messaging.md) §5 의 앱 동작을 데스크톱 밀도로:
- 좌 스레드 목록(그룹 = `groupUri`, 1:1 = 발신자 — `threadKeyOf` 규칙 동일) · 미읽음 배지 · 우 대화(발신 말풍선 상태 🕓→✓→✓✓/⚠ 재전송) ·
  입력 + [📎](FD — 파일 열기 대화상자) + [전송]. 그룹 스레드 수신 말풍선 위 발신자 라벨.
- 발신: `sendGroupSds(acc, groupId, text, requestDelivery)` → 결과는 `onRequestResult`(token 상관, 2xx=SENT) · 수신 disposition 요청이면
  `sendSdsNotification(delivered)` 자동 회신 · `onSds(notification)` → ✓✓.
- 착신 배너·긴급 배너 중에도 메시지 탭 미읽음 배지는 상단 바에 병기(⑤ 가 접혀 있을 때 대비).
- 보관: 로컬 SQLite(`%APPDATA%\CIMS\dispatch-desktop\messages.db`) — 최근 30일(설정). 서버 보관·콘솔 모니터링과 독립.

**디렉터리 탭** — 세 목록(탭 안 세그먼트): **내선**(관제 그룹원 — 이름·내선·BLF 상태·[발신][문자][지정 픽업(링잉 시)]) · **PTT**(사용자:
이름·PTT 번호·[사설콜][애드혹에 추가 ☐]·[문자] / 그룹: 이름·멤버 수·[채널에 추가][문자]) · **최근**(발신·착신·부재 — 시각·상대·종류(VoLTE/사설/애드혹)·
경로(대표/직접)·[재발신]). 검색은 전 목록 공통. 애드혹 대상 다중 선택은 이 탭의 체크 → ② 발신 패널 칩으로 반영.
디렉터리 소스는 §12(내선 목록·PTT 사용자 목록 공급) 확정 전까지 GMS 그룹 문서(멤버) + 프로비저닝 + 로컬 수동 등록(설정 → CSV).

## 5. 감청 창 (팝업)

듣기만 하는 세션 하나 = 창 하나. 주 창과 별개의 비모달 `Window`(기본 440×260, 크기 조절·이동 가능, 두 번째 모니터에 두는 것이 기본 사용례).

| 종류 | 진입 | 내용 | 종료 |
|---|---|---|---|
| **VoLTE 감청** | ③ VoLTE 행·① 타일 [청취] → `join(dlg)` → `CallInfo.listenOnly && joinedDialog≠""` | 제목 "감청 — A ↔ B" · 은닉/투명 배지(`listenVisibility`) · 경과 · 두 줄 `caller`/`callee`(RFC 5576 `label`) 각각 이름·**레벨 미터**·활성 점(`MediaSource.active/level` — U10 관측 API 후속 시 실시간, 그 전엔 `active` 만) · 라우트 토글(기본 🎧) · [양쪽 ▾](표시용 — tap 모드는 서버 운용값) · [청취 종료] | [청취 종료]·창 닫기 = `hangup`. 원 통화 종료 → 서버 BYE → "통화 종료됨" 3초 후 창 자동 닫힘 |
| **PTT 청취** | ③ PTT 행 [청취] → `joinGroupCall(listenOnly)` | 제목 "청취 — 그룹명" · 배지 "청취 전용" · 발언자 이름·레벨·경과(`onFloor` Taken) · 참가자 수(`onRoster`) · 긴급 배지 · "발언 요청 불가(Permission=0)" 고정 문구 · 라우트 토글(기본 🔊) · 음량 · [청취 종료] | [청취 종료]·창 닫기 = `leaveGroupCall`. 480 이면 창을 띄우지 않고 토스트 |

- **창을 최소화해도 청취는 계속된다** — 상단 바 "감청 중 N" 칩이 열린 창을 나열하고 클릭 시 복원. 창 닫기(×)는 종료다(오조작 방지가
  필요하면 설정 "닫기 전 확인").
- 감청 창은 포커스를 훔치지 않는다(새 이벤트가 있어도 뒤에 뜬다). 주 창의 착신·긴급 배너가 우선.
- 여러 창의 소리는 라우트별로 섞인다 — 기본값이 VoLTE 감청 🎧 / PTT 청취 🔊 인 이유. 동시 청취 상한 기본 4(설정) — 넘으면 경고.
- 앱은 감청 이력을 로컬에 남기지 않는다(감사 정본은 서버 `E-AUD-016`).
- 영상(F3): VoLTE 감청 창 아래로 격자(caller | callee)가 붙어 창이 커진다(§9).

## 6. 시작·로그인·프로비저닝

```
[시작] → 단일 인스턴스 확인(명명 Mutex, 둘째 실행은 기존 창 활성화)
      → 저장 토큰(DPAPI) 있으면 refresh → 없으면 로그인 창(아이디/비밀번호 → CscClient.login PKCE)
      → fetchProfile(/provisioning/me) → services[volte, ptt] → Engine.start → addAccount×2 → register×2
      → dispatch 블록 → 데스크 신원·범위 적용 → dialogWatch(내선들·대표번호) · affiliate(멤버 그룹) · subscribeConference(멤버·청취 그룹)
      → 메인 화면
```

- 로그인 창: 아이디·비밀번호·CSC 주소(기본은 마지막 값, 고급 접힘)·"자동 로그인" 체크(refresh token 만 DPAPI 저장 — 비밀번호·H(A1) 는
  저장하지 않는다; `sipHa1` 은 매 로그인 프로파일에서 받는다).
- `dispatch.present=false`(관제 데스크 아님) → 메인은 뜨되 ①③ 과 ② 대기열을 "관제 데스크 미배정" 안내로 접고 통화·PTT·SDS·발신만
  동작(일반 소프트폰 모드). 운영자에게 콘솔 `관리 > 관제 그룹` 배정을 안내.
- 등록 실패(401/403/타임아웃) → 상단 점등 빨강 + 토스트 사유, 자동 재시도(백오프 5→60초). `refreshRegistration` 은 네트워크 복귀 이벤트
  (Windows `NetworkChange`)에서 즉시.
- 로그아웃: 등록 해제 → 토큰 폐기 → 로그인 창. 종료(창 닫기)는 트레이로 최소화(설정), 완전 종료는 메뉴 — 감청 창이 열려 있으면 확인.

## 7. 오디오 배치 UI (ue_sdk.md §6.3)

설정 → **오디오**:

| 항목 | UI | API |
|---|---|---|
| 마이크 | 캡처 장치 콤보 + 레벨 미터(테스트) | `setAudioDevices(capture, -2)` |
| 헤드셋(라우트 0) | 재생 장치 콤보 — 통화·VoLTE 감청 기본 | `setAudioDevices(-1, playback)` |
| 데스크 스피커(라우트 1) | 재생 장치 콤보(없음 가능) — PTT 채널·PTT 청취 기본 | `addPlaybackRoute(dev)` / `removePlaybackRoute` |
| 기본 라우트 정책 | 통화→헤드셋, VoLTE 감청→헤드셋, 사설콜→헤드셋, PTT 그룹·애드혹·PTT 청취→스피커(스피커 없으면 헤드셋) | 세션 생성 시 `setCallRoute` |
| 장치 테스트 | 각 장치로 톤 재생 | 앱 로컬(WASAPI 톤) |

- 핫플러그: 파사드 `AudioEndpoints`(`IMMNotificationClient`) → `refreshAudioDevices()` → 선택 장치가 사라졌으면 기본 장치로 폴백 + 토스트
  "헤드셋 분리됨 — 기본 장치로 전환". 다시 붙으면 자동 복귀(설정).
- 카드·감청 창의 🎧/🔊 토글은 세션별 `setCallRoute(callId, 0|routeId)` — 즉시 재결선. 라우트 1 이 없으면 토글 비활성.
- 두 장치 동시 출력의 지연·에코는 실기 검증 항목(ue_sdk.md §11) — 설정에 "스피커 지연 보정 ms" 는 두지 않는다(엔진 패치 과제).

## 8. 핫키·입력

| 기능 | 기본 | 동작 | 접점 |
|---|---|---|---|
| PTT | `Ctrl+Space`(hold) | 선택 채널 `floorRequest` / 떼면 `floorRelease` | `RegisterHotKey` + 메시지 전용 HWND, key-up 은 `GetAsyncKeyState` 폴링(핫키는 down 만 온다) |
| 응답 | `F9` | 착신 배너 최상단 호 `answer` | 전역 |
| 종료 | `F10` | 활성 통화 `hangup`(감청 창·PTT 채널은 제외) | 전역 |
| 그룹 픽업 | `F8` | `pickup(code)` | 전역 |
| 보류/재개 | `F11` | 활성 통화 토글 | 앱 포커스 시 |
| 음소거 | `F12` | 활성 통화·전이중 사설콜 `setMuted` 토글 | 앱 포커스 시 |

- 전부 설정에서 재배치, 충돌(`RegisterHotKey` 실패)은 설정 화면에 빨강 표시. 게임패드/풋스위치는 HID 키 매핑으로 같은 경로.
- PTT 키 hold 중 창 포커스가 바뀌어도 release 를 놓치지 않도록 key-up 폴링 20ms + 안전장치(Granted 후 `TalkLimit` 는 코어가 자동 Release).

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
| 애드혹 | 403 | "애드혹 그룹통화 자격이 없거나 시스템에서 꺼져 있습니다" | mcptt_emergency_modes §6 (`allow_adhoc_call`·`PttAdhocEnabled`) |
| 긴급 개시 | 403 | "긴급 호출 자격이 없습니다" | mcptt_emergency_modes §4.2·§7 |
| SDS | 403/404/408/503 | "전송 실패 — 재전송" (⚠ 탭) | mcdata §5 |
| 등록 | 401/403 | "인증 실패 — 다시 로그인" | sip_access_security |
| 등록 | 408/503 | "서버 응답 없음 — 재시도 중" | — |

## 10. 영상 (F3 — 예약)

- VoLTE 감청 창 아래 격자: 양측 영상 SSRC 2개를 2분할(caller | callee), `onVideoFrame` 콜백 렌더(WPF `WriteableBitmap`/D3DImage). 창은
  이미 별창이라 주 캔버스 예산은 유지.
- 1:1 영상 통화는 ② 통화 카드 확장(또는 별창).
- F3 전까지 감청 창에 "영상 없음(음성 감청)" 고정 문구.

## 11. 구현 구조 (WPF, MVVM)

```
windows/dispatch-desktop/
  App.xaml(.cs)                 단일 인스턴스·전역 예외·SynchronizationContext 캡처
  Shell/MainWindow.xaml         3열+하단 Grid, GridSplitter, 상단 바, 배너 레이어, 토스트 레이어
  Shell/MonitorWindow.xaml      감청 창(§5) — VoLTE/PTT 두 DataTemplate, 위치·모니터 기억
  ViewModels/
    DeskViewModel               Profile·dispatch·등록 상태·오디오 요약·감청 중 N 칩 (상단 바)
    BlfBoardViewModel           ① — DialogInfo → ExtensionTile 사전
    CallDeskViewModel           ② — QueueItem(대표번호 dialog) · CallCard(VoLTE 통화) · Dialer(3 모드)
    SessionsViewModel           ③ — VoLTE 세션 행(dialog 쌍 결합) · PTT 세션 행(conference 로스터)
    PttChannelsViewModel        ④ — ChannelCard(멤버/사설콜/애드혹) · FloorState · 선택 채널
    MonitorWindowViewModel      감청 창 하나(join 호 또는 listenOnly 그룹콜) · MediaSource 미터
    MessagesViewModel / DirectoryViewModel   ⑤
    Banners(Incoming/Emergency) · Toasts
  Models/  SessionKind 분류: isMcptt&&listenOnly→PTT 청취(창) · isMcptt&&privateCall→사설콜(④) · isMcptt&&groupId adhoc-→애드혹(④) ·
           isMcptt→멤버 채널(④) · listenOnly&&joinedDialog→VoLTE 감청(창) · 그 외 VoLTE 통화(②)
  Services/ SettingsStore(json) · MessageStore(SQLite) · HotKeyMap · AudioPolicy(라우트 기본값) · AdhocIdFactory(adhoc-<나>-<epoch>)
  Views/    구획별 UserControl, 카드·행 DataTemplate, 상태 색 리소스(§3.2)
```

- 파사드 이벤트(`CimsUe.Engine` 의 `CallStateChanged`·`FloorChanged`·`DialogInfo`…)는 이미 UI 스레드로 마샬링돼 온다(ue_sdk.md §6.4) —
  ViewModel 은 `ObservableCollection` 을 직접 갱신.
- **UI 는 코어 상태의 투영**이다: 카드·행·창은 `calls()`/`callInfo` 스냅샷과 구독 이벤트에서 파생하고 앱이 별도 상태 기계를 갖지 않는다
  (재접속·재기동 후 화면 재구성 = 스냅샷 재조회, 열려 있던 감청 창도 `calls()` 의 listenOnly 호에서 복원).
- 접근성: 모든 카드·행 조작은 키보드 도달 가능, 상태는 색+아이콘+텍스트 삼중.

## 12. Android 태블릿 밀도

같은 다섯 구획을 가로 태블릿(1280×800)에 **탭 2개**로: [관제](① 좌 · ② 우 상 · ③ 우 하) / [PTT·메시지](④ 좌 · ⑤ 우). 감청 창은
전면 시트(bottom sheet)로, 착신·긴급 배너와 PTT 하드키(UNIWA 측면 키)는 공통. 상세는 `android/dispatch-tablet` 구현 시 이 절을 확장한다.

## 13. 미해결 / 향후 과제

- **감시 대상 내선·PTT 사용자 목록 공급** — `/provisioning/me` 의 `dispatch` 블록은 그룹·범위만 준다. 자기 그룹원과 `listed` 대상 그룹원의
  내선 목록, 사설콜·애드혹 대상이 되는 PTT 사용자 목록이 필요하다. 선택지: (a) `dispatch` 블록에 `members[]`·`monitorTargets[].members[]`
  확장(CSC), (b) RFC 4662 RLS 목록 구독(표준형 — 서버 §5.2 후속, 구독 N→1). 초기형은 (a) 없이 GMS 그룹 문서 멤버·수동 CSV 로 시작.
- **서버 전제 — 청취 범위 그룹의 conference 이벤트 구독 인가**(dispatch_center.md §10): ③ PTT 세션 목록의 "진행/대기·참가자 수" 는
  RFC 4575 구독이 소스인데 현재 인가는 멤버 기준이다. `CanListenPtt` 범위의 비멤버 관제사 구독을 허용해야 목록이 채워진다(청취 leg 와
  같은 인가 축). 그 전까지 청취 그룹 행은 "미상".
- **U10 관측 API** — `MediaSource.level/active` 실시간 갱신(코어 `onCallMedia` 주기) 확정 후 감청 창 레벨 미터 활성.
- **경보(alert-ind) 파싱** — `onMessage` 의 `mcptt-info` 를 코어가 `McpttInfo` 로 해석해 이벤트로 올리는 API(현재는 본문 전달).
- **CMS user-profile 파싱 API** — `allow_adhoc_call`·`allow_emergency_private_call`·`PrivateCall/EmergencyCall` 수신자 모드를 코어가 구조로
  주면 발신 패널 선차단이 정확해진다(현재 XCAP 본문 문자열).
- **자동 수락 분리** — `AccountConfig.autoAnswerMcptt` 는 그룹콜·사설콜 공통이다. 관제석은 그룹콜 자동 수락 + 사설콜 수동 응답이 맞으므로
  코어에 사설콜 자동 수락 별도 플래그가 필요하다(그 전엔 전체 off + 그룹콜도 배너 응답).
- **대표번호 발신 표시**(`P-Preferred-Identity`=pilot — dispatch_center §10 서버 과제) 확정 시 발신 패널에 "대표번호로 발신" 토글.
- **큐/ACD**(대기열 순번·안내) — 서버 과제, 대기열 UI 는 그 데이터 모델이 오면 열만 추가.
- **영상 F3**, **ambient listening**(단말 무표시 자동응답 — 관제 앱은 개시 측), **끼어들기**(CMP 믹서 — 서버 과제).
