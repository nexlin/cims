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
#include <memory>
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
    FLOOR_ACK      = 10,  // Floor Ack               (양방향)
    FLOOR_MEDIA_FLOW     = 0x0B, // Unicast Media Flow Control (UE→서버) — 자기 하향 미디어 중단/재개
    FLOOR_QUEUED_CANCEL  = 0x0E, // Queued Floor Requests (양방향) — 대기 요청 취소/결과/통지
    FLOOR_RELEASE_MULTI  = 0x0F  // Floor Release Multi Talker (서버→UE 전용, Rel-16 multi-talker):
                                 //   동시 발언 중 한 명이 발언을 끝냈음을 나머지 참가자에게
                                 //   알린다(§8.2.14). 잔여 화자가 있으므로 Floor Idle 은 보내지
                                 //   않는다. 단말이 이 subtype 을 보내면 규격 위반이라 무시한다.
};

// subtype 첫 비트(0x10) = "Acknowledgment is required" 변종 (TS 24.380 §8.2.2 Table 8.2.2.1-1).
//   Granted(x0001)/Taken(x0010)/Deny(x0011)/Release(x0100)/Idle(x0101)/QueuePosInfo(x1001) 등에
//   정의된다 — 수신 시 이 비트를 걷어내 기본 타입으로 처리하고 Floor Ack 로 응답해야 한다.
#define FLOOR_ACK_REQ_BIT 0x10
#define FLOOR_OP(subtype) ((subtype) & 0x0F)

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
    FF_SSRC           = 14,
    FF_GRANTED_USERS  = 15,  // List of Granted Users (multi-talker, 문자열 리스트)
    FF_SSRC_LIST      = 16,  // List of SSRCs         (multi-talker, 화자 순서 동일)
    FF_QUEUED_PURPOSE = 21,  // Queued Floor Requests Purpose (0=Cancel Request/1=Result/2=Notification)
    FF_QUEUED_USERS   = 22,  // List of Queued Users (문자열 리스트)
    FF_QUEUED_RESULT  = 23,  // Queued Floor Requests Result
    FF_MEDIA_FLOW     = 24   // Media Flow Control Indicator (MSB=1 재개 / 0 중단)
};

// Queued Floor Requests Purpose (§8.2.3.23) / Result (§8.2.3.25) 값.
enum FloorQueuedPurpose {
    QFR_CANCEL_REQUEST = 0,
    QFR_CANCEL_RESULT  = 1,
    QFR_CANCEL_NOTIFY  = 2
};
enum FloorQueuedResult {
    QFR_OK            = 0,   // 지정된(또는 전체) 대기 요청을 모두 제거
    QFR_QUEUE_EMPTY   = 2,   // 대기열이 이미 비어 있음
    QFR_NOT_QUEUED    = 3,   // 지정된 사용자들의 대기 요청이 없음
    QFR_PARTIAL       = 5    // 일부 사용자의 대기 요청이 없음
};

// Media Flow Control Indicator (§8.2.3.26) — 값의 MSB(A 비트).
#define FLOOR_MEDIA_RESUME_BIT 0x80

// Source 필드 값 (§8.2.3.12) — Floor Ack 가 "누가 보낸 확인인지"를 싣는다.
enum FloorSourceId {
    FLOOR_SRC_PARTICIPANT     = 0,
    FLOOR_SRC_PARTICIPATING   = 1,
    FLOOR_SRC_CONTROLLING     = 2,  // CMP = controlling MCPTT function 의 미디어 평면
    FLOOR_SRC_NON_CONTROLLING = 3
};

// Permission to Request the Floor 값 (§8.2.3.7) — Floor Taken 수신자의 발언 요청 가부.
enum FloorPermission {
    FLOOR_PERM_DENIED  = 0,   // broadcast 그룹·ambient 청취 leg
    FLOOR_PERM_ALLOWED = 1
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
    CAUSE_REVOKE_TOO_LONG     = 2,   // Media burst too long (T2 Stop talking 만료)
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

// TLV 필드 값 빌더(big-endian u16 / priority 2옥텟 / queue-info 2옥텟 / SSRC 6옥텟 / 리스트).
std::string FloorU16(int v);
std::string FloorPriority(int prio);
std::string FloorQueueInfo(int position, int prio);
std::string FloorSsrc(unsigned int ssrc);
std::string FloorUserList(const std::vector<std::string>& users);
std::string FloorSsrcList(const std::vector<unsigned int>& ssrcs);

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

    // 멤버 프로파일 (PTT_JOIN 확장 — cmp_media_api.md §7.4).
    //   mcpttId: floor 메시지의 User ID/Granted Party 에 실을 **MCPTT ID(URI)**. 비면 sessionId.
    //   queueing: 이 멤버가 SDP `mc_queueing` 을 협상했는지 — 미협상 멤버의 비선점 요청은
    //             큐잉하지 않고 Deny #1 이다(TS 24.380 §6.3.5.4.4).
    //   maxPriority: SDP `mc_priority` 로 협상한 요청 가능 최대 우선순위(-1 = 미협상).
    void setMemberProfile(const std::string& sessionId, const std::string& mcpttId, bool queueing,
                          int maxPriority = -1);
    // 호 성립 시 초기 발언권 부여 (SDP fmtp `mc_granted` 협상 결과 — TS 24.380 §6.3.4.2.2-3b).
    //   발언자가 없고 floor 제어가 켜진 그룹에서만 성립한다. 부여했으면 true.
    bool grantInitialFloor(const std::string& sessionId);

    // Floor Control Logic
    //   indicatorBits: 수신 REQUEST 의 Floor Indicator(emergency/imminent) 비트마스크(-1=없음).
    //   reqPrio: 수신 REQUEST 의 Floor Priority(-1=미포함). 협상 상한(멤버 priority)으로 clamp 한다.
    void handleFloorRequest(const std::string& sessionId, unsigned int userId, int indicatorBits = -1,
                            int reqPrio = -1);
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
    //   **그룹 단위 키**는 모든 멤버에게 같은 키를 쓰는 경우(멀티캐스트/MBMS MuSiK 대응)이고,
    //   유니캐스트 floor 는 TS 33.180 §9.4 대로 **클라이언트별 CSK**(setMemberCrypto)가 정본이다.
    bool setFloorCrypto(const std::string& alg, const std::string& key, const std::string& salt,
                        const std::string& mki, std::string& err);
    // 멤버별 floor 보호 키 (PTT_JOIN.floor_crypto — 클라이언트의 CSK/CSK-ID 로 유도된 값).
    //   설정한 멤버의 floor 메시지는 그 키로만 보호·해제된다. 미설정 멤버는 그룹 키를 쓴다.
    bool setMemberCrypto(const std::string& sessionId, const std::string& alg, const std::string& key,
                         const std::string& salt, const std::string& mki, std::string& err);
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

    // Floor 타이머 값 (TS 24.380 §11.1.3 — 초 단위, 0=비활성). PCmpServer 가 설정에서 읽어
    //   그룹 생성 시 넣어준다.
    //   t1: End of RTP media   — 마지막 RTP 후 이 시간이 지나면 **발언 완료**로 보고 회수
    //                            (Revoke 를 보내지 않는다, §6.3.4.4.3). 규격 기본 4초·최대 6초.
    //   t2: Stop talking       — 최대 발언시간. Floor Granted 의 Duration 으로 광고하고,
    //                            초과하면 Revoke cause #2(Media burst too long). 규격 기본 30초.
    //   t3: Stop talking grace — Revoke 후 Floor Release 를 기다리는 유예(그 동안 미디어 유지).
    //   t8: Floor Revoke       — 유예 중 Revoke 재전송 간격.
    //   t7: Floor Idle         — Floor Idle 재송신 간격(0=비활성, C7=3회까지).
    //   t20: Floor Granted     — 큐에서 승급한 화자에게 Granted 재송신 간격(첫 RTP 까지, C20=3회).
    void setFloorTimers(int t1, int t2, int t3, int t8, int t7 = 0, int t20 = 1);

    // Floor 타이머 점검 (T1/T2/T3/T8) — PCmpServer::timeoutLoop 가 1초마다 호출한다.
    //   발언자 집합이 바뀌었으면 true.
    bool tickFloorTimers();

private:
    struct Talker;   // 발언자 레코드 (정의는 아래 Floor State 절)
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
    // floor 브로드캐스트. roData 가 있으면 recv_only(ambient 청취) 멤버에게만 그 변형을 보낸다
    //   (Floor Taken 의 Permission to Request the Floor 를 수신자별로 달리하기 위한 것).
    //   excludeSessionId 는 제외할 멤버 — Floor Taken 은 화자 본인에게 보내지 않는다(§6.3.4.4.2-3).
    void sendFloorToAll(const char* data, int len, const char* roData = nullptr, int roLen = 0,
                        const std::string& excludeSessionId = "");
    void sendVideoToAll(const char* data, int len, const std::string& excludeSessionId, int slot);
    void sendToMember(const std::string& sessionId, const char* data, int len);
    // 이 멤버의 floor 메시지에 적용할 SRTCP 컨텍스트 (멤버 키 > 그룹 키 > 평문=null).
    PFloorCrypto* _cryptoFor(const std::string& sessionId);
    // 멤버 전용 키를 가진 멤버가 하나라도 있는지 (미식별 소스의 복호 시도 여부 판단)
    bool _anyMemberCrypto() const;
    // 수신 floor 패킷 해제 — sessionId 를 알면 그 멤버 키로, 모르면(NAT 미식별) 그룹 키와
    //   각 멤버 키를 차례로 시도한다. 성공 시 out/outLen 에 평문을 담고 true.
    bool _unprotectFloor(const std::string& sessionId, const char* in, int inLen,
                         char* out, int outCap, int& outLen);
    // 미협상 소스/미등록 멤버 드롭 (호출자가 _mutex 보유) — 카운터 + rate-limited WARN
    void _dropSrc(const char* what, const std::string& memberId, const std::string& ip, int port);
    void broadcastFloorStatus(unsigned char opcode, unsigned int ssrc, const std::string& speakerId);

    // ── Floor 송신 헬퍼 (TS 24.380 TLV) — 호출자는 _mutex 보유 ──
    // emergency/imminent tier + 동시 발언 정책(dual/multi) → Floor Indicator 비트마스크.
    int  _indicatorFor(const std::string& sessionId) const;
    // 화자와 무관한 메시지(Floor Idle)용 그룹 성격 비트 — normal/broadcast/multi.
    int  _groupIndicator() const;
    // SSRC 필드(14)/List of SSRCs(16)에 실을 값 — 학습한 단말 SSRC(없으면 CMP 할당 SSRC).
    unsigned int _uaSsrcOf(const std::string& sessionId) const;
    // Floor Taken/Idle 을 묶는 Message Sequence Number (§8.2.3.10) — 송신마다 +1, 65535 순환.
    int  _nextMsgSeq() { _msgSeq = (unsigned short)(_msgSeq + 1); return _msgSeq; }
    // Ack 요구(subtype 첫 비트=1) 메시지에 대한 Floor Ack 응답 (§8.2.13).
    void _sendFloorAck(const std::string& sessionId, int ackedSubtype);
    // 동시 발언 중 한 화자의 발언 종료를 나머지 참가자에게 통지 (§8.2.14 Floor Release Multi Talker).
    void _sendReleaseMultiTalker(const std::string& sessionId, unsigned int ssrc);
    // floor 메시지에 실을 사용자 식별자 — MCPTT ID(URI)가 있으면 그것, 없으면 sessionId (§8.2.3.8).
    const std::string& _userIdOf(const std::string& sessionId) const;
    // Unicast Media Flow Control(0x0B) 수신 — 이 멤버의 하향 미디어 중단/재개 (§6.3.4.4.14~15).
    void _handleMediaFlowControl(const std::string& sessionId, const ParsedFloor& msg);
    // Queued Floor Requests(0x0E) 수신 — 대기 요청 취소 처리 + 결과/통지 회신 (§6.3.4.4.13).
    void _handleQueuedCancel(const std::string& sessionId, unsigned int ssrc, const ParsedFloor& msg);
    // 화자에게 GRANTED(Duration+Granted Party+Indicator) 송신 + 전체 TAKEN + 녹취 시작.
    //   fromQueue=true 면 대기열 승급이라 T20(Granted 재송신)을 건다.
    void _grantFloorTo(const std::string& sessionId, unsigned int ssrc, int prio,
                       bool preempt, const std::string& prevOwner, bool fromQueue = false);
    // 화자 1명 해제(RELEASE/REVOKE/이탈) 후 상태 수렴: 여유 정원만큼 큐 승급 →
    //   화자가 남아 있으면 잔여 화자 TAKEN 갱신, 아무도 없으면 IDLE + 녹취 세그먼트 종료.
    void _advanceFloorOrIdle();
    // Floor Deny(cause) 송신.
    void _sendDeny(const std::string& sessionId, unsigned int ssrc, int cause);
    // Floor Revoke(cause) 송신 (§8.2.10) — 선점·정원 축소·최대 발언시간 초과 공통.
    void _sendRevoke(const std::string& sessionId, int cause);
    // 'G: pending Floor Revoke' 진입 (§6.3.4.5.2) — Revoke 송신 + T3/T8 무장. T3=0 이면
    //   유예 없이 즉시 회수한다(audio cut-in). 회수까지 끝났으면 true.
    bool _beginRevoke(Talker& t, int cause);
    // 이미 발언 중인 참가자의 재요청에 Floor Granted 를 재송신 (§6.3.4.4.8) — Duration 은 남은 T2.
    void _resendGrant(const Talker& t);
    // 선점 요청자를 대기열 **맨 앞**에 넣는다(§6.3.4.4.7-2e). 이미 있으면 앞으로 당긴다.
    void _queueFront(const std::string& sessionId, unsigned int ssrc, int prio);
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
        // 선점 요청자 — 회수 유예(T3) 동안 대기열 맨 앞을 차지한다(§6.3.4.4.7-2e).
        bool front = false;
    };
    // 대기열 서열 비교 (a 가 b 보다 앞) — 선점 대기 > tier > chair > 수치 priority > 도착순.
    static bool _queueBetter(const QueuedReq& a, const QueuedReq& b);
    std::vector<QueuedReq> _floorQueue;
    bool _queueEnable = true;       // SDP mc_queueing 광고 → 비선점 요청은 큐잉
    int  _queueMax = 30;            // 큐 상한(초과 시 Deny cause=queue full)
    // Floor 타이머 (§11.1.3 권고값) — setFloorTimers 로 주입.
    int  _t1EndRtpSec   = 4;        // T1 End of RTP media (규격 기본 4초, 최대 6초)
    int  _t2StopTalkSec = 30;       // T2 Stop talking — Granted Duration 값이기도 하다
    int  _t3GraceSec    = 3;        // T3 Stop talking grace (audio cut-in 이면 0)
    int  _t8RevokeSec   = 1;        // T8 Floor Revoke 재전송 간격
    int  _t7IdleSec     = 0;        // T7 Floor Idle 재송신 간격 (0=비활성)
    int  _t20GrantSec   = 1;        // T20 Floor Granted 재송신 간격 (큐 승급 화자 한정)
    // C7/C20 재송신 상한 (§6.3.4.3.4 / §6.3.4.4.9) — 도달 보장용이라 작게 잡는다.
    static const int kIdleResendMax  = 3;
    static const int kGrantResendMax = 3;
    int64_t _idleSinceUsec = 0;     // 마지막 Floor Idle 송신 시각 (0=Idle 상태 아님)
    int     _idleResendLeft = 0;    // 남은 Floor Idle 재송신 횟수 (C7)

    std::string _groupId;
    
    struct Peer {
        std::string id;
        std::string ip;
        int port;
        int floorPort;       // floor control 포트 (m=application)
        unsigned int ssrc;   // 멤버 SSRC (joinGroup 시 할당) — 하향 스트림 SSRC 파생 기준
        // 단말이 floor 메시지 헤더에 쓰는 자기 SSRC (첫 수신 시 학습, 0=미상).
        //   Floor Granted/Taken 의 SSRC 필드(14)·List of SSRCs(16)에 싣는 값이다 —
        //   규격이 말하는 "SSRC of granted floor participant" 는 단말의 SSRC 다(§8.2.3.16).
        unsigned int uaSsrc = 0;
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
        std::string mcpttId;  // MCPTT ID(URI) — floor User ID/Granted Party 값 (비면 sessionId)
        bool queueing = true; // SDP mc_queueing 협상 여부 — 미협상이면 비선점 요청은 Deny #1
        // SDP mc_priority 로 협상한 **요청 가능 최대 우선순위**. -1 = 미협상 —
        //   이 경우 요청에 실린 Floor Priority 는 무시하고 기본 우선순위를 쓴다(§6.3.5.4.4-1a-iv).
        int maxPriority = -1;
        bool mediaStopped = false;  // Unicast Media Flow Control(0x0B)로 하향 미디어 중단 요청됨
        // 이 멤버 전용 floor SRTCP 컨텍스트 (CSK 기반). null 이면 그룹 키를 쓴다.
        //   Peer 는 map 에 복사 대입되므로 shared_ptr 로 들고 있는다(PFloorCrypto 는 mutex 보유).
        std::shared_ptr<PFloorCrypto> crypto;
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
        int64_t grantUsec;      // GRANT 시각 (T1 판정 기준점 — RTP 무수신 grant 대비)
        int64_t lastRtpUsec;    // 마지막 RTP(audio/video) 수신 시각 — T1(End of RTP media)
        int64_t talkStartUsec = 0;  // 첫 RTP 수신 시각 — T2(Stop talking) 기준(0=미개시)
        // 'G: pending Floor Revoke' (§6.3.4.5) — Revoke 를 보낸 뒤 T3(Stop talking grace)
        //   동안 화자의 미디어를 계속 중계하며 Floor Release 를 기다리는 상태.
        bool    revokePending = false;
        int     revokeCause = 0;
        int64_t revokeSentUsec = 0;      // 마지막 Revoke 송신 시각 — T8 재전송 기준
        int64_t revokeDeadlineUsec = 0;  // T3 만료 시각 — 넘기면 강제 회수
        // T20(Floor Granted) — 대기열에서 승급한 화자에게만 건다(§6.3.4.4.2-2). 단말이 Granted 를
        //   놓치면 발언 기회가 통째로 사라지므로 첫 RTP 가 올 때까지 C20 회 재송신한다.
        int     grantRetxLeft = 0;
        int64_t grantSentUsec = 0;
    };
    std::vector<Talker> _talkers;   // grant 순서 — front() 가 대표 화자
    bool _floorControl = true;      // floor 제어 유무 (off = full-duplex)
    int  _floorPolicy = FLOOR_POLICY_SINGLE;
    int  _talkerCapacity = 1;       // 동시 발언 정원 (single=1/dual=2/multi=max_talkers)
    bool _privateCall = false;      // group_type=="private" — TS 24.380 §7 절차
    bool _initialGrantDone = false; // 초기 발언권(mc_granted) 부여 완료 여부 — 1회만
    unsigned int _slotUsedMask = 0; // 현재 녹취 세그먼트에서 이미 쓴 슬롯 (트랙 화자 혼입 방지)
    int  _streamSlotNext = 0;       // floor off 멤버 스트림 슬롯 배정 커서

    // 발언자 조회/슬롯 배정 (호출자가 _mutex 보유)
    Talker* _talkerOf(const std::string& sessionId);
    bool _isTalker(const std::string& sessionId) const;
    // 현재 발언자 중 서열이 가장 낮은 화자 (선점 비교 대상). 없으면 nullptr.
    //   skipPending=true 면 이미 회수 진행 중(pending Revoke)인 화자는 후보에서 제외한다.
    Talker* _weakestTalker(bool skipPending = false);
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
    // 슬롯 트랙에 화자 귀속 + 그 화자 leg 의 PT/코덱 부착 (음성·영상 트랙 공통)
    void _recAttachSlot(int slot, const std::string& sessionId);
    // 슬롯 트랙의 화자 구간 종료 (발언 종료·회수 — 슬롯 재사용 시 귀속이 섞이지 않게)
    void _recDetachSlot(int slot);
    int  _recTrackSlots = 0;   // 현재 recorder 에 등록된 슬롯 트랙 수

    static int64_t _nowUsec();

    // Broadcast 그룹 (TS 24.380 §10.3) — 비면 일반(prearranged/chat) floor 정책.
    std::string _groupType;            // "broadcast" 면 개시자 외 floor REQUEST REJECT
    std::string _initiatorSessionId;   // 개시자(broadcaster) sessionId(=userId)
    static unsigned int _nextSsrc;  // SSRC 할당 카운터

    // 서버→단말 floor 메시지의 RTCP 헤더 SSRC = **floor control server 의 SSRC**
    //   (§8.2.5/§8.2.8/§8.2.9 — 화자 SSRC 는 SSRC 필드(14)/List of SSRCs(16)로 싣는다).
    unsigned int _serverSsrc;
    unsigned short _msgSeq = 0;     // Floor Taken/Idle Message Sequence Number



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
    std::string _recordDir;     // 그룹 base — CSP 가 record_dir 로 지정
    std::string _recordSesDir;  // 세션 디렉터리 이름 S{ts}_{n} — 시간버킷 아래 한 겹
    PSyncRtpRecorder* _recorder;

public:
    /** sesDir = PTT_GROUP_ADD 의 session_dir. 기록 자리는 dir/{시간버킷}/{sesDir}/ 이다. */
    void setRecording(bool enable, const std::string& dir, const std::string& sesDir = "");
    bool isRecordEnabled() const { return _recordEnable; }
    void startRecording();
    void stopRecording();
};

#endif // __MCPTT_GROUP_H__
