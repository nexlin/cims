# MCPTT 관리 조회 화면 — 그룹 정보 · 단말 현황 · 이용 정보

> **설계 정본 — 화면 3종·단말 속성 수집·이용 집계 확장은 미구현.** 그룹 편집과 실시간 반영(§2)·PTT 세션
> 이력은 구현돼 있다. 요구 = "MCPTT 그룹 정보 · 단말기 정보 · MCPTT 이용 정보를 저장하며, 필요시 저장 내용을
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
| **MCPTT 그룹 정보** (조회 · 상태) | `서비스 > MCPTT 그룹 정보` (`/service/ptt-groups`) | 미구현 (§3) |
| **단말 현황** (MCPTT 관점 단말 정보) | `서비스 > 단말 현황` (`/service/ptt-terminals`) | 미구현 (§4) — 단말 속성 수집 신규 |
| **MCPTT 이용 정보** (기간 집계) | `서비스 > MCPTT 이용 정보` (`/service/ptt-usage`) | 미구현 (§5) — 롤업 확장 |

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
                                                      │ ④ xcap-diff NOTIFY (그룹 멤버 단말)
                                                      ▼
                                   단말 ⑤ GMS 문서 재조회 → 그룹 목록 · 멤버 갱신
```

- 재시작·재로그인 없이 반영된다. 통지 유실은 PSP `SyncGroupsState`(60초 해시 비교)가 따라잡는다
  ([../modules/csc.md](../modules/csc.md) §5.1).
- 관제 앱 편집은 관리 범위 안 또는 소유(`authorized_user_id`) 그룹만 서버가 허용한다
  ([mcptt_authorization.md](mcptt_authorization.md)).

## 3. MCPTT 그룹 정보 화면

**구성** — ① 필터(전체 · 활동 중 · 긴급 허용) + 그룹명·ID·멤버 검색 ② 그룹 목록 ③ 선택 그룹 상세(그룹 속성 ·
멤버·참여 상태 · 오늘 이용 요약).

| 표시 | 원천 | 비고 |
|---|---|---|
| 그룹 속성 (유형 · 우선순위 · 동시 발언 · 소유자 · 조직 · 긴급/영상/보안 플래그) | DB `ptt_groups` | 편집 화면과 같은 필드, 읽기 전용 |
| 멤버 · 역할(chair/participant) | DB `ptt_group_members` | |
| 멤버 참여(affiliation) | PSP affiliation 상태 probe | TS 24.379 §9 — `affiliated` / 미참여 |
| 멤버 접속 | `ptt_subscriptions.register_time/logout_time` | [monitoring.md](monitoring.md) §1.2 등록 조건 |
| 상태(발언 중 · 대기 · 유휴) · 현재 화자 | CMP 그룹 상태 | [monitoring.md](monitoring.md) §1.4 |
| 오늘 세션 · 최근 활동 · 이용 요약 | 1분 롤업 `by_group` (§5.2) | |

## 4. 단말 현황 화면 (MCPTT 관점)

MCPTT 서비스가 필요로 하는 단말 정보 — 누가 어떤 단말로 로그인·등록돼 어느 그룹에 참여 중인가 — 를 보여 준다.

**구성** — ① 필터(전체 · 접속 중 · 미접속, 단말 유형) + 이름·MCPTT ID 검색 ② 단말 목록(이름 · MCPTT ID · 단말
유형 · 모델·앱 버전 · 로그인 · 등록 · 참여 그룹 수 · 최근 등록) ③ 선택 단말 상세 4영역.

| 영역 | 표시 | 원천 | 상태 |
|---|---|---|---|
| 단말 정보 | 단말 ID(IMEI) · 모델 · OS · 앱 버전 · 코덱 | **§4.1 단말 속성** | 신규 수집 |
| MCPTT 서비스 상태 | 로그인(IdMS 토큰 발급·만료) | CSC `idms_storage` | 있음 |
| | 등록(접속 PSP 노드 · transport · 단말 주소 · 등록 시각 · 만료) | CSP 등록 바인딩 + DB `register_time` | 있음 |
| | 문서 구독(GMS · CMS xcap-diff) | CSP 구독 관리 | 있음 |
| | 보안 · 인증(`sip_transport` · `media_srtp` · `auth_scheme`) | DB 가입 · 접속서비스 | 있음 |
| 그룹 참여 | 그룹별 affiliation · 철도 긴급 그룹 상시 참여 | PSP affiliation 상태 | 있음 |
| 최근 이용 | 오늘 세션 · 발언 · 최근 세션 · 긴급 · 영상 | 1분 롤업 `by_user` (§5.2) | 롤업 확장 |

단말 유형(관제조작반 · 차상 단말 · 휴대 단말)은 로그인 클라이언트(IdMS `client_id`)에서 파생한다 — 별도 입력
필드를 두지 않는다.

### 4.1 단말 속성 수집

규격 경로로 받는다 — 앱 전용 보고 API 를 새로 두지 않는다.

| 속성 | 규격 경로 | 수집 지점 |
|---|---|---|
| 단말 ID (IMEI) | REGISTER Contact `+sip.instance="<urn:gsma:imei:…>"` (TS 24.229 §5.1.1.2 · RFC 7254) | PSP · CSP REGISTER 처리 |
| 모델 · OS · 앱 버전 | REGISTER `User-Agent` (RFC 3261 §20.41) — 형식 `CIMS-PTT/<앱 버전> (<OS>; <모델>)` | 같은 지점 |
| 코덱 능력 | REGISTER · INVITE SDP / Contact feature tag | 같은 지점 |

- 저장 = file-store collection `ue_devices`(설계안 — 신규 데이터는 DB 테이블이 아니라 file-store 로 시작,
  [../runtime_store_design.md](../runtime_store_design.md)): `subscription_id` · `instance_id` · `imei` · `model` ·
  `os` · `app_version` · `user_agent` · `first_seen` · `last_seen`. 키는 불변 id(`subscription_id` + `instance_id`) —
  [../identifier_model.md](../identifier_model.md). REGISTER 경로에서 동기 쓰기하지 않는다 — 변경분만 비동기
  upsert(호 처리 경로 저장소 무조회 원칙, [volte_supplementary_services.md](volte_supplementary_services.md) §2).
- 단말(libcimsue)은 `+sip.instance` 를 이미 보낸다([registration_binding_set.md](registration_binding_set.md));
  IMEI URN 형식과 `User-Agent` 형식을 SDK 에서 맞춘다([ue_sdk.md](ue_sdk.md)).
- IMEI 는 개인 식별 정보다 — 화면은 가운데를 가려 표시하고(`3512…7890`), 원문 조회는 관리자 역할로 한정한다.

## 5. MCPTT 이용 정보 화면

**구성** — ① 기간(오늘 · 최근 7일 · 최근 30일 · 직접 지정) ② 요약 타일(그룹 세션 · 발언 · 총 발언 시간 · 긴급
호출 · 영상 송출 · 평균 개시 시간) ③ 시간대별 발언 추이 · 그룹별 이용 표 · 사용자별 발언 상위 ④ Excel 내려받기.

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

- 발언 원천 = 세션 디스크립터의 floor 이력(화자 구간) — 세션 종료 시 확정되므로 `invite_time` 버킷에 귀속한다
  (sip_statistics §4.3 과 같은 규칙).
- `by_user` 는 `by_group` 과 같은 원칙이다 — 그 분에 활성이던 사용자만 담아 키 수가 전체 가입자 수와 무관하다.
  표시 식별자는 MCPTT ID.

### 5.3 내려받기

같은 조회 API 에 `format=xlsx` 를 붙인다(화면과 같은 숫자). 시트 = 요약 · 시간대 · 그룹별 · 사용자별.

## 6. API (제안)

oam-svc 가 소유한다 — 통계 롤업(`services/stats_rollup`)·세션 인덱스와 같은 곳이다.

| 메서드 · 경로 | 용도 |
|---|---|
| `GET /api/v1/ptt/groups/status?state=&q=` | 그룹 목록 + 상태 · 오늘 요약 |
| `GET /api/v1/ptt/groups/{id}/status` | 그룹 상세 — 멤버별 참여 · 접속 |
| `GET /api/v1/ptt/terminals?state=&type=&q=` | 단말 목록 |
| `GET /api/v1/ptt/terminals/{mcptt_id}` | 단말 상세 4영역 |
| `GET /api/v1/stats/ptt/usage?from=&to=&unit=&format=json\|xlsx` | 이용 정보 |

## 7. 구현 순서

| 단계 | 내용 | 선행 |
|---|---|---|
| V1 | MCPTT 그룹 정보 화면 — 기존 데이터(DB · affiliation probe · CMP 그룹 상태 · `by_group`) | 없음 |
| V2 | 롤업 확장(§5.2) + MCPTT 이용 정보 화면 + xlsx | sip_statistics 롤업 |
| V3 | 단말 속성 수집(§4.1 — REGISTER 파싱 · `ue_devices` · SDK `User-Agent`/IMEI URN) + 단말 현황 화면 | SDK 반영 |

검증 = 롤업 확장 단위시험(발언 · 그룹 · 사용자 합산이 상위 단위에서 보존) · REGISTER `User-Agent`/`+sip.instance`
파싱 단위시험 · 계측기 PTT 시나리오 부하 뒤 이용 정보 숫자와 세션 이력 건수 대조(S3).
