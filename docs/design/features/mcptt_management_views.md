# MCPTT 관리 조회 화면 — 그룹 정보 · 단말 현황 · 이용 정보

> **MCPTT 그룹 정보(§3)·단말 현황(§4, 단말 속성 수집 포함)·MCPTT 이용 정보(§5, 롤업 확장 포함)는 구현.** 그룹 편집과
> 실시간 반영(§2)·PTT 세션 이력은 구현돼 있다. 요구 = "MCPTT 그룹 정보 · 단말기 정보 · MCPTT 이용 정보를 저장하며, 필요시 저장 내용을
> 확인할 수 있다" + "MCPTT 웹 기반 관리 도구에서 그룹 등록/수정/삭제 시 실시간으로 업데이트한다".
>
> 관련: [monitoring.md](monitoring.md)(실시간 상태·이력), [sip_statistics.md](sip_statistics.md)(1분 롤업 — 이용
> 정보의 원천), [registration_binding_set.md](registration_binding_set.md)(등록 바인딩·`+sip.instance`),
> [../modules/csc.md](../modules/csc.md) §3.5·§5(그룹 CRUD·CSP 통지), [dispatch_desktop_ui.md](dispatch_desktop_ui.md)
> §4.7(관제 앱 그룹 편집), [../console_design_system.md](../console_design_system.md)(시각 계약).

---

## 1. 범위와 현재 상태

| 항목 | 화면 | 상태 |
|---|---|---|
| 그룹 등록 · 수정 · 삭제 | 콘솔 `구성 > PTT 그룹`, 관제 앱 [PTT 그룹] | 구현 |
| 그룹 변경 실시간 반영 | — (§2) | 구현 |
| MCPTT 이용 이력 (세션 단위) | 콘솔 `서비스 > PTT 세션 이력` (`/service/history/ptt`) | 구현 |
| **MCPTT 그룹 정보** (조회 · 상태) | `서비스 > MCPTT 그룹 정보` (`/service/ptt-groups`) | 구현 (§3) |
| **단말 현황** (MCPTT 관점 단말 정보) | `서비스 > 단말 현황` (`/service/ptt-terminals`) | 구현 (§4) — 단말 속성 수집 포함 |
| **MCPTT 이용 정보** (기간 집계) | `서비스 > MCPTT 이용 정보` (`/service/ptt-usage`) | 구현 (§5) — 롤업 확장 포함 |

콘솔 `서비스` 메뉴 순서 = 서비스 현황 · MCPTT 그룹 정보 · 단말 현황 · MCPTT 이용 정보 · PTT 세션 이력 · VoLTE 호
이력. 세 화면 모두 `requiredRole: 'monitor'` 로 조회 전용이다 — 편집은 `구성` 메뉴와 관제 앱이 소유한다(조회와
편집 화면을 섞지 않는다).

## 2. 그룹 변경 실시간 반영 (구현)

```
① 콘솔 구성 > PTT 그룹 ─ Admin API PUT/POST/DELETE ─┐
① 관제 앱 [PTT 그룹]  ─ GMS XCAP PUT/DELETE (TS 24.481) ┤
                                                      ▼
                                   SPS(CSC) DB 저장 + GMS 문서 ETag 갱신
                                                      │ ② notify_csp GROUP_CHANGED (UDP)
                                                      ▼
                                   PSP GroupMap reload ── ③ PMP 그룹 세션·floor 정책 동기화
                                                      │ ④ xcap-diff NOTIFY (변경 전·후 멤버 단말)
                                                      ▼
                                   단말 ⑤ GMS 문서 재조회 → 그룹 목록 · 멤버 갱신
```

- 재시작·재로그인 없이 반영된다. ④ 는 ② 의 재적재 **뒤** 재적재 전·후 멤버의 합집합에게 간다(PSP
  `ReloadGroupMap` — 새 그룹·추가 멤버도 통지를 받는다). 통지 유실은 PSP 60초 주기 재적재(같은 전후 비교 — 지문
  `ComputeGroupDocHash` = 문서에 드러나는 그룹 설정 전부: 멤버 표시·이름·속성·MCVideo 몫)가 따라잡는다 ([../modules/csc.md](../modules/csc.md) §5.1). 재적재는 그룹을 다 읽은 뒤 맵을 한 번에 바꾼다
  (`CGroupMap::ReplaceDbGroups` — 즉석 세션 그룹은 같은 락 안에서 보존) — 재적재 중에도 그룹 조회(INVITE·affiliation)가 비지 않는다.
- 관제 앱 편집은 관리 범위 안 또는 소유(`authorized_user_id`) 그룹만 서버가 허용한다
  ([mcptt_authorization.md](mcptt_authorization.md)).

## 3. MCPTT 그룹 정보 화면

**구성** — ① 필터(전체 · 활동 중 · 발언 중 · 긴급 허용, 각 개수) + 그룹명·ID·멤버 번호/이름 검색 ② 그룹 목록(상태 ·
발언자 · 멤버/등록/참여 수 · 오늘 세션 · 허용 배지) ③ 선택 그룹 상세 패널(그룹 속성 · 멤버별 등록/참여/세션 참가/발언 ·
오늘 이용 요약, [편집]→구성 › PTT 그룹 · [세션 이력]). 진행 중 세션·발언자는 5 초 주기로 다시 읽는다
(`PttGroupInfoPage.tsx`).

| 표시 | 원천 | 비고 |
|---|---|---|
| 그룹 속성 (유형 · 우선순위 · 동시 발언 · 소유자 · 조직 · 긴급/보안 플래그) | DB `ptt_groups` | 편집 화면과 같은 필드, 읽기 전용 |
| 서비스 (MCPTT · MCVideo) | DB `mcvideo_group_attrs` 행 유무 → 응답 `mcvideo` | 한 그룹 = 서비스 집합(TS 23.280 §3) — MCPTT 는 늘, MCVideo 는 그룹 문서에 MCVideo 몫이 있을 때([mcvideo.md](mcvideo.md) §5.1). 표가 없으면(마이그레이션 전) 전부 false |
| 멤버 · 역할(chair/participant) | DB `ptt_group_members` | |
| 멤버 참여(affiliation) | DB `ptt_affiliations` (CSP 가 쓴다) | TS 24.379 §9 — 활성 = `status='affiliated'` · 만료 전(CSP `IsAffiliated` 와 같은 조건) |
| 멤버 접속 | `ptt_subscriptions.register_time/logout_time` | [monitoring.md](monitoring.md) §1.2 등록 조건 |
| 상태(발언 중 · 세션 진행 · 유휴) · 현재 화자 | CSP 상태 파일(진행 중 세션·참가자) + CMP STATS `floor_holders` | [monitoring.md](monitoring.md) §1.4 |
| 오늘 세션 · 발언 있던 세션 | 1분 롤업 `by_group` `{sessions, talked}` | 발언 수·시간 등은 §5.2 확장 뒤 |

## 4. 단말 현황 화면 (MCPTT 관점)

MCPTT 서비스가 필요로 하는 단말 정보 — 누가 어떤 단말로 로그인·등록돼 어느 그룹에 참여 중인가 — 를 보여 준다
(콘솔 `PttTerminalsPage`, 10 초 재조회).

**구성** — ① 필터(전체 · 접속 중 · 미접속 / 단말 유형) + 이름·MCPTT 번호 검색 ② 단말 목록(이름 · 번호 · 단말 유형 ·
모델·OS·앱 버전 · 로그인 · 등록 · transport · 참여 그룹 수 · 최근 관측) ③ 선택 단말 상세 4영역.

| 영역 | 표시 | 원천 |
|---|---|---|
| 단말 정보 | 단말 유형 · 단말 ID(IMEI — 가림) · instance · 모델 · OS · 앱 · `User-Agent` · 코덱(오퍼 선호 순) · 능력 태그(Contact feature tag) · 처음/최근 관측, 이 번호로 관측된 단말이 여럿이면 목록 | file-store `ue_devices`(§4.1) |
| MCPTT 서비스 상태 | 로그인(IdMS client_id · 발급 · 만료) | IdMS refresh token(`refresh_tokens` — 폐기·회전 안 됐고 만료 전인 최신) |
| | 등록(접속 여부 · 등록/해제 시각 · 접속 노드 · transport · 단말 주소 · 부여 만료) | DB `register_time`/`logout_time` + 최근 단말 레코드 |
| | 문서 구독(GMS 그룹 문서 · CMS 설정 문서 xcap-diff — 구독 중·만료 시각 / 해지 / 만료) | `ue_devices` 의 `subscriptions`(§4.1) |
| | 보안 · 인증(`sip_transport`(없으면 ANY) · `auth_scheme` · 접속서비스) | DB `ptt_subscriptions` |
| 그룹 참여 | 긴급 그룹(TS 24.484 사용자 프로파일 — 전용 긴급 그룹이면 그 그룹과 상시 참여 = affiliation 여부, 긴급 호출·경보 인가) · 멤버인 그룹별 역할 · affiliation · 참여 시각 | DB `ptt_user_profile`·`ptt_group_members`·`ptt_affiliations`(V1 과 같은 활성 조건) |
| 오늘 이용 | 세션 · 발언 수 · 발언 시간 · 긴급 | 1분 롤업 `by_user` (§5.2) |

단말 유형은 입력 필드가 아니라 로그인한 앱에서 파생한다. 지금 단말 앱은 모두 IdMS `client_id` `MCPTT_UE` 를 쓰므로 가를 수
있는 것은 `User-Agent` 의 product 토큰이다([ue_sdk.md](ue_sdk.md) §4.2 `userAgentOf`): `CIMS-Dispatch` = 관제조작반,
`CIMS-PTT`·`CIMS-VoLTE` = 휴대 단말, `CIMS-UE`(cimsue-cli)·`csim`·`cspsim` = 시험 단말, 그 밖 = 기타. 차상 단말은 그 앱의
product 이름이 정해지면 같은 표(`stats.py` `_TERMINAL_TYPES`)에 한 줄을 더한다.

### 4.1 단말 속성 수집

규격 경로로 받는다 — 앱 전용 보고 API 를 두지 않는다.

| 속성 | 규격 경로 | 수집 지점 |
|---|---|---|
| 단말 ID (IMEI) | REGISTER Contact `+sip.instance="<urn:gsma:imei:…>"` (TS 24.229 §5.1.1.2 · RFC 7254) | CSP `CscfModule` REGISTER 200 뒤 |
| 모델 · OS · 앱 버전 | REGISTER `User-Agent` (RFC 3261 §20.41) — 형식 `CIMS-PTT/<앱 버전> (<OS>; <모델>)` | 같은 지점 |
| 도달 경로 | 수신 transport · 소스 주소 · 부여 Expires · 접속 노드 | 같은 지점 |
| 능력 태그 | REGISTER Contact feature tag (RFC 3840 — `audio`·`video`·`+g.3gpp.mcptt`·`+g.3gpp.icsi-ref=…`) | 같은 지점 |
| 코덱 능력 | 그 단말이 낸 INVITE 오퍼의 rtpmap (RFC 4566 — 오디오 선호 순·영상) | CSP `EventIncomingCall`(등록 가입자 발신만) |
| 문서 구독 | GMS·CMS SUBSCRIBE `Event: xcap-diff` 수락·해지 (TS 24.481·24.484, RFC 5875) | CSP `RecvRequestSubscribe` |

```
REGISTER ─► CSP CscfModule  200 OK 뒤 _NoteDeviceSeen ─► CCallDir::DeviceSeen
               (줄 조립만 — 저장소 무접촉, 변경·1 시간 간격만)      │ StoreOpWriter worker (비동기 append)
                                                                  ▼
                                               {stats}/ue_devices/YYYYMMDD.jsonl
                                                                  │ oam-svc 주기 루프(롤업과 같은 주기) — ue_devices.fold
                                                                  ▼   파일별 바이트 오프셋 커서, 새 줄만
                          file-store  modules/oam-svc/runtime/ue_devices/<번호>__<instance>.json
                                                                  │
                          GET /api/v1/stats/service/ptt-terminals[/{msisdn}] ─► 콘솔 서비스 › 단말 현황
```

- **CSP 관측 줄** (`stats/ue_devices/<일>.jsonl`, [site_directory_layout.md](site_directory_layout.md) 통계 영역) —
  `register|unregister {user, kind, instance, user_agent, transport, addr, expires, node, features}`(features 는 `;` 구분 — icsi-ref 값 안에 쉼표가 있다) ·
  `subscribe {user, package: gms|cms, expires(0 = 해지)}` · `media {user, audio, video}`. REGISTER 경로는
  줄 조립만 하고 기록은 호 이력과 같은 worker 가 한다(호 처리 경로 저장소 무조회 원칙,
  [volte_supplementary_services.md](volte_supplementary_services.md) §2). 갱신 REGISTER 마다 쓰지 않고 **단말·앱·경로가
  달라졌을 때** 또는 같은 값이 1 시간(`kDeviceSeenRefreshSec`)을 넘겼을 때만 쓴다 — 줄 수가 갱신 주기가 아니라 등록 수에
  비례하고, 대신 `last_seen` 해상도가 1 시간이다. 구독 갱신은 **부여 만료의 절반**(60 s~1 시간) 간격으로만 쓴다 — 그래야
  OAM 이 `마지막 수락 + 만료` 로 본 구독이 갱신 사이에 끊긴 것처럼 보이지 않는다. 코덱은 값이 바뀔 때·1 시간 간격. 명시적 해제·해지는 늘 쓴다.
- **레코드** (`ue_devices`) — 키 = 불변 id `<가입 번호>__<instance>`([../identifier_model.md](../identifier_model.md)), instance 가
  없는 단말은 `<번호>__-`. 필드 `subscription_id` · `kind` · `instance_id` · `imei` · `app` · `app_version` · `os` · `model` ·
  `user_agent` · `transport` · `addr` · `node` · `expires` · `registered` · `first_seen` · `last_seen` · `last_register` ·
  `last_unregister` · `features` · `codecs{audio, video, seen}` · `subscriptions{gms|cms: {since, last, expires, ended}}`.
  구독·코덱 줄은 그 번호의 지금 단말(등록 중 → 최근 관측 → 없으면 `<번호>__-`)에 붙는다. 구독 활성 = `last + expires` 가 지금보다 뒤이고 해지 전.
  같은 단말의 재등록은 `last_seen` 만 밀고, 같은 번호에 다른 단말이 등록하면 이전 레코드는 `registered=false`
  (바인딩은 번호당 하나 — [registration_binding_set.md](registration_binding_set.md) §8), 해제 줄은 그 번호의 등록 중 레코드를 모두 내린다.
- **저장 위치** — oam-svc 소유 공간 `modules/oam-svc/runtime/`([../runtime_store_v2_module_namespacing.md](../runtime_store_v2_module_namespacing.md)).
  관리 store 는 단일 writer 라 oam-svc 가 기동 때 이 서브트리에 소유권 리스를 잡는다(계측기와 같은 규약, [oam_ha.md](oam_ha.md) §4.4).
  못 잡으면 접기만 멈추고 조회는 된다. 레코드는 관측 줄에서 다시 만들 수 있는 파생 데이터다(커서 `.fold_cursor.json` 을 지우면 남은 줄로 재구성).
- 단말이 싣는 값([ue_sdk.md](ue_sdk.md) §4.2 단말 속성):

  | 단말 | `User-Agent` | `+sip.instance` |
  |---|---|---|
  | Android PTT·VoLTE(`android/core` `DeviceIdentity`) | `CIMS-PTT/<버전> (Android <판>; <모델>)` · `CIMS-VoLTE/…` | `urn:uuid:` — ANDROID_ID 이름 기반 UUID(RFC 4122 v3). 일반 앱은 Android 10 부터 IMEI 를 못 읽는다(READ_PRIVILEGED_PHONE_STATE). ANDROID_ID 는 서명 키·사용자·기기 단위라 같은 기기의 PTT·VoLTE 가 같은 값 |
  | Windows 관제(`windows/dispatch-desktop`) | `CIMS-Dispatch/<버전> (Windows 11 25H2; <BIOS 모델>)` — .NET `Platform.DeviceIdentity.UserAgent`(OS = 빌드·표시 판, 모델 = BIOS `SystemProductName`, 형식은 코어 `userAgentOf`) | `urn:uuid:` — 기기 GUID(`MachineGuid`) 이름 기반 UUID v3, Android 와 같은 `cims-ue:` 규칙(`DeviceIdentity.InstanceUrn`). 재설치해도 같고 PTT·전화 계정이 같은 값 |
  | libcimsue 앱(관제 태블릿·cimsue-cli) | 앱이 `EngineConfig.userAgent` 를 `userAgentOf()` 형식으로 채운다 | 앱이 `AccountConfig.instanceId` 를 채운다 — IMEI 를 알면 `imeiUrn()`, 모르면 설치 고유 UUID URN. 비우면 pjsip 기본값(호스트명 해시 — 기기마다 같을 수 있다) |

  `userAgentOf` 는 괄호 안 값을 comment 규칙(RFC 3261 §25.1)으로 정리한다 — 괄호·역슬래시를 빼고 공백·제어 문자를 하나로 접으며, OS 의 `;` 도 뺀다
  (수집 쪽 `parse_user_agent` 는 OS 에 `;`·`)`, 모델에 `)` 가 없다고 본다 — 기기 문자열 `Standard PC (Q35 …)` 같은 값이 모델을 자르지 않게).
  TCP/TLS 등록은 RFC 5626 outbound 경로가 `reg-id` 와 함께, UDP 등록은 REGISTER Contact 에 직접 싣는다.
  수집 쪽은 URN 종류로 IMEI 칸을 채운다(`urn:gsma:imei:` 만 IMEI, `urn:uuid:` 는 빈 칸).
- IMEI 는 개인 식별 정보다 — 서버가 가운데를 가려 보내고(`3512…7890`, `imei_masked`), 원문은 콘솔 `admin` 역할에만 준다(`raw_imei`).

남은 것 = 차상 단말 product 이름(정해지면 `_TERMINAL_TYPES` 한 줄).

## 5. MCPTT 이용 정보 화면

**구성** — ① 기간(오늘 · 최근 7일 · 최근 30일 · 직접 지정, 최대 92일) ② 요약 타일(그룹 세션 · 발언 · 총 발언 시간 ·
긴급·임박 세션 · 영상 송출 세션 · 평균 개시 시간) ③ 발언 추이 막대(하루 = 시간대별 `1h`, 여러 날 = 일별 `1d` — 막대 툴팁 ·
[표로 보기] · 자료 없는 구간은 점선) · 그룹별 이용 표 · 사용자별 발언 상위(발언 시간순 50명) ④ Excel 내려받기.
발언 축을 못 잰 세션이 섞이면(`talk_measured < 세션 기록`) 경고 띠로 알리고 해당 타일에 "측정 세션 n건 기준" 을 붙인다
(`PttUsagePage.tsx`).

### 5.1 원천

[sip_statistics.md](sip_statistics.md) 의 1분 롤업 피라미드(1m → 1h → 1d → 1M)를 그대로 쓴다 — 이용 정보를
위해 원본(세션 디스크립터)을 기간마다 다시 훑지 않는다. 기간이 길면 거친 계층을 고른다(같은 문서 §7.2).

| 타일 · 표 | 롤업 필드 |
|---|---|
| 그룹 세션 | `call.sessions` (svc=ptt) |
| 평균 개시 시간 | `call.pdd_sum_ms / call.pdd_n` |
| 발언 · 총 발언 시간 · 긴급 · 영상 | **§5.2 확장 필드** |
| 시간대별 추이 | 1h 버킷의 `turns` |
| 그룹별 · 사용자별 | `by_group` · `by_user` |

### 5.2 롤업 확장

1분 레코드 `call` 에 PTT 발언 카운터를 더하고, 그룹 축을 넓히고, 사용자 축을 추가한다. 모두 합산 가능한
카운터다(비율·평균을 저장하지 않는 규약 유지).

```jsonc
"call": {
  // … 기존 필드 …
  "turns": 156, "talk_sum_sec": 860,        // 발언 수 · 발언 시간 합 (PTT)
  "emergency": 1, "video": 3,               // 긴급 · 임박 위험 세션 · 영상 송출 세션
  "by_group": { "g001": {"sessions": 42, "talked": 40, "turns": 156, "talk_sum_sec": 860,
                          "emergency": 1, "video": 3} },
  "by_user":  { "mcptt-021": {"sessions": 18, "turns": 42, "talk_sum_sec": 192, "emergency": 0} }
}
```

- 발언 원천 = 세션 인덱스(`services/ptt_index`) 행 — 녹취 세그먼트 슬롯 트랙의 화자 구간을 화자별로 모은 `by_speaker`
  `{id: {turns, talk_ms}}`, 세션 이벤트의 긴급·임박 개시(`emergency_activated`·`imminent_activated`) → `emergency`, 세그먼트의
  영상 트랙 → `video_sent`. 세션 시작 버킷에 귀속한다(sip_statistics §4.3 과 같은 규칙).
- 이 필드가 없는 인덱스 행(축 이전 세션)은 이용 카운터를 세지 않고 `talk_measured`(축을 잰 세션 수)에도 넣지 않는다 —
  조회가 그룹 축 세션 합과 비교해 미측정 구간을 알린다. 재집계(`POST /api/v1/stats/calls/rebuild`)가 인덱스를 다시 만들어 채운다.
- `by_user` 는 `by_group` 과 같은 원칙이다 — 그 분에 활성이던 사용자(세션 참여자 ∪ 화자)만 담아 키 수가 전체 가입자
  수와 무관하다. 키는 PTT 회선 번호, 표시명은 조회가 붙인다. 발언 없이 참여한 사용자는 `sessions` 만 는다.

### 5.3 내려받기

같은 조회 API 에 `format=xlsx` 를 붙인다(화면과 같은 숫자). 시트 = 요약 · 시간대(또는 일별) · 그룹별 · 사용자별.

## 6. API

oam-svc 가 소유한다 — 통계 롤업(`services/stats_rollup`)·세션 인덱스와 같은 곳이다. 경로는 oam-svc 가 이미 가진 게이트웨이
세그먼트(`/api/v1/stats/service`) 아래에 둔다 — `/api/v1/ptt/groups/*` 는 CSC 관리 API 세그먼트라 그 아래에 두면
`{id}` 와 겹치고(그룹 id `status`), 그룹 상세는 oam-svc 로 라우팅할 수 없다.

| 메서드 · 경로 | 용도 | 상태 |
|---|---|---|
| `GET /api/v1/stats/service/ptt-groups?state=all\|active\|talking\|emergency&q=` | 그룹 목록 + 상태 · 등록/참여 인원 · 오늘 요약 (`counts` = 필터별 수) | 구현 (`stats.service.ptt-groups`) |
| `GET /api/v1/stats/service/ptt-groups/{id}` | 그룹 상세 — 속성 · 멤버별 등록/참여/세션 참가/발언 | 구현 (`stats.service.ptt-group`) |
| `GET /api/v1/stats/service/ptt-terminals?state=all\|online\|offline&type=&q=` | 단말 목록 — `counts`(상태별) · `types`(유형별) · `terminals[]` | 구현 (`stats.service.ptt-terminals`) |
| `GET /api/v1/stats/service/ptt-terminals/{msisdn}` | 단말 상세 4영역 — `device`·`devices[]`·`login`·`registration`·`security`·`groups[]`·`today` (IMEI 원문은 admin) | 구현 (`stats.service.ptt-terminal`) |
| `GET /api/v1/stats/service/ptt-usage?from=&to=&unit=1h\|1d&format=json\|xlsx` | 이용 정보 — `summary`(+ `talk_measured`/`talk_coverage_sessions`) · `trend` · `by_group` · `by_user`(상위 50) | 구현 (`stats.service.ptt-usage`) |

## 7. 구현 순서

| 단계 | 내용 | 선행 |
|---|---|---|
| V1 | MCPTT 그룹 정보 화면 — 기존 데이터(DB · affiliation probe · CMP 그룹 상태 · `by_group`) | 없음 |
| V2 | 롤업 확장(§5.2) + MCPTT 이용 정보 화면 + xlsx | sip_statistics 롤업 |
| V3 | 단말 속성 수집(§4.1 — REGISTER 파싱 · `ue_devices` · SDK `User-Agent`/IMEI URN) + 단말 현황 화면 | SDK 반영 |

검증 = 롤업 확장 단위시험(발언 · 그룹 · 사용자 합산이 상위 단위에서 보존) · 단말 속성 접기·`User-Agent` 파싱·IMEI 가림 단위시험
(`tests/test_ue_devices.py`, S1 OAM 통계) · 계측기 PTT 시나리오 부하 뒤 이용 정보 숫자와 세션 이력 건수 대조(S3).
