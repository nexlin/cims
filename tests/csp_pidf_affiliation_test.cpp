// CSP 제휴 pidf 파싱 단위시험 — csp/McpttInfo.h `ParsePidfAffiliation` / `McpttBareId`.
//   규격형 제휴 PUBLISH(Event: presence)의 본문 application/pidf+xml (TS 24.379 §9.2.2.2.3 조건 5,
//   §9.3.1.2) 에서 entity(MCPTT ID)·tuple@id(MCPTT client ID)·affiliation@group 집합을 뽑는다.
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
  printf("%s (%d fail)\n",fail?"FAIL":"PASS",fail); return fail?1:0;
}
