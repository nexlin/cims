// libcimsue 내부 — MCVideo 전송 제어 메시지 코덱 (TS 24.581 §9). cmp/PTransmissionCodec.cpp 와 바이트 호환
// (단위시험 McvXCheck 가 CMP 코덱과 교차 검증). 정의 상수는 생성 헤더 tc_defs.h(정본 mcvideo_tc_defs.yaml).
//
// 전송: RTCP APP(PT=204) — name MCV0(참여자→서버)·MCV1(서버→참여자)·MCV2(양방향), 5비트 subtype = 메시지 타입(+ack 요구 비트 0x10).
// 같은 subtype 값이 name 마다 다른 메시지라 메시지 식별 = (app, op). 본문은 필드 TLV 나열이며 필드마다 (헤더+값)을 4옥텟
// 경계로 패딩한다(§9.1.3). decode 는 모르는 subtype 이면 실패하고(§9.1.4 1), 그 메시지 표 밖·모양이 틀린 필드는 버린다(§9.1.4 2·3).
// encode 는 표 밖·모양이 틀린 필드를 싣지 않는다(빈 문자열 — 상태 머신의 잘못을 시험에서 드러낸다).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tc_defs.h"

namespace cimsue {
namespace mcvideo {

struct Tlv {
    uint8_t id;
    std::string value;                // 패딩 제외한 값(Length 가 세는 부분)
};

struct Message {
    AppName app = AppName::MCV0;
    uint8_t op = 0;                   // ack 요구 비트를 걷어낸 메시지 타입
    bool ackRequired = false;
    uint32_t ssrc = 0;                // 헤더 SSRC — 보낸 쪽 RTCP SSRC(§9.1.2, SDP mc_transmission_ssrc 로 정한 값)
    std::vector<Tlv> fields;

    const Tlv* field(Field f) const;
    bool has(Field f) const { return field(f) != nullptr; }
    std::string str(Field f) const;                  // uri 필드
    int u8(Field f, int dflt = -1) const;            // u8 필드 첫 옥텟
    int u16(Field f, int dflt = -1) const;           // u16 필드
    uint32_t ssrcOf(Field f) const;                  // ssrc 필드, 없으면 0

    std::string userId() const { return str(Field::USER_ID); }
    std::string transmittingUserId() const { return str(Field::TRANSMITTING_USER_ID); }
    std::string functionalAlias() const { return str(Field::FUNCTIONAL_ALIAS); }
    uint32_t audioSsrc() const { return ssrcOf(Field::AUDIO_SSRC); }
    uint32_t videoSsrc() const { return ssrcOf(Field::VIDEO_SSRC); }
    int priority() const { return u8(Field::TRANSMISSION_PRIORITY); }
    int receptionPriority() const { return u8(Field::RECEPTION_PRIORITY); }
    int durationSec() const { return u16(Field::DURATION); }
    int cause() const { return u16(Field::REJECT_CAUSE); }
    std::string causePhrase() const;
    int queuePosition() const { return u8(Field::QUEUE_INFO); }
    int queuePriority() const;
    int indicator() const { return u16(Field::TRANSMISSION_INDICATOR); }
    int permission() const { return u16(Field::PERMISSION); }
    int result() const { return u16(Field::RESULT); }
    int receptionMode() const { return u16(Field::RECEPTION_MODE); }
    int msgSeq() const { return u16(Field::MSG_SEQ); }
    int source() const { return u16(Field::SOURCE); }
    std::string messageName() const;                 // Message Name 4옥텟(Ack 대상 name)
    int messageType() const { return u8(Field::MSG_TYPE); }   // Ack 대상 subtype(ack 비트 없음)
};

/** 부호화 — 실패(모르는 메시지·표 밖 필드·고정 길이 불일치·Length 초과)면 빈 문자열. */
std::string encode(const Message& m);
bool decode(const uint8_t* buf, size_t len, Message& out);

// ── 필드 값 빌더(kind 별 모양) ──
Tlv u8Field(Field f, int v);                         // 8비트 + spare
Tlv u16Field(Field f, int v);
Tlv ssrcField(Field f, uint32_t ssrc);               // SSRC + spare 2
Tlv strField(Field f, const std::string& s);         // uri
Tlv causeField(int cause, const std::string& phrase = std::string());

// ── 참여자 측 빌더 (§6.2.4 송출 · §6.2.5 수신 절차가 싣는 필드) ──
/** Transmission Request(§6.2.4.3.2) — priority<0 = 기본 우선순위라 싣지 않는다(2a), indicator<0 = 일반 호라 싣지 않는다(2b). */
std::string transmissionRequest(uint32_t ssrc, int priority = -1, int indicator = -1);
/** Transmission End Request(§6.2.4.5.3·§6.2.4.4.7·§6.2.4.9.4) — indicator 는 normal 로 시작한 방송 호에서만(A-bit). */
std::string transmissionEndRequest(uint32_t ssrc, int indicator = -1);
/** Transmission End Response(§6.2.4.5.7 4) — 서버의 Transmission End Request 에 답한다. */
std::string transmissionEndResponse(uint32_t ssrc);
/** Queue Position Request(§6.2.4.9.3). */
std::string queuePositionRequest(uint32_t ssrc);
/** Receive Media Request(§6.2.5.3.3) — 받을 송출 = Media Transmission Notification 에서 저장한 송출자 ID·Audio/Video SSRC. */
std::string receiveMediaRequest(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc, uint32_t videoSsrc,
                                int receptionPriority = -1, int indicator = -1);
/** Media Reception End Request(§6.2.5.5.3·§6.2.5.4.6) — 송출자 ID·Audio/Video SSRC 를 싣는다(1b). */
std::string mediaReceptionEndRequest(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc, uint32_t videoSsrc,
                                     int indicator = -1);
/** Media Reception End Response(§6.2.5.5.5 5) — 서버의 Media Reception End Request 에 답한다(끝낸 송출을 싣는다). */
std::string mediaReceptionEndResponse(uint32_t ssrc, const std::string& transmitterId, uint32_t audioSsrc, uint32_t videoSsrc);
/** Transmission Control Ack(§9.2.31) — Source = participant, Message Name = 확인 대상 name(MCV1·MCV2 의 subtype 이 겹친다),
 *  Message Type = 확인 대상 subtype(첫 비트 0 — §9.2.3.10). */
std::string ackOf(uint32_t ssrc, AppName ackedApp, uint8_t ackedSubtype);

}  // namespace mcvideo
}  // namespace cimsue
