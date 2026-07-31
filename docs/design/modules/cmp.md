# 11. CMP (Component Media Provider) 모듈 상세 설계

## 1. 개요

CMP는 CIMS 시스템의 미디어 서버로, CSP의 제어 하에 RTP relay, PTT 오디오 믹싱, MCPTT Floor Control을 수행한다.

**서비스 표준 코덱**: VoLTE/PTT 음성 = **AMR-WB** (`AMR-WB/16000/1`), 영상 = **H.264**
(`H264/90000`). 음성 PT 의 정본은 CSP 코덱 테이블(`Setup.Media.Codecs`,
[csp.md](csp.md) §6.1 — 기본 AMR-WB=96)이며, CMP 는 코덱을 해석하지 않는다(트랜스코딩
없음). wire PT 는 기본 무재작성(PT-blind, seq/SSRC 만 재작성)이되, 제어평면이 leg 별 PT
(`user_pt`/`remote_pt` 계열 — [cmp_media_api.md](../../api/cmp_media_api.md) §6.1/§7.4)를
선언하면 **egress 에서 leg 별 audio/telephone-event PT 를 재작성**해 leg 간 PT 불일치
(비 96 offer/answer 단말)를 정합한다. 녹취는 화자 원본 PT 로 기록되며, 녹취 변환
파이프라인([../features/recording.md](../features/recording.md))은 AMR-WB/H.264 를 전제한다.
시험(cspsim) 시에도 AMR-WB 미디어 파일 지정이 필수 —
[../../VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md) 부록 "기본 호시험" 참조.

### 1.1 핵심 기능

| 기능 | 설명 |
|------|------|
| VoIP RTP Relay | 1:1 통화 양방향 RTP/RTCP/Video 중계 |
| PTT 그룹 오디오 | 다자 RTP 수신 → 현재 화자 오디오만 전체 송출 |
| MCPTT Floor Control | RTCP APP 기반 발언권 관리 (m=application 전용 소켓) |
| DTMF PTT | DTMF 숫자로 Floor REQUEST/RELEASE (레거시 단말 대응) |
| 녹취 | VoIP 양방향 / PTT 세션 단위 raw RTP 저장 |
| 세션 타임아웃 | 무활동 세션 자동 정리 |

### 1.2 프로세스 구성

```
bin/cmp <config.json>
  config.json : 설정 파일 경로 (기본: cmp.json)
```

---

## 2. 아키텍처

### 2.1 전체 구조

```
CSP ──(JSON/UDP 9000)──→ CmpServer
                            │
                   ┌────────┼────────┐
                   │        │        │
              VoIP Pool  PTT Pool  Group Map
              (PRtpRelay) (PRtpMulticast/PPttMemberPort) (PMcpttGroup)
                   │        │        │
                   └────────┼────────┘
                            │
                     RTP Worker Threads
                     (PModule/PHandler)
```

### 2.2 VoIP/PTT 핸들러 분리

VoIP와 PTT는 용도별로 핸들러를 분리한다:

| 구분 | 핸들러 | 소켓 구성 | 포트 블록 |
|------|--------|-----------|-----------|
| VoIP | PRtpRelay | peer 별 Audio RTP/RTCP + Video RTP/RTCP | 8포트 (leg 별 4포트 블록 × 2) |
| PTT | PRtpMulticast (그룹 공유 Floor) + PPttMemberPort (멤버 Audio/Video RTP) | Floor 그룹당 1 + 멤버당 Audio/Video 각 1 | 독립 대역 |

**분리 이유:**
- PTT는 RTCP 불필요 (Floor를 m=application 전용 소켓으로 처리)
- 비디오 포트 불필요 (PTT 비디오는 향후 확장)
- 포트 대역 분리로 방화벽/NAT 설정 단순화
- 리소스 풀 독립 관리 (VoIP 고갈이 PTT에 영향 없음)

---

## 3. 클래스 상세

### 3.1 CmpServer

**파일:** `CmpServer.h/.cpp`

UDP 제어 채널 리스너. CSP로부터 JSON 명령을 수신하여 디스패치.

**상속:** `PModule` (pasf 프레임워크의 모듈 베이스)

**주요 멤버:**

```cpp
class CmpServer : public PModule {
    // 세션/그룹 관리
    std::map<std::string, PRtpRelay*> _sessions;   // VoIP 세션
    std::map<std::string, McpttGroup*> _groups;     // PTT 그룹
    std::map<std::string, std::string> _sesidMap;   // key → sesid (CSP 발급 상속)
    std::map<std::string, std::string> _serviceMap; // key → service (volte/mcptt/...)

    // Flow 로그 항목별 활성화 플래그 (cmp.json ServiceLogging.Flow)
    bool _logFlowFloor;   // Floor opcode
    bool _logFlowDtmf;    // DTMF (RFC2833/4733)
    bool _logFlowRtcp;    // RTCP SR/RR/SDES/BYE

    // VoIP 리소스 풀
    std::vector<PRtpRelay*> _resourcePool;
    std::vector<PRtpRelay*> _freeResources;

    // PTT 리소스 풀
    std::vector<PRtpMulticast*> _pttPool;          // 그룹 공유 floor
    std::vector<PRtpMulticast*> _freePttResources;
    std::vector<PPttMemberPort*> _pttMemberPool;   // 멤버 전용 포트 유닛
    std::vector<PPttMemberPort*> _freePttMembers;
    std::map<std::string, PPttMemberPort*> _memberUnits;  // "groupId|sessionId" → unit
};
```

**Flow/Msg 로깅 공통 필드:**

- CSP 가 hdr 에 동봉한 `service` / `sesid`, payload 의 `caller` / `callee` 를 key(session_id/group_id) 별로 저장
- 이후 응답 및 후속 이벤트(RTP/Floor/DTMF/RTCP) 로그에 동일 값을 상속하여 **CSP↔CMP 양측 Flow 가 단일 sesid 로 묶이도록** 보장
- Flow 로그 필드 순서·생략 규칙은 CSP 측과 동일 (`ts, service, caller, callee, sesid, subid, node, from, to, proto, method, detail, mid, seq, iface`)
- 전체 규격은 [../features/flow_logging.md](./../features/flow_logging.md) 참고

**초기화 순서:**

```
CmpServer(name, configFile)
  1. loadConfig()  ── 설정 파일 파싱
  2. Worker 스레드 생성 (RtpWorker_0 ~ RtpWorker_N)
  3. initResourcePool()    ── VoIP PRtpRelay 풀 생성 (호당 8포트)
  4. initPttResourcePool() ── PTT 그룹 floor(PRtpMulticast) 풀 생성
  4'. initPttMemberPool()  ── PTT 멤버 포트 유닛(PPttMemberPort) 풀 생성
```

**UDP JSON 프로토콜 (envelope v2):**

wire 규격 정본은 [../../api/cmp_media_api.md](../../api/cmp_media_api.md) 다. 메시지는
`{hdr, payload}` 두 최상위 키로 구성되고, `hdr` 에 ver/trans_id/node/cmd/type 과
호 문맥 명령의 sesid/service (+응답 status/code/reason)가, `payload` 에 cmd 별 업무
필드만 실린다:
```json
{
  "hdr": { "ver": 2, "trans_id": 1001, "node": "csp_01", "cmd": "RELAY_ADD",
           "type": "request", "sesid": "caller::csp::...", "service": "volte" },
  "payload": { "session_id": "sess_001", "remote_ip": "192.168.1.100",
               "remote_port": 30000, "record_dir": "/data/service_log/voip/.../sess.d" }
}
```
```json
{
  "hdr": { "ver": 2, "trans_id": 1001, "node": "cmp_01", "cmd": "RELAY_ADD",
           "type": "response", "sesid": "caller::csp::...", "service": "volte",
           "status": "OK" },
  "payload": { "local_ip": "192.168.1.10", "local_port": 50000, "local_video_port": 50002 }
}
```

`cmd` 는 수신 시 대문자로 정규화해 비교한다. 에러 응답은 `hdr.status="ERROR"` +
구조화 코드(`code`: UNKNOWN_CMD/BAD_REQUEST/NO_RESOURCE/NOT_FOUND/UNSUPPORTED_VER,
`reason`: 자유 문자열)로 돌려준다. 아래 §3.2 는 명령별 payload 필드와 내부 동작을
서술한다 — envelope 규칙·에러 코드 정의는 API 문서를 본다.

### 3.2 명령 상세

#### RELAY_ADD — VoIP 세션 생성

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| session_id | O | 세션 식별자 |
| remote_ip | O | 상대방 RTP IP (SDP 선언). `0.0.0.0`(또는 port 0) = 주소 미확정 — 목적지 미설정, 이후 MODIFY 로 확정 |
| remote_port | O | 상대방 RTP 포트 |
| remote_video_port | - | 상대방 Video RTP 포트 |
| peer_index | - | 피어 인덱스 (0=발신 A / 1=착신 B) |
| remote_nat | - | 1 이면 해당 peer 전용 포트에 NAT 목적지 latch 허용 ([ue_nat_traversal.md](../features/ue_nat_traversal.md)) |
| remote_sig_ip | - | 해당 peer 의 SIP 시그널링 실소스 IP — latch IP guard |
| record_dir | - | 녹취 디렉토리 경로 |

**응답:** `local_ip`, `local_port`/`local_video_port` (peer0 전용),
`local_port_b`/`local_video_port_b` (peer1 전용). 각 포트의 RTCP 는 +1.

**동작:**
1. `_freeResources`에서 PRtpRelay(8포트 블록) 할당
2. 원격 피어 주소·NAT 정책 설정 (`setRemote`)
3. record_dir 있으면 녹취 시작
4. SESSION_START flow 로그 기록 (통합 ServiceLogDir)

#### RELAY_REMOVE — VoIP 세션 해제

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| session_id | O | 세션 식별자 |

**동작:** 녹취 중지 → reset() → freeResource() → 세션/로그 맵 삭제

#### RELAY_MODIFY — VoIP 세션 수정

processAdd()로 위임 — 기존 세션의 피어 주소만 갱신한다. 세션이 없으면 `NOT_FOUND`
에러 (소실 세션을 부활시키지 않는다).

#### PTT_GROUP_ADD — PTT 그룹 생성

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| members | - | "sid1:prio1:role,sid2:prio2:role" CSV (role=chair/participant) |
| subid | - | 그룹 세션 회차 (Flow 로그 subid) |
| record_dir | - | 녹취 디렉토리 |
| video_enabled | - | 1 이면 video 포트 활성 |
| group_type | - | `prearranged`/`chat`/`broadcast`/`private` (broadcast=개시자 독점, private=1:1 private call) |
| initiator_id | - | 개시자 sessionId(=userId) — broadcast floor 독점 / private 초기 발언권 |
| floor_control | - | `on`(기본)/`off` — floor 중재 유무 |
| floor_policy | - | `single`(기본)/`dual`/`multi` — 그룹 동시 발언 수 |
| max_talkers | multi 시 O | 동시 발언 상한(2..8) |
| floor_crypto | - | floor RTCP SRTCP 보호 키 `{alg,key,salt[,mki]}` (TS 33.180) |

**응답:** `ip`, `floor_port` (그룹 공유 Floor Control — `floor_control:"off"` 면 생략),
`member_ports` (멤버별 전용 RTP 포트 맵 — sid → `{port, video_port}`)

**동작:**
1. PMcpttGroup 생성
2. `_freePttResources`에서 PRtpMulticast(그룹 공유 floor 포트) 할당
3. PRtpMulticast ↔ PMcpttGroup 연결 (`setGroup`, `setPttSession`)
4. DTMF 설정 전달
5. 녹취/로그 설정
6. members CSV 파싱 → 우선순위/role 설정 + 멤버별 전용 포트 유닛(PPttMemberPort) 선할당.
   멤버 pool 고갈 시 `NO_RESOURCE` — 이번 호출로 생성된 그룹이면 floor/유닛을 즉시 롤백
   (기존 그룹의 선할당 유닛은 유지 — 멱등 재시도 시 재사용)
7. `group_type`/`initiator_id` → `setBroadcast()`. **broadcast** 그룹은 `handleFloorRequest` 가 개시자(`_initiatorSessionId`) 외 모든 floor REQUEST 를 REJECT(`floor.jsonl reason=broadcast`) — TS 24.380 §10.3.
8. `floor_control`/`floor_policy`/`max_talkers`/`group_type:"private"` → `setFloorPolicy()` (녹취 슬롯 트랙 수가 정원에 따라 정해지므로 녹취 초기화보다 먼저), `floor_crypto` → `setFloorCrypto()`.
   정책 필드의 미상 값·키 길이 오류는 `BAD_REQUEST` 로 거절한다.

#### PTT_GROUP_MODIFY — 그룹 멤버/우선순위 갱신

processAddGroup()으로 위임 — 기존 그룹의 members 를 재할당 없이 갱신하고 동일
`ip/floor_port/member_ports` 를 응답한다 (신규 멤버는 포트 추가 할당). 그룹이 없으면
`NOT_FOUND` (소실 그룹을 재생성하지 않는다 — CSP 는 PTT_GROUP_ADD 로 재수립).

#### PTT_JOIN — 멤버 참가

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| session_id | O | 멤버 세션 ID |
| user_ip | - | 멤버 RTP IP (①선할당 호출은 생략 — 2단 멱등, [api/cmp_media_api.md §7.4](../../api/cmp_media_api.md)) |
| user_port | - | 멤버 Audio RTP 포트 |
| user_floor_port | - | 멤버 Floor Control 포트 |
| user_video_port | - | 멤버 Video RTP 포트 |
| user_nat | - | 1 이면 멤버 전용 포트에 NAT 목적지 latch 허용 |
| user_sig_ip | - | 멤버의 SIP 시그널링 실소스 IP — latch IP guard |
| role | - | `chair`/`participant` (floor 선점 판정용, 기본 participant) |
| tier | - | `emergency`/`imminent`/`normal` — 긴급 멤버 join 시 동반 |
| recv_only | - | 1 = ambient 청취 leg — 상향 미중계 + floor 요청 거절 |
| floor_suppress | - | 1 = 이 멤버에게 floor 메시지 미송신 (청취 은닉) |

**응답:** `ip`, `port`, `video_port` — 멤버 전용 RTP 포트 (같은 멤버 재요청 시 동일 포트).

**동작:** 멤버 포트 유닛(PPttMemberPort) 확보(멱등) 후, 주소 동반 시
PMcpttGroup::addMember(). 발언 중인 화자가 있으면 신규 멤버에게 화자마다 FLOOR_TAKEN 통지
(private call 은 개시자 합류 시 초기 GRANT).

#### PTT_LEAVE — 멤버 퇴장

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| session_id | O | 멤버 세션 ID |

**동작:** PMcpttGroup::removeMember() + 멤버 포트 유닛 반환. Floor 소유자 퇴장 시 FLOOR_IDLE 브로드캐스트.
이미 없는 그룹이면 `OK` (자연 멱등).

#### PTT_GROUP_REMOVE — 그룹 해제

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |

**동작:** PRtpMulticast(floor) + 멤버 포트 유닛 전체 반환 → PMcpttGroup delete → 맵 삭제.
이미 없는 그룹이면 `OK` (자연 멱등).

#### PTT_FLOOR_TIER — 멤버 floor tier 런타임 변경

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| session_id | O | 대상 멤버 세션 ID |
| tier | O | `emergency` / `imminent` / `normal` |

**응답:** `status: OK` (그룹 없음/세션 미지정 시 `status: ERROR, code: NOT_FOUND`)

**동작:** McpttGroup::setTier() 호출 — 긴급/임박 업그레이드·취소 시 미디어 재협상 없이
floor 선점 우선순위만 갱신한다 (TS 24.380 tier 판정, [../features/mcptt_emergency_modes.md](../features/mcptt_emergency_modes.md)).
CSP 는 re-INVITE(`emergency-ind`/`imminentperil-ind`) 처리 경로(GroupCallService)에서 송신한다.

#### STATS — 통계 조회

CSP 는 이 명령을 보내지 않는다 — OAM stats 핸들러와 검증 파이프라인(stage6)이 직접 조회한다.

**응답 payload** — HEARTBEAT 와 동일한 `resource` 요약 + `detail` 진단 섹션:
```json
{
  "resource": {
    "relay": { "total": 20, "used": 7, "sessions": 5 },
    "ptt":   { "total": 50, "used": 2, "groups": 2, "joined": 4 }
  },
  "detail": {
    "session_timeout": 600,
    "orphan_reclaim_sec": 120,
    "leak_reclaim": { "total": 0, "orphan": 0, "hold": 0 },
    "groups": [ { "group_id": "group_1", "members": 4, "floor_holders": ["1001"] } ]
  }
}
```

- `relay` = VoIP 풀(`_freeResources`/`PRtpRelay`), `ptt` = 그룹(floor) 풀 + 멤버 유닛 풀(`member_total`/`member_used`).
  OAM stats 핸들러가 이를 flat 키(`rtp_ports_*`/`ptt_rtp_ports_*`)로 정규화해 대시보드에 전달.

#### HEARTBEAT — 연결 확인 + 자원 요약

CSP 가 3초 주기로 송신 (hdr-only, sesid/service 없음). **응답 payload** 에 `resource`
요약(STATS 의 resource 와 동일 구조)이 동봉된다 — client 는 이를 부하 기반 CMP 선택·조기
호 거절에 쓸 수 있고, `resource` 의 키 목록이 곧 CMP 의 기능(function) 광고다.

---

### 3.3 PRtpRelay (VoIP 핸들러)

**파일:** `PRtpRelay.h/.cpp`

**상속:** `PHandler` (pasf 프레임워크의 핸들러 베이스)

**소켓 구성 (leg 별 4포트 블록 × 2 = 8포트,
[ue_nat_traversal.md §3.1](../features/ue_nat_traversal.md)):**

```
peer0 (발신 A)                     peer1 (착신 B)
basePort+0 : Audio RTP             basePort+4 : Audio RTP
basePort+1 : Audio RTCP            basePort+5 : Audio RTCP
basePort+2 : Video RTP             basePort+6 : Video RTP
basePort+3 : Video RTCP            basePort+7 : Video RTCP
```

각 peer 는 자기 전용 포트로만 송신한다 — **수신 소켓이 곧 peer 신원**이며, 소스 주소는
검증용이다(선언 주소 불일치 = 드롭 + `rtp_src_drop` 카운터, nat leg 는 목적지 latch).
하향 송신도 그 peer 의 소켓에서 나가 소스 포트 = 광고 포트(symmetric RTP 정합).

**듀얼 leg 구조:**

```cpp
struct Leg {
    PRtpSocket rtp, rtcp, videoRtp, videoRtcp;   // 전용 소켓
    std::string ip;                              // 상대 주소 (SDP 선언 → latch 시 학습 주소)
    unsigned int port, videoPort;
    sockaddr_in addrRtp, addrRtcp, addrVideoRtp, addrVideoRtcp;   // 송신 목적지
    bool nat;              // 제어평면(RELAY_ADD remote_nat)이 지정한 leg 만 latch 허용
    std::string sigIp;     // latch IP guard 기준
    bool latched; uint32_t latchSsrc;   // latch 후 SSRC 고정 (재-latch 는 동일 SSRC 만)
};
Leg _legs[2];  // B2BUA 양 leg
```

**RTP Relay 로직 (proc()) — epoll 리액터가 패킷 도착 시 호출:**

```
proc()
  └─ 각 leg i 에 대해:
      ├─ Audio RTCP 수신 (leg[i].rtcp)
      │   ├─ 소스 검증 (선언 IP + RTP포트+1) — nat leg 는 관측 소스로 목적지 교정
      │   └─ leg[1-i].rtcp 소켓에서 반대편 목적지로 relay
      ├─ Audio RTP 수신 (leg[i].rtp)
      │   ├─ 소스 검증 — nat leg 는 목적지 latch(아래) / 불일치 드롭+카운터
      │   ├─ touchActivity() (유효 수신만 — 무효 트래픽 세션은 orphan 조기 회수)
      │   ├─ leg[1-i].rtp 소켓에서 반대편 목적지로 relay
      │   └─ 녹취: track a/b = 수신 소켓 기준 (발/착 귀속 항상 정확)
      ├─ Video RTP / Video RTCP — 동일 원리
```

**NAT 목적지 latch (nat leg 전용, [ue_nat_traversal.md §5](../features/ue_nat_traversal.md)):**

leg 전용 포트가 신원을 확정하므로 latch 는 신원 판정이 아니라 **송신 목적지 학습**이다.
`RELAY_ADD`/`RELAY_MODIFY` 의 `remote_nat=1` leg 에서만:

1. 첫 유효 RTP(version 2 + 최소 길이 + `remote_sig_ip` 있으면 소스 IP 일치) 소스를
   그 leg 의 송신 목적지로 latch + SSRC 고정.
2. 이후 소스 갱신(재-latch)은 **동일 SSRC** 일 때만 — NAT rebind 추종 + 제3자 주입 차단.
3. RTCP 목적지는 latch 된 IP 의 관측 RTCP 소스로 교정(관측 전에는 선언 포트+1 추정).
4. 주소 갱신(re-INVITE, `setRemote`) 시 latch 리셋 → 재-latch 허용. 선언이 직전과
   동일한 재요청(refresh/재전송)은 latch 유지.
5. latch 발생 INFO 로그 + `STATS detail.nat` 에 학습 주소 노출.

**녹취:**

```cpp
void startRecording(const std::string& rawDir, const std::string& sessionId);
// rawDir/seg_NNNN_a.rtp  — peer[0](발신) 수신 오디오
// rawDir/seg_NNNN_b.rtp  — peer[1](착신) 수신 오디오
// rawDir/seg_NNNN_va.rtp — peer[0] 비디오
// rawDir/seg_NNNN_vb.rtp — peer[1] 비디오
```

### 3.4 PRtpMulticast (그룹 floor 유닛) + PPttMemberPort (멤버 포트 유닛)

**파일:** `PRtpMulticast.h/.cpp`, `PPttMemberPort.h/.cpp`

**상속:** `PHandler`

PTT 미디어는 leg 별 포트셋을 따른다 ([ue_nat_traversal.md §3.2](../features/ue_nat_traversal.md)):

- **PRtpMulticast** — 그룹당 공유 **floor control 소켓 1개** (`PttFloorStartPort + N*2`).
  floor 메시지(RTCP APP "MCPT")는 TS 24.380 User ID 가 in-band 신원이라 공유 포트로 충분.
  proc(): floor 수신 → `PMcpttGroup::onFloorPacket()`.
- **PPttMemberPort** — 멤버당 전용 **audio RTP**(`PttRtpStartPort + N*2`) +
  **video RTP**(`PttVideoStartPort + N*2`) 소켓. 유닛의 포트가 그 멤버의 SDP 에 광고되어
  **수신 소켓이 곧 멤버 신원**이고, 하향 송신도 이 소켓에서 나간다(symmetric RTP 정합).
  proc(): 수신 → `PMcpttGroup::onMemberRtpPacket(memberId, ...)` /
  `onMemberVideoRtpPacket(memberId, ...)`.

```
멤버 유닛 audio 수신 → onMemberRtpPacket(memberId, ip, port, buf, len)
  ├─ 소스 검증 (선언 주소) — nat 멤버는 목적지 latch / 불일치 드롭+카운터
  ├─ DTMF 감지 (PT=101, end bit)
  └─ 발언 중인 화자(floor off 면 전원)의 오디오를 나머지에게 송출
     (각 멤버 유닛 소켓에서 하향 분배, 화자 슬롯별 SSRC/seq — recv_only 멤버 상향은 제외)
```

### 3.5 PMcpttGroup

**파일:** `PMcpttGroup.h/.cpp`

PTT 그룹 오디오 분배 및 MCPTT Floor Control.

**멤버 구조:**

```cpp
struct Peer {
    std::string id;           // 세션 ID
    std::string ip;           // 멤버 주소 (SDP 선언 → NAT latch 시 학습 주소)
    int port;                 // Audio RTP 포트
    int floorPort;            // Floor Control 포트 (m=application)
    int videoPort;            // Video RTP 포트
    unsigned int ssrc;        // CMP 할당 SSRC
    uint16_t audioSeqOut[8];  // 수신자별 오디오 시퀀스 카운터 (동시 발언 슬롯별)
    uint16_t videoSeqOut[8];  // 수신자별 비디오 시퀀스 카운터 (슬롯별)
    int  streamSlot;          // floor off(full-duplex) 시 이 멤버 상향의 고정 슬롯
    bool recvOnly;            // ambient 청취 leg — 상향 미중계 + 발언 거절
    bool floorSuppress;       // 이 멤버에게 floor 메시지 미송신
    PPttMemberPort* unit;     // 멤버 전용 RTP 포트 유닛 (수신 신원 + 하향 송신 소켓)
    bool natEnabled;          // PTT_JOIN user_nat — 목적지 latch 허용
};
```

**수신자별 SSRC/시퀀스 재작성 (화자 슬롯별):**

```cpp
void sendAudioToAll(data, len, excludeSessionId, slot) {
    for (auto& [sid, peer] : _members) {
        if (sid == excludeSessionId) continue;   // 발신자 제외
        // 패킷 복제 후 수신자별 SSRC + 시퀀스 재작성
        char pkt[4096];
        memcpy(pkt, data, len);
        peer.audioSeqOut[slot]++;
        // RTP 헤더 seq(offset 2-3)  = peer.audioSeqOut[slot]
        // RTP 헤더 ssrc(offset 8-11) = 슬롯 0 → 0x10000000+peer.ssrc (종전 고정 SSRC),
        //                              슬롯 k → 0x40000000+(k<<24)+peer.ssrc
        peer.unit->sendAudioTo(peer.ip, peer.port, pkt, len);   // 멤버 전용 유닛 소켓에서 송신
    }
}
```

각 수신자는 화자 슬롯마다 연속적인 시퀀스 번호와 고정 SSRC를 받아 jitter buffer 오동작을
방지한다. 단일 화자 정책에서는 슬롯 0 하나만 쓰이므로 화자가 바뀌어도 하나의 연속 스트림이다.

#### Floor Control 상태 머신

floor 상태는 **발언자 집합**(`_talkers`, 정원 = `_talkerCapacity`)이다. 정원은
`floor_control`/`floor_policy` 로 정해진다 — off=0(중재 없음), single/private=1, dual=2,
multi=`max_talkers`. 정책별 동작 규약은 [api/cmp_media_api.md §7.7](../../api/cmp_media_api.md) 이 정본.

```
              ┌──────────┐  집합이 빔
              │   IDLE   │◀───────────────┐
              └────┬─────┘                │
                   │ REQUEST              │ 마지막 화자 RELEASE/REVOKE
                   ▼                      │
              ┌──────────────────┐        │
              │  TALKING (1..N)  │────────┘
              └────┬─────────────┘
                   │ REQUEST
                   ├─ 정원 여유 + multi        → 동시 GRANT
                   ├─ 정원 여유 + dual + 선점  → 동시 GRANT (기존 화자 유지)
                   ├─ 정원 만석 + 선점         → 최약 화자 REVOKE + GRANT
                   └─ 그 외                    → 큐잉(QUEUE_POS_INFO) / DENY(private·큐포화)
```

**Floor 처리 상세 (handleFloorRequest):**

```
handleFloorRequest(sessionId, ssrc, indicatorBits)
  │
  ├─ floor_control=off → 무시 (중재 없음)
  ├─ Indicator(emergency/imminent) → tier 승격
  ├─ recv_only/floor_suppress 멤버 → DENY(receive only)
  ├─ broadcast 그룹 비개시자 → DENY(receive only)
  │
  ├─ 발언자 없음 → GRANT (슬롯 배정 + 녹취 세그먼트 시작)
  ├─ 이미 발언 중 → 무시
  └─ 발언자 있음
      ├─ 선점 서열: tier(emergency>imminent>normal) > chair > 수치 priority(0~255)
      │             (private call 은 chair 단계 없음 — TS 24.380 §7)
      ├─ multi + 정원 여유          → 동시 GRANT
      ├─ dual + 정원 여유 + 선점자격 → 동시 GRANT (REVOKE 없음, Dual floor 비트)
      ├─ 선점                        → 최약 화자 REVOKE(cause=preempted) 후 GRANT
      └─ 비선점 → 큐잉(QUEUE_POS_INFO) / 큐 없음·포화면 DENY
```
> 멤버 `role`(chair/participant)이 PTT_JOIN/멤버문자열(`id:prio:role`)로 전달되어 선점 판정에 사용.
> 화자 1명이 빠져도 잔여 화자가 있으면 IDLE 대신 잔여 화자 TAKEN 을 재브로드캐스트한다.
> 무활동 자동 회수(`FloorIdleSec`)는 화자별 독립 판정(긴급 tier 제외).
> 모든 floor 이벤트는 세션 시간버킷 `{record_dir}/{YYYY}/{MM}/{DD}/{HH}/floor.jsonl` 에 기록(GRANT/REVOKE/REJECT/RELEASE/IDLE + prio/preempt).
> 세그먼트는 `seg/{NNN}/`(100세그 shard), 빈 트랙(.rtp) 미생성. 상세 [recording.md](../features/recording.md).

#### Floor Control 패킷 (RTCP APP "MCPT")

TS 24.380 §8.2 — RTCP APP 12B 고정 헤더(V/P/**subtype**=메시지 타입, PT=204, length, SSRC,
name="MCPT") 뒤에 floor control field 들의 **TLV**(Field ID 1B + Length 1B + value) 나열.
문자열 필드(Granted Party/User ID/Queued User ID/Track Info)만 4바이트 경계로 패딩한다.
인코더/디코더는 `PFloorCodec.cpp`(단말 `floor/FloorCodec.kt` 와 바이트 호환, 단위테스트
`tests/cmp_floor_codec_test.cpp`).

**메시지 타입 = subtype (TS 24.380 Table 8.2.2-1):**

| 메시지 | subtype | 방향 | 설명 |
|--------|---|------|------|
| Floor Request | 0 | UE → CMP | 발언권 요청 |
| Floor Granted | 1 | CMP → UE | 발언권 승인 |
| Floor Taken | 2 | CMP → ALL | 발언권 점유됨 (화자 ID 포함) |
| Floor Deny | 3 | CMP → UE | 발언권 거절 (Reject Cause) |
| Floor Release | 4 | UE → CMP | 발언권 해제 |
| Floor Idle | 5 | CMP → ALL | 발언자 없음 (전체 통지) |
| Floor Revoke | 6 | CMP → UE | 발언권 강제 회수 (선점/무활동) |
| Floor Queue Position Request | 8 | UE → CMP | 큐 위치 조회 |
| Floor Queue Position Info | 9 | CMP → UE | 큐 위치 응답 |
| Floor Ack | 10 | 양방향 | 수신 확인 |
| Floor Release Multi Talker | 0x0F | UE → CMP | 동시 발언 중 자기 발언만 해제 (Rel-16) |

**SRTCP 보호** — `floor_crypto` 가 설정된 그룹은 위 패킷을 SRTCP(RFC 3711, AES-CM +
HMAC-SHA1)로 주고받는다: 헤더 8B 평문 + 본문 암호화 + `E|SRTCP index` + MKI(선택) + 인증 태그.
구현 `PFloorCrypto.cpp`(단위테스트 `tests/cmp_floor_crypto_test.cpp`), 인증 실패·재전송은
폐기 후 STATS `floor_crypto_drop` 에 누적. 미디어 RTP 는 투명 relay 로 유지한다(TS 33.180).

#### Floor 패킷 전송 경로

```
Floor 요청 수신 (UE → CMP):
  PRtpMulticast._floorSock 수신
  → McpttGroup::onFloorPacket()
  → handleFloorRequest/Release()

Floor 응답 전송 (CMP → UE):
  McpttGroup::sendToMember(sessionId, data, len)
  → PRtpMulticast::sendFloorTo(peer.ip, peer.floorPort, data, len)
  → _floorSock.sendTo()

```

#### DTMF 기반 Floor Control

레거시 단말이 RTCP APP를 지원하지 않는 경우 DTMF 숫자로 대체:

```
RTP 패킷 수신 (PT=101, telephone-event)
  │
  ├─ digitCode 추출 (buf[12])
  ├─ endBit 확인 (buf[13] & 0x80)
  │
  ├─ digit == DtmfPushDigit ("*") && endBit
  │   └─ handleFloorRequest()
  │
  └─ digit == DtmfReleaseDigit ("#") && endBit
      └─ handleFloorRelease()
```

#### NAT 멤버의 목적지 latch

멤버 신원은 전용 포트 유닛이 확정하므로 소스 매칭이 없다. `PTT_JOIN` 의 `user_nat=1`
멤버에서만, 유닛 포트로 도착한 첫 유효 RTP(v2 + `user_sig_ip` guard) 소스를 그 멤버의
**송신 목적지**로 latch 하고 SSRC 를 고정한다 (재-latch 는 동일 SSRC = NAT rebind 추종만).
청취 전용 NAT 멤버도 자기 유닛 포트로 보내는 RTP keepalive 로 하향 경로가 열린다 —
같은 NAT 뒤 다중 멤버도 유닛 포트가 구분하므로 모호성이 없다.
Floor 채널은 별도로 TS 24.380 User ID 기반 주소 latch(`onFloorPacket`)를 유지한다.

---

## 4. 리소스 풀 관리

### 4.1 VoIP 리소스 풀

**초기화 (initResourcePool) — 호당 8포트 (leg 별 4포트 블록 × 2):**

```
RtpStartPort = 50000, RtpPoolSize = 20

포트 할당:
  PRtpRelay[0] : peer0 50000-50003 (RTP/RTCP/VRtp/VRtcp), peer1 50004-50007
  PRtpRelay[1] : 50008-50015
  ...
  PRtpRelay[19]: 50152-50159

Worker 배정: RtpWorker_{i % RtpWorkerCount}
```

**할당/반환:**

```cpp
PRtpRelay* allocResource(rtpIp, rtpPort, videoPort);  // _freeResources.pop_back()
void freeResource(PRtpRelay* rtp);                     // _freeResources.push_back()
```

### 4.2 PTT 리소스 풀

**초기화 (initPttResourcePool + initPttMemberPool):**

```
PttFloorStartPort = 54000, PttRtpPoolSize = 10          — 그룹(공유 floor) 풀
PttRtpStartPort = 52000, PttVideoStartPort = 56000,
PttMemberPoolSize = 40                                  — 멤버 포트 유닛 풀

포트 할당:
  PRtpMulticast[0..9]  : Floor 54000, 54002, ... 54018        (그룹당 1)
  PPttMemberPort[0..39]: Audio 52000+N*2, Video 56000+N*2     (참가 멤버당 1)

Worker 배정: RtpWorker_{i % RtpWorkerCount}
```

**할당/반환:**

```cpp
PRtpMulticast* allocPttResource(rtpIp, floorPort);     // 그룹 생성/해제
PPttMemberPort* ensureMemberUnit(groupId, sessionId);  // (group, member) 멱등 키
void freeMemberUnit(groupId, sessionId);               // PTT_LEAVE / 그룹 해제
```

### 4.3 포트 대역 정리

```
┌─────────────────────────────────────────────────────────────┐
│ VoIP RTP Pool (PRtpRelay) — 호당 8포트                       │
│ 50000 ─────────────────────────── 50159                     │
│ [peer0: RTP/RTCP/VRtp/VRtcp][peer1: 동일] × 20 블록          │
├─────────────────────────────────────────────────────────────┤
│ PTT 멤버 Audio RTP (PPttMemberPort)                          │
│ 52000 ──────────── 52078   [RTP] × 40 유닛 (2포트 간격)      │
├─────────────────────────────────────────────────────────────┤
│ PTT Floor Control (PRtpMulticast, 그룹 공유)                 │
│ 54000 ──────────── 54018   [Floor] × 10 그룹 (2포트 간격)    │
├─────────────────────────────────────────────────────────────┤
│ PTT 멤버 Video RTP (PPttMemberPort)                          │
│ 56000 ──────────── 56078   [VRtp] × 40 유닛 (2포트 간격)     │
└─────────────────────────────────────────────────────────────┘
```

방화벽/배포 포트 대역은 이 표 기준으로 개방한다 (풀 크기 변경 시 대역 재산정 —
[ue_nat_traversal.md §7](../features/ue_nat_traversal.md)).

---

## 5. 세션 타임아웃 / 누수 회수 (sweeper)

sweeper 는 **고아 relay 의 유일한 안전망**이다. owner(CSP)가 비정상 종료(crash/kill)하면 CSP in-memory
CallMap(relay descriptor)이 소실되어 relay 가 REMOVE 를 영영 못 받고 고아가 되는데, 이를 회수한다.
판정은 **RTP 무수신(inactivity)** 시간 기준 — `touchActivity()`가 RTP 수신 시에만 호출되므로
`now - getLastActivityTime()` = RTP 무수신 경과(또는 생성 후 무RTP 경과)다.

**timeoutLoop() — 60초 주기 검사:**

```
1. 개별 세션 (VoIP):
   idle = now - rtp->getLastActivityTime()
   to   = rtp->everReceivedRtp() ? _sessionTimeout(600s) : _orphanReclaimSec(120s)
          # 무RTP(setup 실패/세션 시작됐으나 RTP 0) = 짧게 회수, RTP수신후(활성/홀드) = 길게(hold/DTX 보호)
   if idle >= to:
     reason = everReceivedRtp ? "hold_timeout" : "orphan_no_rtp"
     → SESSION_TIMEOUT 로그(detail=reason) → 카운터(_leakReclaim{Total,Orphan,Hold})++
     → writeLeakReclaim()  ({ServiceLogDir}/leak_reclaim/YYYY/MM/DD/reclaim.jsonl 한 줄)
     → reset() → freeResource() → 삭제

2. 그룹 세션 (PTT):
   getMemberCount() == 0 && now - lastActivity >= _sessionTimeout
   → GROUP_TIMEOUT 로그 → delete group → 삭제
```

**Activity 갱신:** RTP 패킷 수신 시에만 `touchActivity()` 호출 → `time(&_lastActivityTime); _everReceivedRtp=true`
(제어 메시지 ADD/MODIFY/HEARTBEAT 은 갱신 안 함 → 순수 RTP-inactivity 타이머).

**설정** (`cmp.json` / `config_template.json`): `SessionTimeout`(기본 600, got-RTP idle), `OrphanReclaimSec`(기본 120, 무RTP idle).

**관측**: STATS 응답에 `leak_reclaim_total`/`leak_reclaim_orphan`/`leak_reclaim_hold` + `orphan_reclaim_sec`/`session_timeout`.
OAM `GET /api/v1/stats/leak-reclaims?date=` → reclaim.jsonl 목록 + reason/node 집계. 콘솔 '성능 > 누수 회수(sweeper)' 페이지.
> 정상 환경에서는 이 카운터가 **0** 이 기대값 — 증가 시 CSP crash/teardown 누락 등 누수 신호.

---

## 6. 녹취

### 6.1 VoIP 녹취 (PRtpRelay)

```
startRecording(rawDir, sessionId)
  ├─ rawDir/raw_a.rtp  ← peer[0]→peer[1] 방향 오디오
  ├─ rawDir/raw_b.rtp  ← peer[1]→peer[0] 방향 오디오
  ├─ rawDir/raw_va.rtp ← peer[0] 비디오 (lazy start)
  └─ rawDir/raw_vb.rtp ← peer[1] 비디오 (lazy start)
```

### 6.2 PTT 녹취 (McpttGroup)

```
setRecording(enable, dir)
  └─ startRecording()
      ├─ dir/raw_audio.rtp ← 세션 전체 오디오 (화자 변경 관계없이 연속)
      └─ (비디오: lazy start)
```

### 6.3 RTP 파일 형식

```
[uint32 packet_length][int64 recv_timestamp_usec][RTP payload]
[uint32 packet_length][int64 recv_timestamp_usec][RTP payload]
...
```

---

## 7. CMP Flow/Msg 로깅

### 7.1 출력 레이아웃

```
{MsgLogDir}/cmp/{service}/YYYY/MM/DD/HH/
  ├─ {systemId}.flow.jsonl            (Flow 요약)
  └─ {systemId}_{node}.msg.jsonl      (원문 — node = csp/ue/...)
```

- `service` : CSP 가 payload 에 넣은 값 (volte/mcptt/system/console)
- `node`    : 상대 모듈 약식 이름

### 7.2 Flow 엔트리 형식

필드 순서 + 빈 키 생략 규칙은 CSP 와 동일. 대표 예:

```jsonc
// PTT 그룹 생성 응답
{"ts":"14:32:56.123","service":"mcptt","caller":"+82571910001","sesid":"+82571910001::csp::1713...::1",
 "subid":"","node":"csp","from":"cmp","to":"csp","proto":"INT","method":"PTT_GROUP_ADD",
 "mid":12,"seq":13,"iface":"cmp"}

// Floor GRANT 브로드캐스트 (CMP → UE)
{"ts":"14:33:05.789","service":"mcptt","caller":"+82571910001","sesid":"+82571910001::csp::1713...::1",
 "from":"cmp","to":"ue","proto":"MCPTT","method":"FLOOR_GRANT",
 "detail":"{\"speaker\":\"+82571900001\",\"ssrc\":1005}"}

// DTMF (RFC2833/4733, end-bit only)
{"ts":"14:33:10.001","service":"mcptt","caller":"+82571910001","sesid":"...",
 "from":"ue","to":"cmp","proto":"DTMF","method":"DTMF",
 "detail":"{\"digit\":\"*\",\"duration_ms\":120,\"volume\":10,\"user\":\"+82571900001\"}"}
```

### 7.3 Flow 항목별 활성화

`cmp.json` 의 `ServiceLogging.Flow` 로 노이즈가 많은 이벤트를 선택적으로 끈다:

```json
"ServiceLogging": {
  "Dir": "/data/msg_log",
  "Enable": ["csp"],
  "MediaTypes": ["floor","dtmf"],
  "Flow": {
    "Floor": true,
    "Dtmf":  true,
    "Rtcp":  false
  }
}
```

| 플래그 | 기록 대상 |
|--------|-----------|
| `Flow.Floor` | RTCP APP (MCPT) opcode 전체 (REQUEST/GRANT/REJECT/RELEASE/IDLE/TAKEN/REVOKE) |
| `Flow.Dtmf`  | RFC 2833/4733 telephone-event end-bit, digit/duration/volume JSON detail |
| `Flow.Rtcp`  | 일반 RTCP SR/RR/SDES/BYE (기본 off — 패킷 빈도 높음) |

### 7.4 Floor/DTMF 기록 경로

```
onFloorPacket/onMemberRtpPacket(DTMF) → _dtmfFlowLog / logFlow(proto=MCPTT)
broadcastFloorStatus(TAKEN/IDLE/REVOKE) → logFlow(from=cmp, to=ue, proto=MCPTT)
```

- `speaker_id` / `ssrc` / `user` 는 JSON detail 에 포함되어 Console UI 의 메시지 상세창에서 파싱 가능

---

## 8. Worker 스레드 모델

```
CmpServer (PModule)
  │
  ├─ RtpWorker_0 ──→ [PRtpRelay_0, PRtpRelay_4, PttFloor_0, PttMember_0, ...]
  ├─ RtpWorker_1 ──→ [PRtpRelay_1, PRtpRelay_5, PttFloor_1, PttMember_1, ...]
  ├─ RtpWorker_2 ──→ [PRtpRelay_2, PRtpRelay_6, PttFloor_2, PttMember_2, ...]
  └─ RtpWorker_3 ──→ [PRtpRelay_3, PRtpRelay_7, PttFloor_3, PttMember_3, ...]
```

- 각 Worker는 배정된 모든 핸들러의 `proc()`을 순환 호출
- 핸들러는 초기화 시 Worker에 영구 등록 (`addHandler`)
- 핸들러 활성/비활성과 무관하게 상시 polling

---

## 9. 설정 (cmp.json)

```json
{
  "RtpStartPort": 50000,         // VoIP Audio RTP 시작 포트
  "RtpPoolSize": 20,             // VoIP 4포트 블록 수
  "PttRtpStartPort": 52000,      // PTT Audio RTP 시작 포트
  "PttRtpPoolSize": 10,          // PTT 2포트 블록 수
  "PttFloorStartPort": 54000,    // PTT Floor Control 시작 포트
  "RtpWorkerCount": 4,           // RTP 처리 Worker 스레드 수
  "RtpIp": "192.168.1.10",       // RTP 미디어 인터페이스 IP
  "ServerIp": "0.0.0.0",         // UDP 제어 리스닝 IP
  "ServerPort": 9000,            // UDP 제어 리스닝 포트
  "EnableDtmfPtt": true,         // DTMF Floor Control 활성화
  "DtmfPushDigit": "*",          // Floor REQUEST 숫자
  "DtmfReleaseDigit": "#",       // Floor RELEASE 숫자
  "ServiceLogging": {
    "Dir": "/data/msg_log",
    "Enable": ["csp"],           // 원문 저장 대상 노드
    "MediaTypes": ["floor","dtmf"],
    "Flow": {
      "Floor": true,             // RTCP APP (MCPT) opcode 전체
      "Dtmf":  true,             // RFC 2833/4733 end-bit
      "Rtcp":  false             // SR/RR/SDES/BYE (빈도↑)
    }
  },
  "SessionTimeout": 600,         // got-RTP relay RTP-idle 회수 (초, 0=비활성)
  "OrphanReclaimSec": 120,       // 무RTP(setup 실패) relay 회수 (초) — SessionTimeout 보다 짧게
  "LogDir": "log",               // 로그 디렉토리
  "LogMaxSizeMB": 10,            // 로그 파일 최대 크기
  "LogMaxFiles": 5,              // 로그 파일 보관 수
  "LogLevel": "INFO"             // DEBUG/INFO/WARN/ERROR
}
```

---

## 10. 외부 인터페이스

| 인터페이스 | 프로토콜 | 포트 | 방향 | 상대 |
|------------|----------|------|------|------|
| 제어 채널 | JSON/UDP | 9000 | in | CSP |
| VoIP RTP | RTP/UDP | 50000-50079 | bi | UE (CSP 경유) |
| VoIP RTCP | RTCP/UDP | 50001-50079 (홀수) | bi | UE |
| VoIP Video | RTP/UDP | 50002-50079 (4n+2) | bi | UE |
| PTT Audio | RTP/UDP | 52000-52018 | bi | UE (CSP 경유) |
| PTT Floor | RTCP APP/UDP | 54000-54018 | bi | UE |

---

## 11. 관련 문서

- [../features/flow_logging.md](./../features/flow_logging.md) — Flow/Msg 로깅 공통 규격, sesid 상속, CSP↔CMP 인터페이스 필드
