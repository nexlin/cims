// libcimsue 단위시험 — 통화 중 영상 전환의 SDP 조각 (S1-UE-UNIT, ue_sdk.md §4.5)
// 노리는 것: 영상 추가 offer 판정(포트 0 = 거절·제거된 줄), 거절 answer 가 음성 절을 건드리는 것, 거절된 영상 절에 a=crypto 등이
// 남아 중계가 키를 잡는 것, 491 재전송 대기가 RFC 3261 §14.1 범위를 벗어나는 것.
#include <gtest/gtest.h>

#include "../src/call_video.h"

using namespace cimsue::detail;

namespace {
const char* kAnswer =
    "v=0\r\n"
    "o=- 3912345678 3912345679 IN IP4 192.168.0.92\r\n"
    "s=pjmedia\r\n"
    "c=IN IP4 192.168.0.92\r\n"
    "t=0 0\r\n"
    "m=audio 4000 RTP/SAVP 99 101\r\n"
    "a=rtpmap:99 AMR-WB/16000\r\n"
    "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:AAAA\r\n"
    "a=sendrecv\r\n"
    "m=video 4002 RTP/SAVP 97\r\n"
    "b=TIAS:256000\r\n"
    "a=rtpmap:97 H264/90000\r\n"
    "a=fmtp:97 profile-level-id=42e01f;packetization-mode=1\r\n"
    "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:BBBB\r\n"
    "a=sendrecv\r\n";
}  // namespace

TEST(CallVideo, VideoPortOfOffer) {
    EXPECT_EQ(sdpVideoPort(kAnswer), 4002);
    EXPECT_EQ(sdpVideoPort("v=0\r\nm=audio 4000 RTP/AVP 0\r\n"), -1);                // 영상 줄 없음
    EXPECT_EQ(sdpVideoPort("v=0\nm=audio 4000 RTP/AVP 0\nm=video 0 RTP/AVP 97\n"), 0);  // 거절·제거된 줄(\n 줄 끝)
    EXPECT_EQ(sdpVideoPort("m=video 5000/2 RTP/AVP 97\r\n"), 5000);                  // 포트 개수 표기
}

TEST(CallVideo, RejectKeepsAudioAndZeroesVideo) {
    const std::string r = rejectVideoSdp(kAnswer);
    EXPECT_NE(r.find("m=audio 4000 RTP/SAVP 99 101\r\n"), std::string::npos);
    EXPECT_NE(r.find("inline:AAAA"), std::string::npos);                             // 음성 키는 그대로
    EXPECT_NE(r.find("m=video 0 RTP/SAVP 97\r\n"), std::string::npos);                // 형식 목록은 남는다(RFC 3264 §6)
    EXPECT_EQ(r.find("inline:BBBB"), std::string::npos);                             // 거절된 절의 a=·b= 는 뺀다
    EXPECT_EQ(r.find("TIAS"), std::string::npos);
    EXPECT_EQ(r.find("H264"), std::string::npos);
    EXPECT_EQ(sdpVideoPort(r), 0);
    EXPECT_EQ(r.substr(r.size() - 2), "\r\n");
}

TEST(CallVideo, RejectWithoutVideoIsIdentity) {
    const std::string a = "v=0\r\nc=IN IP4 1.2.3.4\r\nm=audio 4000 RTP/AVP 0\r\na=sendrecv\r\n";
    EXPECT_EQ(rejectVideoSdp(a), a);
}

TEST(CallVideo, RetryDelayFollowsRfc3261Section14_1) {
    EXPECT_EQ(reinviteRetryDelayMs(true, 0.0), 2100u);
    EXPECT_EQ(reinviteRetryDelayMs(true, 0.999999), 4000u);
    EXPECT_EQ(reinviteRetryDelayMs(false, 0.0), 0u);
    EXPECT_EQ(reinviteRetryDelayMs(false, 0.999999), 2000u);
    for (double r = 0; r < 1; r += 0.013) {
        const unsigned o = reinviteRetryDelayMs(true, r), n = reinviteRetryDelayMs(false, r);
        EXPECT_TRUE(o >= 2100 && o <= 4000 && o % 10 == 0) << o;
        EXPECT_TRUE(n <= 2000 && n % 10 == 0) << n;
    }
}
