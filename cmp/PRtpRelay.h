#ifndef __PRTP_RELAY_H__
#define __PRTP_RELAY_H__

#include <string>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"

class PMcpttGroup;
class PSyncRtpRecorder;

/**
 * VoIP 1:1 RTP Relay (B2BUA)
 *
 * 4소켓: audio RTP/RTCP + video RTP/RTCP
 * 2-peer: peer[0]=발신(A), peer[1]=착신(B)
 * 패킷을 반대편 peer에게 relay하고 녹취한다.
 */
class PRtpRelay : public PHandler
{
public:
    PRtpRelay(const std::string& name);
    virtual ~PRtpRelay();

    bool init(const std::string& ip, unsigned int rtpPort, unsigned int videoPort = 0);
    bool final();
    void reset();

    bool setRemote(const std::string& ip, unsigned int port, unsigned int videoPort = 0, int peerIdx = -1);

    unsigned int getLocalPort() const { return _localPort; }
    unsigned int getLocalVideoPort() const { return _localVideoPort; }

    void setSessionId(const std::string& id) { _sessionId = id; }
    std::string getSessionId() const { return _sessionId; }
    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }
    void touchActivity() { time(&_lastActivityTime); }
    time_t getLastActivityTime() const { return _lastActivityTime; }

    void sendVideoTo(const std::string& ip, int port, char* data, int len);

    void startRecording(const std::string& rawDir, const std::string& sessionId,
                        const std::string& caller = "", const std::string& callee = "",
                        int segmentIntervalSec = 60);
    void stopRecording();
    bool isRecording() const { return _recorder != nullptr; }

    void setGroup(PMcpttGroup* g);

    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

private:
    struct PeerInfo {
        std::string ip;
        unsigned int port = 0;
        unsigned int videoPort = 0;
        struct sockaddr_in addrRtp{};
        struct sockaddr_in addrRtcp{};
        struct sockaddr_in addrVideoRtp{};
        struct sockaddr_in addrVideoRtcp{};
        bool active = false;
    };

    int _findPeerIndex(const std::string& ip, int port, bool isVideo = false);

    PRtpSocket _rtpSock;
    PRtpSocket _rtcpSock;
    PRtpSocket _videoRtpSock;
    PRtpSocket _videoRtcpSock;

    PMutex      _mutex;
    PMcpttGroup* _group = nullptr;
    std::string _sessionId;
    std::string _workerName;
    time_t      _lastActivityTime = 0;
    unsigned int _localPort = 0;
    unsigned int _localVideoPort = 0;
    PeerInfo    _peers[2];

    // 녹취
    PSyncRtpRecorder* _recorder = nullptr;
    int _segmentIntervalSec = 60;
    int64_t _segStartUsec = 0;
    bool _firstRtpReceived = false;
};

#endif // __PRTP_RELAY_H__
