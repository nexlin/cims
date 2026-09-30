// PTransmissionCodec.h — MCVideo 전송 제어 메시지 ↔ RTCP APP "MCV0"·"MCV1"·"MCV2" 바이트 코덱 (3GPP TS 24.581 §9).
//
// 단말 코어(sdk/core/src/mcvideo/tc_codec.cpp)와 **바이트 호환** — 두 코덱 모두 정의 상수를 같은 정본 테이블
// (docs/design/features/mcvideo_tc_defs.yaml)의 생성 헤더에서 쓰고, cimsue_test 의 McvXCheck 가 서로의 출력을 읽는다.
//
// 전송(§9.1.2): 메시지 하나 = RTCP APP 패킷 하나(PT=204). name 이 메시지 집합(MCV0 참여자→서버 · MCV1 서버→참여자 ·
// MCV2 양방향)을, 5비트 subtype 이 메시지 타입(+첫 비트 = ack 요구)을 가른다 — 같은 subtype 값이 name 마다 다른 메시지다.
// 본문(§9.1.3)은 필드 TLV(Field ID 1옥텟 + Length 1옥텟(ID<192) + 값) 나열이고 필드마다 (헤더+값)을 4옥텟 경계로 0 패딩한다.
//
// 검사(§9.1.4): 모르는 subtype 이면 메시지 전체를 버리고(Parse 실패), 그 메시지 표에 없는 필드·고정 길이가 틀린 필드는 버린다.
// Build 는 그 메시지 표 밖 필드·길이가 틀린 고정 필드를 싣지 않는다(실패 — 상태 머신의 잘못을 시험에서 드러낸다).
//
// 외부 의존 없음(<string>/<vector> + 생성 헤더) → CMP 밖(단위시험)에서도 링크된다.
#ifndef __TRANSMISSION_CODEC_H__
#define __TRANSMISSION_CODEC_H__

#include <string>
#include <vector>

#include "PTransmissionDefs.h"

#define MCV_RTCP_APP_HDR 12   // V/P/subtype + PT + length + SSRC + name

// 전송 제어 필드 하나(TLV). value 는 패딩을 뺀 값 바이트(Length 가 세는 부분).
struct McvTlv {
    int id;
    std::string value;
    McvTlv(int i, const std::string& v) : id(i), value(v) {}
};

// 파싱된 전송 제어 메시지.
struct ParsedTransmission {
    int app = -1;                 // McvAppName
    int subtype = -1;             // 5비트 그대로(ack 요구 비트 포함) — 메시지 타입은 op()
    unsigned int ssrc = 0;        // 헤더 SSRC(보낸 쪽 RTCP SSRC, §9.1.2)
    std::vector<McvTlv> fields;

    int op() const { return MCV_SUBTYPE(subtype); }
    bool ackRequired() const { return subtype >= 0 && (subtype & MCV_ACK_REQ_BIT) != 0; }
    const McvTlv* field(int id) const;
    bool has(int id) const { return field(id) != nullptr; }
    std::string str(int id) const;                 // uri 필드(User ID·Functional Alias …)
    int u8(int id, int dflt = -1) const;           // u8 필드 첫 옥텟(Priority·Message Type)
    int u16(int id, int dflt = -1) const;          // u16 필드(Duration·Indicator·Permission·Result·Source …)
    unsigned int ssrcOf(int id) const;             // ssrc 필드(Audio/Video SSRC), 없으면 0
    int cause() const;                             // Reject Cause 의 16비트 원인, 없으면 -1
    std::string causePhrase() const;               // Reject Phrase(없으면 빈 문자열)
    int queuePosition() const;                     // Queue Info 첫 옥텟, 없으면 -1
    int queuePriority() const;                     // Queue Info 둘째 옥텟, 없으면 -1
    std::string messageName() const;               // Message Name 4옥텟(ASCII), 없으면 빈 문자열
};

// 필드 값 빌더 — kind 별 모양(PTransmissionDefs.h McvFieldKind).
std::string McvU8(int v);                          // 8비트 + spare → Length 2
std::string McvU16(int v);                         // 16비트 → Length 2
std::string McvQueueInfo(int position, int prio);  // 8비트 둘 → Length 2
std::string McvSsrc(unsigned int ssrc);            // SSRC + spare 2 → Length 6
std::string McvName(const char* name4);            // ASCII 4 + spare 2 → Length 6
std::string McvCause(int cause, const std::string& phrase = std::string());   // 16비트 + 선택 phrase

// 메시지 빌드: 12옥텟 RTCP APP 헤더 + TLV 본문. app = McvAppName, subtype = 메시지 타입(| MCV_ACK_REQ_BIT).
// 반환 = 쓴 바이트 수, 실패(버퍼 부족·모르는 메시지·표 밖 필드·고정 길이 불일치) = 0.
int BuildTransmissionMessage(char* buf, int bufSize, int app, unsigned char subtype, unsigned int ssrc,
                             const std::vector<McvTlv>& fields);
// 수신 패킷 파싱 — MCV0~2 APP 이 아니거나 모르는 subtype 이면 false(§9.1.4 1). 표 밖·길이 불일치 필드는 버린다(§9.1.4 2·3).
bool ParseTransmissionMessage(const char* buf, int len, ParsedTransmission& out);

#endif  // __TRANSMISSION_CODEC_H__
