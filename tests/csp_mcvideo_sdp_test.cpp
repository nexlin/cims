// csp_mcvideo_sdp_test.cpp — MCVideo 그룹 호의 SDP·헤더 읽기
//   부품(csp/McVideoSdp.h — mcvideo.md §1.4 K4 · §5.2.1) 단위시험. K3
//   골든(tests/fixtures/mcvideo/sip/)의 SDP 를 psip SDP 파서로 읽어
//   McVideoCallService 가 JOIN ② 선언에 쓰는 값을 확인하고, 골든에 없는
//   모양(a=ssrc·`*` rtcp-fb·fmtp 없음·거절된 제어 채널·AMR-WB+)은 합성 SDP 로
//   본다. 레포 루트에서 실행한다. 전송 제어 채널 = m=application udp
//   MCVideo(fmtp 선택) · 음성 AMR-WB PT · 영상 H264 PT · a=ssrc(RFC 5576) ·
//   키프레임 요청 (nack pli · ccm fir — RFC 4585 §4.2 · RFC 5104 §7.1) ·
//   Session-Expires refresher 제거(TS 24.281 §6.3.3.1.2 6))
#include <cstdio>
#include <fstream>
#include <sstream>

#include "McVideoSdp.h"
#include "SdpMessage.h"

static int g_fail = 0;
#define CK(name, cond)                                                         \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL %s\n", name);                                               \
      ++g_fail;                                                                \
    } else {                                                                   \
      printf("ok   %s\n", name);                                               \
    }                                                                          \
  } while (0)

static const char *kDir = "tests/fixtures/mcvideo/sip/";

static std::string Slurp(const std::string &strPath) {
  std::ifstream f(strPath, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// 골든 메시지의 SDP (multipart 면 application/sdp 파트)
static std::string GoldenSdp(const char *pszName) {
  const std::string d = Slurp(std::string(kDir) + pszName);
  const size_t e = d.find("\r\n\r\n");
  if (e == std::string::npos)
    return "";
  const std::string strHead = d.substr(0, e);
  std::string strCtype;
  const size_t c = strHead.find("\r\nContent-Type: ");
  if (c != std::string::npos) {
    const size_t v = c + 16;
    strCtype = strHead.substr(v, strHead.find("\r\n", v) - v);
  }
  return McVideoBodyPart(d.substr(e + 4), strCtype, "sdp") + "\r\n";
}

static bool ParseSdp(const std::string &strSdp, CSdpMessage &clsSdp) {
  return clsSdp.Parse(strSdp.c_str(), (int)strSdp.size()) > 0;
}

static std::string Crlf(const char *psz) {
  std::string s;
  for (const char *p = psz; *p; ++p) {
    if (*p == '\n')
      s += '\r';
    s += *p;
  }
  return s;
}

int main() {
  // ── 골든 05 — prearranged 개시 offer (암묵 요청) ──
  {
    CSdpMessage clsSdp;
    CK("05 SDP 파싱",
       ParseSdp(GoldenSdp("05_prearranged_initiate_invite.txt"), clsSdp));
    int iPort = 0;
    CMcVideoFmtp f;
    CK("05 전송 제어 채널 있음",
       McvControlChannel(clsSdp.m_clsMediaList, iPort, f));
    CK("05 제어 채널 포트 = m=application 포트(40014)", iPort == 40014);
    CK("05 fmtp 해석 — mc_priority 5 · mc_granted · mc_implicit_request · tc "
       "ssrc",
       f.bPresent && f.iPriority == 5 && f.bGranted && f.bImplicit &&
           f.bHasTcSsrc && f.uTcSsrc == 305419897u);
    unsigned int uSsrc = 1;
    int iPt = 0;
    McvMediaSsrcPt(clsSdp.m_clsMediaList, "audio", "AMR-WB", uSsrc, iPt);
    CK("05 음성 AMR-WB PT 96 · a=ssrc 없음(0)", iPt == 96 && uSsrc == 0);
    McvMediaSsrcPt(clsSdp.m_clsMediaList, "video", "H264", uSsrc, iPt);
    CK("05 영상 H264 PT 97", iPt == 97);
    CK("05 키프레임 요청 = PLI + FIR",
       McvVideoFeedback(clsSdp.m_clsMediaList, 97) == (kMcvFbPli | kMcvFbFir));
    CK("05 다른 PT 의 rtcp-fb 는 세지 않는다",
       McvVideoFeedback(clsSdp.m_clsMediaList, 98) == 0);
  }
  // ── 골든 04 — 서버 answer(chat 합류 200 OK) ──
  {
    CSdpMessage clsSdp;
    CK("04 SDP 파싱", ParseSdp(GoldenSdp("04_chat_join_200.txt"), clsSdp));
    int iPort = 0;
    CMcVideoFmtp f;
    CK("04 제어 채널 58000 · mc_queueing 되돌림 · tc ssrc",
       McvControlChannel(clsSdp.m_clsMediaList, iPort, f) && iPort == 58000 &&
           f.bQueueing && f.bHasTcSsrc && f.uTcSsrc == 2863311530u);
  }
  // ── 합성 — 골든에 없는 모양 ──
  {
    CSdpMessage clsSdp;
    CK("합성 SDP 파싱",
       ParseSdp(
           Crlf("v=0\no=- 1 1 IN IP4 10.0.0.1\ns=-\nc=IN IP4 10.0.0.1\nt=0 0\n"
                "m=audio 40000 RTP/AVP 104 96\na=rtpmap:104 "
                "AMR-WB+/72000\na=rtpmap:96 AMR-WB/16000\n"
                "a=ssrc:305419896 cname:ue-a\na=ssrc:777 cname:other\n"
                "m=video 40002 RTP/AVP 100\na=rtpmap:100 "
                "H264/90000\na=ssrc:2596069104 cname:ue-a\n"
                "a=rtcp-fb:* nack pli\na=rtcp-fb:100 nack\na=rtcp-fb:100 "
                "trr-int 100\n"
                "m=application 40004 udp MCVideo\n"),
           clsSdp));
    unsigned int uSsrc = 0;
    int iPt = 0;
    McvMediaSsrcPt(clsSdp.m_clsMediaList, "audio", "AMR-WB", uSsrc, iPt);
    CK("AMR-WB+ 는 AMR-WB 로 보지 않는다(PT 96) · 첫 a=ssrc",
       iPt == 96 && uSsrc == 305419896u);
    McvMediaSsrcPt(clsSdp.m_clsMediaList, "video", "H264", uSsrc, iPt);
    CK("영상 PT 100 · a=ssrc", iPt == 100 && uSsrc == 2596069104u);
    CK("`*` nack pli = PLI 만(일반 nack·trr-int 제외)",
       McvVideoFeedback(clsSdp.m_clsMediaList, 100) == kMcvFbPli);
    int iPort = 0;
    CMcVideoFmtp f;
    CK("fmtp 없는 제어 채널 = 있음 · bPresent false",
       McvControlChannel(clsSdp.m_clsMediaList, iPort, f) && iPort == 40004 &&
           !f.bPresent);
  }
  {
    CSdpMessage clsSdp;
    ParseSdp(
        Crlf("v=0\no=- 1 1 IN IP4 10.0.0.1\ns=-\nc=IN IP4 10.0.0.1\nt=0 0\n"
             "m=audio 40000 RTP/AVP 96\na=rtpmap:96 AMR-WB/16000\n"
             "m=video 0 RTP/AVP 97\n"
             "m=application 0 udp MCVideo\n"
             "m=application 40006 UDP MCPTT\n"),
        clsSdp);
    int iPort = -1;
    CMcVideoFmtp f;
    CK("거절된 제어 채널(port 0) = 있음 · 포트 0 (호출자가 488)",
       McvControlChannel(clsSdp.m_clsMediaList, iPort, f) && iPort == 0);
    unsigned int uSsrc = 0;
    int iPt = 0;
    McvMediaSsrcPt(clsSdp.m_clsMediaList, "video", "H264", uSsrc, iPt);
    CK("m=video 0 에 rtpmap 없음 → 영상 PT 0", iPt == 0);
    CK("영상 rtcp-fb 없음 → 0",
       McvVideoFeedback(clsSdp.m_clsMediaList, 97) == 0);
  }
  {
    CSdpMessage clsSdp;
    ParseSdp(
        Crlf("v=0\no=- 1 1 IN IP4 10.0.0.1\ns=-\nc=IN IP4 10.0.0.1\nt=0 0\n"
             "m=audio 40000 RTP/AVP 96\na=rtpmap:96 AMR-WB/16000\n"
             "m=application 40006 UDP MCPTT\na=fmtp:MCPTT mc_queueing\n"),
        clsSdp);
    int iPort = 0;
    CMcVideoFmtp f;
    CK("MCPTT floor 채널은 MCVideo 제어 채널이 아니다",
       !McvControlChannel(clsSdp.m_clsMediaList, iPort, f));
  }
  // ── Session-Expires refresher 제거 (TS 24.281 §6.3.3.1.2 6)) ──
  {
    SIP_HEADER_LIST clsHeaders;
    CSipHeader a, b, c;
    a.m_strName = "Session-Expires";
    a.m_strValue = "1800;refresher=uac";
    b.m_strName = "Min-SE";
    b.m_strValue = "90";
    c.m_strName = "session-expires";
    c.m_strValue = "900;refresher=uas;x=1";
    clsHeaders.push_back(a);
    clsHeaders.push_back(b);
    clsHeaders.push_back(c);
    McvStripSessionRefresher(clsHeaders);
    auto it = clsHeaders.begin();
    CK("Session-Expires: 1800 (refresher 없음)", it->m_strValue == "1800");
    ++it;
    CK("다른 헤더는 그대로", it->m_strValue == "90");
    ++it;
    CK("뒤 파라미터는 남긴다(이름 대소문자 무시)", it->m_strValue == "900;x=1");
  }
  printf("csp_mcvideo_sdp_test: %s (%d failure%s)\n", g_fail ? "FAIL" : "PASS",
         g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
