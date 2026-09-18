// McDataSds — libcsim MCData SDS 코덱(TS 24.282 §15 + Annex D/E) — SDK `sdk/core/src/mcdata/sds_codec` 와 같은 와이어 형식
//   (앱·CSP McDataCodec 과 호환). SIP MESSAGE 본문 = multipart/mixed 3파트: mcdata-info+xml(request-type group-sds|one-to-one-sds +
//   request-uri) · mcdata-signalling(SDS SIGNALLING PAYLOAD 0x01 / SDS NOTIFICATION 0x05, TLV base64) · mcdata-payload(DATA PAYLOAD 0x03, TEXT).
//   libcsim 은 libcimsue 를 링크하지 않으므로 코덱을 따로 둔다(mcdata_messaging.md §3 가 정본, 두 구현은 같은 시험 벡터로 대조 — csim_sds_test).
#ifndef _CSIM_MCDATA_SDS_H_
#define _CSIM_MCDATA_SDS_H_

#include <cstdint>
#include <string>

namespace csim_mcdata {

constexpr int kMsgSdsSignalling = 0x01;
constexpr int kMsgFdSignalling = 0x02;
constexpr int kMsgDataPayload = 0x03;
constexpr int kMsgSdsNotification = 0x05;
constexpr int kDispReqDelivery = 0x01;
constexpr int kNotifDelivered = 0x02;
constexpr const char* kCtInfo = "application/vnd.3gpp.mcdata-info+xml";
constexpr const char* kCtSignalling = "application/vnd.3gpp.mcdata-signalling";
constexpr const char* kCtPayload = "application/vnd.3gpp.mcdata-payload";

struct SdsMsg {
    std::string requestType;          // group-sds | one-to-one-sds (mcdata-info)
    std::string requestUri;           // mcdata-info request-uri (그룹 = tel:<gid>, 1:1 = tel:<상대>)
    std::string convId, msgId;        // UUID hex32
    int64_t timeSec = 0;
    int dispositionReq = 0;           // 0 없음 / 1 delivery / 2 read / 3 both
    std::string text;
    bool notification = false;        // SDS NOTIFICATION
    int notifType = 0;                // 2 delivered / 3 read …
    bool fd = false;                  // FD SIGNALLING(파일 URL) — 여기서는 관측만
};

struct Body { std::string contentType; std::string body; };

/** 그룹 conversation ID — 그룹당 결정적 UUID(Java UUID.nameUUIDFromBytes("cims-mcdata:<gid>") 호환, 단말·CSP 와 같은 값). */
std::string conversationIdOf(const std::string& groupId);
/** 1:1 conversation ID — 사용자 쌍당 결정적("cims-mcdata:1to1:<a>:<b>", 쌍 정렬 — 양쪽 단말 동일). */
std::string conversationIdOneToOne(const std::string& a, const std::string& b);
/** 새 message ID — 랜덤 UUID v4 hex32. */
std::string newMessageId();

/** 그룹 SDS 본문(request-type group-sds, request-uri tel:<gid>). */
Body buildGroupSds(const std::string& groupId, const std::string& text, const std::string& convId, const std::string& msgId,
                   bool requestDelivery, int64_t timeSec);
/** 1:1 SDS 본문(request-type one-to-one-sds, request-uri tel:<상대>). */
Body buildOneToOneSds(const std::string& toUser, const std::string& text, const std::string& convId, const std::string& msgId,
                      bool requestDelivery, int64_t timeSec);
/** raw TLV — media plane(MSRP SEND 본문)용. C-plane MESSAGE 는 같은 TLV 를 base64 파트로 싣는다. */
std::string signallingTlv(const std::string& convId, const std::string& msgId, bool requestDelivery, int64_t timeSec);
std::string payloadTlv(const std::string& text);
/** SDS NOTIFICATION(전달/읽음 통지) 본문 — 원 발신자에게 1:1 MESSAGE 로. */
Body buildNotification(const std::string& convId, const std::string& msgId, int notifType, int64_t timeSec);

/** multipart/mixed MCData 본문 파싱 — mcdata-signalling 파트가 없으면 false(MCData 가 아닌 MESSAGE). */
bool parse(const std::string& contentType, const std::string& body, SdsMsg& out);

std::string base64Encode(const std::string& raw);
std::string base64Decode(const std::string& b64);
std::string hexEncode(const std::string& raw);

}  // namespace csim_mcdata

#endif
