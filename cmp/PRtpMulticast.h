#ifndef __PRTP_MULTICAST_H__
#define __PRTP_MULTICAST_H__

#include <string>
#include <vector>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"

class PMcpttGroup;

/**
 * PTT 그룹 floor control 유닛 (PMcpttGroup 연동)
 *
 * 그룹 공유 floor 소켓 1개. floor 메시지(RTCP APP "MCPT")는 TS 24.380 User ID 가
 * in-band 신원이라 그룹 공유 포트로 충분하다. 멤버별 audio/video RTP 는
 * PPttMemberPort(멤버 전용 포트 유닛)가 담당한다 — ue_nat_traversal.md §3.2.
 */
class PRtpMulticast : public PHandler
{
public:
    PRtpMulticast(const std::string& name);
    virtual ~PRtpMulticast();

    // ── Lifecycle ──
    bool init(const std::string& ip, unsigned int floorPort);
    bool final();
    void reset();

    // ── 포트 조회 ──
    unsigned int getLocalFloorPort() const { return _localFloorPort; }

    // ── PMcpttGroup 연동 ──
    void setGroup(PMcpttGroup* g);
    PMcpttGroup* getGroup() const { return _group; }

    // ── 전송 (McpttGroup에서 호출) ──
    void sendFloorTo(const std::string& ip, int port, char* data, int len);

    // ── 식별 ──
    void setSessionId(const std::string& id) { _sessionId = id; }
    std::string getSessionId() const { return _sessionId; }
    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }

    // ── 활성도 (멤버 유닛 RTP 수신도 그룹 활성으로 계상 — PMcpttGroup 이 호출) ──
    void touchActivity() { time(&_lastActivityTime); }
    time_t getLastActivityTime() const { return _lastActivityTime; }

    // ── Worker 메인 루프 ──
    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

    // epoll 리액터 등록용: floor 소켓 fd. 소켓은 프로세스 내내 유지.
    void collectFds(std::vector<int>& out) const;

private:
    PRtpSocket  _floorSock;

    PMutex      _mutex;
    PMcpttGroup* _group = nullptr;
    std::string _sessionId;
    std::string _workerName;
    time_t      _lastActivityTime = 0;
    unsigned int _localFloorPort = 0;
};

#endif // __PRTP_MULTICAST_H__
