// PFloorCodec.cpp — MCPTT Floor Control 메시지 ↔ RTCP APP "MCPT" 바이트 인코더/디코더.
//
// 3GPP TS 24.380 §8 (Media Plane Control). 단말(android/ptt-client floor/FloorCodec.kt)과
// **동일 규약**: RTCP APP 의 5비트 subtype 이 메시지 타입을 운반하고, 본문은 floor control
// specific field 들의 TLV(Field ID(8) + Length(8) + value) 나열이다.
//
// 정렬(§8.2.3): 가변 길이 문자열 필드(Granted Party/User ID/Queued User ID/Track Info)만
// 32비트 경계로 패딩(2+Length 기준), 고정 길이 필드는 패딩 없음. 전체 패킷도 32비트 정렬.
//
// 외부 의존 없음(<cstring>/<string>/<vector> 만) → 독립 단위테스트 가능(tests/cmp_floor_codec_test.cpp).

#include "PMcpttGroup.h"
#include <cstring>

static int _pad4(int n) { return (4 - (n % 4)) % 4; }
static bool _isStringField(int id) {
    return id == FF_GRANTED_PARTY || id == FF_USER_ID ||
           id == FF_QUEUED_USER_ID || id == FF_TRACK_INFO;
}

std::string FloorU16(int v) {
    std::string s(2, '\0');
    s[0] = (char)((v >> 8) & 0xFF);
    s[1] = (char)(v & 0xFF);
    return s;
}
// Floor Priority(§8.2.3.2): 2옥텟 중 MSB 옥텟이 우선순위.
std::string FloorPriority(int prio) {
    std::string s(2, '\0');
    s[0] = (char)(prio & 0xFF);
    return s;
}
// Queue Info(§8.2.3.3): position(1옥텟) + priority(1옥텟).
std::string FloorQueueInfo(int position, int prio) {
    std::string s(2, '\0');
    s[0] = (char)(position & 0xFF);
    s[1] = (char)(prio & 0xFF);
    return s;
}

int BuildFloorMessage(char* buf, int bufSize, unsigned char subtype,
                      unsigned int ssrc, const std::vector<FloorTlv>& fields)
{
    std::string body;
    for (const auto& f : fields) {
        if (f.value.size() > 255) continue;     // Length 는 1옥텟
        body.push_back((char)(f.id & 0xFF));
        body.push_back((char)(f.value.size() & 0xFF));
        body += f.value;
        if (_isStringField(f.id))                // 문자열 필드만 4B 경계 패딩
            body.append(_pad4((int)(2 + f.value.size())), '\0');
    }
    body.append(_pad4((int)body.size()), '\0');  // 전체 본문 4B 정렬

    int total = RTCP_APP_HDR + (int)body.size();
    if (total > bufSize) return 0;
    memset(buf, 0, total);
    buf[0] = (char)(0x80 | (subtype & 0x1F));    // V=2, P=0, subtype=메시지타입
    buf[1] = (char)RTCP_PT_APP;
    int words = total / 4 - 1;
    buf[2] = (char)((words >> 8) & 0xFF);
    buf[3] = (char)(words & 0xFF);
    buf[4] = (char)((ssrc >> 24) & 0xFF);
    buf[5] = (char)((ssrc >> 16) & 0xFF);
    buf[6] = (char)((ssrc >> 8) & 0xFF);
    buf[7] = (char)(ssrc & 0xFF);
    memcpy(buf + 8, "MCPT", 4);
    if (!body.empty()) memcpy(buf + RTCP_APP_HDR, body.data(), body.size());
    return total;
}

bool ParseFloorMessage(const char* buf, int len, ParsedFloor& out)
{
    if (len < RTCP_APP_HDR) return false;
    if (((unsigned char)buf[0] & 0xC0) != 0x80) return false;          // V=2
    if ((unsigned char)buf[1] != RTCP_PT_APP) return false;            // PT=204
    if (memcmp(buf + 8, "MCPT", 4) != 0) return false;                 // name

    out.subtype = (unsigned char)buf[0] & 0x1F;
    out.ssrc = (((unsigned int)(unsigned char)buf[4]) << 24) |
               (((unsigned int)(unsigned char)buf[5]) << 16) |
               (((unsigned int)(unsigned char)buf[6]) << 8) |
                ((unsigned int)(unsigned char)buf[7]);
    out.fields.clear();

    int p = RTCP_APP_HDR;
    while (p + 2 <= len) {
        int id = (unsigned char)buf[p];
        int fl = (unsigned char)buf[p + 1];
        int start = p;
        p += 2;
        if (p + fl > len) break;                                       // 손상
        std::string value(buf + p, fl);
        p += fl;
        if (_isStringField(id)) p += _pad4(p - start);                 // 문자열 4B 정렬
        if (id == 0 && fl == 0) break;                                 // trailing zero 패딩
        out.fields.push_back(FloorTlv(id, value));
    }
    return true;
}

const FloorTlv* ParsedFloor::field(int id) const {
    for (const auto& f : fields) if (f.id == id) return &f;
    return nullptr;
}
std::string ParsedFloor::str(int id) const {
    const FloorTlv* f = field(id);
    return f ? f->value : std::string();
}
int ParsedFloor::u16(int id, int dflt) const {
    const FloorTlv* f = field(id);
    if (!f || f->value.size() < 2) return dflt;
    return (((unsigned char)f->value[0]) << 8) | (unsigned char)f->value[1];
}
int ParsedFloor::priority() const {
    const FloorTlv* f = field(FF_PRIORITY);
    if (!f || f->value.empty()) return -1;
    return (unsigned char)f->value[0];
}
