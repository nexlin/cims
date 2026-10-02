# MCPTT 타이머 — 소유 · 값의 출처 · 설정 · 전달

> MCPTT 의 타이머는 **누가 돌리는가**(호 제어 서버 / 발언권 제어 서버 / 단말)와 **값이 어느 문서에서 오는가**(그룹 문서 /
> service configuration / UE initial configuration)로 갈린다. 이 문서는 그 두 축과, 콘솔·관제 앱에서 바꾼 값이 서버와
> 단말에 닿는 경로, 만료 때의 처리 주체를 한곳에 모은 정본이다. 타이머가 속한 절차 자체는 각 기능 문서가 정본이다 —
> 발언권 [ptt_flows.md](ptt_flows.md) · 그룹 호 해제 정책 [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) ·
> 긴급 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) · CMP 제어 [cmp_media_api.md](../../api/cmp_media_api.md) §7.7.
> MCVideo 의 송출·수신 제어 타이머는 [mcvideo.md](mcvideo.md) 가 정본이고 이 문서 범위 밖이다.
>
> 근거 규격 판본: TS 24.379 V19.8.0 · TS 24.380 V19.3.0 · TS 24.481 V19.3.0 · TS 24.484 V19.6.0 (Release 19).

## 1. 한눈에

| 소유 | 타이머 | 값의 정본 문서 | CIMS 에서 돌리는 곳 |
|---|---|---|---|
| 호 제어 서버 (controlling MCPTT function) | TNG1 · TNG2 · TNG3 | 그룹 문서(TNG1·TNG3) · service configuration(TNG2) | CSP |
| 발언권 제어 서버 (floor control server) | T1 · T2 · T3 · T4 · T7 · T8 · T20 | service configuration(T1·T2·T3·T7·T8·T20) · 그룹 문서(그룹 호의 T4) | CMP — 값은 CSP 가 싣는다 |
| 단말 (floor participant) | T100 · T101 · T103 · T104 · T132 | UE initial configuration | 단말 SDK `libcimsue` |

- 세션을 끝내는 타이머(T4·TNG3)와 개시를 판정하는 타이머(TNG1)는 **전부 서버 것**이다. 단말에는 대응 타이머가 없고, 만료의
  결과(BYE · 개시 응답)만 받는다.
- 단말이 직접 돌리는 타이머는 **발언권 메시지 재전송·판정용**(T100 계열)뿐이다.
- 서버 타이머 가운데 단말에 값이 닿는 것은 T2 하나다 — Floor Granted 의 Duration 필드로 «이번 발언 허용 시간» 이 온다.

## 2. 규격의 타이머와 값의 출처

### 2.1 호 제어 서버 — TS 24.379 부록 B.2.1 (표 B.2.1-1)

| 타이머 | 값의 출처 | 시작 | 정상 정지 | 만료 |
|---|---|---|---|---|
| **TNG1** acknowledged call setup timer | 그룹 문서 `<on-network-timeout-for-acknowledgement-of-required-members>` | 그룹 세션 개시 INVITE 수신 — 제휴 중인 필수 멤버(`<on-network-required>`)가 있을 때, 초대를 내보내기 **전에** (§6.3.3.3) | 필수 멤버 전원의 200 | `<on-network-action-upon-expiration-of-…>` 가 proceed 면 개시자에게 200 + Warning `111`, abandon 이면 480 + Warning `112` · 성립한 leg 은 BYE, 미성립 leg 은 CANCEL |
| **TNG2** in-progress emergency group call timer | service configuration `<on-network><emergency-call><group-time-limit>` | 긴급 그룹 호를 여는 INVITE·re-INVITE | 긴급 상태 해제 수락 | 긴급 상태를 풀고 보통 우선순위로 되돌린다(§6.3.3.1.16). 호는 이어진다 |
| **TNG3** group call timer | 그룹 문서 `<on-network-maximum-duration>` · 애드혹 그룹 호 = service configuration `<adhoc-group-call><max-duration-of-call>` | 그룹 세션 개시 INVITE — 세션 식별자를 부여한 뒤(§6.3.3.5.1) | 마지막 단말이 세션을 떠남 | 그룹 호 해제(§6.3.8.1 5)) |

- 권장값은 표에 없다 — 세 타이머 모두 «문서에서 얻는다» 만 적혀 있다.
- `<on-network-maximum-duration>` 은 편성(prearranged) 그룹에 필수, chat 그룹에 선택이다(TS 24.481 검증 제약 · §6.3.3.5.1 NOTE 1).
  요소가 없으면 TNG3 를 켜지 않는다.
- TNG2 를 켜면 TNG3 를 켜지 않고, 돌던 TNG3 는 긴급 격상 때 멈춘다. 긴급이 끝나면(해제·TNG2 만료) TNG3 를 새로 센다(§6.3.3.5.2).
- 만료 동작 값이 정의 밖이면 abandon 으로 해석한다(TS 24.481 §7.2.2 u)).

### 2.2 발언권 제어 서버 — TS 24.380 §11.1.3 (표 11.1.3-1)

| 타이머 | 규격 기본값 | 값의 출처 | 만료 |
|---|---|---|---|
| **T1** End of RTP media | 4초 (최대 6초) | service configuration `<fc-timers-counters><T1-end-of-rtp-media>` | 승인된 발언이 끝난 것으로 본다 |
| **T2** Stop talking | 최대 30초 | service configuration `<transmit-time><time-limit>` | 발언이 너무 길다 — 회수 절차로 |
| **T3** Stop talking grace | 3초 (audio cut-in 그룹은 0) | `<fc-timers-counters><T3-stop-talking-grace>` | 미디어 중단, 발언권 유휴 |
| **T4** Inactivity | 30초 | 그룹 호 = 그룹 문서 `<on-network-hang-timer>` · 개별 호 = service configuration `<private-call><hang-time>` · 애드혹 그룹 호 = `<adhoc-group-call><hang-time>` | 호 해제 |
| **T7** Floor Idle | 무선망 특성에 따름 | `<fc-timers-counters><T7-floor-idle>` | Floor Idle 재송신 |
| **T8** Floor Revoke | 1초 | `<fc-timers-counters><T8-floor-revoke>` | Floor Revoke 재송신 |
| **T20** Floor Granted | 1초 | `<fc-timers-counters><T20-floor-granted>` | Floor Granted 재송신(대기열에서 승급한 화자) |

- T4 는 `G: Floor Idle` 진입 때 시작하고 발언 요청이 오면 멈춘다. 만료되면 발언권 제어 서버가 호 제어에 알리고, 해제할지
  다시 걸지는 사업자 정책이다(§6.3.4.3.5).
- T4 만료로 세션을 해제하는 호 종류는 편성·일제·애드혹 그룹 호다(TS 24.379 §6.3.8.1 1) — chat 은 목록에 없다).

### 2.3 단말 — TS 24.380 §11.1.1 (표 11.1.1-1) · §11.2.1

| 타이머 | 역할 | 규격 값 | 짝 카운터 |
|---|---|---|---|
| **T100** Floor Release | Floor Release 를 보낸 뒤 Floor Idle(또는 남의 미디어)이 올 때까지 재전송 간격 | 설정값. 재전송 총 시간은 6초 미만이어야 한다(NOTE 1) | C100 기본 3 |
| **T101** Floor Request | Floor Request 응답(Granted·Taken·Deny·Queue Position Info) 대기 — 만료마다 재전송 | 설정값. 재전송 총 시간은 6초 미만을 권장(NOTE 2) | C101 기본 3 |
| **T103** End of RTP media | Floor Taken 뒤 미디어가 끊기면 «그 발언이 끝났다» 로 판정 | T1 과 같게 권장 | — |
| **T104** Floor Queue Position Request | 대기열 위치 요청 재전송 간격 | 설정값 | C104 기본 3 |
| **T132** Queued granted user action | 대기 끝에 승인됐는데 사용자가 PTT 를 누르지 않는 시간 — 만료 때 Floor Release | 기본 2초 | — |

- 값의 출처는 **MCS UE initial configuration** 이다 — 문서 `<on-network><Timers>` 의 `<T100>`·`<T101>`·`<T103>`·`<T104>`·`<T132>`
  (초, 0~255, TS 24.484 §7.2.2.1·§7.2.2.6·§7.2.2.7 = TS 24.483 §8.2.11~§8.2.15 의 MO 요소).
- 단말에는 T4·TNG 계열에 대응하는 타이머가 없다. 온네트워크에서 세션 유휴를 재는 것은 서버뿐이다(T230 Inactivity 는
  오프네트워크 단말의 타이머다 — §11.1.2).

## 3. 값의 정본과 편집 자리

값은 세 문서에 나뉘어 있고, 문서마다 편집 자리와 저장 위치가 다르다.

| 문서 | 담는 타이머 | 편집 자리 | 저장 | 범위 · 기본값 |
|---|---|---|---|---|
| **그룹 문서** (TS 24.481, 그룹마다) | T4 · TNG3 · TNG1(+ 만료 동작 · 시작 최소 응답 · 필수 멤버) | 콘솔 `구성 › PTT 그룹` 편집 · 관제 앱 [PTT 그룹] 화면(GMS XCAP PUT) | DB `ptt_groups` 의 `hang_timer_sec`·`max_duration_sec`·`ack_timeout_sec`·`ack_action`·`min_number_to_start` | T4 0~3600초 기본 30(0 = 미사용) · TNG3 0~86400초 기본 3600(0 = 무제한) · TNG1 1~300초 기본 5 · 만료 동작 기본 abandon |
| **service configuration** (TS 24.484 §8.4, 시스템 전역 1건) | T1 · T2 · T3 · T7 · T8 · T20 · C7 · C20 · TNG2 | CSC 모듈 설정 `ServiceConfig.*` — 섹션 «MCPTT 서비스 설정 문서 (service-config) — on-network 파라미터» | CSC 설정(`csc.json`) | T1 4000 ms · T2 30000 ms(`TransmitTime.TimeLimit`) · T3 3000 ms · T7 0(비활성) · T8 1000 ms · T20 1000 ms · C7 3 · C20 3 · TNG2 0(`EmergencyCall.GroupTimeLimit` — 0 = 요소 생략 = 미가동) |
| **UE initial configuration** (TS 24.484 §7.2, 시스템 전역 1건) | T100 · T101 · T103 · T104 · T132 | CSC 모듈 설정 `UeInitConfig.Timers.*` — 섹션 «MCS UE 초기 설정 문서 (ue-init-config)» | CSC 설정(`csc.json`) | 0~255초 · T100·T101·T103·T104 기본 4 · T132 기본 6 |

- 두 쓰기 경로(콘솔 관리 API · 관제 앱 XCAP)는 같은 DB 열에 쓰고 같은 범위 검사를 탄다. 화면 칸 이름은 규격 이름을 괄호로
  병기한다 — 규칙 정본 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.7.
- T4 는 그룹마다 다르고 나머지 발언권 타이머는 시스템 전역이다 — T4 만 그룹 문서에 있기 때문이다(§2.2).
- CSC 모듈 설정의 두 섹션은 재기동 없이 재적재(SIGUSR1)로 반영된다. 문서의 ETag 는 내용에서 파생되므로 값이 바뀌면 함께 바뀐다.

## 4. 전달 경로

### 4.1 그룹 문서 — T4 · TNG3 · TNG1

```
콘솔 ──관리 API PUT /api/v1/ptt/groups──┐
                                        ├→ CSC → DB(ptt_groups) → GROUP_CHANGED(UDP) → CSP
관제 앱 ──GMS XCAP PUT(TS 24.481)───────┘                                              │
                                                                  그룹 맵 재적재 ←──────┤
                                                    CMP PTT_GROUP_MODIFY(T4) ←─────────┤
                                       멤버 단말 xcap-diff NOTIFY(RFC 5875) ←───────────┘
단말 ──XCAP GET(그룹 문서)──→ CSC(GMS)
```

1. CSC 가 DB 에 쓰고 CSP 에 `GROUP_CHANGED` 를 보낸다.
2. CSP 가 그룹 맵을 DB 에서 다시 읽는다. 통지를 놓친 변경은 60초 주기 재적재가 따라잡는다.
3. CSP 가 재적재 전후의 그룹 문서 지문을 비교해, 바뀐 그룹의 멤버(변경 전 ∪ 변경 후) 가운데 GMS 문서 변경을 구독한 단말에
   xcap-diff NOTIFY 를 보낸다. 통지에는 «어느 문서가 바뀌었다» 만 있고 값은 없다.
4. 단말이 그룹 문서를 다시 받는다. 문서에 `<on-network-hang-timer>`·`<on-network-maximum-duration>`·
   `<on-network-timeout-for-acknowledgement-of-required-members>` 가 실려 있다.

### 4.2 service configuration — T1 · T2 · T3 · T7 · T8 · T20 · TNG2

```
CSC 모듈 설정 변경 → CSC 재적재 → 문서 ETag 변화 → SERVICE_CONFIG_CHANGED(UDP) → CSP
                                                                               │
                              GET /internal/mcptt/service-config(문서 재취득) ←─┤
                              cms 구독 단말 전원에 xcap-diff NOTIFY ←───────────┘
CSP ──PTT_GROUP_ADD / MODIFY floor_timers──→ CMP
```

- CSP 는 문서를 기동 · SIGUSR1 · `CSC_RESTART` · `SERVICE_CONFIG_CHANGED` 때 받아 사본을 든다. 받지 못하면 마지막 성공값을 쓴다.
- CSP 는 그 값을 그룹 세션의 `PTT_GROUP_ADD`·`PTT_GROUP_MODIFY` 마다 `floor_timers` 로 CMP 에 싣는다
  (초 단위 — 1초 미만은 버린다, CMP 허용 범위로 맞춘다). CMP 설정 `Floor*Sec` 는 CSP 가 문서를 받지 못했을 때의 폴백이다.
- 단말은 xcap-diff(cms 축)를 받으면 문서를 다시 받는다. 단말이 이 문서에서 쓰는 것은 Resource-Priority 와 자격 판정이고,
  발언권 타이머 값은 쓰지 않는다(서버 타이머다).
- **T2 는 발언권 메시지로 단말에 닿는다** — CMP 가 Floor Granted 의 Duration 에 이번 발언 허용 시간을 싣는다(§5.2).

### 4.3 UE initial configuration — T100 · T101 · T103 · T104 · T132

```
CSC 모듈 설정 변경 → CSC 재적재 → 문서 ETag 변화
단말 ──GET <XCAP root>/org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID>(로그인 전 · 토큰 없음)──→ CSC(CMS)
```

- 이 문서를 내는 것은 **설정 관리 서버(CMS)** 의 일이다 — 문서는 CMS 의 XCAP 트리에 있고(TS 24.484 §7.2.1.1), CMS 가 전역(master)
  문서에서 단말별 문서를 만들어 준다. CIMS 에서는 CSC 가 CMS 역할이다(`handle_ue_init_config`).
- 로그인 전 부트스트랩 문서다 — 인증 서버(IdMS) 주소가 이 문서에 있어 사용자 인증보다 앞 단계다. 단말은 온라인 설정 때 가진
  사본과 다르면 새 문서를 내려받는다(§4.2.2.1.1). CIMS 단말은 계정을 올리기 전에 토큰 없이 받는다(관제 앱은 가진 사본의
  ETag 로 조건부 요청). XCAP 경로의 사용자 자리는 단말 인스턴스 ID 이고, CSC 는 어떤 ID 가 와도 같은 전역 문서를 준다.
- 규격은 이 문서도 변경 구독을 지원하게 한다(§7.2.2.12 → §6.3.13.3). CIMS 에는 이 문서의 변경 통지가 없다 — 바뀐 값은 단말이
  다음에 문서를 받을 때(로그인) 닿는다(§7 D10).

## 5. 타이머별 처리 주체와 만료 동작 (CIMS)

### 5.1 세션·개시 타이머

| 타이머 | 돌리는 곳 | 적용 대상 | 바꾼 값이 먹는 시점 | 만료 때 서버 | 단말이 받는 것 |
|---|---|---|---|---|---|
| **T4** | CMP — `G: Floor Idle` 진입에 무장, `G: Floor Taken` 진입에 정지 | 편성 그룹 호(chat·개별·애드혹은 0 = 미가동) | 진행 중 세션에도 — CSP 가 변경을 감지해 `PTT_GROUP_MODIFY` | CMP 가 `PTT_FLOOR_INACTIVITY` 이벤트를 올리고 다시 무장 → CSP 가 세션 해제(leg 마다 BYE, 미성립은 CANCEL) | BYE |
| **TNG3** | CSP — 1초 틱, 세션 시작 시각 기준 | 편성 그룹 호. 긴급 상태 동안은 세지 않는다 | 진행 중 세션에도 — 매 틱 현재 그룹 값으로 판정 | 세션 해제 | BYE |
| **TNG2** | CSP — 1초 틱 | 긴급 상태인 그룹 | 다음 틱 | 긴급 상태 해제 — 참여 멤버 re-INVITE, 비참여 제휴 멤버 통지 | re-INVITE · 통지 |
| **TNG1** | CSP — 개시 때 무장, 1초 틱으로 만료 확인 | 필수 멤버가 제휴 중이고 초대 대상인 편성 그룹 호의 새 세션 | 다음 개시부터 | proceed = 개시자 200 + Warning 111, abandon = 480 + Warning 112 | 개시 응답 · 필수 멤버 단말은 그 시간 안의 응답 |

- 세션 해제(T4·TNG3)는 **호만 끝낸다**. 그룹 선택과 제휴(affiliation)는 남는다 — 제휴가 끝나는 것은 해제 PUBLISH 와 로그오프뿐이다
  (TS 24.379 §7.3.5 NOTE).
- 해제 사유(T4 / TNG3)는 서버 로그와 세션 이력에만 남는다. BYE 에는 싣지 않으므로 단말에는 두 경우가 같게 보인다.
- TNG1 이 proceed 로 끝나면 CSP 는 미응답 필수 멤버를 개시 단말에 INFO(`<non-acknowledged-user>`)로 알린다 — 관제 앱은
  «필수 멤버 n명이 응답하지 않은 채 통화가 열렸습니다» 경고를 띄운다.

### 5.2 발언권 타이머 (CMP)

| 타이머 | 동작 | 단말에 닿는 것 |
|---|---|---|
| T1 | 승인 뒤 미디어가 오지 않으면 발언 완료로 회수 | Floor Idle / Floor Taken |
| T2 | 발언 한도 초과 → Floor Revoke cause #2. 긴급·임박 화자는 제외, 0 = 무제한 | **Floor Granted 의 Duration**(이번 발언 허용 시간) · Floor Revoke |
| T3 | Revoke 뒤 Release 를 기다리는 유예 — 그동안 미디어 중계, 만료 때 강제 회수 | Floor Idle |
| T7 · C7 | 발언자 없는 동안 Floor Idle 재송신 | Floor Idle |
| T8 | 유예 중 Floor Revoke 재전송 간격 | Floor Revoke |
| T20 · C20 | 대기열에서 승급한 화자에게 첫 미디어까지 Floor Granted 재송신 | Floor Granted |

값·필드 이름·범위의 정본은 [cmp_media_api.md](../../api/cmp_media_api.md) §7.7 이다.

### 5.3 단말이 결과를 받아 하는 처리

| 사건 | 현장 앱 | 관제 앱(Windows · 태블릿) |
|---|---|---|
| 세션 해제 BYE (T4·TNG3) | 고른 주채널은 «무전 통화 없음» 대기로 남는다. PTT = 새 그룹 호 | 채널 카드가 대기로 돌아간다. 영상 채널(MCVideo)은 무전 세션 해제와 무관하게 이어진다 |
| Floor Granted Duration (T2) | 남은 발언 시간 표시 · 마감 직전 자체 종료 | 남은 발언 게이지 · 마감 직전 자체 종료 |
| 개시 200 + 미응답 멤버 통지 (TNG1 proceed) | — | 경고 토스트 |
| 개시 480 (TNG1 abandon) | 개시 실패 | 개시 실패 |
| 그룹 문서 변경 xcap-diff | 그룹 문서 재조회 | 그룹 목록 재조회 · 그룹 편집 폼에 새 값 |

## 6. 단말 구현 — 발언권 참여자 타이머 (`libcimsue`)

단말 코어의 발언권 참여자(`sdk/core/src/floor/floor_participant.*`)는 타이머 값을 **코드 상수**로 든다. UE initial configuration 의
`<Timers>` 는 읽지 않는다 — `UeInitConfigDoc` 이 해석하는 것은 참여 기능 PSI(`*-Service-Details`)뿐이다.

| 규격 타이머 | 구현 | 값 |
|---|---|---|
| T100 · C100 | Floor Release 뒤 Floor Idle 이 올 때까지 재전송. 횟수가 차면 `U: has no permission` 으로 | 800 ms 간격 · 재전송 2회(모두 3회 송신) |
| T101 · C101 | Floor Request 응답 대기 한 번. 만료 = 유휴로 돌아가고 «요청 시간 초과» 이벤트 — **재전송 없음** | 3000 ms |
| T103 | 없음 — 발언 종료는 서버의 Floor Idle·Floor Taken 으로만 안다 | — |
| T104 · C104 | 없음 — 대기열 위치는 서버가 보내는 Queue Position Info 로만 갱신한다 | — |
| T132 | 없음 | — |
| (T2 대응) | Floor Granted 의 Duration 마감 300 ms 전에 스스로 Floor Release | 서버가 준 값 |
| (NAT 유지) | 발언권 채널 Ack 주기 송신 — 시작 직후 1초 간격 2회, 이후 15초 | 상수 |

## 7. 규격 대비 편차 · 미구현

| # | 항목 | 규격 | CIMS | 근거 절 |
|---|---|---|---|---|
| D1 | 단말 타이머 값의 출처 | UE initial configuration `<Timers>` | 단말 코어가 문서 값을 읽지 않고 상수를 쓴다. CSC 는 문서에 값을 싣는다 — 콘솔에서 바꿔도 단말 동작은 변하지 않는다 | TS 24.484 §7.2.2.7 · TS 24.380 표 11.1.1-1 |
| D2 | T101 재전송 | 만료마다 Floor Request 재전송(C101 회) | 한 번 기다리고 포기 | TS 24.380 §11.1.1 · §11.2.1 |
| D3 | T103 · T104 · T132 | 단말 타이머 | 미구현 | TS 24.380 표 11.1.1-1 |
| D4 | UE initial configuration `<Timers>` 기본값 | T100 재전송 총 시간 6초 미만(NOTE 1) · T132 기본 2초 | 문서 기본값 T100 = 4초(C100 3회면 12초) · T132 = 6초 | TS 24.380 표 11.1.1-1 |
| D5 | 개별 호·애드혹 그룹 호의 T4 | service configuration `<private-call><hang-time>` · `<adhoc-group-call><hang-time>` | 문서에 두 요소가 없고, CSP 가 편성 그룹 호 밖에는 T4 = 0(미가동)을 싣는다 | TS 24.380 표 11.1.3-1 · TS 24.484 §8.4 |
| D6 | 애드혹 그룹 호의 TNG3 | `<adhoc-group-call><max-duration-of-call>` | 문서에 요소가 없고 TNG3 를 돌리지 않는다 | TS 24.379 표 B.2.1-1 |
| D7 | chat 그룹의 TNG3 | 문서에 `<on-network-maximum-duration>` 이 있으면 TNG3 를 켠다 | 그룹 문서는 chat 그룹에도 요소를 싣지만 CSP 는 편성 그룹 호에만 TNG3 를 돌린다(chat 세션은 상시) | TS 24.379 §6.3.3.5.1 |
| D8 | T4·TNG3 의 0 | 규격에 «0 = 미사용/무제한» 규정 없음 | 0 을 미사용(T4)·무제한(TNG3)으로 쓴다. 편성 그룹에 TNG3 값이 필수라는 제약은 0 일 때 `PT0S` 로 채운다 | TS 24.481 검증 제약 |
| D9 | 480 의 Warning 112 표시 | Warning 문구로 사유를 가른다 | 단말 코어가 최종 응답의 Warning 을 앱에 올리지 않는다 — 앱은 TNG1 포기를 다른 480 과 가르지 못한다 | TS 24.379 §4.4 · §6.3.3.3 |
| D10 | UE initial configuration 변경 구독 | 문서의 application usage 가 변경 구독을 지원한다 | CSC 재적재로 문서가 바뀌어도 통지가 없다(service configuration 은 `SERVICE_CONFIG_CHANGED` 로 통지) — 다음 로그인 때 반영 | TS 24.484 §7.2.2.12 · §6.3.13.3 |

## 8. 구현 위치

| 구성요소 | 위치 | 내용 |
|---|---|---|
| CSC | `csc/src/services/mcptt.py` | 그룹 문서 요소 산출·XCAP PUT 해석 · service configuration 문서 · UE initial configuration 문서(`_UE_INIT_DEFAULTS`·`_SERVICE_CONFIG_PARAM_DEFAULTS`) · 재적재 때 `SERVICE_CONFIG_CHANGED` |
| CSC | `csc/src/handlers/admin.py` | 관리 API 그룹 CRUD — 타이머 범위 검사(`_norm_group_timer`·`_norm_ack_setup`) · `GROUP_CHANGED` |
| CSC | `csc/config/config_template.json` | 모듈 설정 `UeInitConfig.Timers.*` · `ServiceConfig.*` 의 칸·범위·기본값 |
| CSP | `csp/GroupCallService.cpp` | `CmpSessionOf`(T4 출처) · `OnFloorInactivity`(T4 만료 → 해제) · `CheckSessionLimits`(TNG3·TNG2) · 개시 게이트(TNG1) · `ReloadGroupMap`(문서 변경 통지) · `SyncGroupsState`(CMP MODIFY) |
| CSP | `csp/CspServiceConfig.*` · `csp/CmpClient.cpp` | service configuration 사본 · `floor_timers` 조립(`AppendFloorTimers`) |
| CSP | `csp/CscInterface.cpp` · `csp/CspServer.cpp` | `GROUP_CHANGED`·`SERVICE_CONFIG_CHANGED` 수신 · xcap-diff NOTIFY |
| CMP | `cmp/PMcpttGroup.*` · `cmp/PCmpServer.cpp` | 발언권 타이머 구동 · T4 만료 이벤트 `PTT_FLOOR_INACTIVITY` · `floor_timers` 수신 |
| 단말 SDK | `sdk/core/src/floor/floor_participant.*` | 발언권 참여자 타이머(§6) |
| 단말 SDK | `sdk/core/src/csc/group_doc.cpp` · `cms_doc.cpp` | 그룹 문서 타이머 요소 읽기·쓰기 · UE initial configuration 해석(PSI) |
| 관제 앱 | `windows/dispatch-desktop/Views/GroupEditView.xaml` · `android/dispatch-tablet/…/ui/groups/GroupFormSections.kt` | 그룹 편집 폼의 T4·TNG3·TNG1 칸 |
