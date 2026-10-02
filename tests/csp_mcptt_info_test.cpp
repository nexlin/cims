// CSP mcptt-info 파싱 단위시험 — csp/McpttInfo.h `ParseMcpttInfo` / `McpttElemValue`.
//   TS 24.379 Annex F.1 의 contentType 요소(mcptt-request-uri·alert-ind 등)는 값을 자식 <mcpttURI>/<mcpttString>/
//   <mcpttBoolean> 에 싣는다. 그 형식과 값을 요소에 바로 적는 형식(현행 단말) 둘 다 읽는지, 이름이 겹치는
//   요소(<alert-ind-rcvd>)를 <alert-ind> 로 오인하지 않는지, 접두사·엔티티를 처리하는지가 요점이다.
//   `McpttPsiTarget` — 참여 기능 PSI 로 온 개시 INVITE 의 대상(mcptt-request-uri, TS 24.379 §10.1.1.2.1.1·§11.1.1.2.1.1).
//   빌드·실행은 S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py)가 수행한다.
#include "McpttInfo.h"
#include <cstdio>
static int fail=0;
#define CK(n,c) do{ if(!(c)){printf("FAIL %s\n",n);fail++;}else printf("ok   %s\n",n);}while(0)
int main(){
  // 현행 형식 — 값 직접 기재 (SDK alertInfo)
  auto a=ParseMcpttInfo(
    "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params>"
    "<mcptt-request-uri>tel:g005</mcptt-request-uri>"
    "<mcptt-calling-user-id>tel:+82500000013</mcptt-calling-user-id>"
    "<alert-ind>true</alert-ind><mcptt-client-id>urn:uuid:1</mcptt-client-id>"
    "</mcptt-Params></mcpttinfo>");
  CK("flat alert",a.bHasAlertInd && a.bAlert);
  CK("flat request-uri",a.strRequestUri=="tel:g005" && McpttBareId(a.strRequestUri)=="g005");
  CK("flat calling",a.strCallingUserId=="tel:+82500000013");
  CK("flat client",a.strClientId=="urn:uuid:1");
  CK("flat no emergency-ind",!a.bHasEmergencyInd);
  // 규격 형식 — contentType 자식 + 접두사
  auto b=ParseMcpttInfo(
    "<mi:mcpttinfo xmlns:mi=\"urn:3gpp:ns:mcpttInfo:1.0\"><mi:mcptt-Params>"
    "<mi:mcptt-request-uri type=\"Normal\"><mi:mcpttURI>sip:g005@ptt.d</mi:mcpttURI></mi:mcptt-request-uri>"
    "<mi:emergency-ind><mi:mcpttBoolean>false</mi:mcpttBoolean></mi:emergency-ind>"
    "<mi:alert-ind><mi:mcpttBoolean>false</mi:mcpttBoolean></mi:alert-ind>"
    "<mi:originated-by><mi:mcpttURI>tel:+8250&amp;1</mi:mcpttURI></mi:originated-by>"
    "</mi:mcptt-Params></mi:mcpttinfo>");
  CK("wrapped alert-ind present",b.bHasAlertInd && !b.bAlert);
  CK("wrapped request-uri",McpttBareId(b.strRequestUri)=="g005");
  CK("wrapped emergency-ind false",b.bHasEmergencyInd && !b.bEmergency);
  CK("entity",b.strOriginatedBy=="tel:+8250&1");
  auto c=ParseMcpttInfo("<mcpttinfo><mcptt-Params><alert-ind><mcpttBoolean>true</mcpttBoolean></alert-ind></mcptt-Params></mcpttinfo>");
  CK("wrapped true",c.bAlert);
  // 이름 경계 — alert-ind-rcvd 는 alert-ind 가 아니다
  auto d=ParseMcpttInfo("<mcpttinfo><mcptt-Params><alert-ind-rcvd>true</alert-ind-rcvd></mcptt-Params></mcpttinfo>");
  CK("rcvd not alert",!d.bHasAlertInd && !d.bAlert);
  // 기존 호출부 — session-type·condition
  auto e=ParseMcpttInfo("<mcpttinfo><mcptt-Params><session-type>prearranged</session-type>"
                        "<emergency-ind>true</emergency-ind><broadcast-ind>true</broadcast-ind></mcptt-Params></mcpttinfo>");
  CK("session-type",e.strSessionType=="prearranged");
  CK("condition",e.Condition()==2 && e.bBroadcast);
  CK("empty",!ParseMcpttInfo("").bHasAlertInd);
  std::string v;
  CK("self-closing",McpttElemValue("<x><alert-ind/></x>","alert-ind",v) && v.empty());
  // 생성(F.1 contentType) → 해석 왕복
  std::string body = "<mcpttinfo><mcptt-Params>" + McpttInfoValue("session-type","prearranged") +
                     McpttInfoUri("mcptt-request-uri","tel:g0&1") + McpttInfoBool("emergency-ind",true) +
                     McpttInfoString("mcptt-client-id","urn:uuid:9") + "</mcptt-Params></mcpttinfo>";
  CK("builder uri form",body.find("<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:g0&amp;1</mcpttURI></mcptt-request-uri>")!=std::string::npos);
  CK("builder bool form",body.find("<emergency-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></emergency-ind>")!=std::string::npos);
  auto f=ParseMcpttInfo(body);
  CK("round trip",f.strSessionType=="prearranged" && f.strRequestUri=="tel:g0&1" && f.bEmergency && f.strClientId=="urn:uuid:9");
  // PSI 대상 — 회명 HM-TRCP 실측 본문(Request-URI mcptt1_opf_psi, mcptt-request-uri 값 직접 기재)
  auto g=ParseMcpttInfo("<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params><session-type>prearranged</session-type>"
                        "<mcptt-request-uri>tel:g006</mcptt-request-uri></mcptt-Params></mcpttinfo>");
  CK("psi group",McpttPsiTarget("mcptt1_opf_psi",g.strRequestUri)=="g006");
  CK("psi private",McpttPsiTarget("mcptt_psi","tel:+82500000002")=="+82500000002");
  CK("psi sip uri",McpttPsiTarget("mcptt_psi","sip:g001@ptt.d")=="g001");
  CK("legacy same",McpttPsiTarget("g006","tel:g006").empty());       // 구형: Request-URI 에 그룹을 직접
  CK("no request-uri",McpttPsiTarget("mcptt_psi","").empty());
  // 긴급 경보 서비스 판별 (McEmergencyAlertServiceOf — TS 24.379 §12.1 · TS 24.282 §16.2) — MCData 경보(mcdata-info
  //   <alert-ind>)는 MCPTT 경보로 읽지 않는다(S03 MCData EMG-1). 본문 = 단말 §16.2.1.1 4)·5) 형식(mcdata-info +
  //   mcdata-location-info multipart).
  const std::string mdAlert =
    "--b\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n"
    "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\"><mcdata-Params>"
    "<mcdata-request-uri type=\"Normal\"><mcdataURI>sip:gap1@ptt.d</mcdataURI></mcdata-request-uri>"
    "<alert-ind type=\"Normal\"><mcdataBoolean>true</mcdataBoolean></alert-ind></mcdata-Params></mcdatainfo>\r\n"
    "--b\r\nContent-Type: application/vnd.3gpp.mcdata-location-info+xml\r\n\r\n<location-info/>\r\n--b--\r\n";
  CK("alert svc mcdata",McEmergencyAlertServiceOf(mdAlert,"multipart/mixed")==EMcAlertService::McData);
  const std::string mdSds =
    "--b\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n"
    "<mcdatainfo><mcdata-Params><mcdata-request-uri>tel:gap1</mcdata-request-uri></mcdata-Params></mcdatainfo>\r\n"
    "--b\r\nContent-Type: application/vnd.3gpp.mcdata-payload\r\n\r\n<alert-ind>true</alert-ind>\r\n--b--\r\n";
  CK("alert svc mcdata sds (payload text not alert)",McEmergencyAlertServiceOf(mdSds,"multipart/mixed")==EMcAlertService::None);
  CK("alert svc mcptt",McEmergencyAlertServiceOf(McpttInfoDocument(McpttInfoBool("alert-ind",true)),
                                                 "application/vnd.3gpp.mcptt-info+xml")==EMcAlertService::Mcptt);
  CK("alert svc mcptt emergency cancel",McEmergencyAlertServiceOf(McpttInfoDocument(McpttInfoBool("emergency-ind",false)),
                                                 "application/vnd.3gpp.mcptt-info+xml")==EMcAlertService::Mcptt);
  CK("alert svc mcptt legacy text/plain",McEmergencyAlertServiceOf("<mcpttinfo><mcptt-Params><alert-ind>true</alert-ind>"
                                                 "</mcptt-Params></mcpttinfo>","text/plain")==EMcAlertService::Mcptt);
  CK("alert svc none",McEmergencyAlertServiceOf("hello","text/plain")==EMcAlertService::None);
  // 정원 안 초대 대상 (McpttCapInvitees — TS 24.379 §6.3.5.5, S01 GCS-5): 필수 멤버 먼저, 나머지는 순서대로
  { bool cap=false;
    auto v=McpttCapInvitees({"a","b","c","d"},{"c"},2,cap);
    CK("cap required first",cap && v.size()==2 && v[0]=="c" && v[1]=="a");
    v=McpttCapInvitees({"a","b"},{},2,cap);
    CK("cap fits",!cap && v.size()==2);
    v=McpttCapInvitees({"a","b","c"},{"b","c"},1,cap);
    CK("cap only required kept",cap && v.size()==1 && v[0]=="b");
    v=McpttCapInvitees({"a","b"},{},-1,cap);
    CK("cap unlimited",!cap && v.size()==2);
    v=McpttCapInvitees({"a"},{},0,cap);
    CK("cap zero slots",cap && v.empty()); }
  // floor 우선순위 협상값 (McpttNegotiatedFloorPriority — TS 24.380 §14.3.3, S06 SDP-3): offer 를 그룹 문서 값 위로 못 올린다
  CK("prio offer capped by user-priority",McpttNegotiatedFloorPriority(255,5,256)==5);
  CK("prio offer lower kept",McpttNegotiatedFloorPriority(2,5,256)==2);
  CK("prio capped by levels",McpttNegotiatedFloorPriority(9,9,4)==4);
  CK("prio not offered",McpttNegotiatedFloorPriority(0,9,256)==0);
  printf("%s (%d fail)\n",fail?"FAIL":"PASS",fail); return fail?1:0;
}
