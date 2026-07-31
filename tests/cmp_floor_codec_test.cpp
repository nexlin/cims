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

    // 10) 모든 필드가 4옥텟 배수(§8.1.3) — 값 길이가 2 가 아닌 필드도 패딩되고, 그 뒤 필드가
    //     정확히 이어져 읽힌다. (구 규칙: 문자열 4종만 패딩 → 아래 SSRC(6옥텟) 다음 필드가 밀림)
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_SSRC, FloorSsrc(0xDEADBEEFu)),
                                 FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0x8080)),
                                 FloorTlv(FF_DURATION, FloorU16(30)) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_TAKEN, 5u, f);
        CHECK(n % 4 == 0, "aligned packet");
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "ssrc-field msg decodes");
        const FloorTlv* s = m.field(FF_SSRC);
        CHECK(s && s->value.size() == 6, "SSRC field length=6 (§8.2.3.16)");
        CHECK(s && (unsigned char)s->value[0] == 0xDE && (unsigned char)s->value[3] == 0xEF, "SSRC value");
        CHECK(m.u16(FF_FLOOR_INDICATOR) == 0x8080, "indicator after 6-octet field");
        CHECK(m.u16(FF_DURATION) == 30, "duration after 6-octet field");
    }

    // 11) 3옥텟 값(패딩 1옥텟) 뒤 필드도 어긋나지 않는다 — 규격의 미지/가변 필드 대비.
    {
        std::vector<FloorTlv> f{ FloorTlv(19 /* Location(미구현 필드) */, std::string("\x01\x02\x03", 3)),
                                 FloorTlv(FF_DURATION, FloorU16(7)) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_TAKEN, 1u, f);
        CHECK(n % 4 == 0, "aligned with 3-octet field");
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "3-octet field msg decodes");
        CHECK(m.field(19) && m.field(19)->value.size() == 3, "unknown field preserved");
        CHECK(m.u16(FF_DURATION) == 7, "field after padded unknown field");
    }

    // 12) multi-talker 리스트 필드 (§8.2.3.17/§8.2.3.18)
    {
        std::vector<std::string> users{ "tel:+82571900001", "tel:+8257190002" };
        std::vector<unsigned int> ssrcs{ 0x11111111u, 0x22222222u };
        std::vector<FloorTlv> f{ FloorTlv(FF_FLOOR_INDICATOR, FloorU16(FI_NORMAL | FI_MULTI_TALKER)),
                                 FloorTlv(FF_GRANTED_USERS, FloorUserList(users)),
                                 FloorTlv(FF_SSRC_LIST, FloorSsrcList(ssrcs)) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_TAKEN, 3u, f);
        CHECK(n % 4 == 0, "aligned multi-talker taken");
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "multi-talker taken decodes");
        const FloorTlv* gu = m.field(FF_GRANTED_USERS);
        CHECK(gu && (unsigned char)gu->value[0] == 2, "granted users count=2");
        CHECK(gu && (unsigned char)gu->value[1] == users[0].size(), "first user id length");
        CHECK(gu && gu->value.compare(2, users[0].size(), users[0]) == 0, "first user id");
        const FloorTlv* sl = m.field(FF_SSRC_LIST);
        CHECK(sl && (unsigned char)sl->value[0] == 2, "ssrc list count=2");
        CHECK(sl && sl->value.size() == 2 + 8, "ssrc list length");
        CHECK(sl && (unsigned char)sl->value[2] == 0x11 && (unsigned char)sl->value[6] == 0x22, "ssrc list values");
        CHECK(m.u16(FF_FLOOR_INDICATOR) == (FI_NORMAL | FI_MULTI_TALKER), "multi-talker bit");
    }

    // 13) Ack 요구 변종 — subtype 첫 비트(0x10)를 걷어내면 기본 타입이다 (§8.2.2)
    {
        std::vector<FloorTlv> f{ FloorTlv(FF_USER_ID, "tel:+82571900003") };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_RELEASE | FLOOR_ACK_REQ_BIT, 8u, f);
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "ack-req release decodes");
        CHECK(m.subtype == (FLOOR_RELEASE | FLOOR_ACK_REQ_BIT), "subtype keeps ack bit");
        CHECK((m.subtype & FLOOR_ACK_REQ_BIT) != 0, "ack required flagged");
        CHECK(FLOOR_OP(m.subtype) == FLOOR_RELEASE, "base op = RELEASE");
        CHECK(m.userId() == "tel:+82571900003", "ack-req release userId");
    }

    // 14) Floor Ack 본문 (§8.2.13) — Source + Message Type(확인 대상 subtype, ack 비트 포함)
    {
        std::string mt(2, '\0');
        mt[0] = (char)(FLOOR_RELEASE | FLOOR_ACK_REQ_BIT);
        std::vector<FloorTlv> f{ FloorTlv(FF_SOURCE, FloorU16(FLOOR_SRC_CONTROLLING)),
                                 FloorTlv(FF_MSG_TYPE, mt) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_ACK, 2u, f);
        ParsedFloor m;
        CHECK(ParseFloorMessage(buf, n, m), "floor ack decodes");
        CHECK(m.u16(FF_SOURCE) == FLOOR_SRC_CONTROLLING, "source=controlling(2)");
        const FloorTlv* t = m.field(FF_MSG_TYPE);
        CHECK(t && (unsigned char)t->value[0] == (FLOOR_RELEASE | FLOOR_ACK_REQ_BIT), "acked message type");
    }

    printf("\ncmp_floor_codec_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
