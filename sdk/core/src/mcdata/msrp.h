// libcimsue 내부 — MCData SDS **media plane**(TS 24.282 §9.2.3, RFC 4975 MSRP) 단말 쪽. 상대 = cmdp(서버 media storage,
// mcdata_messaging.md §4.7) — 서버는 늘 passive 라 단말이 out-connect 한다(발신 a=setup:actpass, 수신 a=setup:active).
//   · 발신: SDS SIGNALLING PAYLOAD·DATA PAYLOAD 를 SEND 2건(raw TLV — 두 번째에 Success-Report, 큰 본문은 청크 stop-and-wait) → 200/REPORT
//   · 수신: bodiless SEND(연결 바인딩) → SEND 수신·200(청크 조립) → 본문(multipart/mixed 또는 파트별 SEND)
// 원천 = android/ptt-client mcdata/msrp(MsrpCodec·MsrpSession)·libcsim cspsim/McDataMsrp — 프레이밍·절차가 같다.
// 릴레이(RFC 4976)·MSRPS 는 cmdp 와 같이 미지원. 전송은 net::Stream(평문 TCP) — Linux·Android·Windows 공용.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace cimsue {
namespace msrp {

constexpr const char* kAcceptTypes =
    "multipart/mixed application/vnd.3gpp.mcdata-signalling application/vnd.3gpp.mcdata-payload";
constexpr const char* kIcsiMcDataSds = "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds";
/** a=path·m=message 에 광고하는 명목 포트 — 서버가 passive 라 이 포트로 붙어 오지 않는다. */
constexpr int kNominalPort = 2855;
/** 청크 크기(stop-and-wait) — 원천 앱과 같다. */
constexpr size_t kChunkBytes = 16 * 1024;

/** 수신 프레임 — 요청(SEND/REPORT)이면 method, 응답이면 status. headers 키는 소문자. */
struct Frame {
    bool request = true;
    std::string tid, method;
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    char flag = '$';                  // end-line 플래그 — $ 끝 · + 계속 · # 중단(RFC 4975 §7.1)
    std::string header(const char* name) const;
};

/** 버퍼 앞의 완성 프레임 하나를 잘라낸다(end-line `-------<tid><flag>`). 없으면 false(버퍼 유지). */
bool extractFrame(std::string& buf, Frame& out);
std::string newTransId();
std::string newSessionId();
/** SEND 한 청크 — body 는 전체 메시지의 [start, start+chunk) 이고 Byte-Range 는 1 기준(RFC 4975 §7.1.1). contentType 이 비면
 *  bodiless(바인딩). total 은 전체 크기. flag = '+'(계속) 또는 '$'(끝). */
std::string buildSend(const std::string& tid, const std::string& toPath, const std::string& fromPath, const std::string& msgId,
                      const std::string& contentType, const std::string& chunk, size_t start, size_t total, bool successReport,
                      char flag);
std::string buildResponse(const std::string& tid, int code, const std::string& toPath, const std::string& fromPath);
/** msrp://host:port/session;tcp → host·port. 여러 URI(릴레이)면 첫 URI. */
bool parsePath(const std::string& path, std::string& host, int& port);
std::string localPath(const std::string& ip, const std::string& session);
/** SDP m=message 섹션(TS 24.282 §9.2.3 — m=message TCP/MSRP *, a=path·accept-types·setup·방향). */
std::string sdpSection(const std::string& localPath, const char* setup, const char* direction);
/** SDP 의 m=message 섹션 a=path(첫 URI). 없으면 빈 문자열. */
std::string pathOfSdp(const std::string& sdp);
/** SDP 의 첫 c=IN IP4 주소. */
std::string connAddrOf(const std::string& sdp);
/** SDP 의 m=audio 섹션 방향을 inactive 로 — 수신 배포 INVITE 의 더미 오디오(서버 계약: 포트 9 inactive 와 짝). */
std::string audioInactive(const std::string& sdp);

/** mcdata-info(TS 24.282 Annex E) 요소 안의 mcdataURI — <elem>…<mcdataURI>v</mcdataURI>. 없으면 빈 문자열. */
std::string mcdataInfoUri(const std::string& body, const char* elem);

/** 발신 절차 — serverPath 에 붙어 signalling·payload TLV 를 보내고 최종 결과를 SIP 풍 코드로 돌려준다(200 = 저장소가 받음,
 *  408 = 응답 없음, 503 = 연결 실패, 그 밖 = MSRP 응답 코드). cancel 이 서면 가능한 빨리 그만둔다. progress(sent,total) 는 청크마다. */
int sendSds(const std::string& serverPath, const std::string& localPath, const std::string& signallingTlv,
            const std::string& payloadTlv, int timeoutSec, const std::atomic<bool>& cancel,
            const std::function<void(size_t, size_t)>& progress, std::string& err);

/** 수신 절차 — serverPath 에 붙어 바인딩 SEND 를 보내고 메시지 하나를 받는다. 성공이면 contentType·body 는 mcdata::parse 에 넘길
 *  본문(multipart/mixed 한 건이거나, 파트별 SEND 두 건이면 합성한 multipart). */
bool receiveSds(const std::string& serverPath, const std::string& localPath, int timeoutSec, const std::atomic<bool>& cancel,
                std::string& contentType, std::string& body, std::string& err);

}  // namespace msrp
}  // namespace cimsue
