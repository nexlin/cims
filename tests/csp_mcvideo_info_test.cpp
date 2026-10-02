// CSP MCVideo 호 제어 경계 단위시험 — csp/McVideoInfo.h (계약 K3·K4 해석·생성
// 쪽).
//   골든 = tests/fixtures/mcvideo/sip/*.txt (전송 바이트 그대로) 를 직접 읽는다
//   — 생성 쪽(CSP)과 해석 쪽(SDK)이 같은 파일을 본다. · mcvideo-info 해석(Annex
//   F.1 contentType 자식) · multipart 파트 추출 · 서비스 판별 —
//   P-Preferred-Service/Accept-Contact/Contact 의 MCVideo ICSI·특성
//   태그(TS 24.281 §7.2.1·§9.2.2.2.1.1) · 제어 채널 fmtp 해석과 answer/offer
//   조립 — offer(03·05) → answer(04·06) 골든과 같은 값(TS 24.581 §14.3,
//   mcvideo.md §1.4) · fmtp 이름 = 전송 제어 정의
//   정본(docs/design/features/mcvideo_tc_defs.yaml) · Warning 117/118 = 골든
//   09·10 의 문구 빌드·실행은
//   S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py)가 레포 루트에서 수행한다.
#include <cstdio>
#include <fstream>
#include <sstream>

#include "McVideoInfo.h"

static int fail = 0;
#define CK(n, c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      printf("FAIL %s\n", n);                                                  \
      fail++;                                                                  \
    } else                                                                     \
      printf("ok   %s\n", n);                                                  \
  } while (0)

static const char *kDir = "tests/fixtures/mcvideo/sip/";

struct Msg {
  std::string start, headers, body;
};

static std::string slurp(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static Msg load(const char *name) {
  Msg m;
  const std::string d = slurp(std::string(kDir) + name);
  const size_t e = d.find("\r\n\r\n");
  const std::string head = d.substr(0, e);
  m.body = e == std::string::npos ? "" : d.substr(e + 4);
  const size_t l = head.find("\r\n");
  m.start = head.substr(0, l);
  m.headers = head.substr(l + 2);
  return m;
}

/** 헤더 값들(같은 이름 여러 줄) — 대소문자 무시. */
static std::vector<std::string> hdr(const Msg &m, const std::string &name) {
  std::vector<std::string> out;
  std::istringstream in(m.headers);
  std::string ln;
  while (std::getline(in, ln)) {
    if (!ln.empty() && ln.back() == '\r')
      ln.pop_back();
    const size_t c = ln.find(':');
    if (c == std::string::npos)
      continue;
    std::string k = ln.substr(0, c);
    if (k.size() != name.size())
      continue;
    bool eq = true;
    for (size_t i = 0; i < k.size(); ++i)
      if (tolower((unsigned char)k[i]) != tolower((unsigned char)name[i]))
        eq = false;
    if (!eq)
      continue;
    std::string v = ln.substr(c + 1);
    v.erase(0, v.find_first_not_of(' '));
    out.push_back(v);
  }
  return out;
}

static std::string ctype(const Msg &m) {
  auto v = hdr(m, "Content-Type");
  return v.empty() ? "" : v[0];
}

/** SDP 본문의 a=fmtp:MCVideo 줄 */
static std::string fmtpLine(const std::string &sdp) {
  const size_t p = sdp.find("a=fmtp:MCVideo ");
  if (p == std::string::npos)
    return "";
  const size_t e = sdp.find("\r\n", p);
  return sdp.substr(p, e - p);
}

int main() {
  // ── 03 chat 합류 INVITE — 판별·mcvideo-info·offer fmtp ──
  const Msg inv = load("03_chat_join_invite.txt");
  CK("03 loaded", inv.start.rfind("INVITE sip:mcvideo_psi@", 0) == 0);
  auto pps = hdr(inv, "P-Preferred-Service");
  CK("03 P-Preferred-Service = MCVideo ICSI",
     pps.size() == 1 && pps[0] == kMcVideoIcsi && McVideoIcsiIn(pps[0]));
  bool acIcsi = false, acTag = false;
  for (const auto &a : hdr(inv, "Accept-Contact")) {
    acIcsi = acIcsi || McVideoIcsiIn(a);
    acTag = acTag || McVideoFeatureIn(a);
  }
  CK("03 Accept-Contact ICSI + tag", acIcsi && acTag);
  CK("03 Contact capable", McVideoContactCapable(hdr(inv, "Contact")[0]));
  const std::string info =
      McVideoBodyPart(inv.body, ctype(inv), kMcVideoInfoSubtype);
  const CMcVideoInfo mi = ParseMcVideoInfo(info);
  CK("03 mcvideo-info present", mi.bPresent);
  CK("03 session-type chat", mi.strSessionType == "chat");
  CK("03 request-uri tel:g101",
     mi.strRequestUri == "tel:g101" && McpttBareId(mi.strRequestUri) == "g101");
  CK("03 client-id",
     mi.strClientId == "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f");
  CK("03 no indicators",
     !mi.bHasEmergencyInd && !mi.bHasImminentInd && !mi.bHasAlertInd);
  const std::string sdpOffer =
      McVideoBodyPart(inv.body, ctype(inv), "application/sdp");
  CK("03 sdp part",
     sdpOffer.find("m=application 40004 udp MCVideo") != std::string::npos);
  const CMcVideoFmtp fo = ParseMcVideoFmtp(fmtpLine(sdpOffer));
  CK("03 fmtp parsed", fo.bPresent && fo.bQueueing && fo.iPriority == 5 &&
                           fo.bHasTcSsrc && fo.uTcSsrc == 305419896u);
  CK("03 fmtp no implicit", !fo.bImplicit && !fo.bGranted);

  // ── 04 answer fmtp = BuildMcVideoAnswerFmtp(03 offer, user-priority 5, CMP
  // tc_ssrc) ──
  const Msg ok = load("04_chat_join_200.txt");
  const std::string ansFmtp = fmtpLine(ok.body);
  const std::string built =
      "a=fmtp:MCVideo " + BuildMcVideoAnswerFmtp(fo, 5, 2863311530u);
  CK("04 answer fmtp = builder", ansFmtp == built);
  CK("04 answer echoes mc_queueing (§14.3.2 — CMP 송출 큐)",
     ansFmtp.find("mc_queueing;") != std::string::npos);
  CK("04 answer mc_priority=min(offer,user)",
     BuildMcVideoAnswerFmtp(fo, 3, 1).find("mc_priority=3;") !=
         std::string::npos);
  CK("04 focus contact", hdr(ok, "Contact")[0].find(
                             kMcVideoFocusContactParams) != std::string::npos);
  CK("04 session identity gr",
     hdr(ok, "Contact")[0].find(";gr=") != std::string::npos);
  // TS 24.281 §6.3.3.2.3.2 8)~10) — 제어 기능 200 OK 의 Supported (RFC 4538·4488·7614)
  CK("04 Supported tdialog·norefersub·explicitsub·nosub",
     hdr(ok, "Supported").size() == 1 &&
         hdr(ok, "Supported")[0] == kMcFocusOkSupported);

  // ── 05 prearranged 개시 + 암묵 요청 → 06 answer(수락·허가·SSRC 쌍) ──
  const Msg pinv = load("05_prearranged_initiate_invite.txt");
  const CMcVideoInfo pmi = ParseMcVideoInfo(
      McVideoBodyPart(pinv.body, ctype(pinv), kMcVideoInfoSubtype));
  CK("05 session-type prearranged",
     pmi.strSessionType == "prearranged" &&
         McpttBareId(pmi.strRequestUri) == "g103");
  const CMcVideoFmtp pfo = ParseMcVideoFmtp(
      fmtpLine(McVideoBodyPart(pinv.body, ctype(pinv), "application/sdp")));
  CK("05 implicit + granted offered",
     pfo.bImplicit && pfo.bGranted && !pfo.bQueueing);
  const Msg pok = load("06_prearranged_initiate_200.txt");
  const CMcVideoFmtp pfa = ParseMcVideoFmtp(fmtpLine(pok.body));
  CK("06 answer ssrc pair", pfa.bHasAudioSsrc && pfa.bHasVideoSsrc &&
                                pfa.uAudioSsrc == 1111638594u &&
                                pfa.uVideoSsrc == 1111638595u);
  CK("06 answer fmtp = builder",
     fmtpLine(pok.body) ==
         "a=fmtp:MCVideo " + BuildMcVideoAnswerFmtp(pfo, 5, 2863311531u, true,
                                                    true, 1111638594u,
                                                    1111638595u));
  CK("06 not accepted → no implicit",
     BuildMcVideoAnswerFmtp(pfo, 5, 7) ==
         "mc_priority=5;mc_transmission_ssrc=7");
  CK("06 granted only when offered",
     BuildMcVideoAnswerFmtp(fo, 5, 7, true, true, 1, 2).find("mc_granted") ==
         std::string::npos);

  // ── 07 멤버 초대 offer fmtp · mcvideo-info 식별자 ──
  const Msg fan = load("07_prearranged_member_invite.txt");
  CK("07 P-Asserted-Service",
     hdr(fan, "P-Asserted-Service").size() == 1 &&
         hdr(fan, "P-Asserted-Service")[0] == kMcVideoIcsi);
  CK("07 invite fmtp = builder",
     fmtpLine(McVideoBodyPart(fan.body, ctype(fan), "application/sdp")) ==
         "a=fmtp:MCVideo " + BuildMcVideoInviteFmtp(5, 2863311532u));
  // TS 24.581 §14.2.2 — 대기열을 지원하는 제어 기능은 offer 에 mc_queueing (shall)
  CK("07 offer mc_queueing (§14.2.2)",
     BuildMcVideoInviteFmtp(5, 1).rfind("mc_queueing;", 0) == 0 &&
         BuildMcVideoInviteFmtp(-1, 1) == "mc_queueing;mc_transmission_ssrc=1");
  // TS 24.281 §6.3.2.2.3 5)·6) Supported tdialog·norefersub (+ §6.3.3.1.2 7) timer) · §6.3.2.2.5.2 8) Answer-Mode
  {
    const std::vector<std::string> sup = hdr(fan, "Supported");
    CK("07 Supported timer + tdialog·norefersub",
       sup.size() == 2 && sup[0] == "timer" && sup[1] == kMcMemberInviteSupported);
    CK("07 Answer-Mode Auto", hdr(fan, "Answer-Mode").size() == 1 &&
                                  hdr(fan, "Answer-Mode")[0] == "Auto");
  }
  const CMcVideoInfo fmi = ParseMcVideoInfo(
      McVideoBodyPart(fan.body, ctype(fan), kMcVideoInfoSubtype));
  CK("07 ids", fmi.strRequestUri == "tel:+82510002002" &&
                   fmi.strCallingUserId == "tel:+82510002001" &&
                   fmi.strCallingGroupId == "tel:g103");
  // 생성 → 해석 왕복
  const std::string doc = McVideoInfoDocument(
      McVideoInfoValue("session-type", "prearranged") +
      McVideoInfoUri("mcvideo-request-uri", "tel:+82510002002") +
      McVideoInfoUri("mcvideo-calling-user-id", "tel:+82510002001") +
      McVideoInfoUri("mcvideo-calling-group-id", "tel:g103"));
  const CMcVideoInfo rt = ParseMcVideoInfo(doc);
  CK("builder round trip", rt.bPresent && rt.strSessionType == "prearranged" &&
                               rt.strRequestUri == fmi.strRequestUri &&
                               rt.strCallingGroupId == "tel:g103");

  // ── 01 REGISTER Contact · 08 재합류 ──
  const Msg reg = load("01_register.txt");
  CK("01 contact capable (mcptt+mcvideo)",
     McVideoContactCapable(hdr(reg, "Contact")[0]));
  CK("01 mcvideo-info token",
     !ParseMcVideoInfo(
          McVideoBodyPart(reg.body, ctype(reg), kMcVideoInfoSubtype))
          .strAccessToken.empty());
  CK("tag boundary — +g.3gpp.mcvideo-x 는 아니다",
     !McVideoFeatureIn("+g.3gpp.mcvideo-x;+g.3gpp.mcptt"));
  CK("logoff contact (tags 없음)",
     !McVideoContactCapable("<sip:+8251@1.2.3.4>;+g.3gpp.mcptt"));
  const Msg rj = load("08_prearranged_rejoin_invite.txt");
  CK("08 R-URI session identity", rj.start.find(";gr=") != std::string::npos);

  // ── fmtp 수신 관대 — ABNF COLON 구분도 받는다 ──
  const CMcVideoFmtp colon =
      ParseMcVideoFmtp("mc_priority=7:mc_granted:mc_transmission_ssrc=9");
  CK("colon separators",
     colon.iPriority == 7 && colon.bGranted && colon.uTcSsrc == 9u);

  // ── Warning 117/118 = 골든 문구 ──
  CK("09 Warning 117",
     hdr(load("09_reject_404_117.txt"), "Warning")[0] ==
         McpttWarning(117, kMcVideoWarn117, "ptt.cims.example.kr"));
  CK("10 Warning 118",
     hdr(load("10_reject_404_118.txt"), "Warning")[0] ==
         McpttWarning(118, kMcVideoWarn118, "ptt.cims.example.kr"));

  // ── fmtp 이름 = 전송 제어 정의 정본(yaml) ──
  const std::string yaml = slurp("docs/design/features/mcvideo_tc_defs.yaml");
  for (const char *n :
       {"\"mc_queueing\"", "\"mc_priority\"", "\"mc_reception_priority\"",
        "\"mc_granted\"", "\"mc_implicit_request\"", "\"mc_audio_ssrc\"",
        "\"mc_video_ssrc\"", "\"mc_transmission_ssrc\""})
    CK(n, yaml.find(n) != std::string::npos);

  // ── 요청의 서비스 판별 (A9 — PUBLISH·SUBSCRIBE·INVITE 를 MCPTT 로 잘못 읽지
  // 않는다, TS 24.281 §8.2.2.2.3 3)) ──
  auto services = [](const Msg &m) {
    std::string s;
    for (const char *h :
         {"P-Asserted-Service", "P-Preferred-Service", "Accept-Contact"})
      for (const auto &v : hdr(m, h))
        s += v + ",";
    return s;
  };
  const Msg pub = load("02_publish_affiliation.txt");
  CK("02 PUBLISH is MCVideo",
     McVideoRequestIndicated(services(pub), "", pub.body, ctype(pub)));
  CK("02 PUBLISH by body alone (no PPS)",
     McVideoRequestIndicated("", "", pub.body, ctype(pub)));
  const Msg inv3 = load("03_chat_join_invite.txt");
  CK("03 INVITE is MCVideo",
     McVideoRequestIndicated(services(inv3), "", inv3.body, ctype(inv3)));
  CK("03 INVITE by Accept-Contact alone",
     McVideoRequestIndicated(services(inv3), "", "", ""));
  const std::string mcpttPidf =
      "<?xml version=\"1.0\"?><presence xmlns=\"urn:ietf:params:xml:ns:pidf\" "
      "xmlns:mcpttPI10=\"urn:3gpp:ns:mcpttPresInfo:1.0\" "
      "entity=\"tel:+8251\"><tuple id=\"c1\"><status>"
      "<mcpttPI10:affiliation group=\"tel:g001\"/></status></tuple></presence>";
  CK("MCPTT pidf PUBLISH is not MCVideo",
     !McVideoRequestIndicated("urn:urn-7:3gpp-service.ims.icsi.mcptt,", "",
                              mcpttPidf, "application/pidf+xml"));
  CK("MCPTT Accept mcptt-affiliation-info is not MCVideo",
     !McVideoRequestIndicated(
         "", "application/vnd.3gpp.mcptt-affiliation-info+xml", "", ""));
  // 골든 02 의 pidf 파트 — served ID·client ID·p-id·그룹 (mcvideoPI10 접두사)
  const CMcpttPidfAffiliation aff =
      ParsePidfAffiliation(McVideoBodyPart(pub.body, ctype(pub), "pidf+xml"));
  CK("02 pidf entity/tuple/p-id",
     aff.bValid && aff.strEntity == "tel:+82510002001" &&
         aff.strClientId == "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f" &&
         aff.strPid == "a1-mcv-aff-0001");
  CK("02 pidf groups", aff.vecGroups.size() == 2 &&
                           aff.vecGroups[0] == "tel:g101" &&
                           aff.vecGroups[1] == "tel:g103");
  CK("02 served ID (mcvideo-request-uri)",
     McpttBareId(ParseMcVideoInfo(
                     McVideoBodyPart(pub.body, ctype(pub), kMcVideoInfoSubtype))
                     .strRequestUri) == "+82510002001");
  // NOTIFY 본문 — MCVideo 는 mcvideoPresInfo 네임스페이스(§8.3.1), MCPTT 는
  // 그대로
  CMcpttAffClient cl;
  cl.strClientId = "urn:uuid:c1";
  CMcpttAffGroup g;
  g.strGroupUri = "tel:g101";
  cl.vecGroups.push_back(g);
  const std::string nv =
      BuildPidfAffiliationInfo("tel:+82510002001", {cl}, "p1", true);
  CK("NOTIFY mcvideoPresInfo",
     nv.find("xmlns:mcvideoPI10=\"urn:3gpp:ns:mcvideoPresInfo:1.0\"") !=
             std::string::npos &&
         nv.find("<mcvideoPI10:affiliation group=\"tel:g101\"") !=
             std::string::npos &&
         nv.find("<mcvideoPI10:p-id>p1</mcvideoPI10:p-id>") !=
             std::string::npos &&
         nv.find("mcpttPI10") == std::string::npos);
  const std::string np =
      BuildPidfAffiliationInfo("tel:+82510002001", {cl}, "", false);
  CK("NOTIFY mcpttPresInfo unchanged",
     np.find("xmlns:mcpttPI10=\"urn:3gpp:ns:mcpttPresInfo:1.0\" "
             "entity=\"tel:+82510002001\">") != std::string::npos &&
         np.find("mcvideo") == std::string::npos);
  // NOTIFY 본문을 다시 읽으면 같은 그룹 (단말 해석과 같은 규칙)
  CK("NOTIFY round trip", ParsePidfAffiliation(nv).vecGroups.size() == 1 &&
                              ParsePidfAffiliation(nv).strPid == "p1");

  // ── N2 — 동시 MCVideo 제휴 그룹 상한 (TS 24.281 §8.2.2.2.3 14)b)c)) ──
  {
    using V = std::vector<std::string>;
    // 다른 클라이언트 없음, 새 요청 5 개, N2 4 → 요청 순서 앞 4 개
    CK("N2 new in request order",
       McvAffiliationsWithinN2({"g1", "g2", "g3", "g4", "g5"}, {}, {}, 4) ==
           V({"g1", "g2", "g3", "g4"}));
    // 이 클라이언트가 이미 g5 에 제휴 — 기존 제휴를 먼저 지킨다(결과는 요청
    // 순서)
    CK("N2 keeps held first",
       McvAffiliationsWithinN2({"g1", "g2", "g3", "g4", "g5"}, {"g5"}, {}, 4) ==
           V({"g1", "g2", "g3", "g5"}));
    // 다른 클라이언트가 g1·g9 를 쥐고 있다 — g1 은 자리를 더 쓰지 않고, g9 는
    // 자리를 쓴다
    CK("N2 counts other clients",
       McvAffiliationsWithinN2({"g1", "g2", "g3", "g4"}, {}, {"g1", "g9"}, 4) ==
           V({"g1", "g2", "g3"}));
    // N2 를 낮췄다 — 이 클라이언트의 기존 제휴도 줄인다(다른 클라이언트 몫 2 +
    // 1 = 3)
    CK("N2 lowered reduces held",
       McvAffiliationsWithinN2({"g1", "g2"}, {"g1", "g2"}, {"g8", "g9"}, 3) ==
           V({"g1"}));
    CK("N2 <= 0 = no limit",
       McvAffiliationsWithinN2({"g1", "g2"}, {}, {"g3"}, 0) == V({"g1", "g2"}));
    CK("N2 dedup", McvAffiliationsWithinN2({"g1", "g1", "g2"}, {}, {}, 4) ==
                       V({"g1", "g2"}));
  }

  // 미인가 우선순위 요청의 403 본문 (McVideoPriorityRejectBody — TS 24.281
  // §6.3.3.1.13 · §9.2.1.4.2 10)·11) · §9.2.2.4.1.1 6)·7) · §9.2.1.4.7
  // 3)·4), S03 VGC-1)
  {
    auto info = [](const std::string &params) {
      return ParseMcVideoInfo(McVideoInfoDocument(params));
    };
    const std::string emg =
        McVideoPriorityRejectBody(info(McVideoInfoBool("emergency-ind", true)));
    const CMcVideoInfo emgBack = ParseMcVideoInfo(emg);
    CK("prio emergency → emergency-ind false + alert-ind false",
       !emg.empty() && emgBack.bHasEmergencyInd && !emgBack.bEmergency &&
           emgBack.bHasAlertInd && !emgBack.bAlert && !emgBack.bHasImminentInd);
    CK("prio emergency body = contentType form",
       emg.find("<emergency-ind type=\"Normal\"><mcvideoBoolean>false"
                "</mcvideoBoolean></emergency-ind>") != std::string::npos);
    const CMcVideoInfo alertBack = ParseMcVideoInfo(
        McVideoPriorityRejectBody(info(McVideoInfoBool("alert-ind", true))));
    CK("prio alert → emergency-ind false + alert-ind false",
       alertBack.bHasEmergencyInd && !alertBack.bEmergency &&
           alertBack.bHasAlertInd && !alertBack.bAlert);
    const CMcVideoInfo impBack = ParseMcVideoInfo(McVideoPriorityRejectBody(
        info(McVideoInfoBool("imminentperil-ind", true))));
    CK("prio imminent → imminentperil-ind false only",
       impBack.bHasImminentInd && !impBack.bImminent &&
           !impBack.bHasEmergencyInd && !impBack.bHasAlertInd);
    CK("prio cancel direction passes",
       McVideoPriorityRejectBody(info(McVideoInfoBool("emergency-ind", false) +
                                      McVideoInfoBool("alert-ind", false)))
           .empty());
    CK("prio broadcast passes",
       McVideoPriorityRejectBody(info(McVideoInfoBool("broadcast-ind", true)))
           .empty());
    CK("prio none passes",
       McVideoPriorityRejectBody(info(McVideoInfoValue("session-type", "chat")))
           .empty());
  }

  // 그룹 호가 아닌 호 종류의 거절 사유 (McVideoNonGroupSessionWarn — TS 24.281 표 4.4.2-2, S09 VPRV-1)
  {
    const char *t = nullptr;
    CK("ng chat/prearranged/empty pass",
       McVideoNonGroupSessionWarn("chat", &t) == 0 && McVideoNonGroupSessionWarn("prearranged", &t) == 0 &&
           McVideoNonGroupSessionWarn("", &t) == 0);
    CK("ng private 107", McVideoNonGroupSessionWarn("private", &t) == 107 && std::string(t) == kMcVideoWarn107);
    CK("ng ambient-viewing 154", McVideoNonGroupSessionWarn("ambient-viewing", &t) == 154);
    CK("ng adhoc 186", McVideoNonGroupSessionWarn("adhoc", &t) == 186 && std::string(t) == kMcVideoWarn186);
    CK("ng pull/push 100", McVideoNonGroupSessionWarn("pull-recorded", &t) == 100 &&
                               McVideoNonGroupSessionWarn("push", &t) == 100 && std::string(t) == kMcVideoWarn100);
  }
  printf("%s (%d fail)\n", fail ? "FAIL" : "PASS", fail);
  return fail ? 1 : 0;
}
