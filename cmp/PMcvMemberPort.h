#ifndef __PMCV_MEMBER_PORT_H__
#define __PMCV_MEMBER_PORT_H__

#include <string>
#include <vector>
#include <memory>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"

class PMcvideoGroup;

// MCVideo 멤버 유닛의 수신 채널 (cmp_media_api.md §7.9).
enum McvChannel {
    MCV_CH_AUDIO = 0,       // audio RTP          (port)
    MCV_CH_VIDEO,           // video RTP          (video_port)
    MCV_CH_VIDEO_RTCP,      // video RTCP         (video_port + 1 — 수신자 PLI·FIR, RFC 4585·5104)
    MCV_CH_CONTROL          // 전송 제어 채널      (control_port — RTCP APP MCV0/1/2, TS 24.581 §4.3.3.1)
};

/**
 * MCVideo 멤버 전용 포트 유닛 — leg 별 포트셋 (ue_nat_traversal.md §3.2, cmp.md §3.6)
 *
 * 멤버마다 6포트 블록 하나(McVideoStartPort + N*6):
 *   +0 audio RTP · +1 audio RTCP(예약) · +2 video RTP · +3 video RTCP · +4 전송 제어 · +5 예약.
 * MCVideo 에는 그룹 공유 포트가 없다 — 전송 제어도 멤버 포트라 수신 소켓이 곧 멤버 신원이고, 하향 송신도
 * 이 소켓에서 나간다(symmetric RTP 정합). 소켓은 기동 시 열려 epoll 에 영구 등록되고 유닛은 풀에서 재사용된다.
 *
 * 그룹은 shared_ptr 로 잡는다 — 리액터가 수신 콜백을 부르는 사이 PTT_GROUP_REMOVE 가 그룹을 지워도 콜백이 끝날
 * 때까지 객체가 산다(그룹은 해제 때 멤버를 먼저 비우므로 늦은 패킷은 미등록 멤버로 버려진다).
 */
class PMcvMemberPort : public PHandler
{
public:
    static const int kStride = 6;   // 멤버당 포트 블록 크기

    PMcvMemberPort(const std::string& name);
    virtual ~PMcvMemberPort();

    // ── Lifecycle ──
    bool init(const std::string& ip, unsigned int basePort);
    bool final();
    void bind(const std::shared_ptr<PMcvideoGroup>& g, const std::string& memberId);
    void reset();

    // ── 포트 조회 ──
    unsigned int getAudioPort() const { return _basePort; }
    unsigned int getVideoPort() const { return _basePort + 2; }
    unsigned int getVideoRtcpPort() const { return _basePort + 3; }
    unsigned int getControlPort() const { return _basePort + 4; }

    // ── 전송 (그룹 하향 분배에서 호출) ──
    void sendTo(McvChannel ch, const std::string& ip, int port, const char* data, int len);

    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }

    // ── Worker 메인 루프 ──
    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

    void collectFds(std::vector<int>& out) const;

private:
    PRtpSocket* _sock(McvChannel ch);

    PRtpSocket  _audioSock;
    PRtpSocket  _videoSock;
    PRtpSocket  _videoRtcpSock;
    PRtpSocket  _controlSock;

    PMutex      _mutex;
    std::shared_ptr<PMcvideoGroup> _group;
    std::string _memberId;
    std::string _workerName;
    unsigned int _basePort = 0;
};

#endif // __PMCV_MEMBER_PORT_H__
