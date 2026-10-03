// csp_mcptt_request_test.cpp — 규격형 MCPTT·MCData 요청의 대상·floor 판정
//   부품 단위시험(WP S17 — docs/dev/conformance_gap_plan.md §7 «요청 형식
//   계약»). 골든(tests/fixtures/mcptt/sip/)을 읽어 CSP 가 정하는 값을 본다.
//   레포 루트에서 실행한다.
//   · 개별 호(csp/McpttInfo.h McpttPrivateCalledParty — TS 24.379
//     §11.1.1.2.1.1 9) · §11.1.1.3.1.1 8)·9)): 착신자 = resource-lists 의
//     entry 하나, 없거나 둘 이상이면 145
//   · floor 유무(csp/McpttSdp.h McpttFloorChannelOffered — §11.1.2.2 1) ·
//     §11.1.2.3.1): offer 의 m=application … MCPTT, 포트 0 은 제안 아님,
//     fmtp mc_no_floor_ctrl 은 floor 유무로 읽지 않는다
//   · MCData 대상(csp/McDataCodec.cpp McDataRequestTarget — TS 24.282
//     §9.2.2.3.1 4)·5) · §9.2.2.4.2 5)·6) · §10.2.4.4.2 10)·12) · §9.2.3.3.3
//     4)): request-type 이 절차를 가른다 — 그룹 = <mcdata-request-uri>, 1:1 =
//     resource-lists, 없으면 403 204·205 / request-type 없음 404 142
//   · 엄격 검사(WP S18 — csp/McpttInfo.h McpttAcceptContactOk ·
//     McDataSdsAcceptContactOk · McEmergencyAlertServiceOf, csp/McDataCodec.cpp
//     McDataMissingBodies): 그룹 호·미디어 평면 SDS Accept-Contact 403 · MCData
//     MIME 본문 199 · 경보는 mcptt-info 파트만
#include <cstdio>
#include <fstream>
#include <sstream>

#include "McDataCodec.h"
#include "McpttInfo.h"
#include "McpttSdp.h"
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

static const char *kDir = "tests/fixtures/mcptt/sip/";

static std::string Slurp(const std::string &strPath) {
  std::ifstream f(strPath, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Golden {
  std::string strStart;  // 시작 줄
  std::string strCtype;  // Content-Type 헤더 값(파라미터 포함)
  std::string strBody;
  std::string strHead;
};

static Golden Load(const char *pszName) {
  Golden g;
  const std::string d = Slurp(std::string(kDir) + pszName);
  const size_t e = d.find("\r\n\r\n");
  if (e == std::string::npos)
    return g;
  g.strHead = d.substr(0, e);
  g.strStart = g.strHead.substr(0, g.strHead.find("\r\n"));
  const size_t c = g.strHead.find("\r\nContent-Type: ");
  if (c != std::string::npos) {
    const size_t v = c + 16;
    g.strCtype = g.strHead.substr(v, g.strHead.find("\r\n", v) - v);
  }
  g.strBody = d.substr(e + 4);
  return g;
}

static bool OfferFloor(const Golden &g, bool &bFloor) {
  const std::string strSdp = McBodyPart(g.strBody, g.strCtype, "sdp") + "\r\n";
  CSdpMessage clsSdp;
  if (clsSdp.Parse(strSdp.c_str(), (int)strSdp.size()) <= 0)
    return false;
  bFloor = McpttFloorChannelOffered(clsSdp.m_clsMediaList);
  return true;
}

static bool SynthFloor(const char *pszApp) {
  std::string s = "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\ns=-\r\nc=IN IP4 "
                  "10.0.0.1\r\nt=0 0\r\nm=audio 4000 RTP/AVP 96\r\na=rtpmap:96 "
                  "AMR-WB/16000\r\n";
  s += pszApp;
  CSdpMessage clsSdp;
  clsSdp.Parse(s.c_str(), (int)s.size());
  return McpttFloorChannelOffered(clsSdp.m_clsMediaList);
}

/** 골든 헤더의 Accept-Contact 값들을 ',' 로 잇는다(CSP JoinedHeaderValues 와 같은 모양). */
static std::string Accept(const Golden &g) {
  std::string s;
  size_t p = 0;
  while ((p = g.strHead.find("\r\nAccept-Contact: ", p)) != std::string::npos) {
    const size_t v = p + 18;
    s += g.strHead.substr(v, g.strHead.find("\r\n", v) - v) + ",";
    p = v;
  }
  return s;
}

static int Target(const Golden &g, bool &bGroup, std::string &strId,
                  int &iWarn, CMcDataSdsInfo &clsInfo) {
  if (!McDataParseBody(g.strCtype, g.strBody, clsInfo))
    return -1;
  return McDataRequestTarget(clsInfo, bGroup, strId, &iWarn);
}

int main() {
  // ── 개별 호 (TS 24.379) ──
  {
    const Golden g = Load("01_private_invite.txt");
    CK("01 Request-URI = 참여 기능 PSI",
       g.strStart == "INVITE sip:mcptt_psi@ptt.cims.example.kr SIP/2.0");
    std::string strCallee;
    CK("01 착신자 = resource-lists entry",
       McpttPrivateCalledParty(g.strBody, strCallee) &&
           strCallee == "+82510002002");
    CK("01 mcptt-info session-type private",
       ParseMcpttInfo(g.strBody).strSessionType == "private");
    bool bFloor = false;
    CK("01 SDP 파싱", OfferFloor(g, bFloor));
    CK("01 발언권 제어 채널 있음 → floor 있는 개별 호", bFloor);
  }
  {
    const Golden g = Load("02_private_full_duplex_invite.txt");
    std::string strCallee;
    CK("02 착신자 = resource-lists entry",
       McpttPrivateCalledParty(g.strBody, strCallee) &&
           strCallee == "+82510002002");
    bool bFloor = true;
    CK("02 SDP 파싱", OfferFloor(g, bFloor));
    CK("02 m=application 없음 → floor 없는 개별 호(§11.1.2.3.1)", !bFloor);
  }
  {
    const Golden g = Load("03_private_reject_403_145.txt");
    CK("03 403 + Warning 145",
       g.strStart == "SIP/2.0 403 Forbidden" &&
           g.strHead.find("Warning: 399 ptt.cims.example.kr \"145 unable to "
                          "determine called party\"") != std::string::npos);
  }
  {
    std::string strCallee = "x";
    // 옛 형식 — 대상을 <mcptt-request-uri>·Request-URI 에 싣고 resource-lists
    // 가 없다(결정 D10 — 받지 않는다)
    const std::string strOld =
        "--b\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n"
        "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params>"
        "<session-type>private</session-type><mcptt-request-uri "
        "type=\"Normal\"><mcpttURI>tel:+82510002002</mcpttURI></"
        "mcptt-request-uri></mcptt-Params></mcpttinfo>\r\n--b--\r\n";
    CK("resource-lists 없음 → 착신자 못 정함(145)",
       !McpttPrivateCalledParty(strOld, strCallee) && strCallee.empty());
    const std::string strTwo =
        "--b\r\nContent-Type: application/resource-lists+xml\r\n\r\n"
        "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\">"
        "<list><entry uri=\"tel:+82510002002\"/><entry "
        "uri=\"sip:+82510002003@d\"/></list></resource-lists>\r\n--b--\r\n";
    CK("entry 둘 → 착신자 못 정함(145, §11.1.1.3.1.1 9))",
       !McpttPrivateCalledParty(strTwo, strCallee));
    CK("SDP 본문만(resource-lists 없음) → 145",
       !McpttPrivateCalledParty("v=0\r\nm=audio 4000 RTP/AVP 96\r\n",
                                strCallee));
  }
  CK("UDP MCPTT(대문자 proto)도 발언권 제어 채널",
     SynthFloor("m=application 4002 UDP MCPTT\r\na=floorid:0 mstrm:audio\r\n"));
  CK("포트 0 m=application 은 제안이 아니다",
     !SynthFloor("m=application 0 udp MCPTT\r\n"));
  CK("m=application 없음 + 음성만 → floor 없음", !SynthFloor(""));
  CK("MCVideo 제어 채널은 MCPTT 발언권 채널이 아니다",
     !SynthFloor("m=application 4002 udp MCVideo\r\n"));
  CK("mc_no_floor_ctrl 이 있어도 채널이 있으면 floor 있음(on-demand)",
     SynthFloor("m=application 4002 udp MCPTT\r\na=fmtp:MCPTT "
                "mc_queueing;mc_no_floor_ctrl\r\n"));

  // ── conference 구독 (TS 24.379 §10.1.3.2·§10.1.3.3) ──
  {
    const Golden g = Load("10_conference_subscribe.txt");
    CK("10 Request-URI = 세션 식별자(그룹 AoR + gr)",
       g.strStart.rfind("SUBSCRIBE sip:g101@", 0) == 0 &&
           g.strStart.find(";gr=1790775600123456-7") != std::string::npos);
    CK("10 mcptt-info <mcptt-request-uri> = 그룹",
       McpttBareId(ParseMcpttInfo(g.strBody).strRequestUri) == "g101");
    CK("10 Expires 4294967295 · Accept conference-info",
       g.strHead.find("Expires: 4294967295") != std::string::npos &&
           g.strHead.find("Accept: application/conference-info+xml") !=
               std::string::npos);
    const Golden r = Load("11_conference_reject_404_137.txt");
    CK("11 404 + Warning 137",
       r.strStart == "SIP/2.0 404 Not Found" &&
           r.strHead.find("\"137 the indicated group call does not exist\"") !=
               std::string::npos);
  }

  // ── 재합류 (TS 24.379 §10.1.1.2.4.1 · §10.1.1.4.5.1) ──
  {
    const Golden g = Load("12_rejoin_invite.txt");
    CK("12 Request-URI = 세션 식별자(그룹 AoR + gr)",
       g.strStart.rfind("INVITE sip:g101@", 0) == 0 &&
           g.strStart.find(";gr=1790775600123456-7") != std::string::npos);
    const CMcpttInfo mi =
        ParseMcpttInfo(McBodyPart(g.strBody, g.strCtype, "vnd.3gpp.mcptt-info+xml"));
    CK("12 mcptt-info prearranged · <mcptt-request-uri> = 그룹 · client ID",
       mi.strSessionType == "prearranged" &&
           McpttBareId(mi.strRequestUri) == "g101" && !mi.strClientId.empty());
    bool bFloor = false;
    CK("12 SDP offer 에 발언권 제어 채널", OfferFloor(g, bFloor) && bFloor);
    const Golden r = Load("13_rejoin_reject_404.txt");
    CK("13 404 · Warning 없음",
       r.strStart == "SIP/2.0 404 Not Found" &&
           r.strHead.find("Warning:") == std::string::npos);
  }

  // ── MCData (TS 24.282) ──
  {
    const Golden g = Load("04_sds_group_message.txt");
    CK("04 Request-URI = 참여 MCData 기능 PSI",
       g.strStart == "MESSAGE sip:mcdata_psi@ptt.cims.example.kr SIP/2.0");
    bool bGroup = false;
    std::string strId;
    int iWarn = -1;
    CMcDataSdsInfo clsInfo;
    CK("04 group-sds → 그룹 g101",
       Target(g, bGroup, strId, iWarn, clsInfo) == 0 && bGroup &&
           strId == "g101" && iWarn == 0);
    CK("04 request-type·SDS SIGNALLING·TEXT",
       clsInfo.m_strRequestType == "group-sds" &&
           clsInfo.m_iMsgType == MCDATA_MSG_SDS_SIGNALLING &&
           clsInfo.m_strText == "현장 도착" &&
           clsInfo.m_strMsgId == "a1000000000040008000000000000001");
  }
  {
    const Golden g = Load("05_sds_one_to_one_message.txt");
    bool bGroup = true;
    std::string strId;
    int iWarn = -1;
    CMcDataSdsInfo clsInfo;
    CK("05 one-to-one-sds → resource-lists 의 MCData ID",
       Target(g, bGroup, strId, iWarn, clsInfo) == 0 && !bGroup &&
           strId == "+82510002002");
  }
  {
    const Golden g = Load("06_sds_reject_403_204.txt");
    CK("06 403 + Warning 204",
       g.strStart == "SIP/2.0 403 Forbidden" &&
           g.strHead.find("\"204 unable to determine targeted user for "
                          "one-to-one SDS\"") != std::string::npos);
  }
  {
    const Golden g = Load("07_fd_group_message.txt");
    bool bGroup = false;
    std::string strId;
    int iWarn = -1;
    CMcDataSdsInfo clsInfo;
    CK("07 group-fd → 그룹 g101",
       Target(g, bGroup, strId, iWarn, clsInfo) == 0 && bGroup &&
           strId == "g101");
    CK("07 FD SIGNALLING · FILEURL 하나",
       clsInfo.m_iMsgType == MCDATA_MSG_FD_SIGNALLING &&
           clsInfo.m_iFdPayloadCount == 1 &&
           clsInfo.m_strFileUrl ==
               "https://csc.ptt.cims.example.kr:4430/mcdata/fd/"
               "0123456789abcdef0123456789abcdef");
  }
  {
    const Golden g = Load("08_fd_one_to_one_message.txt");
    bool bGroup = true;
    std::string strId;
    int iWarn = -1;
    CMcDataSdsInfo clsInfo;
    CK("08 one-to-one-fd → resource-lists 의 MCData ID",
       Target(g, bGroup, strId, iWarn, clsInfo) == 0 && !bGroup &&
           strId == "+82510002002");
  }
  {
    const Golden g = Load("09_sds_media_group_invite.txt");
    CMcDataSdsInfo clsInfo;
    McDataParseInfo(
        McBodyPart(g.strBody, g.strCtype, "vnd.3gpp.mcdata-info+xml"),
        clsInfo);
    bool bGroup = false;
    std::string strId;
    int iWarn = -1;
    CK("09 미디어 평면 INVITE mcdata-info → group-sds g101",
       clsInfo.m_strRequestType == "group-sds" &&
           McDataRequestTarget(clsInfo, bGroup, strId, &iWarn) == 0 &&
           bGroup && strId == "g101");
  }
  {
    // 옛 형식·이 서버가 맡지 않는 요청
    CMcDataSdsInfo i;
    bool bGroup = true;
    std::string strId = "x";
    int iWarn = 0;
    i.m_strRequestType = "one-to-one-sds";
    i.m_strGroupUri = "tel:+82510002002";  // 옛 형식 — 대상을 request-uri 에
    CK("1:1 SDS resource-lists 없음 → 403 204",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 403 && iWarn == 204 &&
           strId.empty() && !bGroup);
    i.m_strRequestType = "one-to-one-fd";
    CK("1:1 FD resource-lists 없음 → 403 205",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 403 && iWarn == 205);
    i.m_strRequestType = "one-to-one-sds";
    i.m_vecListUris = {"tel:+82510002002", "tel:+82510002003"};
    CK("1:1 SDS entry 둘 → 403 204",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 403 && iWarn == 204);
    i.m_vecListUris = {"sip:+82510002003@ptt.cims.example.kr"};
    CK("1:1 SDS sip: entry → MCData ID 맨 값",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 0 &&
           strId == "+82510002003");
    i.m_strRequestType = "group-sds";
    i.m_strGroupUri.clear();
    CK("그룹 SDS <mcdata-request-uri> 없음 → 404 142",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 404 && iWarn == 142);
    i.m_strRequestType.clear();
    i.m_strGroupUri = "tel:g101";
    CK("request-type 없음 → 404 142(Request-URI 를 대상으로 읽지 않는다)",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 404 && iWarn == 142);
    i.m_strRequestType = "ad-hoc-group-sds";
    CK("ad hoc 그룹 SDS(미제공) → 404 142",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 404 && iWarn == 142);
    i.m_strRequestType = "group-sds";
    i.m_strGroupUri = "sip:g101@ptt.cims.example.kr";
    CK("그룹 = sip: URI 도 맨 값",
       McDataRequestTarget(i, bGroup, strId, &iWarn) == 0 && bGroup &&
           strId == "g101");
  }
  {
    // request-type 을 contentType 자식(<mcdataString>)으로 실은 것도 읽는다
    CMcDataSdsInfo i;
    McDataParseInfo("<mcdatainfo><mcdata-Params><request-type "
                    "type=\"Normal\"><mcdataString>group-fd</mcdataString></"
                    "request-type></mcdata-Params></mcdatainfo>",
                    i);
    CK("request-type <mcdataString> 자식", i.m_strRequestType == "group-fd");
    CMcDataSdsInfo j;
    McDataParseInfo("<mcdatainfo><mcdata-Params><request-type-x>group-sds</"
                    "request-type-x></mcdata-Params></mcdatainfo>",
                    j);
    CK("request-type 이름 경계(<request-type-x> 는 아니다)",
       j.m_strRequestType.empty());
  }

  // ── 엄격 검사 (WP S18 — 스위치 없이 늘 규격대로) ──
  {
    // 그룹 호 제어 기능 Accept-Contact (TS 24.379 §10.1.1.4.2 3) · §10.1.1.4.5.1
    // 4) · §10.1.2.4.1.1 2) · §17.4.2.2 3))
    CK("12 재합류 Accept-Contact 둘 → 통과",
       McpttAcceptContactOk(Accept(Load("12_rejoin_invite.txt"))));
    CK("01 개별 호 Accept-Contact 둘 → 통과",
       McpttAcceptContactOk(Accept(Load("01_private_invite.txt"))));
    CK("10 icsi-ref 만(g.3gpp.mcptt 없음) → 403",
       !McpttAcceptContactOk(Accept(Load("10_conference_subscribe.txt"))));
    CK("g.3gpp.mcptt 만(icsi-ref 없음) → 403",
       !McpttAcceptContactOk("*;+g.3gpp.mcptt;require;explicit,"));
    CK("특성 태그 접두사 일치(+g.3gpp.mcptt-x)는 아니다",
       !McpttAcceptContactOk(
           "*;+g.3gpp.mcptt-x;require;explicit,*;+g.3gpp.icsi-ref=\"urn%3Aurn-"
           "7%3A3gpp-service.ims.icsi.mcptt\";require;explicit,"));
    CK("ICSI 접두사 일치(…icsi.mcpttx)는 아니다",
       !McIcsiIn("*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi."
                 "mcpttx\"",
                 kMcpttIcsi, kMcpttIcsiEnc));
    CK("원 표기 ICSI(P-Preferred-Service 형)도 읽는다",
       McIcsiIn("urn:urn-7:3gpp-service.ims.icsi.mcptt", kMcpttIcsi,
                kMcpttIcsiEnc));
    // 미디어 평면 SDS INVITE (TS 24.282 §9.2.3.4.4 3))
    CK("09 MSRP INVITE Accept-Contact mcdata.sds 둘 → 통과",
       McDataSdsAcceptContactOk(Accept(Load("09_sds_media_group_invite.txt"))));
    CK("MSRP INVITE icsi-ref 만 → 403",
       !McDataSdsAcceptContactOk(
           "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata."
           "sds\";require;explicit,"));
    CK("mcdata ICSI 는 mcdata.sds 가 아니다",
       !McIcsiIn("\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.fd\"",
                 kMcDataSdsIcsi, kMcDataSdsIcsiEnc));
    // MIME 본문 (TS 24.282 §9.2.2.4.2 2) · §10.2.4.4.2 3)) — 199
    {
      const Golden g = Load("04_sds_group_message.txt");
      CMcDataSdsInfo i;
      const bool b = McDataParseBody(g.strCtype, g.strBody, i);
      CK("04 SDS info·signalling·payload → 199 아님",
         b && i.m_bHasInfo && i.m_bHasPayload && !McDataMissingBodies(b, i));
      i.m_bHasPayload = false;
      CK("SDS payload 없음 → 199", McDataMissingBodies(b, i));
      i.m_bHasPayload = true;
      i.m_bHasInfo = false;
      CK("SDS mcdata-info 없음 → 199", McDataMissingBodies(b, i));
      CK("signalling 없음 → 199", McDataMissingBodies(false, i));
    }
    {
      const Golden g = Load("07_fd_group_message.txt");
      CMcDataSdsInfo i;
      const bool b = McDataParseBody(g.strCtype, g.strBody, i);
      CK("07 FD info·signalling(payload 없음) → 199 아님",
         b && !i.m_bHasPayload && !McDataMissingBodies(b, i));
    }
    // 경보 판별은 mcptt-info 파트에서만 (TS 24.379 §12.1)
    CK("mcptt-info 파트의 alert-ind → MCPTT 경보",
       McEmergencyAlertServiceOf(
           "<mcpttinfo><mcptt-Params><alert-ind>true</alert-ind></mcptt-Params>"
           "</mcpttinfo>",
           "application/vnd.3gpp.mcptt-info+xml") == EMcAlertService::Mcptt);
    CK("mcptt-info 파트가 아닌 본문의 alert-ind → 경보 아님",
       McEmergencyAlertServiceOf(
           "<mcpttinfo><mcptt-Params><alert-ind>true</alert-ind></mcptt-Params>"
           "</mcpttinfo>",
           "text/plain") == EMcAlertService::None);
  }

  printf(g_fail ? "FAILED %d\n" : "ALL PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
