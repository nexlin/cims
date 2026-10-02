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
| 호 제어 서버 (controlling MCPTT function) | TNG1 · TNG2 · TNG3 · 개별 호 최대 통화 시간 | 그룹 문서(TNG1 · 편성 그룹 호의 TNG3) · service configuration(TNG2 · 애드혹 그룹 호의 TNG3 · 개별 호 최대 통화 시간) | CSP |
| 발언권 제어 서버 (floor control server) | T1 · T2 · T3 · T4 · T7 · T8 · T20 | service configuration(T1·T2·T3·T7·T8·T20 · 개별 호·애드혹 그룹 호의 T4) · 그룹 문서(편성 그룹 호의 T4) | CMP — 값은 CSP 가 싣는다 |
| 단말 (floor participant) | T100 · T101 · T103 · T104 · T132 | UE initial configuration | 단말 SDK `libcimsue` |

- 세션을 끝내는 타이머(T4·TNG3·개별 호 최대 통화 시간)와 개시를 판정하는 타이머(TNG1)는 **전부 서버 것**이다. 단말에는 대응 타이머가 없고, 만료의
  결과(BYE · 개시 응답)만 받는다.
- 단말이 직접 돌리는 타이머는 **발언권 메시지 재전송·판정용**(T100 계열)뿐이다.
- 서버 타이머 가운데 단말에 값이 닿는 것은 T2 하나다 — Floor Granted 의 Duration 필드로 «이번 발언 허용 시간» 이 온다.

## 2. 규격의 타이머와 값의 출처

### 2.1 호 제어 서버 — TS 24.379 부록 B.2.1 (표 B.2.1-1)

| 타이머 | 값의 출처 | 시작 | 정상 정지 | 만료 |
|---|---|---|---|---|
| **TNG1** acknowledged call setup timer | 그룹 문서 `<on-network-timeout-for-acknowledgement-of-required-members>` | 그룹 세션 개시 INVITE 수신 — 제휴 중인 필수 멤버(`<on-network-required>`)가 있을 때, 초대를 내보내기 **전에** (§6.3.3.3) | 필수 멤버 전원의 200 | `<on-network-action-upon-expiration-of-…>` 가 proceed 면 개시자에게 200 + Warning `111`, abandon 이면 480 + Warning `112` · 성립한 leg 은 BYE, 미성립 leg 은 CANCEL |
| **TNG2** in-progress emergency group call timer | service configuration `<on-network><emergency-call><group-time-limit>` | 긴급 그룹 호를 여는 INVITE·re-INVITE | 긴급 상태 해제 수락 | 긴급 상태를 풀고 보통 우선순위로 되돌린다(§6.3.3.1.16). 호는 이어진다 |
| **TNG3** group call timer | 그룹 문서 `<on-network-maximum-duration>` · 애드혹 그룹 호 = service configuration `<anyExt><adhoc-group-call><max-duration-of-call>` | 그룹 세션 개시 INVITE — 세션 식별자를 부여한 뒤(§6.3.3.5.1). 애드혹 그룹 호는 긴급·임박으로 개시한 호(priority adhoc group call)가 아닐 때(§17.4.2.2 13)) | 마지막 단말이 세션을 떠남 | 그룹 호 해제(§6.3.8.1 5)) |
| 개별 호 최대 통화 시간 (private call timer) | service configuration `<private-call><max-duration-with-floor-control>` · 발언권 제어 없는 호 = `<max-duration-without-floor-control>` | 개별 호 초대 INVITE 송신(§11.1.1.4.1 10)) | 호 해제 | 개별 호 해제(§6.3.8.2 2) · §11.1.4.4) |

- 권장값은 표에 없다 — 모든 타이머가 «문서에서 얻는다» 만 적혀 있다.
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
| **T4** Inactivity | 30초 | 그룹 호 = 그룹 문서 `<on-network-hang-timer>` · 개별 호 = service configuration `<private-call><hang-time>` · 애드혹 그룹 호 = `<adhoc-group-call><hang-time>` · 일제 애드혹 그룹 호 = `<adhoc-group-call><broadcast-hang-time>` | 호 해제 |
| **T7** Floor Idle | 무선망 특성에 따름 | `<fc-timers-counters><T7-floor-idle>` | Floor Idle 재송신 |
| **T8** Floor Revoke | 1초 | `<fc-timers-counters><T8-floor-revoke>` | Floor Revoke 재송신 |
| **T20** Floor Granted | 1초 | `<fc-timers-counters><T20-floor-granted>` | Floor Granted 재송신(대기열에서 승급한 화자) |

- T4 는 `G: Floor Idle` 진입 때 시작하고 발언 요청이 오면 멈춘다. 만료되면 발언권 제어 서버가 호 제어에 알리고, 해제할지
  다시 걸지는 사업자 정책이다(§6.3.4.3.5).
- T4 만료로 세션을 해제하는 호 종류는 편성·일제·애드혹 그룹 호(TS 24.379 §6.3.8.1 1) — chat 은 목록에 없다)와 개별 호다
  (§6.3.8.2 1)). 발언권 제어가 없는 개별 호(full-duplex)에는 T4 를 돌릴 발언권 제어 서버가 없다.

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
| **service configuration** (TS 24.484 §8.4, 시스템 전역 1건) | T1 · T2 · T3 · T7 · T8 · T20 · C7 · C20 · TNG2 · 개별 호 T4·최대 통화 시간 · 애드혹 그룹 호 T4·TNG3 | CSC 모듈 설정 `ServiceConfig.*` — 섹션 «MCPTT 서비스 설정 문서 (service-config) — on-network 파라미터» | CSC 설정(`csc.json`) | T1 4000 ms · T2 30000 ms(`TransmitTime.TimeLimit`) · T3 3000 ms · T7 0(비활성) · T8 1000 ms · T20 1000 ms · C7 3 · C20 3 · TNG2 0(`EmergencyCall.GroupTimeLimit` — 0 = 요소 생략 = 미가동) · 개별 호 `PrivateCall.HangTime` 30000 ms · `MaxDurationWithFloorControl`·`MaxDurationWithoutFloorControl` 3600000 ms · 애드혹 `AdhocGroupCall.HangTime`·`BroadcastHangTime` 30000 ms · `MaxDurationOfCall` 3600000 ms(시간 값 0 = 요소 생략 = 미가동) |
| **UE initial configuration** (TS 24.484 §7.2, 시스템 전역 1건) | T100 · T101 · T103 · T104 · T132 | CSC 모듈 설정 `UeInitConfig.Timers.*` — 섹션 «MCS UE 초기 설정 문서 (ue-init-config)» | CSC 설정(`csc.json`) | 0~255초 · T100·T101 기본 1 · T103·T104 기본 4 · T132 기본 2 |

- 두 쓰기 경로(콘솔 관리 API · 관제 앱 XCAP)는 같은 DB 열에 쓰고 같은 범위 검사를 탄다. 화면 칸 이름은 규격 이름을 괄호로
  병기한다 — 규칙 정본 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.7.
- **그룹 문서의 0** — «0 = 미사용/무제한» 은 CIMS 의 약속이라 문서에는 0 을 싣지 않는다(`PT0S` 는 규격 단말이 «0초» 로 읽는다).
  T4 0 = `<on-network-hang-timer>` 생략(요소가 없으면 T4 를 걸지 않는다). TNG3 0 = 편성 그룹은 값이 필수라(TS 24.481 §7.2.7
  «invite-members true 면 shall contain a value») 무제한 표기 `PT2147483647S`(`GROUP_MAX_DURATION_UNLIMITED`)를 싣는다 —
  §7 D8. XCAP PUT 은 그 값 이상을 0 으로, 명시한 `PT0S` 도 0 으로 읽고, 요소가 없으면 기존값을 둔다 — 받은 문서를 그대로 PUT 해도
  DB 값이 바뀌지 않는다.
- **chat 그룹 문서에는 `<on-network-maximum-duration>` 이 없다** — chat 그룹 호는 상시 세션이라 TNG3 를 돌리지 않고, 규격은
  요소가 있을 때만 TNG3 를 켠다(TS 24.379 §6.3.3.5.1 — chat 은 요소가 선택). DB 값(`max_duration_sec`)은 그대로 남아 그룹을 편성으로
  바꾸면 쓰인다.
- **단말 타이머 기본값**은 TS 24.380 표 11.1.1-1 범위다 — T100·T101 은 재전송 총 시간(× C100·C101 기본 3회)이 6초 미만이어야
  해서(NOTE 1 shall · NOTE 2 should) 초 단위로 1, T103 = T1(4초), T132 = 규격 기본 2초, T104 = 사이트 값. 설정에 값이 저장된
  사이트(configure·배포 overlay 가 템플릿 기본값을 써 둔 경우)는 저장값이 그대로 쓰인다 — 기본값을 따르려면 저장값을 고친다.
- T4 는 그룹마다 다르고 나머지 발언권 타이머는 시스템 전역이다 — T4 만 그룹 문서에 있기 때문이다(§2.2).
- CSC 모듈 설정의 두 섹션은 재기동 없이 재적재(SIGUSR1)로 반영된다. 문서의 ETag 는 내용에서 파생되므로 값이 바뀌면 함께 바뀌고,
  재적재로 문서가 바뀌면 CSC 가 CSP 에 알린다(service configuration = `SERVICE_CONFIG_CHANGED` · UE initial configuration =
  `UE_INIT_CONFIG_CHANGED` — §4.2·§4.3).

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
4. 단말이 그룹 문서를 다시 받는다. 문서에 `<on-network-hang-timer>`(T4 0 이면 없음)·`<on-network-maximum-duration>`(편성 그룹만
   — 0 이면 무제한 표기)·`<on-network-timeout-for-acknowledgement-of-required-members>` 가 실려 있다(§3).

### 4.2 service configuration — T1 · T2 · T3 · T7 · T8 · T20 · TNG2 · 개별 호·애드혹 그룹 호의 T4·최대 시간

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
- 개별 호·애드혹 그룹 호의 세션 타이머 — `<on-network><private-call>`(`<hang-time>`·`<max-duration-with-floor-control>`·
  `<max-duration-without-floor-control>`)와 `<on-network><anyExt><adhoc-group-call>`(`<allow-adhoc-group-call-support>`·
  `<max-no-participants>` 필수 뒤 `<hang-time>`·`<broadcast-hang-time>`·`<max-duration-of-call>`, TS 24.484 §8.4.2.1·§8.4.2.3).
  시간 값 0 인 요소는 싣지 않고(그 타이머 미가동) `<private-call>` 은 자식이 없으면 요소째 뺀다. `<adhoc-group-call>` 은 늘
  싣는다 — 요소가 없으면 «애드혹 미지원» 이라 규격 단말이 애드혹 호를 개시하지 않는다(§8.4.2.6 · TS 24.379 §17.2.2.1.1).
  CSP 는 T4 를 호 종류에 맞게 골라 `floor_timers.t4_inactivity` 로 싣고(§5.1), 최대 시간을 1초 틱으로 센다. 바뀐 T4 는 진행 중
  세션에도 다음 60초 주기 동기화의 `PTT_GROUP_MODIFY` 로 닿는다.
- 단말은 xcap-diff(cms 축)를 받으면 문서를 다시 받는다. 단말이 이 문서에서 쓰는 것은 Resource-Priority 와 자격 판정이고,
  발언권 타이머 값은 쓰지 않는다(서버 타이머다).
- **T2 는 발언권 메시지로 단말에 닿는다** — CMP 가 Floor Granted 의 Duration 에 이번 발언 허용 시간을 싣는다(§5.2).

### 4.3 UE initial configuration — T100 · T101 · T103 · T104 · T132

```
CSC 모듈 설정 변경 → CSC 재적재 → 문서 ETag 변화 → UE_INIT_CONFIG_CHANGED(UDP) → CSP
                                                                               │
     cms 구독 단말마다 xcap-diff NOTIFY(sel = 그 단말의 ue-init-config 문서) ←──┘
단말 ──GET <XCAP root>/org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID>(로그인 전 · 토큰 없음)──→ CSC(CMS)
```

- 이 문서를 내는 것은 **설정 관리 서버(CMS)** 의 일이다 — 문서는 CMS 의 XCAP 트리에 있고(TS 24.484 §7.2.1.1), CMS 가 전역(master)
  문서에서 단말별 문서를 만들어 준다. CIMS 에서는 CSC 가 CMS 역할이다(`handle_ue_init_config`).
- 로그인 전 부트스트랩 문서다 — 인증 서버(IdMS) 주소가 이 문서에 있어 사용자 인증보다 앞 단계다. 단말은 온라인 설정 때 가진
  사본과 다르면 새 문서를 내려받는다(§4.2.2.1.1). CIMS 단말은 계정을 올리기 전에 토큰 없이 받는다(관제 앱은 가진 사본의
  ETag 로 조건부 요청). XCAP 경로의 사용자 자리는 단말 인스턴스 ID 이고, CSC 는 어떤 ID 가 와도 같은 전역 문서를 준다.
- **변경 통지** — 이 문서의 application usage 도 변경 구독을 지원한다(§7.2.2.12 → §6.3.13.3). CSC 재적재(SIGUSR1)로 문서가
  바뀌면 CSC 가 `UE_INIT_CONFIG_CHANGED`(etag = 새 문서 ETag)를 보내고, CSP 가 cms 구독 단말마다 xcap-diff NOTIFY(RFC 5875)를
  보낸다. 문서 선택자는 단말마다 다르다 — `org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID>`(§7.2.1.1), MCS UE ID =
  단말의 instance ID(§7.2.1.0)라 CSP 는 그 구독 단말(SUBSCRIBE Contact 와 같은 단말)의 등록 Contact `+sip.instance` 에서 얻는다.
  등록이 없거나 instance 가 없는 구독은 건너뛴다(그 단말은 다음 로그인 때 받는다). CSP 는 이 문서의 값을 쓰지 않는다.
- 비교·new-etag 는 `McpttServer.PublicUrl` 기준 문서다. PublicUrl 이 없는 올인원에서는 주소류가 단말의 요청 Host 로 갈려 new-etag 가
  참고값이고, 단말은 자기 사본의 ETag(If-None-Match)로 다시 받는다. 첫 적재(기동)·내용이 같은 재적재는 통지하지 않는다.

## 5. 타이머별 처리 주체와 만료 동작 (CIMS)

### 5.1 세션·개시 타이머

| 타이머 | 돌리는 곳 | 적용 대상 | 바꾼 값이 먹는 시점 | 만료 때 서버 | 단말이 받는 것 |
|---|---|---|---|---|---|
| **T4** | CMP — `G: Floor Idle` 진입에 무장, `G: Floor Taken` 진입에 정지 | 호 종류별 출처(`CspSessionT4Sec`): 편성·일제 그룹 호 = 그룹 `hang_timer_sec` · 애드혹 그룹 호 = `<adhoc-group-call><hang-time>`(일제 통화면 `<broadcast-hang-time>`) · 발언권 제어가 있는 개별 호 = `<private-call><hang-time>`. chat·발언권 제어 없는 개별 호는 0 = 미가동 | 진행 중 세션에도 — 그룹 값은 변경 감지, service configuration 값은 60초 주기 동기화에서 `PTT_GROUP_MODIFY` | CMP 가 `PTT_FLOOR_INACTIVITY` 이벤트를 올리고 다시 무장 → CSP 가 호 해제(그룹 호 TS 24.379 §6.3.8.1 1) · 개별 호 §6.3.8.2 1) — leg 마다 BYE, 미성립은 CANCEL) | BYE |
| **TNG3** | CSP — 1초 틱, 세션 시작 시각 기준 | 편성 그룹 호(그룹 `max_duration_sec`) · 애드혹 그룹 호(`<adhoc-group-call><max-duration-of-call>` — 긴급·임박으로 개시한 호 제외, §17.4.2.2 13)). 긴급 상태 동안은 세지 않는다 | 진행 중 세션에도 — 매 틱 현재 값으로 판정 | 세션 해제 | BYE |
| **개별 호 최대 통화 시간** | CSP — 1초 틱, 세션 시작 시각 기준 | 개별 호 — 발언권 제어 있는 호 = `<max-duration-with-floor-control>`, 없는 호 = `<max-duration-without-floor-control>`. 긴급 개별 호에도 센다(§6.3.8.2 2)) | 진행 중 세션에도 — 매 틱 현재 값으로 판정 | 호 해제(§6.3.8.2 2)) | BYE |
| **TNG2** | CSP — 1초 틱 | 긴급 상태인 그룹 | 다음 틱 | 긴급 상태 해제 — 참여 멤버 re-INVITE, 비참여 제휴 멤버 통지 | re-INVITE · 통지 |
| **TNG1** | CSP — 개시 때 무장, 1초 틱으로 만료 확인 | 필수 멤버가 제휴 중이고 초대 대상인 편성 그룹 호의 새 세션 | 다음 개시부터 | proceed = 개시자 200 + Warning 111, abandon = 480 + Warning 112 | 개시 응답 · 필수 멤버 단말은 그 시간 안의 응답 |

- 세션 해제(T4·TNG3·개별 호 최대 통화 시간)는 **호만 끝낸다**. 그룹 선택과 제휴(affiliation)는 남는다 — 제휴가 끝나는 것은 해제 PUBLISH 와 로그오프뿐이다
  (TS 24.379 §7.3.5 NOTE).
- 해제 사유(T4 / TNG3·최대 통화 시간)는 서버 로그와 세션 이력에만 남는다. BYE 에는 싣지 않으므로 단말에는 두 경우가 같게 보인다.
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
| 세션 해제 BYE (T4·TNG3·최대 통화 시간) | 고른 주채널은 «무전 통화 없음» 대기로 남는다. PTT = 새 그룹 호 | 채널 카드가 대기로 돌아간다. 영상 채널(MCVideo)은 무전 세션 해제와 무관하게 이어진다 |
| Floor Granted Duration (T2) | 남은 발언 시간 표시 · 마감 직전 자체 종료 | 남은 발언 게이지 · 마감 직전 자체 종료 |
| 개시 200 + 미응답 멤버 통지 (TNG1 proceed) | — | 경고 토스트 |
| 개시 480 (TNG1 abandon) | 개시 실패 | 개시 실패 |
| 그룹 문서 변경 xcap-diff | 그룹 문서 재조회 | 그룹 목록 재조회 · 그룹 편집 폼에 새 값 |

## 6. 단말 구현 — 발언권 참여자 타이머 (`libcimsue`)

단말 코어의 발언권 참여자(`sdk/core/src/floor/floor_participant.*`)가 값을 받는 경로 — 앱이 UE initial configuration 을 받아
(`CscClient::fetchUeInitConfig` → `UeInitConfigDoc.floorTimers`, `<on-network><Timers>` 초 → ms) 계정에 싣는다(`AccountConfig.floorTimers`).
계정의 MCPTT 호마다 그 값으로 참여자를 연다. 문서가 바뀌면(cms xcap-diff 의 `org.3gpp.mcptt.ue-init-config` 선택자 — §4.3) 앱이 다시 받아
`Engine::setFloorTimers` 로 계정에 싣는다(다음 호부터). 문서를 못 받았거나 요소가 없으면(0) 기본값이다. 카운터 C100·C101·C104 는 문서에 없어 기본값 3 이다.

| 규격 타이머 | 구현 | 기본값 |
|---|---|---|
| T100 · C100 | Floor Release 뒤 Floor Idle(또는 남의 Floor Taken·내 이탈의 Release Multi Talker)이 올 때까지 T100 만료마다 재전송, C100 회 송신 뒤 다음 만료에 `U: has no permission`(§6.2.4.6.2·§6.2.4.6.3) | 1 s · 3 |
| T101 · C101 | Floor Request 응답(Granted·Taken·Deny·Queue Position Info) 대기 — T101 만료마다 재전송, C101 회 송신 뒤 다음 만료에 «요청 시간 초과» + `U: has no permission`(§6.2.4.4.5·§6.2.4.4.6). 암묵적 발언 요청(개시 INVITE)이 받아들여졌을 때도 같다 — 첫 송신을 INVITE 로 치고 만료부터 명시 Floor Request(§6.2.4.2.2 4.a) | 1 s · 3 |
| T103 | 받던 미디어(RTP)가 끊기면 그 발언이 끝났다 — `U: has no permission`(화자 있음)에서 만료 = 화자 표시를 비우고 «floor idle» 알림(§6.2.4.3.6). 코어가 1초마다 호의 수신 RTP 패킷 수를 보고 늘면 다시 건다 — 미디어를 한 번도 보지 못했으면 돌지 않는다(선택 타이머) | 4 s(= 서버 T1) |
| T104 · C104 | 대기열 위치 요청(`Engine::floorQueuePosition`, §6.2.4.9.9) 뒤 Queue Position Info 가 올 때까지 재전송, C104 회 뒤 «대기 시간 초과» + Floor Release(§6.2.4.9.11) | 4 s · 3 |
| T132 | 대기 끝 승인(`U: queued` 에서 Floor Granted, §6.2.4.9.4) 뒤 사용자가 누르지 않으면 Floor Release(§6.2.4.9.13). CIMS 앱은 누른 채 대기하므로(떼면 Floor Release 로 대기를 거둔다) 승인 = 곧 송출 의사(§6.2.4.9.12) — T132 는 «떼고 대기» 조작에서만 돈다 | 2 s |
| (T2 대응) | Floor Granted 의 Duration 마감 300 ms 전에 스스로 Floor Release | 서버가 준 값 |
| (NAT 유지) | 발언권 채널 Ack 주기 송신 — 시작 직후 1초 간격 2회, 이후 15초 | 상수 |

## 7. 규격 대비 편차 · 미구현

| # | 항목 | 규격 | CIMS | 근거 절 |
|---|---|---|---|---|
| D7 | chat 그룹의 TNG3 칸 (편집 폼) | chat 그룹 문서의 `<on-network-maximum-duration>` 은 선택 — 요소가 없으면 TNG3 를 켜지 않는다 | 서버는 규격대로다 — chat 그룹 문서에 요소를 싣지 않고 CSP 는 chat 세션에 TNG3 를 돌리지 않는다(§3). 콘솔 그룹 편집은 chat 그룹에서 «최대 통화 시간(TNG3, 초)» 칸을 잠근다. 관제 앱 두 벌의 그룹 편집 폼은 chat 그룹에서도 칸을 열어 둔다 — 값은 저장되지만 문서에도 호에도 쓰이지 않는다. 잠그는 것은 미구현(Windows 몫) | TS 24.379 §6.3.3.5.1 · TS 24.481 §7.2.7 |
| D8 | TNG3 의 «무제한» | 규격에 «무제한» 표기가 없다 — 편성 그룹은 `<on-network-maximum-duration>` 에 값이 필수 | 0(무제한)을 문서에 무제한 표기 `PT2147483647S`(약 68년)로 싣는다. 규격·스키마에 xs:duration 상한이 없어(XML Schema Part 2 §3.2.6) 초를 32비트 부호 있는 정수로 드는 수신 측이 넘치지 않는 최댓값을 골랐다 — 설정 범위(0~86400초)와 겹치지 않아 XCAP PUT 이 0 으로 되읽는다. CSP 는 0 이면 TNG3 를 돌리지 않는다. T4 0 은 요소 생략이라 규격대로다(§3). 문서를 읽어 폼에 보이는 쪽(관제 앱 두 벌)은 `<on-network-hang-timer>` 없음 = T4 0(미사용), 무제한 표기 = TNG3 0(무제한)으로 보여야 한다 — 미구현(Windows 몫) | TS 24.481 §7.2.7 |

## 8. 구현 위치

| 구성요소 | 위치 | 내용 |
|---|---|---|
| CSC | `csc/src/services/mcptt.py` | 그룹 문서 요소 산출·XCAP PUT 해석(0 의 표기 — `GROUP_MAX_DURATION_UNLIMITED`) · service configuration 문서(`<private-call>`·`<adhoc-group-call>` 포함) · UE initial configuration 문서(`_UE_INIT_DEFAULTS`·`_SERVICE_CONFIG_PARAM_DEFAULTS`) · 재적재 때 `SERVICE_CONFIG_CHANGED`·`UE_INIT_CONFIG_CHANGED` |
| CSC | `csc/src/handlers/admin.py` | 관리 API 그룹 CRUD — 타이머 범위 검사(`_norm_group_timer`·`_norm_ack_setup`) · `GROUP_CHANGED` |
| CSC | `csc/config/config_template.json` | 모듈 설정 `UeInitConfig.Timers.*` · `ServiceConfig.*` 의 칸·범위·기본값 |
| CSP | `csp/GroupCallService.cpp` | `CmpSessionOf`(T4 — 호 종류별 출처) · `OnFloorInactivity`(T4 만료 → 해제, chat 제외) · `CheckSessionLimits`(TNG3·개별 호 최대 통화 시간·TNG2) · 개시 게이트(TNG1) · `ReloadGroupMap`(문서 변경 통지) · `SyncGroupsState`(CMP MODIFY — 지문에 service configuration T4 포함) |
| CSP | `csp/CspServiceConfig.*` · `csp/CmpClient.cpp` | service configuration 사본(`ParseCallTimers` — 개별·애드혹 세션 타이머) · 호 종류별 선택 `CspSessionT4Sec`·`CspSessionMaxDurationSec` · `floor_timers` 조립(`AppendFloorTimers`) · ue-init-config 문서 선택자 `CspUeInitConfigSelector` |
| CSP | `csp/CscInterface.cpp` · `csp/CspServer.cpp` | `GROUP_CHANGED`·`SERVICE_CONFIG_CHANGED`·`UE_INIT_CONFIG_CHANGED` 수신 · xcap-diff NOTIFY(`SendUeInitConfigNotify` — 구독 단말별 선택자) |
| CMP | `cmp/PMcpttGroup.*` · `cmp/PCmpServer.cpp` | 발언권 타이머 구동 · T4 만료 이벤트 `PTT_FLOOR_INACTIVITY` · `floor_timers` 수신 |
| 단말 SDK | `sdk/core/src/floor/floor_participant.*` | 발언권 참여자 타이머(§6) |
| 단말 SDK | `sdk/core/src/csc/group_doc.cpp` · `cms_doc.cpp` | 그룹 문서 타이머 요소 읽기·쓰기 · UE initial configuration 해석(PSI) |
| 관제 앱 | `windows/dispatch-desktop/Views/GroupEditView.xaml` · `android/dispatch-tablet/…/ui/groups/GroupFormSections.kt` | 그룹 편집 폼의 T4·TNG3·TNG1 칸 |
