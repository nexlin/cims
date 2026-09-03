#include "floor_codec.h"

#include <cstring>

namespace cimsue {
namespace floor {

static constexpr size_t kHdr = 12;
static int pad4(size_t n) { return (int)((4 - (n % 4)) % 4); }
static int fieldHdr(uint8_t id) { return id >= 192 ? 3 : 2; }   // Length 는 ID<192 면 1옥텟, 이상이면 2옥텟(§8.1.3)

const Tlv* Message::field(uint8_t id) const {
    for (auto& f : fields) if (f.id == id) return &f;
    return nullptr;
}
std::string Message::str(uint8_t id) const { const Tlv* f = field(id); return f ? f->value : std::string(); }
int Message::u16(uint8_t id, int dflt) const {
    const Tlv* f = field(id);
    if (!f) return dflt;
    if (f->value.size() >= 2) return (((unsigned char)f->value[0]) << 8) | (unsigned char)f->value[1];
    if (f->value.size() == 1) return (unsigned char)f->value[0];
    return dflt;
}
uint32_t Message::u32(uint8_t id) const {
    const Tlv* f = field(id);
    if (!f || f->value.size() < 4) return 0;
    const unsigned char* v = (const unsigned char*)f->value.data();
    return ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
}
int Message::priority() const {
    const Tlv* f = field((uint8_t)Field::PRIORITY);
    return (f && !f->value.empty()) ? (unsigned char)f->value[0] : -1;
}
int Message::queuePosition() const {
    const Tlv* f = field((uint8_t)Field::QUEUE_INFO);
    return (f && !f->value.empty()) ? (unsigned char)f->value[0] : -1;
}
std::vector<std::string> Message::grantedUsers() const {
    std::vector<std::string> out;
    const Tlv* f = field((uint8_t)Field::GRANTED_USERS);
    if (!f || f->value.empty()) return out;
    const std::string& v = f->value;
    size_t n = (unsigned char)v[0], p = 1;
    for (size_t i = 0; i < n && p < v.size(); ++i) {
        size_t l = (unsigned char)v[p];
        if (p + 1 + l > v.size()) break;
        out.push_back(v.substr(p + 1, l));
        p += 1 + l;
    }
    return out;
}
std::vector<uint32_t> Message::ssrcList() const {
    std::vector<uint32_t> out;
    const Tlv* f = field((uint8_t)Field::SSRC_LIST);
    if (!f || f->value.size() < 2) return out;
    const std::string& v = f->value;
    size_t n = (unsigned char)v[0], p = 2;
    for (size_t i = 0; i < n && p + 4 <= v.size(); ++i, p += 4) {
        const unsigned char* b = (const unsigned char*)v.data() + p;
        out.push_back(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]);
    }
    return out;
}
std::vector<Speaker> Message::talkers() const {
    std::vector<Speaker> out;
    auto users = grantedUsers();
    if (!users.empty()) {
        auto ss = ssrcList();
        for (size_t i = 0; i < users.size(); ++i) out.push_back(Speaker{users[i], i < ss.size() ? ss[i] : 0});
        return out;
    }
    std::string one = grantedParty();
    if (one.empty()) one = userId();
    if (!one.empty()) out.push_back(Speaker{one, speakerSsrc()});
    return out;
}

std::string encode(const Message& m) {
    std::string body;
    for (auto& f : m.fields) {
        int hdr = fieldHdr(f.id);
        if (hdr == 2 && f.value.size() > 255) continue;
        if (hdr == 3 && f.value.size() > 65535) continue;
        body += (char)f.id;
        if (hdr == 3) body += (char)((f.value.size() >> 8) & 0xFF);
        body += (char)(f.value.size() & 0xFF);
        body += f.value;
        body.append(pad4(hdr + f.value.size()), '\0');
    }
    body.append(pad4(body.size()), '\0');
    std::string out(kHdr, '\0');
    uint8_t subtype = (uint8_t)((m.op & 0x0F) | (m.ackRequired ? kAckRequiredBit : 0));
    out[0] = (char)(0x80 | (subtype & 0x1F));
    out[1] = (char)kRtcpPtApp;
    size_t words = (kHdr + body.size()) / 4 - 1;
    out[2] = (char)((words >> 8) & 0xFF);
    out[3] = (char)(words & 0xFF);
    out[4] = (char)((m.ssrc >> 24) & 0xFF);
    out[5] = (char)((m.ssrc >> 16) & 0xFF);
    out[6] = (char)((m.ssrc >> 8) & 0xFF);
    out[7] = (char)(m.ssrc & 0xFF);
    std::memcpy(&out[8], kRtcpName, 4);
    return out + body;
}

bool decode(const uint8_t* buf, size_t len, Message& out) {
    if (len < kHdr) return false;
    if ((buf[0] & 0xC0) != 0x80) return false;
    if (buf[1] != kRtcpPtApp) return false;
    if (std::memcmp(buf + 8, kRtcpName, 4) != 0) return false;
    uint8_t subtype = buf[0] & 0x1F;
    out.op = subtype & 0x0F;
    out.ackRequired = (subtype & kAckRequiredBit) != 0;
    out.ssrc = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    out.fields.clear();
    size_t p = kHdr;
    while (p + 2 <= len) {
        uint8_t id = buf[p];
        int hdr = fieldHdr(id);
        if (p + hdr > len) break;
        size_t fl = hdr == 3 ? (((size_t)buf[p + 1] << 8) | buf[p + 2]) : buf[p + 1];
        if (id == 0 && fl == 0) break;                 // trailing zero 패딩
        if (p + hdr + fl > len) break;
        out.fields.push_back(Tlv{id, std::string((const char*)buf + p + hdr, fl)});
        p += hdr + fl;
        p += pad4(hdr + fl);
    }
    return true;
}

Tlv u16Field(uint8_t id, int v) {
    std::string s(2, '\0');
    s[0] = (char)((v >> 8) & 0xFF);
    s[1] = (char)(v & 0xFF);
    return Tlv{id, s};
}

std::string request(uint32_t ssrc, const std::string& userId, int priority, int indicator) {
    Message m; m.op = (uint8_t)Op::REQUEST; m.ssrc = ssrc;
    if (priority >= 0) { std::string p(2, '\0'); p[0] = (char)(priority & 0xFF); m.fields.push_back(Tlv{(uint8_t)Field::PRIORITY, p}); }
    m.fields.push_back(Tlv{(uint8_t)Field::USER_ID, userId});
    if (indicator >= 0) m.fields.push_back(u16Field((uint8_t)Field::FLOOR_INDICATOR, indicator));
    return encode(m);
}
std::string release(uint32_t ssrc, const std::string& userId, int indicator) {
    Message m; m.op = (uint8_t)Op::RELEASE; m.ssrc = ssrc;
    m.fields.push_back(Tlv{(uint8_t)Field::USER_ID, userId});
    if (indicator >= 0) m.fields.push_back(u16Field((uint8_t)Field::FLOOR_INDICATOR, indicator));
    return encode(m);
}
std::string queuePositionRequest(uint32_t ssrc, const std::string& userId) {
    Message m; m.op = (uint8_t)Op::QUEUE_POS_REQ; m.ssrc = ssrc;
    m.fields.push_back(Tlv{(uint8_t)Field::USER_ID, userId});
    return encode(m);
}
std::string cancelQueuedRequest(uint32_t ssrc) {
    Message m; m.op = (uint8_t)Op::QUEUED_CANCEL; m.ssrc = ssrc;
    m.fields.push_back(u16Field((uint8_t)Field::QUEUED_PURPOSE, (int)QueuedPurpose::CANCEL_REQUEST));
    return encode(m);
}
std::string ackOf(uint32_t ssrc, uint8_t ackedSubtype) {
    Message m; m.op = (uint8_t)Op::ACK; m.ssrc = ssrc;
    m.fields.push_back(u16Field((uint8_t)Field::SOURCE, (int)Source::PARTICIPANT));
    std::string t(2, '\0'); t[0] = (char)(ackedSubtype & 0x1F);          // Message Type: 상위 옥텟 subtype, 하위 spare
    m.fields.push_back(Tlv{(uint8_t)Field::MSG_TYPE, t});
    return encode(m);
}
std::string ack(uint32_t ssrc, const std::string& userId) {
    Message m; m.op = (uint8_t)Op::ACK; m.ssrc = ssrc;
    if (!userId.empty()) m.fields.push_back(Tlv{(uint8_t)Field::USER_ID, userId});
    return encode(m);
}

}  // namespace floor
}  // namespace cimsue
