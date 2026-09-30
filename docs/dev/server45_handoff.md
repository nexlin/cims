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
  - **.45 몫** — ① SDK(C++ 코어) 재빌드면 INFO 가 200 이 된다 — 옛 SDK 는 500 을 답하지만 호에는 영향이 없다. C API·.NET·Kotlin 노출(`onNonAcknowledgedUsers`·
    `answerState`)과 앱 표시는 Windows·Android 몫. ② CMP 0.2.104 이상이면 개시 200 이 멤버 확인 전에 나가고 `P-Answer-State: Unconfirmed` 가 붙는다 —
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
- **남은 경보 편차** — `<mc-org>`(§6.3.3.1.12, 값 정본 = CSC 설정)·수신 확인 `<alert-ind-rcvd>`(§6.3.3.1.20)·미인가 취소 403(§12.1.3.2 1)) —
  [mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) §4.3 편차 표.
- **보안 관찰** — CSP `CmdpClient` 는 이벤트 datagram 의 출처(cmdp 주소)를 검사하지 않는다. 별도 과제.
