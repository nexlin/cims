# 11. CMP (Component Media Provider) 모듈 상세 설계

## 1. 개요

CMP는 CIMS 시스템의 미디어 서버로, CSP의 제어 하에 RTP relay, PTT 오디오 믹싱, MCPTT Floor Control을 수행한다.

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
              (PRtpTrans) (PPttTrans) (McpttGroup)
                   │        │        │
                   └────────┼────────┘
                            │
                     RTP Worker Threads
                     (PModule/PHandler)
```

### 2.2 VoIP/PTT 핸들러 분리

이전 구조에서는 VoIP와 PTT가 동일한 PRtpTrans(4포트 블록)을 공유했으나, 현재는 용도별로 분리:

| 구분 | 핸들러 | 소켓 구성 | 포트 블록 |
|------|--------|-----------|-----------|
| VoIP | PRtpTrans | Audio RTP + RTCP + Video RTP + RTCP | 4포트 (연속) |
| PTT | PPttTrans | Audio RTP + Floor Control | 2포트 (독립 대역) |

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
    std::map<std::string, PRtpTrans*> _sessions;   // VoIP 세션
    std::map<std::string, McpttGroup*> _groups;     // PTT 그룹
    std::map<std::string, std::string> _logDirs;    // key → log 경로
    std::map<std::string, std::string> _sesidMap;   // key → sesid (CSP 발급 상속)
    std::map<std::string, std::string> _serviceMap; // key → service (volte/mcptt/...)

    // Flow 로그 항목별 활성화 플래그 (cmp.json ServiceLogging.Flow)
    bool _logFlowFloor;   // Floor opcode
    bool _logFlowDtmf;    // DTMF (RFC2833/4733)
    bool _logFlowRtcp;    // RTCP SR/RR/SDES/BYE

    // VoIP 리소스 풀
    std::vector<PRtpTrans*> _resourcePool;
    std::vector<PRtpTrans*> _freeResources;

    // PTT 리소스 풀
    std::vector<PPttTrans*> _pttPool;
    std::vector<PPttTrans*> _freePttResources;
};
```

**Flow/Msg 로깅 공통 필드:**

- CSP 가 payload 에 동봉한 `service` / `sesid` / `caller` / `callee` 를 key(session_id/group_id) 별로 저장
- 이후 응답 및 후속 이벤트(RTP/Floor/DTMF/RTCP) 로그에 동일 값을 상속하여 **CSP↔CMP 양측 Flow 가 단일 sesid 로 묶이도록** 보장
- Flow 로그 필드 순서·생략 규칙은 CSP 측과 동일 (`ts, service, caller, callee, sesid, subid, node, from, to, proto, method, detail, mid, seq, iface`)
- 전체 규격은 [../features/flow_logging.md](./../features/flow_logging.md) 참고

**초기화 순서:**

```
CmpServer(name, configFile)
  1. loadConfig()  ── 설정 파일 파싱
  2. Worker 스레드 생성 (RtpWorker_0 ~ RtpWorker_N)
  3. initResourcePool()    ── VoIP PRtpTrans 풀 생성
  4. initPttResourcePool() ── PTT PPttTrans 풀 생성
```

**UDP JSON 프로토콜:**

요청 형식:
```json
{
  "trans_id": 1001,
  "payload": {
    "cmd": "ADD",
    "session_id": "sess_001",
    "remote_ip": "192.168.1.100",
    "remote_port": 30000,
    "record_dir": "/data/service_log/voip/.../sess.d"
  }
}
```

응답 형식:
```json
{
  "trans_id": 1001,
  "response": {
    "status": "OK",
    "local_ip": "192.168.1.10",
    "local_port": 50000,
    "local_video_port": 50002
  }
}
```

### 3.2 명령 상세

#### ADD / ADD_SESSION — VoIP 세션 생성

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| session_id | O | 세션 식별자 |
| remote_ip | O | 상대방 RTP IP |
| remote_port | O | 상대방 RTP 포트 |
| remote_video_port | - | 상대방 Video RTP 포트 |
| peer_index | - | 피어 인덱스 (0 또는 1) |
| record_dir | - | 녹취 디렉토리 경로 |
| log_dir | - | CMP flow 로그 경로 |

**응답:** `local_ip`, `local_port`, `local_video_port`

**동작:**
1. `_freeResources`에서 PRtpTrans 할당
2. 원격 피어 주소 설정 (`setRmt`)
3. record_dir 있으면 녹취 시작
4. log_dir 있으면 `_logDirs`에 저장, SESSION_START 로그

#### REMOVE / REMOVE_SESSION — VoIP 세션 해제

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| session_id | O | 세션 식별자 |

**동작:** 녹취 중지 → reset() → freeResource() → 세션/로그 맵 삭제

#### MODIFY / MODIFY_SESSION — VoIP 세션 수정

processAdd()로 위임. 기존 세션이 있으면 피어 주소만 갱신.

#### ADD_GROUP / ADDGROUP — PTT 그룹 생성

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| members | - | "sid1:prio1,sid2:prio2" CSV 형식 |
| record_dir | - | 녹취 디렉토리 |
| log_dir | - | CMP flow 로그 경로 |

**응답:** `ip`, `port` (Audio RTP), `floor_port` (Floor Control), `video_port`

**동작:**
1. McpttGroup 생성
2. `_freePttResources`에서 PPttTrans 할당
3. PPttTrans ↔ McpttGroup 연결 (`setGroup`, `setPttSession`)
4. DTMF 설정 전달
5. 녹취/로그 설정
6. members CSV 파싱 → 우선순위 설정

#### JOIN_GROUP / JOINGROUP — 멤버 참가

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| session_id | O | 멤버 세션 ID |
| user_ip | O | 멤버 RTP IP |
| user_port | O | 멤버 Audio RTP 포트 |
| user_floor_port | - | 멤버 Floor Control 포트 |
| user_video_port | - | 멤버 Video RTP 포트 |

**동작:** McpttGroup::addMember() 호출. Floor taken 상태면 신규 멤버에게 FLOOR_TAKEN 통지.

#### LEAVE_GROUP / LEAVEGROUP — 멤버 퇴장

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |
| session_id | O | 멤버 세션 ID |

**동작:** McpttGroup::removeMember(). Floor 소유자 퇴장 시 FLOOR_IDLE 브로드캐스트.

#### REMOVE_GROUP / REMOVEGROUP — 그룹 해제

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| group_id | O | 그룹 식별자 |

**동작:** PPttTrans 리소스 반환 → McpttGroup delete → 맵 삭제

#### STATS / STATS_REQUEST — 통계 조회

**응답:**
```json
{
  "status": "OK",
  "sessions": 5,
  "groups": 2,
  "rtp_ports_total": 20,
  "rtp_ports_used": 7,
  "rtp_ports_free": 13,
  "ptt_rtp_ports_total": 50,
  "ptt_rtp_ports_used": 2,
  "ptt_rtp_ports_free": 48,
  "session_timeout": 600,
  "group_details": [
    {
      "group_id": "group_1",
      "members": 4,
      "floor_holder": "1001"
    }
  ]
}
```

- `rtp_ports_*` = VoIP 풀(`_freeResources`/`PRtpTrans`), `ptt_rtp_ports_*` = PTT 전용 풀(`_freePttResources`/`PPttTrans`) — 리소스 풀 분리(0.0.6+). OAM `/stats/health` 가 `cmp.rtp_ports` + `cmp.rtp_ports_ptt` 로 분리 전달.

#### ALIVE / HEARTBEAT — 연결 확인

**응답:** `{"trans_id": N, "response": "OK"}`

---

### 3.3 PRtpTrans (VoIP 핸들러)

**파일:** `PRtpHandler.h/.cpp`

**상속:** `PHandler` (pasf 프레임워크의 핸들러 베이스)

**소켓 구성 (4포트 블록):**

```
basePort+0 : Audio RTP     (_rtpSock)
basePort+1 : Audio RTCP    (_rtcpSock)
basePort+2 : Video RTP     (_videoRtpSock)
basePort+3 : Video RTCP    (_videoRtcpSock)
```

**듀얼 피어 구조:**

```cpp
struct PeerInfo {
    std::string ip;
    unsigned int port;
    unsigned int videoPort;
    struct sockaddr_in addrRtp, addrRtcp, addrVideoRtp, addrVideoRtcp;
    bool active;
};
PeerInfo _peers[2];  // B2BUA 양 leg
```

**RTP Relay 로직 (proc()):**

```
proc() — Worker 스레드에서 주기 호출
  │
  ├─ RTCP 수신 (Audio)
  │   ├─ 그룹 모드 → McpttGroup::onRtcpPacket() (legacy floor)
  │   └─ 1:1 모드 → 반대편 피어로 relay
  │
  ├─ RTP 수신 (Audio)
  │   ├─ touchActivity() (세션 타임아웃 갱신)
  │   ├─ 그룹 모드 → McpttGroup::onRtpPacket()
  │   ├─ 브릿지 모드 → bridgePeer로 전달
  │   └─ 1:1 모드 → 반대편 피어로 relay
  │   └─ 녹취 활성 시 → RtpRecorder::WritePacket()
  │
  ├─ Video RTP 수신
  │   ├─ 그룹 모드 → McpttGroup::onVideoRtpPacket()
  │   └─ 1:1 모드 → 반대편 피어로 relay
  │
  └─ Video RTCP 수신
      └─ 반대편 피어로 relay
```

**Symmetric RTP (IP Learning):**

NAT 환경에서 최초 패킷 수신 시 IP를 학습하여 피어 주소 갱신:
```cpp
if (rmtIp == _peers[i].ip && rmtPort != _peers[i].port) {
    // 포트 latching
    _peers[i].port = rmtPort;
}
```

**녹취:**

```cpp
void startRecording(const std::string& rawDir, const std::string& sessionId);
// rawDir/raw_a.rtp  — peer[0]→peer[1] 오디오
// rawDir/raw_b.rtp  — peer[1]→peer[0] 오디오
// rawDir/raw_va.rtp — peer[0] 비디오
// rawDir/raw_vb.rtp — peer[1] 비디오
```

### 3.4 PPttTrans (PTT 핸들러)

**파일:** `PRtpHandler.h/.cpp`

**상속:** `PHandler`

**소켓 구성 (2포트, 독립 대역):**

```
PttRtpStartPort + N*2    : Audio RTP   (_rtpSock)
PttFloorStartPort + N*2  : Floor Ctrl  (_floorSock)
```

**주요 메서드:**

| 메서드 | 역할 |
|--------|------|
| `init(ip, rtpPort, floorPort)` | 오디오 RTP + Floor 소켓 바인드 |
| `setGroup(McpttGroup*)` | 그룹 연결 |
| `sendFloorTo(ip, port, data, len)` | Floor 소켓으로 패킷 전송 |
| `proc()` | Floor 수신 → onFloorPacket(), Audio 수신 → onRtpPacket() |
| `reset()` | 세션 초기화 (그룹 해제) |

**proc() 처리 순서:**

```
proc() — Worker 스레드에서 주기 호출
  │
  ├─ 1. Floor Control 패킷 수신 (_floorSock)
  │   └─ McpttGroup::onFloorPacket(ip, port, buf, len)
  │       ├─ 멤버 floorPort로 매칭
  │       ├─ Symmetric floor (포트 매칭 + IP 학습)
  │       └─ opcode 디스패치 → handleFloorRequest/Release
  │
  └─ 2. Audio RTP 수신 (_rtpSock)
      ├─ touchActivity()
      └─ McpttGroup::onRtpPacket(ip, port, buf, len)
          ├─ 발신자 매칭 (IP:port → sessionId)
          ├─ DTMF 감지 (PT=101, end bit)
          └─ Floor 소유자 오디오만 전체 송출
```

### 3.5 McpttGroup

**파일:** `McpttGroup.h/.cpp`

PTT 그룹 오디오 믹싱 및 MCPTT Floor Control.

**멤버 구조:**

```cpp
struct Peer {
    std::string id;           // 세션 ID
    std::string ip;           // 멤버 IP
    int port;                 // Audio RTP 포트
    int floorPort;            // Floor Control 포트 (m=application)
    int videoPort;            // Video RTP 포트
    unsigned int ssrc;        // CMP 할당 SSRC
    uint16_t audioSeqOut;     // 수신자별 오디오 시퀀스 카운터
    uint16_t videoSeqOut;     // 수신자별 비디오 시퀀스 카운터
    uint32_t audioSsrcOut;    // 수신자에게 보내는 고정 오디오 SSRC
    uint32_t videoSsrcOut;    // 수신자에게 보내는 고정 비디오 SSRC
};
```

**수신자별 SSRC/시퀀스 재작성:**

```cpp
void sendAudioToAll(data, len, excludeIp, excludePort) {
    for (auto& [sid, peer] : _members) {
        // 발신자 제외
        if (peer.ip == excludeIp && peer.port == excludePort) continue;
        // 패킷 복제 후 수신자별 SSRC + 시퀀스 재작성
        char pkt[4096];
        memcpy(pkt, data, len);
        peer.audioSeqOut++;
        // RTP 헤더 seq(offset 2-3) = peer.audioSeqOut
        // RTP 헤더 ssrc(offset 8-11) = peer.audioSsrcOut
        _sharedSession->sendTo(peer.ip, peer.port, pkt, len);
    }
}
```

이 방식으로 각 수신자는 연속적인 시퀀스 번호와 고정 SSRC를 받아 jitter buffer 오동작을 방지한다.

#### Floor Control 상태 머신

```
                    ┌─────────────────────────┐
                    │                         │
                    ▼                         │
              ┌──────────┐                    │
              │   IDLE   │                    │
              └────┬─────┘                    │
                   │ REQUEST                  │
                   ▼                          │
              ┌──────────┐    RELEASE         │
              │  TAKEN   │────────────────────┘
              └────┬─────┘
                   │ REQUEST (높은 우선순위)
                   ▼
              ┌──────────┐
              │ PREEMPT  │── REVOKE(이전 화자) + GRANT(새 화자)
              └──────────┘
```

**Floor 처리 상세 (handleFloorRequest):**

```
handleFloorRequest(sessionId, ssrc)
  │
  ├─ Floor IDLE 상태
  │   ├─ _floorTaken = true
  │   ├─ _floorOwnerSessionId = sessionId
  │   ├─ 요청자에게 FLOOR_GRANT 전송
  │   ├─ 전체에게 FLOOR_TAKEN 브로드캐스트
  │   └─ 녹취 시작 (미시작 시)
  │
  └─ Floor TAKEN 상태
      ├─ 동일 화자 → 무시
      ├─ 선점 판정: chair > participant (chair 항상 선점), 동급이면 우선순위(낮을수록 우선)
      ├─ 선점 시
      │   ├─ 현재 화자에게 FLOOR_REVOKE
      │   ├─ 새 화자 등록, FLOOR_GRANT
      │   └─ 전체에게 FLOOR_TAKEN 브로드캐스트
      └─ 비선점 → 요청자에게 FLOOR_REJECT
```
> 2026-06: 멤버 `role`(chair/participant)이 JOIN_PTT_GROUP/멤버문자열(`id:prio:role`)로 전달되어 선점 판정에 사용.
> 모든 floor 이벤트는 세션 시간버킷 `{record_dir}/{YYYY}/{MM}/{DD}/{HH}/floor.jsonl` 에 기록(GRANT/REVOKE/REJECT/RELEASE/IDLE + prio/preempt).
> 세그먼트는 `seg/{NNN}/`(100세그 shard), 빈 트랙(.rtp) 미생성. 상세 [recording.md](../features/recording.md).

#### Floor Control 패킷 (RTCP APP)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  subtype |   PT=204    |          length               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         SSRC                                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   name = "MCPT" (4 bytes)                                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   opcode    |   id_len      |          reserved               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   speaker_id (가변 길이, 4바이트 정렬)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**OpCode 정의:**

| OpCode | 값 | 방향 | 설명 |
|--------|---|------|------|
| FLOOR_REQUEST | 1 | UE → CMP | 발언권 요청 |
| FLOOR_GRANT | 2 | CMP → UE | 발언권 승인 |
| FLOOR_REJECT | 3 | CMP → UE | 발언권 거절 |
| FLOOR_RELEASE | 4 | UE → CMP | 발언권 해제 |
| FLOOR_IDLE | 5 | CMP → ALL | 발언권 해제됨 (전체 통지) |
| FLOOR_TAKEN | 6 | CMP → ALL | 발언권 점유됨 (화자 ID 포함) |
| FLOOR_REVOKE | 7 | CMP → UE | 발언권 강제 회수 |

#### Floor 패킷 전송 경로

```
Floor 요청 수신 (UE → CMP):
  PPttTrans._floorSock 수신
  → McpttGroup::onFloorPacket()
  → handleFloorRequest/Release()

Floor 응답 전송 (CMP → UE):
  McpttGroup::sendToMember(sessionId, data, len)
  → PPttTrans::sendFloorTo(peer.ip, peer.floorPort, data, len)
  → _floorSock.sendTo()

Floor Fallback (legacy RTCP):
  _pttSession 없거나 floorPort == 0인 경우
  → _sharedSession->sendTo(peer.ip, peer.port + 1, data, len)
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

#### 오디오 포트 Latching

NAT 환경에서 멤버 IP/포트를 동적으로 학습:

```
onRtpPacket(ip, port, ...)
  │
  ├─ 1차: IP:port 정확 매칭 → 성공
  │
  ├─ 2차: IP 매칭 + port 불일치 (후보 1명)
  │   └─ 포트 latching: peer.port = port
  │
  └─ 실패: "RTP from unknown sender" 로그
```

---

## 4. 리소스 풀 관리

### 4.1 VoIP 리소스 풀

**초기화 (initResourcePool):**

```
RtpStartPort = 50000, RtpPoolSize = 20

포트 할당:
  PRtpTrans[0] : 50000 (RTP), 50001 (RTCP), 50002 (VRtp), 50003 (VRtcp)
  PRtpTrans[1] : 50004, 50005, 50006, 50007
  ...
  PRtpTrans[19]: 50076, 50077, 50078, 50079

Worker 배정: RtpWorker_{i % RtpWorkerCount}
```

**할당/반환:**

```cpp
PRtpTrans* allocResource(rtpIp, rtpPort, videoPort);  // _freeResources.pop_back()
void freeResource(PRtpTrans* rtp);                     // _freeResources.push_back()
```

### 4.2 PTT 리소스 풀

**초기화 (initPttResourcePool):**

```
PttRtpStartPort = 52000, PttFloorStartPort = 54000, PttRtpPoolSize = 10

포트 할당:
  PPttTrans[0] : RTP 52000, Floor 54000
  PPttTrans[1] : RTP 52002, Floor 54002
  ...
  PPttTrans[9] : RTP 52018, Floor 54018

Worker 배정: RtpWorker_{i % RtpWorkerCount}
```

**할당/반환:**

```cpp
PPttTrans* allocPttResource(rtpIp, rtpPort, floorPort);  // _freePttResources.pop_back()
void freePttResource(PPttTrans* ptt);                     // _freePttResources.push_back()
```

### 4.3 포트 대역 정리

```
┌─────────────────────────────────────────────────────────────┐
│ VoIP RTP Pool (PRtpTrans)                                   │
│ 50000 ─────────────────────────── 50079                     │
│ [RTP][RTCP][VRtp][VRtcp] × 20 블록                          │
├─────────────────────────────────────────────────────────────┤
│ (gap: 50080 ~ 51999)                                        │
├─────────────────────────────────────────────────────────────┤
│ PTT Audio RTP Pool (PPttTrans._rtpSock)                     │
│ 52000 ──────────── 52018                                    │
│ [RTP] × 10 블록 (2포트 간격)                                 │
├─────────────────────────────────────────────────────────────┤
│ (gap: 52020 ~ 53999)                                        │
├─────────────────────────────────────────────────────────────┤
│ PTT Floor Control Pool (PPttTrans._floorSock)               │
│ 54000 ──────────── 54018                                    │
│ [Floor] × 10 블록 (2포트 간격)                               │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. 세션 타임아웃

**timeoutLoop() — 60초 주기 검사:**

```
1. 개별 세션 (VoIP):
   now - rtp->getLastActivityTime() >= _sessionTimeout
   → SESSION_TIMEOUT 로그 → reset() → freeResource() → 삭제

2. 그룹 세션 (PTT):
   getMemberCount() == 0 && now - lastActivity >= _sessionTimeout
   → GROUP_TIMEOUT 로그 → delete group → 삭제
```

**Activity 갱신:** RTP 패킷 수신 시 `touchActivity()` 호출 → `time(&_lastActivityTime)`

---

## 6. 녹취

### 6.1 VoIP 녹취 (PRtpTrans)

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
 "subid":"","node":"csp","from":"cmp","to":"csp","proto":"INT","method":"ADD_PTT_GROUP",
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
onRtcpPacket/onFloorPacket → _dtmfFlowLog / logFlow(proto=MCPTT)
broadcastFloorStatus(TAKEN/IDLE/REVOKE) → logFlow(from=cmp, to=ue, proto=MCPTT)
```

- `speaker_id` / `ssrc` / `user` 는 JSON detail 에 포함되어 Console UI 의 메시지 상세창에서 파싱 가능

---

## 8. Worker 스레드 모델

```
CmpServer (PModule)
  │
  ├─ RtpWorker_0 ──→ [PRtpTrans_0, PRtpTrans_4, PPttTrans_0, PPttTrans_4, ...]
  ├─ RtpWorker_1 ──→ [PRtpTrans_1, PRtpTrans_5, PPttTrans_1, PPttTrans_5, ...]
  ├─ RtpWorker_2 ──→ [PRtpTrans_2, PRtpTrans_6, PPttTrans_2, PPttTrans_6, ...]
  └─ RtpWorker_3 ──→ [PRtpTrans_3, PRtpTrans_7, PPttTrans_3, PPttTrans_7, ...]
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
  "SessionTimeout": 600,         // 세션 타임아웃 (초, 0=비활성)
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
