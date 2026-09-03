// libcimsue 내부 — MCData SDS 코덱 (TS 24.282 §15 + Annex D/E). 원천: android/ptt-client mcdata/McDataCodec.kt.
//
// SIP MESSAGE 본문 = multipart/mixed:
//   application/vnd.3gpp.mcdata-info+xml   : 대상 그룹 URI
//   application/vnd.3gpp.mcdata-signalling : SDS SIGNALLING PAYLOAD / SDS NOTIFICATION (TLV, base64)
//   application/vnd.3gpp.mcdata-payload    : DATA PAYLOAD (TLV, base64)
// 바이너리 TLV 파트는 Content-Transfer-Encoding: base64 (Android 바인딩과의 와이어 호환 — mcdata_messaging.md).
#pragma once

#include <string>
#include <vector>

#include "cimsue/types.h"

namespace cimsue {
namespace mcdata {

constexpr int kMsgSdsSignalling = 0x01;
constexpr int kMsgFdSignalling = 0x02;
constexpr int kMsgDataPayload = 0x03;
constexpr int kMsgSdsNotification = 0x05;
constexpr int kDispReqDelivery = 0x01;
constexpr const char* kCtInfo = "application/vnd.3gpp.mcdata-info+xml";
constexpr const char* kCtSignalling = "application/vnd.3gpp.mcdata-signalling";
constexpr const char* kCtPayload = "application/vnd.3gpp.mcdata-payload";

/** 그룹 스레드 conversation ID — 그룹당 결정적 UUID(Java UUID.nameUUIDFromBytes("cims-mcdata:<groupId>") 호환). */
std::string conversationIdOf(const std::string& groupId);
/** 새 message ID — 랜덤 UUID hex32. */
std::string newMessageId();

struct Body { std::string contentType; std::string body; };
/** 그룹 SDS 발신 본문. groupUri 예 "tel:g001". */
Body buildGroupSds(const std::string& groupUri, const std::string& text, const std::string& convId,
                   const std::string& msgId, bool requestDelivery, long timeSec);
/** SDS NOTIFICATION(전달/읽음 통지) 본문 — 원 발신자 1:1 대상. */
Body buildNotification(const std::string& convId, const std::string& msgId, int notifType, long timeSec);

/** multipart/mixed MCData 본문 파싱 — mcdata-signalling 파트가 없으면 false. */
bool parse(const std::string& contentType, const std::string& body, SdsMessage& out);

// 유틸(시험용 공개)
std::string base64Encode(const std::string& raw);
std::string base64Decode(const std::string& b64);
std::string hexEncode(const std::string& raw);
std::string hexDecode(const std::string& hex);
std::string sdsSignallingTlv(const std::string& convId, const std::string& msgId, bool requestDelivery, long timeSec);
std::string sdsPayloadTlv(const std::string& text);

}  // namespace mcdata
}  // namespace cimsue
