// libcimsue 내부 — 통화 중 영상 전환 SDP 조각 (call_video.h).
#include "call_video.h"

#include <cstdlib>

namespace cimsue {
namespace detail {

namespace {

/** 한 줄씩 — 줄 끝(\r\n 또는 \n)은 line 에 남긴다. */
template <typename F>
void eachLine(const std::string& s, F f) {
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        const size_t end = b == std::string::npos ? s.size() : b + 1;
        f(s.substr(a, end - a));
        a = end;
    }
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

}  // namespace

int sdpVideoPort(const std::string& sdp) {
    int port = -1;
    eachLine(sdp, [&](const std::string& line) {
        if (port >= 0 || !startsWith(line, "m=video ")) return;
        port = std::atoi(line.c_str() + 8);                               // "m=video <port>[/<n>] <proto> <fmt>…" — atoi 는 '/'·공백에서 멈춘다
    });
    return port;
}

std::string rejectVideoSdp(const std::string& sdp) {
    std::string out;
    out.reserve(sdp.size());
    bool inVideo = false;
    eachLine(sdp, [&](const std::string& line) {
        if (startsWith(line, "m=")) {
            inVideo = startsWith(line, "m=video ");
            if (inVideo) {
                // m=video <port>[/<n>] <proto> <fmt…> → m=video 0 <proto> <fmt…>
                const size_t sp = line.find(' ', 8);
                out += sp == std::string::npos ? line : "m=video 0" + line.substr(sp);
                return;
            }
        } else if (inVideo && (startsWith(line, "a=") || startsWith(line, "b="))) {
            return;
        }
        out += line;
    });
    return out;
}

unsigned reinviteRetryDelayMs(bool callIdOwner, double r) {
    if (r < 0) r = 0;
    if (r >= 1) r = 0.999999;
    const unsigned lo = callIdOwner ? 210 : 0, hi = callIdOwner ? 400 : 200;      // 10 ms 단위
    return (lo + (unsigned)(r * (hi - lo + 1))) * 10;
}

}  // namespace detail
}  // namespace cimsue
