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

## 7. PTT 그룹 편집 — 관제 앱 폼 (Windows 몫)

SDK 반영은 끝났고(`GroupDoc` 이 TS 24.481 요소 다섯을 더 싣는다), **폼만 남았다**. 정본 [../design/features/dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §4.7.
서버는 XCAP PUT 으로 이미 받는다(바뀐 것 없음). 태블릿은 Windows 반영 뒤 같은 방식으로 옮긴다.

| # | 할 일 | 대상 |
|---|---|---|
| G1 | 편집 폼에 다섯 필드 — 유지 시간 T4(`HangTimerSec`) · 최대 통화 시간(`MaxDurationSec`) · 참가자 정보 구독(`AllowConferenceState`) · 메시지 최대 크기(`MaxSdsSize`) · 자동수신 최대(`MaxAutoRecv`). GET 값으로 채우고 그대로 되돌려 보낸다 | `GroupEditViewModel`·`GroupEditView.xaml` |
| G2 | **멤버 우선순위 보존** — 저장 때 `Priority = m.IsChair ? 7 : 5` 로 고정해 콘솔이 준 멤버별 우선순위를 초기화한다(XCAP `<list>` = 멤버 전체 교체). 멤버 행이 GET 의 `GroupMember.Priority` 를 들고 있다가 보낸다 | `GroupEditViewModel` 저장 경로 |

- **다섯 필드는 `null` = 미기재**다(`int?`·`bool?`). 폼이 아직 다루지 않는 지금도 PUT 에 싣지 않으므로 서버 값을 덮지 않는다 — G1 전이라도 안전하다.
- 범위·기본값은 콘솔과 같다(T4 0~3600초 기본 30 · 최대 시간 0~86400초 기본 3600 · 참가자 정보 구독 기본 허용 · 크기 0 = 무제한).
- **동시 발언은 넣지 않는다** — 관리 API 전용(`docs/api/mcptt_api.md` §2).
- **.NET 은 Windows 에서 빌드해 확인**할 것 — C API `cimsue_group_doc_t` 끝에 `has_*`/값 10개를 덧붙였고(`NativeStructs.cs` 같은 순서), `AbiLayoutTests` 가 크기를 대조한다. 새 시험 `CscTests.GroupDocCallTimersAreOptional`.

## 8. MCPTT 암묵적 발언 요청 · 애드혹 일제 통화 — 서버 과제 (.48 몫)

.48 에서 반영·배포(csp 0.2.165)·실측했다 — 정본 [mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) R13·R14 판정 ✅.
.45 스택에 올릴 때는 csp 0.2.165 이상. 단말 동작은 바뀌지 않는다(SDK 는 두 속성을 함께 싣는다).

## 9. 단말 SDK P0b 실측에서 드러난 서버 과제 (.48 몫)

.45 에서 SDK 코어(긴급·경보·MSRP)를 실서버로 시험하며 본 것이다([sdk_port_handoff.md](sdk_port_handoff.md) §4).
M1~M5 는 .48 에서 반영·배포(**csp 0.2.166 · csc 0.2.133**)·실측했다. .45 스택에 올릴 때는 이 버전 이상.

| # | 판정 | 반영 | .48 실측 |
|---|---|---|---|
| M1 MSRP 배포 유실(발신 leg 가 통지보다 먼저 끝남) | ✅ | 끝난 수신 leg 의 cmdp 세션을 30 s 기억해 통지를 한 번 받는다 — 배포는 통지 payload 만으로 한다([mcdata_messaging.md](../design/features/mcdata_messaging.md) §4.7) | 013 → g005 2403 B media plane → 014 `plane=media` 수신, 계측기 `MCDATA-SDS-GROUP-MEDIA` ×5 pass. 경합 창(서브 ms) 자체는 재현하지 못했다 |
| M2 경보 Request-URI = 참여 기능 PSI | ✅ | 대상 그룹 = 본문 `mcptt-request-uri`, Request-URI 그룹은 전환기로 받는다. 경보는 `CPttAsModule::OnEmergencyAlert` | `cimsue-cli --mcptt-psi sip:mcptt_psi@ptt.cims.example.kr alert g005` 200 → CSP `R-URI(mcptt_psi) fanout=3` → 014 수신 · 그룹 R-URI(전환기) 200 |
| M3 경보 팬아웃 본문 재작성 | ✅ | §6.3.3.1.11·§6.3.3.1.12·§12.1.3.2 — request-uri = 수신자, calling-user-id·calling-group-id, 취소의 originated-by·emergency-ind false, Accept-Contact·P-Asserted-Service-Id, 위치 파트 | 014 수신 `group=g005 user=+82500000013` · 015 제3자 취소 `originated_by=+82500000013 emergency=-1` |
| M4 ad hoc 인가 규격 요소 | ✅ | `<cp:actions><anyExt><allow-adhoc-group-call>`(TS 24.484 **Rel-18** §8.3.2.1 11)xxxviii)R)) + 전환기 별칭 `<cims:allow-adhoc-group-call>`(옛 ptt-client — P3 이식 뒤 뺀다) | `tests/test_csc_user_profile.py` 12 OK |
| M5 service-config Resource-Priority | ✅ | service-config 문서 **전체를 TS 24.484 §8.4 스키마로 재구성** — `<service-configuration-info>` › `<service-configuration-params domain>` › `<common><broadcast-group>`(계층 수) · `<on-network>`(`<fc-timers-counters>` 17 요소 = CMP floor 기본값 · `<emergency-/imminent-peril-/normal-resource-priority>` = `mcpttp` 15/8/0). 값 = DB 행(N2·계층 수) + CSC 설정 `ServiceConfig.*`(타이머 ms·RP). **시스템 인가 스위치 5종 제거**(1:1·긴급·경보·발언 요청·그룹 생성 — §8.4 에 없는 요소, 인가 = user profile ruleset·그룹 문서) | `tests/test_csc_user_profile.py` 14 OK(스키마 순서·필수 요소·기본값·덮어쓰기) · SDK `CmsDoc.*` |

P3(ptt-client SDK 전환) 실측에서 더 드러난 것 — **미반영**([sdk_port_handoff.md](sdk_port_handoff.md) §5):

| # | 영향 | 위치 | 문제 | 규격 | 방향 |
|---|---|---|---|---|---|
| M6 | 중 | CSP 그룹콜 착신 INVITE(멤버 leg) | 멤버 leg INVITE 에 그룹 멤버 전원의 `application/resource-lists+xml`(`mcpttgi:participant-type`·`user-priority`)을 싣는다 — 12인 그룹에서 4.5 KB 가 UDP 로 나간다. 멤버 수에 비례해 커져 UDP 조각화·단말 수신 버퍼(pjsip 기본 4000 B — 단말은 65535 로 올렸다)를 넘는다 | TS 24.379 §6.3.3.1.2(제어 기능이 멤버에게 보내는 INVITE — mcptt-info 복사, resource-lists 없음) · RFC 3261 §18.1.1 | 멤버 leg INVITE 에서 resource-lists 제거(명단은 conference 이벤트로) |
| M7 | 하 | 같은 INVITE | Contact 가 `<sip:gNNN@…>;isfocus` 뿐이다(`g.3gpp.mcptt`·`g.3gpp.icsi-ref` 없음) · `Session-Expires` 에 `refresher=uac` 를 붙인다 | TS 24.379 §6.3.3.1.2 1)·6)(refresher 생략) | Contact 특성 태그 셋 · refresher 생략 |
| M8 | 하 | CSP in-dialog INFO | VoLTE 영상 호의 키프레임 요청 INFO(pjsua `media_control+xml`)에 501 — 상대에게 전달되지 않는다. 키프레임 요청은 RTCP PLI 로만 간다 | TS 26.114 §7.3(영상 코덱 제어 = RTCP AVPF PLI/FIR) · RFC 6086 | 대화 안 INFO 를 상대 leg 로 중계(또는 단말이 RTCP 만 쓰도록 계정 설정 — .45 결정) |

- 참고(관찰): 두 단말이 같은 사내 NAT 뒤에서 영상 통화할 때 CMP 가 한 peer 자리의 RTCP 목적지를 두 포트 사이에서 몇 초마다 다시 latch 하고, 다른 peer 의
  영상 RTCP 를 "unnegotiated src" 로 한 번 버린다(`PRtpRelay`). 영상·음성 품질에는 영향이 보이지 않았다(손실 0.3 %·RTT 17 ms) — 같은 공인 IP 뒤 두 peer 의
  latch 판정 확인 필요.

- **M5 에 딸린 변경** — SDK `ServiceConfigDoc`(§8.4 해석: domain·계층 수·`rpEmergency/rpImminentPeril/rpNormal` r-value) ·
  `UserProfileDoc.allowPrivateCall` · `Capabilities`(인가 = user profile 만, `transmitRequest` 제거, N2 = user profile) · Kotlin 파사드 같은 구조
  (SWIG 재생성·빌드는 .45) · 옛 ptt-client(인가 = user profile, 발언 요청 게이트 제거, N2 = user-profile `MaxAffiliationsN2`, RP 해석 —
  빌드 확인은 .45) · 콘솔 **구성 > MCPTT 정책** = N2·계층 수만 · 관리 API `GET/PUT /api/v1/mcptt/service-config` 도 그 셋만.
- **DB 마이그레이션(보류)** — `sql/migrate_service_config_drop_switches.sql`(스위치 컬럼 5개 DROP). 같은 DB 를 쓰는 옛 CSC 는 그 컬럼을
  SELECT 하므로 **.45·.135 CSC 가 0.2.133 이상이 된 뒤** 적용한다. 새 CSC 는 적용 전에도 정상이다.
- **남은 것(M5)** — floor 타이머의 단일 정의: 지금은 CSC `ServiceConfig.FcTimersCounters.*` 와 CMP `Floor*Sec` 를 같게 둔다.
  CSP 가 문서 값을 `PTT_JOIN.floor_timers` 로 CMP 에 넘기면 정본 하나가 된다(CMP 는 이미 받는다).
- **mcptt-info 인코딩(Annex F.1 contentType)** — `mcptt-request-uri`·`alert-ind` 등은 자식 `<mcpttURI>`/`<mcpttBoolean>` 에 값을 싣는 것이 규격인데
  CSP·SDK·ptt-client 모두 값을 요소에 바로 적는다. CSP 수신은 이제 두 형식을 다 읽는다(`McpttElemValue`, `tests/csp_mcptt_info_test.cpp`).
  **SDK `localText` 는 자식 형식을 못 읽는다** — SDK·앱 수신을 먼저 두 형식으로, 그다음 송신 전환(CSP 는 `PttAsModule.cpp` `_InfoElem` 한 곳).
- **SDK 쪽 반영(.48)** — `AccountConfig.mcpttServerUri`(경보 Request-URI = PSI, 비면 그룹 URI) · `cimsue-cli --mcptt-psi`. 앱은 ue-init-config
  `MCPTT-Service-Details/Server-URI` 를 넣는다. Kotlin 파사드·C API·.NET 노출은 .45·Windows 몫(sdk_port_handoff §4.1).
- **옛 ptt-client(.45 빌드 확인 필요)** — ad hoc 인가를 규격 요소 먼저 읽고, 경보 그룹은 `mcptt-calling-group-id` 먼저(`PttController.kt`·`McpttXml.kt`).
- **설정(.48 → 공유 DB)** — g005 그룹 능력 `emergency_call` 켬 · 계측기 신원 013 의 user profile 긴급 대상 = `DedicatedGroup g005`.
  `cimsue-cli group-call g005 --upgrade-at 3 --cancel-at 7` → 상향 **Confirmed 200** · 하향 Confirmed 200. .45 CSP 는 같은 DB 라도 캐시가 통지로만
  갱신되므로 .45 에서 시험하기 전에 CSP 재적재가 필요할 수 있다.
- **남은 경보 편차** — `<mc-org>`(§6.3.3.1.12, 값 정본 = CSC 설정)·수신 확인 `<alert-ind-rcvd>`(§6.3.3.1.20)·미인가 취소 403(§12.1.3.2 1)) —
  [mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) §4.3 편차 표.
- **보안 관찰** — CSP `CmdpClient` 는 이벤트 datagram 의 출처(cmdp 주소)를 검사하지 않는다. 별도 과제.
