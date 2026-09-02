# SIP 호·메시지 통계

유입 시간대별 SIP 호 통계(성공률·소통률·완료율·참여율)와 SIP 메시지 통계(메시지별 인입
건수)를 서비스(VoLTE/PTT)별로 산출한다. 시간 단위는 분·시간·일·주·월.

집계 원천은 두 갈래로 나눈다 — **호 통계는 호 이력, 메시지 통계는 SIP 원문**. 같은 사실을
두 번 계산하지 않기 위한 경계이며, 그 근거는 §3 에 있다.

## 1. 세는 단위 — 3계층

기능에 따라 "호 1건"의 경계가 달라진다. 그룹통화는 발신 1건에 INVITE 가 멤버 수만큼 나가고,
감청·착신전환·당겨받기도 leg 을 늘린다. **SIP 메시지나 dialog 를 세면 분모가 기능에 따라
부풀어 비율이 무의미해진다.** 그래서 세는 단위를 분리한다.

```
① 호 시도 (attempt)    가입자의 발신 의도 1건       ← 성공률·소통률·완료율 분모
     │
     └ ② 세션 (session)  성립한 통화 1건             ← 동시통화·자원 점유
           │
           └ ③ leg      SIP dialog 각각              ← 참여율·실패 위치
```

### 1.1 기능별 매핑

| 기능 | 호 시도 | 세션 | leg | 비고 |
|---|---|---|---|---|
| 1:1 통화 | 1 | 1 | 2 | 기준 |
| 그룹통화 (멤버 N) | 1 | 1 | 1+N | 발신 1 + 착신 N (fan-out) |
| 감청 | 1 | 1 | +1 | **감청 leg 은 시도·세션 지표에서 제외** |
| 착신전환 | 1 | 1 | 2~3 | 원착신 실패 + 전환착신 성공 = 시도 1·성공 1 |
| 당겨받기 | 1 | 1 | 2~3 | 받은 주체가 달라도 시도는 1 |
| 헌트그룹 (순차착신) | 1 | 1 | N | 순차 INVITE 전부 한 시도 |
| 3자통화·전환(REFER) | 1 | 재구성 | +N | §9 미결 |

감청만 성질이 다르다 — 나머지는 *한 의도를 여러 leg 으로 시도*하는 것이지만, 감청은
**의도와 무관하게 시스템이 붙이는 leg** 이다. 이를 사후에 구분할 수단이 없으면 감청을 켠
순간 모든 비율이 부풀고 되돌릴 수 없다. 그래서 leg 에 역할을 남긴다.

### 1.2 leg 역할 (`role`)

| 값 | 뜻 | 시도 분모 | 참여율 분모 |
|---|---|---|---|
| `originating` | 발신 leg — 시도의 주체 | 포함 | 제외 |
| `terminating` | 착신 leg (그룹 fan-out 포함) | — | **포함** |
| `forwarded` | 착신전환으로 생긴 leg | — | 포함 |
| `pickup` | 당겨받기 leg | — | 포함 |
| `intercept` | 감청 leg | **제외** | **제외** |

감청·PBX 계열(`forwarded`/`pickup`/`intercept`)은 기능 구현 예정이다. 데이터 모델과 집계는
지금 이 어휘를 수용하도록 열어 두고, 실제 기록은 각 기능 구현 시 채운다. **미기록 leg 은
`terminating` 으로 취급**하므로 기존 동작이 깨지지 않는다.

## 2. 지표 정의

### 2.1 호 통계 — 시도(attempt) 기준

```
성공률 = 세션이 성립한 시도 / 전체 시도
소통률 = 실제 통화가 있었던 시도 / 전체 시도
완료율 = 정상 종료한 시도 / 세션이 성립한 시도
```

**호 시도에서 제외하는 것은 없다.** 재호출·CANCEL·미응답(408/480)·거절 전부 시도로 계상하고
실패 사유만 분리한다(§2.3). 다만 SIP 프로토콜 상의 중복은 시도가 아니다 — 인증 챌린지
재전송(401/407 후 재INVITE), UDP 재전송, re-INVITE(hold/코덱 변경)는 **같은 시도 1건**이다.
집계 원천이 호 이력(레코드 1건 = 호 1건)이므로 이 중복은 원천에서 이미 합쳐져 있다.

판정 근거는 서비스별로 다르다.

| 지표 | VoLTE | PTT (세션 단위) |
|---|---|---|
| 세션 성립 | `answer_time != null` | 멤버 1명 이상 join |
| 실제 통화 | `duration > 0` | **`turn_count > 0`** |
| 정상 종료 | `end_reason == "normal"` | 정상 종료 (§8 선행) |

**PTT 의 소통률 판정이 `turn_count > 0` 인 것이 핵심이다.** 세션은 수립됐는데 아무도 발언을
못 한 상태 — floor control 장애 — 가 여기서만 드러난다. 성공률로는 정상으로 보인다.
VoLTE 의 `answer_time` 있음 + `duration == 0`(붙었으나 미디어 없음)이 같은 성질이다.

### 2.2 참여율 — leg 기준

```
참여율 = join 한 leg / 초대한 leg      (role: terminating·forwarded·pickup)
```

그룹통화에서 "멤버 몇 명이 실제로 붙었나"는 성공률과 **다른 정보**다. 멤버 1명만 붙어도
세션은 서므로 성공률은 100% 인데 참여율은 25% 일 수 있다. 한 칸에 섞으면 두 정보가 모두
망가지므로 별도 지표로 둔다.

### 2.3 실패 사유 분해

호 이력의 `end_reason` 어휘를 그대로 쓴다.

| 값 | 뜻 |
|---|---|
| `normal` | 정상 종료 |
| `canceled` | 발신자 포기 (CANCEL) — §8 선행 |
| `no_answer` | 무응답 |
| `busy` | 통화중 |
| `rejected` | 거절 |
| `error` | 오류 |
| `timeout` | 시간초과 |
| `incomplete` | 비정상 종료 (기록 없음) |

응답코드 단위(4xx/5xx/6xx 별) 분해는 호 이력에 없다 — 메시지 통계 축(§2.4)에서 제공한다.
두 축이 역할을 나눠 가진다: **호 통계는 "몇 건이 어떻게 끝났나", 메시지 통계는 "어떤 응답이
얼마나 나갔나"**.

### 2.4 메시지 통계

SIP 원문 1건 = 1 카운트. 방향(`dir`)·메서드/응답코드·서비스로 분류한다.

- 요청: `_SIP_REQUEST_METHODS` 14종 (INVITE·ACK·BYE·CANCEL·OPTIONS·REGISTER·PRACK·
  SUBSCRIBE·NOTIFY·PUBLISH·INFO·REFER·MESSAGE·UPDATE)
- 응답: 상태코드 (`200`·`401`·`180` …). 계열(2xx/4xx…) 집계는 조회 시 합산.

## 3. 원천

| 통계 | 원천 | 위치 |
|---|---|---|
| 호 (VoLTE) | `call.json` | `{ServiceLogging.Dir}/volte/YYYY/MM/DD/HH/…/{key}.d/call.json` |
| 호 (PTT) | 세션 디스크립터 | `ptt/{gid}/YYYY/MM/DD/HH/{sessKey}/session.json` (`services.ptt_index` 경유) |
| leg 묶음 | `session.json` | `{"session_id":…, "sesid":…, "call_ids":[…]}` |
| 메시지 | SIP 원문 JSONL | `{ServiceLogging.Dir}/YYYY/MM/DD/HH/{sys}_{iface}.msg.{5분버킷}.jsonl` |

**SIP 원문으로 호를 재구성하지 않는다.** `sesid` 로 묶어 상태기계를 다시 돌리면 같은 사실을
두 번 계산하는 구조가 되고, 콘솔 `호 이력` 화면과 숫자가 어긋난다. PTT 집계가 디렉터리를
직접 훑지 않고 `services.ptt_index` 를 쓰는 것과 같은 이유다.

원문 레코드 키: `ts · dir · peer · caller · callee · sesid · proto · msg`. `proto` 에는
`SIP` 외에 `CSC`(내부 HTTP)·`JSON`(CMP 제어)·`HTTPS` 가 섞여 들어오므로 **인터페이스로 먼저
분리**한다(기존 `/stats/messages/{sip|cmp|csc|https}` 축과 동일).

### 3.1 서비스 판정

`access_services` 의 `domain → kind` 로 SIP URI 를 분류한다. 순서는 CSP 와 동일하게
Request-URI → To → From, 첫 매치. 응답은 Request-URI 가 없으므로 To/From 만 본다.

이 판정이 서비스축(요구 ⑤) 전체의 근거다. **현재 깨져 있다 — §8 X1 참조.**

## 4. 시간 축

### 4.1 롤업 피라미드 — 1분 기저

```
SIP 원문 / 호 이력          ← 원본. 조회에 직접 쓰지 않는다
      │ 집계 (원본을 읽는 유일한 단계)
      ▼
   [1m]  1440/일
      ├→ [5m]   1m×5
      ├→ [10m]  1m×10
      └→ [1h]   1m×60
            └→ [1d]  1h×24
                  ├→ [1w]  1d×7
                  └→ [1M]  1d×해당월일수
```

**모든 단위가 1분의 정수배 경로로 유도된다 — 근사치가 없다.** 기저를 5분으로 잡으면 1분을
영원히 만들 수 없으므로 되돌릴 수 없는 선택을 피해 1분으로 둔다.

원본 크기가 조회 비용과 무관해지는 것이 이 구조의 이점이다. 기존 `_GRAN_MAX_DAYS`
(5m=3일/10m=7일/1h=30일/1d=730일) 같은 조회 범위 제한이 필요 없어진다 — 제한은 계층별
보존기간(§5.3)으로만 남는다.

### 4.2 경계

- **타임존 KST 고정.** 운영자는 로컬 시간으로 사고하며, 한국은 DST 가 없어 경계가 단순하다.
- **주는 일요일 시작.**
- 월은 달력 그대로 (1일 00:00:00 ~ 말일 23:59:59).
- 버킷 라벨은 **버킷 시작 시각**을 가리킨다. `[15:59]` = `15:59:00 ~ 15:59:59`.

### 4.3 버킷 귀속 시각

**호는 `invite_time`(유입 시각) 기준으로 버킷에 귀속한다.** 요구가 "유입 시간대별"이므로
15:59 에 들어와 16:05 에 끝난 호는 `[15:59]` 버킷 소속이다.

메시지는 레코드의 `ts` 기준이다.

## 5. 데이터 모델

### 5.1 1분 집계 레코드

계층별로 같은 스키마를 쓴다 — 롤업이 단순 합산이 되도록.

```jsonc
{
  "bucket": "2026-09-02 15:59",        // 버킷 시작 (KST)
  "unit": "1m",
  "svc": "ptt",                         // volte | ptt | unknown
  "call": {                             // 호 통계 — 시도 기준
    "attempts": 12,
    "sessions": 11,                     // 세션 성립 (성공률 분자)
    "talked": 10,                       // 실제 통화 (소통률 분자)
    "completed": 9,                     // 정상 종료 (완료율 분자)
    "reasons": {"normal": 9, "no_answer": 2, "canceled": 1},
    "duration_sum_sec": 842,            // 평균은 조회 시 계산 (합산 가능성 유지)
    "pdd_sum_ms": 21400, "pdd_n": 11,   // answer_time - invite_time
    "legs_invited": 44, "legs_joined": 39   // 참여율
  },
  "msg": {                              // 메시지 통계
    "in":  {"INVITE": 8, "200": 130, "NOTIFY": 76},
    "out": {"200": 126, "180": 6}
  },
  "open": 3,                            // 이 버킷에 시작해 아직 미결인 호 (§6)
  "late_dropped": 0                     // 되짚기 실패로 누락된 호 (§6)
}
```

**비율을 저장하지 않고 분자·분모만 저장한다.** 비율은 합산이 불가능해서(5분 = 1분 비율 5개의
평균이 아니다) 롤업이 성립하지 않는다. 평균값(`avg_duration`)도 같은 이유로 합(`sum`)과
건수(`n`)로 나눠 둔다.

### 5.2 저장 위치

```
{ServiceLogging.Dir}/stats/{1m,1h,1d}/YYYY/MM/DD.jsonl
```

`alerts/`·`events/` 와 같은 자리다. **관리 store(`CimsRuntimeDir`)에 두지 않는다** — 관리
store 는 이중화에서 단일 writer 리스 하에 있는 공유 자원이고, 통계는 append-only 관측
데이터이므로 서비스 로그와 같은 수명·권한 정책을 따르는 것이 맞다(`oam_ha.md` §4.1 과 동일한
분리 근거).

5m·10m·1w·1M 은 저장하지 않는다 — 1m·1h·1d 에서 조회 시 합산한다. 저장 계층을 늘리면 롤업
경로와 보존 정책이 함께 늘어나는데, 합산 비용은 무시할 수준이다.

### 5.3 보존

| 키 | 대상 | 잠정 기본값 |
|---|---|---|
| `ServiceLogging.StatsRetainDays.1m` | 1분 집계 | 14 |
| `ServiceLogging.StatsRetainDays.1h` | 시간 집계 | 400 (365 하한 클램프) |
| `ServiceLogging.StatsRetainDays.1d` | 일 집계 | 0 (무제한) |

`0` 은 무제한(`daily_jsonl.purge_old` 규약). 1시간 계층에 **365일 하한 클램프**를 두는 이유는
전년 동월 비교가 깨지지 않게 하기 위함이며, 알람 스트림의 90일 클램프와 같은 방식이다.

기본값은 **잠정이다.** 실트래픽 관측 후 조정한다 — 통계는 카운터라 호가 늘어도 크기가 거의
변하지 않지만(버킷 수 고정), 확정에는 실측이 필요하다.

> **원문·녹취 보존기간은 별건이며 현재 존재하지 않는다.** purge 대상은 metric(3일)·job(2일)·
> alerts(180일)·events(365일) 넷뿐이고, SIP 원문(`*.msg.*`/`*.flow.*`)과 녹취는 무제한이다.
> 부피가 트래픽에 정비례하는 쪽에 정책이 없다. 통계 계층이 생기면 원문을 장기 보관할 이유가
> 줄어들므로 함께 정하는 것이 자연스럽다 — 이 문서의 범위 밖이지만 선후 관계가 있다.

## 6. 롤업 알고리즘

### 6.1 미결 호 추적

호 이력은 **호가 끝나야** 완성된다(`duration`·`end_reason` 이 종료 시 채워짐). 그런데 버킷
귀속은 `invite_time` 기준이므로, 긴 통화는 자기 버킷이 이미 집계된 뒤에 완성된다. 되짚지
않으면 **그 호는 통계에서 영구히 누락되고, 긴 통화일수록 많이 누락되어 통계가 실제보다 나쁘게
나온다.**

고정 시간창(예: 최근 60분 재계산)으로 풀지 않는다 — 창 값은 **최대 통화시간을 예측해야**
정할 수 있고, 예측할 수 없는 값을 설계 입력으로 삼으면 창을 넘는 통화가 조용히 누락된다.

대신 **미결 호를 추적한다.** `end_reason` 이 비어 있는 호가 곧 미결이다.

```
매 롤업:
  ① 미결 목록 확인
  ② 이번에 완성된 호 → 그 호의 invite_time 버킷만 재계산
  ③ 여전히 미결인 호는 목록에 유지 (버킷의 `open` 카운터로 노출)
```

통화 길이와 무관하게 정확하고, 재계산 대상이 **실제로 끝난 호가 있는 버킷으로 한정**되어
고정 창보다 비용이 작다. 비용 기준은 창 크기가 아니라 동시통화 수다.

미결 목록은 무한히 커지지 않는다 — CSP 가 아는 살아있는 호에 없는 기록을
`end_reason=incomplete` 로 마감하는 장치가 이미 있다(`flow_logger.py`). CSP 재기동으로
사라진 호도 여기서 걷힌다.

되짚기가 실패한 호(보존기간이 지나 버킷 파일이 이미 삭제된 경우 등)는 `late_dropped` 로
계상한다. **조용히 버리지 않는다** — 이 카운터가 계속 올라가면 1분 계층 보존기간이 짧다는
신호다.

### 6.2 실행 주체

oam-svc 에 주기 작업으로 둔다(통계는 oam-svc 귀속 — `oam_base_service_split` §4).
`services.ptt_index`(`PttIndex.Enabled`/`Interval`)가 녹취에서 세션 읽기 모델을 만드는 것과
같은 구조다.

| 키 | 뜻 | 기본값 |
|---|---|---|
| `StatsRollup.Enabled` | 롤업 수행 | `true` |
| `StatsRollup.Interval` | 주기(초) | 60 |

`Enabled=false` 면 조회가 원본 스캔으로 폴백한다(되돌리기 경로 — `PttIndex` 와 같은 규약).

## 7. API

### 7.1 버킷 키 스키마 통일

현재 응답은 단위에 따라 키가 갈린다 — 5m/10m/1h 는 `{"hour": 15}`, 1d 는
`{"date": "2026-09-02"}`. 단위가 7종으로 늘면 이 분기가 유지되지 않는다.

**모든 단위가 같은 키를 쓴다.**

```jsonc
{ "bucket": "2026-09-02 15:59",   // 표시 라벨 = 버킷 시작 (KST)
  "bucket_start": "2026-09-02T15:59:00+09:00" }
```

### 7.2 엔드포인트

```
GET /api/v1/stats/calls?from=&to=&granularity=&svc=
GET /api/v1/stats/messages/{iface}?from=&to=&granularity=&svc=
```

- `granularity`: `1m|5m|10m|1h|1d|1w|1M`
- `svc`: `volte|ptt|all` (기본 `all` — 서비스별 분리 + 합계 동시 반환)
- 응답에 분자·분모를 함께 실어 화면이 비율을 재계산할 수 있게 한다. 비율만 주면 구간 합산이
  불가능하다.

응답 예:

```jsonc
{ "from": "…", "to": "…", "granularity": "1h", "svc": "all",
  "totals": { "ptt": { "attempts": 120, "sessions": 118, "talked": 110,
                       "completed": 105, "legs_invited": 480, "legs_joined": 441,
                       "success_rate": 98.3, "talk_rate": 91.7,
                       "completion_rate": 89.0, "join_rate": 91.9 } },
  "buckets": [ { "bucket": "2026-09-02 15:00", "ptt": { … } } ] }
```

## 8. 선행 조건

이 통계를 세우기 전에 해소해야 하는 항목. 앞의 셋은 통계가 **동작하지 못하게** 막는다.

| | 내용 | 대상 |
|---|---|---|
| **X1** | OAM 이 `access_services.jsonl` 을 못 찾음 — `_access_services_paths()` 가 OAM 자기 트리만 훑는데 파일은 `modules/csp/<ver>/config/` 에 있다. 결과: **서비스 판정이 전부 `unknown` → 서비스축(요구 ⑤) 전체 무효** | OAM |
| **X2** | `AccessServicesFile` 이 `config_template.json` 미선언 — 콘솔로 우회 주입이 불가(`_prune_to_template` 이 버림) | OAM |
| **X3** | `_calc_voip_stats` 가 `answer_time` 을 읽지 않음 (필드는 `call.json` 에 이미 있다). 호 이력 API 응답 필드 목록에도 없음. **성공률·완료율이 계산 불가** | OAM |
| Y1 | `session.json` 의 `call_ids` 가 2개 고정(B2BUA 전제) → **N개**로 확장. 그룹 fan-out·감청·전환 leg 을 담지 못함 | CSP |
| Y2 | leg 별 `role` 기록 (§1.2) — 감청·PBX 구분 근거 | CSP |
| Y3 | 시도(attempt) 식별자 — 여러 leg 을 한 시도로 묶는 키. `sesid` 로 충분한지 확인 필요 | CSP |
| Y4 | `PttSessionEnd()` 에 종료 사유 인자 추가 — PTT 완료율의 유일한 결손 | CSP |
| Y5 | `canceled` 사유 분리 (현재 `no_answer`/`incomplete` 에 섞임) | CSP |

**X1·X3 은 코드 한 곳씩이다.** Y1~Y3 은 감청·PBX 기능 구현과 함께 간다 — 그 전까지 leg 은
전부 `terminating` 으로 취급되어 기존 동작이 유지된다.

## 9. 이행

기존 엔드포인트는 이 모델의 특수형이다.

| 기존 | 이행 |
|---|---|
| `/stats/messages/{iface}` (`date`·`granularity`, 버킷 `label`) | `svc` 파라미터 + 1m/1w/1M 추가, 버킷 키 통일. 기존 파라미터는 유지 |
| `/stats/service/voip` (`attempts`/`success`/`success_rate`) | `/stats/calls?svc=volte` 로 흡수. `success_rate` 는 `sessions/attempts` 와 같은 값 |
| `/stats/service/ptt` (`calls`) | `/stats/calls?svc=ptt`. `calls` = `attempts` |

`_GRAN_MAX_DAYS` 조회 범위 제한은 롤업 도입과 함께 제거한다(보존기간이 그 역할을 대체).

## 10. 미결

- **3자통화·전환(REFER)** 의 세션 재구성 — 세션 병합/분리를 시도 1건으로 유지할지, 새 시도로
  볼지. 기능 설계와 함께 정한다.
- **보존기간 확정** — 실트래픽 관측 후.
- **PTT 참여율의 분모** — 초대한 leg 기준(현 정의)과 affiliate 한 멤버 기준이 다를 수 있다.
  affiliation 은 세션과 무관하게 유지되므로 현 정의를 쓰되, 운영에서 후자가 필요하면 별도
  지표로 추가한다.

## 관련 문서

- [flow_logging.md](flow_logging.md) — SIP/Flow 로깅 (`sesid` 규칙·5분 버킷)
- [monitoring.md](monitoring.md) — 모니터링
- [recording.md](recording.md) — 녹취 (PTT 세션 이력·슬롯 트랙)
- [ptt_flows.md](ptt_flows.md) — PTT 그룹호 메시지 flow (fan-out 구조)
- [sip_service_model.md](sip_service_model.md) — `access_services` (서비스 판정 근거)
- [oam_base_service_split.md](oam_base_service_split.md) — 통계의 oam-svc 귀속
