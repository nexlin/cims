// CSP service-config 해석 단위시험 — csp/CspServiceConfig.h `DurationMs` / `Parse` / `ParsePriority` /
//   `ParseEmergencyGroupTimeLimitSec`.
//   TS 24.484 §8.4 문서의 on-network <transmit-time><time-limit>(T2)·<fc-timers-counters> 를 CMP floor_timers 값(초·횟수)으로
//   옮긴다. xs:duration(PT<h>H<m>M<n>S, 소수 초), on-network 범위 한정(off-network 의 transmit-time 무시), 이름 경계
//   (<group-time-limit> 은 <time-limit> 이 아니다), 문서에 없는 값 = -1 이 요점이다. Resource-Priority 는
//   <emergency-/imminent-peril-/normal-resource-priority> 의 namespace.priority(TS 24.379 §6.3.3.1.19, 없는 항목 = mcpttp 기본값),
//   TNG2 는 <emergency-call><group-time-limit>(§6.3.3.1.16, 없으면 -1).
//   개별 호·애드혹 그룹 호의 세션 타이머(`ParseCallTimers` — <private-call>·<anyExt><adhoc-group-call>, TS 24.484 §8.4.2.1)와
//   호 종류별 T4·최대 시간 선택(`CspSessionT4Sec`·`CspSessionMaxDurationSec` — TS 24.380 표 11.1.3-1 · TS 24.379 §6.3.8 ·
//   §17.4.2.2 13)), UE initial configuration 변경 통지의 문서 선택자(`CspUeInitConfigSelector` — §7.2.1.1)·xcap-diff 본문(바뀐 문서만 — CMS 문서
//   선택자 `CspMcpttUserProfileSel`·`CspMcVideoUserProfileSel`·`CspMcVideoServiceConfigSel`).
//   빌드·실행은 S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py)가 수행한다.
#include "CspServiceConfig.h"
#include <cstdio>
static int fail=0;
#define CK(n,c) do{ if(!(c)){printf("FAIL %s\n",n);fail++;}else printf("ok   %s\n",n);}while(0)
int main(){
  CK("dur s",CCspServiceConfig::DurationMs("PT4S")==4000);
  CK("dur frac",CCspServiceConfig::DurationMs("PT0.5S")==500);
  CK("dur hms",CCspServiceConfig::DurationMs("PT1H2M3S")==3723000);
  CK("dur bad",CCspServiceConfig::DurationMs("4S")==-1 && CCspServiceConfig::DurationMs("PT4")==-1 && CCspServiceConfig::DurationMs("PTS")==-1);
  const char* doc =
    "<service-configuration-info xmlns=\"urn:3gpp:ns:mcpttServiceConfig:1.0\"><service-configuration-params domain=\"d\">"
    "<on-network><emergency-call><group-time-limit>PT99S</group-time-limit></emergency-call>"
    "<transmit-time><time-limit>PT45S</time-limit></transmit-time>"
    "<fc-timers-counters><T1-end-of-rtp-media>PT4S</T1-end-of-rtp-media><T3-stop-talking-grace>PT3S</T3-stop-talking-grace>"
    "<T7-floor-idle>PT0S</T7-floor-idle><T8-floor-revoke>PT1S</T8-floor-revoke><T20-floor-granted>PT1.5S</T20-floor-granted>"
    "<C7-floor-idle>4</C7-floor-idle><C20-floor-granted>5</C20-floor-granted></fc-timers-counters></on-network>"
    "<off-network><transmit-time><time-limit>PT7S</time-limit></transmit-time></off-network>"
    "</service-configuration-params></service-configuration-info>";
  CspFloorParams f;
  CK("parse",CCspServiceConfig::Parse(doc,f) && f.bValid);
  CK("T2 on-network time-limit",f.iT2Sec==45);
  CK("T1 T3",f.iT1Sec==4 && f.iT3Sec==3);
  CK("T7 zero",f.iT7Sec==0);
  CK("T8 T20 truncate",f.iT8Sec==1 && f.iT20Sec==1);
  CK("C7 C20",f.iC7==4 && f.iC20==5);
  CspFloorParams g;
  CK("absent = -1",CCspServiceConfig::Parse("<service-configuration-info/>",g) && g.iT1Sec==-1 && g.iT2Sec==-1 && g.iC7==-1);
  CspFloorParams h;
  CK("old root rejected",!CCspServiceConfig::Parse("<mcptt-service-config/>",h));
  CK("TNG2 group-time-limit",CCspServiceConfig::ParseEmergencyGroupTimeLimitSec(doc)==99);
  CK("TNG2 absent",CCspServiceConfig::ParseEmergencyGroupTimeLimitSec("<service-configuration-info><on-network/></service-configuration-info>")==-1);
  const char* rpdoc =
    "<service-configuration-info><service-configuration-params domain=\"d\"><on-network>"
    "<emergency-resource-priority>\n <resource-priority-namespace>mcpttp</resource-priority-namespace>\n"
    " <resource-priority-priority>14</resource-priority-priority></emergency-resource-priority>"
    "<normal-resource-priority><resource-priority-namespace>mcpttq</resource-priority-namespace>"
    "<resource-priority-priority>1</resource-priority-priority></normal-resource-priority>"
    "<imminent-peril-resource-priority><resource-priority-namespace>mcpttp</resource-priority-namespace></imminent-peril-resource-priority>"
    "</on-network></service-configuration-params></service-configuration-info>";
  CspPriorityParams rp;
  CCspServiceConfig::ParsePriority(rpdoc,rp);
  CK("RP emergency ns.prio",rp.strEmergency=="mcpttp.14");
  CK("RP normal other ns",rp.strNormal=="mcpttq.1");
  CK("RP imminent partial = default",rp.strImminentPeril=="mcpttp.8");
  CspPriorityParams rp0;
  CCspServiceConfig::ParsePriority("<service-configuration-info/>",rp0);
  CK("RP absent = default",rp0.strEmergency=="mcpttp.15" && rp0.strImminentPeril=="mcpttp.8" && rp0.strNormal=="mcpttp.0");
  // 개별 호·애드혹 그룹 호 세션 타이머 — on-network <private-call> · <anyExt><adhoc-group-call> (off-network 의 private-call 무시)
  const char* ctdoc =
    "<service-configuration-info><service-configuration-params domain=\"d\"><on-network>"
    "<private-call><hang-time>PT20S</hang-time><max-duration-with-floor-control>PT600S</max-duration-with-floor-control>"
    "<max-duration-without-floor-control>PT1H</max-duration-without-floor-control></private-call>"
    "<hang-time-warning>PT5S</hang-time-warning><transmit-time><time-limit>PT30S</time-limit></transmit-time>"
    "<anyExt><adhoc-group-call><allow-adhoc-group-call-support>true</allow-adhoc-group-call-support>"
    "<max-no-participants>64</max-no-participants><hang-time>PT40S</hang-time><broadcast-hang-time>PT9S</broadcast-hang-time>"
    "<max-duration-of-call>PT1800S</max-duration-of-call></adhoc-group-call></anyExt></on-network>"
    "<off-network><private-call><hang-time>PT7S</hang-time></private-call></off-network>"
    "</service-configuration-params></service-configuration-info>";
  CspCallTimerParams ct;
  CCspServiceConfig::ParseCallTimers(ctdoc,ct);
  CK("private hang-time T4",ct.iPrivateHangSec==20);
  CK("private max with/without floor",ct.iPrivateMaxFloorSec==600 && ct.iPrivateMaxNoFloorSec==3600);
  CK("adhoc hang-time ≠ broadcast-hang-time",ct.iAdhocHangSec==40 && ct.iAdhocBroadcastHangSec==9);
  CK("adhoc max-duration-of-call TNG3",ct.iAdhocMaxDurationSec==1800);
  CspCallTimerParams ct0;
  CCspServiceConfig::ParseCallTimers("<service-configuration-info><on-network><private-call><hang-time>PT5S</hang-time>"
                                     "</private-call></on-network></service-configuration-info>",ct0);
  CK("absent elements = -1",ct0.iPrivateHangSec==5 && ct0.iPrivateMaxFloorSec==-1 && ct0.iAdhocHangSec==-1 &&
     ct0.iAdhocMaxDurationSec==-1);
  // 호 종류별 T4 (TS 24.380 표 11.1.3-1): 편성 = 그룹 값 · 애드혹 = hang-time(일제면 broadcast-hang-time) · 개별 = 발언권 제어 있을 때 · chat 0
  CK("T4 prearranged = group",CspSessionT4Sec(ECspCallKind::Prearranged,30,false,true,ct)==30 &&
     CspSessionT4Sec(ECspCallKind::Prearranged,0,true,true,ct)==0);
  CK("T4 chat = 0",CspSessionT4Sec(ECspCallKind::Chat,30,false,true,ct)==0);
  CK("T4 adhoc / broadcast adhoc",CspSessionT4Sec(ECspCallKind::Adhoc,30,false,true,ct)==40 &&
     CspSessionT4Sec(ECspCallKind::Adhoc,30,true,true,ct)==9);
  CK("T4 private floor on / off",CspSessionT4Sec(ECspCallKind::Private,30,false,true,ct)==20 &&
     CspSessionT4Sec(ECspCallKind::Private,30,false,false,ct)==0);
  CK("T4 absent = 0",CspSessionT4Sec(ECspCallKind::Adhoc,30,false,true,ct0)==0 &&
     CspSessionT4Sec(ECspCallKind::Private,30,false,true,CspCallTimerParams())==0);
  // 호 종류별 최대 시간 (TS 24.379 §6.3.8 · §17.4.2.2 13) · §6.3.3.5.2)
  CK("max prearranged = group, emergency → TNG2",CspSessionMaxDurationSec(ECspCallKind::Prearranged,3600,true,0,0,ct)==3600 &&
     CspSessionMaxDurationSec(ECspCallKind::Prearranged,3600,true,0,2,ct)==0 &&
     CspSessionMaxDurationSec(ECspCallKind::Prearranged,3600,true,0,1,ct)==3600);
  CK("max prearranged 0 = 무제한",CspSessionMaxDurationSec(ECspCallKind::Prearranged,0,true,0,0,ct)==0);
  CK("max chat = 0",CspSessionMaxDurationSec(ECspCallKind::Chat,3600,true,0,0,ct)==0);
  CK("max adhoc TNG3",CspSessionMaxDurationSec(ECspCallKind::Adhoc,0,true,0,0,ct)==1800);
  CK("max adhoc priority start = none",CspSessionMaxDurationSec(ECspCallKind::Adhoc,0,true,1,0,ct)==0 &&
     CspSessionMaxDurationSec(ECspCallKind::Adhoc,0,true,2,0,ct)==0 &&
     CspSessionMaxDurationSec(ECspCallKind::Adhoc,0,true,0,2,ct)==0);
  CK("max private with / without floor, condition 무관",CspSessionMaxDurationSec(ECspCallKind::Private,0,true,0,0,ct)==600 &&
     CspSessionMaxDurationSec(ECspCallKind::Private,0,false,2,2,ct)==3600);
  // UE initial configuration 변경 통지 — MCS UE ID = +sip.instance(따옴표·<> 제거), 선택자 = §7.2.1.1 경로
  CK("MCS UE ID from +sip.instance",CspMcsUeIdOf("\"<urn:uuid:00000000-0000-1000-8000-AABBCCDDEEFF>\"")==
     "urn:uuid:00000000-0000-1000-8000-AABBCCDDEEFF");
  CK("ue-init-config selector",CspUeInitConfigSelector("urn:uuid:ab-12")==
     "org.3gpp.mcptt.ue-init-config/users/sip:urn:uuid:ab-12/urn:uuid:ab-12");
  CK("selector escapes non-pchar",CspUeInitConfigSelector("urn:x/y z&\"")==
     "org.3gpp.mcptt.ue-init-config/users/sip:urn:x%2Fy%20z%26%22/urn:x%2Fy%20z%26%22");
  CK("selector empty id",CspUeInitConfigSelector("").empty());
  // 애드혹 초대 인원 상한 <adhoc-group-call><max-no-participants> (TS 24.484 §8.4.2.1 13)d) — S11 ADH-3)
  CK("adhoc max participants",CCspServiceConfig::ParseAdhocMaxParticipants(ctdoc)==64);
  CK("adhoc max absent",CCspServiceConfig::ParseAdhocMaxParticipants("<service-configuration-info><on-network></on-network></service-configuration-info>")==-1);
  CK("adhoc max zero = absent",CCspServiceConfig::ParseAdhocMaxParticipants("<service-configuration-info><on-network><anyExt><adhoc-group-call><max-no-participants>0</max-no-participants></adhoc-group-call></anyExt></on-network></service-configuration-info>")==-1);
  const std::string xd = CspXcapDiffDocBody("https://csc:4430/","org.3gpp.mcptt.ue-init-config/users/sip:a/a","e1");
  CK("xcap-diff body",xd.find("<xcap-diff xmlns=\"urn:ietf:params:xml:ns:xcap-diff\" xcap-root=\"https://csc:4430/\">")!=std::string::npos &&
     xd.find("<document new-etag=\"e1\" sel=\"org.3gpp.mcptt.ue-init-config/users/sip:a/a\"/>")!=std::string::npos);
  CK("xcap-diff no etag",CspXcapDiffDocBody("r","s","").find("<document sel=\"s\"/>")!=std::string::npos);
  // 바뀐 문서만 싣는다(RFC 5874) — 사용자 문서 = MCPTT·MCVideo user profile, MCVideo service config 는 전역 문서(S19 VCMS-1·CMS-5)
  const std::string xu = CspXcapDiffDocsBody("r",{CspMcpttUserProfileSel("+8250"),CspMcVideoUserProfileSel("+8250")},"");
  CK("user docs body",xu.find("<document sel=\"org.3gpp.mcptt.user-profile/users/tel:+8250/user-profile\"/>")!=std::string::npos &&
     xu.find("<document sel=\"org.3gpp.mcvideo.user-profile/users/tel:+8250/mcvideo-user-profile-1.xml\"/>")!=std::string::npos &&
     xu.find("service-config")==std::string::npos);
  CK("mcvideo service config sel",CspMcVideoServiceConfigSel()=="org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml");
  // 규격형 구독 본문(TS 24.481 §6.3.13.2.1 a) — resource-lists entry → XCAP root 뒤 경로, multipart 안이어도)
  const std::string rl = "--b\r\nContent-Type: application/resource-lists+xml\r\n\r\n<?xml version=\"1.0\"?>"
    "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\"><list>"
    "<entry uri=\"org.openmobilealliance.groups/global/byGroupID/tel:g-0a1b2c3d\"/>"
    "<rl:entry uri='https://csc:4430/org.openmobilealliance.groups/global/byGroupID/sip%3Ag2%40ptt.example'/>"
    "<entry uri=\"https://csc:4430/org.3gpp.mcptt.user-profile/users/sip:a@d/mcptt-user-profile-1.xml\"/>"
    "<entry uri=\"https://other/no-auid\"/></list></resource-lists>\r\n--b\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n"
    "<mcpttinfo><mcptt-Params><mcptt-access-token>x</mcptt-access-token></mcptt-Params></mcpttinfo>\r\n--b--\r\n";
  const auto ents = CspXcapDiffEntries(rl);
  CK("entries parsed (relative · absolute · prefixed tag · non-AUID dropped)",ents.size()==3 &&
     ents[0]=="org.openmobilealliance.groups/global/byGroupID/tel:g-0a1b2c3d" &&
     ents[1]=="org.openmobilealliance.groups/global/byGroupID/sip%3Ag2%40ptt.example" &&
     ents[2]=="org.3gpp.mcptt.user-profile/users/sip:a@d/mcptt-user-profile-1.xml");
  CK("group doc match by group ID",CspXcapSelIsGroupDoc(ents[0],"g-0a1b2c3d") && CspXcapSelIsGroupDoc(ents[1],"g2") &&
     !CspXcapSelIsGroupDoc(ents[0],"g-0a1b2c3e") && !CspXcapSelIsGroupDoc(ents[2],"a"));
  CK("auid match",CspXcapSelIsAuid(ents[2],"org.3gpp.mcptt.user-profile") && !CspXcapSelIsAuid(ents[2],"org.3gpp.mcptt.user") &&
     !CspXcapSelIsAuid(ents[0],"org.3gpp.mcptt.user-profile"));
  const std::string xr = CspXcapDiffDocsBody("r",{"org.openmobilealliance.groups/global/byGroupID/tel:g1"},"e9",true);
  CK("removed document = previous-etag only (RFC 5874 §3)",xr.find("<document previous-etag=\"e9\" sel=")!=std::string::npos &&
     xr.find("new-etag")==std::string::npos);
  CK("no body entries",CspXcapDiffEntries("").empty() && CspXcapDiffEntries("<presence entity=\"x\"/>").empty());
  CK("mcptt service config sel",CspMcpttServiceConfigSel("+8250")=="org.3gpp.mcptt.service-config/users/tel:+8250/service-config");
  printf("%s (%d fail)\n",fail?"FAIL":"PASS",fail); return fail?1:0;
}
