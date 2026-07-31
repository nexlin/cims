# MCPTT 로드맵 기능 — CSP↔CMP 연동 메시지 규격

> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §0-R 미반영 로드맵의
> 구현을 2인(**Call Control & Signaling** / **Media Plane & Floor**)으로 분담하기 위한
> **CSP↔CMP 계약 정본**이다. wire 규약은 기존 [../../api/cmp_media_api.md](../../api/cmp_media_api.md)
> (UDP JSON envelope v2)를 그대로 확장하며, 여기서 정의한 명령/필드가 구현되면 그 정본은
> `cmp_media_api.md` 로 이관한다(본 문서는 착수 전 계약·분담 정본).

## 0. 설계 원칙 — 분담선

CMP 의 **floor 제어는 CMP↔UE in-band**(RTCP APP "MCPT", 그룹 공유 `floor_port`)로 동작하고
CSP 는 floor 루프에 들어가지 않는다. 따라서 두 파트의 경계는 자연히 다음과 같다.

| 파트 | 담당 | CSP↔CMP 메시지에서의 역할 |
|---|---|---|
| **A. Call Control & Signaling** (Dev A, `csp/`) | SIP 시그널링 → **세션 수명/토폴로지** 결정 | 세션 생성/변경/해제 요청과 그 payload(참여자·세션유형·정책 플래그)를 **발행** |
| **B. Media Plane & Floor** (Dev B, `cmp/`) | **floor 정책·미디어 분배·보안** 실행 | 정책 플래그를 받아 floor 동작을 구현, floor/미디어 상태를 **이벤트로 통지** |

원칙:
1. **정책은 세션 생성 시 1회 전달** — dual/multi-talker 같은 floor 동작은 CSP 가 in-band 로 개입하지
   않으므로, `PTT_GROUP_ADD/MODIFY` payload 의 **정책 필드**로만 전달하고 이후는 CMP 자율.
2. **기존 자원/멱등성 모델 재사용** — 새 function group 신설을 최소화하고 RELAY/PTT 를 확장한다
   (자원 키·재전송·`session_digest` audit 규칙 불변).
3. **SIP-only 는 CMP 메시지 없음** — 응답 타이밍/포킹/권한 판정 등은 시그널링 계층에서 끝난다.
   Dev B 스코프 밖임을 §A.4 에 명시한다.

---

## A. Call Control & Signaling 파트 (Dev A 발행)

세션의 **유형·참여자·수명**을 결정하는 요청. 모두 CSP 가 SIP `mcptt-info+xml`/포킹 결과를
근거로 발행한다.

### A.0 명령 사용 규약 — 생성 vs 변경, 서비스 경계

**(1) 생성=ADD 1회, 변경=MODIFY 로 통일.** `PTT_GROUP_ADD` 와 `PTT_GROUP_MODIFY` 는 그룹이
**없을 때의 동작만** 다르다 — ADD 는 생성(idempotent create), MODIFY 는 `NOT_FOUND`(재생성 안 함).
MODIFY 가 재생성을 막는 이유는 재생성 시 포트가 재할당되어 CSP 가 이미 SDP 로 광고한 포트와
어긋나기 때문이다([../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.2). 따라서 규약은:

> **최초 수립 → `PTT_GROUP_ADD`. 이후 모든 상태 변경(멤버 증감·우선순위·`floor_policy` 등)
> → `PTT_GROUP_MODIFY`. MODIFY 가 `NOT_FOUND` 면 그때만 `PTT_GROUP_ADD` 로 복구**(전체 상태
> 재전송 후 응답 포트로 캐시 갱신). 본 문서의 신규 필드도 모두 이 규약을 따른다.

**(2) MCPTT 세션은 `PTT_*` 로 통일, 서비스 경계는 명령으로 분리.** private call 을 포함한 모든
MCPTT 세션은 `PTT_GROUP_ADD` 하나를 파라미터로 구분한다(§A.1) — floor 유무·1:1/그룹은 payload
차이일 뿐 별도 명령이 아니다. 반면 **VoLTE `RELAY_*` 와 MCPTT `PTT_*` 는 합치지 않는다** —
자원 키(`(node,session_id)` node-전속 vs `(service,group_id)` service-공유)·격리 의미·PT 재작성
모델이 근본적으로 달라, 병합 시 필수필드가 조건부로 갈리는 단일-이름/이중-스키마가 된다.

### A.1 Private call (1:1) — PTT_GROUP_ADD 파라미터화

TS 24.379 §11. private call 도 별도 명령/RELAY 재사용 없이 `PTT_GROUP_ADD` 에
`group_type:"private"` + floor 모드(`floor_policy`, §B.1)로 표현한다 — **floor 유무는 같은
메시지의 파라미터 차이**다. 2인 그룹의 멤버 포트 모델이 1:1 미디어를 그대로 표현하므로
VoLTE `RELAY_*` 를 끌어올 필요가 없다(§A.0-(2)).

| 모드 | 파라미터 | 응답 |
|---|---|---|
| **with floor** (한 번에 한 명) | `group_type:"private"`, `floor_policy:"single"`(기본) | `member_ports`(2) + `floor_port` |
| **without floor** (full-duplex) | `group_type:"private"`, `floor_policy:"none"` | `member_ports`(2), `floor_port` 생략 |

**PTT_GROUP_ADD 확장 필드** (private):

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_type` | O | 신규 값 **`private`** 추가 (기존 `prearranged`/`chat`/`broadcast`) |
| `floor_policy` | - | `none`=floor 없음(full-duplex), `single`(기본)=한 명씩. 값 정의는 §B.1 |
| `members` | O | 정확히 2 (`caller_sid:prio:role`, `callee_sid:prio:role`) |
| `initiator_id` | O | 발신자 sessionId (floor 초기 부여 후보; `floor_policy:"none"` 시 무의미) |

commencement mode(automatic/manual)와 direction 은 **SIP-only**(§A.4)이므로 CMP 필드가 없다.
private call 은 affiliation 불요 — CSP 가 멤버십 게이트를 우회(상대 MCPTT ID 직접 지정).

```json
{
  "hdr": { "ver": 2, "trans_id": 2001, "node": "csp01", "cmd": "PTT_GROUP_ADD",
           "type": "request", "sesid": "01011112222::csp::...::1", "service": "mcptt" },
  "payload": {
    "group_id": "priv-01011112222-01033334444",
    "group_type": "private", "floor_policy": "single",
    "initiator_id": "01011112222",
    "members": "01011112222:6:participant,01033334444:6:participant"
  }
}
```
응답은 기존 PTT_GROUP_ADD 와 동일(`ip`/`floor_port`/`member_ports`; `floor_policy:"none"` 시
`floor_port` 생략).

### A.2 Pre-established session — 2단 수명

TS 24.379 §11.2 / TS 24.380. 호 이전에 미디어 포트를 **예약**해 두고, 실제 호에서 **바인딩**한다.

- **① 예약**: `PTT_GROUP_ADD`(또는 `RELAY_ADD`)에 신규 필드 **`pre_established: 1`** + 원격 주소
  없이 호출 → CMP 는 포트만 할당하고 **RTP 무활동 sweep 을 면제**(설정 grace, 결정 D3)한다.
- **② 바인딩**: 실제 호 개시 시 `PTT_JOIN`(phase-②) / `RELAY_MODIFY` 로 원격 주소·참여자를
  기존 멱등 경로로 채운다. 포트는 ①에서 광고한 값 그대로(재할당 없음).

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `pre_established` | O(①) | 1 이면 예약 세션 — sweeper 면제 대상 |

해제는 기존 `PTT_GROUP_REMOVE`/`RELAY_REMOVE`(자연 멱등).

### A.3 Regroup (임시 그룹)

TS 24.379 + GMS(TS 24.481). CMP 관점에선 **일반 그룹**이며, 구성 그룹의 멤버를 합친 로스터로
`PTT_GROUP_ADD` 한다. 계약 추가는 로깅/녹취 상관용 **메타데이터**뿐.

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `regroup` | - | 1 이면 임시 재그룹 (로그/녹취 태깅용) |
| `constituent_groups` | - | 원 구성 그룹 ID 배열 (감사·녹취 메타) |

로스터 합성·권한·GMS 문서 생성은 **CSP+CSC 몫**(CMP 무변경).

### A.4 CMP 메시지 없음 (SIP/XCAP-only) — Dev B 스코프 밖

다음은 **미디어 세션 생성은 §A.1 을 그대로 재사용**하고, 추가 CSP↔CMP 메시지가 **없다**.
Dev B 는 이들 때문에 새 CMP 동작을 만들지 않는다.

| 기능 | 처리 계층 | 미디어 세션 |
|---|---|---|
| Automatic/Manual commencement | SIP 200 OK 타이밍/auto-answer | §A.1 |
| Private call **call-back** (요청/취소) | SIP MESSAGE/재INVITE | 응답 시 §A.1 |
| **First-to-answer** | SIP 포킹(다중 target, 최초 응답 wins) | 응답한 leg 만 §A.1 |
| **Functional alias** 활성/비활성 | XCAP(CMS) + SIP 헤더 치환 | 무관 |

> **예외 후보 — Ambient listening**: 청취자 leg 를 **수신전용·floor 표시 억제**로 열어야 하면
> 멤버 플래그(`recv_only`/`silent`)가 필요할 수 있다 → §B.4 결정 D4 로 이관.

---

## B. Media Plane & Floor 파트 (Dev B 구현/통지)

CSP 가 넘긴 정책 플래그를 받아 floor 를 구현하고, floor/미디어 상태를 이벤트로 통지한다.
floor 트래픽 자체는 CMP↔UE in-band 라 아래 필드는 **세션 생성/변경 시의 정책 입력**이다.

### B.1 Floor policy — dual / multi-talker (정책 필드)

`PTT_GROUP_ADD`/`PTT_GROUP_MODIFY` 에 신규 필드 **`floor_policy`** 추가. 미지정=현행 단일화자.

| payload 필드 | 값 | 설명 |
|---|---|---|
| `floor_policy` | `single`(기본) / `none` / `dual` / `multi` | floor 동시 발언 정책 |
| `max_talkers` | 정수(`multi` 시 필수, 2..K) | 동시 발언 상한 N (TS 24.380 Rel-16 multi-talker) |

- **`none`**: floor 중재 없음(full-duplex) — private call without floor(§A.1) 전용. `floor_port`
  미할당, floor RTCP 미처리.
- **`single`**(기본): 현행 단일화자.

- **`dual`** (TS 24.380 dual floor): 선점 시 기존 화자를 REVOKE 하지 않고 override 화자에게
  **동시 GRANT**(최대 2명). 기존 tier 선점(`PTT_FLOOR_TIER`) 판정 위에 "revoke 대신 dual grant"
  분기를 얹는다.
- **`multi`** (multi-talker): 동시 최대 `max_talkers` 명 GRANT. Floor Request/Granted/Taken 및
  **Floor Release Multi Talker** 메시지를 in-band 로 처리(코덱=`cmp/PFloorCodec.cpp`, 상태머신=
  `cmp/PMcpttGroup.cpp`). SSRC/포트 분배는 화자 수만큼 확장.

```json
{
  "payload": {
    "group_id": "grp-rail-01", "group_type": "prearranged",
    "floor_policy": "multi", "max_talkers": 3,
    "members": "...:6:participant,..."
  }
}
```

### B.2 Floor/미디어 보안 키 (E2E, TS 33.180)

| 대상 | E2E 처리 | CMP 필요 키 |
|---|---|---|
| **미디어 RTP** | UE↔UE SRTP(GMK/PCK 파생) | **없음** — CMP 는 암호문을 투명 relay (복호 불요) |
| **Floor control RTCP(MCPT)** | CMP 가 floor 중재자로 참여 | **필요** — GMK/PCK 파생 floor 보호 키 |

→ `PTT_GROUP_ADD` 에 신규 필드 **`floor_crypto`** 추가(floor SRTCP 보호용). 미디어 키는 넘기지
않는다(투명 relay 유지).

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `floor_crypto` | - | `{ "alg":"...", "key":"<b64>", "salt":"<b64>", "mki":"<hex>" }` — floor RTCP 보호 파생 키. 생략=평문 floor(현행) |

키 생성은 **CSC(KMS, GMK/PCK)** 몫, CSP 가 세션 생성 시 파생 floor 키를 relay(결정 D2).
참 ECCSI/SAKKE(RFC 6507/6508) 도입은 `mcptt_standard_conformance.md` S5 후속과 연동.

### B.3 Multicast/MBMS 분배 (예약)

`PTT_GROUP_ADD` 신규 필드 **`distribution`** — 근스코프 밖(예약).

| payload 필드 | 값 | 설명 |
|---|---|---|
| `distribution` | `unicast`(기본) / `multicast` | 그룹 미디어 분배 방식 |
| `multicast_addr` / `multicast_port` | - | `multicast` 시 CMP 송신 목적지 |

### B.4 상태 통지 확장 (CMP → CSP)

multi-talker/dual 에서 현재 발언자 집합을 CSP·콘솔이 알아야 하므로 관측 채널을 확장한다.

- **STATS `detail.groups[]`**: 기존 `floor_holder`(단일) → **`floor_holders`**(배열)로 확장.
- **신규 이벤트 `FLOOR_TALKERS`** (§8 이벤트 채널, ack=동일 trans_id response): 발언자 집합
  변경 시 push. payload `group_id`, `talkers:[sid,...]`, `policy`. (로스터/녹취 태깅·콘솔 실시간용)

```json
{
  "hdr": { "ver": 2, "trans_id": 90101, "node": "cmp01", "cmd": "FLOOR_TALKERS",
           "type": "event", "sesid": "...", "service": "mcptt" },
  "payload": { "group_id": "grp-rail-01", "policy": "multi", "talkers": ["01011112222","01033334444"] }
}
```

> **결정 D4 (ambient)**: §A.4 청취자 수신전용/floor 억제가 필요하면 `PTT_JOIN` 에 멤버 플래그
> `recv_only`/`floor_suppress` 를 추가한다(Dev B 소규모). 요건 확정 시 반영.

---

## C. 신규 필드/명령 매트릭스 (조율 계약)

착수 전 **양 파트가 함께 확정**할 표. cmd 별 신규 필드와 소유 파트.

| cmd | 신규 필드 | 소유 | 기능 |
|---|---|---|---|
| `PTT_GROUP_ADD`/`_MODIFY` | `group_type:"private"` | A | Private call (1:1) |
| `PTT_GROUP_ADD`/`_MODIFY` | `floor_policy`(`none`/`single`/`dual`/`multi`), `max_talkers` | B | Private no-floor / Dual / Multi-talker |
| `PTT_GROUP_ADD` | `floor_crypto` | B | Floor E2E 보호 |
| `PTT_GROUP_ADD` | `distribution`, `multicast_*` | B(예약) | MBMS/멀티캐스트 |
| `PTT_GROUP_ADD`/`RELAY_ADD` | `pre_established` | A | Pre-established session |
| `PTT_GROUP_ADD` | `regroup`, `constituent_groups` | A | Regroup |
| `PTT_JOIN` | `recv_only`, `floor_suppress` (D4) | B | Ambient listening |
| STATS `detail.groups[]` | `floor_holders[]` | B | 다중 발언자 관측 |
| (event) `FLOOR_TALKERS` | 신규 이벤트 | B | 발언자 집합 통지 |

멱등성·자원 키·재전송·`session_digest` audit 규칙은 **불변** — 위 필드는 모두 기존 명령의
payload 확장이라 신뢰성 모델에 영향 없음(신규 이벤트 `FLOOR_TALKERS` 만 §8 ack/재전송 정책 준수).

## D. 착수 전 확정할 설계 결정

| # | 결정 | 권고 |
|---|---|---|
| **D1** | ~~Private-without-floor 를 RELAY 재사용 vs PTT-private~~ | **확정**: private call 은 with/without 모두 `PTT_GROUP_ADD` + `floor_policy`(`single`/`none`)로 통일. VoLTE `RELAY_*` 는 서비스 경계로 분리 유지 (§A.0/§A.1) |
| **D2** | Floor 보호 키 전달 = inline material vs key-id 참조(CMP 가 KMS fetch) | **inline `floor_crypto`** (CSP↔CMP 단일 계약 유지, KMS 접점 CSC 로 국한) |
| **D3** | Pre-established sweeper grace 값 | 별도 `pre_established_grace_sec`(분 단위) — 운영 설정 |
| **D4** | Ambient listening 멤버 플래그 필요 여부 | 요건 확정 후 `recv_only`/`floor_suppress` 추가 |

## E. 분담 진입점 요약

| 파트 | 정본 규격 | 주 코드 | 이 문서 범위 |
|---|---|---|---|
| **A. Call Control & Signaling** | TS 24.379 | `csp/`(+`csc/`) | §A.1~A.4, D1/D3 |
| **B. Media Plane & Floor** | TS 24.380 / TS 33.180 | `cmp/`(+`csc/` KMS) | §B.1~B.4, D2/D4 |

Phase 순서·기능 우선순위는 [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §0-R 로드맵을 본다.
