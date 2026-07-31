#ifndef __MCPTT_GROUP_H__
#define __MCPTT_GROUP_H__

#include <string>
#include <map>
#include <vector>
#include <set>
#include <tuple>
#include <cstdint>
#include <ctime>
#include <functional>
#include <atomic>
#include "pbase.h"
#include "PFloorCrypto.h"

class PRtpMulticast;
class PPttMemberPort;
class PSyncRtpRecorder;

// RTCP APP Packet for Floor Control — 3GPP TS 24.380 §8 (Media Plane Control).
// 메시지 타입은 RTCP APP 의 5비트 subtype 으로 운반되고, 본문은 floor control
// specific field 들의 TLV(Field ID(8)+Length(8)+value) 나열이다. 단말
// (android/ptt-client floor/FloorCodec.kt)과 동일 규약.

#define RTCP_PT_APP 204
#define RTCP_APP_HDR 12   // RTCP APP 고정 헤더(V/P/subtype + PT + length + SSRC + name)

// Floor control 메시지 타입 = RTCP APP subtype (TS 24.380 Table 8.2.2-1).
enum FloorOpCode {
    FLOOR_REQUEST  = 0,   // Floor Request          (UE→서버)
    FLOOR_GRANT    = 1,   // Floor Granted          (서버→UE)
    FLOOR_TAKEN    = 2,   // Floor Taken            (서버→ALL)
    FLOOR_REJECT   = 3,   // Floor Deny             (서버→UE)
    FLOOR_RELEASE  = 4,   // Floor Release          (UE→서버)
    FLOOR_IDLE     = 5,   // Floor Idle             (서버→ALL)
    FLOOR_REVOKE   = 6,   // Floor Revoke           (서버→화자)
    FLOOR_QUEUE_POS_REQ  = 8,  // Floor Queue Position Request (UE→서버)
    FLOOR_QUEUE_POS_INFO = 9,  // Floor Queue Position Info    (서버→UE)
    FLOOR_ACK      = 10,  // Floor Ack
    FLOOR_RELEASE_MULTI  = 0x0F  // Floor Release Multi Talker (UE→서버, Rel-16 multi-talker):
                                 //   동시 발언 중 자기 발언만 해제 — 나머지 화자는 유지되므로
                                 //   서버는 Floor Idle 대신 잔여 화자 Taken 갱신으로 응답한다.
};

// 동시 발언 정책 — floor 제어 "유무"(floor_control)와 직교하는 "동시성" 축
// (cmp_media_api.md §7.1). group_type:"private" 은 이 축을 해석하지 않는다(TS 24.380 §7).
enum FloorPolicy {
    FLOOR_POLICY_SINGLE = 0,  // 단일 화자 (기본)
    FLOOR_POLICY_DUAL   = 1,  // dual floor — 선점자에게 REVOKE 없이 동시 GRANT (최대 2)
    FLOOR_POLICY_MULTI  = 2   // multi-talker — 동시 최대 max_talkers 명 (TS 24.380 Rel-16)
};
// "single"/"dual"/"multi" → FloorPolicy (미지정·미상 = single)
int ParseFloorPolicy(const std::string& s);

// 동시 발언 슬롯 상한 — 슬롯은 수신자별 하향 스트림(SSRC/seq)과 녹취 트랙을 가르는 키다.
// max_talkers 는 이 값으로 clamp 된다.
#define MCPTT_MAX_TALKER_SLOTS 8

// Floor control field ID (TS 24.380 §8.2.3).
enum FloorFieldId {
    FF_PRIORITY       = 0,
    FF_DURATION       = 1,
    FF_REJECT_CAUSE   = 2,   // Floor Deny / Floor Revoke 공용 cause
    FF_QUEUE_INFO     = 3,
    FF_GRANTED_PARTY  = 4,   // 문자열(4B 정렬)
    FF_PERMISSION     = 5,
    FF_USER_ID        = 6,   // 문자열(4B 정렬)
    FF_QUEUE_SIZE     = 7,
    FF_MSG_SEQ        = 8,
    FF_QUEUED_USER_ID = 9,   // 문자열(4B 정렬)
    FF_SOURCE         = 10,
    FF_TRACK_INFO     = 11,  // 문자열(4B 정렬)
    FF_MSG_TYPE       = 12,
    FF_FLOOR_INDICATOR= 13,
    FF_SSRC           = 14
};

// Floor Indicator 비트마스크 (TS 24.380 §8.2.3.13).
enum FloorIndicatorBits {
    FI_NORMAL          = 0x8000,
    FI_BROADCAST_GROUP = 0x4000,
    FI_SYSTEM          = 0x2000,
    FI_EMERGENCY       = 0x1000,
    FI_IMMINENT_PERIL  = 0x0800,
    FI_QUEUEING        = 0x0400,
    FI_DUAL_FLOOR      = 0x0200,
    FI_TEMPORARY_GROUP = 0x0100,
    FI_MULTI_TALKER    = 0x0080
};

// Floor Deny/Revoke cause 코드 (TS 24.380 §8.2.3.4 / §8.2.3.x).
enum FloorCause {
    CAUSE_DENY_ANOTHER_CLIENT = 1,   // Another MCPTT client has permission
    CAUSE_DENY_ONLY_ONE       = 3,   // Only one participant
    CAUSE_DENY_RECEIVE_ONLY   = 5,   // Receive only (broadcast 비개시자)
    CAUSE_DENY_NO_RESOURCES   = 6,   // No resources available
    CAUSE_DENY_QUEUE_FULL     = 7,   // Queue full
    CAUSE_REVOKE_PREEMPTED    = 4,   // Media Burst pre-empted
    CAUSE_OTHER               = 255  // Other reason
};

// Floor 우선순위 tier (TS 24.380) — emergency > imminent peril > normal.
// chair override·수치 priority 비교보다 상위. condition 은 group_type 와 직교(런타임 상태).
enum FloorTier {
    TIER_NORMAL    = 0,
    TIER_IMMINENT  = 1,
    TIER_EMERGENCY = 2
};
// tier 문자열("emergency"/"imminent"/"normal" 또는 숫자) → FloorTier
int ParseFloorTier(const std::string& s);

// RTCP APP 고정 헤더 (12 bytes) — TS 24.380 §8.2.1.
//   |V=2|P| subtype |    PT=204     |        length(words-1)        |
//   |                         SSRC (sender)                         |
//   |                       name = "MCPT"                           |
// subtype(5비트)=메시지타입. 본문은 헤더 직후부터 TLV 나열.
struct RtcpAppHeader {
    unsigned char version_subtype; // V=2(0x80), P=0, Subtype=메시지타입
    unsigned char type;            // PT=204 (APP)
    unsigned short length;         // words - 1 (network order)
    unsigned int ssrc;             // SSRC of sender (network order)
    char name[4];                  // "MCPT"
};

// 하나의 floor control 필드(TLV). value 는 패딩 제외한 실제 값 바이트.
struct FloorTlv {
    int id;
    std::string value;
    FloorTlv(int i, const std::string& v) : id(i), value(v) {}
};

// 파싱된 floor control 메시지.
struct ParsedFloor {
    int subtype = -1;
    unsigned int ssrc = 0;
    std::vector<FloorTlv> fields;
    const FloorTlv* field(int id) const;
    std::string str(int id) const;          // 문자열 필드(User ID 등)
    int u16(int id, int dflt = -1) const;    // 2옥텟 필드(Indicator/Cause/Duration)
    std::string userId() const { return str(FF_USER_ID); }
    int priority() const;                    // FF_PRIORITY 첫 옥텟, 없으면 -1
    int indicator() const { return u16(FF_FLOOR_INDICATOR); }
};

// TLV 필드 값 빌더(big-endian u16 / priority 2옥텟 / queue-info 2옥텟).
std::string FloorU16(int v);
std::string FloorPriority(int prio);
std::string FloorQueueInfo(int position, int prio);

// Floor 메시지 빌드: 12B RTCP APP 헤더 + TLV 본문(문자열 필드 4B 정렬, 전체 4B 정렬).
int BuildFloorMessage(char* buf, int bufSize, unsigned char subtype,
                      unsigned int ssrc, const std::vector<FloorTlv>& fields);
// 수신 패킷 파싱(MCPT APP 아니면 false).
bool ParseFloorMessage(const char* buf, int len, ParsedFloor& out);

class PMcpttGroup {
public:
    PMcpttGroup(const std::string& groupId);
    virtual ~PMcpttGroup();

    void setPttSession(PRtpMulticast* session) { _pttSession = session; }
    PRtpMulticast* getPttSession() const { return _pttSession; }
    // unit: 멤버 전용 RTP 포트 유닛 (PCmpServer 가 할당·소유, 그룹은 참조만)
    // nat/sigIp: NAT 목적지 latch 허용 + latch IP guard (ue_nat_traversal.md §4-5)
    // ptOut/tePtOut: 이 leg 로 송신 시 스탬프할 audio/telephone-event PT (0=재작성 없음).
    // srcTePt: 이 leg 가 송신에 쓰는 telephone-event PT — fan-out 시 audio/TE 분류 기준.
    // codec: 협상 오디오 코덱 문자열 (user_codec, 예 "AMR-WB/16000") — 녹취 세그먼트 메타용.
    // recvOnly/floorSuppress: ambient listening 청취 leg (cmp_media_api.md §7.3) —
    //   recvOnly=상향 미디어 미중계(+floor 요청 거절), floorSuppress=이 멤버에게 floor 메시지 미송신.
    void addMember(const std::string& sessionId, const std::string& ip, int port, int floorPort = 0, int videoPort = 0,
                   const std::string& role = "participant", PPttMemberPort* unit = nullptr,
                   bool nat = false, const std::string& sigIp = "",
                   int ptOut = 0, int srcPt = 0, int tePtOut = 0, int srcTePt = 0,
                   const std::string& codec = "", bool recvOnly = false, bool floorSuppress = false);
    void removeMember(const std::string& sessionId);
    bool hasMember(const std::string& sessionId);

    // Floor Control Logic
    //   indicatorBits: 수신 REQUEST 의 Floor Indicator(emergency/imminent) 비트마스크(-1=없음).
    void handleFloorRequest(const std::string& sessionId, unsigned int userId, int indicatorBits = -1);
    void handleFloorRelease(const std::string& sessionId, unsigned int userId);

    // floor 정책 (PTT_GROUP_ADD/MODIFY payload — cmp_media_api.md §7.1).
    //   floorControl: floor 제어 유무. off = 중재 없는 full-duplex (floor RTCP 미처리).
    //   policy/maxTalkers: floor 有 그룹의 동시 발언 수 (single=1, dual=2, multi=maxTalkers).
    //   privateCall: group_type=="private" — TS 24.380 §7 private-call floor(정원 1, 큐 없음),
    //                group 동시성 정책(policy)은 해석하지 않는다.
    void setFloorPolicy(bool floorControl, int policy, int maxTalkers, bool privateCall);
    bool isFloorControlEnabled() const { return _floorControl; }
    int  getMaxTalkers() const { return _talkerCapacity; }

    // floor RTCP SRTCP 보호 키 (PTT_GROUP_ADD.floor_crypto — TS 33.180).
    //   key/salt/mki 는 디코드된 바이트열. 실패 시 err 를 채우고 false(평문 유지).
    bool setFloorCrypto(const std::string& alg, const std::string& key, const std::string& salt,
                        const std::string& mki, std::string& err);
    bool isFloorCryptoEnabled() { return _floorCrypto.enabled(); }
    // SRTCP 인증 실패/재전송으로 폐기한 floor 패킷 누적 (STATS floor_crypto_drop)
    long getFloorCryptoDrop() const { return _floorCryptoDrop.load(); }

    // Called by PRtpMulticast when a floor control packet is received (m=application)
    void onFloorPacket(const std::string& ip, int port, char* buf, int len);

    // Called by PPttMemberPort — 멤버 전용 포트 수신 (수신 소켓이 곧 멤버 신원)
    void onMemberRtpPacket(const std::string& memberId, const std::string& ip, int port, char* buf, int len);
    void onMemberVideoRtpPacket(const std::string& memberId, const std::string& ip, int port, char* buf, int len);

    void updatePriorities(const std::map<std::string, int>& priorities);
    void updateRoles(const std::map<std::string, std::string>& roles);
    // condition tier(emergency/imminent/normal) 갱신. 일괄(updateTiers) 또는 단건(setTier).
    void updateTiers(const std::map<std::string, int>& tiers);
    void setTier(const std::string& sessionId, int tier);
    int  tierOf(const std::string& sessionId) const;
    // broadcast 그룹(TS 24.380 §10.3): 개시자(initiator)만 floor 보유, 타 멤버 REQUEST REJECT.
    void setBroadcast(const std::string& groupType, const std::string& initiator) {
        _groupType = groupType;
        _initiatorSessionId = initiator;
    }
    void setDtmfConfig(bool enable, const std::string& pushDigit, const std::string& releaseDigit);

    // Floor 이벤트 로그 콜백 (PCmpServer::logFlow 연결용)
    using LogFlowFunc = std::function<void(const std::string& key, const char* from, const char* to,
                                            const char* proto, const char* label, const char* body)>;
    void setLogCallback(LogFlowFunc fn) { _logFlow = fn; }
    /** RTCP SR/RR/SDES/BYE 등 일반 RTCP 메시지도 Flow 에 기록할지 여부 */
    void setRtcpLogEnable(bool b) { _rtcpLogEnable = b; }

    // 발언자 집합 변경 통지 콜백 (PCmpServer → FLOOR_TALKERS 이벤트, cmp_media_api.md §8).
    //   호출 시 그룹 _mutex 를 보유한 상태다 (_logFlow 콜백과 동일 규약). 이벤트 hdr 의
    //   sesid/service 는 그룹이 보관한 값을 실어 보낸다 — 콜백이 PCmpServer 의 맵을
    //   되짚지 않게 해(비재귀 _mutex) 호출 경로와 무관하게 안전하다.
    using TalkersFunc = std::function<void(const std::string& groupId, const char* policy,
                                            const std::vector<std::string>& talkers,
                                            const std::string& sesid, const std::string& service)>;
    void setTalkersCallback(TalkersFunc fn) { _onTalkers = fn; }
    // 이 그룹의 세션 식별 메타 (CSP 발행 sesid / service) — 이벤트 hdr 용.
    void setSessionMeta(const std::string& sesid, const std::string& service);

    int getMemberCount() const { return (int)_members.size(); }
    // 현재 발언자 전원 (STATS detail.groups[].floor_holders — dual/multi-talker 관측).
    //   대표 화자는 out[0](가장 먼저 grant 된 발언자).
    void getFloorHolders(std::vector<std::string>& out);
    // 적용 중인 floor 정책 이름 ("off"/"single"/"dual"/"multi"/"private") — STATS 표기용
    std::string getFloorPolicyName();
    // 그룹 생성 시각 — audit SESSION_LIST 의 grace(min_age) 판정 기준(now-created, 단조 증가).
    time_t getCreatedTime() const { return _createdTime; }
    // 미협상 소스/미등록 멤버 드롭 누적 (STATS rtp_src_drop)
    long getSrcDrop() const { return _srcDrop; }
    // NAT latch 관측 (STATS detail.nat) — latch 완료 멤버의 (sessionId, learnedIp, learnedPort)
    void collectNatLatched(std::vector<std::tuple<std::string, std::string, int>>& out);

    // Floor 무활동(inactivity) 자동 회수 — owner 가 RELEASE 없이 RTP 송출을 멈춘 경우
    // (예: 검증 마지막 발언자가 RELEASE 없이 호 종료). 마지막 RTP 수신 후 idleSec 초가
    // 지나면 세그먼트 종료 + REVOKE/IDLE 송출 + floor 해제. PCmpServer::timeoutLoop 가 주기 호출.
    // 회수했으면 true. idleSec<=0 이면 비활성.
    bool checkFloorInactivity(int idleSec);

private:
    /** DTMF(RFC2833/4733) 이벤트 Flow 기록 헬퍼.
     *  detail JSON: {"digit":"X","duration":ms,"volume":V,"user":"..."}
     *  PCmpServer 의 _logFlow 콜백을 통해 proto="DTMF", label="DTMF" 로 기록. */
    void _dtmfFlowLog(const std::string& senderId, char digit, unsigned short duration, unsigned char volume);

    /** 세션 로컬 floor 이벤트 기록 — _recordDir/floor.jsonl 에 1줄 append.
     *  중앙 cmp_NN.flow.jsonl 과 별개로 세션 .d 에 floor 타임라인을 co-locate 한다
     *  (40명 세션의 화자교대/선점/거절을 시간별 로그 스캔 없이 재구성).
     *  op: GRANT|REVOKE|REJECT|RELEASE|IDLE|TAKEN, extraJson: 추가 키(앞에 , 없이) */
    void _logFloorLocal(const char* op, const std::string& user, unsigned int ssrc, int prio,
                        const char* extraJson = nullptr);

    // 하향 분배 — slot 은 화자의 동시 발언 슬롯(수신자별 egress SSRC/seq 를 가른다).
    void sendAudioToAll(const char* data, int len, const std::string& excludeSessionId, int slot);
    void sendFloorToAll(const char* data, int len);
    void sendVideoToAll(const char* data, int len, const std::string& excludeSessionId, int slot);
    void sendToMember(const std::string& sessionId, const char* data, int len);
    // 미협상 소스/미등록 멤버 드롭 (호출자가 _mutex 보유) — 카운터 + rate-limited WARN
    void _dropSrc(const char* what, const std::string& memberId, const std::string& ip, int port);
    void broadcastFloorStatus(unsigned char opcode, unsigned int ssrc, const std::string& speakerId);

    // ── Floor 송신 헬퍼 (TS 24.380 TLV) — 호출자는 _mutex 보유 ──
    // emergency/imminent tier + 동시 발언 정책(dual/multi) → Floor Indicator 비트마스크.
    int  _indicatorFor(const std::string& sessionId) const;
    // 화자에게 GRANTED(Duration+Granted Party+Indicator) 송신 + 전체 TAKEN + 녹취 시작.
    void _grantFloorTo(const std::string& sessionId, unsigned int ssrc, int prio,
                       bool preempt, const std::string& prevOwner);
    // 화자 1명 해제(RELEASE/REVOKE/이탈) 후 상태 수렴: 여유 정원만큼 큐 승급 →
    //   화자가 남아 있으면 잔여 화자 TAKEN 갱신, 아무도 없으면 IDLE + 녹취 세그먼트 종료.
    void _advanceFloorOrIdle();
    // Floor Deny(cause) 송신.
    void _sendDeny(const std::string& sessionId, unsigned int ssrc, int cause);
    // Floor Queue Position Info 송신(position=1-based, 없으면 0).
    void _sendQueuePos(const std::string& sessionId, unsigned int ssrc);
    // 큐 항목을 우선순위(tier>chair>prio>ts)로 정렬한 순서에서의 1-based 위치(없으면 0).
    int  _queuePositionOf(const std::string& sessionId) const;
    // 큐에서 최우선 대기자 추출(없으면 빈 문자열). 추출 시 큐에서 제거.
    std::string _popBestQueued(unsigned int& outSsrc, int& outPrio);

    // Floor 대기열 (TS 24.380 §8 queueing — SDP `mc_queueing` 광고).
    struct QueuedReq {
        std::string sessionId;
        unsigned int ssrc;
        int prio;
        int tier;
        bool chair;
        int64_t ts;
    };
    std::vector<QueuedReq> _floorQueue;
    bool _queueEnable = true;       // SDP mc_queueing 광고 → 비선점 요청은 큐잉
    int  _queueMax = 30;            // 큐 상한(초과 시 Deny cause=queue full)
    int  _grantDurationSec = 60;    // Floor Granted Duration 필드(최대 발언시간 표시값)

    std::string _groupId;
    
    struct Peer {
        std::string id;
        std::string ip;
        int port;
        int floorPort;       // floor control 포트 (m=application)
        unsigned int ssrc;   // 멤버 SSRC (joinGroup 시 할당)
        int videoPort;
        std::string role;    // "chair" | "participant" (TS 24.380 floor 선점 판정)
        // 수신자별 하향 스트림 — 동시 발언 슬롯마다 별도 SSRC/seq 를 쓴다. 슬롯 0 은 단일
        //   화자 정책의 유일 스트림(화자가 바뀌어도 연속) — 종전 동작 그대로다.
        uint16_t audioSeqOut[MCPTT_MAX_TALKER_SLOTS] = {0};
        uint16_t videoSeqOut[MCPTT_MAX_TALKER_SLOTS] = {0};
        int  streamSlot = 0;           // floor off(full-duplex) 시 이 멤버 상향의 고정 슬롯
        bool recvOnly = false;         // ambient 청취 leg — 상향 미디어 미중계 + floor 요청 거절
        bool floorSuppress = false;    // 이 멤버에게 floor 메시지 미송신 (청취 은닉)
        bool floorNatLatched = false;  // floor User ID latch 이력 — NAT 단말 표식
        PPttMemberPort* unit = nullptr;  // 멤버 전용 RTP 포트 유닛 (PCmpServer 소유)
        std::string declIp;   // 마지막 SDP 선언 주소 원본 (latch 와 무관하게 보존 —
        int declPort = 0;     //   재-JOIN 시 선언 불변 여부 비교용, PRtpRelay::Leg.decl* 와 동형)
        int declVideoPort = 0;

        // NAT 목적지 latch (제어평면이 nat 지정한 멤버만 — ue_nat_traversal.md §5)
        bool natEnabled = false;
        std::string sigIp;             // latch IP guard 기준 (빈 값 = guard 없음)
        // leg 별 PT 재작성 (cmp_media_api.md §6.1) — 0 = 재작성 없음(현행 PT-blind 통과).
        int ptOut = 0;      // egress audio PT — 이 leg 로 송신 시 스탬프 (user_pt)
        int srcPt = 0;      // ingress audio PT — 이 leg 가 송신에 쓰는 PT (user_src_pt)
        int tePtOut = 0;    // egress telephone-event PT (user_te_pt)
        int srcTePt = 0;    // ingress telephone-event PT (user_src_te_pt, TE 분류 기준)
        std::string codec;  // 협상 오디오 코덱 (user_codec) — 녹취 세그먼트 메타용
        bool natLatched = false;       // audio 소스 추종 학습 완료 (관측용)
        bool natLatchedVideo = false;
        int64_t followLogUsec = 0;     // dest follow 로그 rate-limit (소스 경합 시 폭주 방지)
    };

    // nat 멤버의 RTP 수신 판정+latch (호출자가 _mutex 보유). 수락 시 true.
    bool _acceptNatRtp(Peer& peer, bool isVideo, const std::string& ip, int port, const char* buf, int len);
    std::map<std::string, Peer> _members; // SessionID -> Peer
    std::map<std::string, int> _priorities; // SessionID (UserId) -> Priority
    std::map<std::string, std::string> _roles; // SessionID (UserId) -> role (chair/participant)
    std::map<std::string, int> _tier;       // SessionID -> FloorTier (없으면 TIER_NORMAL)
    bool isChair(const std::string& sessionId) const; // role==chair 여부
    PRtpMulticast* _pttSession;      // PTT 전용 세션 (audio RTP + floor + video)

    // ── Floor State — 발언자 집합 (정원 1 = 단일 화자, 2 = dual, N = multi-talker) ──
    struct Talker {
        std::string sessionId;
        unsigned int ssrc;      // 요청자 SSRC (GRANT/REVOKE 송신 대상)
        int prio;
        int slot;               // 하향 스트림/녹취 트랙 슬롯 (0..capacity-1)
        int64_t grantUsec;      // GRANT 시각 (무활동 판정 기준점 — RTP 무수신 grant 대비)
        int64_t lastRtpUsec;    // 마지막 RTP(audio/video) 수신 시각
    };
    std::vector<Talker> _talkers;   // grant 순서 — front() 가 대표 화자
    bool _floorControl = true;      // floor 제어 유무 (off = full-duplex)
    int  _floorPolicy = FLOOR_POLICY_SINGLE;
    int  _talkerCapacity = 1;       // 동시 발언 정원 (single=1/dual=2/multi=max_talkers)
    bool _privateCall = false;      // group_type=="private" — TS 24.380 §7 절차
    bool _initialGrantDone = false; // private call 개시자 초기 부여 완료 여부
    unsigned int _slotUsedMask = 0; // 현재 녹취 세그먼트에서 이미 쓴 슬롯 (트랙 화자 혼입 방지)
    int  _streamSlotNext = 0;       // floor off 멤버 스트림 슬롯 배정 커서

    // 발언자 조회/슬롯 배정 (호출자가 _mutex 보유)
    Talker* _talkerOf(const std::string& sessionId);
    bool _isTalker(const std::string& sessionId) const;
    // 현재 발언자 중 서열이 가장 낮은 화자 (선점 비교 대상). 없으면 nullptr.
    Talker* _weakestTalker();
    // TS 24.380 선점 서열 비교: condition tier > chair override > 수치 priority.
    //   private call(§7)은 chair 개념이 없어 tier·priority 만 본다.
    bool _preempts(const std::string& reqId, int reqPrio, const std::string& ownerId, int ownerPrio) const;
    // 여유 슬롯 배정 — 세그먼트 내 재사용이 필요하면 녹취 세그먼트를 먼저 끊는다(-1=정원 초과)
    int  _allocSlot();
    // 발언자 제거(해제/회수/이탈 공통) — 녹취 트랙 정리 포함. 제거했으면 true.
    bool _dropTalker(const std::string& sessionId);
    // 발언자 집합 통지 (FLOOR_TALKERS) — 집합이 바뀐 직후 호출
    void _notifyTalkers();
    // 정책 문자열 ("single"/"dual"/"multi"/"private"/"off")
    const char* _policyName() const;
    // 동시 발언 슬롯의 녹취 트랙명 ("audio"/"audio1".., "video"/"video1"..)
    static std::string _slotTrack(int slot, bool video);
    // 녹취 세그먼트 시작/재시작 (대표 화자 = 슬롯 0 또는 최초 화자)
    void _recStartSegment(const std::string& speakerId, int prio, bool preempt, const std::string& prevOwner);
    // 슬롯 트랙 등록 (0..slots-1) — 이미 등록된 슬롯은 건너뛴다
    void _recEnsureTracks(int slots);
    int  _recTrackSlots = 0;   // 현재 recorder 에 등록된 슬롯 트랙 수

    static int64_t _nowUsec();

    // Broadcast 그룹 (TS 24.380 §10.3) — 비면 일반(prearranged/chat) floor 정책.
    std::string _groupType;            // "broadcast" 면 개시자 외 floor REQUEST REJECT
    std::string _initiatorSessionId;   // 개시자(broadcaster) sessionId(=userId)
    static unsigned int _nextSsrc;  // SSRC 할당 카운터



    // DTMF PTT Config
    bool _dtmfEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;
    
    PMutex _mutex;
    LogFlowFunc _logFlow;  // floor event log callback
    TalkersFunc _onTalkers;  // 발언자 집합 변경 통지 (FLOOR_TALKERS)
    std::string _sesid;      // CSP 발행 세션 ID (이벤트 hdr)
    std::string _service;    // 서비스 (mcptt) — 이벤트 hdr
    bool _rtcpLogEnable = false; // 일반 RTCP 로깅 활성화 플래그
    PFloorCrypto _floorCrypto;   // floor RTCP SRTCP 보호 (미설정 = 평문 floor)
    // SRTCP 인증 실패/재전송 폐기 누적 — 판정이 _mutex 밖(복호가 파싱보다 앞)이라 atomic.
    std::atomic<long> _floorCryptoDrop{0};
    long _srcDrop = 0;           // 미협상 소스/미등록 멤버 드롭 누적
    time_t _lastDropWarn = 0;    // 드롭 WARN rate-limit
    time_t _createdTime = time(nullptr);  // 그룹 생성 시각 (audit grace) — 구성 시점 고정

    // 녹취 (Floor 단위 세그먼트 — 화자 교대 시 파일 분할)
    bool _recordEnable;
    std::string _recordDir;
    PSyncRtpRecorder* _recorder;

public:
    void setRecording(bool enable, const std::string& dir);
    bool isRecordEnabled() const { return _recordEnable; }
    void startRecording();
    void stopRecording();
};

#endif // __MCPTT_GROUP_H__
