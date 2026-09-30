// libcimsue 내부 — MCVideo 호 제어 경계 코덱 (TS 24.281 등록·affiliation·그룹 호 · TS 24.581 §14 제어 채널 SDP).
//
// 순수 함수만 둔다(엔진 부팅 없이 시험). 계약 골든 = tests/fixtures/mcvideo/sip/ (K3 SIP · K4 SDP — mcvideo.md §1.4), 서버 짝 =
// csp/McVideoInfo.h. MCVideo 는 MCPTT 의 확장이 아니라 나란한 MC 서비스라 ICSI·특성 태그·info 본문·제어 채널을 따로 둔다
// (TS 23.280 §5.2.5, TS 24.281 §7.1). fmtp 이름은 생성 헤더 tc_defs.h(정본 mcvideo_tc_defs.yaml).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cimsue {
namespace mcvideo {

constexpr const char* kIcsi = "urn:urn-7:3gpp-service.ims.icsi.mcvideo";          // TS 24.281 Annex E.2.1
constexpr const char* kIcsiEnc = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcvideo";   // 특성 태그 값 표기(RFC 3840 — 퍼센트 인코딩)
constexpr const char* kFeatureTag = "+g.3gpp.mcvideo";                             // Annex D.2
constexpr const char* kCtInfo = "application/vnd.3gpp.mcvideo-info+xml";           // Annex F.1
constexpr const char* kNsInfo = "urn:3gpp:ns:mcvideoInfo:1.0";
constexpr const char* kNsPresInfo = "urn:3gpp:ns:mcvideoPresInfo:1.0";             // §8.3.1.2
constexpr const char* kCtPidf = "application/pidf+xml";
constexpr const char* kAudioInfo = "audio component of MCVideo";                   // §6.2.1 2)c) · §6.2.2 3)c)
constexpr const char* kVideoInfo = "video component of MCVideo";                   // §6.2.1 3)d) · §6.2.2 4)c)
/** affiliation PUBLISH Expires — 관심 그룹이 하나라도 있으면 2^32-1, 없으면 0(§8.2.1.2 4)·5)). */
constexpr const char* kAffiliationExpires = "4294967295";

/** MCVideo 요청·응답의 Contact 특성 태그 — `;+g.3gpp.mcvideo;+g.3gpp.icsi-ref="…mcvideo"`(§9.2.1.2.1.1 · §6.2.3.1.1 3)·4)). */
std::string contactFeatureParams();
/** Accept-Contact 두 값(require;explicit — §9.2.1.2.1.1 · RFC 3841). */
std::string acceptContactFeature();
std::string acceptContactIcsi();

/** mcvideo-info 본문(Annex F.1) — 빈 값은 싣지 않는다. 요소 순서 = mcvideo-ParamsType sequence(access-token → session-type →
 *  request-uri → calling-user-id → called-party-id → calling-group-id → … → client-id). URI 값은 `<mcvideoURI>`, 문자열은
 *  `<mcvideoString>` 자식(암호화하지 않은 type="Normal"). */
struct InfoParams {
    std::string accessToken;
    std::string sessionType;          // chat | prearranged
    std::string requestUri;           // 그룹 호 = 대상 그룹 · affiliation = 자기 MCVideo ID
    std::string callingUserId;
    std::string calledPartyId;
    std::string callingGroupId;
    std::string clientId;
};
std::string info(const InfoParams& p);

/** 수신 SIP 원문(또는 본문)의 mcvideo-info — mcvideo-info 파트만 본다(mcptt-info 와 겹치는 session-type·지시자를 섞지 않게). */
struct InfoRx {
    bool present = false;
    std::string sessionType, requestUri, callingUserId, calledPartyId, callingGroupId, clientId;
    int emergency = 0, imminentPeril = 0;   // 1 true · -1 false · 0 없음
};
InfoRx parseInfo(const std::string& wholeOrBody);

/** affiliation PUBLISH 의 pidf(§8.2.1.2 6) · §8.3.1) — entity = MCVideo ID, tuple id = MCVideo client ID, 관심 그룹 **전부**를
 *  `<mcvideoPI10:affiliation group>` 로(status·expires 속성 없음), `<mcvideoPI10:p-id>` = 요청마다 전역 유일 값. */
std::string affiliationPidf(const std::string& entity, const std::string& clientId,
                            const std::vector<std::string>& groupUris, const std::string& pid);

/** 제어 채널 fmtp — TS 24.581 §14.2(offer) · §14.3(answer). 순서 = mc_queueing · mc_priority · mc_reception_priority ·
 *  mc_granted · mc_implicit_request · mc_audio_ssrc · mc_video_ssrc · mc_transmission_ssrc, 구분자 `;`(K4 — mcvideo.md §9). */
struct TcFmtp {
    bool present = false;             // 해석: a=fmtp:MCVideo 줄이 있었다
    bool queueing = false;
    int priority = -1;                // 1~255, <0 = 없음
    int receptionPriority = -1;
    bool granted = false;
    bool implicitRequest = false;
    bool hasAudioSsrc = false, hasVideoSsrc = false, hasTcSsrc = false;
    uint32_t audioSsrc = 0, videoSsrc = 0, tcSsrc = 0;
};
std::string fmtpString(const TcFmtp& f);
/** SDP 의 `m=application … udp MCVideo` 섹션 — 제어 채널 목적지(섹션 c= 우선, 없으면 세션 c=)와 fmtp. 섹션이 없으면 false. */
bool parseControl(const std::string& sdp, std::string& ip, int& port, TcFmtp& fmtp);
/** 주입할 제어 채널 섹션 `m=application <port> udp MCVideo` + `a=fmtp:MCVideo …`(TS 24.581 표 4.3.3.1-1 — proto 소문자 udp). */
std::string controlSdp(int port, const TcFmtp& f);

/** 영상 없는 엔진 빌드(PJMEDIA_HAS_VIDEO 0 — Linux 헤드리스·Windows 1차)의 m=video 자리 — port 0(RFC 3264 §5.1 «제안하되 쓰지 않는
 *  스트림»). MCVideo offer 는 m=video 를 빼지 않는다(§6.2.1 3)) — 음성·전송 제어는 그대로 협상된다. */
constexpr const char* kVideoPlaceholderSdp = "m=video 0 RTP/AVP 97\r\na=rtpmap:97 H264/90000";

/** SDP 가 MCVideo 호 SDP 인가 — `udp MCVideo` 제어 채널 m-line 이 있다. */
bool isMcVideoSdp(const std::string& sdp);
/** `i=` 성분 표시를 m=audio·m=video 바로 뒤에 넣는다(RFC 4566 순서 — i= 는 m= 다음 줄). 이미 있으면 그대로. MCVideo SDP 가 아니면
 *  그대로. pjmedia SDP 는 미디어 i= 를 담지 못해(파서가 버린다) 전송 직전에 넣는다. */
std::string withMediaInfo(const std::string& sdp);

/** multipart 본문 텍스트(pjsip 인쇄본 — 파트마다 Content-Type·Content-Length)에서 application/sdp 파트에 withMediaInfo 를 적용하고 그 파트의
 *  Content-Length 를 새 길이로 고친다. 구분자는 본문의 첫 `--` 줄. 다른 파트·서문·끝 구분자는 그대로. */
std::string withMediaInfoMultipart(const std::string& text);

/** 계정 Contact(`"이름" <sip:…>;파라미터`)에서 URI 부분 `<…>` 만 — 서비스 호가 자기 특성 태그를 붙일 바탕. 꺾쇠가 없으면 전체. */
std::string contactUriPart(const std::string& contact);

/** `+g.3gpp.icsi-ref="…"` 파라미터를 뺀 파라미터 문자열과 그 목록 값(쉼표 구분, 퍼센트 표기 그대로). */
std::string withoutIcsiRef(const std::string& params, std::vector<std::string>* icsis = nullptr);

}  // namespace mcvideo
}  // namespace cimsue
