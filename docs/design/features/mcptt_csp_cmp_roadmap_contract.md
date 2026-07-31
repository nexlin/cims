# MCPTT 로드맵 기능 — CSP↔CMP 연동 메시지 규격

> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §0-R 미반영 로드맵의
> 구현을 2인(**Call Control & Signaling** / **Media Plane & Floor**)으로 분담하기 위한
> **CSP↔CMP 계약 정본**이다. wire 규약은 기존 [../../api/cmp_media_api.md](../../api/cmp_media_api.md)
> (UDP JSON envelope v2)를 그대로 확장하며, 구현된 명령/필드의 정본은 `cmp_media_api.md` 로
> 이관한다 — 본 문서는 **분담 경계와 미착수 항목**의 계약을 유지한다.
> Media Plane & Floor 파트(§B)는 구현 완료, Call Control 파트(§A)는 미착수다(§C 매트릭스 참조).

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

> **명명 규약 (의식적 tradeoff)**: 이 통일로 `PTT_GROUP_*` 명령이 1:1 private call 도 다루게 되어
> 이름의 "GROUP" 이 부정합해진다. 명령 rename 은 기존 구현·API 계약 비용이 크므로 채택하지
> 않고, **`PTT_GROUP_*` 를 "PTT 미디어 세션(멤버 1..N)" 으로 재정의**하며 `group_type` 이 실제
> 형태(private=2인 1:1 / prearranged·chat·broadcast=그룹)를 구분한다. 원칙 ②(일관성)와 최소
> 변경 사이의 명시적 선택임을 남긴다 — 숨은 band-aid 가 아니다.

### A.1 Private call (1:1) — PTT_GROUP_ADD 파라미터화

TS 24.379 §11 / TS 24.380 §7(private call floor control). private call 도 별도 명령/RELAY
재사용 없이 `PTT_GROUP_ADD` 에 `group_type:"private"` + `floor_control`(§B.1) 로 표현한다 —
**floor 유무는 같은 메시지의 파라미터 차이**다. 2인 그룹의 멤버 포트 모델이 1:1 미디어를 그대로
표현하므로 VoLTE `RELAY_*` 를 끌어올 필요가 없다(§A.0-(2)).

> **규격 주의(원칙 ①)**: TS 24.380 은 **private call floor control(§7)을 group floor 와 별도
> 절차로** 정의한다. 따라서 `group_type:"private"` 를 받은 CMP 는 group 의 동시성 정책
> (`floor_policy`, §B.1)이 아니라 **private-call floor 상태머신**을 적용한다 — private 은 2인
> 이므로 `floor_policy`(single/dual/multi)를 해석하지 않는다. private 의 floor 유무는 오직
> `floor_control` 로 정한다.

| 모드 | 파라미터 | 응답 | floor 절차 |
|---|---|---|---|
| **with floor** (한 번에 한 명) | `group_type:"private"`, `floor_control:"on"`(기본) | `member_ports`(2) + `floor_port` | TS 24.380 §7 private-call floor |
| **without floor** (full-duplex) | `group_type:"private"`, `floor_control:"off"` | `member_ports`(2), `floor_port` 생략 | 없음(양방향) |

**PTT_GROUP_ADD 확장 필드** (private):

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_type` | O | 신규 값 **`private`** 추가 (기존 `prearranged`/`chat`/`broadcast`) |
| `floor_control` | - | `on`(기본)=floor 제어 有(private-call floor), `off`=full-duplex. 값 정의는 §B.1 |
| `members` | O | 정확히 2 (`caller_sid:prio:role`, `callee_sid:prio:role`) |
| `initiator_id` | O | 발신자 sessionId (floor 초기 부여 후보; `floor_control:"off"` 시 무의미) |

commencement mode(automatic/manual)와 direction 은 **SIP-only**(§A.4)이므로 CMP 필드가 없다.
private call 은 affiliation 불요 — CSP 가 멤버십 게이트를 우회(상대 MCPTT ID 직접 지정).

```json
{
  "hdr": { "ver": 2, "trans_id": 2001, "node": "csp01", "cmd": "PTT_GROUP_ADD",
           "type": "request", "sesid": "01011112222::csp::...::1", "service": "mcptt" },
  "payload": {
    "group_id": "priv-01011112222-01033334444",
    "group_type": "private", "floor_control": "on",
    "initiator_id": "01011112222",
    "members": "01011112222:6:participant,01033334444:6:participant"
  }
}
```
응답은 기존 PTT_GROUP_ADD 와 동일(`ip`/`floor_port`/`member_ports`; `floor_control:"off"` 시
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

> **B.1·B.2·B.4 는 구현 완료** — wire 규격 정본은
> [../../api/cmp_media_api.md](../../api/cmp_media_api.md) 로 이관했다(§7.1 payload 필드,
> §7.4 ambient 플래그, §7.7 floor 정책, §7.8 floor_crypto, §5.2 STATS, §8 FLOOR_TALKERS).
> 여기서는 분담 경계와 CSP 측 소비 책임만 남긴다.

### B.1 Floor — 유무(`floor_control`)와 동시성(`floor_policy`)

floor 는 **직교하는 두 축**이다(원칙 ② — 한 enum 에 섞지 않는다): `floor_control`(on/off) 이
제어 유무, `floor_policy`(single/dual/multi + `max_talkers`) 가 동시 발언 수.
`group_type:"private"` 은 동시성 축을 해석하지 않고 TS 24.380 §7 private-call floor 절차를
적용한다(정원 1·큐 없음·개시자 초기 발언권).

Dev A 는 세션 생성 시 이 필드들을 실어 보내면 된다 — 이후 floor 절차(GRANT/TAKEN/REVOKE/
Floor Release Multi Talker·동시 발언 슬롯·녹취 트랙 분리)는 CMP 자율이다.
구현 위치: 코덱 `cmp/PFloorCodec.cpp`, 상태머신 `cmp/PMcpttGroup.cpp`.
정책은 `PTT_GROUP_MODIFY` 로 언제든 바꿀 수 있고, 정원이 줄면 CMP 가 초과 화자를 Revoke 해
상태를 정책에 맞춘다. 동시 발언의 **실호 검증은 단말 정합이 전제**다
([mcptt_standard_conformance.md](mcptt_standard_conformance.md) §0-R R2 각주).

### B.2 Floor/미디어 보안 키 (E2E, TS 33.180)

| 대상 | E2E 처리 | CMP 필요 키 |
|---|---|---|
| **미디어 RTP** | UE↔UE SRTP(GMK/PCK 파생) | **없음** — CMP 는 암호문을 투명 relay (복호 불요) |
| **Floor control RTCP(MCPT)** | CMP 가 floor 중재자로 참여 | **필요** — GMK/PCK 파생 floor 보호 키 |

`PTT_GROUP_ADD.floor_crypto` 로 inline 전달(결정 D2). **키 생성은 CSC(KMS, GMK/PCK) 몫이고
CSP 가 세션 생성 시 파생 floor 키를 relay 한다 — 아직 미연결이라 현재 운용은 평문 floor 다.**
참 ECCSI/SAKKE(RFC 6507/6508) 도입은 `mcptt_standard_conformance.md` S5 후속과 연동.

### B.3 Multicast/MBMS 분배 (예약)

`PTT_GROUP_ADD` 신규 필드 **`distribution`** — 근스코프 밖(예약, 미구현).

| payload 필드 | 값 | 설명 |
|---|---|---|
| `distribution` | `unicast`(기본) / `multicast` | 그룹 미디어 분배 방식 |
| `multicast_addr` / `multicast_port` | - | `multicast` 시 CMP 송신 목적지 |

### B.4 상태 통지 확장 (CMP → CSP)

STATS `detail.groups[].floor_holders`(배열)와 이벤트 `FLOOR_TALKERS`(발언자 집합 변경 시 push)로
동시 발언을 관측한다. **CSP 측 소비는 Dev A 후속 과제** — 현재 CSP 는 이벤트를 ack 만 하고
버린다(콘솔은 STATS 폴링으로 표시). 로스터 상태 반영·녹취 태깅·콘솔 실시간 push 가 붙을 자리다.

---

## C. 신규 필드/명령 매트릭스 (조율 계약)

착수 전 **양 파트가 함께 확정**할 표. cmd 별 신규 필드와 소유 파트.

| cmd | 신규 필드 | 소유 | 기능 | 상태 |
|---|---|---|---|---|
| `PTT_GROUP_ADD`/`_MODIFY` | `group_type:"private"` | A | Private call (1:1) | CMP 수용 완료 / CSP 발행 미구현 |
| `PTT_GROUP_ADD`/`_MODIFY` | `floor_control`(`on`/`off`) | B | Floor 유무 (private no-floor 포함) | 완료 |
| `PTT_GROUP_ADD`/`_MODIFY` | `floor_policy`(`single`/`dual`/`multi`), `max_talkers` | B | Dual / Multi-talker (그룹 전용) | 완료 |
| `PTT_GROUP_ADD`/`_MODIFY` | `floor_crypto` | B | Floor E2E 보호 | 완료 (CSC KMS 연결 대기) |
| `PTT_GROUP_ADD` | `distribution`, `multicast_*` | B(예약) | MBMS/멀티캐스트 | 미구현 |
| `PTT_GROUP_ADD`/`RELAY_ADD` | `pre_established` | A | Pre-established session | 미구현 |
| `PTT_GROUP_ADD` | `regroup`, `constituent_groups` | A | Regroup | 미구현 |
| `PTT_JOIN` | `recv_only`, `floor_suppress` | B | Ambient listening | 완료 (CSP 발행 미구현) |
| STATS `detail.groups[]` | `floor_holders[]` | B | 다중 발언자 관측 | 완료 (OAM 콘솔 반영) |
| (event) `FLOOR_TALKERS` | 신규 이벤트 | B | 발언자 집합 통지 | CMP push 완료 / CSP 소비 미구현 |

멱등성·자원 키·재전송·`session_digest` audit 규칙은 **불변** — 위 필드는 모두 기존 명령의
payload 확장이라 신뢰성 모델에 영향 없음(신규 이벤트 `FLOOR_TALKERS` 만 §8 ack/재전송 정책 준수).

## D. 착수 전 확정할 설계 결정

| # | 결정 | 권고 |
|---|---|---|
| **D1** | ~~Private-without-floor 를 RELAY 재사용 vs PTT-private~~ | **확정**: private call 은 with/without 모두 `PTT_GROUP_ADD` + `group_type:"private"` + `floor_control`(`on`/`off`)로 통일. CMP 는 private-call floor 절차(TS 24.380 §7)를 적용(group `floor_policy` 미해석). VoLTE `RELAY_*` 는 서비스 경계로 분리 유지 (§A.0/§A.1) |
| **D2** | Floor 보호 키 전달 = inline material vs key-id 참조(CMP 가 KMS fetch) | **확정·구현**: inline `floor_crypto`(CSP↔CMP 단일 계약 유지, KMS 접점은 CSC 로 국한). 프로파일 `AES_CM_128_HMAC_SHA1_80`/`_32` |
| **D3** | Pre-established sweeper grace 값 | 별도 `pre_established_grace_sec`(분 단위) — 운영 설정 |
| **D4** | Ambient listening 멤버 플래그 필요 여부 | **확정·구현**: `PTT_JOIN` 에 `recv_only`(상향 미중계+발언 거절)/`floor_suppress`(floor 메시지 미송신) |

## E. 분담 진입점 요약

| 파트 | 정본 규격 | 주 코드 | 이 문서 범위 |
|---|---|---|---|
| **A. Call Control & Signaling** | TS 24.379 | `csp/`(+`csc/`) | §A.1~A.4, D1/D3 |
| **B. Media Plane & Floor** | TS 24.380 / TS 33.180 | `cmp/`(+`csc/` KMS) | §B.1~B.4, D2/D4 |

Phase 순서·기능 우선순위는 [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §0-R 로드맵을 본다.
