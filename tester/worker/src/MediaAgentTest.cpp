// 미디어 전담 워커 단위시험 — 같은 프로세스에 에이전트(MediaAgent + HttpServer)와 클라이언트(MediaAgentClient) 를 두고 CRtpThread 원격 모드로
//   할당 → Start → 로컬 CRtpThread 와 양방향 RTP → RemoteSync 통계 → 송출 정지 제어 → Stop/Release 를 확인한다. S1-UNIT-TESTER 의 네이티브 항목.
#include <chrono>
#include <cstdio>
#include <thread>

#include "HttpServer.h"
#include "Json.h"
#include "MediaAgent.h"
#include "RtpThread.h"

int main() {
    bool all = true;
    auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); all = all && ok; };
    MediaAgent agent("127.0.0.1", "", "");
    HttpServer http;
    std::string err;
    if (!http.start("127.0.0.1", 0, [&](const HttpRequest& r) {
            Json body; std::string e;
            if (!r.body.empty()) Json::parse(r.body, body, e);
            if (r.path == "/health") { Json j = Json::Object(); Json m = Json::Object(); m["agent_streams"] = Json(agent.streams()); j["media"] = m; HttpResponse h; h.body = j.dump(); return h; }
            return agent.handle(r, body);
        }, err)) { printf("http start failed: %s\n", err.c_str()); return 2; }
    MediaAgentClient client("http://127.0.0.1:" + std::to_string(http.port()));
    std::string perr;
    check(client.probe(perr), "probe agent health");

    CRtpThread remote;                 // 시그널링 워커 쪽 객체 — 소켓은 에이전트에
    remote.SetRemote(&client);
    check(remote.Create(), "remote Create (allocate)");
    check(remote.IsRemote() && remote.m_iPort > 0 && remote.m_strMediaIp == "127.0.0.1", "allocated port and media ip");
    check(agent.streams() == 1, "agent holds one stream");
    CRtpThread local;                  // 상대(로컬 소켓)
    check(local.Create(), "local Create");
    remote.m_iAudioPt = 0; local.m_iAudioPt = 0;
    check(remote.Start("127.0.0.1", local.m_iPort), "remote Start");
    check(local.Start("127.0.0.1", remote.m_iPort), "local Start → agent port");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    check(remote.RemoteSync(), "RemoteSync");
    printf("     remote tx=%llu rx=%llu lost=%llu · local rx=%llu\n", remote.m_ullSentTotal.load(), remote.m_ullRecvTotal.load(), remote.m_ullRecvLost.load(), local.m_ullRecvTotal.load());
    check(remote.m_ullSentTotal.load() >= 20 && remote.m_ullRecvTotal.load() >= 20 && remote.m_ullRecvLost.load() == 0, "RTP flows both ways through the agent");
    check(local.m_ullRecvTotal.load() >= 20 && local.m_iRecvPt.load() == 0, "local peer receives agent stream (PT 0)");
    unsigned long long before = local.m_ullRecvTotal.load();
    remote.MediaStop();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    unsigned long long mid = local.m_ullRecvTotal.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(local.m_ullRecvTotal.load() - mid <= 2 && mid >= before, "MediaStop control pauses agent send");
    remote.MediaSendDefault();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    check(local.m_ullRecvTotal.load() > mid + 5, "MediaSendDefault control resumes");
    check(remote.Stop(), "remote Stop");
    remote.Destroy();
    check(agent.streams() == 0, "Release frees the agent stream");
    local.Stop();
    http.stop();
    printf(all ? "PASS\n" : "FAIL\n");
    return all ? 0 : 1;
}
