// cmp_floor_codec_test.cpp — CMP floor 코덱(PFloorCodec.cpp)이 단말(android/ptt-client
// floor/FloorCodec.kt + FloorCodecTest.kt)과 **바이트 호환**임을 검증하는 독립 테스트.
//
// 빌드: g++ -std=c++17 -I../cmp tests/cmp_floor_codec_test.cpp ../cmp/PFloorCodec.cpp -o /tmp/floortest
// (PFloorCodec.cpp 는 외부 의존이 없어 단독 링크 가능)
//
// 검증 항목은 단말 FloorCodecTest 와 1:1 대응한다:
//   requestRoundTrip / releaseRoundTrip / grantedWithDuration / denyCause /
//   stringFieldPaddedTo4Bytes / rejectsNonMcpt / ackHasNoFields / nameBytesExact

#include "PMcpttGroup.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// 단말 FloorCodec.request(ssrc, userId, priority) 와 동일한 바이트열 합성.
static int buildRequest(char* buf, int sz, unsigned int ssrc, const std::string& userId, int prio) {
    std::vector<FloorTlv> f{ FloorTlv(FF_PRIORITY, FloorPriority(prio)),
                             FloorTlv(FF_USER_ID, userId) };
    return BuildFloorMessage(buf, sz, FLOOR_REQUEST, ssrc, f);
}

int main() {
    char buf[512];

    // 1) Request roundtrip + RTCP APP 헤더 불변식 (FloorCodecTest.requestRoundTrip)
    {
        int n = buildRequest(buf, sizeof(buf), 0x01020304u, "tel:+82571900001", 3);
        CHECK(n > 0, "request encodes");
        CHECK(((unsigned char)buf[0] & 0xE0) == 0x80, "V=2,P=0");
        CHECK((buf[0] & 0x1F) == FLOOR_REQUEST, "subtype=REQUEST(0)");
        CHECK((unsigned char)buf[1] == 204, "PT=204");
        CHECK(memcmp(buf + 8, "MCPT", 4) == 0, "name=MCPT");
        CHECK(n % 4 == 0, "32-bit aligned");

        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "request decodes");
        CHECK(m.subtype == FLOOR_REQUEST, "decoded subtype");
        CHECK(m.ssrc == 0x01020304u, "decoded ssrc");
        CHECK(m.userId() == "tel:+82571900001", "decoded userId");
        CHECK(m.priority() == 3, "decoded priority");
    }

    // 2) Release roundtrip (FloorCodecTest.releaseRoundTrip)
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_USER_ID, "tel:+82571900002") };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_RELEASE, 7u, f);
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "release decodes");
        CHECK(m.subtype == FLOOR_RELEASE, "release subtype");
        CHECK(m.ssrc == 7u, "release ssrc");
        CHECK(m.userId() == "tel:+82571900002", "release userId");
    }

    // 3) Granted with Duration + Granted Party (FloorCodecTest.grantedWithDurationDecodes)
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_DURATION, FloorU16(30)),
                                 FloorTlv(FF_GRANTED_PARTY, "tel:+82571900001") };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_GRANT, 1000u, f);
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "granted decodes");
        CHECK(m.subtype == FLOOR_GRANT, "granted subtype=1");
        CHECK(m.u16(FF_DURATION) == 30, "duration=30");
        CHECK(m.str(FF_GRANTED_PARTY) == "tel:+82571900001", "granted party");
    }

    // 4) Deny cause (FloorCodecTest.denyCauseDecodes)
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_REJECT_CAUSE, FloorU16(1)) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_REJECT, 1u, f);
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "deny decodes");
        CHECK(m.subtype == FLOOR_REJECT, "deny subtype=3");
        CHECK(m.u16(FF_REJECT_CAUSE) == 1, "reject cause=1");
    }

    // 5) String field padded to 4B — userId len 5 다음 priority 가 안 깨져야 함
    //    (FloorCodecTest.stringFieldPaddedTo4Bytes)
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_USER_ID, "abcde"),
                                 FloorTlv(FF_PRIORITY, FloorPriority(2)) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_REQUEST, 1u, f);
        CHECK(n % 4 == 0, "padded packet 32-bit aligned");
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "padded decodes");
        CHECK(m.userId() == "abcde", "padded userId");
        CHECK(m.priority() == 2, "priority after padded string");
    }

    // 6) Non-MCPT / 너무 짧은 패킷 거부 (FloorCodecTest.rejectsNonMcptPacket)
    {
        char bogus[16]; memset(bogus, 0, sizeof(bogus));
        bogus[0] = (char)0x80; bogus[1] = (char)200;   // PT=200, name=0
        ParsedFloor m;
        CHECK(!ParseFloorMessage(bogus, sizeof(bogus), m), "rejects PT!=204");
        char tiny[4] = {0,0,0,0};
        CHECK(!ParseFloorMessage(tiny, sizeof(tiny), m), "rejects too short");
    }

    // 7) Ack has no fields (FloorCodecTest.ackHasNoFields)
    {
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_ACK, 9u, {});
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "ack decodes");
        CHECK(m.subtype == FLOOR_ACK, "ack subtype=10");
        CHECK(m.ssrc == 9u, "ack ssrc");
        CHECK(m.fields.empty(), "ack has no fields");
    }

    // 8) name 바이트 정확 (FloorCodecTest.encodedNameBytesExact)
    {
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_ACK, 1u, {});
        CHECK(n >= 12 && buf[8]=='M' && buf[9]=='C' && buf[10]=='P' && buf[11]=='T', "name bytes MCPT");
    }

    // 9) Queue Position Info — position/size 인코딩
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_QUEUE_INFO, FloorQueueInfo(2, 5)),
                                 FloorTlv(FF_QUEUE_SIZE, FloorU16(4)) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_QUEUE_POS_INFO, 1u, f);
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "queue-pos decodes");
        CHECK(m.subtype == FLOOR_QUEUE_POS_INFO, "queue subtype=9");
        const FloorTlv* qi = m.field(FF_QUEUE_INFO);
        CHECK(qi && (unsigned char)qi->value[0] == 2, "queue position=2");
        CHECK(m.u16(FF_QUEUE_SIZE) == 4, "queue size=4");
    }

    printf("\ncmp_floor_codec_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
