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
| 단말 ID(IMEI) | REGISTER Contact `+sip.instance="<urn:gsma:imei:TTTTTTTT-SSSSSS-0>"` (RFC 7254 · TS 23.003 §13.8 — 셋째 칸은 spare, 단말이 보낼 때 항상 0) — 지금 보내는 `+sip.instance` 형식을 IMEI URN 으로 | `sdk/core` 등록 경로 · Android |
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
| U5 뒤 GMS 문서 전환기 `<mcpttgi:session-type>` | **제거(csc 0.2.130)** — 그룹 문서에 싣지 않고 XCAP PUT 도 invite-members 만 읽는다. 계측기 real-ue 일제 통화 게이트도 해제(tester 0.1.34·worker 0.1.30 — drive `group_call <g> broadcast`) |
| §2 ICB 마이그레이션 | **공유 DB 에 적용됨(2026-09-29 00:33, .45 정지창 안)**. .45 = csp 0.2.160·csc 0.2.130·oam 0.2.174·oam-svc 0.2.131·tester 0.1.34·worker 0.1.30(.48 반영분 `978c0156`·`1517448f` 재배포). .48 은 ICB 빌드로 재배포 완료(csp 0.2.159·csc 0.2.130·oam 0.2.172·oam-svc 0.2.130 — `Unknown column 's.dnd'` 해소). **.135 쪽은 아직 옛 빌드** |
| ICB 실측(.45 `VOLTE-ICB-ALL`·`VOLTE-ICB-IDENTITY`) | 603·착신전환보다 우선은 PASS, 거절 안내 미재생(등록 착신자는 `ScreenInvite` 가 다이얼로그 전 603) → **.48 반영(csp 0.2.159)**: 판정 한 곳 `ApplyTerminationServices`(등록 여부 무관 — DB 폴백 조회), `ScreenInvite` 는 ptt 전용 모드 403 만. .48 실측 `VOLTE-ICB-ALL` 5/5·`VOLTE-ICB-IDENTITY` 8/8(`RELAY_PLAY sys:busy_kr` 6 s → 603). **.45 재실측(csp 0.2.160) PASS** — `VOLTE-ICB-ALL` 5/5·`VOLTE-ICB-IDENTITY` 8/8, 등록 착신자에게도 `Announcement: declined → sys:busy_kr` 6000 ms → 603, 픽스처 복원(`reverted`) |
| `PTT-GROUP-CALL-BROADCAST`(.45) | 시작 전 종료 — tb45 의 PTT 풀이 전부 g005 멤버라 `member: false` 역할(monitor) 신원이 없다(환경) |
| §3 단말 속성 V3 | ✅ Android PTT·VoLTE: `User-Agent: CIMS-PTT/<버전> (Android <판>; <모델>)`·`CIMS-VoLTE/…`, `+sip.instance` = ANDROID_ID 이름 기반 `urn:uuid:`(일반 앱은 IMEI 를 못 읽는다 — Android 10+). 모든 transport(UDP 는 Contact 직접). SDK: `AccountConfig.instanceId`·`imeiUrn()`·`userAgentOf()` — libcimsue 앱(관제 태블릿·Windows)은 앱이 채울 몫([ue_sdk.md](../design/features/ue_sdk.md) §4.2) |
| §1 U6 관제 앱 — Windows 데스크톱 | ✅ ① 포커스 카드 3줄 [일제 통화] — 멤버 편성 그룹에 진행 중 세션이 없을 때만(chat·진행 중은 비활성 — 서버가 합류로만 받거나 broadcast-ind 를 무시) → `JoinGroupCall(Broadcast)`·개시 카드 자동 포커스·단일 발언 대상. 판정 = 착신 broadcast-ind 또는 floor B-bit(.NET `FloorIndicator.BroadcastGroup` — 늦은 합류 leg 포함), 수신 멤버(Permission 0)는 발언 대상 체크 불가, 서버가 일반 통화로 연 개시(첫 floor 메시지에 B-bit 없음)는 경고, ⑤ 개시·수신·종료 행. PTT 그룹 편집 유형에서 `broadcast` 제거. 빌드·단위시험·화면 렌더 확인, **서버 실측은 아직**. Android 태블릿은 이어서 |
| §3 단말 속성 V3 — Windows 관제 | ✅ `User-Agent: CIMS-Dispatch/<앱 버전> (Windows 11 25H2; <BIOS 모델>)`(개발 PC 예 `CIMS-Dispatch/0.1.0 (Windows 11 25H2; 960QGK)`)·`+sip.instance` = `MachineGuid` 이름 기반 `urn:uuid:`(Android 와 같은 `cims-ue:` v3 규칙, PTT·전화 계정 같은 값). 코어 `userAgentOf` 가 괄호 안 값을 comment 규칙으로 정리한다(괄호·역슬래시·OS 의 `;` 제거 — `parse_user_agent` 가 `Standard PC (Q35 …)` 같은 모델을 자르지 않게). C API `cimsue_user_agent_of`·`cimsue_imei_urn`, .NET `Engine.UserAgentOf`·`Platform.DeviceIdentity` |
| Windows 빌드 확인(→ .48) | Windows 컴파일 없이 들어간 곳 — pjsua2 CMake 목록·.NET 그룹 문서 시험·녹취 재생 토큰 세 건은 `f54a5968`, 표본 화면 스위치(`--ui-preview-shot`)가 청취 창 종료 확인에서 멈추던 것은 `App.IsExiting`(종료가 정해지면 창들이 다시 묻지 않는다). `cimsue_test` 39·.NET 60 통과 |
| 규격 대조 점검 — 단말 몫 수정 | 인계 항목을 규격 원문과 대조해 단말 쪽 결함 넷을 고쳤다. ① **짧은 PTT 탭**(승인 전에 놓음) — pending Release 중 늦게 온 Floor Granted 가 마이크를 열고 Speaking 으로 가 이어 오는 Idle 을 무시했다(서버는 유휴인데 단말만 송출, 일제 통화 개시자는 호 해제 누락) → Ack 만 하고 상태 유지(TS 24.380 §6.2.4.6.8), 손으로 놓은 Release·Granted Duration 자체 종료 Release 도 T100 재전송(§6.2.4.6.2·§6.2.4.6.3), pending 중 Taken 이면 해제(§6.2.4.6.5). SDK `floor_participant.cpp`(시험 `floor_participant_test.cpp` 2건 — Windows·Linux 공통) · Android `FloorClient.kt`(**Android 빌드·실기 확인 필요**) ② **IMEI URN** 셋째 칸 = spare 0(RFC 7254 §4.2.3) — `imeiUrn` 이 Luhn 검사 숫자를 싣던 것 수정(입력 = CD 15자리 검증·14자리·끝 0 15자리) ③ **TCP/TLS 에서 `+sip.instance` 소실** — CSP 가 RFC 5626 outbound 를 모르니 pjsua 가 OUTBOUND_NA 로 두고 NAT 재작성(재연결) 때 outbound 경로의 instance 를 뺐다 → 모든 transport 에서 REGISTER Contact 파라미터로, pjsua outbound 끔(SDK `account_map.cpp` · Android `SipController.kt`) ④ **관제 앱** — 첫 서버 floor 메시지가 Deny(참가자 1명)여도 B-bit 를 메시지 Floor Indicator 로 봐 "일제 통화로 열리지 않음" 오경고 제거(코어도 Deny·Revoke 의 Indicator 를 상태에 담음), 관리 범위 없는 관제사는 그룹 종류를 모르는 채로 두고 [일제 통화] 때 GMS 문서 `on-network-invite-members` 로 확인. `cimsue_test` 46·.NET 63 통과 |
| 서비스 로그 경로 | .45 는 단일 루트(`/mnt/cims/service_log`)인데 이 디렉터리를 **다른 사이트의 옛 CSP(CMP .136/.139 와 통신, node `csp_01`)도 쓴다**. 새 코드(.45)는 `<log>/sip/<연>/…`·`<log>/stats/ptt_{index,attempts}` 에, 옛 코드는 `<log>/<연>/…`·`<log>/ptt/{index,attempts}` 에 쓴다 — 옛 기록을 옮기면 그 사이트 것까지 섞여 움직이므로 옮기지 않았다. .45 도 자기 사이트 디렉터리로 옮기는 것이 정리 방향([site_directory_layout.md](../design/features/site_directory_layout.md) §6) |

## 6. .48 에 넘기는 서버 과제 — 규격 대조 점검 결과

인계 항목(§1 일제 통화·§2 ICB·§3 단말 속성)을 규격 원문(TS 24.379 V18.14·24.380 V18.8·24.611 V18.0·24.628·23.003·RFC 7254/5626)과
코드로 대조했다. 정상 경로는 규격대로이고, 아래는 **서버(CMP·CSP·CSC) 몫**으로 남은 어긋남이다. 정본 반영 위치는
[mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) §2 ⚠·§7.

| # | 심각도 | 위치 | 어긋남 | 규격 | 고칠 방향 |
|---|---|---|---|---|---|
| S1 | 중상 | CMP `PMcpttGroup::grantInitialFloor`(`PMcpttGroup.cpp` ~1232, `PCmpServer.cpp` ~2174) | 초기 발언권(SDP `mc_granted` → PTT_JOIN `granted`)에 일제 개시자 검사가 없다 — 개시자가 놓은 뒤 T4 사이에 멤버가 `mc_granted` 로 재합류하면 발언권을 받고 전원에게 Taken | TS 24.380 §6.3.5.3.4 (비개시자 = Deny #5) | `_broadcast && sessionId != _initiatorSessionId` 이면 부여하지 않는다 |
| S2 | 중하 | CMP `addMember` 늦은 합류 Floor Taken(`PMcpttGroup.cpp` ~286) | Permission to Request the Floor 가 없다(Message Sequence Number·서버 SSRC·MCPTT ID 도 `broadcastFloorStatus` 와 다름) — 늦게 합류한 일제 통화 수신 멤버의 PTT 가 활성으로 보이고 누르면 Deny #5 | TS 24.380 §6.3.4.4.2 3d (일제·ambient 는 Permission 0) | `broadcastFloorStatus` 와 같은 형식으로 |
| S3 | 중 | CSP `GroupCallService::ProcessGroupCall` 세션 캐시(~448) | 일제 속성을 개시 성공 전에 적고 실패 경로(세션 창·`AcceptCall` 실패·CMP 포트 부족)에서 지우지 않는다 → 그 그룹의 conference SUBSCRIBE 가 다음 세션까지 480/105 | TS 24.379 §10.1.3.4.1 (일제 통화 **진행 중**에만) | 자기 leg 추가 뒤 확정, 실패 경로에서 제거 |
| S4 | 중 | CSP `AuthzSweepConferenceSubscriptions`(`AuthzRevoke.cpp` ~67) | 권한 재점검 스윕이 일제 통화의 일시 480(Warning 105)을 인가 상실로 보고 기존 구독을 `rejected` 로 끊는다(역할 변경·CSC 재기동·회선 개설 때) — RFC 6665 상 단말은 재구독하지 않아 일제 통화 뒤 로스터가 멈춘다 | TS 24.379 §10.1.3.4.1 · RFC 6665 §4.1.3(reason=rejected → 재구독하지 않음) | 스윕은 480/105 를 제외 |
| S5 | 하 | CSP `ProcessGroupCall` | 빈 그룹에 INVITE 두 개가 거의 동시에 오면(psip UDP 스레드 10) 뒤 INVITE 가 캐시(개시자·broadcast)를 덮어 CMP·CSP 판단이 갈린다 | TS 24.380 §6.3.5.3.4 (개시자 = 세션을 연 사용자) | 그룹 단위 직렬화 |
| S6 | 중 | CSP ICB 지정 번호 판정(`CspUser::IncomingBarredBy`, `SipUserAgentInvite.hpp` ~211) | From user part 정확 일치 — 규격은 **P-Asserted-Identity** 대조. 트렁크의 `Privacy: id`(From anonymous)·국내 번호 형식이면 차단되지 않는다 | TS 24.611 §4.5.2.6.1 (cp:identity ↔ PAI, 선택적으로 From) | PAI 우선 + E.164 정규화 비교 |
| S7 | 하 | CSP ICB 거절 안내 뒤 603(`TasModule.cpp` ~111 `Reject(…, NULL, …)`) | 안내(early media) 뒤 최종 응답에 Reason 헤더가 없다 | TS 24.628 §4.2.4 (early media 방식은 최종 응답에 Reason) | `Reason: SIP;cause=603` 또는 Q.850 21 |
| S8 | 하 | CSC `admin.py` 구 키 전환기(~838 → ~865) | 구 키 `dnd` 를 PTT 회선에 보내면 `icb_all` 로 바뀐 뒤 400 — 옛 콘솔은 PTT 회선에도 `dnd:false` 를 보냈다("한 릴리스 동안 구 키 수용"과 어긋남) | (전환기 약속) | PTT 회선의 구 키 `dnd` 는 무시(WARN) |
| S9 | 하 | CSP ICB 목록 적재(`DbManager.cpp` ~433·643) | 사람의 `icb_identities` 가 PTT 회선에도 실려 non-MCPTT 1:1 INVITE 를 막고, CSC 는 전화 회선만 갱신해 PTT 쪽 복사본이 낡는다 | TS 24.611 (MMTel 부가서비스 — MCPTT 대상 아님) | PTT 회선에는 싣지 않는다 |
| S10 | 문서 | 정본 R6·`ptt_flows.md` | "Floor Idle 에 Permission 0" — Floor Idle 메시지에는 그 필드가 없다(코드는 맞다) | TS 24.380 §8.2.8 | **이번에 문서 정정함** |

**.48 반영 — 배포(csp 0.2.164·cmp 0.2.102·csc 0.2.131 — csp 0.2.163 은 .45 의 다른 빌드 라벨)·실측 PASS**: tb48 `VOLTE-ICB-ALL` 5/5·`VOLTE-ICB-IDENTITY` 8/8(와이어 603 에 `Reason: SIP;cause=603`),
`PTT-GROUP-CALL-BROADCAST` 9/9(CMP `broadcast initiator=…`·비개시자 Floor Request Deny #5), 회귀 `PTT-GROUP-CALL-BASIC` 11/11·`PTT-FLOOR-HANDOVER` 7/7·
`PTT-GROUP-LISTEN` 7/7·`PTT-GROUP-LISTEN-ROSTER`(visible) 6/6·`PTT-GROUP-LISTEN-CONF-DENIED` 4/4. S1·S3·S5 의 가장자리(재합류 mc_granted·개시 실패·동시 개시)는
계측기 시나리오가 없어 코드 경로만 확인했다. .45·.135 는 미반영.

| # | 반영 |
|---|---|
| S1 | `grantInitialFloor` — 일제 세션이면 개시자 외 초기 발언권 거부(요청 경로 Deny #5 와 같은 판정) |
| S2 | 늦은 합류 Taken = `_takenFields` 공용(전원 통지와 같은 필드·헤더 서버 SSRC, 화자 여럿이면 목록 한 건). 일제·수신 전용 수신자 Permission 0 |
| S3·S5 | 세션 캐시 **선점**(`GroupSession::bPending`) — 개시자 leg 확립에서 확정(`SettlePendingSession`), 그 전에 끝나는 모든 경로는 가드가 지운다. 선점 중 같은 그룹 INVITE 는 합류로 처리(선점 시한 32 s). 480/105 는 확정 세션만(`IsBroadcastInProgress`) |
| S4 | `CheckConferenceSubscribe(…, bAuthzOnly)` — 권한 스윕·수락 직후 재검사는 일제 480/105 를 보지 않는다 |
| S6 | `CTasModule::IncomingBarredBy` — 후보 PAI + From, 착신 가입자 다이얼 플랜으로 +E.164 정규화 대조(ICB 판정 3곳 공통) |
| S7 | 안내 뒤 최종 응답은 항상 Reason — 받은 원인이 없으면 `SIP;cause=<코드>`(`FinishEarly`, ICB 외 자체 거절 전부) |
| S8 | CSC `_icb_legacy_keys(…, ptt=True)` — PTT 회선의 구 키 `dnd` 는 WARN 후 버림 |
| S9 | CSP `DbManager` 단건·전량 적재와 파일 폴백 모두 PTT 회선에 `icb_identities` 를 싣지 않음 |

정본 반영: [mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) §2 R5·R6·R7·R11 ✅, [volte_supplementary_services.md](../design/features/volte_supplementary_services.md) §6B.2·§6B.3, [announcements.md](../design/features/announcements.md) §3.1(최종 응답 Reason).

- **알려진 미구현(정본 §7)**: 최소 affiliation 인원 미달 해제(TS 24.379 §6.3.8.1 4)), ACR(익명 착신 거부 — 433, TS 24.611 §4.5.2.6.2).
- **참고 — 일제 통화의 수신자**: 규격 정의(TS 22.179 §3.1)는 "개시자만 송신·응답 없음·송신 끝 = 호 끝"이고 "전원 참여"가 아니다.
  현행 절차(TS 24.379 §6.3.5.5)는 그 그룹의 **affiliated 멤버**를 일반 편성 그룹 호처럼 초대한다 — 서버 fan-out 이 그렇다
  (`require_affiliation` 이 켜진 그룹 기준. DB 단절 시 affiliation 검사를 건너뛴다). 사용자/그룹 방송 그룹(전원·조직 단위 수신 집합)은
  TS 24.379 §4.12 가 현 릴리스에서 따르지 않는다고 적는다.

## 7. PTT 그룹 편집 — 관제 앱 폼 (Windows 반영)

G1(다섯 필드)·G2(멤버 우선순위 보존) 반영 — 확인 통화 설정 세 칸(`MinNumberToStart`·`AckTimeoutSec`·`AckAction`)과 멤버 [필수] 토글도 같은 폼에 두었다.
정본 [../design/features/dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §4.7(칸·범위·기본값 = 콘솔과 같다, 문서에 없던 칸은 기본값 그대로면
미기재 유지, 역할을 바꾸지 않은 멤버는 읽은 우선순위를 되돌림). 서버는 바뀐 것 없음. 동시 발언은 넣지 않는다(관리 API 전용). 태블릿은 같은 방식으로 옮긴다.

## 8. MCPTT 암묵적 발언 요청 · 애드혹 일제 통화 — 서버 과제 (.48 몫)

.48 에서 반영·배포(csp 0.2.165)·실측했다 — 정본 [mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) R13·R14 판정 ✅.
.45 스택에 올릴 때는 csp 0.2.165 이상. 단말 동작은 바뀌지 않는다(SDK 는 두 속성을 함께 싣는다).

## 9. 단말 SDK P0b 실측에서 드러난 서버 과제 (.48 몫)

.45 에서 SDK 코어(긴급·경보·MSRP)를 실서버로 시험하며 본 것이다([sdk_port_handoff.md](sdk_port_handoff.md) §4).
M1~M5 와 딸린 두 과제(floor 파라미터 단일 정의·mcptt-info 규격 인코딩)는 .48 에서 반영·배포(**csp 0.2.167 · cmp 0.2.103 · csc 0.2.134 · oam 0.2.178 · worker 0.1.33**)·실측했다. .45 스택에 올릴 때는 이 버전 이상.

| # | 판정 | 반영 | .48 실측 |
|---|---|---|---|
| M1 MSRP 배포 유실(발신 leg 가 통지보다 먼저 끝남) | ✅ | 끝난 수신 leg 의 cmdp 세션을 30 s 기억해 통지를 한 번 받는다 — 배포는 통지 payload 만으로 한다([mcdata_messaging.md](../design/features/mcdata_messaging.md) §4.7) | 013 → g005 2403 B media plane → 014 `plane=media` 수신, 계측기 `MCDATA-SDS-GROUP-MEDIA` ×5 pass. 경합 창(서브 ms) 자체는 재현하지 못했다 |
| M2 경보 Request-URI = 참여 기능 PSI | ✅ | 대상 그룹 = 본문 `mcptt-request-uri`, Request-URI 그룹은 전환기로 받는다. 경보는 `CPttAsModule::OnEmergencyAlert` | `cimsue-cli --mcptt-psi sip:mcptt_psi@ptt.cims.example.kr alert g005` 200 → CSP `R-URI(mcptt_psi) fanout=3` → 014 수신 · 그룹 R-URI(전환기) 200 |
| M3 경보 팬아웃 본문 재작성 | ✅ | §6.3.3.1.11·§6.3.3.1.12·§12.1.3.2 — request-uri = 수신자, calling-user-id·calling-group-id, 취소의 originated-by·emergency-ind false, Accept-Contact·P-Asserted-Service, 위치 파트 | 014 수신 `group=g005 user=+82500000013` · 015 제3자 취소 `originated_by=+82500000013 emergency=-1` |
| M4 ad hoc 인가 규격 요소 | ✅ | `<cp:actions><anyExt><allow-adhoc-group-call>`(TS 24.484 **Rel-18** §8.3.2.1 11)xxxviii)R)) + 전환기 별칭 `<cims:allow-adhoc-group-call>`(옛 ptt-client — P3 이식 뒤 뺀다) | `tests/test_csc_user_profile.py` 12 OK |
| M5 service-config Resource-Priority | ✅ | service-config 문서 **전체를 TS 24.484 §8.4 스키마로 재구성** — `<service-configuration-info>` › `<service-configuration-params domain>` › `<common><broadcast-group>`(계층 수) · `<on-network>`(`<fc-timers-counters>` 17 요소 = CMP floor 기본값 · `<emergency-/imminent-peril-/normal-resource-priority>` = `mcpttp` 15/8/0). 값 = DB 행(N2·계층 수) + CSC 설정 `ServiceConfig.*`(타이머 ms·RP). **시스템 인가 스위치 5종 제거**(1:1·긴급·경보·발언 요청·그룹 생성 — §8.4 에 없는 요소, 인가 = user profile ruleset·그룹 문서) | `tests/test_csc_user_profile.py` 14 OK(스키마 순서·필수 요소·기본값·덮어쓰기) · SDK `CmsDoc.*` |

P3(ptt-client SDK 전환) 실측에서 더 드러난 것 — .48 반영·배포(**csp 0.2.168 · worker 0.1.34**(cimsue-cli))·실측([sdk_port_handoff.md](sdk_port_handoff.md) §5):

| # | 판정 | 반영 | .48 실측 |
|---|---|---|---|
| M6 멤버 leg INVITE 의 resource-lists | ✅ | 멤버 leg INVITE = mcptt-info + SDP 만(TS 24.379 §6.3.3.1.2, 부록 A.1.3-7). 명단은 conference 이벤트(§10.1.3)·GMS. `BuildResourceListXml` 삭제 — 수신 쪽(SDK·앱·cspsim)에 이 명단을 읽는 코드가 없었다 | g001 멤버 INVITE **2194 B**(명단 없음) · 계측기 `PTT-GROUP-CALL-BASIC`·`FLOOR-HANDOVER`·`BROADCAST`·`LISTEN-ROSTER`(visible·hidden)·`NONMEMBER-DENIED` pass · cimsue-cli 착신 자동 응답·floor TAKEN |
| M7 Contact 특성 태그 · refresher | ✅ / 편차 | Contact = `+g.3gpp.mcptt;+g.3gpp.icsi-ref="…mcptt";isfocus` · 서비스 식별 = `P-Asserted-Service`(§6.3.3.1.2 3), RFC 6050 — 경보 팬아웃의 `P-Asserted-Service-Id` 도 헤더 이름을 바로잡음). **refresher 는 `uac` 유지** — 규격은 "생략 권고, 싣는다면 uac" 라 허용 값이고, 생략하면 단말이 갱신자를 골라(부록 A.1.3-24 = uas) 서버측 leg 회수(leg_liveness §5.3)가 단말 구현에 기대게 된다 → 권고 편차로 [mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md) §C4a | 와이어 확인 · 경보 fanout=1 수신 |
| M8 영상 키프레임 INFO 501 | ✅(단말) | 서버는 그대로 — INFO 는 Allow 에 없고 501 은 RFC 3261 §8.2.1 대로다. 키프레임 요청의 규격 경로는 RTCP AVPF PLI/FIR(TS 26.114 §7.3)라 **SDK 가 SIP INFO 를 보내지 않는다**(`reqKeyframeMethod = RTCP_PLI` — 발신·응답·재개) | Linux 빌드는 영상 off(`PJMEDIA_HAS_VIDEO 0`)라 실측 불가 — `cimsue_test` 86 OK·음성 호 회귀. **Android·Windows 는 재빌드 뒤 영상 호에서 INFO 가 없는지 확인(.45)** |

- **개시자 200 OK 규격화(§6.3.3.2.3.2) — 반영(csp 0.2.171)** — Contact 특성 태그+`isfocus`(psip `SetContactParams` — 주소는 스택, 갱신 re-INVITE 2xx·
  in-dialog 요청에도) · refresher `uac`+`Require: timer`(psip 다이얼로그 정책 `SetSessionRefresher` — **이제 개시 단말이 90 s 마다 갱신**하고 CSP 는 만료 감시,
  timer 미지원 단말은 RFC 4028 대로 `uas`) · PAI = 그룹 URI · `Supported: tdialog`. 같이 고친 psip 결함 = **세션 갱신 answer 가 `m=application 0`**(floor
  거절)으로 나가던 것 — 수신 re-INVITE 경로가 floor 포트·fmtp·video 포트·코덱 목록을 잃었다. 규격 단말이면 첫 갱신(90 s)에 floor 가 꺼진다.
  실측: 013→g005(014) 150 s 호 — 단말 갱신 re-INVITE 200(태그·`m=application 54018`)·CSP→멤버 갱신 200, floor 8/8 승인 · 긴급 상향/하향 200·멤버 재광고 수신 ·
  계측기 PTT 8종·VoLTE hold/TLS 4종 pass · psip 단위시험 H·I. **.45 확인** — Android·Windows 단말이 개시자 leg 갱신 re-INVITE 를 보내는지(pjsua 기본 timer
  OPTIONAL 이면 보낸다), 갱신 없이 180 s 에 끊기는 단말이 없는지.
- **in-dialog 목적지 재해석이 AoR 대표 바인딩을 고르던 것 — 반영(csp 0.2.172)** — 같은 AoR 에 단말이 둘이면(다중 단말, 계측기 워커 상시 풀 + cimsue-cli)
  CSP 의 갱신 re-INVITE·BYE 가 **다이얼로그 상대가 아닌 단말**로 가서 leg 이 끊겼다. 이제 psip 이 다이얼로그 remote target(상대 Contact)을 `EventGetLegDest` 에
  넘기고, CSP 는 등록 Contact 가 그와 같은 바인딩으로만 교정한다(`CUserMap::SelectForTarget`) — 아니면 다이얼로그 주소 유지(RFC 3261 §12.2.1.1). 같이 고친 psip
  결함 = 수신 re-INVITE 가 remote target 을 갱신하지 않던 것(§12.2.2 target refresh). 실측: 002(A) 멤버 호 중 002(B) 등록 → 90 s 갱신 re-INVITE 가 A 로
  (`LegDest … 이 다이얼로그 상대가 아니다 — 다이얼로그 주소 유지`), floor 8/8, B 는 수신 없음 · 같은 단말 승격 TCP→UDP 교정 유지 · 계측기 PTT·VoLTE 전달/픽업/BLF/hold/TLS
  pass · psip 단위시험 G. 새 호·통지는 여전히 마지막 등록 단말로 간다 — 다중 단말은 범위 밖([registration_binding_set.md](../design/features/registration_binding_set.md) §8).
- **남은 편차 보완 — 반영(csp 0.2.173 · csc 0.2.135 · oam 0.2.179 · worker 0.1.35, DB `sql/migrate_ptt_groups_ack_call_setup.sql` 공유 DB 적용 완료)**
  - **확인 통화 설정**(TS 24.379 §6.3.3.3·§10.1.1.4.2) — 새 세션 개시의 200 OK 를 멤버 응답 뒤로(개시자 응답 게이트). 그룹 속성
    `min_number_to_start`·`ack_timeout_sec`(TNG1)·`ack_action`(proceed/abandon), 멤버 `required`(`<on-network-required>`) — DB·CSC 그룹 문서·관리 API·
    콘솔 그룹 편집·CSP. **기본값(필수 없음·최소 0) = 종전 동작**. CSC 그룹 문서가 모든 멤버에 싣던 `<on-network-required/>` 는 필수 멤버에만.
    사설 호는 착신자 180 전달·착신자 200 뒤 수락(§11.1.1.4.2). 실측(g005): 최소 1 → 멤버 200 뒤 수락 · 필수 014 정지 + abandon → 5 s 뒤 480 +
    Warning 112·세션 회수 · proceed → 200 + Warning 111 · 필수 응답 → TNG1 정지·경고 없음 · 사설 호 180→200. g005 는 원복.
  - **세션 식별자 GRUU**(§4.5) — Contact `…;gr=<세션 토큰>`, 재합류 R-URI 가 끝난 세션이면 404(§10.1.1.4.5.1 2)) — 실측 404·진행 중 식별자는 합류 200.
  - **Warning 형식**(§4.4) — `399 <PTT 도메인> "<코드> <문구>"`(종전 `105 CIMS "…"` 은 RFC 3261 warn-code 자리에 MCPTT 코드를 넣은 비규격). 105·138·111·112·MCData 203.
    libcsim 은 따옴표 안 코드를 읽는다 — `PTT-GROUP-LISTEN-CONF-DENIED` pass.
  - **.45 몫** — ① **CSC 를 0.2.135 이상으로 올린 뒤** 관제 앱으로 그룹을 저장해야 한다: 옛 SDK·앱은 모든 멤버에 `<on-network-required/>` 를 실어 보내고 새 CSC 는
    그 표시를 읽는다 → 저장한 그룹의 멤버 전원이 필수가 된다. 새 SDK(C++·C API·.NET·Kotlin `required`)와 관제 앱 두 벌(행 모델이 `required` 보존)은 읽은 값만
    되돌린다 — Windows·Android 재빌드가 선행이다. ② CSP 0.2.173 과 CSC 0.2.135 를 같은 창에(CSP 는 새 열을 SELECT 한다 — 마이그레이션은 적용됨).
- **남은 편차 보완 2 — 반영(csp 0.2.174 · cmp 0.2.104 · csc 0.2.136 · oam 0.2.180, DB `sql/migrate_ptt_non_ack_users_info.sql` 공유 DB 적용 완료)** —
  [mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md) C4b·C4c·C4f·C4g.
  - **affiliation 검사**(§10.1.1.4.2 14)a)·15)a)·§10.1.1.4.5.1 8)) — `require_affiliation` 그룹에 affiliate 하지 않은 개시·합류·재합류 = 403
    `"120 user is not affiliated to this group"`. 인가된 긴급·임박·chat = 암묵적 affiliation(§9.2.2.3.7 — 기록·NOTIFY·감사). 실측: 013 무 affiliation → 403
    120 · `--emergency` → 암묵 affiliation 뒤 200 · de-register 회수.
  - **Warning 전달**(§6.3.3.2.3.2 7)) — 게이트 동안 멤버 응답의 Warning 을 개시자 200 에(111 뒤, 쉼표 연결).
  - **멤버 확인 전 수락 + 미디어 버퍼링**(§10.1.1.4.2·§11.1.1.4.2) — CMP `PttMediaBufferMs`(기본 5000, HEARTBEAT `resource.media_buffer`)가 첫 수신자
    합류 전 화자 음성을 담았다가 원래 간격으로 재생한다. 버퍼링이 있으면 즉시 수락 200 에 `P-Answer-State: Unconfirmed`, 없으면 첫 멤버 200 뒤 수락.
    멤버 183 Unconfirmed(게이트 중·TNG1 정지)도 수락 계기, 신뢰성 18x 는 PRACK. psip 새 콜백 `EventInviteResponse`(응답 원문). 실측: 멤버 정지 상태
    `--implicit` 개시 → `buffering` → 합류 시 12패킷(192 ms) 재생.
  - **미응답 멤버 INFO**(§6.3.3.3) — 111 로 진행했고 개시자 프로파일 `allow_non_ack_users_info`(TS 24.484 `<allow-to-receive-non-acknowledged-users-information>`,
    콘솔 가입자 PTT 회선 «그룹 통화»·관리 API·관제 앱 디렉터리 `allowNonAckUsersInfo`)면 ACK 뒤 INFO `Info-Package: g.3gpp.mcptt-info` +
    `<anyExt><non-acknowledged-user>`. psip `SendInfoWithBody`. 실측(g005 014 필수·proceed 임시, 원복): 200+111 → 1 s 뒤 INFO `tel:+82500000014`.
  - **SDK** — INFO g.3gpp.mcptt-info 200(종전 pjsua 500)·`CallInfo.nonAcknowledgedUsers`/`onNonAcknowledgedUsers`·`CallInfo.answerState`, 모르는 패키지 469.
  - **.45 몫** — ① SDK(C++ 코어) 재빌드면 INFO 가 200 이 된다 — 옛 SDK 는 500 을 답하지만 호에는 영향이 없다. C API·.NET 노출과 Windows 관제 앱 표시(⑤ «미응답 멤버 n명»
    + 토스트, «멤버 확인 전 연결»)는 반영, Kotlin 노출·Android 표시는 Android 몫. ② CMP 0.2.104 이상이면 개시 200 이 멤버 확인 전에 나가고 `P-Answer-State: Unconfirmed` 가 붙는다 —
    CMP 를 `PttMediaBufferMs=0` 으로 두면 첫 멤버 200 뒤 수락(홀로 개시한 호는 480). ③ `require_affiliation` 그룹은 affiliate 하지 않은 단말의 일반
    개시·합류가 403 120 이 된다 — 단말 앱이 그룹 선택 시 affiliate 하는지 확인. ④ CSP 는 새 컬럼을 기동 때 확인한다(마이그레이션 뒤 CSP 재기동).
  - 남은 편차 — Supported `norefersub`/`explicitsub`/`nosub` 미광고(그룹 세션 REFER 미지원) · 진행 MESSAGE 안내(선택) · 개시 전 affiliation 인원 검사
    (`<on-network-minimum-number-of-affiliated-members>` 등 그룹 문서 요소 없음) · 멤버별 Answer-Mode(전원 자동 응답으로 봄).
  - 계측기 회귀(tb48): PTT 13종·MCData 4종·VoLTE 7종·TRUNK early media pass. `VOLTE-FA-PARALLEL` 은 대표번호 `70200` 이 다이얼 플랜(volte) 번역 불가
    484 — 시나리오·설정 과제(이번 변경 무관). `PTT-GROUP-LISTEN-CONF-DENIED` 는 연속 실행 중 1회 합류 집계 시한 초과, 단독 재실행 pass.
- **cimsue-cli 관찰** — `--from-profile ptt` 로 띄우면 `--mcptt-psi` 가 먹지 않는다(경보 R-URI = 그룹, 전환기로 200). 명시 계정(`--server …`)으로는 PSI.

- 참고(관찰): 두 단말이 같은 사내 NAT 뒤에서 영상 통화할 때 CMP 가 한 peer 자리의 RTCP 목적지를 두 포트 사이에서 몇 초마다 다시 latch 하고, 다른 peer 의
  영상 RTCP 를 "unnegotiated src" 로 한 번 버린다(`PRtpRelay`). 영상·음성 품질에는 영향이 보이지 않았다(손실 0.3 %·RTT 17 ms) — 같은 공인 IP 뒤 두 peer 의
  latch 판정 확인 필요.

- **M5 에 딸린 변경** — SDK `ServiceConfigDoc`(§8.4 해석: domain·계층 수·`rpEmergency/rpImminentPeril/rpNormal` r-value) ·
  `UserProfileDoc.allowPrivateCall` · `Capabilities`(인가 = user profile 만, `transmitRequest` 제거, N2 = user profile) · Kotlin 파사드 같은 구조
  (SWIG 재생성·빌드는 .45) · 옛 ptt-client(인가 = user profile, 발언 요청 게이트 제거, N2 = user-profile `MaxAffiliationsN2`, RP 해석 —
  빌드 확인은 .45) · 콘솔 **구성 > MCPTT 정책** = N2·계층 수만 · 관리 API `GET/PUT /api/v1/mcptt/service-config` 도 그 셋만.
- **DB 마이그레이션(보류)** — `sql/migrate_service_config_drop_switches.sql`(스위치 컬럼 5개 DROP). 같은 DB 를 쓰는 옛 CSC 는 그 컬럼을
  SELECT 하므로 **.45·.135 CSC 가 0.2.133 이상이 된 뒤** 적용한다. 새 CSC 는 적용 전에도 정상이다.
- **floor 파라미터 단일 정의** — 정본 = service-config 문서(CSC 설정 `ServiceConfig.*` — `TransmitTime.TimeLimit`(T2)·`FcTimersCounters.*`).
  CSP `CCspServiceConfig` 가 CSC 내부 API `GET /internal/mcptt/service-config`(TS 24.484 Annex A.2.3)로 받아(기동·SIGUSR1·CSC_RESTART·
  SERVICE_CONFIG_CHANGED — CSC 설정 재적재로 문서가 바뀌어도 통지) PTT_GROUP_ADD/MODIFY `floor_timers` 로 CMP 에 싣는다. CMP 는 C7·C20
  (`c7_idle`·`c20_grant`)도 받는다. CMP 설정 `Floor*Sec` 는 폴백. 실측: CSC T2 30 → 20 s → CSP `[service-config] … T2=20` → g005 그룹콜
  CMP `floor timers: … T2=20s … C7=3 C20=3` → 30 s 복원.
- **mcptt-info 규격 인코딩(Annex F.1)** — contentType 요소 = `type="Normal"` + `<mcpttURI>`/`<mcpttString>`/`<mcpttBoolean>` 자식, 요소 순서 =
  mcptt-ParamsType sequence. 송신 전환 = CSP(그룹 INVITE·긴급 403·경보 통지)·SDK 코어(`mcpttInfo`·`alertInfo`)·옛 ptt-client `McpttXml`·
  cspsim/libcsim. 수신 = 모두 두 형식(옛 송신자의 값 직접 기재도) — CSP `McpttElemValue`·SDK `localText`·ptt-client DOM·android core-sip 정규식·
  cspsim `ExtractXmlTag`. 실측: 새 CSP↔새 SDK 로 그룹콜 fan-out 본문 규격형, 긴급 상향 Confirmed·재광고 수신, 경보·취소 200·수신.
  **올리는 순서(.45)** — 새 SDK·앱은 규격형으로 보내므로 CSP 0.2.167 이상이 먼저(옛 CSP 는 자식 형식 지시자를 못 읽어 긴급·경보를 놓친다).
  새 CSP 는 규격형으로 보내므로 옛 SDK·옛 앱(값 직접 기재만 읽음)은 함께 다시 빌드한다 — 서버·단말을 같은 창에 올린다.
- **SDK 쪽 반영(.48)** — `AccountConfig.mcpttServerUri`(경보 Request-URI = PSI, 비면 그룹 URI) · `cimsue-cli --mcptt-psi`. 앱은 ue-init-config
  `MCPTT-Service-Details/Server-URI` 를 넣는다. C API·.NET 노출은 Windows 반영(관제 데스크톱은 ue-init-config 를 읽지 않아 비어 있다 — 그룹 URI 전환기),
  Kotlin 파사드는 .45 몫(sdk_port_handoff §4.1).
- **옛 ptt-client(.45 빌드 확인 필요)** — ad hoc 인가를 규격 요소 먼저 읽고, 경보 그룹은 `mcptt-calling-group-id` 먼저(`PttController.kt`·`McpttXml.kt`).
- **설정(.48 → 공유 DB)** — g005 그룹 능력 `emergency_call` 켬 · 계측기 신원 013 의 user profile 긴급 대상 = `DedicatedGroup g005`.
  `cimsue-cli group-call g005 --upgrade-at 3 --cancel-at 7` → 상향 **Confirmed 200** · 하향 Confirmed 200. .45 CSP 는 같은 DB 라도 캐시가 통지로만
  갱신되므로 .45 에서 시험하기 전에 CSP 재적재가 필요할 수 있다.
- **남은 경보 편차** — `<mc-org>`(§6.3.3.1.12, 값 정본 = CSC 설정)·수신 확인 `<alert-ind-rcvd>`(§6.3.3.1.20) —
  [mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) §4.3 편차 표.
- **보안 관찰** — CSP `CmdpClient` 는 이벤트 datagram 의 출처(cmdp 주소)를 검사하지 않는다. 별도 과제.

## 10. 긴급·경보 관제 경로 — 서버 반영분의 .45 짝

Windows 관제 앱 보완이 서버에 남긴 과제(긴급·임박 해제 인가, 청취 leg 조건 재광고, 경보 취소 인가, Resource-Priority 정본, TNG2·MESSAGE 긴급 해제,
SDS 전달 확인 규격 경로)를 .48 에서 반영·배포(**csp 0.2.180 · csc 0.2.138 · oam 0.2.182**)·실측했다. 정본 = [mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md)
§4.2·§4.3 · [mcdata_messaging.md](../design/features/mcdata_messaging.md) §4.4.

- **DB** — `sql/migrate_ptt_user_profile_cancel_authz.sql`(`allow_cancel_group_emergency` 0 · `allow_cancel_imminent_peril` 1 ·
  `allow_cancel_emergency_alert` = 발령 값) **공유 DB 적용 완료**. 컬럼 추가뿐이라 옛 CSP·CSC 는 영향이 없다.
- **해제 인가(TS 24.484 ruleset)** — 긴급 해제 = 개시자 ∨ `allow-cancel-group-emergency`(local policy §6.3.3.1.13.4), 임박 해제 = `allow-cancel-imminent-peril`
  (§6.3.3.1.13.6), 경보 취소 = `allow-cancel-emergency-alert`(§6.3.3.1.13.3). 비인가·다른 긴급 사용자 송출 중(7a))은 **403 + 현재 상태 지시자**.
  CSC user profile 문서가 세 요소를 싣고(규격 목록 순), 콘솔 PTT 회선 카드 «긴급 (SOS)» 에 체크박스가 있다. 관제사에게 긴급 해제를 주려면
  그 사람의 PTT 회선에 «긴급 해제» 를 켠다.
- **단말에 보이는 동작 변화** — 미인가 경보 **발령**도 이제 403 + `alert-ind` false(§12.1.3.1 4)a) — 전에는 200 으로 받고 전파만 안 했다).
  SDK 는 `onRequestResult` MESSAGE 403 으로 받는다. 옛 ptt-client 의 경보 배너 정합은 403 을 실패로 다뤄야 한다(빌드 확인은 .45).
- **SDK 코어(.45 몫, [ue_sdk.md](../design/features/ue_sdk.md) §11)** — ① user profile 의 `allow-cancel-group-emergency`·`allow-cancel-imminent-peril` 해석 →
  C API·.NET·Kotlin (관제 앱 [긴급 해제] 자격이 기다린다) ② `sendSdsNotification` 규격 본문(MCData PSI · resource-lists · `mcdata-calling-group-id` ·
  ICSI Accept-Contact) — CSP 는 규격형·옛 형식 둘 다 받는다(서버 대역 `tests/sds_disposition_spec.py`) ③ `setCallCondition` 의 임박 → 긴급 상향이
  `imminentperil-ind` false 를 함께 싣는 것(§6.3.3.1.17 조합 위반 — 서버는 지금 검증하지 않는다).
- **.45 스택에 올릴 때** — CSP·CSC 를 함께(CSC 가 새 프로파일 요소를 내고 CSP 가 같은 값으로 판정한다). 시험 전에 해제 권한이 필요한 관제
  계정의 «긴급 해제» 를 켠다 — 기본값은 개시자만이다.
- **.48 실측** — cimsue-cli(013 개시·014/015/016 멤버·010 청취): 비인가 해제 403(`emergency-ind` true, 코어 `denied`) · 인가 해제 200 + 참여 leg
  재광고(멤버 leg 는 초기 fan-out 구성의 floor `m=application`, 개시자 leg 는 성립 SDP 그대로) · 비참여 제휴 멤버 통지 MESSAGE · 청취 leg 재광고
  (`sendonly` 오퍼 ↔ `recvonly` 응답, 은닉 청취는 로스터 비노출) · 7a) 개시자 발언 중 해제 403 · 경보 취소 비인가 403(`alert-ind` true) · 제3자 경보
  취소 전파(`originated-by`) · 미인가 발령 403 · MESSAGE 긴급 해제(경보는 남김, 1)b)) · TNG2 8 s 만료 해제 · RP `mcpttp.14` 반영 ·
  `tests/sds_disposition_spec.py` 11/11. 계측기 PTT·MCData 회귀 pass. Warning 149(+INFO)·임박 해제는 cimsue-cli 에 해당 동작이 없어 코드 경로만.

**.45 반영 결과 (→ .48)**

| 항목 | 상태 |
|---|---|
| ① 해제 인가 요소 | ✅ SDK 코어 `UserProfileDoc.allowCancelGroupEmergency`·`allowCancelImminentPeril`(TS 24.484 §8.3.2.1 11)xiv)·xvii), 요소 없음 = 허용) → `Capabilities.cancelGroupEmergency`·`cancelImminentPeril`. C API(`cimsue_user_profile_doc_t`·`cimsue_capabilities_t` 끝에 두 필드)·.NET(`UserProfileDoc`·`Capabilities` 끝 인자, 기본 true)·Kotlin 같은 이름. 관제 앱은 `cancelGroupEmergency ∨ McpttCondition.mine` 으로 [긴급 해제] 를 연다(§6.2.8.1.7 local policy — 서버 판정과 같은 식) |
| ② disposition 통지 규격 본문 | ✅ TS 24.282 V18.13.0 원문 대조 — `sendSdsNotification(…, groupId)`: 계정 `mcdataServerUri`(= ue-init-config `MCData-Service-Details/Server-URI`)가 있으면 Request-URI = PSI, Accept-Contact `+g.3gpp.mcdata.sds`·ICSI mcdata.sds(require;explicit)·P-Preferred-Service(§6.2.4.1), 본문 = [mcdata-info `<mcdata-calling-group-id>`] + SDS NOTIFICATION + resource-lists entry 하나(§12.2.1.1 3)·5)·6)). PSI 가 없으면 옛 형식(원 발신자 직행 — 전환기). 수신 SDS 의 `fromUri` = `<mcdata-calling-user-id>`, `groupUri` = `<mcdata-calling-group-id>` 우선(§12.2.1.1 — 통지 대상 결정), 중계된 통지의 request-uri 는 그룹으로 읽지 않는다. C API `cimsue_engine_send_sds_notification` 에 `group_id` 인자(msg_id 뒤) — **DLL·.NET 을 같이 다시 빌드** |
| UE initial configuration | ✅ 규격 경로로 PSI 를 받는다 — `CscClient::fetchUeInitConfig(mcsUeId)`(TS 24.484 §7.2.1.1 XCAP URI `users/sip:<MCS UE ID>/<MCS UE ID>`, 토큰 없음) → `UeInitConfigDoc{mcpttServerUri, mcdataServerUri}`. C API `cimsue_csc_fetch_ue_init_config`·`cimsue_ue_init_config_parse`(struct id `UE_INIT_CONFIG_DOC`)·.NET `CscClient.FetchUeInitConfig`·Kotlin `fetchUeInitConfig`. `AccountConfig.mcdataServerUri` 신설(C `mcdata_server_uri` — `mcptt_server_uri` 뒤). Kotlin `AccountConfig.mcpttServerUri` 도 이제 있다(§9 .45 몫 해소) |
| ③ 조건 조합(§6.3.3.1.17) | ✅ 긴급(개시 INVITE·상향 re-INVITE) = `emergency-ind` true + `alert-ind` false(§6.2.8.1.1 4)), 임박 지시자 없음 — 임박 → 긴급 상향도 같다(제어 기능이 임박을 내린다, §6.3.3.1.6 3)d)). 임박 = 긴급·경보 지시자 없이. 긴급 중 임박 상향은 코어가 거절(§6.2.8.1.9 1)). 요소 순서 = mcptt-ParamsType. 옛 CSP(0.2.176)도 re-INVITE 의 `alert-ind` 를 보지 않아 영향 없음 |
| 옛 ptt-client 경보 403 | ✅ `PttEmergency.onAlertResult` — 경보 MESSAGE 최종 응답을 token 으로 받아 발령 403(+ `alert-ind` false, §12.1.3.1 4)a)) = 내 배너 회수, 취소 403(+ `alert-ind` true) = 배너 복원. ptt-client 는 등록 직전 ue-init-config 로 PSI 둘을 채운다(경보 Request-URI = MCPTT PSI — CSP 0.2.167+ 수용), 받은 SDS 의 그룹을 통지에 넘긴다 |
| cimsue-cli | `--mcdata-psi URI`·`--instance-id URN` · `--from-profile ptt` 면 ue-init-config 로 PSI 를 채우고 명시값이 덮는다(§9 «--mcptt-psi 가 먹지 않는다» 해소) · `sds-recv --notify-delivered` · `sds … --wait-disposition S` |
| 검증 | S1-UE-UNIT(cimsue_test 98 — 새 `CmsDoc.ParseUeInitConfig`·`SdsCodec.NotificationSpecForm`·`SdsCodec.CallingIdentitiesFromMcdataInfo`·`McpttXml.IndicatorOrderAndAlert`, `McpttCondition` 에 임박→긴급 와이어·규격형 통지 MESSAGE)·S1-CPP-FORMAT·S1-UE-ANDROID-BIND·S1-UE-ENGINE-SINGLE·S1-UE-CSC-XCHECK·S1-UE-TABLET-UNIT(381)·S1-PY-SYNTAX PASS, APK 4종(ptt·volte·dispatch-tablet·sdk-probe) 빌드. .NET 은 이 호스트에 dotnet 이 없어 **Windows 쪽 빌드·`CimsUe.Tests`(AbiLayout·CscTests 확장) 필요** |
| .48 실측(cimsue-cli, CSP 0.2.180) | 013 → g005 SDS(전달 요청) → 014 `--notify-delivered`(PSI `sip:mcdata_psi@ptt.cims.example.kr`) → CSP 상관·중계 → 013 `disposition:2`(from `tel:+82500000014`·group `tel:g005`) · 013 g005 그룹콜 긴급 상향(`emergency-ind` true + `alert-ind` false) 200 Confirmed → 해제 200 · `--from-profile ptt` → ue-init-config `mcptt=sip:mcptt_psi@…`, **MCData 는 미광고**(.48 CSC `UeInitConfig.ServiceDetails.McData.Enable` off — 통지는 옛 형식) |

- **Windows 쪽 몫(관제 앱 두 벌)** — ① [긴급 해제] = `Capabilities.CancelGroupEmergency ∨ condition.mine` ② 받은 SDS 통지에 `GroupUri` 넘기기(데스크톱 `McDataMessagesViewModel`·태블릿 `PttPlane.applySds`) ③ 계정 만들기 전 `FetchUeInitConfig` → `McpttServerUri`·`McdataServerUri`(데스크톱은 지금 둘 다 비어 있다) ④ 데스크톱은 새 DLL·.NET 을 같이 빌드(C API 인자 변경).
- **.45 배포 완료(2026-09-30 20:22)** — oam 0.2.182·csp 0.2.180·csc 0.2.138(pkg 283~285, `--no-bump` = .48 라벨), oam-svc 0.2.131 재기동.
  CSP 기동 Roles 전부 ON · service-config CSC 정본 적재(RP 15/8/0, TNG2 없음) · csc.json 보존 · MF52·W999 새 APK(ea2b617d) 재등록 200.
  CSC MCData 광고(`UeInitConfig.ServiceDetails.McData.Enable`)는 아직 off. 관찰 = 001·002 가 g001 멤버가 아니라 affiliation 403(멤버 구성 변경).
- **.45 스택에 올릴 때** — CSP 0.2.180·CSC 0.2.138(·oam 0.2.182) 를 한 창에(§10 위). 새 CSP 가 선 뒤에만 .45 CSC 의 `UeInitConfig.ServiceDetails.McData.Enable` 을 켠다 — 켜면 새 SDK 단말이 PSI 로 통지를 보내는데 옛 CSP 는 그것을 상관하지 못한다.

## 11. MCVideo — M0 계약 (.45 몫 K5·K7·V0 SDK) · .48 리뷰 요청

정본 [mcvideo.md](../design/features/mcvideo.md) · 분담 [mcvideo_dev_plan.md](mcvideo_dev_plan.md) §3(계약 K1~K7)·§4. 규격 원문 = TS 24.581 V18.8.0 · TS 24.281 V18.14.0 ·
TS 24.484(docx k00). 이 절은 두 호스트가 MCVideo 계약을 주고받는 자리다 — 소유 호스트가 계약물을 먼저 고치고 여기에 적는다.

**.45 가 낸 것 (→ .48 리뷰)**

| 계약 | 산출 | 리뷰 요청 |
|---|---|---|
| K5 전송 제어 정의 | 정본 `docs/design/features/mcvideo_tc_defs.yaml`(MCV0/1/2 subtype·메시지별 필드·field id·값 모양·indicator·source/permission/result/reception mode·원인 셋·§11 타이머/카운터·§12.1.2 fmtp) → `scripts/gen_mcvideo_tc_defs.py` 가 **CMP `cmp/PTransmissionDefs.h` 와 SDK `sdk/core/src/mcvideo/tc_defs.h` 를 함께 생성**. S1 `S1-UE-MCVIDEO-TC-DEFS`(`--check` — 테이블 정합 + 두 생성물 최신) | CMP 이름(`MCV1_TRANSMISSION_GRANTED`·`TF_AUDIO_SSRC`·`TC_REVOKE_PREEMPTED`·`MCV_T3_MS` …)이 B3~B5 상태 머신에 쓸 만한지. **생성물은 손으로 고치지 않는다** — 바꿀 값은 yaml 에서(이 절에 적고) |
| K7 SDK 공개 표면 | `McService` · `AccountConfig.mcvideoEnabled`·`mcvideoServerUri` · `affiliate(…, service)` · `joinVideoGroupCall(VideoGroupCallOptions)` · `requestTransmission`·`releaseTransmission`·`acceptReception`·`endReception`·`transmissionInfo` · `Listener::onTransmission`·`onReception` · `CallInfo.service` — 선언 + 실패 반환 스텁([ue_sdk.md](../design/features/ue_sdk.md) §4.6). SWIG Java 확인(불투명 타입 0) | 이름·이벤트 종류(Windows 관제 앱도 — [dispatch_windows_next.md](dispatch_windows_next.md) W3) |
| V0 SDK 몫(C1) | `GroupDoc::toXml` MCPTT `<service enabler>` = `urn:urn-7:3gpp-service.ims.icsi.mcptt`(TS 24.379 Annex E.2.1). CSC `parse_group_document_xml` 은 enabler 를 보지 않아 옛 CSC 와도 맞는다 | A1(CSC 생성 쪽)은 .48 |

검증: `cimsue_test` 98/98 · `S1-UE-MCVIDEO-TC-DEFS`·`S1-UE-FLOOR-CODEC` PASS · 두 생성 헤더 단독 컴파일(`-Wall -Wextra`).

**K5 를 만들며 원문에서 본 것 — .48 계약(K2·K4·K6)에 넘긴다**

1. **service configuration `<tc-timers-counters-R14>` 는 요소 17개가 전부 필수**다(TS 24.484 XSD — `anyExt` 외 minOccurs 없음, T100~T104·C2·C4·C6·C7·C11 은
   unsignedByte, 나머지 `xs:duration`). A4 가 이 요소를 내려면 전부 채우거나 통째로 뺀다. 값은 yaml 의 기본값 — 규격에 기본값이 없는 **T100~T104·T2 는 CIMS 1 s**
   (`origin: cims`, 단말 재전송 총 시간 < 6 s 권고 NOTE 1~4). 다른 값을 쓰려면 yaml 을 먼저 바꾼다.
2. **T1·T5 는 그룹 문서 값** — T1 = MCPTT `on-network-hang-timer` 재사용(TS 24.581 §11.1.3), T5 = `on-network-reception-hang-timer`(TS 24.481). K6 PTT_GROUP_ADD
   (`service: mcvideo`)에 둘 다 싣는 것을 제안한다.
3. **C9 = 멤버별 동시 수신 상한 = user profile `<MaxSimultaneousVideoStreams>`**(없으면 4, 1차 CIMS 1) — CMP 가 멤버마다 알아야 Receive Media Request 를
   원인 #7(Max no of simultaneous stream)로 거절한다. K6 JOIN 에 멤버 값(예 `max_rx_streams`)을 제안한다. C7(2)·C11(4)은 service configuration 값(서버 전역).
4. **fmtp** — 구분자 `;`(§4.3.3.1 예시. §12.1.2.3 ABNF 의 COLON 은 따르지 않는다 — mcvideo.md §9), `mc_transmission_ssrc` 는 값을 싣는다. `mc_transmission_ssrc` =
   **받는 쪽이 기대하는 RTCP 헤더 SSRC**를 서로 광고하고 상대가 그 값을 쓴다(§4.3.3.1 — 한 IP·포트에 여러 세션을 다중화할 때의 열쇠). 1차는 멤버 전용 포트라
   다중화가 없지만 K4 에서 «SDK 는 offer 에 싣는다 · answer 에 CMP 값» 을 제안한다. `mc_audio_ssrc`·`mc_video_ssrc` 는 **암묵적 송출 요청을 받아들인 answer
   에만** 온다(§12.1.2.2).
5. **field ID 가 없는 필드** — Remote ID·SSRC of queued transmission participant·Granted Party's Identity·SSRC of granted transmission participant·SSRC of
   transmission control server(Table 9.2.3.1-1 밖). 모두 off-network·원격 송출이라 1차 코덱은 부호화하지 않는다.
6. **R2(SSRC 재작성)** — 계획의 권고(허가 때 CMP 가 준 Audio·Video SSRC 를 Granted·Media Transmission Notification 에 싣고 그대로 전달)와 K5 는 맞는다 —
   Granted·Notification·Receive Media Request/Response·End 계열이 모두 두 SSRC 필드를 가진다.

**.48 리뷰 결과 (K5·K7·V0 SDK) — 09-30**

- **K5 정의 테이블 = 원문 일치.** TS 24.581 V18.8.0 과 대조 — subtype 세 표(Table 9.2.2.1-1~3)·field ID 24개(Table 9.2.3.1-1)·필드 모양(§9.2.3.2~§9.2.3.23 —
  Length 2/6·가변 패딩·Reception Mode 0 = 자동·1 = 수동)·메시지별 필드(§9.2.4~§9.2.31 — `User Id of the Transmitting User` 가 있는 Taken·Notification·
  Receive Media Request/Response·End 계열 포함)·원인 셋(§9.2.6.2·§9.2.10.2·§9.2.15.2)·지시자·Source·Permission·Result·Queue 특수값 모두 맞다.
- **CMP 이름은 B3~B5 에 그대로 쓴다** — `MCV0_/MCV1_/MCV2_*` subtype · `TF_*`·`TFK_*` 필드 · `TI_*` · `TC_SRC_/TC_PERM_/TC_RESULT_/TC_RECEPTION_*` ·
  `TC_REJECT_/TC_REVOKE_/TC_RECV_REJECT_*` + `Mcv*CauseText` · `MCV_T*_MS`·`MCV_C*`. 기존 CMP 식별자(floor `FLOOR_*`·`FF_*`)와 겹치는 이름이 없다.
  참여 기능 타이머(§11.1.4·§11.2.4)는 MBMS 전용이라 빠진 것이 맞다. 동시 송출 상한 «Cx» 는 그룹 속성이라 K6 `max_transmitters` 로 받는다.
- **yaml 을 고쳤다(.48 — 생성기 재실행, 두 생성 헤더 내용은 그대로, `--check` PASS)**:
  1. `server_counters.C7.element` = XSD 표기 **`C7-reception-accpeted`**(본문 §9.4.2.1·§9.4.2.7 은 accepted — XSD 를 따르지 않으면 service configuration 이 스키마
     검증에 실패한다, mcvideo.md §9).
  2. `server_timers.T1`·`T5` 에 **`element_private`**(`private-call-hang-timer`·`reception-hang-time`) — `<tc-timers-counters-R14>` 필수 17요소를 yaml 이 전부 덮게
     (그룹 호의 T1·T5 는 그룹 문서 값, 1:1 호는 이 두 요소).
- **K7** — 서버 계약(K6)과 맞다. 알림 하나: 규격은 송출자가 Granted 의 Audio·Video SSRC 를 자기 RTP 에 쓰게 한다(TS 24.581 §6.2.4 «use them in the RTP media
  packets»). CMP 는 송출자를 멤버 전용 포트로 가리고 내보낼 때 할당 SSRC 를 찍으므로(K6), SDK 가 1차에 pjmedia 스트림 SSRC 를 바꾸지 못해도 서버 분배는
  깨지지 않는다 — 그렇게 가면 ue_sdk.md 편차로 적어 달라.
- **V0 SDK 몫** — 확인. CSC 생성 쪽(A1)도 이번 .48 커밋에 들어 있다(아래) — 두 끝 모두 MCPTT ICSI enabler.

**제안 1~6 판정 (→ .48 계약에 반영)**

| # | 판정 | 반영 |
|---|---|---|
| 1 | 채택 | K2 — CSC 가 `<tc-timers-counters-R14>` 17요소를 **전부** 싣는다(값 = yaml 기본값). CSC 기본값 = yaml 인지 `tests/test_csc_mcvideo.py` `TcDefsAgreementTest` 가 대조한다 — 값을 바꿀 땐 yaml 먼저 |
| 2 | 채택 | K6 `PTT_GROUP_ADD`(`service:"mcvideo"`) `tc_timers{t1_ms, t2_ms, t3_ms, t4_ms, t5_ms, t6_ms, t11_ms, c2, c4, c6, c7, c11}` — **t1_ms = 그룹 `on-network-hang-timer`**, **t5_ms = 그룹 `on-network-reception-hang-timer`**, 나머지 = MCVideo service configuration(CSP 가 CSC `/internal/mcvideo/service-config` 로 받는다). 미지정 = K5 기본값 |
| 3 | 채택 | K6 `PTT_JOIN.max_rx_streams`(C9 = user profile `MaxSimultaneousVideoStreams`, 초과 → Receive Media Response #7). C7·C11 = ADD `tc_timers` |
| 4 | 채택 + 보강 | K4(mcvideo.md §1.4) — 구분자 `;` · 단말 offer 에 `mc_transmission_ssrc` 값. **서버 answer·fan-out offer 는 늘 `mc_transmission_ssrc` 를 싣는다**(TS 24.281 §6.3.3.1.1 4)·§6.3.3.2.1 2)b) «shall» — TS 24.581 §14 의 «다중화 지원 시» 보다 이쪽, mcvideo.md §9) = CMP 가 JOIN 응답으로 준 `tc_ssrc`. CMP 는 단말 offer 값(`user_tc_ssrc`)을 자기가 보내는 전송 제어 RTCP 헤더 SSRC 로 쓴다. 1차 answer 에 `mc_queueing` 없음(송출 큐 = V8). `mc_audio_ssrc`·`mc_video_ssrc` 는 새 prearranged 세션의 암묵 요청을 받아들일 때만(JOIN `implicit_request` → 응답 `granted`·`audio_ssrc`·`video_ssrc`) |
| 5 | 채택 | 1차 코덱은 ID 없는 필드를 부호화하지 않는다(on-network 그룹 호에 쓰이지 않는다) |
| 6 | 채택 | K6 SSRC 규칙 — 송출 허가 때 CMP 가 송출마다 **전역 유일 Audio·Video SSRC 쌍을 할당·보관**(TS 24.581 §6.3.4.3.3 d)해 Granted·Media Transmission Notification·Receive Media Response·End 계열에 싣고, egress 에 그 값을 찍는다. 수신자별 **Active SSRC List**(§6.3.7)로 분배 |

**.48 이 낸 것 (→ .45 리뷰)** — 이번 커밋. CSC 코드는 미배포, DB 마이그레이션은 공유 DB 에 아직 적용하지 않았다(A2 에서 — 표 추가만이라 옛 코드 무영향).

| 계약·작업 | 산출 | .45 가 볼 것 |
|---|---|---|
| K1 DB | `sql/migrate_mcvideo.sql`(+ `cims_schema.sql`·db_schema.md) — `mcvideo_group_attrs`(행 = MCVideo 그룹) · `mcvideo_user_profile`(행 = 자격, `max_video_streams`·`max_calls_n6`) · **`mcvideo_affiliations`**(서비스별 affiliation — 공유 DB 의 옛 CSP 가 dereg 때 `ptt_affiliations` 를 사용자 단위로 지우고 옛 OAM 이 그 표를 MCPTT 로 세므로 열 추가가 아니라 표를 따로 뒀다) | 없음(서버 몫) |
| K2 설정 문서 | `tests/fixtures/mcvideo/*.xml` + **README(해석 쪽이 확인할 값)** + `xsd/`(원문 XSD + 보조) + `tests/mcvideo_fixture_check.py`(S1 `S1-MCVIDEO-CONTRACT`, xmlschema 없으면 SKIP) | **C2 입력** — 그룹 문서 MCVideo `<service>`·`mcvideo-*`·entry `mcvideo-mcvideo-id` · MCVideo user profile · service configuration(전역 문서 `…/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml`, MIME `application/vnd.3gpp.mcvideo-service-config+xml`) · ue-init-config `MCVideo-Service-Details`(MCPTT → MCVideo → MCData). user profile 주소 = `…/org.3gpp.mcvideo.user-profile/users/<MCVideo ID>/mcvideo-user-profile-1.xml` |
| K4 SDP | mcvideo.md §1.4 «CIMS SDP 프로파일» — 골든 SDP 는 K3 메시지 안에 | C4 offer 모양(m 순서 audio → video → application, `i=`, `udp MCVideo`, rtcp-fb pli·fir) |
| K6 CSP↔CMP | cmp_media_api.md **§7.9**(`PTT_*` + `service:"mcvideo"`, 멤버 `port`·`video_port`·`control_port`, JOIN `tc_ssrc`·`user_tc_ssrc`·`max_rx_streams`·`implicit_request`, SSRC 규칙) + §8 이벤트 `TRANSMITTERS`·`TRANSMISSION_INACTIVITY` | B2 코덱을 CMP 에 붙일 자리(§7.9) |
| A1 V0(CSC) | `get_group_xml` MCPTT enabler = MCPTT ICSI + 규칙 `<is-list-member>`·`<allow-initiate-conference>`·`<join-handling>` | 옛 앱·새 SDK 모두 그룹 문서를 그대로 읽는지(C1 시험) |
| A3·A4·A5 CSC | `csc/src/services/mcvideo.py` — 그룹 문서 MCVideo 몫·XCAP PUT 해석(전환기 규칙 — MCVideo `<service>` 없는 PUT 은 MCVideo 를 건드리지 않는다)·CMS 두 문서·ue-init-config(기본 끔 `UeInitConfig.ServiceDetails.McVideo.Enable`)·scope `3gpp:mc:video_*` 넷(자격 있는 사용자만)·토큰 `mcvideo_id`·`/internal/mcvideo/service-config` | SDK 가 video scope 를 요청하면 자격 없는 사용자는 응답 `scope` 에서 빠진다 |
| K3 SIP (+ K4 골든 SDP) | `tests/fixtures/mcvideo/sip/` — 10개(REGISTER · affiliation PUBLISH · chat 합류 INVITE/200 · prearranged 개시 INVITE/200(암묵 송출 요청 수락·`mc_audio_ssrc`·`mc_video_ssrc`) · 멤버 초대 · 재합류(R-URI = 세션 식별자) · 404 117/118). 전송 바이트 그대로(CRLF·Content-Length — 정본 `build_goldens.py`, `--check`), README = 메시지별 규격 절·요지. S1-MCVIDEO-CONTRACT 가 본문 XSD + K3·K4 규칙(ICSI 헤더·Accept-Contact 둘·m 순서·`udp MCVideo`·`i=`·fmtp `;`·answer 파라미터) 을 본다 | **C3·C4 입력** — 01·02·03·05·08 은 SDK 가 만드는 모양, 04·06·07·09·10 은 SDK 가 읽는 모양. prearranged 시험용 그룹 `tel:g103` 은 K3 에서만 쓴다 |

**B2 — 양 끝 전송 제어 코덱 (.45 → .48)**

| 항목 | 내용 |
|---|---|
| CMP 코덱 | `cmp/PTransmissionCodec.{h,cpp}` — `BuildTransmissionMessage(buf, size, app, subtype, ssrc, fields)`(그 메시지 표 밖 필드·모양이 틀린 고정 필드·모르는 메시지면 **0** — 상태 머신 잘못이 시험에서 드러난다) · `ParseTransmissionMessage`(MCV0~2 아님·모르는 subtype = false(§9.1.4 1), 표 밖·모양 틀린 필드는 버림(§9.1.4 2·3), 헤더 length 끝까지만(§9.1.1)) · `ParsedTransmission`(`op()`·`ackRequired()`·`u8/u16/ssrcOf/str`·`cause()/causePhrase()`·`queuePosition()`·`messageName()`) · 값 빌더 `McvU8/U16/QueueInfo/Ssrc/Name/Cause`. 의존 = 생성 헤더 `PTransmissionDefs.h` 뿐(`PMcpttGroup.h`·pasf 없음). **CMP `CMakeLists.txt` 등록은 B3 때 .48 이**(이 호스트는 cmp 빌드 파일을 건드리지 않았다) |
| SDK 코덱 | `sdk/core/src/mcvideo/tc_codec.{h,cpp}` — 같은 규칙 + 참여자 빌더(Transmission Request·End Request/Response·Queue Position Request·Receive Media Request·Media Reception End Request/Response·Ack(Source 0 + **Message Name 늘** + Message Type 첫 비트 0)) |
| 교차 시험 | `cimsue_test` `McvCodec` 9(골든 바이트·정렬·수신 검사) + `McvXCheck` 5(두 생성 표 전수 대조 · 코어 → CMP · CMP 서버 메시지 → 코어 · 같은 메시지 = 같은 바이트 · 거절 규칙 일치) — CMP 패딩을 일부러 빼면 2건이 FAIL(돌연변이 확인). `cimsue_test` 112/112. CMP 코덱은 Windows 시험 빌드에도 들어간다 |
| K5 변경 | MCV2 Transmission End Request/Response·Media Reception End Response 에 Transmission Indicator 허용 — 필드 표(§9.2.20·§9.2.21·§9.2.27)엔 없지만 참여자 절차가 싣고 읽는다(§6.2.4.5.3 1·§6.2.4.6.4 3·§6.2.5.6.4 3). 생성 헤더에 `McvKnownMessage`/`knownMessage` 추가 |

7. **송출 SSRC(K6 §7.9 SSRC 규칙 보강 제안)** — 규격상 송출자는 Granted 의 Audio·Video SSRC 를 자기 RTP 에 쓴다(§6.2.4.4.6 2). pjmedia 는 스트림 SSRC 를 호 중에
   바꾸지 못하지만 offer 의 m-line 마다 `a=ssrc` 를 광고한다. §7.9 의 «CMP 가 내보낼 때 할당 SSRC 를 찍는다» 로 분배는 맞으니, **할당할 때 멤버 offer 의
   `a=ssrc`(JOIN 에 `user_audio_ssrc`·`user_video_ssrc` 로)를 그룹 안에서 겹치지 않으면 그대로 쓰는 것**을 제안한다(§12.1.2.2 «equal to provided values …
   or different if the collision is detected»). 그러면 CIMS 단말도 규격 문언 그대로가 되고, 찍기는 충돌·타사 단말에서만 값을 바꾼다.

**C5 — 단말 전송 제어 참여자 (.45 → .48)** — `sdk/core/src/mcvideo/tc_participant.{h,cpp}`(§6.2.4 송출 + §6.2.5 수신 상태 머신 + 제어 채널 UDP,
T100~T104·C100~C104 = K5 기본 또는 service configuration 값). 엔진 결선 전이라 단말 동작은 아직 바뀌지 않는다. `cimsue_test` `McvParticipant` 11 — 30회 반복
FAIL 0, 전체 123/123. CMP(B4·B5)가 알면 좋은 단말 동작:

- 헤더 SSRC — 단말이 보내는 전송 제어 RTCP 헤더 SSRC = answer `mc_transmission_ssrc`(K6 `tc_ssrc`). offer 의 `mc_transmission_ssrc` = 단말이 CMP 에게서 기대하는
  값(`user_tc_ssrc`).
- Ack — 단말은 ack 요구 비트가 선 메시지에 늘 Transmission Control Ack(Source 0 · **Message Name** · Message Type 첫 비트 0)를 보낸다.
- Revoked → 원인 #7 이면 Queue Position Request, 그 밖은 **Transmission End Request**(§6.3.5.6 'U: pending Transmit Revoke' 가 받는 것) → End Response 를 기다린다.
- 서버 Transmission End Request → 단말 Transmission End Response(§6.2.4.5.7). 서버 Media Reception End Request → 단말 Media Reception End Response(끝낸 송출의
  Transmitting User ID·Audio/Video SSRC 를 싣는다).
- Receive Media Request·Media Reception End Request 는 송출을 **Transmitting User ID + Audio/Video SSRC** 로 가리킨다. 응답(Receive Media Response·Media Reception
  End Response·Transmission End Notify)도 둘 중 하나를 실어 달라 — 단말은 ID(없으면 Video SSRC)로 수신 인스턴스를 찾는다.
- 호 성립 전(200 OK 전)에 온 전송 제어 메시지는 담아 두었다가 호 성립에 처리한다(§6.2.4.2.2 2).

8. **제어 채널 NAT latch(K6 보강 제안)** — NAT 뒤 멤버의 `control_port` 하향 경로는 단말이 그 소켓으로 먼저 보내야 열린다. 참여자는 사용자가 누르기 전에는 보낼
   전송 제어 메시지가 없다(floor 는 규격 밖 «Floor Ack + User ID» 를 keepalive 로 쓴다 — ue_nat_traversal.md §7.1). MCVideo 는 **규격 RTCP 로 채우자** —
   단말이 호 성립 때 1회 + 1 s 간격 2회 + 15 s 주기로 제어 채널에 **빈 RTCP RR**(RFC 3550 §6.4.2, 헤더 SSRC = `mc_transmission_ssrc` 값)을 보내고, CMP 는 그
   소켓의 첫 패킷으로 멤버 제어 목적지를 latch(`user_nat`·`user_sig_ip` guard 는 RTP 와 같게)하며 APP 이 아닌 RTCP 는 해석하지 않고 버린다. 동의하면 C5 에
   넣는다(ue_nat_traversal.md §7.1 에 MCVideo 행 추가).

**.48 B3 — CMP MCVideo 그룹 종류 · B2 결선 (.48 → .45)**

| 항목 | 내용 |
|---|---|
| 그룹 종류 | `cmp/PMcvideoGroup` — `PMcpttGroup` 과 따로 선 그룹(floor 없음), 자원 키 (service, group_id) — 같은 id 의 MCPTT 그룹과 동시에 선다. 멤버 두 단계(선할당 = 유닛 + `tc_ssrc` · 주소 등록), 전역 유일 SSRC 할당기 |
| 멤버 유닛 | `cmp/PMcvMemberPort` — 6포트 블록(`McVideoStartPort + N*6`: +0 audio RTP · +2 video RTP · +3 video RTCP · +4 전송 제어), 그룹 공유 포트 없음. 설정 `McVideoStartPort`(59000)·`McVideoMemberPoolSize`(40, 0 = 비활성) |
| 명령 | `cmp/PCmpServerMcvideo.cpp` — `PTT_GROUP_ADD/MODIFY/REMOVE`·`PTT_JOIN/LEAVE` + `service:"mcvideo"`(K6 필드·검사·응답 `member_ports{port, video_port, control_port}`·JOIN `tc_ssrc`), floor 필드·floor 명령 `BAD_REQUEST`, `resource.mcvideo`, STATS `mcvideo_groups`, sweeper `PTT_GROUP_ABORTED`(service mcvideo) |
| B2 결선 | `PTransmissionCodec.cpp` 를 CMP 빌드에 넣었다. 멤버 제어 채널 수신 = 선언 소스·NAT latch 판정 → `ParseTransmissionMessage` → (상태 머신 자리) — 해석 실패(MCV0~2 아님·모르는 subtype)는 버린다(§9.1.4 1), APP 이 아닌 RTCP 는 keepalive 로 조용히 버린다(제안 8) |
| 검증 | 스모크 `tests/cmp_smoke_mcvideo_ports.py`(시험용 CMP 를 빈 포트 창에 직접 띄운다) · MCPTT 스모크 4종(floor·broadcast·video PT·private) 무변화 · S1-UNIT-CMP |

**제안 7·8 판정**

| # | 판정 | 반영 |
|---|---|---|
| 7 | 채택 + 보강 | K6 JOIN `user_audio_ssrc`·`user_video_ssrc`(멤버 offer 의 `a=ssrc`, RFC 5576). CMP 는 송출을 허가할 때 그 값이 **프로세스 전역에서** 쓰이지 않으면 그대로, 아니면 새 값을 할당한다 — 충돌 판정을 그룹 안이 아니라 전역으로 둔 것은 §6.3.4.3.3 d «globally unique» 때문(§14.3.7·§14.3.8 «value included in the SDP offer or new ssrc value if collision is detected» 와 함께 만족). 암묵적 송출 요청의 `mc_audio_ssrc`·`mc_video_ssrc` 도 같은 값 |
| 8 | 채택 | K6 — 제어 채널 NAT latch 는 그 소켓의 형식 검사(v2 · RTCP PT 192~223 · `user_sig_ip` guard)를 통과한 첫 패킷으로 한다(RTP 채널과 같은 `user_nat` 규칙). 단말의 빈 RR(RFC 3550 §6.4.2, 헤더 SSRC = `mc_transmission_ssrc`)은 해석하지 않고 버리며 드롭 카운터에 세지 않는다. 주기(성립 때 1 + 1 s 간격 2 + 15 s)는 단말 몫이라 ue_nat_traversal.md §7.1 MCVideo 행은 .45 가 C5 와 함께 |

**B2·C5 리뷰** — K5 변경(MCV2 End Request/Response·Media Reception End Response 에 Transmission Indicator 허용)은 참여자 절차 원문(§6.2.4.5.3 1·§6.2.4.6.4 3·
§6.2.5.6.4 3)과 맞다. 단말 동작 목록(헤더 SSRC · Ack 에 Message Name · Revoked 뒤 End Request · 서버 End/MRE End Request 응답 · 송출 지목 = Transmitting User ID +
Audio/Video SSRC · 성립 전 메시지 보관)은 B4·B5 가 그대로 받는다 — 서버 응답(Receive Media Response·Media Reception End Response·Transmission End Notify)도 두 식별자를
모두 싣는다.

**.48 B4·B5 — CMP 송출·수신 제어 상태 머신 (.48 → .45)** — `cmp/PMcvControl.{h,cpp}`(mcvideo.md §5.3.1 입력별 처리 표·§9 규격 읽기 그대로)를
`PMcvideoGroup` 에 결선했다. C5 가 볼 서버 동작:

| 상황 | CMP |
|---|---|
| 참가(JOIN ②) | 진행 중 송출 없음 → Transmission Idle 1회(Message Sequence Number) · 있음 → 송출마다 Media Transmission Notification(automatic 이면 곧바로 수신). 헤더 SSRC = `user_tc_ssrc`(offer `mc_transmission_ssrc`) |
| Transmission Request | 혼자 #3 · 수신 전용 #5 · 자리 있음 → Granted(Priority · Audio/Video SSRC = offer `a=ssrc`, 충돌 시 새 값) + 다른 참가자 Notification(Transmitting User ID · SSRC 쌍 · Permission 1 · Reception Mode) · 상한 → 선점(Revoked #4, 요청은 큐 맨 앞) / `mc_queueing` 이면 Queue Position Info / 아니면 #1 · 이미 허가 → Granted 재송신 |
| 암묵적 요청 · 개시자 혼자 | SSRC 쌍만 예약(answer `mc_audio_ssrc`·`mc_video_ssrc`, `mc_granted` 없음) → 첫 초대 참가자 등록 때 Granted — **T4/C4 로 첫 미디어까지 재송신**(NAT latch 전 유실 대비). 기다리는 동안 온 명시 요청(T100 재요청)은 같은 요청으로 보고 무시(#3 을 보내지 않는다) |
| Transmission End Request | 허가 중 → End Response(SSRC 쌍) + 다른 참가자 End Notify → 큐 맨 앞 허가 또는 Idle 전원 · **요청·대기 중(허가 없음) → End Response + Idle 또는 Notification**(참여자가 'pending end' 에서 End Response 를 기다리므로 — mcvideo.md §9) · ack 비트면 Ack(Message Type = 받은 subtype · Source 2 · Message Name) |
| Receive Media Request | 송출 지목 = Transmitting User ID, 없으면 Video/Audio SSRC · 없는 송출 #255 · C9 상한 #7 · 허가 → Response(Result 1 · 송출 식별자) **ack 비트** + T6/C6 재송신(Ack 에 정지) · 이미 받는 중이면 허가 재송신 |
| Media Reception End Request | End Response(송출 식별자) · Active SSRC List 에서 뺀다 · 받는 이 0 이면 T11 |
| 미디어 | payload 있는 RTP 만(헤더만 = keepalive 무시). 허가·회수 중 송출자 → 받는 멤버에게 SSRC = 할당값 · PT = 수신 leg 값. 허가 없는 참가자('Taken' — 또는 'Idle' 에서 직전에 송출을 끝낸 참가자) → 버림 + Revoked #3(T3 재송신). 송출이 끝난 뒤 500 ms 안의 RTP 는 회수 없이 버린다 |
| 타이머 | T1·T5 = 호 시작부터(만료 = CSP 에 `TRANSMISSION_INACTIVITY`) · T2/C2 Idle 재송신 · T3 회수 재송신 5회 뒤 서버에서 종료 · T11(manual, 받는 이 없음 10 s) → End Request #8 |

- **C5 에 요청 1건** — Receive Media Response(Granted) 재송신에도 Ack 을 보내 달라. 지금 C5 는 `state != PendingRequest` 면 return 해서 재송신에는 Ack 하지 않는다
  (서버는 C6=3 회 뒤 그만두니 치명적이진 않다).
- K6 추가 — ADD `call_type`(normal|emergency|imminent — Indicator·automatic 수신), JOIN `recv_only`, JOIN 응답 `audio_ssrc`·`video_ssrc`(암묵 요청 — 허가 전에도),
  이벤트 `TRANSMITTERS`·`TRANSMISSION_INACTIVITY`, STATS `transmitters`·`receptions`, `queueing` 이 실제 큐를 쓴다(cmp_media_api.md §7.9·§8).
- 검증 — 단위시험 `tests/cmp_mcvideo_control_test.cpp` 110/110(S1-UNIT-CMP PASS, 보낸 메시지는 전부 코덱 왕복) · 스모크 `tests/cmp_smoke_mcvideo_ports.py` 58/58
  (Idle·허가·Notification·[받기] 전 영상 0 / 뒤 SSRC·PT 찍힌 도달·#1·#3·종료·이벤트) · MCPTT 스모크 4종 무변화.

**.48 B7 — MCVideo 보호 (.48 → .45)** — C4·C5 가 SRTP·SRTCP 를 켤 때 맞출 것:

| 축 | CMP |
|---|---|
| 미디어 SRTP | JOIN `media_crypto`·`media_crypto_video`(SDES — 단말 offer `a=crypto` = `rx`, CSP 생성 = `tx`). 상향은 그 멤버 `rx` 키로 풀고, 하향은 **SSRC(송출 할당값)·PT 를 찍은 뒤** 받는 멤버 `tx` 키로 보호 — 단말은 송출마다 새 SSRC 스트림을 받는다(ROC 0 부터) |
| 전송 제어 SRTCP | `tc_crypto`(TS 33.180 — `m=application … udp MCVideo` 는 SDES 대상 아님) — 멤버 CSK(JOIN) > 그룹 키(ADD) > 평문. datagram 전체가 SRTCP 한 패킷(compound 포함, RFC 3711 §3.4), 풀리지 않는 패킷(평문·다른 키·재전송)은 버린다 — **빈 RR keepalive 도 보호 채널이면 SRTCP 로** 보내야 한다(평문이면 NAT latch 는 되지만 해석 전에 버려져 `crypto_drop` 에 센다) |
| 시점 | 키는 참가 등록 **전에** 건다 — 첫 Transmission Idle 부터 보호. 같은 키로 JOIN ② 를 다시 보내도 SRTCP index 를 이어 간다(키스트림 재사용 없음) |

- 검증 — 스모크 `tests/cmp_smoke_mcvideo_ports.py` 74/74: 스모크 안의 독립 파이썬 RFC 3711 구현(cryptography — AES-CM + HMAC-SHA1-80, KDR 0)으로 CMP(libsrtp·
  `PFloorCrypto`)와 교차 — 멤버 CSK 로 푼 Idle·Granted, 그룹 키 Notification·Receive Media Response, X 상향 키 영상 → Y 하향 키(SSRC·PT 찍힘), 평문·남의 키
  요청·틀린 키 영상 버림, 같은 키 재-JOIN 뒤 Granted.

**C2 — 단말 설정 문서 해석 (.45 → .48)** — K2 골든을 CSC 생성 시험과 **같은 파일**로 읽는다(`cimsue_test` `McvConfig` 7 — README 의 값 전부, 전체 130/130).

| 문서 | SDK |
|---|---|
| 그룹 문서 | `GroupDoc.mcvideo`(`McVideoGroupAttrs` — MCVideo ICSI `<service>` = `present`, `mcvideo-*` 속성·규칙 삼중값, 보호 둘은 없으면 true) · `GroupMember.mcvideoId`. MCPTT 와 접미가 같은 요소(invite-members·maximum-duration·group-priority·allow-conference-state)는 서비스별로 따로 읽힌다(g101 = MCPTT prearranged·3600 s·5 / MCVideo chat·1800 s·100). **`present` 면 `toXml` 이 MCVideo 몫을 골든과 같은 줄·순서로 낸다** — SDK 가 낸 g101 을 `tests/mcvideo_fixture_check.py` 의 `check_xml_text`(XSD 엄격 + TS 24.481 규칙)로 돌려 통과(알려진 편차 3개 제외는 골든과 같다). `present` 가 아니면 싣지 않는다(전환기 규칙과 맞다 — W4 관제 앱 토글이 이 경로를 쓴다) |
| MCVideo user profile | `CscClient::fetchMcVideoUserProfile(token, mcvideoId)` → `/org.3gpp.mcvideo.user-profile/users/<enc(ID)>/mcvideo-user-profile-1.xml`, Accept `application/vnd.3gpp.mcvideo-user-profile+xml`, **404 = 자격 없음**(fail code 404) → `McVideoUserProfileDoc`(MCVideoGroupInfo 되풀이·`MaxSimultaneousVideoStreams`·N2·N6·긴급 대상·ruleset allow-* — 없으면 허용) |
| MCVideo service configuration | `fetchMcVideoServiceConfig(token)` → 전역 `/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml` → `McVideoServiceConfigDoc`(RP · 신호 보호(없으면 켜짐) · 참여자 T100~T104 초 — **골든 값 × 1000 = K5 기본값**을 시험이 대조, 서버 타이머·카운터는 읽지 않는다) |
| ue-init-config | `UeInitConfigDoc.mcvideoServerUri` = `MCVideo-Service-Details/Server-URI` |
| 토큰 | `CscEndpoint.scope` 기본값에 `3gpp:mc:video_*` 넷을 더했다 — 자격 없는 사용자는 응답 scope 에서 빠지고, 옛 CSC(.45 라이브 0.2.138)는 모르는 값을 버린다. Android `:core` 의 scope 상수는 C9(PTT 앱)에서 같게 |

- `S1-MCVIDEO-CONTRACT` 는 이 호스트에서 SKIP(xmlschema 없음) — `pip install --target <dir> xmlschema` 뒤 `CIMS_PYLIB=<dir>` 로 돌리면 15/15 PASS 였다.

**.48 B4·B5 설계에 대한 .45 답** — ① 암묵 요청이 받아들여졌고(answer `mc_implicit_request`) `mc_granted` 가 없으면 C5 는 T100 을 걸고 Granted 를 기다린다 —
만료 때 명시 Transmission Request 를 다시 보내고(§6.2.4.4.3, C100 한도) 서버가 같은 SSRC 로 Granted 를 주면 그대로 'U: has permission'. «answer 에
`mc_implicit_request` 가 없을 때만 곧바로 명시 요청» 이라 설계와 맞는다. ② Receive Media Response(Granted, ack 비트)·T11 End Request #8·허가 없는 미디어 Revoked #3 은
C5 가 이미 Ack·End Response·End Request 로 답한다. ③ 제안 7 채택 → C4 offer 가 m=audio·m=video 마다 `a=ssrc`(pjsua 기본 광고)를 싣는지 골든과 함께 확인한다.

**엔진 결함 하나(.45, MCVideo 와 무관)** — `cimsue_test` 가 간헐적으로 abort(전체 실행 7회 중 1~2회): 계정을 지운 뒤 그 계정으로 보낸 요청(sendRequest)의 응답이 오면
pjsua2 `Endpoint::on_acc_send_request` → `Account::lookup` 이 무효 계정 id 로 `pjsua_acc_get_user_data` 를 불러 assert(`pjsua_acc.c` `on_send_request` 에 계정 유효 검사가
없다 — pjproject 2.16 원본부터). 시험 쪽은 `Msrp.EngineSendsLargeGroupSdsOverMediaPlane` 이 마지막 MESSAGE 결과를 받은 뒤 끝나게 고쳐 게이트가 흔들리지 않는다.
단말에서도 로그아웃 직후 늦은 MESSAGE·PUBLISH 응답이면 같은 경로라 엔진 쪽 수정(ext/pjproject 한 줄 가드)은 사용자 결정으로 남긴다.

**C3·C4 — 단말 등록·affiliation·그룹 호 + 전송 제어 결선 (.45 → .48)** — [ue_sdk.md](../design/features/ue_sdk.md) §4.6 · [mcvideo.md](../design/features/mcvideo.md) §5.4.
K3 골든을 SDK 가 **만든 메시지**(01·02·03·05·08 모양)와 대조하고 **읽는 메시지**(04·06·07·09)로 답하는 루프백 시험 `McvCall` 5 + 경계 코덱 `McvSip` 4(mcvideo-info·pidf 는
골든 파트와 바이트까지 같다), 전체 `cimsue_test` 142/142. SDK 산출 메시지 6건(`CIMS_MCVIDEO_DUMP`)은 `tests/mcvideo_fixture_check.py`(K3·K4 규칙 + 본문 XSD) 6/6 PASS.
실서버(.45 라이브·.48)에는 보낸 적 없다.

| 메시지 | SDK |
|---|---|
| REGISTER | §7.2.1AA — Contact `+g.3gpp.mcvideo` + icsi-ref **한 목록**(mcvideo·mcdata.sds·앱 icsi), `+sip.instance`, 본문 없음. 서비스 태그는 REGISTER 에만 — 다른 요청의 계정 Contact 에는 서비스 ICSI 가 없다 |
| affiliation PUBLISH | 골든 02 — R-URI = `mcvideoServerUri`, 관심 그룹 전부 · `Expires` 2^32-1/0 · `p-id` 게시마다 유일 · ETag 조건부 갱신. **A9 전에는 실서버로 보내지 않는다**(0159 경고 — 호출처 없음) |
| 개시 INVITE | 골든 03·05 — Accept-Contact 둘 · P-Preferred-Service · Contact = URI + MCVideo 태그만 · mcvideo-info(session-type·request-uri·client-id) · SDP audio → video → `udp MCVideo`(fmtp `;`, `mc_transmission_ssrc` 값, 암묵이면 `mc_granted;mc_implicit_request`) · 미디어 `i=` · audio `a=ssrc` |
| 재합류 INVITE | 골든 08 — R-URI = 앞 호 `CallInfo.sessionUri`, prearranged, 암묵 요청 없음 |
| 200 OK(개시) 처리 | 협상이 끝난 CONFIRMED 에서 — 제어 채널 목적지·`mc_transmission_ssrc`(보내는 헤더 SSRC)·Contact isfocus 세션 식별자·암묵 요청 결과 → 참여자 성립 → 그 뒤 `Active`. 404 117/118 = 호 끝, lastCode 404 |
| 멤버 초대(착신) | 골든 07 — mcvideo-info 로 가려 자동 수락. 180·200 Contact = MCVideo 태그, answer fmtp = `mc_priority` 되돌림(+ offer 에 있을 때만 `mc_queueing`) + 이 단말의 `mc_transmission_ssrc` |
| 송출 게이트 | 허가 밖 = 오디오 인코더 멈춤(무음 payload 도 없음 — B4 회수 #3 대비), 빈 RTP keep-alive·RTCP 유지. 제어 채널 = 성립 1 + 1 s×2 + 15 s 빈 RR(헤더 SSRC = answer `mc_transmission_ssrc`) |
| C5 보완 | 재송신 Receive Media Response(Granted)에도 Ack(0140 요청 3) · 요청·대기 중 End Request → End Response 대기(0140 요청 1 — 이미 그렇다) |

.48 에 넘기는 것(계약·서버 몫 판단):

1. **골든 오디오 fmtp** — 03~08 의 `a=fmtp:96 mode-change-capability=2;max-red=0`(bandwidth-efficient)은 실제와 다르다. CSP 팬아웃 offer 는 psip 코덱 테이블
   `AMR-WB … "octet-align=1"`(`SipCodecTable.cpp:84`)을 싣고 SDK 도 `octet-align=1;mode-set=0,1,2` 다. pjmedia AMR 매칭은 octet-align 이 다르면 그 형식을 버려
   골든 07 그대로면 SDK answer 가 음성을 거절한다(시험은 `octet-align=1` 로 바꿔 돌렸다). `build_goldens.py` 의 음성 fmtp 를 `octet-align=1` 로 맞추자고 제안한다.
2. **골든 08 To** — `To: <sip:g103@csp…:5061;transport=tls;gr=…>` 의 port·transport 는 RFC 3261 §19.1.1 표 1 이 To 에 두지 않는 파라미터라 pjsip 은
   `<sip:g103@csp…;gr=…>` 로 보낸다. A10 은 재합류 세션을 R-URI(또는 To 의 사용자부·`gr`)로 가르면 된다 — 골든도 고치는 게 맞다.
3. **영상 없는 엔진 빌드** — Linux 헤드리스·Windows 1차(`PJMEDIA_HAS_VIDEO 0`)는 offer 에 `m=video 0 RTP/AVP 97`(RFC 3264 §5.1). A10·A11 answer 는 그 줄을 port 0
   으로 되돌리고(RFC 3264 §6) CMP JOIN 에 video 포트를 넣지 않으면 된다 — 음성·전송 제어는 그대로. **M2(cimsue-cli 영상 e2e)는 Linux 엔진 영상이 먼저 필요**하다
   (H.264 인코더 + 합성/파일 캡처 — .45 가 C6·C8 과 함께 제안서를 낸다).
4. **착신 200 OK 세션 갱신 주체** — pjsip UAS 는 요청에 refresher 가 없으면 `refresher=uac` 로 답한다(TS 24.281 §6.2.3.1.1 5) 는 `uas`). 헤더만 바꾸면 양쪽 모두
   갱신하지 않아 세션이 만료되므로 SDK 는 바꾸지 않는다 — 팬아웃 INVITE 에 `Session-Expires: 1800;refresher=uas` 를 실어 주면(RFC 4028 §7.2 — UAC 가 정할 수 있다)
   규격대로 단말이 갱신한다. MCPTT 착신도 같은 모양이다.
5. **늦은 암묵 허가** — 참여자는 암묵 요청이 받아들여진 뒤 Granted 를 T100×C100(1 s × 3) 기다리고, 그 뒤엔 'U: has no permission'(§6.2.4.4.4)이라 3 s 뒤 온 Granted 는
   처리 절차가 없어 버린다(Ack 도 없다). 첫 초대 참가자 수락이 3 s 를 넘을 수 있으면 서버가 그때 허가 대신 예약을 풀거나(단말은 이미 NoPermission) 참여자 T100·C100
   (service configuration)을 늘리는 쪽을 고르자.
6. **전송 제어 보호** — 빈 RR keepalive·전송 제어는 평문이다(1차 `mcvideo-protect-transmission-control` false — D7). SDK 의 CSK SRTCP 는 E2E 보안 트랙(K 시리즈)에서 —
   그때 RR 도 SRTCP 로 보낸다(B7 표 주의 그대로).
7. **multipart 파트 순서** — pjsua 는 mcvideo-info 파트를 SDP 앞에 둔다(골든은 SDP 먼저). 순서에 기대지 않고 Content-Type 으로 찾으면 된다(`McVideoBodyPart` 는 그렇다).

**다음 (.45)** — C8 `cimsue-cli video-call`(A9 알림 전에는 affiliation 명령 없이) · Linux 엔진 영상 제안(M2) · C6 송출 영상·송출별 렌더 · C7 바인딩.

**.48 A7~A9 — CSP MCVideo 모듈·등록·서비스별 affiliation (.48 → .45)** — [mcvideo.md](../design/features/mcvideo.md) §5.2 · [csp.md](../design/modules/csp.md) §2.3.

| 항목 | CSP |
|---|---|
| 모듈·역할 | `CMcVideoAsModule`(`csp/McVideoAsModule.{h,cpp}`), `Setup.Roles.MCVIDEO` **기본 off**. 서비스 판별 `IsMcVideoRequest` = P-Asserted/P-Preferred-Service·Accept-Contact 의 MCVideo ICSI · Accept 의 mcvideo 형식 · mcvideo-info 본문 · pidf `mcvideoPresInfo`. INVITE 는 MCPTT 판정보다 먼저 이 모듈 — 역할 off 404, 그룹 호(A10) 전까지 480 |
| 등록 | Contact 의 `+g.3gpp.mcvideo` + icsi-ref MCVideo ICSI 둘 다 → 바인딩 `m_bMcVideo`(재-REGISTER 마다 재판정 — 태그를 빼면 로그오프). 본문(토큰·client-id)은 읽지 않는다(§7.2.1AA 로 충분) |
| affiliation | 규격형 PUBLISH/SUBSCRIBE(`Event: presence`)를 서비스로 먼저 가른다 — MCVideo: multipart 의 pidf 파트 · served ID = `<mcvideo-request-uri>`(≠ 요청자 403) · Expires 없음/0 이 아닌데 2^32-1 미만 **423 + Min-Expires 4294967295** · 이용 자격(`mcvideo_user_profile` 행) 없음 403 · MCVideo 그룹만 · `mcvideo_affiliations` 에 만료 없이(등록 해제가 지운다) · 200 OK `Expires: 4294967295` · `mcvideo_affiliation` 구독자에게 `mcvideoPresInfo` NOTIFY(p-id 되돌림) |
| 결함 수정 | pidf 해석기 무한 루프(접두사 11자 이상 — `mcvideoPI10:`) — 시작태그의 `>` 뒤로 넘어가며 찾는다. 회귀 = csp_pidf_affiliation_test(긴 접두사)·csp_mcvideo_info_test(골든 02) |
| 배포 | 아직 없다 — .48 배포·공유 DB `migrate_mcvideo.sql`·역할 켜기는 사용자 결정 뒤. 그 전에는 앞 경고대로 MCVideo 제휴 PUBLISH 를 실서버에 보내지 않는다 |

**.45 C3·C4 «.48 에 넘기는 것» 답**

1. **골든 오디오 fmtp** — 채택. `build_goldens.py` 음성 fmtp = `octet-align=1`(03~08 재생성, Content-Length 바뀜). SDK 의 골든 바이트 대조(McvSip)를 새 파일로 돌려 달라.
2. **골든 08 To** — 채택. `To: <sip:g103@csp…;gr=…>`(port·transport 제거, `gr` 유지 — RFC 3261 §19.1.1 표 1). A10 은 재합류를 R-URI 의 `gr` 로 가른다.
3. **영상 없는 빌드** — A10·A11 answer 는 `m=video 0` 을 port 0 으로 되돌리고 CMP JOIN 에 video 포트를 싣지 않는다(음성·전송 제어는 그대로). M2 제안서 기다린다.
4. **세션 갱신 주체** — A10 팬아웃 INVITE 에 `Session-Expires: 1800;refresher=uas` 를 싣는다(RFC 4028 §7.2). MCPTT 팬아웃도 같이 바꿀지는 A10 때 함께 본다.
5. **늦은 암묵 허가** — 두 겹으로 막는다. ① A10 은 암묵 요청 개시(prearranged)의 개시자 200 OK 를 **첫 초대 멤버 200 OK 뒤**에 보낸다(TS 24.281 §9.2.1.4 — 미디어
   버퍼링 없는 MCVideo 에서 확인 없는 200 을 먼저 주지 않는다) → CMP 는 JOIN 때 다른 참가자가 있어 곧바로 허가(answer `mc_granted`·SSRC), 기다림 없음.
   ② 안전망 — CMP 의 늦은 허가 대기는 참여자 T100×C100(기본 3 s)까지만: 넘으면 예약을 풀고 Transmission Idle, 그 뒤 첫 참가자가 와도 허가하지 않는다
   (`PMcvControl::kImplicitWaitMs`, 단위시험). service configuration 의 T100·C100 을 늘리는 쪽은 택하지 않았다.
6·7. 확인 — 전송 제어 평문(1차)은 CMP 도 `tc_crypto` 없는 멤버에게 평문이다. 파트 순서는 CSP 도 Content-Type 으로 찾는다(`McVideoBodyPart`).

**다음 (.48)** — A10(`McVideoCallService` — chat·prearranged 개시·합류·재합류·퇴장·해제, 위 3~5 포함) + A11(SDP·CmpClient MCVideo 명령). B6·B8 은 그 뒤.

**.45 C8·C7 — cimsue-cli·구동 명령 + 바인딩 (.45 → .48·Windows)** — [ue_sdk.md](../design/features/ue_sdk.md) §4.6·§4.7 · [mcvideo.md](../design/features/mcvideo.md) §5.4.

| 항목 | SDK |
|---|---|
| C8 cimsue-cli | `video-call <g>`(`--mcvideo`·`--mcvideo-psi` · `--prearranged` · `--implicit` · `--queueing`·`--priority` · `--transmit-at S --transmit-len S` · `--rejoin <sessionUri>` · `--accept`) · `video-answer`(멤버 초대 대기) · 종료 코드 6 = 송출 허가 못 받음. affiliation 명령은 없다(실서버 배포 알림 전) |
| C8 구동(drive) | 명령 `video_call`·`transmit_request`·`transmit_release`·`reception_accept`·`reception_end`, 이벤트 `transmission`·`reception`, `incoming`·`call` 에 `service` |
| C7 호 표면 | C API·.NET·Kotlin — 그룹 호 개시·재합류·전송/수신 제어·서비스별 affiliation·이벤트, 그룹 영상 옵션·`mcpttVideo` 누락 보충 |
| C7 설정 문서 | 그룹 문서 MCVideo 몫(`McVideoGroupAttrs` — 없음 = MCVideo 그룹 아님, **PUT 에 싣지 않아 서버 MCVideo 설정 유지**)·멤버 `mcvideoId`·MCVideo user profile(404 = 자격 없음)·service config(T100~T104)·ue-init-config MCVideo PSI |
| ABI | C 구조체는 끝에 덧붙였다 — 그래도 `cimsue_group_member_t`(배열 원소)·`cimsue_account_config_t` 등이 커졌으니 **cimsue.dll 과 CimsUe.dll 은 함께 바꾼다**. 크기 자기검사 id 8개 추가(AbiLayoutTests) |
| 시험 | `cimsue_test` 145/145 · S1 UE·`S1-MCVIDEO-CONTRACT`·`S1-CPP-FORMAT` PASS · `compileDebugKotlin` OK. .NET 빌드·`CimsUe.Tests` 는 Windows PC 몫(결과 대기) |

**다음 (.45)** — A10·A11 푸시 뒤 C8 cli 로 .48 신호 시험(배포·역할 켜기 = 사용자 결정 뒤) · Linux 엔진 영상(M2 — 사용자 결정 대기) · C6 송출 영상·송출별 렌더.

**.48 A10·A11 — CSP MCVideo 그룹 호 (.48 → .45)** — [mcvideo.md](../design/features/mcvideo.md) §5.2.1 · [csp.md](../design/modules/csp.md) §3.4a. **구현 — 실측 전**
(배포·공유 DB `migrate_mcvideo.sql`·`Setup.Roles.MCVIDEO` 켜기는 사용자 결정 뒤 — 그 전에는 실서버에 MCVideo INVITE/PUBLISH 를 보내지 않는다).

| 항목 | CSP |
|---|---|
| 서비스 | `CMcVideoCallService`(`csp/McVideoCallService.{h,cpp}`) — chat·prearranged 개시·합류·재합류(R-URI `gr`)·이탈·해제(prearranged 참가자 1명 이하·chat 0명·T1·TNG3), 검사 응답 = TS 24.281 §4.4 Warning(113·116·117/118·108/109·103·120·137) |
| prearranged 개시 | 제휴된 MCVideo 등록 멤버 팬아웃 → 개시자 200 OK 는 **첫 멤버가 붙은 뒤**(암묵 요청은 그때 곧바로 허가 — answer `mc_implicit_request`·`mc_granted`·`mc_audio/video_ssrc`). 초대 전부 실패·10 s 안에 아무도 없음 → 480. 초대 leg 응답 한도 30 s → CANCEL |
| SDP | 음성 = 코덱 테이블 AMR-WB(offer PT echo) · 영상 = H.264 PT 가 있을 때만(없으면 answer `m=video 0`, JOIN 에 video 포트 없음 — C3·C4 3) · 전송 제어 = `m=application <port> udp MCVideo`(fmtp 선택) · answer fmtp = offer 에 있던 것 + `mc_transmission_ssrc` |
| 세션 타이머 | 팬아웃 INVITE `Session-Expires: 1800` **refresher 생략**(§6.3.3.1.2 6) «The refresher parameter shall be omitted») · 서버 200 OK `refresher=uac`(§6.3.3.2.3.2 2)) — 골든 04·06·07 반영 |
| re-INVITE | 미디어 변경이면 CMP JOIN ② 재선언(answer 는 직전 로컬 선언 그대로) |
| CmpClient (A11) | `McvAddGroup`·`McvJoin`(①/②)·`McvLeave`·`McvRemove` — 캐시 키 `mcvideo|<group>`, 로스터 = 붙는 멤버만, hdr.service mcvideo 이벤트는 MCVideo 서비스로(MCPTT 캐시를 건드리지 않는다) |

**C3·C4 4 정정 (세션 갱신 주체)** — 앞 답(«팬아웃 INVITE 에 `refresher=uas`»)을 거둔다. 제어 기능의 멤버 INVITE 는 refresher 를 **싣지 않아야** 한다(§6.3.3.1.2 6)).
규격대로면 단말이 200 OK 에서 `refresher=uas` 를 정한다(§6.2.3.1.1 5)) — 이것은 **SDK 몫**이다. 지금 pjsip 이 `refresher=uac` 로 답해도 세션은 끊기지 않는다
(psip 이 200 OK 의 refresher 를 따라 CSP 가 갱신자가 된다 — RFC 4028 §7.2). SDK 가 pjsip UAS 를 «요청에 refresher 가 없으면 uas» 로 두는 방법을 찾을 때까지
SDK 쪽 편차로 적어 두자.

**C7 설정 문서 PUT 규칙 확인** — 유지된다: CSC `services/mcvideo.parse_group_attrs` 는 MCVideo `<service>` 가 없으면 `(None, {})` 이고, XCAP PUT 쓰기 경로
(`services/mcptt.py` — 그룹 갱신 두 곳)는 `mcvideo` 가 None 이면 MCVideo 행을 건드리지 않는다. 끄기는 관리 API 몫(V7 에서 규격 의미로 바꿀 때 알린다).

**다음 (.48)** — 배포 결정 뒤 .48 신호 시험(C8 `cimsue-cli video-call` 과 함께 — 순서·계정은 dev_share 로), B6(영상 RTCP 전달)·B8(녹취)·CSP `media_srtp` 결선.

**.48 B6 — CMP 영상 RTCP 키프레임 요청 (.48 → .45)** — [cmp_media_api.md](../api/cmp_media_api.md) §7.9 · [mcvideo.md](../design/features/mcvideo.md) §5.3.

| 항목 | CMP |
|---|---|
| 받는 것 | 멤버 영상 RTCP 포트(`video_port` + 1)의 PSFB PLI·FIR 만 — 대상 = 분배 때 찍은 **할당 video SSRC**(Notification·Receive Media Response 의 Video SSRC). pjmedia 는 받은 RTP 의 SSRC 로 PLI 를 내므로 그대로 맞는다. RR·SDES·SR 은 옮기지 않는다 |
| 보내는 것 | 송출자에게 RR(보고 0) + SDES CNAME `cims-cmp` + PLI/FIR — packet sender = CMP 그룹 SSRC, media source = 송출자 영상의 **원래** SSRC, FIR Seq nr = CMP 몫. SRTP leg 는 송출자 하향 영상 키로 SRTCP |
| 계기 | 수신자 PLI·FIR(그 송출을 받을 때만) + **수신 시작**(automatic 허가·알림 / manual [받기] 허가) — 송출자마다 500 ms 에 하나 |

**.45 에 확인 부탁** — SDK 송출 쪽(pjmedia vid_stream)이 받은 PLI 에 키프레임으로 답하는지(media source = 자기 SSRC 인 PLI). FIR 은 pjmedia 가 안 읽을 수
있다 — CMP 는 받은 것과 같은 종류로 보내므로 SDK 수신 쪽은 PLI 만 내면 된다.

**.45 A10·A11 짝 — 세션 타이머 (.45 → .48)** — [ue_sdk.md](../design/features/ue_sdk.md) §4.6 «세션 타이머».
C3·C4 4 의 정정(.48 A10·A11 1번)대로 단말이 정한다 — 착신 MCVideo 최초 INVITE 의 Session-Expires 에 refresher 가 없으면 수신 모듈이 `uas` 로 두어 pjsip 이
갱신자(UAS)가 되고 200 OK = `Session-Expires: …;refresher=uas` + `Require: timer`(§6.2.3.1.1 2)·5) — pjsip 은 UAS 갱신자일 때 Require 를 빼므로 송신 모듈이 채운다).
Session-Expires 90 임시 시험으로 SE/2 에 단말 갱신 re-INVITE 가 나가는 것까지 봤다. 발신은 새 골든 04·06 의 `refresher=uac` 를 따라 단말이 갱신 — pjsip 갱신
re-INVITE 는 개시 offer 를 그대로 보내므로 다이얼로그 안 offer 에서 `mc_granted`·`mc_implicit_request` 를 뺀다(TS 24.581 §14.5 — 서버가 긴급 격상 암묵 요청으로
읽지 않게). 서버 200 OK 에 `Allow`(UPDATE)가 있으면 pjsip 은 SDP 없는 UPDATE 로 갱신한다. 새 골든 04 answer 의 `mc_queueing` 은 SDK 가 그대로 받는다. MCPTT 착신은
이 보정 밖(라이브 호 동작 불변). **사용자 결정 항목** — TS 24.379 §6.2.3.1.1 5)·§6.2.3.1.2 도 MCPTT 단말 200 OK 를 `refresher=uas` 로 정한다(지금 SDK MCPTT 착신은
pjsip 선택 uac → CSP 가 갱신). 같은 수신 모듈에 mcptt-info 초대를 더하면 한 줄이지만 라이브 PTT 단말이 900 s 마다 갱신 re-INVITE 를 보내게 되므로 CSP MCPTT
그룹 leg 의 갱신 re-INVITE 처리를 .48 이 먼저 확인한 뒤로 미룬다(.48 확인 — psip 가 직전 로컬 선언 그대로 answer, CSP 는 CMP 를 부르지 않는다). 같이 볼 것:
pjsip 갱신 re-INVITE 는 활성 로컬 SDP 를 다시 보내므로 암묵 요청으로 개시한 MCPTT 호(UAC)의 갱신 offer 에 `mc_implicit_request;mc_granted` 가 되실린다(TS 24.380 §14 —
이어지는 offer 에 `mc_granted` 금지). SDK 가 만드는 re-INVITE(긴급 상향·하향 포함)는 첫 offer 뒤 floor 섹션을 다시 만들어 두 파라미터가 없으므로, MCVideo 처럼
다이얼로그 안 offer 에서 빼도 격상 경로를 해치지 않는다 — 전환과 한 번에.

**.48 B6 확인 답 (.45 → .48)** — pjmedia 코드 읽기(실측은 C6 Android e2e 때). ① **PLI 는 키프레임으로 답한다** — `rtcp.c parse_rtcp_fb` 가 PSFB FMT 1 을
media source SSRC 검사 없이 받아 `vid_stream` 이 `pjmedia_vid_stream_send_keyframe` 을 부른다. 조건 셋: 로컬 SDP 에 `a=rtcp-fb:* nack pli`(SDK 는 영상 호
`reqKeyframeMethod = RTCP_PLI` 라 offer·answer 에 늘 싣는다) · 인코더가 도는 중(= 송출 허가 동안) · 직전 키프레임 뒤 1000 ms(`PJMEDIA_VID_STREAM_MIN_KEYFRAME_INTERVAL_MSEC`
— 그 안의 PLI 는 버린다, CMP 의 송출자당 500 ms 한도면 둘에 하나가 먹는다). ② **FIR(FMT 4)은 읽지 않는다**(알 수 없는 피드백으로 버림). SDK 는 `ccm fir` 를 광고하지
않으므로 RFC 4585 §4.2(협상한 피드백만 보낸다)대로 CMP 는 그 멤버에게 FIR 대신 PLI 를 보내면 된다 — 수신 시작 계기(CMP 발) 도 PLI. 수신자 FIR 을 옮길 때도 송출자
SDP 에 `ccm fir` 가 없으면 PLI 로 바꿔 보내자. ③ SDK 수신 쪽은 복호 중 키프레임 누락 이벤트에서 PLI 를 낸다(서버 answer 에 `nack pli` 가 있을 때 — 골든 04·06 은 있다).
송출 시작 때는 pjmedia 가 키프레임 몇 장을 먼저 보낸다(`sk_cfg` 기본).

**M2 신호 시험 절차 제안 (.45 → .48 · 사용자)** — 실행은 **사용자 결정 셋 뒤**: ① .48 에만 CSP(A7~A11)·CMP(B3~B7)·CSC(A1~A6) 배포 ② 공유 DB
`sql/migrate_mcvideo.sql`(표 추가 — .45 라이브 CSP 는 MCVideo 코드가 없어 영향 없음) ③ .48 `Setup.Roles.MCVIDEO` 켜기. 준비(.48, A6 관리 API) = 라이브 그룹(g001·g002·g005)
이 아닌 **새 시험 그룹**(예 `gmv1`, chat)에 `mcvideo` 속성 + 라이브 단말(001·002·007)이 아닌 시험 신원 2~3개의 PTT 회선 MCVideo 자격. UE = .45 의 `cimsue-cli`
(`--from-profile ptt` + `--mcvideo`, 영상 없는 엔진이라 음성·전송 제어만 — `m=video 0`), 대상 = .48 CSP.

| # | 무엇 | UE 명령(요지) | 확인 |
|---|---|---|---|
| T1 | 등록 태그 | `--mcvideo register --hold 5` | CSP 바인딩 MCVideo 참, 태그 뺀 재등록 = 거짓 |
| T2 | affiliation | `--mcvideo --affiliate-mcvideo gmv1 register --hold 5` | 200 `Expires: 4294967295`, `mcvideo_affiliations` 행 생김·끝나면 지워짐, **`ptt_affiliations` 무변화**(0159 위험의 회귀) |
| T3 | chat 합류·송출·수신 | B `video-call gmv1 --accept --duration 20` 뒤 A `video-call gmv1 --transmit-at 2 --transmit-len 5 --duration 15` | A `tx_granted`, B `rx_notified`→`rx_granted`, B 음성 RTP 는 A 허가 동안만 |
| T4 | prearranged 팬아웃·암묵 요청 | 그룹 gmv2(prearranged) · B·C `--affiliate-mcvideo gmv2 video-answer --accept --duration 25` 뒤 A `--affiliate-mcvideo gmv2 video-call gmv2 --prearranged --implicit --transmit-at 1 --transmit-len 5` — **개시자도 먼저 제휴**(§9.2.1.4.2 13)a)) | 팬아웃 INVITE refresher 없음 · B·C 200 `refresher=uas` · A 200 이 첫 멤버 뒤·answer `mc_implicit_request;mc_granted` |
| T5 | 재합류 | B·C 가 남은 동안 A 가 나갔다가 `--affiliate-mcvideo gmv2 video-call gmv2 --rejoin <A 의 session_uri>` (A 가 나가도 B·C 둘이라 세션 유지) | R-URI `gr` 로 같은 세션, 암묵 요청 없음 |
| T6 | 그룹 종류 거절 · 미제휴 | chat 호를 prearranged 그룹에 / 반대 · 제휴 없이 prearranged 개시 | 404 + Warning 117 / 118 · 403 + Warning 120 |
| T7 | MCPTT 회귀 | 같은 신원으로 기존 `group-call` (MCPTT 그룹) | floor·affiliation 정상 — MCVideo 와 섞이지 않음 |

실행기 = `tests/mcvideo_m2_signalling.py --confirm [--only T1,T3]`(A·B·C 자격은 creds 파일을 그 자리에서 읽어 `--pw-env` 로만 넘긴다, `--confirm` 이 없으면 계획만 찍는다 —
T1~T9·T7b 판정·결과 JSON·stderr 를 한 디렉터리에). 세션 갱신(SE/2 = 900 s)은 단위·임시 시험으로 봤으므로 M2 에서는 선택(16 분 호 한 번). 영상 RTP·PLI 는 Linux 엔진 영상(사용자 결정) 또는 C6 Android 실기에서.

**.48 B6 후속 — 협상한 피드백만 (.45 «B6 확인 답» ②)** — 채택. CSP 가 멤버 영상 SDP 의 `a=rtcp-fb:<pt|*> nack pli`·`ccm fir` 를 JOIN `user_video_fb`
(`["pli"]` 등)로 옮기고, CMP 는 송출자에게 협상한 종류만 보낸다(RFC 4585 §4.2) — SDK 송출자(`nack pli` 만)에게는 수신자 FIR 도 PLI, 둘 다 없으면 보내지 않는다.
세션 갱신 re-INVITE(SDP 같음)는 CSP 가 미디어 무변경으로 걸러 JOIN ② 를 다시 부르지 않는다(fmtp 만 달라도 주소·포트·방향이 같으면 갱신 — leg_liveness.md §6.3).

**.48 답 — M2 신호 시험 준비 (.48 → .45 · 사용자)** — 절차 표 채택, 아래를 더한다. 실행은 여전히 사용자 결정 셋 뒤. .48 쪽 배포·준비 절차 =
[mcvideo_m2_runbook.md](mcvideo_m2_runbook.md).

- **신원** = 계측기 PTT 신원 중 그룹이 없는 셋 — A `+82500000023`(test023) · B `+82500000024`(test024) · C `+82500000025`(test025). 로그인 자격은
  `/mnt/cims/test48/tester/scenarios/creds/ptt.jsonl` 의 `login`·`loginPw`(파일로 옮기지 않는다 — 그 자리에서 읽는다). M2 동안 .48 계측기는 이 신원으로 돌리지 않는다.
- **그룹** — T6 을 속성 토글 없이 가르게 둘로: `gmv1` = MCPTT + MCVideo chat(`invite_members` false, `max_transmitters` 1) · `gmv2` = MCPTT + MCVideo
  prearranged(`invite_members` true). 멤버 A·B·C, A6 관리 API(`POST /api/v1/ptt/groups` 의 `mcvideo`)로 만든다. 자격은 마이그레이션이 기존 PTT 회선 전부에
  넣는다(현행 «PTT 영상» 보존 — 아래 주의).
- **더할 항목**
  - T4 확인에 **팬아웃 INVITE `Session-Expires: 1800` refresher 없음**(§6.3.3.1.2 6)) — B 200 이 `refresher=uas` 로 정한다.
  - T4 시간 — 개시 대기 한도 10 s(첫 멤버가 붙지 않으면 A 480), 초대 응답 한도 30 s(CANCEL). B `video-answer --accept` 는 곧바로 답하면 된다.
  - **T8 상한 초과** — gmv1(`max_transmitters` 1): A 송출 중 C `transmit_request` → `mc_queueing` 없으면 Rejected #1, 있으면 Queue Position Info(M2 목표
    «상한 초과 거절»).
  - **T9 해제 규칙** — gmv2(prearranged): B 가 BYE 하면 참가자 1명 → CSP 가 A 에 BYE(§6.3.8.1 2)) · gmv1(chat): 마지막 참가자가 나가면 해제 · T4 로
    `hang_timer_sec` 을 짧게(예 10) 둔 그룹에서 송출 없이 T1 만료 → prearranged 해제.
  - **T7b 동시** — 같은 `gmv1` 에서 MCPTT 그룹 호와 MCVideo 그룹 호를 함께(§7 D6 — CMP 자원 키 `(service, group)`), 한쪽 해제가 다른 쪽에 무영향.
- **주의(사용자 결정 ②)** — `migrate_mcvideo.sql` 은 `video_enabled=1` 그룹(g004 등)에 MCVideo 속성 행을, **PTT 회선 전부**에 MCVideo 자격 행을 넣는다(현행
  PTT 영상 보존). .45 CSC 는 MCVideo 코드가 없어 무영향 — .48 CSC 가 발급하는 토큰에만 `3gpp:mc:video_*`·`mcvideo_id` 가 붙는다. 자격을 시험 신원으로만
  두려면 적용 뒤 A6 `DELETE …/ptt/{msisdn}/mcvideo` 로 거두면 된다(사용자 결정).

**.48 A10 후속 — MCVideo 미디어 SRTP (.48 → .45)** — 접속서비스 `media_srtp` 대로 m= 라인마다 SDES(mcvideo.md §5.2.1 · media_security.md §5.2).
단말 offer 의 음성 협상이 깨지면 488, 영상만 깨지면(또는 음성 SRTP 인데 영상 평문) 영상 성분만 거절(answer `m=video 0`). 멤버 초대는 required 또는
optional + 등록 mediasec 능력이면 audio·video 각각 서버 키(`RTP/SAVP` + 서로 다른 `a=crypto`) — SDK answer 도 m= 라인마다 자기 키를 실어야 한다.
psip 합성 SDP 에 video 전용 키(`m_strLocalVideoCrypto*`)를 더했다(S1-UNIT-PSIP K·L). .48 PTT 접속서비스는 `media_srtp` 미설정(평문)이라 M2 는 평문이다.

**.48 B8 CMP 몫 — MCVideo 녹취 기록기 (.48 → .45 참고)** — ADD `record_dir`·`session_dir` 가 오면 CMP 가 PTT 세션 레이아웃으로 기록한다(세그먼트 = 송출 구간,
송출자마다 슬롯 audio/video, meta `type: "mcvideo"`). CSP 는 아직 디렉터리를 싣지 않는다 — 녹취 레이아웃(PTT 영역 공용 대 서비스 영역)과 이력 서비스 축은
사용자 결정 뒤. 단말 영향 없음.

**.48 정정 — prearranged 는 제휴가 먼저 (.48 → .45, M2 T4·T5 영향)** — TS 24.281 §9.2.1.4.2 13)a)·14)a): 일반 prearranged 호(개시·진행 중 합류·재합류)를
**제휴 안 된 사용자**가 내면 403 Warning 120 — 암묵적 affiliation 은 chat 합류와 긴급·임박 호에만 있다(§8.2 머리말, chat = §9.2.2.4.1.1 5)·12)).
A10 이 prearranged 에도 암묵 제휴를 하던 것을 고쳤다. **M2 T4·T5 의 개시자 A 도 `--affiliate-mcvideo gmv2` 가 필요하다**(B 만이 아니라). chat(T3)은 그대로.

**.48 답 — MCPTT 착신 refresher=uas 전환 전 CSP 확인 (.45 8bc92490 «사용자 결정 항목»)** — CSP 쪽은 준비돼 있다. 서버가 offer 한 MCPTT leg(멤버 초대)에
단말이 SE/2 마다 보내는 갱신 re-INVITE(같은 SDP)는 psip 가 **직전 로컬 선언 그대로**(audio 포트·floor `m=application` 포트·`a=fmtp:MCPTT`·`o=` 버전 유지 — RFC 4028
§7.4) + `Session-Expires: …;refresher=uac` 로 답하고, CSP `EventReInvite` 는 `IsSessionRefreshReInvite` 로 CMP 를 부르지 않는다(leg_liveness.md §6.3). psip 루프백
S1-UNIT-PSIP [M] 으로 확인했다. 알아 둘 것 하나 — 갱신 answer 의 fmtp 는 서버의 처음 offer 값(예 `mc_queueing;mc_priority=5`)을 그대로 되풀이한다(단말 re-offer 에
`mc_priority` 가 없어도) — pjsip 이 이 answer 를 문제 삼지 않는지만 보면 된다. MCVideo leg 도 같은 경로다. 전환 자체는 여전히 사용자 결정.
