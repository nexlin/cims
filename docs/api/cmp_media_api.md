# CMP Media API 규격 (UDP JSON, envelope v2)

CMP 가 제공하는 미디어 서비스 기능(function)의 제어 API 정본이다. CMP 는 **미디어 기능
서버**이고, 이를 사용하는 클라이언트는 불특정 다수의 **서비스 AS**(현재는 CSP 가 유일)라는
관점으로 규격을 정의한다.

> **구현 상태**: 본 envelope v2 가 유일한 wire 규격이다 (`hdr` 없는 패킷은
> `BAD_REQUEST` 로 거절). 명령별 payload 필드·내부 동작은
> [modules/cmp.md](../design/modules/cmp.md) §3.2 참조. **규격만 예약되고 미구현인 것**:
> ① 이벤트 채널([§8](#8-이벤트-type-event)), ② 자원 복합 키(멀티 client 격리 —
> [§4](#4-자원-모델과-이벤트-라우팅)), ③ MIX function. CMDP(MCData 미디어평면)도 동일
> envelope v2 를 따른다 — 명령(`MSRP_*`)·이벤트 정본은
> [mcdata_messaging.md](../design/features/mcdata_messaging.md) §4.7. CMDP 이벤트 채널은
> §8 규격(ack=동일 trans_id 의 response, 1s×5 재전송)의 실구현 선례다.

## 1. 개요

### 1.1 모델

- **서버**: CMP. UDP 단일 포트(기본 9001)에서 JSON datagram 요청을 수신하고 응답한다.
- **클라이언트**: 서비스 AS. 논리 노드 ID(`hdr.node`)로 식별하며, 여러 AS 가 하나의 CMP 를
  동시에 사용할 수 있다. 전송 주소가 아닌 논리 노드 ID 가 식별자이므로 HA 절체 시 standby 가
  노드 정체성을 이어받아 기존 자원을 계속 주소지정할 수 있다.
- **기능(function) 그룹**: 명령은 기능 그룹별 prefix 로 명명한다.

| Function | 제공 기능 | 자원 | 자원 키 (멱등성·격리 범위) |
|---|---|---|---|
| **CORE** | 생존 확인·자원 요약, 상세 상태 조회 | client 레지스트리 | `hdr.node` |
| **RELAY** | 1:1 RTP relay (VoLTE 등) — peer 별 전용 포트 블록 | relay session | `(node, session_id)` — 생성 client 전속 |
| **PTT** | 그룹통화(멤버별 전용 RTP 포트) + MCPTT floor control(그룹 공유 포트) | group / member | group: `(service, group_id)` — 동일 service 의 AS 들이 공유<br>member: `(node, session_id)` |
| **MIX** *(예약)* | VoLTE 그룹통화 mixing/conference | mixer / participant | `(service, conf_id)` — PTT 와 동형 |

### 1.2 전송

- UDP JSON, 메시지 = datagram 1개 = 한 줄 JSON(UTF-8). 최대 크기 4KB — 초과 메시지는
  송신 측에서 거부한다 (수신 버퍼 절단으로 파싱 실패가 확정되므로). 실질 상한은 PTT
  로스터 크기: 요청 `members` ≈ 110명, 응답 `member_ports` ≈ 75명. 대형 그룹은 초기
  로스터를 줄이고 나머지 멤버를 PTT_JOIN 2단([§7.4](#74-ptt_join--멤버-참가-2단-멱등))으로
  합류시킨다.
- 요청-응답 매칭은 `hdr.trans_id`. 신뢰성(재전송·멱등성)은 [§3](#3-신뢰성-모델).

## 2. 메시지 envelope

모든 메시지는 `hdr` + (선택적) `payload` 두 최상위 키로 구성한다.

```json
{
  "hdr": {
    "ver": 2,
    "trans_id": 1024,
    "node": "csp01",
    "cmd": "RELAY_ADD",
    "type": "request",
    "sesid": "01011112222::csp::1768531200123456::7",
    "service": "volte"
  },
  "payload": { "...cmd 별 업무 필드..." : "..." }
}
```

### 2.1 hdr 필드

| 필드 | 규칙 | 설명 |
|---|---|---|
| `ver` | 필수 | 프로토콜 버전. 본 규격은 `2` |
| `trans_id` | 필수 | 요청-응답 매칭 ID. 발신 방향별 독립 공간 (client 발행: request, CMP 발행: event) |
| `node` | 필수 | **메시지를 만든 노드**의 논리 ID (요청=client, 응답/이벤트=CMP) |
| `cmd` | 필수 | 명령명 ([§1.1](#11-모델) 의 function prefix 체계). 응답에도 동일 cmd 를 실어 어떤 명령의 응답인지 자명하게 한다 |
| `type` | 필수 | `request` \| `response` \| `event` |
| `status` | response 필수 | `OK` \| `ERROR` |
| `code`, `reason` | ERROR 시 필수 | 구조화 에러 ([§9](#9-에러-코드)). `code` 는 기계 판독용, `reason` 은 사람용 자유 문자열 |
| `sesid` | 호 문맥 명령 필수 | 로그 상관관계 ID (flow 로깅 역조회용, 업무 로직에 사용하지 않음). RELAY_*/PTT_*/MIX_* 에서 필수, CORE 명령은 생략 |
| `service` | 호 문맥 명령 필수 | 자원이 속한 서비스 (`volte`/`mcptt`/...). PTT 그룹 자원 키의 일부. CORE 명령은 생략 |

### 2.2 payload

- **요청**: cmd 별 업무 필드(조작 대상 자원 키 포함). 자원 이름(session_id 등)은 **클라이언트가
  명명**한다 — 응답 유실 시에도 재시도·해제가 가능해야 하는 UDP 멱등성 요건 때문이다.
  CMP 가 발행하는 것은 자원의 실체(포트)이며 응답 payload 로 반환된다.
- **응답**: cmd 별 결과 데이터만 싣는다. 요청 필드의 echo(session_id 등)는 하지 않는다
  (요청자는 trans_id 로 보류 요청과 매칭한다). 결과 데이터가 없으면 payload 자체를 생략한다
  (HEARTBEAT 제외 — [§5.1](#51-heartbeat)).
- **이벤트**: [§8](#8-이벤트-type-event).

## 3. 신뢰성 모델

| 항목 | 규칙 |
|---|---|
| 요청 재전송 | client 책임. 권장: 100ms 대기 × 3회 (현행 CSP 구현과 동일, 총 ceiling 300ms) |
| 멱등성 | 자원 생성 명령(RELAY_ADD, PTT_GROUP_ADD, MIX_CREATE)은 같은 자원 키의 재요청에 **재할당 없이 동일 결과를 반환**한다. 나머지 명령은 자연 멱등(같은 상태로 수렴) |
| 멱등성 범위 | 자원 키에 `node`(RELAY) 또는 `service`(PTT/MIX) 가 포함되므로, 서로 다른 client/service 의 동일 이름은 충돌하지 않는다 |
| 이벤트 재전송 | CMP 책임. ack(동일 trans_id 의 `type:"response"`) 미수신 시 1s 간격 최대 5회 (CMDP event 채널과 동일 정책) |

## 4. 자원 모델과 이벤트 라우팅

> **복합 키는 목표 규격** — 현행 구현은 단일 client(CSP) 전제로 session_id/group_id
> 단독 키를 사용한다. 여러 client 가 한 CMP 를 공유하는 배치가 도입될 때
> `(node, session_id)`/`(service, group_id)` 격리를 활성화한다.

- **relay session** — 생성한 client(node) 전속. 다른 node 는 같은 session_id 로도 접근 불가
  (별개 자원으로 취급). 관련 이벤트는 소유 node 로만 push.
- **PTT group** — `(service, group_id)` 로 식별되는 **공유 자원**. 같은 service 를 분담하는
  여러 AS 가 동일 그룹에 JOIN/LEAVE 할 수 있다(scale-out). member 는 `(node, session_id)` 로
  소속 client 를 기억하며, 그룹 이벤트는 참여 중인 node 전체로, 멤버 이벤트는 해당 member 의
  소유 node 로 push.
- **유휴 회수(sweeper)** — CMP 는 RTP 무활동 자원을 자체 회수할 수 있다. 회수 시
  RELAY_ABORTED / PTT_GROUP_ABORTED 이벤트로 통지한다 ([§8](#8-이벤트-type-event)).

## 5. CORE

CORE 명령은 `sesid`/`service` 를 싣지 않는다.

### 5.1 HEARTBEAT

client 접속(attach)·생존 확인·자원 요약 보고를 겸한다. client 가 주기 송신한다
(권장 3초, 연속 3회 무응답 시 Disconnected 판정 — client 정책). CMP 는 이 요청(및 모든
제어 요청)의 소스 주소를 **이벤트 push 대상**(CSP CmpClient 소켓)으로 학습한다 —
sweeper 회수 시 그 주소로 [§8](#8-이벤트-type-event) 이벤트를 보낸다. 현행은 단일
client(CSP) 전제라 마지막 소스를 유지한다(다중 client 격리는 [§4](#4-자원-모델과-이벤트-라우팅)
복합 키와 함께 후속).

요청:
```json
{ "hdr": { "ver": 2, "trans_id": 55, "node": "csp01", "cmd": "HEARTBEAT", "type": "request" } }
```

응답 — 자원 요약을 동봉한다. `resource` 의 **키 목록이 곧 CMP 의 지원 기능 광고**다
(MIX 지원 CMP 는 `resource.mix` 가 나타난다):
```json
{
  "hdr": { "ver": 2, "trans_id": 55, "node": "cmp01", "cmd": "HEARTBEAT",
           "type": "response", "status": "OK" },
  "payload": {
    "resource": {
      "relay": { "total": 500, "used": 8, "sessions": 4 },
      "ptt":   { "total": 100, "used": 2, "groups": 2, "joined": 5,
                 "member_total": 200, "member_used": 5 }
    },
    "session_digest": {
      "relay": { "count": 4, "hash": "61799bd4b6b64b3f" },
      "group": { "count": 2, "hash": "0b7712cf9a3d5521" }
    }
  }
}
```

| resource 필드 | 의미 |
|---|---|
| `relay.total` / `relay.used` | VoIP relay 블록(호당 peer 별 포트셋) 풀 크기 / 사용 중 |
| `relay.sessions` | 활성 relay 세션 수 |
| `ptt.total` / `ptt.used` | PTT 그룹(floor) 풀 크기 / 사용 중 |
| `ptt.groups` / `ptt.joined` | 활성 그룹 수 / 참가 멤버 총수 |
| `ptt.member_total` / `ptt.member_used` | PTT 멤버 포트 유닛 풀 크기 / 사용 중 |

client 는 이 요약으로 부하 기반 CMP 선택, 조기 호 거절(admission control)을 할 수 있다.

**`session_digest`** — 세션 재조정(audit 수준2, [ha_design.md](../design/ha_design.md)) 용 세션집합 지문.
`relay`/`group` 각각 `{count, hash}` 로, `hash = XOR(fnv1a64(id))` (전 세션ID의 FNV-1a 64bit XOR
누적 — **순서무관** 16진 문자열, JSON number 정밀도 한계로 문자열 표기). client(CSP)는 자기
소유 세션집합의 동일 지문을 유지해 **매 HEARTBEAT(3초)마다 대조**한다. `count`+`hash` 가 같으면
정합(고확률)이라 아무 것도 하지 않고, 다르면 그 때만 [SESSION_LIST](#53-session_list)로 상세
diff 하여 고아 자원을 회수한다. 비정상 원인(메시지 유실·프로세스 재기동·이중화 절체)으로 생긴
CSP↔CMP 자원 불일치를 sweeper 타임아웃(분 단위)보다 빠르게(초 단위) 수렴시킨다.

### 5.2 STATS

on-demand 상세 조회 (진단·대시보드용 — OAM/검증 파이프라인이 주 사용자). 응답 payload 는
HEARTBEAT 와 동일한 `resource` 구조에 `detail` 섹션을 더한다.

```json
{
  "hdr": { "ver": 2, "trans_id": 90, "node": "oam01", "cmd": "STATS", "type": "request" }
}
```
```json
{
  "hdr": { "ver": 2, "trans_id": 90, "node": "cmp01", "cmd": "STATS",
           "type": "response", "status": "OK" },
  "payload": {
    "resource": { "relay": { "...": "..." }, "ptt": { "...": "..." } },
    "detail": {
      "session_timeout": 7200,
      "orphan_reclaim_sec": 60,
      "leak_reclaim": { "total": 3, "orphan": 2, "hold": 1 },
      "rtp_src_drop": 0,
      "floor_crypto_drop": 0,
      "nat": [ { "key": "a84b4c76e66710", "leg": "b",
                 "learned_ip": "203.0.113.7", "learned_port": 41022 } ],
      "groups": [ { "group_id": "grp-fire-01", "members": 5, "floor_policy": "dual",
                    "floor_holders": ["01011112222", "01033334444"] } ]
    }
  }
}
```

| detail 필드 | 의미 |
|---|---|
| `rtp_src_drop` | 미협상 소스 드롭 누적 카운터 (no-NAT leg 의 선언 주소 불일치 패킷). 해제된 자원의 몫을 이월한 **단조 증가** 값 |
| `floor_crypto_drop` | floor SRTCP 인증 실패·재전송 폐기 누적([§7.8](#78-floor_crypto--floor-rtcp-보호-ts-33180)). 해제된 그룹의 몫을 이월한 **단조 증가** 값 |
| `nat` | NAT latch 완료 leg 목록 — `key`(session_id 또는 `group_id:member`), `leg`(`a`/`b`/멤버 sid), 학습된 실주소. 최대 20개 (전체 수는 `nat_total`) |
| `groups` | 활성 그룹별 상세 (`group_id`/`members`/`floor_policy`/`floor_holders`). `floor_policy` 는 적용 중인 정책(`off`/`single`/`dual`/`multi`/`private`), `floor_holders` 는 현재 발언자 **배열**(동시 발언 시 복수, 발언자 없으면 생략). 최대 20개 (전체 수는 `groups_total`) |

`nat`/`groups` 배열 상한은 응답 datagram 4KB 계약([§1.2](#12-전송)) 내 안전 상한이다.
STATS 응답도 HEARTBEAT 와 동일하게 [`session_digest`](#51-heartbeat)를 함께 싣는다.

### 5.3 SESSION_LIST

세션 재조정(audit 수준2)용 세션 열거. [`session_digest`](#51-heartbeat) 대조에서 **불일치가
감지됐을 때만** client(CSP)가 호출해 CMP 보유 세션의 전체 집합을 당겨(pull) 자기 소유 집합과
diff 한다. push(이벤트)로는 절체 후 새 active 가 옛 세션을 기억하지 못해 상관지을 수 없으므로,
재조정은 pull 로만 성립한다.

```json
{
  "hdr": { "ver": 2, "trans_id": 71, "node": "csp01", "cmd": "SESSION_LIST", "type": "request" },
  "payload": { "kind": "relay", "offset": 0, "limit": 40, "min_age_sec": 0 }
}
```
```json
{
  "hdr": { "ver": 2, "trans_id": 71, "node": "cmp01", "cmd": "SESSION_LIST",
           "type": "response", "status": "OK" },
  "payload": {
    "kind": "relay", "total": 128, "next_offset": 40,
    "entries": [
      { "session_id": "csp_20260720170500999_1", "age_sec": 812 },
      { "session_id": "csp_20260720170501004_1", "age_sec": 806 }
    ]
  }
}
```

| 요청 필드 | 의미 |
|---|---|
| `kind` | `relay`(1:1 RTP 세션) 또는 `group`(PTT 그룹). 기본 `relay` |
| `offset` / `limit` | 페이지. `limit` 는 4KB datagram 계약 내 상한 **40**으로 clamp |
| `min_age_sec` | 이 초 이상 존재한 세션만 반환(grace) — 설정 중인 신규 세션의 오회수 방지. client 는 보통 `0`(전량)으로 받아 age 를 client-side 에서 판정 |

| 응답 필드 | 의미 |
|---|---|
| `total` | grace 필터 적용 후 전체 세션 수 |
| `next_offset` | 다음 페이지 offset. 마지막 페이지면 `-1` |
| `entries[].session_id` | 세션(relay) 또는 그룹 식별자 |
| `entries[].age_sec` | CMP 에서 세션이 존재한 기간(초) — client 가 grace 판정에 사용 |

**재조정 규칙(client=CSP)**: `orphan`(CMP有 CSP無, `age_sec ≥ GraceSec`) → [RELAY_REMOVE](#63-relay_remove)
로 회수. `zombie`(CSP有 CMP無, 미디어 소실) → 호 종료(opt-in). 회수는 **active 역할일 때만**
실행하고 standby 는 탐지·로그만 한다(hot-standby 가 active 세션을 오회수하지 않도록). 자세한
수준·정책은 [ha_design.md](../design/ha_design.md) 수준2 참조.

## 6. RELAY — 1:1 RTP relay

자원 키 `(node, session_id)`. `session_id` 는 client 가 명명한다 — CMP 는 불투명 문자열로만
취급한다. CSP 는 `csp_{yyyymmddHHMMSSmmm}_{n}` (발행자 prefix + ms 타임스탬프 + 동일-ms 순번)
을 발행해 **재시작 경계 포함 전역 유일**을 보장한다 — 재기동 후 첫 발행이 CMP 잔존 고아
세션과 멱등 충돌하지 않는다.

### 6.1 RELAY_ADD — relay 생성 (멱등)

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `session_id` | O | 세션 식별자 (client 명명) |
| `remote_ip` / `remote_port` | O | 상대방 RTP 주소 (SDP 선언). IP `0.0.0.0` 또는 port `0` = 해당 peer 주소 미확정 — CMP 는 송신 목적지를 설정하지 않고, 이후 RELAY_MODIFY 로 확정한다 |
| `remote_video_port` | - | 상대방 Video RTP 포트 |
| `peer_index` | - | 피어 인덱스 (0=발신 A / 1=착신 B) |
| `remote_nat` | - | 1 이면 해당 peer 가 NAT 뒤 — 그 peer 전용 포트에 목적지 latch 허용 (생략=0). [ue_nat_traversal.md §5](../design/features/ue_nat_traversal.md) |
| `remote_sig_ip` | - | 해당 peer 의 SIP 시그널링 실소스 IP — latch IP guard 기준 |
| `remote_pt` / `remote_te_pt` | - | 이 peer 가 **수신** 선언한 audio/telephone-event wire PT — CMP 가 이 peer 로 송신 시 스탬프(leg 별 PT 재작성). 생략=0=재작성 없음(PT-blind 통과). marker bit 보존, 녹취는 talker 원본 PT 유지 |
| `remote_src_pt` / `remote_src_te_pt` | - | 이 peer 가 **송신**에 쓰는 audio/TE PT(= 반대편에 낸 SDP 의 PT, RFC 3264) — ingress audio/TE 분류 기준 + 녹취 세그먼트 메타(`audio_pt_a/b`). `remote_src_te_pt` 생략 시 TE 는 관례 PT 101 로 분류 |
| `remote_codec` | - | 이 peer 의 협상 오디오 코덱 문자열(예 `"AMR-WB/16000"`) — 녹취 세그먼트 메타(`audio_codec_a/b`)용. CSP 는 코덱 테이블 top 의 rtpmap prefix 를 싣는다 |
| `caller` / `callee` | - | 발/착신자 (flow 로깅·녹취 메타용) |
| `record_dir` | - | 녹취 디렉토리 (있으면 녹취 시작) |

> PT 재작성 배경: 1:1 relay 는 규격 준수 단말이면 비대칭 PT 로도 동작하므로(RFC 3264 —
> answer 의 PT echo 는 SHOULD) `remote_pt` 계열은 대칭 PT 를 가정하는 단말 방어용 보험이다.
> 그룹(fan-out) 경로의 leg 별 PT 재작성은 [§7.4](#74-ptt_join--멤버-참가-2단-멱등) 를 본다.

같은 `(node, session_id)` 재요청 시 재할당 없이 기존 포트를 반환한다 (재전송 안전).

relay 세션은 **peer 별 전용 포트 블록**(audio RTP/RTCP + video RTP/RTCP × 2 peer)을
소유한다. client 는 peer0(발신) leg 의 SDP 에 `local_port*` 를, peer1(착신) leg 의 SDP 에
`local_port_b*` 를 광고해야 한다 — 수신 포트가 곧 peer 신원이다.

```json
{
  "hdr": { "ver": 2, "trans_id": 1024, "node": "csp01", "cmd": "RELAY_ADD",
           "type": "request", "sesid": "01011112222::csp::1768531200123456::7", "service": "volte" },
  "payload": { "session_id": "a84b4c76e66710", "caller": "01011112222", "callee": "01033334444",
               "remote_ip": "192.168.10.21", "remote_port": 40000,
               "remote_nat": 1, "remote_sig_ip": "203.0.113.7",
               "record_dir": "/data/record/2026/07/16" }
}
```
```json
{
  "hdr": { "ver": 2, "trans_id": 1024, "node": "cmp01", "cmd": "RELAY_ADD",
           "type": "response", "sesid": "01011112222::csp::1768531200123456::7",
           "service": "volte", "status": "OK" },
  "payload": { "local_ip": "192.168.10.11",
               "local_port": 30000, "local_video_port": 30002,
               "local_port_b": 30004, "local_video_port_b": 30006 }
}
```

| 응답 필드 | 설명 |
|---|---|
| `local_ip` | relay 미디어 IP |
| `local_port` / `local_video_port` | peer0(발신 A) 전용 audio/video RTP 포트 |
| `local_port_b` / `local_video_port_b` | peer1(착신 B) 전용 audio/video RTP 포트 |

각 포트의 RTCP 는 +1 이다.

실패 응답 (payload 생략):
```json
{
  "hdr": { "ver": 2, "trans_id": 1024, "node": "cmp01", "cmd": "RELAY_ADD",
           "type": "response", "sesid": "01011112222::csp::1768531200123456::7",
           "service": "volte", "status": "ERROR", "code": "NO_RESOURCE",
           "reason": "rtp pool exhausted" }
}
```

### 6.2 RELAY_MODIFY — 피어 주소 재협상

payload 는 RELAY_ADD 와 동일. 기존 세션의 원격 피어 주소만 갱신하고 동일 로컬 포트를
응답한다 (내부적으로 RELAY_ADD 와 같은 멱등 경로). 없는 세션이면 `NOT_FOUND` 에러 —
소실 세션을 부활시키지 않는다 (부활 시 포트가 재할당되어 client 가 이미 광고한 포트와
어긋난다). 주소가 갱신된 peer 의 NAT latch 상태는 리셋되어 재-latch 가 허용되며,
선언 주소·NAT 속성이 직전과 동일한 재요청(세션 refresh 성 re-INVITE, 재전송)은
latch 를 유지한다.

### 6.3 RELAY_REMOVE — relay 해제

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `session_id` | O | 세션 식별자 |
| `caller` / `callee` | - | flow 로깅용 |

녹취 중지 → 자원 반납. 이미 없는 세션이면 `OK` (자연 멱등).

## 7. PTT — 그룹통화 + floor control

group 자원 키 `(service, group_id)` — 같은 service 의 AS 들이 공유한다.
member 키 `(node, session_id)`.

> **사용 규약 (생성 vs 변경)**: 최초 수립만 `PTT_GROUP_ADD`, **이후 모든 상태 변경(멤버 증감·
> 우선순위 등)은 `PTT_GROUP_MODIFY`** 로 한다. 둘은 없는 그룹일 때만 다르다 — ADD 는 생성,
> MODIFY 는 `NOT_FOUND`(재생성 금지: 재할당 포트가 client 가 광고한 SDP 포트와 어긋남).
> MODIFY 가 `NOT_FOUND` 를 반환하면 그때만 `PTT_GROUP_ADD` 로 복구하고 응답 포트로 캐시를
> 갱신한다. RELAY 도 동형(`RELAY_ADD` 생성/복구, `RELAY_MODIFY` 변경).

### 7.1 PTT_GROUP_ADD — 그룹 생성 (멱등)

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_id` | O | 그룹 식별자 |
| `members` | - | `"sid:prio[:role[:tier]],..."` CSV (role=`chair`/`participant`, tier=`emergency`/`imminent`/`normal`) |
| `subid` | - | 그룹 세션 회차 (flow 로그 subid) |
| `video_enabled` | - | 1 이면 video 포트 활성 |
| `group_type` | - | `prearranged`/`chat`/`broadcast`/`private` — `broadcast` 는 개시자 floor 독점(TS 24.380 §6.3.5.4.4 — 타 멤버는 Deny #5, Floor Taken 의 Permission=0), `private` 은 1:1 private call(2인, TS 24.379 §11 — floor 절차는 TS 24.380 §6.3 공통) |
| `initiator_id` | - | 개시자 sessionId — broadcast 는 유일 발언자. private 에서는 **초기 발언권을 주지 않는다**(초기 발언권의 정본은 PTT_JOIN `granted`) |
| `floor_control` | - | `on`(기본)/`off`. `off` = floor 중재 없음(full-duplex) — `floor_port` 미광고, floor RTCP 미처리 |
| `floor_policy` | - | `single`(기본)/`dual`/`multi` — floor 有 **그룹**의 동시 발언 수([§7.7](#77-floor-정책--동시-발언과-private-call)). `private` 은 해석하지 않는다 |
| `max_talkers` | `multi` 시 O | 동시 발언 상한(2..8). `multi` 인데 누락/1 이하, 또는 8 초과면 `BAD_REQUEST` |
| `floor_timers` | - | 그룹별 floor 타이머(초) `{t1_end_rtp, t2_stop_talk, t3_grace, t8_revoke, t7_idle_resend, t20_grant_retx}` — 미지정 필드는 CMP 설정값. 범위 밖이면 `BAD_REQUEST` ([§7.7](#77-floor-정책--동시-발언과-private-call)) |
| `floor_crypto` | - | floor RTCP 보호 키 `{alg,key,salt[,mki]}` ([§7.8](#78-floor_crypto--floor-rtcp-보호-ts-33180)) |
| `record_dir` | - | 녹취 그룹 base 디렉토리 (있으면 녹취 시작) |
| `session_dir` | - | 세션 디렉터리 이름 `S{yyyymmddHHMMSSuuuuuu}_{n}` — 기록 자리는 `record_dir/{YYYY}/{MM}/{DD}/{HH}/{session_dir}/`. 기록 단위가 세션이라 같은 시간대의 다음 통화가 앞 통화에 섞이지 않는다. 미전달 시 시간버킷 직행(구 동작). 세그먼트 `seq` 는 **세션 단위 단조증가** — 세션이 시간버킷을 넘어가도 리셋하지 않는다 ([recording.md §3.3](../design/features/recording.md)) |

응답 payload: `ip`, `floor_port`(그룹 공유 floor control 포트 — `floor_control:"off"` 면
**생략**), `member_ports`(멤버별 전용 RTP 포트 맵 — members 로 전달된 초기 로스터에 대해 할당).
기존 그룹 재요청 시 재할당 없이 members 만 갱신하고 동일 포트를 응답한다.

정책 필드의 미상 값(`floor_policy:"dual2"` 등)은 기본값으로 대체하지 않고 `BAD_REQUEST` 로
거절한다 — 계약 위반이 조용히 다른 동작으로 굳는 것을 막는다.

```json
{
  "payload": {
    "ip": "192.168.10.11",
    "floor_port": 54000,
    "member_ports": {
      "01011112222": { "port": 52000, "video_port": 56000 },
      "01033334444": { "port": 52002, "video_port": 56002 }
    }
  }
}
```

audio RTP 는 그룹 공유 포트가 아니라 **멤버별 전용 포트**다 — client 는 각 멤버의 SDP 에
그 멤버의 `port`/`video_port` 를 광고한다 (floor 는 그룹 공유 `floor_port`).
멤버 신원은 수신 포트로 확정되며, floor control 은 TS 24.380 User ID(in-band)로 식별한다.

### 7.2 PTT_GROUP_MODIFY — 멤버/우선순위 갱신

PTT_GROUP_ADD 와 동일 payload·응답 (기존 그룹의 멱등 갱신 — floor·기존 멤버 포트 유지,
신규 멤버 포트 추가 할당). 없는 그룹이면 `NOT_FOUND` 에러 — 소실 그룹을 재생성하지
않는다 (재생성 시 포트가 재할당되어 client 캐시와 어긋난다). client 는 NOT_FOUND 수신 시
PTT_GROUP_ADD 로 재수립하고 응답 포트로 캐시를 갱신한다.

### 7.3 PTT_GROUP_REMOVE — 그룹 해제

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_id` | O | 그룹 식별자 |

그룹 floor 포트와 멤버 유닛 전체를 풀로 반환한다. 이미 없는 그룹이면 `OK` (자연 멱등 —
RELAY_REMOVE 와 동일 규칙).

### 7.4 PTT_JOIN — 멤버 참가 (2단 멱등)

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_id` / `session_id` | O | 대상 그룹 / 멤버 세션 ID |
| `user_ip` / `user_port` | - | 멤버 RTP 주소 (SDP answer 수신 후 전달 — ①단계 선할당 호출은 생략) |
| `user_floor_port` | - | 멤버 floor control 포트 |
| `user_video_port` | - | 멤버 Video RTP 포트 |
| `user_nat` | - | 1 이면 NAT 뒤 멤버 — 멤버 전용 포트에 목적지 latch 허용 (생략=0) |
| `user_sig_ip` | - | 멤버의 SIP 시그널링 실소스 IP — latch IP guard 기준 |
| `user_pt` / `user_te_pt` | - | 이 멤버가 **수신** 선언한 audio/telephone-event wire PT(멤버 자신의 SDP — 개시자=offer, 수신자=answer) — CMP 가 fan-out 으로 이 멤버에 송신 시 스탬프(leg 별 PT 재작성). 생략=0=재작성 없음(현행 PT-blind: 전 leg 와이어 PT 통일 전제) |
| `user_src_pt` / `user_src_te_pt` | - | 이 멤버가 **송신**에 쓰는 audio/TE PT(= CSP 가 그 leg 쪽에 낸 SDP 의 PT, RFC 3264) — 화자 ingress 의 audio/TE 분류 기준 + 녹취 세그먼트 메타(`audio_pt`, 화자 leg). `user_src_te_pt` 생략 시 TE 는 관례 PT 101 로 분류(DTMF push/release 판독도 동일 기준) |
| `user_codec` | - | 이 멤버의 협상 오디오 코덱 문자열(예 `"AMR-WB/16000"`) — 녹취 세그먼트 메타(`audio_codec`)용. CSP 는 코덱 테이블 top 의 rtpmap prefix 를 싣는다 |
| `role` | - | `chair`/`participant` (기본 participant) |
| `tier` | - | 긴급 멤버 join 시 condition tier 동반 |
| `recv_only` | - | 1 = ambient 청취 leg — 이 멤버의 **상향 미디어를 중계하지 않고** floor 요청도 거절(receive only) |
| `floor_suppress` | - | 1 = 이 멤버에게 floor 메시지(GRANT/TAKEN/IDLE/DENY)를 **보내지 않는다** — 청취 사실이 floor 상태로 드러나지 않게 한다 |
| `user_uri` | - | 이 멤버의 **MCPTT ID(URI)** — floor 메시지의 User ID(6)/Granted Party(4)/화자 리스트에 싣는 값(TS 24.380 §8.2.3.8). 생략 시 `session_id` |
| `queueing` | - | `0` = 이 멤버가 SDP `mc_queueing` 을 협상하지 않음 → 비선점 요청은 큐잉하지 않고 **Deny #1**(기본 1) |
| `max_priority` | - | SDP `mc_priority=N` 로 협상한 **요청 가능 최대 우선순위**. 이 값이 있을 때만 Floor Request 의 Floor Priority 로 우선순위를 낮출 수 있다(둘 중 낮은 쪽). 없으면(미협상) 요청의 우선순위 필드를 무시하고 `members` 의 기본값을 쓴다(TS 24.380 §6.3.5.4.4-1a) |
| `granted` | - | `1` = SDP fmtp `mc_granted` 협상 — 참가 시점에 발언자가 없으면 이 멤버에게 **초기 발언권**을 준다(TS 24.380 §6.3.4.2.2) |
| `floor_crypto` | - | 이 멤버의 floor SRTCP 키 `{alg,key,salt[,mki]}` — **유니캐스트 floor 는 클라이언트별 CSK 로 보호**(TS 33.180 §9.4)한다. 생략 시 그룹 키([§7.8](#78-floor_crypto--floor-rtcp-보호-ts-33180)) |

응답 payload: `ip`, `port`, `video_port` — **멤버 전용 RTP 포트** (client 는 이 포트를
그 멤버의 SDP 에 광고). 같은 `(group, session_id)` 재요청은 재할당 없이 동일 포트 반환.

늦은 참가자(초기 로스터 외 멤버)는 2단으로 호출한다:
① SDP offer 생성 전 `user_ip` 없이 JOIN → 멤버 포트 할당·응답,
② SDP answer 수신 후 동일 JOIN 으로 `user_ip`/`user_port` 등 주소 갱신 (멱등 갱신 경로).
초기 로스터 멤버는 PTT_GROUP_ADD 응답의 `member_ports` 가 ①을 대신하므로 ②만 호출한다.

주소가 갱신된 멤버의 NAT latch 상태는 리셋되어 재-latch 가 허용되며, 선언 주소·NAT
속성이 직전과 동일한 재요청(재전송, 세션 refresh)은 latch 를 유지한다 (RELAY_MODIFY 와
동일 규칙). 멤버가 re-INVITE 로 주소를 재협상하면 client 는 ② 를 다시 호출해 전달한다.
`user_video_port` 는 video 를 협상한 멤버만 싣는다 (비협상 멤버에 유령 포트 광고 금지).

**leg 별 PT 재작성** (`user_pt` 계열): fan-out 시 화자 leg 의 `user_src_te_pt` 로 패킷을
audio/TE 로 분류한 뒤, 각 수신 leg 의 `user_pt`/`user_te_pt` 를 스탬프한다(marker bit
보존, seq/SSRC 재작성과 같은 자리). 이로써 그룹 전 leg 와이어 PT 통일 전제가 풀려 —
개시자가 비 96 PT 로 offer 하거나 타사 단말이 비 96 으로 answer 해도 그룹이 정합된다.
TE 인데 수신 leg `user_te_pt` 미지정이면 원본 PT 를 유지한다(audio PT 로 뭉개면 DTMF
파손). 녹취는 화자 원본 PT 로 기록된다(egress 재작성 전 탭). PT 파라미터는 주소 불변
재-JOIN(재협상)에서도 항상 최신 선언으로 갱신된다.

### 7.5 PTT_LEAVE — 멤버 이탈

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_id` / `session_id` | O | 대상 그룹 / 멤버 세션 ID |

멤버 전용 포트 유닛을 풀로 반환한다. 이미 없는 그룹/멤버면 `OK` (자연 멱등).

### 7.6 PTT_FLOOR_TIER — 멤버 condition tier 런타임 갱신

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_id` / `session_id` | O | 대상 그룹 / 멤버 세션 ID |
| `tier` | O | `emergency`/`imminent`/`normal` |

긴급 개시/업그레이드/취소 시 호출. 미디어 재협상 불필요 (floor 우선순위만 변경).

### 7.7 floor 정책 — 동시 발언과 private call

floor 는 **직교하는 두 축**으로 정해진다. `floor_control` 이 제어의 **유무**, `floor_policy` 가
그 안에서의 **동시 발언 수**다. 정책은 세션 생성 시 1회 전달되고 이후 floor 절차는 CMP↔UE
in-band(RTCP APP "MCPT")로만 진행한다 — CSP 는 floor 루프에 들어가지 않는다.

| 조합 | 동작 |
|---|---|
| `floor_control:"off"` | 중재 없음. 모든 멤버의 상향을 나머지 전원에게 중계(full-duplex). `floor_port` 미광고, 수신 floor 메시지 무시. private call without floor 가 이 형태다 |
| `floor_policy:"single"` (기본) | 단일 화자. 점유 중 요청은 선점 서열 판정 → 선점(REVOKE 후 GRANT) 또는 큐잉/Deny |
| `floor_policy:"dual"` | 동시 최대 2명. **2번째 자리는 override 전용** — 선점 자격(tier>chair>priority)이 있는 요청만 기존 화자를 REVOKE 하지 않고 동시 GRANT 한다(TS 24.380 dual floor). 자격 없는 요청은 single 과 같이 큐잉/Deny |
| `floor_policy:"multi"` | 동시 최대 `max_talkers` 명. 정원 여유가 있으면 서열 비교 없이 즉시 GRANT, 정원이 차면 선점 판정(최약 화자 REVOKE) 또는 큐잉 (TS 24.380 Rel-16 multi-talker) |
| `group_type:"private"` | 2인 세션용 floor — 정원 1, **큐잉 없음**(점유 중 요청은 즉시 Deny), chair 개념 없음(tier·priority 만 비교). **초기 발언권은 PTT_JOIN `granted`(=fmtp `mc_granted` 협상) 로만 부여한다** — `initiator_id` 만으로 주지 않는다(협상하지 않은 단말에서 아무도 말하지 않는데 상대에게 Floor Taken 이 날아가 "수신 중" 으로 표시됨). group 의 `floor_policy` 는 해석하지 않는다. TS 24.380 은 온넷 private call 에 별도 floor 절차를 두지 않으므로(§6.3 공통) 이 3가지는 CMP 로컬 정책이며, 초기 발언권은 규격상 fmtp `mc_granted` 협상 결과여야 한다([../design/features/mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md) §0-R G17) |

동시 발언 시 in-band 표식과 메시지:

- Floor Granted/Taken 의 **Floor Indicator**(TS 24.380 §8.2.3.15)에 `multi` 는 Multi-talker 비트
  (0x0080), `dual` 은 화자가 2명일 때 Dual floor 비트(0x0200)를 세운다.
- 동시 발언 중의 **Floor Taken** 은 화자 전원을 **List of Granted Users**(15)+**List of SSRCs**(16)
  로 싣는다(단일 화자면 Granted Party + SSRC 필드). 화자 본인에게는 보내지 않는다.
- 화자 1명이 빠지면 **Floor Release Multi Talker**(subtype 0x0F)로 나머지 참가자에게 알리고
  (SSRC + User ID), 잔여 화자가 있는 동안 Floor Idle 은 보내지 않는다. 마지막 화자가 빠질 때만
  Floor Idle. 0x0F 는 **서버→단말 통지 전용**이라 단말이 이 subtype 을 보내면 무시한다
  (발언 해제는 Floor Release `0x04`/`0x14`).
- 타이머는 **화자별로 독립** 판정한다. 값은 CMP 설정이 기본이고 `floor_timers` 로 그룹마다
  덮어쓴다(TS 24.380 §11.1.3):

| 타이머 | 필드 / 설정 | 기본 | 만료 시 |
|---|---|---|---|
| T1 End of RTP media | `t1_end_rtp` / `FloorIdleSec` | 4초 | **발언 완료**로 회수 — Revoke 없이 잔여 화자 0x0F, 없으면 IDLE |
| T2 Stop talking | `t2_stop_talk` / `FloorStopTalkSec` | 30초 | Floor Revoke cause **#2**(Media burst too long). Granted 의 Duration 으로 광고. 긴급/임박 화자는 제외. 0=무제한 |
| T3 Stop talking grace | `t3_grace` / `FloorRevokeGraceSec` | 3초 | Revoke 후 Release 대기 유예 — 그 동안 미디어 계속 중계, 만료 시 강제 회수. 0=즉시 |
| T8 Floor Revoke | `t8_revoke` / `FloorRevokeRetxSec` | 1초 | 유예 중 Revoke 재전송 간격 |
| T7 Floor Idle | `t7_idle_resend` / `FloorIdleResendSec` | 0(비활성) | 발언자 없는 동안 Floor Idle 재송신(최대 3회) |
| T20 Floor Granted | `t20_grant_retx` / `FloorGrantRetxSec` | 1초 | **큐 승급** 화자에게 첫 RTP 까지 Granted 재송신(최대 3회) |

- **선점은 즉시 교체가 아니다**: 최약 화자에게 Revoke → 요청자는 **대기열 맨 앞**에서 대기
  (Queue Position Info 회신) → 그 화자의 Floor Release 또는 T3 만료 후 승급한다. 유예 중에도
  기존 화자의 미디어는 계속 중계되므로 발언이 뚝 끊기지 않는다.
- 발언 중인 참가자의 Floor Request 재전송에는 **Floor Granted 를 재송신**하고(남은 T2 를
  Duration 으로), 대기 중인 요청의 재전송은 **큐 위치를 유지**한 채 Queue Position Info 만
  다시 보낸다.

**하향 스트림 분리** — 동시 발언 화자는 슬롯(0..7)을 배정받고, 수신자에게 나가는 RTP 는
슬롯마다 별도 SSRC·시퀀스를 쓴다. 슬롯 0 은 종전과 동일한 수신자별 고정 SSRC(단일 화자
정책에서 화자가 바뀌어도 하나의 연속 스트림)라 기존 단말 동작은 그대로다.

**정책 변경(`PTT_GROUP_MODIFY`)** — 정책은 언제든 바꿀 수 있고 CMP 가 현재 상태를 정책에
맞춘다. 정원이 줄면(예: `multi`(3) → `single`, 또는 `floor_control:"off"` 전환) 초과 화자를
서열 최약자부터 **Floor Revoke** 로 회수한다(동급이면 나중에 발언을 시작한 화자부터 —
먼저 말하던 화자의 발언이 끊기지 않도록). `off` 로 바꾸면 대기열도 비우고 멤버마다 상향
스트림 슬롯을 재배정한다. `dual`↔`multi`↔`single` 전환은 이후 요청 판정부터 새 정책을 따른다.

**녹취** — 슬롯 0 은 `audio`/`video` 트랙(파일명 종전과 동일), 동시 발언 슬롯은
`audioN`/`videoN` 트랙에 기록한다. 세그먼트는 발언자 집합이 비는 시점에 닫힌다.

세그먼트가 여러 발언을 담으므로 **한 트랙 안에서 화자가 바뀔 수 있다** — 선점 회수로 비워진
슬롯을 다른 화자가 이어받는 경우다. 그래서 화자 귀속은 트랙당 한 값이 아니라 **구간 목록**
(`tracks[].speakers[] = {id, offset_ms, dur_ms}`)으로 남긴다. 트랙별 `pt`/`codec` 도 슬롯마다
따로 기록한다(화자 leg 가 이종 단말이면 협상 PT 가 다르다).
세그먼트 메타 전체 형식은 [../design/features/recording.md](../design/features/recording.md)
§3.3.1 이 정본이며, 구 소비자 호환용 flat 키(`audio_file`/`speaker_id_audioN`)도 함께 기록된다.

### 7.8 floor_crypto — floor RTCP 보호 (TS 33.180)

MCPTT E2E 보안에서 **미디어 RTP 는 CMP 가 복호하지 않는다**(UE↔UE SRTP 를 투명 relay).
반면 floor control 은 CMP 가 중재자로 참여하므로 floor RTCP 만 보호 키를 받아 SRTCP
(RFC 3711)로 복호·재암호한다. 키는 CSC(KMS)가 GMK/PCK 에서 파생해 CSP 를 거쳐 inline 으로
전달한다 — CMP 는 KMS 와 직접 접점을 갖지 않는다.

| `floor_crypto` 필드 | 필수 | 설명 |
|---|---|---|
| `alg` | - | `AES_CM_128_HMAC_SHA1_80`(기본) / `AES_CM_128_HMAC_SHA1_32` |
| `key` | O | master key — base64(16 바이트) |
| `salt` | O | master salt — base64(14 바이트) |
| `mki` | - | MKI — hex(≤16 바이트). 지정 시 송신 패킷에 동봉하고 수신 시 대조 |

```json
{
  "payload": {
    "group_id": "grp-fire-01",
    "floor_control": "on", "floor_policy": "multi", "max_talkers": 3,
    "floor_crypto": { "alg": "AES_CM_128_HMAC_SHA1_80",
                      "key": "4fl6DT4Bi+DWT6MsBt5BOQ==", "salt": "DsZ1rUmK/uu2lgs6q+Y=" }
  }
}
```

- 보호 범위: RTCP 헤더 8B 는 평문, 이후 본문 암호화 + `E|SRTCP index`(4B) + MKI(선택) +
  인증 태그. 인증은 (헤더+본문+E/index) 전체를 덮는다.
- 재전송 방지: SSRC 별 index 최고값 + 64 슬롯 윈도우. 인증 실패·재전송 패킷은 폐기하고
  STATS `detail.floor_crypto_drop` 에 누적한다(위조 floor 시도 관측).
- 키 갱신(rekey)은 같은 필드를 `PTT_GROUP_MODIFY` 로 다시 보내면 된다(새 키로 세션 키를
  재파생하고 index·재전송 윈도우를 초기화). **보호 해제는 지원하지 않는다** — 한 번 보호된
  그룹을 세션 도중 평문으로 되돌리는 downgrade 경로를 두지 않는다. 평문이 필요하면 새 그룹으로 수립한다.
- `floor_control:"off"` 와 함께 오면 `BAD_REQUEST`(보호할 floor 가 없다). 키 길이·alg 오류도
  같은 코드로 거절한다.

## 8. 이벤트 (type: "event")

CMP → client 비동기 push. `trans_id` 는 CMP 가 발행하고, 수신 client 는 동일 trans_id 로
`type:"response", status:"OK"` 를 반환(ack)한다. ack 미수신 시 CMP 가 1s 간격 최대 5회
재전송하고 그 뒤 폐기한다(CSP 재기동 구간 손실 허용 — [§5.1](#51-heartbeat) digest audit 이 보완).
push 대상은 마지막 제어 요청(HEARTBEAT 등)의 소스로 학습한 CSP 소켓 주소다([§4](#4-자원-모델과-이벤트-라우팅)).

**sweeper 회수 통지** — CMP sweeper(`timeoutLoop`)가 RTP 무활동 자원을 자체 회수할 때 발행한다:

| cmd | 라우팅 | payload |
|---|---|---|
| `RELAY_ABORTED` | 소유 node | `session_id`, `reason`(`orphan_no_rtp`=무RTP setup 실패 / `hold_timeout`=RTP 후 유휴), `held_sec` |
| `PTT_GROUP_ABORTED` | 참여 node 전체 | `group_id`, `reason`(`idle_no_members`) |

```json
{
  "hdr": { "ver": 2, "trans_id": 90001, "node": "cmp01", "cmd": "RELAY_ABORTED",
           "type": "event", "sesid": "01011112222::csp::1768531200123456::7", "service": "volte" },
  "payload": { "session_id": "a84b4c76e66710", "reason": "orphan_no_rtp", "held_sec": 7300 }
}
```

**발언자 집합 통지** — floor 는 CMP↔UE in-band 라 "지금 누가 말하는가"를 CSP·콘솔이 알 길이
없다. 발언자 집합이 바뀔 때마다(GRANT/RELEASE/REVOKE/이탈) 발행한다:

| cmd | 라우팅 | payload |
|---|---|---|
| `FLOOR_TALKERS` | 참여 node | `group_id`, `policy`(`single`/`dual`/`multi`/`private`/`off`), `talkers`(현재 발언자 배열 — 비면 무발언) |

```json
{
  "hdr": { "ver": 2, "trans_id": 90101, "node": "cmp01", "cmd": "FLOOR_TALKERS",
           "type": "event", "sesid": "01011112222::csp::1768531200123456::7", "service": "mcptt" },
  "payload": { "group_id": "grp-fire-01", "policy": "multi",
               "talkers": ["01011112222", "01033334444"] }
}
```

**CSP 처리** — CSP CmpClient 가 event 를 ack 한 뒤 별도 dispatch 스레드에서 처리한다(핸들러가
RemoveSession 을 부를 수 있어 수신 스레드와 분리 — 데드락 회피). `RELAY_ABORTED` → 해당
`session_id` 를 가진 호를 즉시 종료(StopCall+RemoveSession, 멱등). `PTT_GROUP_ABORTED` → 그룹
캐시(sesid)를 정리해 다음 사용 시 재수립. **회수(teardown)는 active 역할일 때만** 수행하고
standby 는 탐지·로그만 한다([ha_design.md](../design/ha_design.md) §5.6 역할 게이트와 동일).
이벤트는 CMP 가 자원을 확정 회수했다는 authoritative 신호라 audit(pull)의 zombie 추정과 달리
opt-in 없이 즉시 처리하며, [§5.1](#51-heartbeat) digest-on-HB audit 과 **상보적**이다 — 이벤트는
회수 즉시 특정 세션을 지목해 수렴 지연을 단축하고, audit 은 이벤트 유실·이중화 절체까지 커버한다.

`FLOOR_TALKERS` 는 CSP 가 ack 만 하고(미소비) 있다 — 로스터·녹취 태깅·콘솔 실시간 반영은
Call Control 파트의 후속 과제다([mcptt_csp_cmp_roadmap_contract.md](../design/features/mcptt_csp_cmp_roadmap_contract.md) §B.4).
콘솔은 그때까지 STATS `detail.groups[].floor_holders` 폴링으로 발언자를 표시한다.

> **RELAY_NAT_LATCHED**(NAT 목적지 latch 통지)는 규격 예약 — 현행은 STATS `detail.nat`/로그로 관측한다.

## 9. 에러 코드

`status:"ERROR"` 응답의 `hdr.code`. `reason` 은 자유 문자열(진단용)로 wire 계약이 아니다.

| code | 의미 |
|---|---|
| `UNKNOWN_CMD` | 인식할 수 없는 cmd |
| `BAD_REQUEST` | `hdr` 부재 등 envelope 형식 오류, 필수 필드 누락 |
| `NO_RESOURCE` | 자원 풀 고갈 (relay/ptt 포트) |
| `NOT_FOUND` | 대상 자원 없음 (group/session) |
| `UNSUPPORTED_VER` | 지원하지 않는 `hdr.ver` |
| `INTERNAL` | CMP 내부 오류 |
