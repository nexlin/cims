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
- **다섯 구획 고정**: ① 통화 보드(BLF) · ② 대표번호 데스크(대기열 + 내 통화) · ③ 감청 패널 · ④ PTT 채널 · ⑤ SDS·디렉터리.
  구획 위치는 고정이고 크기만 운영자가 조정한다(§3.3).
- **앱은 화면·장치·수명주기만 안다.** SIP/RTP/floor/SDS/CSC 는 전부 SDK — 앱 코드에 프로토콜 상수(응답 코드 의미 해석 표
  §8 제외)가 나타나면 경계 위반이다(ue_sdk.md §1 경계 규칙).
- **합법감청 UI 원칙**: 감청·PTT 청취는 관제사에게는 **항상 명시**(진행 표시·시간·대상), 당사자 은닉은 서버 몫이다. 앱은
  `listenVisibility` 를 표시만 하고 동작을 바꾸지 않는다.
- **오디오 배치**(ue_sdk.md §6.3): 통화·감청은 헤드셋(라우트 0), PTT 청취 채널은 데스크 스피커(라우트 ≥1) 기본. 호마다
  라우트를 바꿀 수 있다.

## 2. 관제사 작업 모델 (화면이 지원해야 하는 일)

| 작업 | 트리거 | 화면 위치 | 코어 API (ue_sdk.md §7) |
|---|---|---|---|
| 대표번호 착신 응대 | INVITE `P-Called-Party-ID`=pilot | ② 대기열 카드 + 착신 배너 | `CallInfo.calledParty` → `answer` |
| 링 중인 대표번호 호 당겨받기 | 대기열의 다른 관제사 링잉 호 | ② 대기열 [당겨받기] | `pickup(code)` / `pickup(code, pilot)` |
| 그룹원 내선 링잉 지정 픽업 | ① 타일 ringing | ① 타일 클릭 → [지정 픽업] | `pickup(code, ext)` |
| 호 전달 blind / attended | 통화 중 | ② 통화 카드 [전달]·[상담 전달] | `transfer` / `dial`+`transferAttended` |
| 진행 중 통화 청취(감청) | ① 타일 confirmed, 범위 안 | ① 타일 → [청취] → ③ 카드 | `dialogWatch` → `join(dlg)` |
| PTT 그룹 발언(자기 그룹) | 채널 선택 + PTT 키 | ④ 채널 카드 PTT 버튼 / 핫키 | `joinGroupCall`·`floorRequest/Release` |
| PTT 그룹 청취(타 그룹) | 범위 `pttListen` 안 그룹 | ④ 채널 카드 [청취] | `joinGroupCall(listenOnly)` |
| 긴급 상황 인지 | emergency/imminent/alert | 전역 배너 + 채널 카드 배지 | `CallInfo.mcptt.emergency/imminentPeril`, `onMessage(alert-ind)` |
| 그룹·1:1 문자 | SDS 수신/발신 | ⑤ 메시지 탭 | `sendGroupSds`·`onSds`·`sendSdsNotification` |
| 내선·그룹 찾기, 발신 | 디렉터리 검색 | ⑤ 디렉터리 탭·② 다이얼 | `dial` |
| 장치·핫키 | 설정 | 상단 바 → 설정 창 | `audioDevices`·`setAudioDevices`·`addPlaybackRoute`·`setCallRoute` |

## 3. 화면 구성

### 3.1 캔버스 (1920×1080)

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐ 56
│ 상단 바  [CIMS 관제]  홍길동 · 내선 1002 · 관제1(dg-1) · 대표 7000     ●VoLTE ●PTT  🎧헤드셋 🔊스피커  ⌨PTT:Ctrl+Space  14:32  ⚙ │
├────────────────────────┬─────────────────────────────────────┬──────────────────────────┤
│ ① 통화 보드 (BLF)       │ ② 대표번호 데스크                    │ ③ 감청                   │
│ [내 그룹 ▾] [검색…]     │ ┌ 대기열 (대표 7000) ───────────────┐ │ ┌ 1003 ↔ 010-1234-5678 ┐ │
│ ┌────┐┌────┐┌────┐┌────┐│ │ 🔔 010-9876-5432 → 7000  00:12   │ │ │ caller ▮▮▮▮▯ 발신자   │ │
│ │1001││1002││1003││1004││ │    링잉: 1001,1004    [당겨받기] │ │ │ callee ▮▮▯▯▯ 착신자   │ │
│ │대기 ││ 나 ││통화││링잉││ │ 🔔 02-555-0001 → 7000    00:03   │ │ │ 03:41  🎧  은닉 [종료] │ │
│ └────┘└────┘└────┘└────┘│ └─────────────────────────────────┘ │ └──────────────────────┘ │
│ ┌────┐┌────┐┌────┐┌────┐│ ┌ 내 통화 ──────────────────────────┐ │ (영상 격자 — F3)          │
│ │1005││1006││1007││1008││ │ ▶ 010-9876-5432 (대표 7000 착신)  │ │                          │
│ │통화││보류││대기││대기││ │   02:15  🎧  [보류][음소거][DTMF]  │ ├──────────────────────────┤
│ └────┘└────┘└────┘└────┘│ │   [전달 ▾ blind/상담] [종료]      │ │ ④ PTT 채널               │
│  …                     │ │ ⏸ 1006 (보류) 05:02 [재개][종료]  │ │ ┌ 순찰1 (멤버) ─────────┐ │
│                        │ └─────────────────────────────────┘ │ │ 발언: 김순경 ▮▮▮  02:10 │ │
│ 범례 ▢대기 ▣통화 ◐보류  │ ┌ 다이얼 ─────────────────────────┐ │ │ [  PTT  ] 🔊 [이탈]    │ │
│      ◉링잉 ✚감청가능    │ │ [번호/내선………………] [발신] [그룹픽업]│ │ └──────────────────────┘ │
│                        │ └─────────────────────────────────┘ │ │ ┌ 야간 (청취) ──────────┐ │
│                        │                                     │ │ │ 발언: 박경장  PTT 불가 │ │
│                        │                                     │ │ │ 🔊 대기 00:40  [청취종료]│ │
│                        │                                     │ │ └──────────────────────┘ │
├────────────────────────┴─────────────────────────────────────┴──────────────────────────┤
│ ⑤ [메시지] [디렉터리]                                                                     │ 280
│ 스레드: 순찰1 · 야간 · 1003     │ 순찰1 — 김순경: 현장 도착 ✓✓ 14:20 … [입력………………] [📎][전송]        │
└──────────────────────────────────────────────────────────────────────────────────────────┘
   560                       760                                 600
```

세로 예산: 상단 바 56 + 본문 744 + 하단 ⑤ 280 = 1080. 가로: ① 560 · ② 760 · ③/④ 600(③ 상단 300 · ④ 하단 444).
구획은 WPF `Grid` 의 `*` 비율이며 `GridSplitter` 로 경계만 움직인다(§3.3). 어떤 구획도 창 밖으로 자라지 않는다 —
목록이 넘치면 **그 구획 안에서** 스크롤한다(①·②대기열·④·⑤).

### 3.2 전역 요소

| 요소 | 내용 |
|---|---|
| 상단 바 | 데스크 신원(`Profile.displayName`·내선(volte msisdn)·`dispatch.groupName(groupId)`·`pilotId`), 계정 등록 점등 2개(volte/ptt — `RegState` 색: 회색 미등록·노랑 등록중·녹색 등록·빨강 실패, 툴팁에 코드·사유), 오디오 요약(헤드셋/스피커 장치명, 클릭 → 설정), PTT 핫키 표시, 시각, 설정 ⚙ |
| 착신 배너 | 화면 상단에 슬라이드 — "대표번호 7000 착신 · 010-9876-5432 · [응답 F9] [거절]". 대표번호 착신(`calledParty`=pilot)은 주황, 내선 직접 착신은 파랑. 여러 착신은 스택(최신 위). 응답 핫키는 배너 최상단 호 |
| 긴급 배너 | 빨강(emergency) / 주황(imminent peril) / 자주(alert) 풀폭 배너 — 그룹명·개시자·경과. 채널 카드 배지와 동기. 취소(`emergency-ind=false` re-INVITE / `alert-ind=false`) 수신 시 해제 |
| 토스트 | 명령 실패의 사유(§8 사전) — 우하단, 6초, 오류는 수동 닫기. 원문 코드는 접힘(▸ 상세) |
| 상태 색상 | 대기 회색 · 링잉 주황(점멸) · 통화 녹색 · 보류 파랑 · 감청 보라 · 청취 청록 · 긴급 빨강. 색맹 대비를 위해 아이콘 병기(▢ ◉ ▣ ◐ ✚ 🔊 ⚠) |
| 시간 | 모든 진행 항목은 `mm:ss` 경과 시간(1초 갱신) — 링잉·통화·감청·PTT 세션 |
| 신원 표시 | 내선 → 디렉터리의 표시 이름(`users.name`) 병기 "1003 이순경"; 외부 번호는 E.164 → 국내 표기. 항상 원 값(URI user part)을 툴팁으로 |

### 3.3 크기 조정과 저장

- `GridSplitter` 3개(①|②, ②|③④, ③/④, 본문/⑤) — 최소 크기: ① 400, ② 560, ③④ 480, ⑤ 200, ③ 200, ④ 240.
- 위치 잠금 없음(구획 순서 고정), 비율만 `%APPDATA%\CIMS\dispatch-desktop\settings.json` 에 저장. "기본 배치" 복원 버튼(설정).
- 다중 모니터: 주 창은 한 모니터. F3 감청 영상 격자는 두 번째 모니터로 **뗄 수 있는** 창(§9).

## 4. 구획 상세

### 4.1 ① 통화 보드 (BLF)

**목적**: 범위 안 내선의 통화 상태를 한눈에, 링잉이면 픽업·통화 중이면 청취로 이어지는 진입점.

- **소스**: 감시 대상 내선마다 `dialogWatch(acc, ext, true)`(RFC 4235) → `onDialogInfo`. 대표번호 AoR 도 구독한다(§4.2 대기열).
  구독 대상 목록(내 그룹 멤버 + `monitorScope=listed/all` 의 타 그룹 멤버)의 공급은 §12 미해결 — 초기형은 디렉터리(⑤)에
  있는 내선 전부를 구독하고 403 인 대상은 타일에 "범위 밖" 표시로 접는다.
- **타일**: 내선 · 표시 이름 · 상태(dialog `state`: 없음/terminated=대기, early=링잉, confirmed=통화) · 상대(`remoteIdentity`) ·
  경과. 자기 내선은 "나" 로 고정 표시(조작 없음). 통화 중이고 `monitorScope` 안이면 ✚ 감청 가능 배지.
- **조작**(타일 클릭 → 팝오버): 대기 → [발신] · 링잉 → [지정 픽업 `<code><ext>`] · 통화 → [청취](§4.3) · 어느 상태든 [문자].
- **필터**: [내 그룹 / 범위 전체] 토글, 검색(내선·이름). 정렬은 내선 순 고정(위치 기억).
- **밀도**: 타일 128×72 → 560px 폭에 4열, 744−크롬 높이에 8행 = 32 타일 무스크롤. 그 이상은 구획 안 스크롤(운영 권고: 한
  데스크 감시 대상 32 이내).
- `state=terminated` 는 즉시 대기로 바꾸되 3초간 "종료" 잔상(누가 끊었는지 확인용).

### 4.2 ② 대표번호 데스크

**대기열 (상단)** — 대표번호 AoR dialog 구독(dispatch_center.md §4.5)으로 얻은 **링잉·응답 상태**:
- 행: 발신자 · `→ 대표번호` · 링 경과 · 현재 링잉 중 그룹원(포크 대기 leg — 표준형 RLS 전까지는 각 내선 dialog 의 early 로 추정) ·
  [당겨받기]. 응답되면 "응답: 1004 최순경" 으로 3초 표시 후 목록에서 제거(또는 내 통화로 승격).
- [당겨받기] = `pickup(code, pilotId)`(지정 픽업 — 대표번호 링잉 호, dispatch_center.md §4.4 `PickUpFork`). 자기 단말에도 링잉 중이면
  그냥 [응답].
- sequential 모드(`alert_mode=sequential`)는 서버가 한 명씩 울리므로 "링잉: 1001" 한 명만 보인다 — 화면 규칙은 같다.
- 큐 비어 있음 상태: "대기 호 없음" + 오늘 응대 건수(로컬 집계).

**내 통화 (중단)** — `calls()` 중 통화(`!isMcptt && !listenOnly`) 카드. 최대 4개(넘으면 스크롤, 운영상 1 활성 + 보류 n):
- 카드: 방향 아이콘 · 상대 · **착신 경로 배지**(`calledParty`=pilot 이면 "대표 7000 착신", 내선 직접이면 없음) · 상태 · 경과 · 🎧/🔊 라우트 토글.
- 조작: [응답](Incoming) · [거절 486] · [보류]/[재개] · [음소거] · [DTMF 패드] · [전달 ▾ blind / 상담] · [종료].
- **blind 전달**: [전달] → 대상 입력(디렉터리 선택 가능) → `transfer(callId, target)`. 진행은 카드에 "전달 중 → 1003" 표시, 서버가
  전달 완결 후 BYE 하면 카드 소멸·토스트 "전달 완료". 실패(대상 거절)면 원 통화 유지·토스트.
- **attended 전달**: [상담 전달] → 원 통화 자동 보류 + `dial(target)` 상담 통화 카드 생성(배지 "상담") → 상담 중 [전달 완결] =
  `transferAttended(원 callId, 상담 callId)` / [취소] = 상담 종료 + 원 통화 재개.
- 활성 통화가 있는데 새 착신을 응답하면 기존 통화는 자동 보류(설정 가능: 자동 보류 / 거절).

**다이얼 (하단)** — 번호/내선 입력(디렉터리 자동완성) · [발신] · [그룹 픽업 `<code>`] · 최근 발신 5개. 입력 규칙은 SDK 가 처리
(번호는 도메인 자동 결합, `sip:` URI 도 허용).

### 4.3 ③ 감청 패널

**목적**: 진행 중 통화의 청취 leg(RFC 3911 Join, `a=recvonly`) 하나당 카드 하나, **두 화자를 분리 표시**(합법감청 귀속 요건 —
dispatch_center.md §5.4).

- **진입**: ① 타일(confirmed) → [청취] → `join(acc, targetUri, dlg)`(dlg = 그 타일의 `DialogInfo`). 성공 시 `CallInfo.listenOnly && joinedDialog≠""`
  인 호가 생기고 카드가 뜬다.
- **카드**: 대상 "1003 ↔ 010-1234-5678" · 두 줄 `caller`/`callee`(RFC 5576 `label`) 각각 이름·**레벨 미터**·활성 점(`MediaSource.active/level` —
  U10 관측 API 후속 시 실시간, 그 전엔 `active` 만) · 경과 · 라우트 토글(기본 🎧) · 은닉/투명 배지(`listenVisibility`) · [종료]=`hangup`.
- **상시 표시**: 감청 카드가 하나라도 있으면 상단 바에 보라색 "감청 중 N" 점등 — 관제사 본인이 잊지 않게. 서버 감사(`E-AUD-016`)와
  별개로 앱은 로컬 이력에 남기지 않는다(감사 정본은 서버).
- **동시 감청**: 카드 스택(세션당 tap 상한은 서버 — 486 이면 §8 사전 문구).
- **원 통화 종료**: 서버가 청취 leg 에 BYE → 카드 "통화 종료됨" 3초 후 제거.
- 영상(F3): 카드 아래 격자 영역 예약 — 양측 영상 SSRC 2개를 2분할(§9).

### 4.4 ④ PTT 채널

**목적**: 관제사가 발언할 수 있는 **멤버 그룹**과 듣기만 하는 **청취 그룹**을 같은 카드 모양으로, 발언자·긴급 상태를 항상 보이게.

- **채널 목록 소스**: GMS `listGroups(userUri)`(멤버 그룹, ⑤ 디렉터리와 공유) + `dispatch.pttListen` 범위의 그룹(`listed` 대상은 §12 공급
  과제, `all` 은 GMS 전체 목록). 카드는 운영자가 고른 채널만 표시(설정 → 채널 선택, 기본 = 멤버 그룹 전부).
- **멤버 채널 카드**: 그룹명 · 배지 "멤버" · affiliation 상태 · 세션 상태(대기 / 진행 — 현재 발언자 이름·레벨·경과) · 긴급/임박 배지 ·
  **PTT 버튼**(누르는 동안 `floorRequest`, 떼면 `floorRelease`; 상태 색: 대기 회색·요청 노랑·**발언 중 녹색**·대기열 "n번째" 파랑·거부 빨강 1초) ·
  라우트 토글(기본 🔊 스피커) · 음량 · [참여]/[이탈](`joinGroupCall`/`leaveGroupCall`).
- **청취 채널 카드**: 배지 "청취" · 발언자·레벨·경과 · **PTT 버튼은 항상 비활성**(`FloorInfo.canRequest=false`, Taken `Permission=0`) —
  라벨 "청취 전용" · [청취 시작]=`joinGroupCall(listenOnly)` / [청취 종료]. 진행 세션이 없으면(480) 카드에 "진행 중 통화 없음 — 자동
  재시도 30초" (설정 가능).
- **선택 채널**: 카드 하나가 "선택" 상태(테두리) — 전역 PTT 핫키는 선택 채널에 적용. 청취 채널은 선택해도 핫키 무효(토스트 없음, 버튼
  비활성으로 충분).
- **floor 이벤트 표시**: Granted → 버튼 녹색 + "발언 중 mm:ss"(Duration 남은 시간 게이지) · Taken → 발언자 이름(로스터 매칭) ·
  Denied/Revoked → 사유(`causeText`) 1초 토스트 없이 버튼 색 + 카드 하단 한 줄 · QueuePosition → "대기 n" · TalkLimit → 게이지 빨강 ·
  RequestTimeout → 회색 복귀.
- **긴급**: `mcptt.emergency`/`imminentPeril` 인 세션은 카드 테두리 빨강/주황 + 전역 배너. 관제사의 긴급 개시(`GroupCallOptions.emergency`)는
  카드 메뉴 [긴급 호출](확인 대화상자) — 프로파일 자격 없으면 403 → 사전 문구.
- **로스터**: 카드 확장(▾)으로 `onRoster` 참가자 목록(청취 멤버는 `listenVisibility=visible` 일 때만 `listener` 역할로 보인다 — 앱은 그대로 표시).
- 청취 채널 수는 스피커 한 대에 섞이므로 기본 상한 4(설정) — 넘으면 경고.

### 4.5 ⑤ SDS · 디렉터리

**메시지 탭** — [mcdata_messaging.md](mcdata_messaging.md) §5 의 앱 동작을 데스크톱 밀도로:
- 좌 스레드 목록(그룹 = `groupUri`, 1:1 = 발신자 — `threadKeyOf` 규칙 동일) · 미읽음 배지 · 우 대화(발신 말풍선 상태 🕓→✓→✓✓/⚠ 재전송) ·
  입력 + [📎](FD — 파일 열기 대화상자) + [전송]. 그룹 스레드 수신 말풍선 위 발신자 라벨.
- 발신: `sendGroupSds(acc, groupId, text, requestDelivery)` → 결과는 `onRequestResult`(token 상관, 2xx=SENT) · 수신 disposition 요청이면
  `sendSdsNotification(delivered)` 자동 회신 · `onSds(notification)` → ✓✓.
- 착신 배너·긴급 배너 중에도 메시지 탭 미읽음 배지는 상단 바에 병기(⑤ 가 접혀 있을 때 대비).
- 보관: 로컬 SQLite(`%APPDATA%\CIMS\dispatch-desktop\messages.db`) — 최근 30일(설정). 서버 보관·콘솔 모니터링과 독립.

**디렉터리 탭** — 세 목록(탭 안 세그먼트): **내선**(관제 그룹원 — 이름·내선·BLF 상태·[발신][문자][지정 픽업(링잉 시)]) · **PTT 그룹**(GMS —
이름·멤버 수·[채널에 추가][문자]) · **최근**(발신·착신·부재 — 시각·상대·경로(대표/직접)·[재발신]). 검색은 전 목록 공통.
디렉터리 소스는 §12(내선 목록 공급) 확정 전까지 GMS 그룹 + 프로비저닝 + 로컬 수동 등록(설정 → 내선 가져오기 CSV).

## 5. 시작·로그인·프로비저닝

```
[시작] → 단일 인스턴스 확인(명명 Mutex, 둘째 실행은 기존 창 활성화)
      → 저장 토큰(DPAPI) 있으면 refresh → 없으면 로그인 창(아이디/비밀번호 → CscClient.login PKCE)
      → fetchProfile(/provisioning/me) → services[volte, ptt] → Engine.start → addAccount×2 → register×2
      → dispatch 블록 → 데스크 신원·범위 적용 → dialogWatch(내선들·대표번호) · affiliate(멤버 그룹) · subscribeConference
      → 메인 화면
```

- 로그인 창: 아이디·비밀번호·CSC 주소(기본은 마지막 값, 고급 접힘)·"자동 로그인" 체크(refresh token 만 DPAPI 저장 — 비밀번호·H(A1) 는
  저장하지 않는다; `sipHa1` 은 매 로그인 프로파일에서 받는다).
- `dispatch.present=false`(관제 데스크 아님) → 메인은 뜨되 ①③ 과 ② 대기열을 "관제 데스크 미배정" 안내로 접고 통화·PTT·SDS 만
  동작(일반 소프트폰 모드). 운영자에게 콘솔 `관리 > 관제 그룹` 배정을 안내.
- 등록 실패(401/403/타임아웃) → 상단 점등 빨강 + 토스트 사유, 자동 재시도(백오프 5→60초). `refreshRegistration` 은 네트워크 복귀 이벤트
  (Windows `NetworkChange`)에서 즉시.
- 로그아웃: 등록 해제 → 토큰 폐기 → 로그인 창. 종료(창 닫기)는 트레이로 최소화(설정), 완전 종료는 메뉴.

## 6. 오디오 배치 UI (ue_sdk.md §6.3)

설정 → **오디오**:

| 항목 | UI | API |
|---|---|---|
| 마이크 | 캡처 장치 콤보 + 레벨 미터(테스트) | `setAudioDevices(capture, -2)` |
| 헤드셋(라우트 0) | 재생 장치 콤보 — 통화·감청 기본 | `setAudioDevices(-1, playback)` |
| 데스크 스피커(라우트 1) | 재생 장치 콤보(없음 가능) — PTT 채널 기본 | `addPlaybackRoute(dev)` / `removePlaybackRoute` |
| 기본 라우트 정책 | 통화→헤드셋, 감청→헤드셋, PTT→스피커(스피커 없으면 헤드셋) | 카드 생성 시 `setCallRoute` |
| 장치 테스트 | 각 장치로 톤 재생 | 앱 로컬(WASAPI 톤) |

- 핫플러그: 파사드 `AudioEndpoints`(`IMMNotificationClient`) → `refreshAudioDevices()` → 선택 장치가 사라졌으면 기본 장치로 폴백 + 토스트
  "헤드셋 분리됨 — 기본 장치로 전환". 다시 붙으면 자동 복귀(설정).
- 카드의 🎧/🔊 토글은 호별 `setCallRoute(callId, 0|routeId)` — 즉시 재결선. 라우트 1 이 없으면 토글 비활성.
- 두 장치 동시 출력의 지연·에코는 실기 검증 항목(ue_sdk.md §11) — 설정에 "스피커 지연 보정 ms" 는 두지 않는다(엔진 패치 과제).

## 7. 핫키·입력

| 기능 | 기본 | 동작 | 접점 |
|---|---|---|---|
| PTT | `Ctrl+Space`(hold) | 선택 채널 `floorRequest` / 떼면 `floorRelease` | `RegisterHotKey` + 메시지 전용 HWND, key-up 은 `GetAsyncKeyState` 폴링(핫키는 down 만 온다) |
| 응답 | `F9` | 착신 배너 최상단 호 `answer` | 전역 |
| 종료 | `F10` | 활성 통화 `hangup`(감청·PTT 는 제외) | 전역 |
| 그룹 픽업 | `F8` | `pickup(code)` | 전역 |
| 보류/재개 | `F11` | 활성 통화 토글 | 앱 포커스 시 |
| 음소거 | `F12` | 활성 통화 `setMuted` 토글 | 앱 포커스 시 |

- 전부 설정에서 재배치, 충돌(`RegisterHotKey` 실패)은 설정 화면에 빨강 표시. 게임패드/풋스위치는 HID 키 매핑으로 같은 경로.
- PTT 키 hold 중 창 포커스가 바뀌어도 release 를 놓치지 않도록 key-up 폴링 20ms + 안전장치(Granted 후 `TalkLimit` 는 코어가 자동 Release).

## 8. 응답 코드 → 화면 문구 (사전)

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
| 긴급 개시 | 403 | "긴급 호출 자격이 없습니다" | mcptt_emergency_modes §4.2 |
| SDS | 403/404/408/503 | "전송 실패 — 재전송" (⚠ 탭) | mcdata §5 |
| 등록 | 401/403 | "인증 실패 — 다시 로그인" | sip_access_security |
| 등록 | 408/503 | "서버 응답 없음 — 재시도 중" | — |

## 9. 영상 (F3 — 예약)

- ③ 감청 카드 아래 격자: 양측 영상 SSRC 2개를 2분할(caller | callee), `onVideoFrame` 콜백 렌더(WPF `WriteableBitmap`/D3DImage).
- 별창으로 분리(두 번째 모니터) — 주 캔버스 예산은 유지. 1:1 영상 통화는 ② 통화 카드 확장.
- F3 전까지 카드에 "영상 없음(음성 감청)" 고정 문구.

## 10. 구현 구조 (WPF, MVVM)

```
windows/dispatch-desktop/
  App.xaml(.cs)                 단일 인스턴스·전역 예외·SynchronizationContext 캡처
  Shell/MainWindow.xaml         3열+하단 Grid, GridSplitter, 상단 바, 배너 레이어, 토스트 레이어
  ViewModels/
    DeskViewModel               Profile·dispatch·등록 상태·오디오 요약 (상단 바)
    BlfBoardViewModel           ① — DialogInfo → ExtensionTile 사전
    PilotDeskViewModel          ② — QueueItem(대표번호 dialog) · CallCard(통화) · Dialer
    MonitorPanelViewModel       ③ — MonitorCard(join 호) · MediaSource 미터
    PttChannelsViewModel        ④ — ChannelCard(멤버/청취) · FloorState · 선택 채널
    MessagesViewModel / DirectoryViewModel   ⑤
    Banners(Incoming/Emergency) · Toasts
  Models/  CallKind(통화·감청·PTT) 분류: isMcptt→PTT, listenOnly&&joinedDialog→감청, 그 외 통화
  Services/ SettingsStore(json) · MessageStore(SQLite) · HotKeyMap · AudioPolicy(라우트 기본값)
  Views/    구획별 UserControl, 카드 DataTemplate, 상태 색 리소스(§3.2)
```

- 파사드 이벤트(`CimsUe.Engine` 의 `CallStateChanged`·`FloorChanged`·`DialogInfo`…)는 이미 UI 스레드로 마샬링돼 온다(ue_sdk.md §6.4) —
  ViewModel 은 `ObservableCollection` 을 직접 갱신.
- **UI 는 코어 상태의 투영**이다: 카드는 `calls()`/`callInfo` 스냅샷에서 파생하고 앱이 별도 상태 기계를 갖지 않는다(재접속·재기동 후
  화면 재구성 = 스냅샷 재조회).
- 접근성: 모든 카드 조작은 키보드 도달 가능, 상태는 색+아이콘+텍스트 삼중.

## 11. Android 태블릿 밀도

같은 다섯 구획을 가로 태블릿(1280×800)에 **탭 2개**로: [관제](① 좌 · ② 우 상 · ③ 우 하) / [PTT·메시지](④ 좌 · ⑤ 우). 착신·긴급 배너와
PTT 하드키(UNIWA 측면 키)는 공통. 상세는 `android/dispatch-tablet` 구현 시 이 문서 §11 을 확장한다.

## 12. 미해결 / 향후 과제

- **감시 대상 내선 목록 공급** — `/provisioning/me` 의 `dispatch` 블록은 그룹·범위만 준다. 자기 그룹원과 `listed` 대상 그룹원의 내선 목록이
  필요하다. 선택지: (a) `dispatch` 블록에 `members[]`·`monitorTargets[].members[]` 확장(CSC), (b) RFC 4662 RLS 목록 구독(표준형 — 서버
  §5.2 후속, 구독 N→1). 초기형은 (a) 없이 GMS·수동 CSV 로 시작.
- **U10 관측 API** — `MediaSource.level/active` 실시간 갱신(코어 `onCallMedia` 주기) 확정 후 ③ 레벨 미터 활성.
- **경보(alert-ind) 파싱** — `onMessage` 의 `mcptt-info` 를 코어가 `McpttInfo` 로 해석해 이벤트로 올리는 API(현재는 본문 전달).
- **대표번호 발신 표시**(`P-Preferred-Identity`=pilot — dispatch_center §10 서버 과제) 확정 시 다이얼에 "대표번호로 발신" 토글.
- **큐/ACD**(대기열 순번·안내) — 서버 과제, 대기열 UI 는 그 데이터 모델이 오면 열만 추가.
- **영상 F3**, **ambient listening**(단말 무표시 자동응답 — 관제 앱은 개시 측), **끼어들기**(CMP 믹서 — 서버 과제).
