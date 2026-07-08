// cmdp MSRP 프레이머(RFC 4975 부분집합) 단위 시험
// 빌드: g++ -std=c++17 -Icmdp tests/cmdp_msrp_parser_test.cpp cmdp/PMsrpParser.cpp -o /tmp/msrptest
#include "PMsrpParser.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) { g_pass++; }                                                  \
        else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static const char* kToPath = "msrp://192.168.1.20:2855/md_1;tcp";
static const char* kFromPath = "msrp://10.0.0.5:49152/abc;tcp";

static std::string makeSend(const std::string& tid, const std::string& body,
                            long long from, long long to, long long total, char flag,
                            bool successReport = false) {
    return MsrpBuildSendChunk(tid, kToPath, kFromPath, "msg1",
                              "application/vnd.3gpp.mcdata-payload", body,
                              from, to, total, successReport, flag);
}

// 1) 단일 청크 SEND 파싱
static void testSingleChunk() {
    PMsrpParser p;
    std::string body = "hello mcdata";
    std::string frame = makeSend("tid1", body, 1, body.size(), body.size(), '$', true);
    p.feed(frame.data(), frame.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.kind == PMsrpMessage::MSRP_REQUEST);
    CHECK(m.method == "SEND");
    CHECK(m.transId == "tid1");
    CHECK(m.contFlag == '$');
    CHECK(m.body == body);
    CHECK(m.GetHeader("Message-ID") == "msg1");
    CHECK(m.GetHeader("message-id") == "msg1");  // 대소문자 무시
    CHECK(m.GetHeader("Success-Report") == "yes");
    PMsrpByteRange r = MsrpParseByteRange(m.GetHeader("Byte-Range"));
    CHECK(r.valid && r.start == 1 && r.end == (long long)body.size() &&
          r.total == (long long)body.size());
    CHECK(!p.next(m));  // 더 없음
    CHECK(!p.hasError());
}

// 2) 멀티 청크(+, $) 파싱
static void testMultiChunk() {
    PMsrpParser p;
    std::string c1 = makeSend("t2a", "AAAA", 1, 4, 8, '+');
    std::string c2 = makeSend("t2b", "BBBB", 5, 8, 8, '$');
    std::string all = c1 + c2;
    p.feed(all.data(), all.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.contFlag == '+' && m.body == "AAAA");
    CHECK(p.next(m));
    CHECK(m.contFlag == '$' && m.body == "BBBB");
    PMsrpByteRange r = MsrpParseByteRange(m.GetHeader("Byte-Range"));
    CHECK(r.valid && r.start == 5 && r.end == 8 && r.total == 8);
}

// 3) 스트리밍형 Byte-Range "1-*/*"
static void testByteRangeStar() {
    PMsrpByteRange r = MsrpParseByteRange("1-*/*");
    CHECK(r.valid && r.start == 1 && r.endStar && r.totalStar);
    r = MsrpParseByteRange("10-20/*");
    CHECK(r.valid && r.start == 10 && r.end == 20 && r.totalStar);
    r = MsrpParseByteRange("garbage");
    CHECK(!r.valid);
    r = MsrpParseByteRange("5-3/10");  // end < start
    CHECK(!r.valid);
}

// 4) abort 플래그 '#'
static void testAbort() {
    PMsrpParser p;
    std::string frame = makeSend("t4", "partial", 1, 7, 100, '#');
    p.feed(frame.data(), frame.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.contFlag == '#');
}

// 5) 임의 경계 분할 feed — 모든 절단점에서 동일 결과
static void testSplitFeeds() {
    std::string body = "split-feed-body-0123456789";
    std::string frame = makeSend("t5", body, 1, body.size(), body.size(), '$');
    for (size_t cut = 1; cut < frame.size(); ++cut) {
        PMsrpParser p;
        p.feed(frame.data(), cut);
        PMsrpMessage m;
        bool early = p.next(m);
        p.feed(frame.data() + cut, frame.size() - cut);
        if (!early) CHECK(p.next(m));
        CHECK(m.body == body);
        CHECK(m.transId == "t5");
        CHECK(!p.hasError());
    }
}

// 6) 본문에 다른 trans-id 의 가짜 end-line 열 포함 — 종료 오인 금지
static void testFakeEndLineInBody() {
    PMsrpParser p;
    std::string body = "xx\r\n-------othertid$\r\nyy";  // 다른 tid — 무시돼야 함
    std::string frame = makeSend("realtid", body, 1, body.size(), body.size(), '$');
    p.feed(frame.data(), frame.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.body == body);
    // 같은 tid 라도 플래그/CRLF 형식이 안 맞으면 통과
    PMsrpParser p2;
    std::string body2 = "xx\r\n-------t6Xyy";  // 플래그 자리가 'X' — 무효
    std::string frame2 = makeSend("t6", body2, 1, body2.size(), body2.size(), '$');
    p2.feed(frame2.data(), frame2.size());
    CHECK(p2.next(m));
    CHECK(m.body == body2);
}

// 7) 바이너리(NUL 포함) 본문
static void testBinaryBody() {
    PMsrpParser p;
    std::string body("\x01\x00\xff\x00 binary", 15);
    std::string frame = makeSend("t7", body, 1, body.size(), body.size(), '$');
    p.feed(frame.data(), frame.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.body.size() == body.size());
    CHECK(memcmp(m.body.data(), body.data(), body.size()) == 0);
}

// 8) 응답 파싱 + 빌더 바이트 정확성
static void testResponse() {
    std::string resp = MsrpBuildResponse("t8", 200, "OK", kFromPath, kToPath);
    std::string expected = std::string("MSRP t8 200 OK\r\n") + "To-Path: " + kFromPath +
                           "\r\nFrom-Path: " + kToPath + "\r\n-------t8$\r\n";
    CHECK(resp == expected);
    PMsrpParser p;
    p.feed(resp.data(), resp.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.kind == PMsrpMessage::MSRP_RESPONSE);
    CHECK(m.statusCode == 200 && m.reason == "OK");
    CHECK(m.body.empty());
    // reason 없는 응답
    std::string r2 = MsrpBuildResponse("t8b", 413, "", kFromPath, kToPath);
    PMsrpParser p2;
    p2.feed(r2.data(), r2.size());
    CHECK(p2.next(m));
    CHECK(m.statusCode == 413 && m.reason.empty());
}

// 9) REPORT 빌더/파싱 (본문 없는 요청 — blank line 없이 end-line 직행)
static void testReport() {
    std::string rep = MsrpBuildReport("t9", kFromPath, kToPath, "msg1", 1, 25, 25, 200, "OK");
    PMsrpParser p;
    p.feed(rep.data(), rep.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.kind == PMsrpMessage::MSRP_REQUEST && m.method == "REPORT");
    CHECK(m.GetHeader("Status") == "000 200 OK");
    CHECK(m.body.empty());
}

// 10) 본문 없는 SEND (bodiless — contentType 빈값)
static void testBodilessSend() {
    std::string s = MsrpBuildSendChunk("t10", kToPath, kFromPath, "m10", "", "", 1, 0, 0, false, '$');
    CHECK(s.find("\r\n\r\n") == std::string::npos);  // blank line 없음
    PMsrpParser p;
    p.feed(s.data(), s.size());
    PMsrpMessage m;
    CHECK(p.next(m));
    CHECK(m.method == "SEND" && m.body.empty());
}

// 11) 프레이밍 오류 — MSRP 아닌 스트림
static void testGarbage() {
    PMsrpParser p;
    const char* junk = "GET / HTTP/1.1\r\n\r\n";
    p.feed(junk, strlen(junk));
    PMsrpMessage m;
    CHECK(!p.next(m));
    CHECK(p.hasError());
}

// 12) URI 파서
static void testUri() {
    std::string host, sess;
    int port = 0;
    CHECK(MsrpParseUri("msrp://192.168.1.20:2855/md_17;tcp", host, port, sess));
    CHECK(host == "192.168.1.20" && port == 2855 && sess == "md_17");
    CHECK(!MsrpParseUri("http://x/y", host, port, sess));
    CHECK(!MsrpParseUri("msrp://hostonly", host, port, sess));
}

// 13) 파이프라인 — 한 feed 에 두 프레임 + 세 번째는 절반
static void testPipelined() {
    std::string f1 = makeSend("p1", "one", 1, 3, 3, '$');
    std::string f2 = makeSend("p2", "two", 1, 3, 3, '$');
    std::string f3 = makeSend("p3", "three", 1, 5, 5, '$');
    std::string stream = f1 + f2 + f3.substr(0, 10);
    PMsrpParser p;
    p.feed(stream.data(), stream.size());
    PMsrpMessage m;
    CHECK(p.next(m) && m.body == "one");
    CHECK(p.next(m) && m.body == "two");
    CHECK(!p.next(m));
    p.feed(f3.data() + 10, f3.size() - 10);
    CHECK(p.next(m) && m.body == "three");
}

int main() {
    testSingleChunk();
    testMultiChunk();
    testByteRangeStar();
    testAbort();
    testSplitFeeds();
    testFakeEndLineInBody();
    testBinaryBody();
    testResponse();
    testReport();
    testBodilessSend();
    testGarbage();
    testUri();
    testPipelined();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
