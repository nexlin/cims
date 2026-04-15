#ifndef __PRTP_MULTICAST_H__
#define __PRTP_MULTICAST_H__

#include <string>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"

class PMcpttGroup;

/**
 * PTT 1:N RTP 분배 (PMcpttGroup 연동)
 *
 * 3소켓: audio RTP + floor control + video RTP
 * 패킷 수신 → PMcpttGroup 콜백 → McpttGroup이 멤버에게 분배
 * 녹취는 McpttGroup이 floor 단위로 직접 관리.
 */
class PRtpMulticast : public PHandler
{
public:
    PRtpMulticast(const std::string& name);
    virtual ~PRtpMulticast();

    // ── Lifecycle ──
    bool init(const std::string& ip, unsigned int rtpPort, unsigned int floorPort, unsigned int videoPort = 0);
    bool final();
    void reset();

    // ── 포트 조회 ──
    unsigned int getLocalRtpPort() const { return _localRtpPort; }
    unsigned int getLocalFloorPort() const { return _localFloorPort; }
    unsigned int getLocalVideoPort() const { return _localVideoPort; }

    // ── PMcpttGroup 연동 ──
    void setGroup(PMcpttGroup* g);
    PMcpttGroup* getGroup() const { return _group; }

    // ── 전송 (McpttGroup에서 호출) ──
    void sendAudioTo(const std::string& ip, int port, char* data, int len);
    void sendFloorTo(const std::string& ip, int port, char* data, int len);
    void sendVideoTo(const std::string& ip, int port, char* data, int len);

    // ── 식별 ──
    void setSessionId(const std::string& id) { _sessionId = id; }
    std::string getSessionId() const { return _sessionId; }
    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }

    // ── 활성도 ──
    void touchActivity() { time(&_lastActivityTime); }
    time_t getLastActivityTime() const { return _lastActivityTime; }

    // ── Worker 메인 루프 ──
    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

private:
    // 소켓별 수신 → 콜백 헬퍼
    template<typename Callback>
    void _drainSocket(PRtpSocket& sock, Callback cb);

    PRtpSocket  _rtpSock;
    PRtpSocket  _floorSock;
    PRtpSocket  _videoRtpSock;

    PMutex      _mutex;
    PMcpttGroup* _group = nullptr;
    std::string _sessionId;
    std::string _workerName;
    time_t      _lastActivityTime = 0;
    unsigned int _localRtpPort = 0;
    unsigned int _localFloorPort = 0;
    unsigned int _localVideoPort = 0;
};

#endif // __PRTP_MULTICAST_H__
