// McDataMsrp — libcsim MCData SDS **media plane** 단말 쪽(TS 24.282 §9.2.3, RFC 4975 MSRP). cmdp(서버 media storage) 를 상대로
//   · 발신: INVITE(더미 m=audio + m=message TCP/MSRP a=sendonly a=setup:actpass) → 200 의 a=path 로 TCP 접속 → SDS SIGNALLING PAYLOAD·
//     DATA PAYLOAD 를 SEND 2건(raw TLV, Success-Report) → 200/REPORT → 서버 BYE
//   · 수신: 서버발 INVITE(mcdata-info + m=message a=sendonly) 에 200(audio inactive + m=message a=recvonly a=setup:active) → 서버 path 로
//     접속 → bodiless SEND(바인딩) → SEND 수신·200 → 본문(multipart/mixed: signalling+payload) 종단
//   프레이밍은 이 파일(청킹 없는 단일 청크 송신·다중 청크 수신, end-line `$`/`+`/`#`), 세션 절차는 SimSession(McDataMsrp 스레드).
//   릴레이(RFC 4976)·MSRPS 는 cmdp 와 같이 미지원. 참조 구현 = tests/msrp_sds_client.py(표준 단말 대역).
#ifndef _CSIM_MCDATA_MSRP_H_
#define _CSIM_MCDATA_MSRP_H_
#include <map>
#include <string>

namespace csim_msrp {

constexpr const char* kAcceptTypes = "multipart/mixed application/vnd.3gpp.mcdata-signalling application/vnd.3gpp.mcdata-payload";
constexpr const char* kIcsiMcDataSds = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds";
constexpr int kLocalPort = 2855;   // a=path 에 광고하는 명목 포트(우리는 항상 out-connect — 서버 passive)

/** 수신 프레임 — 요청(SEND/REPORT)이면 method, 응답이면 status. headers 키는 소문자. */
struct Frame {
    bool request = true;
    std::string tid;
    std::string method;   // SEND | REPORT
    int status = 0;       // 응답
    std::map<std::string, std::string> headers;
    std::string body;
    char flag = '$';      // end-line 플래그 — $ 끝 · + 계속 · # 중단
    std::string header(const char* name) const;
};

/** 스트림 버퍼 앞의 완성 프레임 하나를 잘라낸다(RFC 4975 §7 — end-line `-------<tid><flag>`). 없으면 false(버퍼 유지). */
bool extractFrame(std::string& buf, Frame& out);
std::string newTransId();
std::string newSessionId();
/** SEND 프레임(단일 청크 Byte-Range 1-N/N). contentType 이 비면 bodiless(바인딩). */
std::string buildSend(const std::string& tid, const std::string& toPath, const std::string& fromPath, const std::string& msgId,
                      const std::string& contentType, const std::string& body, bool successReport, char flag = '$');
std::string buildResponse(const std::string& tid, int code, const std::string& toPath, const std::string& fromPath);
/** msrp://host:port/session;tcp → host·port. 다중 URI(릴레이)면 첫 URI. */
bool parsePath(const std::string& path, std::string& host, int& port);
std::string localPath(const std::string& ip, const std::string& session);

/** 동기 TCP 클라이언트 — 전용 스레드에서 쓴다(스택 콜백 스레드를 막지 않는다). */
class Client {
public:
    ~Client() { Close(); }
    bool Connect(const std::string& host, int port, int timeoutMs);
    void Close();
    bool Send(const std::string& data);
    /** 완성 프레임 하나를 timeoutMs 안에 받는다(버퍼 잔여분 우선). */
    bool RecvFrame(Frame& out, int timeoutMs);
    bool IsOpen() const { return m_fd >= 0; }
private:
    int m_fd = -1;
    std::string m_buf;
};

}  // namespace csim_msrp
#endif
