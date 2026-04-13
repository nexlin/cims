#ifndef __MCPTT_GROUP_H__
#define __MCPTT_GROUP_H__

#include <string>
#include <map>
#include <vector>
#include <set>
#include <cstdint>
#include <functional>
#include "pbase.h"

// Forward declaration
class PRtpTrans;
class PPttTrans;
class RtpRecorder;

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

class McpttGroup {
public:
    McpttGroup(const std::string& groupId);
    virtual ~McpttGroup();

    void setSharedSession(PRtpTrans* session) { _sharedSession = session; }
    PRtpTrans* getSharedSession() const { return _sharedSession; }
    void setPttSession(PPttTrans* session) { _pttSession = session; }
    PPttTrans* getPttSession() const { return _pttSession; }
    void addMember(const std::string& sessionId, const std::string& ip, int port, int floorPort = 0, int videoPort = 0);
    void removeMember(const std::string& sessionId);
    bool hasMember(const std::string& sessionId);

    // Floor Control Logic
    void handleFloorRequest(const std::string& sessionId, unsigned int userId);
    void handleFloorRelease(const std::string& sessionId, unsigned int userId);

    // Called by PRtpTrans when an RTCP packet is received (legacy)
    void onRtcpPacket(const std::string& ip, int port, char* buf, int len);
    // Called by PPttTrans when a floor control packet is received (m=application)
    void onFloorPacket(const std::string& ip, int port, char* buf, int len);

    // Called by PRtpTrans when an RTP packet is received
    void onRtpPacket(const std::string& ip, int port, char* buf, int len);

    // Called by PRtpTrans when a Video RTP packet is received
    // Called by PRtpTrans when a Video RTP packet is received
    void onVideoRtpPacket(const std::string& ip, int port, char* buf, int len);

    void updatePriorities(const std::map<std::string, int>& priorities);
    void setDtmfConfig(bool enable, const std::string& pushDigit, const std::string& releaseDigit);

    // Floor 이벤트 로그 콜백 (CmpServer::logFlow 연결용)
    using LogFlowFunc = std::function<void(const std::string& key, const char* from, const char* to,
                                            const char* proto, const char* label, const char* body)>;
    void setLogCallback(LogFlowFunc fn) { _logFlow = fn; }

    int getMemberCount() const { return (int)_members.size(); }
    std::string getFloorHolder() const { return _floorTaken ? _floorOwnerSessionId : ""; }

private:
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
        uint16_t audioSeqOut;   // 수신자별 오디오 시퀀스 카운터
        uint16_t videoSeqOut;   // 수신자별 비디오 시퀀스 카운터
        uint32_t audioSsrcOut;  // 수신자에게 보내는 고정 오디오 SSRC
        uint32_t videoSsrcOut;  // 수신자에게 보내는 고정 비디오 SSRC
    };
    std::map<std::string, Peer> _members; // SessionID -> Peer
    std::map<std::string, int> _priorities; // SessionID (UserId) -> Priority
    PRtpTrans* _sharedSession;   // VoIP 공유 세션 (legacy)
    PPttTrans* _pttSession;      // PTT 전용 세션 (audio RTP + floor)
    
    // Floor State
    bool _floorTaken;
    std::string _floorOwnerSessionId;
    unsigned int _floorOwnerSsrc;
    static unsigned int _nextSsrc;  // SSRC 할당 카운터



    // DTMF PTT Config
    bool _dtmfEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;
    
    PMutex _mutex;
    LogFlowFunc _logFlow;  // floor event log callback

    // 녹취 (세션 단위 — PTT는 한 시점에 한 명만 발언하므로 단일 파일로 충분)
    bool _recordEnable;
    std::string _recordDir;
    RtpRecorder* _sessionRecorderAudio;
    RtpRecorder* _sessionRecorderVideo;

public:
    void setRecording(bool enable, const std::string& dir);
    bool isRecordEnabled() const { return _recordEnable; }
    void startRecording();
    void stopRecording();
};

#endif // __MCPTT_GROUP_H__
