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
(권장 3초, 연속 3회 무응답 시 Disconnected 판정 — client 정책). 이벤트 채널이
활성화되면 CMP 는 이 요청의 `hdr.node` + 소스 주소로 **이벤트 push 대상 레지스트리**를
유지한다 (예약 — [§8](#8-이벤트-type-event). 현행 CMP 는 요청-응답만 지원하므로
레지스트리를 두지 않는다).

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
      "nat": [ { "key": "a84b4c76e66710", "leg": "b",
                 "learned_ip": "203.0.113.7", "learned_port": 41022 } ],
      "groups": [ { "group_id": "grp-fire-01", "members": 5, "floor_holder": "01011112222" } ]
    }
  }
}
```

| detail 필드 | 의미 |
|---|---|
| `rtp_src_drop` | 미협상 소스 드롭 누적 카운터 (no-NAT leg 의 선언 주소 불일치 패킷). 해제된 자원의 몫을 이월한 **단조 증가** 값 |
| `nat` | NAT latch 완료 leg 목록 — `key`(session_id 또는 `group_id:member`), `leg`(`a`/`b`/멤버 sid), 학습된 실주소. 최대 20개 (전체 수는 `nat_total`) |
| `groups` | 활성 그룹별 상세 (`group_id`/`members`/`floor_holder`). 최대 20개 (전체 수는 `groups_total`) |

`nat`/`groups` 배열 상한은 응답 datagram 4KB 계약([§1.2](#12-전송)) 내 안전 상한이다.

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
| `caller` / `callee` | - | 발/착신자 (flow 로깅·녹취 메타용) |
| `record_dir` | - | 녹취 디렉토리 (있으면 녹취 시작) |

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

### 7.1 PTT_GROUP_ADD — 그룹 생성 (멱등)

| payload 필드 | 필수 | 설명 |
|---|---|---|
| `group_id` | O | 그룹 식별자 |
| `members` | - | `"sid:prio[:role[:tier]],..."` CSV (role=`chair`/`participant`, tier=`emergency`/`imminent`/`normal`) |
| `subid` | - | 그룹 세션 회차 (flow 로그 subid) |
| `video_enabled` | - | 1 이면 video 포트 활성 |
| `group_type` | - | `prearranged`/`chat`/`broadcast` (broadcast floor 독점 정책 — TS 24.380 §10.3) |
| `initiator_id` | - | broadcast 개시자 sessionId |
| `record_dir` | - | 녹취 디렉토리 (있으면 녹취 시작) |

응답 payload: `ip`, `floor_port`(그룹 공유 floor control 포트), `member_ports`
(멤버별 전용 RTP 포트 맵 — members 로 전달된 초기 로스터에 대해 할당).
기존 그룹 재요청 시 재할당 없이 members 만 갱신하고 동일 포트를 응답한다.

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
| `role` | - | `chair`/`participant` (기본 participant) |
| `tier` | - | 긴급 멤버 join 시 condition tier 동반 |

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

## 8. 이벤트 (type: "event")

CMP → client 비동기 push. `trans_id` 는 CMP 가 발행하고, 수신 client 는 동일 trans_id 로
`type:"response", status:"OK"` 를 반환(ack)한다. ack 미수신 시 CMP 가 1s 간격 최대 5회
재전송한다. push 대상은 HEARTBEAT 로 유지되는 노드 레지스트리에서 [§4](#4-자원-모델과-이벤트-라우팅)
의 라우팅 규칙으로 정한다.

> **미구현 — 규격 예약.** 현행 CMP 는 요청-응답만 지원한다. 초기 이벤트 후보:

| cmd | 라우팅 | payload |
|---|---|---|
| `RELAY_ABORTED` | 소유 node | `session_id`, `reason`(`orphan_no_rtp`/`hold_timeout`/...), `held_sec` |
| `PTT_GROUP_ABORTED` | 참여 node 전체 | `group_id`, `reason` |
| `RELAY_NAT_LATCHED` | 소유 node | `session_id`, `peer_index`, `learned_ip`, `learned_port` — NAT 목적지 latch 통지 (현행은 STATS/로그로 관측) |

```json
{
  "hdr": { "ver": 2, "trans_id": 90001, "node": "cmp01", "cmd": "RELAY_ABORTED",
           "type": "event", "sesid": "01011112222::csp::1768531200123456::7", "service": "volte" },
  "payload": { "session_id": "a84b4c76e66710", "reason": "orphan_no_rtp", "held_sec": 7300 }
}
```

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
