// CSP 제휴 pidf 단위시험 — csp/McpttInfo.h `ParsePidfAffiliation` / `McpttBareId` / `McpttIdUri` / `McpttGroupUri` /
//   `BuildPidfAffiliationInfo`.
//   규격형 제휴 PUBLISH(Event: presence)의 본문 application/pidf+xml (TS 24.379 §9.2.2.2.3 조건 5,
//   §9.3.1.2) 에서 entity(MCPTT ID)·tuple@id(MCPTT client ID)·p-id·affiliation@group 집합을 뽑고,
//   제휴 상태 NOTIFY 본문(§9.2.2.2.5 3), per-user affiliation information)을 만든다.
//   namespace prefix 무관 매칭과, 접두가 겹치는 이름·속성(<affiliationX>, groupStatus=)·종료태그를
//   오매칭하지 않는지가 요점이다(외부 XML 파서 의존 없이 태그 경계로만 판정하므로).
//   빌드·실행은 S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py)가 수행한다.
#include "McpttInfo.h"
#include <cstdio>
static int fail=0;
#define CK(n,c) do{ if(!(c)){printf("FAIL %s\n",n);fail++;}else printf("ok   %s\n",n);}while(0)
int main(){
  auto a=ParsePidfAffiliation(
    "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" entity=\"sip:+8250000006@ptt.d\">"
    "<tuple id=\"urn:uuid:27be\"><status>"
    "<mcpttPI10:affiliation group=\"sip:g001@ptt.d\" status=\"affiliated\"/>"
    "<affiliation group='sip:g002@ptt.d'/>"
    "</status></tuple></presence>");
  CK("valid",a.bValid); CK("entity",a.strEntity=="sip:+8250000006@ptt.d");
  CK("client",a.strClientId=="urn:uuid:27be"); CK("groups=2",a.vecGroups.size()==2);
  CK("g1",a.vecGroups[0]=="sip:g001@ptt.d"); CK("g2",a.vecGroups[1]=="sip:g002@ptt.d");
  auto b=ParsePidfAffiliation("<presence entity=\"x\"><tuple id=\"c\"><status></status></tuple></presence>");
  CK("empty set",b.bValid && b.vecGroups.empty());
  CK("no pidf",!ParsePidfAffiliation("<affiliation-command/>").bValid);
  CK("bare sip",McpttBareId("sip:g001@ptt.d")=="g001");
  CK("bare tel",McpttBareId("tel:+82500000006")=="+82500000006");
  CK("bare plain",McpttBareId("g001")=="g001");
  CK("bare params",McpttBareId("<sip:g001@ptt.d;user=phone>")=="g001");
  // 종료태그·유사이름 배제
  auto c=ParsePidfAffiliation("<presence entity=\"x\"><affiliationX group=\"zz\"/></affiliation><affiliation group=\"sip:g9@d\"/></presence>");
  CK("no false match",c.vecGroups.size()==1 && c.vecGroups[0]=="sip:g9@d");
  // groupStatus 오매칭 방지
  auto d=ParsePidfAffiliation("<presence entity=\"x\"><affiliation groupStatus=\"nope\" group=\"sip:gA@d\"/></presence>");
  CK("attr boundary",d.vecGroups.size()==1 && d.vecGroups[0]=="sip:gA@d");
  // p-id (§9.3.1.2 4)) — prefix 무관·공백 제거, 없거나 빈 요소면 빈 값
  auto e=ParsePidfAffiliation("<presence entity=\"x\"><tuple id=\"c\"><status/></tuple><mcpttPI10:p-id> pub-77 </mcpttPI10:p-id></presence>");
  CK("p-id",e.strPid=="pub-77");
  CK("p-id absent",a.strPid.empty());
  CK("p-id empty elem",ParsePidfAffiliation("<presence entity=\"x\"><p-id/></presence>").strPid.empty());
  // MCPTT ID·그룹 ID URI (CSC _group_uri 와 같은 규칙)
  CK("id uri",McpttIdUri("+82500000006")=="tel:+82500000006");
  CK("id uri keep",McpttIdUri("sip:a@d")=="sip:a@d");
  CK("group uri",McpttGroupUri("g001")=="tel:g001");
  CK("group uri digits",McpttGroupUri("5001")=="tel:+5001");
  CK("group uri plus",McpttGroupUri("+5001")=="tel:+5001");
  CK("group uri keep",McpttGroupUri("tel:g001")=="tel:g001");
  // 제휴 상태 NOTIFY 본문 — tuple = 클라이언트마다, 빈 클라이언트 생략, expires 는 있을 때만, p-id 는 tuple 뒤
  std::vector<CMcpttAffClient> v(3);
  v[0].strClientId="RoT_MCX_1"; v[0].vecGroups={{"tel:g001","2026-09-30T05:00:00Z"},{"tel:g002",""}};
  v[1].strClientId="empty";
  v[2].strClientId="sip:+82500000006@10.0.0.1:5070;ob"; v[2].vecGroups={{"tel:g003",""}};
  std::string n=BuildPidfAffiliationInfo(McpttIdUri("+82500000006"),v,"pub-77");
  CK("ns pidf",n.find("xmlns=\"urn:ietf:params:xml:ns:pidf\"")!=std::string::npos);
  CK("ns presinfo",n.find("xmlns:mcpttPI10=\"urn:3gpp:ns:mcpttPresInfo:1.0\"")!=std::string::npos);
  CK("entity attr",n.find("entity=\"tel:+82500000006\"")!=std::string::npos);
  CK("tuple 2",n.find("<tuple id=\"RoT_MCX_1\">")!=std::string::npos && n.find("id=\"empty\"")==std::string::npos);
  CK("group+status+expires",n.find("<mcpttPI10:affiliation group=\"tel:g001\" status=\"affiliated\" expires=\"2026-09-30T05:00:00Z\"/>")!=std::string::npos);
  CK("no expires",n.find("group=\"tel:g002\" status=\"affiliated\"/>")!=std::string::npos);
  CK("p-id after tuples",n.find("<mcpttPI10:p-id>pub-77</mcpttPI10:p-id>")>n.rfind("</tuple>"));
  CK("no legacy root",n.find("mcptt-affiliation-info")==std::string::npos);
  auto r=ParsePidfAffiliation(n);   // 우리 파서로 되읽기(단말이 보는 값)
  CK("roundtrip",r.bValid && r.strEntity=="tel:+82500000006" && r.strClientId=="RoT_MCX_1" && r.strPid=="pub-77" &&
                 r.vecGroups.size()==3 && r.vecGroups[0]=="tel:g001");
  std::vector<CMcpttAffClient> esc(1); esc[0].strClientId="a&\"b"; esc[0].vecGroups={{"tel:g<1>",""}};
  std::string en=BuildPidfAffiliationInfo("tel:+1",esc,"");
  CK("escape",en.find("id=\"a&amp;&quot;b\"")!=std::string::npos && en.find("tel:g&lt;1&gt;")!=std::string::npos &&
             en.find("p-id")==std::string::npos);
  CK("no clients",BuildPidfAffiliationInfo("tel:+1",{},"").find("<tuple")==std::string::npos);
  printf("%s (%d fail)\n",fail?"FAIL":"PASS",fail); return fail?1:0;
}
