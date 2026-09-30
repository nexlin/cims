// libcimsue 단위시험 — 구동 줄 프로토콜 DriveSession (S1-UE-UNIT, ue_voice_quality.md §5.3)
//   엔진을 띄우지 않고 명령 해석·결과 줄·앱 소유 등록·회선 선택을 검사한다(네트워크 없음).
#include <gtest/gtest.h>

#include <mutex>
#include <vector>

#include "cimsue/drive.h"

using namespace cimsue;

namespace {

struct VecSink : LineSink {
    std::mutex m;
    std::vector<std::string> lines;
    void writeLine(const std::string& l) override { std::lock_guard<std::mutex> lk(m); lines.push_back(l); }
    std::string last() { std::lock_guard<std::mutex> lk(m); return lines.empty() ? "" : lines.back(); }
    bool any(const std::string& needle) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& l : lines) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

DriveOptions twoLines(bool appOwned) {
    DriveOptions o;
    o.accounts.push_back({ "volte", 0, "sip:+821300000001@volte.example", "+821300000001" });
    o.accounts.push_back({ "ptt", 1, "sip:+821300000001@ptt.example", "+821300000001" });
    o.appOwnedRegistration = appOwned;
    o.hangupOnStop = DriveOptions::Hangup::Driven;
    return o;
}

}  // namespace

TEST(Drive, ReadyAndResultLines) {
    Engine eng;
    VecSink sink;
    DriveSession ds(eng, sink, twoLines(false));
    ds.start(true);
    EXPECT_NE(sink.lines.front().find("\"event\":\"ready\""), std::string::npos);
    EXPECT_NE(sink.lines.front().find("sip:+821300000001@volte.example"), std::string::npos);   // 기본 회선 = 첫 항목
    ds.handleLine("dial +821300000002");
    EXPECT_NE(sink.last().find("\"op\":\"dial\",\"ok\":false"), std::string::npos);            // 엔진이 없으면 거절 — 결과 줄은 반드시 하나
    ds.handleLine("bogus");
    EXPECT_NE(sink.last().find("unknown command"), std::string::npos);
    ds.handleLine("   ");                                                                         // 빈 줄은 결과 없음
    EXPECT_NE(sink.last().find("unknown command"), std::string::npos);
    EXPECT_FALSE(ds.quitRequested());
    ds.handleLine("quit");
    EXPECT_TRUE(ds.quitRequested());
    ds.stop();
    ds.stop();                                                                                    // 두 번 불러도 된다
}

TEST(Drive, AppOwnedRegistrationAndUse) {
    Engine eng;
    VecSink sink;
    DriveSession ds(eng, sink, twoLines(true));
    ds.start(false);
    EXPECT_TRUE(sink.lines.empty());                                                              // ready 없음(링크는 hello 가 대신)
    ds.handleLine("register");
    EXPECT_NE(sink.last().find("\"ok\":false"), std::string::npos);
    EXPECT_NE(sink.last().find("app_owned"), std::string::npos);
    ds.handleLine("unregister");
    EXPECT_NE(sink.last().find("app_owned"), std::string::npos);
    ds.handleLine("use ptt");
    EXPECT_NE(sink.last().find("\"op\":\"use\",\"ok\":true"), std::string::npos);
    ds.handleLine("use voip");
    EXPECT_NE(sink.last().find("no_account"), std::string::npos);
    ds.handleLine("media sample");
    EXPECT_NE(sink.last().find("no_sample"), std::string::npos);                                  // 기본 음원이 없으면
    ds.handleLine("media speaker");
    EXPECT_NE(sink.last().find("mic|sample"), std::string::npos);
    ds.handleLine("quality 3");
    EXPECT_NE(sink.last().find("no_quality"), std::string::npos);
    ds.stop();
}

TEST(Drive, FieldHelpers) {
    EXPECT_EQ(drive::jsonEscape("a\"b\\c\nd"), "a\\\"b\\\\c\\nd");
    EXPECT_EQ(drive::qualityFields(CallQuality{}), "");                                           // 무효 품질은 필드 없음
    CallQuality q;
    q.valid = true; q.codec = "AMR-WB"; q.rtdMs = 84; q.mosCq = 4.1;
    std::string f = drive::qualityFields(q);
    EXPECT_NE(f.find("\"codec\":\"AMR-WB\""), std::string::npos);
    EXPECT_NE(f.find("\"rtd_ms\":84.00"), std::string::npos);
    EXPECT_NE(f.find("\"mos_lq\":-1"), std::string::npos);                                      // 없음 = -1
    StreamStats st; st.rxPackets = 5; st.valid = true;
    EXPECT_NE(drive::statsFields(st).find("\"rx_pkts\":5"), std::string::npos);
}

TEST(Drive, LinkRejectsBadConfig) {
    Engine eng;
    DeviceLink link(eng);
    DeviceLinkConfig c;
    EXPECT_FALSE(link.start(c, twoLines(true)).ok);                                               // host 없음
    c.host = "127.0.0.1";
    EXPECT_FALSE(link.start(c, DriveOptions{}).ok);                                               // 회선 없음
    EXPECT_EQ(link.state(), LinkState::Idle);
    link.stop();
}

// MCVideo 명령(TS 24.281 호 · TS 24.581 전송 제어) — 엔진이 없으면 거절하되 명령마다 결과 줄 하나, 알려진 명령으로 해석된다
TEST(Drive, McVideoCommands) {
    Engine eng;
    VecSink sink;
    DriveSession ds(eng, sink, twoLines(false));
    ds.start(false);
    for (const char* cmd : {"video_call g101 prearranged queueing implicit", "transmit_request 0 5", "transmit_release 0",
                            "reception_accept 0 tel:+82510002001", "reception_end 0 tel:+82510002001", "affiliate g101 on mcvideo"}) {
        size_t before = sink.lines.size();
        ds.handleLine(cmd);
        ASSERT_EQ(sink.lines.size(), before + 1) << cmd;
        std::string op(cmd, std::string(cmd).find(' '));
        EXPECT_NE(sink.last().find("\"op\":\"" + op + "\",\"ok\":false"), std::string::npos) << sink.last();
        EXPECT_EQ(sink.last().find("unknown command"), std::string::npos) << sink.last();
    }
    ds.stop();
}
