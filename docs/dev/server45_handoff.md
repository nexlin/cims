# .45 서버에서 할 일 — .48(소스 서버) 반영분의 단말·배포 짝

.48 에서 서버(CSP·CMP·CSC·OAM·콘솔·계측기) 쪽을 반영·실측했고, 단말 빌드·.45 스택 반영이 필요한 짝만 여기 모았다.
관제 앱(windows/dispatch-desktop · android/dispatch-tablet)은 Windows 쪽 몫이라 각 항목에 표시만 한다.
시험 대상 = .48 배포본(관리평면 OAM `https://121.161.164.48:4419`, CSP SIP UDP `121.161.164.48:15060`, PTT 도메인 `ptt.cims.example.kr`, VoLTE 도메인 `volte.cims.example.kr`).

## 1. 일제 통화 — 단말 (WP5, U1~U5)

정본 [../design/features/mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) §4.4 · §7.5. 서버는 반영 끝(§2 판정표 서버 ✅).

| # | 할 일 | 대상 |
|---|---|---|
| U1 | 일제 통화 발신 API — 그룹 INVITE mcptt-info 에 `session-type=prearranged` + `<broadcast-ind>true</broadcast-ind>` | `sdk/core/src/mcptt/mcptt_xml.cpp`·그룹 호 발신 API · `cimsue-cli group-call --broadcast` · `android/ptt-client/.../McpttXml.kt`(`SessionType.BROADCAST` 제거) |
| U2 | 개시자 Floor Request 에 Floor Indicator B-bit(0x4000) | `sdk/core/src/floor/floor_participant.cpp`, Android `FloorControl.kt` |
| U3 | 개시 단말: 발언 종료 뒤 B-bit Floor Idle 수신 → 호 해제(BYE) (TS 24.380 §6.2.4.6.4) | 같은 파일 |
| U4 | 수신 단말: B-bit → "일제 통화" 표시, Permission 0 → PTT 비활성 | SDK FloorEvent · `PttController.kt` |
| U5 | 그룹 종류 판정을 `<on-network-invite-members>`(true=prearranged, false=chat)로 | `sdk/core/src/csc/group_doc.cpp`(~127·~187), Android `CscModels.kt` |
| U6 | 관제 앱 "일제 통화" 동작 — **Windows 쪽** | `windows/dispatch-desktop`, `android/dispatch-tablet` |

**서버가 이미 하는 것**
- 개시 INVITE 의 `<broadcast-ind>` 로 세션 속성을 정한다. 합류 INVITE 는 개시자를 바꾸지 않는다.
- 비개시자의 Floor Request 는 Deny #5(reason=broadcast)로 거절하고, Floor Taken/Idle 에 B-bit 와 Permission 0 을 싣는다.
- conference 구독은 480 + `Warning: 105` 로 거절한다.
- T4(그룹 `hang_timer_sec`, 기본 30초)가 만료되면 세션을 해제한다.
- GMS 그룹 문서는 규격 요소 `<on-network-invite-members>`·`<on-network-hang-timer>PT30S`·`<on-network-maximum-duration>` 을 싣는다.
- 그 문서에는 **전환기 요소 `<mcpttgi:session-type>`(prearranged/chat)도 아직 함께 싣는다.** U5 가 들어가면 .48 쪽이 이 요소를 제거한다.

**시험**
- 서버 동작의 기준은 cspsim `-broadcast` 로 실측했다(`./build/bin/cspsim -server_ip 121.161.164.48 -server_port 15060 -domain ptt.cims.example.kr -mode ptt -scenario group_call -count 4 -group g001 -call_duration 12 -floor_hold 2 -broadcast -db <csp.json> -media_dir tests/media`).
- 단말 반영 뒤 같은 그룹에서 `cimsue-cli` 로 개시해 확인할 것:
  - 개시자만 발언권을 받는다.
  - 멤버는 PTT 가 비활성으로 보인다.
  - 개시자가 발언을 놓으면 개시 단말이 BYE 를 보낸다.
  - 서버는 T4 가 만료되면 나머지 참가자를 해제한다.
- **T4 를 짧게 보려면** 콘솔 구성 › PTT 그룹 › 유지 시간(T4)을 5초로 바꾸고, 시험 뒤 30초로 되돌린다.
- **계측기 real-ue 경로**: cimsue-cli 가 U1 을 지원하면 .48 에서 계측기 쪽 게이트를 연다. 게이트는 `tester_compile.check_kind_gates` 의 real-ue broadcast 컴파일 오류와 `Worker::epGroupCall` 의 real 분기, 두 곳이다. 지원하게 되면 cimsue-cli 명령 형식(예: `group_call <g> broadcast`)을 알려 달라.

## 2. 착신 차단(ICB, TS 24.611) — .45 스택 반영 · DB 마이그레이션 시점

정본 [../design/features/volte_supplementary_services.md](../design/features/volte_supplementary_services.md) §6B. 코드는 푸시됨(`0f33a5db` 이후), **.48 도 아직 미배포**.

- `sql/migrate_icb_naming.sql` 은 컬럼·테이블 **이름을 바꾼다**.
  - `volte/voip_subscriptions.dnd` → `icb_all`
  - `ptt_subscriptions.dnd` 제거
  - `user_rejects` → `icb_identities`
- DB 는 .45·.48·.135 가 **공유**한다(121.161.164.45:3306/cims). 적용 순간 옛 코드의 CSP·CSC·OAM 은 옛 이름을 조회하다 실패한다. 가입자 적재가 깨져 등록·호가 안 되고, 사용자 관리 API 는 500 이 난다.
- **절차 (같은 정지창):**
  1. .45 에서 새 코드로 csp·csc·oam(콘솔 포함)·oam-svc 패키지를 준비한다.
  2. 마이그레이션을 적용한다(`sudo mysql cims < sql/migrate_icb_naming.sql`, 또는 .48 쪽에 적용을 요청).
  3. .45·.48 을 올린다. .135 도 같은 DB 를 쓰므로 함께 올린다.
- **시점이 정해지면 .48 쪽에 알려 달라** — .48 배포와 실측(계측기 `VOLTE-ICB-ALL`·`VOLTE-ICB-IDENTITY`)을 그 창에 맞춘다.
- 관리 API 는 한 릴리스 동안 구 키 `dnd`·`reject_id` 도 받는다(응답은 새 키만). 단말·관제 앱 코드는 이 키를 쓰지 않는다(확인함).

## 3. MCPTT 관리 조회 화면 — 단말 속성 수집 (V3 의 단말 짝)

정본 [../design/features/mcptt_management_views.md](../design/features/mcptt_management_views.md) §4.1. 서버의 REGISTER 파싱·`ue_devices` 저장·
"단말 현황" 화면은 .48 쪽에서 한다. 단말은 **규격 경로로 속성을 싣기만** 하면 된다(앱 전용 보고 API 없음).

| 속성 | 단말이 할 일 | 대상 |
|---|---|---|
| 단말 ID(IMEI) | REGISTER Contact `+sip.instance="<urn:gsma:imei:NNNNNNNN-SSSSSS-C>"` (RFC 7254) — 지금 보내는 `+sip.instance` 형식을 IMEI URN 으로 | `sdk/core` 등록 경로 · Android |
| 모델·OS·앱 버전 | REGISTER `User-Agent: CIMS-PTT/<앱 버전> (<OS>; <모델>)` (RFC 3261 §20.41) | 같은 곳 · 관제 앱은 **Windows 쪽**(형식만 맞춤) |

IMEI 를 못 얻는 플랫폼(Windows 데스크톱 등)은 지금처럼 UUID URN(`urn:uuid:`)을 그대로 두면 된다. .48 쪽 V3 구현은 URN 종류를 보고 IMEI 칸을 비우게 한다.

## 4. 참고 — .48 에서 반영된 것(서버)

- 일제 통화 서버 몫, 착신 차단 코드(미배포), 착신 차단 지정 번호 콘솔 편집, MCPTT 그룹 정보 화면(서비스 › MCPTT 그룹 정보).
  - API: `GET /api/v1/stats/service/ptt-groups[/{id}]`
  - 버전: oam 0.2.169 · oam-svc 0.2.128
- 배포 함정: oam-svc 는 base oam 설치본의 핸들러 코드를 쓴다. base oam 을 먼저 올리고 oam-svc 를 재기동한다. `scripts/oam-deploy.py upgrade` 가 이제 이 순서로 올린다.

## 5. .45 반영 결과 (→ .48)

| 항목 | 상태 |
|---|---|
| §1 일제 통화 단말 U1~U5 | ✅ SDK·Android PTT 반영([mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) §4.4). U6(관제 앱)은 Windows 쪽 |
| cimsue-cli 명령 형식 | 원샷 `group-call <g> --broadcast [--ptt-at S --ptt-len S]`(결과 JSON 에 `broadcast_released:true`) · drive `group_call <g> broadcast` — real-ue 게이트(`tester_compile.check_kind_gates`·`Worker::epGroupCall`)를 열 수 있다 |
| 라이브 실측(.45, g005 가상 신원 013 개시·014 청취) | 개시 INVITE `prearranged`+`<broadcast-ind>true` → CMP `PTT_GROUP_ADD … broadcast initiator=+82500000013` → Floor Request `ind=0x4000` → Granted → 해제 → Idle → 개시 단말 BYE. 청취 쪽 fan-out INVITE 에 `broadcast-ind`, Taken 수신 |
| U5 뒤 GMS 문서 전환기 `<mcpttgi:session-type>` | 제거 가능 — SDK·Android 가 invite-members 로 판정(없는 옛 문서만 폴백), SDK PUT 본문은 session-type 을 싣지 않는다 |
| §2 ICB 마이그레이션 | **공유 DB 에 적용됨(2026-09-29 00:33, .45 정지창 안)**. .45 = csp 0.2.158·csc 0.2.129·oam 0.2.170·oam-svc 0.2.129. **.48(csp 0.2.157 = ICB 이전 빌드)는 `Unknown column 's.dnd'` 조회 오류가 나기 시작했다 — ICB 빌드로 재배포 필요. .135 쪽도 같다** |
| ICB 실측(.45 `VOLTE-ICB-ALL`·`VOLTE-ICB-IDENTITY`) | 603·착신전환보다 우선은 PASS. **거절 안내(early media) 미재생** — 착신자가 등록 상태면 `CTasModule::ScreenInvite`(다이얼로그 생성 전)가 603 을 바로 보내 `ApplyTerminationServices` 의 `gclsAnnouncement.Reject` 경로에 닿지 않는다. 등록되지 않은 착신자만 안내가 난다. CSP 과제 — 원인·보완 방향(판정 한 곳 = `ApplyTerminationServices`)·확인 항목은 [volte_supplementary_services.md](../design/features/volte_supplementary_services.md) §6B.5 |
| `PTT-GROUP-CALL-BROADCAST`(.45) | 시작 전 종료 — tb45 의 PTT 풀이 전부 g005 멤버라 `member: false` 역할(monitor) 신원이 없다(환경) |
| §3 단말 속성 V3 | ✅ Android PTT·VoLTE: `User-Agent: CIMS-PTT/<버전> (Android <판>; <모델>)`·`CIMS-VoLTE/…`, `+sip.instance` = ANDROID_ID 이름 기반 `urn:uuid:`(일반 앱은 IMEI 를 못 읽는다 — Android 10+). 모든 transport(UDP 는 Contact 직접). SDK: `AccountConfig.instanceId`·`imeiUrn()`·`userAgentOf()` — libcimsue 앱(관제 태블릿·Windows)은 앱이 채울 몫([ue_sdk.md](../design/features/ue_sdk.md) §4.2) |
| 서비스 로그 경로 | .45 는 단일 루트(`/mnt/cims/service_log`)인데 이 디렉터리를 **다른 사이트의 옛 CSP(CMP .136/.139 와 통신, node `csp_01`)도 쓴다**. 새 코드(.45)는 `<log>/sip/<연>/…`·`<log>/stats/ptt_{index,attempts}` 에, 옛 코드는 `<log>/<연>/…`·`<log>/ptt/{index,attempts}` 에 쓴다 — 옛 기록을 옮기면 그 사이트 것까지 섞여 움직이므로 옮기지 않았다. .45 도 자기 사이트 디렉터리로 옮기는 것이 정리 방향([site_directory_layout.md](../design/features/site_directory_layout.md) §6) |
