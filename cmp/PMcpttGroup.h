#ifndef __MCPTT_GROUP_H__
#define __MCPTT_GROUP_H__

#include <string>
#include <map>
#include <vector>
#include <set>
#include <cstdint>
#include <functional>
#include "pbase.h"

class PRtpRelay;
class PRtpMulticast;
class PSyncRtpRecorder;

// RTCP APP Packet for Floor Control (Simplified)
// 3GPP TS 24.379 uses specific RTCP APP packets. 
// We will use a simplified structure for this implementation.

#define RTCP_PT_APP 204

enum FloorOpCode {
    FLOOR_REQUEST = 1,
    FLOOR_GRANT   = 2,
    FLOOR_REJECT  = 3,
    FLOOR_RELEASE = 4,
    FLOOR_IDLE    = 5,
    FLOOR_TAKEN   = 6,
    FLOOR_REVOKE  = 7
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

// 고정 헤더 (12 bytes RTCP APP + 8 bytes app-data)
struct FloorControlPacket {
    unsigned char version_subtype; // V=2, P=0, Subtype=...
    unsigned char type;            // PT=204 (APP)
    unsigned short length;
    unsigned int ssrc;             // SSRC of sender
    char name[4];                  // "MCPT"
    unsigned char opcode;          // FloorOpCode
    unsigned char id_len;          // speaker identity 문자열 길이 (0이면 없음)
    unsigned short reserved;
    // 가변: char speaker_id[id_len] + padding (4-byte aligned)
};

// Floor 패킷 빌드 헬퍼 (speaker_id 문자열 포함)
int BuildFloorPacket(char* buf, int bufSize, unsigned char opcode,
                     unsigned int ssrc, const std::string& speakerId);
// Floor 패킷에서 speaker_id 추출
std::string ParseFloorSpeakerId(const char* buf, int len);

class PMcpttGroup {
public:
    PMcpttGroup(const std::string& groupId);
    virtual ~PMcpttGroup();

    void setPttSession(PRtpMulticast* session) { _pttSession = session; }
    PRtpMulticast* getPttSession() const { return _pttSession; }
    void addMember(const std::string& sessionId, const std::string& ip, int port, int floorPort = 0, int videoPort = 0,
                   const std::string& role = "participant");
    void removeMember(const std::string& sessionId);
    bool hasMember(const std::string& sessionId);

    // Floor Control Logic
    void handleFloorRequest(const std::string& sessionId, unsigned int userId);
    void handleFloorRelease(const std::string& sessionId, unsigned int userId);

    // Called by PRtpRelay when an RTCP packet is received (legacy)
    void onRtcpPacket(const std::string& ip, int port, char* buf, int len);
    // Called by PRtpMulticast when a floor control packet is received (m=application)
    void onFloorPacket(const std::string& ip, int port, char* buf, int len);

    // Called by PRtpRelay when an RTP packet is received
    void onRtpPacket(const std::string& ip, int port, char* buf, int len);

    // Called by PRtpRelay when a Video RTP packet is received
    // Called by PRtpRelay when a Video RTP packet is received
    void onVideoRtpPacket(const std::string& ip, int port, char* buf, int len);

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

    int getMemberCount() const { return (int)_members.size(); }
    std::string getFloorHolder() const { return _floorTaken ? _floorOwnerSessionId : ""; }

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

    void sendAudioToAll(const char* data, int len, const std::string& excludeIp, int excludePort);
    void sendAudioRtcpToAll(const char* data, int len, const std::string& excludeIp, int excludePort);
    // Video functions for future use
    void sendVideoToAll(const char* data, int len, const std::string& excludeIp, int excludePort);
    void sendVideoRtcpToAll(const char* data, int len, const std::string& excludeIp, int excludePort);
    void sendToMember(const std::string& sessionId, const char* data, int len);
    void broadcastFloorStatus(unsigned char opcode, unsigned int ssrc, const std::string& speakerId);

    std::string _groupId;
    
    struct Peer {
        std::string id;
        std::string ip;
        int port;
        int floorPort;       // floor control 포트 (m=application)
        unsigned int ssrc;   // 멤버 SSRC (joinGroup 시 할당)
        int videoPort;
        std::string role;    // "chair" | "participant" (TS 24.380 floor 선점 판정)
        uint16_t audioSeqOut;   // 수신자별 오디오 시퀀스 카운터
        uint16_t videoSeqOut;   // 수신자별 비디오 시퀀스 카운터
        uint32_t audioSsrcOut;  // 수신자에게 보내는 고정 오디오 SSRC
        uint32_t videoSsrcOut;  // 수신자에게 보내는 고정 비디오 SSRC
    };
    std::map<std::string, Peer> _members; // SessionID -> Peer
    std::map<std::string, int> _priorities; // SessionID (UserId) -> Priority
    std::map<std::string, std::string> _roles; // SessionID (UserId) -> role (chair/participant)
    std::map<std::string, int> _tier;       // SessionID -> FloorTier (없으면 TIER_NORMAL)
    bool isChair(const std::string& sessionId) const; // role==chair 여부
    PRtpMulticast* _pttSession;      // PTT 전용 세션 (audio RTP + floor + video)
    
    // Floor State
    bool _floorTaken;
    std::string _floorOwnerSessionId;
    unsigned int _floorOwnerSsrc;
    int64_t _floorGrantUsec = 0;   // floor GRANT 시각 (무활동 판정 기준점 — RTP 무수신 grant 대비)
    int64_t _lastRtpUsec = 0;      // owner 의 마지막 RTP(audio/video) 수신 시각

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
    bool _rtcpLogEnable = false; // 일반 RTCP 로깅 활성화 플래그

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
