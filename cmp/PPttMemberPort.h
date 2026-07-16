#ifndef __PPTT_MEMBER_PORT_H__
#define __PPTT_MEMBER_PORT_H__

#include <string>
#include <vector>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"

class PMcpttGroup;

/**
 * PTT 멤버 전용 RTP 포트 유닛 — leg 별 포트셋 (ue_nat_traversal.md §3.2)
 *
 * audio RTP + video RTP 소켓 각 1개. 멤버 참가 시 그룹·멤버에 바인딩되고,
 * 이 유닛의 포트가 그 멤버의 SDP 에 광고된다 — 수신 소켓이 곧 멤버 신원이며
 * 하향(청취) 송신도 이 소켓에서 나간다(멤버가 보는 소스 포트 = 광고 포트,
 * symmetric RTP 정합). 소켓은 프로세스 기동 시 열려 epoll 에 영구 등록되고,
 * 유닛은 풀에서 alloc/free 로 재사용된다.
 */
class PPttMemberPort : public PHandler
{
public:
    PPttMemberPort(const std::string& name);
    virtual ~PPttMemberPort();

    // ── Lifecycle ──
    bool init(const std::string& ip, unsigned int audioPort, unsigned int videoPort);
    bool final();
    // 그룹·멤버 바인딩 (alloc 시) / 해제 (free 시)
    void bind(PMcpttGroup* g, const std::string& memberId);
    void reset();

    // ── 포트 조회 ──
    unsigned int getAudioPort() const { return _localAudioPort; }
    unsigned int getVideoPort() const { return _localVideoPort; }

    // ── 전송 (McpttGroup 하향 분배에서 호출) ──
    void sendAudioTo(const std::string& ip, int port, char* data, int len);
    void sendVideoTo(const std::string& ip, int port, char* data, int len);

    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }

    // ── Worker 메인 루프 ──
    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

    void collectFds(std::vector<int>& out) const;

private:
    PRtpSocket  _audioSock;
    PRtpSocket  _videoSock;

    PMutex      _mutex;
    PMcpttGroup* _group = nullptr;
    std::string _memberId;
    std::string _workerName;
    unsigned int _localAudioPort = 0;
    unsigned int _localVideoPort = 0;
};

#endif // __PPTT_MEMBER_PORT_H__
