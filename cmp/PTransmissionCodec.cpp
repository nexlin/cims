// PTransmissionCodec.cpp — MCVideo 전송 제어 메시지 코덱 (3GPP TS 24.581 §9). 규약은 PTransmissionCodec.h.
//
// 정렬(§9.1.3): 필드마다 (헤더 + 값)을 4옥텟 배수로 0 패딩한다 — 규격 문구 «(2 + multiple of 4) bytes» 와 같다.
// 고정 길이 필드(Length 2·6)는 2+2=4 · 2+6=8 이라 패딩이 없고, 가변 필드(User ID·Functional Alias·Reject Phrase·Track Info)만
// 실제로 패딩된다. 헤더가 12옥텟이므로 모든 필드가 4옥텟 경계에서 시작해 모르는 필드도 건너뛸 수 있다(§9.1.4 2).

#include "PTransmissionCodec.h"

#include <cstring>

static int _pad4(int n) { return (4 - (n % 4)) % 4; }
// 필드 헤더 길이 — Length 는 ID<192 면 1옥텟, ID>=192 면 2옥텟(§9.1.3).
static int _fieldHdr(int id) { return id >= 192 ? 3 : 2; }
static const char* _appName(int app) {
    switch (app) {
    case MCV_APP_0: return MCV_NAME_0;
    case MCV_APP_1: return MCV_NAME_1;
    case MCV_APP_2: return MCV_NAME_2;
    }
    return nullptr;
}
// 이 메시지에 실을 수 있는 필드이고 값 모양이 맞는가(표 = PTransmissionDefs.h McvAllowedFields · McvFieldKindOf).
static bool _fieldOk(int app, int subtype, int id, size_t len) {
    if (id < 0 || id >= 32 || !((McvAllowedFields(app, subtype) >> id) & 1u)) return false;
    McvFieldKind k = McvFieldKindOf(id);
    int fixed = McvFixedLength(k);
    if (fixed >= 0) return (int)len == fixed;
    if (k == TFK_CAUSE || k == TFK_TRACK) return len >= 2;
    return true;
}

std::string McvU8(int v) {
    std::string s(2, '\0');
    s[0] = (char)(v & 0xFF);
    return s;
}
std::string McvU16(int v) {
    std::string s(2, '\0');
    s[0] = (char)((v >> 8) & 0xFF);
    s[1] = (char)(v & 0xFF);
    return s;
}
std::string McvQueueInfo(int position, int prio) {
    std::string s(2, '\0');
    s[0] = (char)(position & 0xFF);
    s[1] = (char)(prio & 0xFF);
    return s;
}
std::string McvSsrc(unsigned int ssrc) {
    std::string s(6, '\0');
    s[0] = (char)((ssrc >> 24) & 0xFF);
    s[1] = (char)((ssrc >> 16) & 0xFF);
    s[2] = (char)((ssrc >> 8) & 0xFF);
    s[3] = (char)(ssrc & 0xFF);
    return s;
}
std::string McvName(const char* name4) {
    std::string s(6, '\0');
    if (name4) memcpy(&s[0], name4, 4);
    return s;
}
// Reject Cause(§9.2.3.4): 16비트 원인 + 선택 Reject Phrase — Length 는 phrase 까지(패딩 제외).
std::string McvCause(int cause, const std::string& phrase) {
    return McvU16(cause) + phrase;
}

int BuildTransmissionMessage(char* buf, int bufSize, int app, unsigned char subtype, unsigned int ssrc,
                             const std::vector<McvTlv>& fields)
{
    const char* name = _appName(app);
    if (!name || !McvKnownMessage(app, subtype)) return 0;
    std::string body;
    for (const auto& f : fields) {
        if (!_fieldOk(app, subtype, f.id, f.value.size())) return 0;
        int hdr = _fieldHdr(f.id);
        if (hdr == 2 && f.value.size() > 255) return 0;          // Length 1옥텟
        body.push_back((char)(f.id & 0xFF));
        if (hdr == 3) body.push_back((char)((f.value.size() >> 8) & 0xFF));
        body.push_back((char)(f.value.size() & 0xFF));
        body += f.value;
        body.append(_pad4((int)(hdr + f.value.size())), '\0');   // 필드 단위 4옥텟 정렬(§9.1.3)
    }
    int total = MCV_RTCP_APP_HDR + (int)body.size();
    if (total > bufSize) return 0;
    memset(buf, 0, total);
    buf[0] = (char)(0x80 | (subtype & 0x1F));                    // V=2, P=0, subtype
    buf[1] = (char)MCV_RTCP_PT_APP;
    int words = total / 4 - 1;
    buf[2] = (char)((words >> 8) & 0xFF);
    buf[3] = (char)(words & 0xFF);
    buf[4] = (char)((ssrc >> 24) & 0xFF);
    buf[5] = (char)((ssrc >> 16) & 0xFF);
    buf[6] = (char)((ssrc >> 8) & 0xFF);
    buf[7] = (char)(ssrc & 0xFF);
    memcpy(buf + 8, name, 4);
    if (!body.empty()) memcpy(buf + MCV_RTCP_APP_HDR, body.data(), body.size());
    return total;
}

bool ParseTransmissionMessage(const char* buf, int len, ParsedTransmission& out)
{
    if (len < MCV_RTCP_APP_HDR) return false;
    if (((unsigned char)buf[0] & 0xC0) != 0x80) return false;          // V=2
    if ((unsigned char)buf[1] != MCV_RTCP_PT_APP) return false;        // PT=204
    int app = -1;
    for (int a = MCV_APP_0; a <= MCV_APP_2; ++a)
        if (memcmp(buf + 8, _appName(a), 4) == 0) app = a;
    if (app < 0) return false;                                          // 전송 제어 APP 이 아니다
    int subtype = (unsigned char)buf[0] & 0x1F;
    if (!McvKnownMessage(app, subtype)) return false;                   // §9.1.4 1 — 메시지 전체를 버린다
    // 헤더 length(32비트 워드 - 1)가 가리키는 끝까지만 읽는다 — 한 IP 패킷에 여러 메시지가 올 수 있다(§9.1.1).
    int declared = ((((unsigned char)buf[2]) << 8) | (unsigned char)buf[3]) * 4 + 4;
    if (declared < MCV_RTCP_APP_HDR) return false;                      // 헤더도 못 담는 length — 손상
    if (declared < len) len = declared;

    out.app = app;
    out.subtype = subtype;
    out.ssrc = (((unsigned int)(unsigned char)buf[4]) << 24) |
               (((unsigned int)(unsigned char)buf[5]) << 16) |
               (((unsigned int)(unsigned char)buf[6]) << 8) |
                ((unsigned int)(unsigned char)buf[7]);
    out.fields.clear();

    int p = MCV_RTCP_APP_HDR;
    while (p + 2 <= len) {
        int id = (unsigned char)buf[p];
        int hdr = _fieldHdr(id);
        if (p + hdr > len) break;                                      // 손상
        int fl = (hdr == 3) ? (((unsigned char)buf[p + 1] << 8) | (unsigned char)buf[p + 2])
                            : (unsigned char)buf[p + 1];
        if (id == 0 && fl == 0) break;                                 // 꼬리 0 패딩
        if (p + hdr + fl > len) break;                                 // 손상
        if (_fieldOk(app, subtype, id, fl))                            // §9.1.4 2·3 — 표 밖·모양이 틀린 필드는 버린다
            out.fields.push_back(McvTlv(id, std::string(buf + p + hdr, fl)));
        p += hdr + fl;
        p += _pad4(hdr + fl);
    }
    return true;
}

const McvTlv* ParsedTransmission::field(int id) const {
    for (const auto& f : fields) if (f.id == id) return &f;
    return nullptr;
}
std::string ParsedTransmission::str(int id) const {
    const McvTlv* f = field(id);
    return f ? f->value : std::string();
}
int ParsedTransmission::u8(int id, int dflt) const {
    const McvTlv* f = field(id);
    return (f && !f->value.empty()) ? (unsigned char)f->value[0] : dflt;
}
int ParsedTransmission::u16(int id, int dflt) const {
    const McvTlv* f = field(id);
    if (!f || f->value.size() < 2) return dflt;
    return (((unsigned char)f->value[0]) << 8) | (unsigned char)f->value[1];
}
unsigned int ParsedTransmission::ssrcOf(int id) const {
    const McvTlv* f = field(id);
    if (!f || f->value.size() < 4) return 0;
    const unsigned char* v = (const unsigned char*)f->value.data();
    return ((unsigned int)v[0] << 24) | ((unsigned int)v[1] << 16) | ((unsigned int)v[2] << 8) | v[3];
}
int ParsedTransmission::cause() const { return u16(TF_REJECT_CAUSE); }
std::string ParsedTransmission::causePhrase() const {
    const McvTlv* f = field(TF_REJECT_CAUSE);
    return (f && f->value.size() > 2) ? f->value.substr(2) : std::string();
}
int ParsedTransmission::queuePosition() const { return u8(TF_QUEUE_INFO); }
int ParsedTransmission::queuePriority() const {
    const McvTlv* f = field(TF_QUEUE_INFO);
    return (f && f->value.size() >= 2) ? (unsigned char)f->value[1] : -1;
}
std::string ParsedTransmission::messageName() const {
    const McvTlv* f = field(TF_MESSAGE_NAME);
    return (f && f->value.size() >= 4) ? f->value.substr(0, 4) : std::string();
}
