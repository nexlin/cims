#include "tc_codec.h"

#include <cstring>

namespace cimsue {
namespace mcvideo {

static constexpr size_t kHdr = 12;
static int pad4(size_t n) { return (int)((4 - (n % 4)) % 4); }
static int fieldHdr(uint8_t id) { return id >= 192 ? 3 : 2; }   // Length 는 ID<192 면 1옥텟, 이상이면 2옥텟(§9.1.3)
static const char* nameOf(AppName a) {
    switch (a) {
        case AppName::MCV0: return kNameMcv0;
        case AppName::MCV1: return kNameMcv1;
        case AppName::MCV2: return kNameMcv2;
    }
    return kNameMcv0;
}
// 이 메시지에 실을 수 있는 필드이고 값 모양이 맞는가(정본 = tc_defs.h allowedFields·fieldKind).
static bool fieldOk(AppName app, uint8_t op, uint8_t id, size_t len) {
    if (id >= 32 || !((allowedFields(app, op) >> id) & 1u)) return false;
    FieldKind k = fieldKind(id);
    int fixed = fixedLength(k);
    if (fixed >= 0) return (int)len == fixed;
    if (k == FieldKind::Cause || k == FieldKind::Track) return len >= 2;
    return true;
}

const Tlv* Message::field(Field f) const {
    for (auto& t : fields) if (t.id == (uint8_t)f) return &t;
    return nullptr;
}
std::string Message::str(Field f) const { const Tlv* t = field(f); return t ? t->value : std::string(); }
int Message::u8(Field f, int dflt) const {
    const Tlv* t = field(f);
    return (t && !t->value.empty()) ? (unsigned char)t->value[0] : dflt;
}
int Message::u16(Field f, int dflt) const {
    const Tlv* t = field(f);
    if (!t || t->value.size() < 2) return dflt;
    return (((unsigned char)t->value[0]) << 8) | (unsigned char)t->value[1];
}
uint32_t Message::ssrcOf(Field f) const {
    const Tlv* t = field(f);
    if (!t || t->value.size() < 4) return 0;
    const unsigned char* v = (const unsigned char*)t->value.data();
    return ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
}
std::string Message::causePhrase() const {
    const Tlv* t = field(Field::REJECT_CAUSE);
    return (t && t->value.size() > 2) ? t->value.substr(2) : std::string();
}
int Message::queuePriority() const {
    const Tlv* t = field(Field::QUEUE_INFO);
    return (t && t->value.size() >= 2) ? (unsigned char)t->value[1] : -1;
}
std::string Message::messageName() const {
    const Tlv* t = field(Field::MESSAGE_NAME);
    return (t && t->value.size() >= 4) ? t->value.substr(0, 4) : std::string();
}

std::string encode(const Message& m) {
    uint8_t op = m.op & kSubtypeMask;
    if (!knownMessage(m.app, op)) return std::string();
    std::string body;
    for (auto& f : m.fields) {
        if (!fieldOk(m.app, op, f.id, f.value.size())) return std::string();
        int hdr = fieldHdr(f.id);
        if (hdr == 2 && f.value.size() > 255) return std::string();
        body += (char)f.id;
        if (hdr == 3) body += (char)((f.value.size() >> 8) & 0xFF);
        body += (char)(f.value.size() & 0xFF);
        body += f.value;
        body.append(pad4(hdr + f.value.size()), '\0');        // 필드 단위 4옥텟 정렬(§9.1.3)
    }
    std::string out(kHdr, '\0');
    uint8_t subtype = (uint8_t)(op | (m.ackRequired ? kAckRequiredBit : 0));
    out[0] = (char)(0x80 | (subtype & 0x1F));
    out[1] = (char)kRtcpPtApp;
    size_t words = (kHdr + body.size()) / 4 - 1;
    out[2] = (char)((words >> 8) & 0xFF);
    out[3] = (char)(words & 0xFF);
    out[4] = (char)((m.ssrc >> 24) & 0xFF);
    out[5] = (char)((m.ssrc >> 16) & 0xFF);
    out[6] = (char)((m.ssrc >> 8) & 0xFF);
    out[7] = (char)(m.ssrc & 0xFF);
    std::memcpy(&out[8], nameOf(m.app), 4);
    return out + body;
}

bool decode(const uint8_t* buf, size_t len, Message& out) {
    if (len < kHdr) return false;
    if ((buf[0] & 0xC0) != 0x80) return false;
    if (buf[1] != kRtcpPtApp) return false;
    AppName app;
    if (!appNameOf((const char*)buf + 8, app)) return false;           // 전송 제어 APP 이 아니다
    uint8_t subtype = buf[0] & 0x1F;
    uint8_t op = subtype & kSubtypeMask;
    if (!knownMessage(app, op)) return false;                          // §9.1.4 1 — 메시지 전체를 버린다
    // 헤더 length 가 가리키는 끝까지만 — 한 IP 패킷에 여러 메시지가 올 수 있다(§9.1.1)
    size_t declared = ((((size_t)buf[2]) << 8) | buf[3]) * 4 + 4;
    if (declared < kHdr) return false;
    if (declared < len) len = declared;
    out.app = app;
    out.op = op;
    out.ackRequired = (subtype & kAckRequiredBit) != 0;
    out.ssrc = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    out.fields.clear();
    size_t p = kHdr;
    while (p + 2 <= len) {
        uint8_t id = buf[p];
        int hdr = fieldHdr(id);
        if (p + hdr > len) break;
        size_t fl = hdr == 3 ? (((size_t)buf[p + 1] << 8) | buf[p + 2]) : buf[p + 1];
        if (id == 0 && fl == 0) break;                                 // 꼬리 0 패딩
        if (p + hdr + fl > len) break;
        if (fieldOk(app, op, id, fl))                                  // §9.1.4 2·3 — 표 밖·모양이 틀린 필드는 버린다
            out.fields.push_back(Tlv{id, std::string((const char*)buf + p + hdr, fl)});
        p += hdr + fl;
        p += pad4(hdr + fl);
    }
    return true;
}

Tlv u8Field(Field f, int v) {
    std::string s(2, '\0');
    s[0] = (char)(v & 0xFF);
    return Tlv{(uint8_t)f, s};
}
Tlv u16Field(Field f, int v) {
    std::string s(2, '\0');
    s[0] = (char)((v >> 8) & 0xFF);
    s[1] = (char)(v & 0xFF);
    return Tlv{(uint8_t)f, s};
}
Tlv ssrcField(Field f, uint32_t ssrc) {
    std::string s(6, '\0');
    s[0] = (char)((ssrc >> 24) & 0xFF);
    s[1] = (char)((ssrc >> 16) & 0xFF);
    s[2] = (char)((ssrc >> 8) & 0xFF);
    s[3] = (char)(ssrc & 0xFF);
    return Tlv{(uint8_t)f, s};
}
Tlv strField(Field f, const std::string& s) { return Tlv{(uint8_t)f, s}; }
Tlv causeField(int cause, const std::string& phrase) {
    Tlv t = u16Field(Field::REJECT_CAUSE, cause);
    t.value += phrase;
    return t;
}

static Message msg(AppName app, uint8_t op, uint32_t ssrc) {
    Message m;
    m.app = app;
    m.op = op;
    m.ssrc = ssrc;
    return m;
}
static void addTransmission(Message& m, const std::string& id, uint32_t audioSsrc, uint32_t videoSsrc) {
    m.fields.push_back(strField(Field::TRANSMITTING_USER_ID, id));
    m.fields.push_back(ssrcField(Field::AUDIO_SSRC, audioSsrc));
    m.fields.push_back(ssrcField(Field::VIDEO_SSRC, videoSsrc));
}

std::string transmissionRequest(uint32_t ssrc, int priority, int indicator) {
    Message m = msg(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST, ssrc);
    if (priority >= 0) m.fields.push_back(u8Field(Field::TRANSMISSION_PRIORITY, priority));
    if (indicator >= 0) m.fields.push_back(u16Field(Field::TRANSMISSION_INDICATOR, indicator));
    return encode(m);
}
// 내 송출을 가리키는 세 필드 — ID 는 늘, SSRC 는 아는 값만(허가 전 취소에는 아직 SSRC 가 없다).
static void addOwnTransmission(Message& m, const std::string& id, uint32_t audioSsrc, uint32_t videoSsrc) {
    if (id.empty()) return;
    m.fields.push_back(strField(Field::TRANSMITTING_USER_ID, id));
    if (audioSsrc) m.fields.push_back(ssrcField(Field::AUDIO_SSRC, audioSsrc));
    if (videoSsrc) m.fields.push_back(ssrcField(Field::VIDEO_SSRC, videoSsrc));
}
std::string transmissionEndRequest(uint32_t ssrc, int indicator, const std::string& transmitterId, uint32_t audioSsrc,
                                   uint32_t videoSsrc) {
    Message m = msg(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_REQUEST, ssrc);
    addOwnTransmission(m, transmitterId, audioSsrc, videoSsrc);
    if (indicator >= 0) m.fields.push_back(u16Field(Field::TRANSMISSION_INDICATOR, indicator));
    return encode(m);
}
std::string transmissionEndResponse(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc, uint32_t videoSsrc) {
    Message m = msg(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_RESPONSE, ssrc);
    addOwnTransmission(m, transmitterId, audioSsrc, videoSsrc);
    return encode(m);
}
std::string queuePositionRequest(uint32_t ssrc) {
    return encode(msg(AppName::MCV0, (uint8_t)Mcv0::QUEUE_POSITION_REQUEST, ssrc));
}
std::string receiveMediaRequest(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc, uint32_t videoSsrc,
                                int receptionPriority, int indicator) {
    Message m = msg(AppName::MCV0, (uint8_t)Mcv0::RECEIVE_MEDIA_REQUEST, ssrc);
    addTransmission(m, transmitterId, audioSsrc, videoSsrc);
    if (receptionPriority >= 0) m.fields.push_back(u8Field(Field::RECEPTION_PRIORITY, receptionPriority));
    if (indicator >= 0) m.fields.push_back(u16Field(Field::TRANSMISSION_INDICATOR, indicator));
    return encode(m);
}
std::string mediaReceptionEndRequest(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc, uint32_t videoSsrc,
                                     int indicator) {
    Message m = msg(AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_REQUEST, ssrc);
    addTransmission(m, transmitterId, audioSsrc, videoSsrc);
    if (indicator >= 0) m.fields.push_back(u16Field(Field::TRANSMISSION_INDICATOR, indicator));
    return encode(m);
}
std::string mediaReceptionEndResponse(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc,
                                      uint32_t videoSsrc) {
    Message m = msg(AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_RESPONSE, ssrc);
    addTransmission(m, transmitterId, audioSsrc, videoSsrc);
    return encode(m);
}
std::string ackOf(uint32_t ssrc, AppName ackedApp, uint8_t ackedSubtype) {
    Message m = msg(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_CONTROL_ACK, ssrc);
    m.fields.push_back(u16Field(Field::SOURCE, (int)Source::PARTICIPANT));
    std::string name(6, '\0');
    std::memcpy(&name[0], nameOf(ackedApp), 4);
    m.fields.push_back(Tlv{(uint8_t)Field::MESSAGE_NAME, name});
    m.fields.push_back(u8Field(Field::MSG_TYPE, ackedSubtype & kSubtypeMask));   // 첫 비트 0(§9.2.3.10)
    return encode(m);
}

}  // namespace mcvideo
}  // namespace cimsue
