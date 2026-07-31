// PFloorCodec.cpp — MCPTT Floor Control 메시지 ↔ RTCP APP "MCPT" 바이트 인코더/디코더.
//
// 3GPP TS 24.380 §8 (Media Plane Control). 단말(android/ptt-client floor/FloorCodec.kt)과
// **동일 규약**: RTCP APP 의 5비트 subtype 이 메시지 타입을 운반하고, 본문은 floor control
// specific field 들의 TLV(Field ID(8) + Length(8) + value) 나열이다.
//
// 정렬(§8.1.3): **모든** 필드가 패딩을 포함해 4옥텟 배수다 — 필드 헤더(ID+Length)와 값의 합이
// 4의 배수가 아니면 0 으로 채운다. 고정 2옥텟 값 필드는 2+2=4 라 패딩이 없고, 가변 길이 필드
// (User ID/Track Info/List of Granted Users/Functional Alias/Location…)만 실제로 패딩된다.
// 헤더가 12옥텟이므로 이 규칙만 지키면 모든 필드가 4옥텟 경계에서 시작한다 — 모르는 필드도
// 건너뛸 수 있다(§8.1.4 "미지 필드는 무시"의 전제).
// 필드 ID 가 192 이상이면 Length 가 2옥텟이다(현 규격엔 그런 필드가 없지만 파서는 견딘다).
//
// 외부 의존 없음(<cstring>/<string>/<vector> 만) → 독립 단위테스트 가능(tests/cmp_floor_codec_test.cpp).

#include "PMcpttGroup.h"
#include <cstring>

static int _pad4(int n) { return (4 - (n % 4)) % 4; }
// 필드 헤더 길이 — Length 는 ID<192 면 1옥텟, ID>=192 면 2옥텟 (§8.1.3).
static int _fieldHdr(int id) { return id >= 192 ? 3 : 2; }

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
// Queue Info(§8.2.3.5): position(1옥텟) + priority(1옥텟).
std::string FloorQueueInfo(int position, int prio) {
    std::string s(2, '\0');
    s[0] = (char)(position & 0xFF);
    s[1] = (char)(prio & 0xFF);
    return s;
}
// SSRC 필드(§8.2.3.16): SSRC(4옥텟) + spare(2옥텟) — Length=6.
std::string FloorSsrc(unsigned int ssrc) {
    std::string s(6, '\0');
    s[0] = (char)((ssrc >> 24) & 0xFF);
    s[1] = (char)((ssrc >> 16) & 0xFF);
    s[2] = (char)((ssrc >> 8) & 0xFF);
    s[3] = (char)(ssrc & 0xFF);
    return s;
}
// List of Granted Users(§8.2.3.17): No of users(1옥텟) + [User ID length(1옥텟) + User ID]* .
//   전체 필드의 4옥텟 정렬은 BuildFloorMessage 가 붙인다(규격의 "(1 + 4의 배수)" 와 동치).
std::string FloorUserList(const std::vector<std::string>& users) {
    std::string s(1, (char)(users.size() & 0xFF));
    for (const auto& u : users) {
        if (u.size() > 255) continue;
        s.push_back((char)(u.size() & 0xFF));
        s += u;
    }
    return s;
}
// List of SSRCs(§8.2.3.18): Number of SSRCs(1옥텟) + spare(1옥텟) + SSRC(4옥텟)* .
//   순서는 List of Granted Users 의 사용자 순서와 같아야 한다.
std::string FloorSsrcList(const std::vector<unsigned int>& ssrcs) {
    std::string s(2, '\0');
    s[0] = (char)(ssrcs.size() & 0xFF);
    for (unsigned int v : ssrcs) s += FloorSsrc(v).substr(0, 4);
    return s;
}

int BuildFloorMessage(char* buf, int bufSize, unsigned char subtype,
                      unsigned int ssrc, const std::vector<FloorTlv>& fields)
{
    std::string body;
    for (const auto& f : fields) {
        int hdr = _fieldHdr(f.id);
        if (hdr == 2 && f.value.size() > 255) continue;      // Length 1옥텟
        if (hdr == 3 && f.value.size() > 65535) continue;    // Length 2옥텟
        body.push_back((char)(f.id & 0xFF));
        if (hdr == 3) body.push_back((char)((f.value.size() >> 8) & 0xFF));
        body.push_back((char)(f.value.size() & 0xFF));
        body += f.value;
        body.append(_pad4((int)(hdr + f.value.size())), '\0');   // 필드 단위 4B 정렬(§8.1.3)
    }
    body.append(_pad4((int)body.size()), '\0');  // 전체 본문 4B 정렬(필드 정렬로 이미 충족)

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
        int hdr = _fieldHdr(id);
        if (p + hdr > len) break;                                      // 손상
        int fl = (hdr == 3) ? (((unsigned char)buf[p + 1] << 8) | (unsigned char)buf[p + 2])
                            : (unsigned char)buf[p + 1];
        if (id == 0 && fl == 0) break;                                 // trailing zero 패딩
        if (p + hdr + fl > len) break;                                 // 손상
        out.fields.push_back(FloorTlv(id, std::string(buf + p + hdr, fl)));
        p += hdr + fl;
        p += _pad4(hdr + fl);                                          // 필드 단위 4B 정렬(§8.1.3)
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
