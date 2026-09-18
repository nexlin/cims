// MediaAgent — 미디어 전담 워커(test_instrument.md §4 미디어 평면 후속). 한 워커는 두 얼굴을 가진다:
//   · 에이전트(서버) — 다른 워커의 UE 풀이 위임한 RTP 스트림을 자기 호스트의 소켓·CRtpThread 로 굴린다(`/media/*` 제어 API). 시그널링은 그쪽 워커에 남고
//     SDP c=/m= 만 이 호스트를 가리킨다 — RTP CPU 를 시그널링 워커에서 떼어 낸다.
//   · 클라이언트(MediaAgentClient : IRtpRemote) — UE 풀 `media_worker` 가 가리킨 에이전트에 CRtpThread 의 Create/Start/Stop/송출 제어/DTMF/통계를 HTTP 로 위임한다.
//   프로토콜(JSON): POST /media/alloc {video} → {id, ip, port, video_port} · POST /media/<id>/start {…RtpRemoteStart} · POST /media/<id>/stop ·
//   POST /media/<id>/ctl {op, a,b,c,d, loop} · GET /media/<id>/stats → RtpRemoteStats · DELETE /media/<id>. 요청은 짧고 호마다 몇 번(연결마다 하나).
#ifndef _CIMS_TESTER_MEDIA_AGENT_H_
#define _CIMS_TESTER_MEDIA_AGENT_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "HttpServer.h"
#include "Json.h"
#include "RtpRemote.h"
#include "RtpThread.h"

class MediaAgent {
public:
    MediaAgent(const std::string& localIp, const std::string& mediaFile, const std::string& videoFile) : m_localIp(localIp), m_mediaFile(mediaFile), m_videoFile(videoFile) {}
    /** /media/* 요청 처리 — path 는 "/media/..." 전체 */
    HttpResponse handle(const HttpRequest& req, const Json& body);
    long long streams();           // 할당된 스트림 수(health media.agent_streams)
    long long running();           // 송수신 중인 스트림 수
    void stopAll();

private:
    std::string m_localIp, m_mediaFile, m_videoFile;
    std::mutex m_mtx;
    std::map<std::string, std::unique_ptr<CRtpThread>> m_streams;
    long long m_seq = 0;
};

/** IRtpRemote 의 HTTP 구현 — 에이전트 URL(http://ip:port) 하나. 호출은 psip 스택 스레드(EventCallStart)에서도 오므로 짧은 시한(기본 3 s)으로 막는다. */
class MediaAgentClient : public IRtpRemote {
public:
    explicit MediaAgentClient(const std::string& url, int timeoutMs = 3000);
    const std::string& url() const { return m_url; }
    bool Allocate(bool bVideo, std::string& id, std::string& ip, int& port, int& videoPort) override;
    bool Release(const std::string& id) override;
    bool Start(const std::string& id, const RtpRemoteStart& s) override;
    bool Stop(const std::string& id) override;
    bool Control(const std::string& id, const std::string& op, const std::string& a, const std::string& b, const std::string& c,
                 const std::string& d, bool bLoop) override;
    bool Stats(const std::string& id, RtpRemoteStats& out) override;
    /** 에이전트 health(GET /health) — 도달·media.agent 확인 */
    bool probe(std::string& err);

private:
    std::string m_url, m_host;
    int m_port = 7100;
    int m_timeoutMs;
    bool request(const std::string& method, const std::string& path, const Json* body, Json& out, int& status);
};

#endif
