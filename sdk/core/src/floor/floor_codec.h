// libcimsue 내부 — MCPTT floor control 메시지 코덱 (TS 24.380 §8). cmp/PFloorCodec.cpp · android FloorCodec.kt 와
// 바이트 호환(단위시험 cimsue_floor_xcheck 가 CMP 코덱과 교차 검증). 정의 상수는 생성 헤더 floor_defs.h.
//
// 전송: RTCP APP(PT=204, name "MCPT"), 5비트 subtype=메시지 타입(+ack 요구 비트 0x10). 본문은 TLV 나열이며
// 모든 필드가 (헤더+값)을 4옥텟 경계로 패딩한다(§8.1.3) — 미지 필드도 건너뛸 수 있다(§8.1.4).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "floor_defs.h"

namespace cimsue {
namespace floor {

struct Tlv {
    uint8_t id;
    std::string value;                // 패딩 제외한 실제 값
};

struct Speaker {
    std::string id;
    uint32_t ssrc = 0;                // 0 = 미상
};

struct Message {
    uint8_t op = 0;                   // ack 요구 비트를 걷어낸 기본 타입
    bool ackRequired = false;
    uint32_t ssrc = 0;                // 헤더 SSRC(송신자) — 화자 식별에 쓰지 않는다(§8.2.5)
    std::vector<Tlv> fields;

    const Tlv* field(uint8_t id) const;
    std::string str(uint8_t id) const;
    int u16(uint8_t id, int dflt = -1) const;
    uint32_t u32(uint8_t id) const;   // 선두 4옥텟(SSRC 필드)

    std::string userId() const { return str((uint8_t)Field::USER_ID); }
    std::string grantedParty() const { return str((uint8_t)Field::GRANTED_PARTY); }
    int priority() const;             // PRIORITY 첫 옥텟, 없으면 -1
    int durationSec() const { return u16((uint8_t)Field::DURATION); }
    int cause() const { return u16((uint8_t)Field::REJECT_CAUSE); }
    int queuePosition() const;        // QUEUE_INFO 첫 옥텟
    int indicator() const { return u16((uint8_t)Field::FLOOR_INDICATOR); }
    int permission() const { return u16((uint8_t)Field::PERMISSION); }
    int msgSeq() const { return u16((uint8_t)Field::MSG_SEQ); }
    uint32_t speakerSsrc() const { return u32((uint8_t)Field::SSRC); }
    int queuedPurpose() const { return u16((uint8_t)Field::QUEUED_PURPOSE); }
    int queuedResult() const { return u16((uint8_t)Field::QUEUED_RESULT); }
    std::vector<std::string> grantedUsers() const;   // §8.2.3.17
    std::vector<uint32_t> ssrcList() const;          // §8.2.3.18
    /** 이 메시지가 알리는 화자 집합 — 리스트 필드(동시 발언) 우선, 없으면 Granted Party/User ID + SSRC. */
    std::vector<Speaker> talkers() const;
};

std::string encode(const Message& m);
bool decode(const uint8_t* buf, size_t len, Message& out);

// ── participant 측 빌더 ──
/** Floor Request — priority<0 미기재(§6.3.5.4.4-1a: 유효 우선순위를 요청값으로 깎지 않게), indicator<0 미기재. */
std::string request(uint32_t ssrc, const std::string& userId, int priority = -1, int indicator = -1);
std::string release(uint32_t ssrc, const std::string& userId, int indicator = -1);
std::string queuePositionRequest(uint32_t ssrc, const std::string& userId);
/** Queued Floor Requests(§8.2.15) Purpose=Cancel Request — 자기 대기 요청 취소(목록 없음). */
std::string cancelQueuedRequest(uint32_t ssrc);
/** Floor Ack(§8.2.13) — Source=participant + Message Type(확인 대상 subtype, ack 비트 포함). */
std::string ackOf(uint32_t ssrc, uint8_t ackedSubtype);
/** Floor Ack keepalive 변형 — User ID 로 서버가 NAT 뒤 참가자 주소를 latch(ue_nat_traversal.md §7.1). */
std::string ack(uint32_t ssrc, const std::string& userId);

Tlv u16Field(uint8_t id, int v);

}  // namespace floor
}  // namespace cimsue
