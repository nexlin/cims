// CSP MCData 본문 해석 단위시험 — csp/McDataCodec.cpp `McDataParseBody` 의 disposition 통지 규격 경로 요소
//   (TS 24.282 V18.13.0 §12.2.1.1 — 단말 통지 본문, §12.2.3 — 제어 기능 중계, mcdata_messaging.md §4.4).
//   SDS NOTIFICATION(type 0x05) 해석 · application/resource-lists+xml 의 <entry uri> 목록 · mcdata-info 의
//   <mcdata-calling-group-id>·<mcdata-calling-user-id>(이름 경계) · mcdata-signalling 파트 원문 보존(중계가 그대로 옮긴다) ·
//   resource-lists 없는 옛 형식 구분이 요점이다. 빌드·실행은 S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py).
#include "McDataCodec.h"
#include <cstdio>
#include <string>
static int fail=0;
#define CK(n,c) do{ if(!(c)){printf("FAIL %s\n",n);fail++;}else printf("ok   %s\n",n);}while(0)
static std::string b64(const std::string& in){
  static const char* t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o; int v=0,b=-6;
  for(unsigned char c:in){ v=(v<<8)+c; b+=8; while(b>=0){ o+=t[(v>>b)&0x3F]; b-=6; } }
  if(b>-6) o+=t[((v<<8)>>(b+8))&0x3F];
  while(o.size()%4) o+='=';
  return o;
}
int main(){
  // SDS NOTIFICATION — [type 0x05][DELIVERED 0x02][date-time 5][conv 16][msg 16]
  std::string sig("\x05\x02", 2); sig += std::string(5,'\0');
  for(int i=0;i<16;i++) sig += (char)(0x10+i);
  for(int i=0;i<16;i++) sig += (char)(0xA0+i);
  const std::string sigPart = "Content-Type: application/vnd.3gpp.mcdata-signalling\r\nContent-Transfer-Encoding: base64\r\n\r\n" + b64(sig);
  const std::string body =
    "--bb\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n"
    "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\"><mcdata-Params>"
    "<mcdata-calling-user-id-x type=\"Normal\"><mcdataURI>tel:+829999</mcdataURI></mcdata-calling-user-id-x>"
    "<mcdata-calling-group-id type=\"Normal\"><mcdataURI>tel:g005</mcdataURI></mcdata-calling-group-id>"
    "</mcdata-Params></mcdatainfo>\r\n"
    "--bb\r\n" + sigPart + "\r\n"
    "--bb\r\nContent-Type: application/resource-lists+xml\r\nContent-Disposition: recipient-list\r\n\r\n"
    "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\"><list><entry uri=\"tel:+82500000013\"/><entry uri='sip:+82500000014@d'/></list></resource-lists>\r\n"
    "--bb--\r\n";
  CMcDataSdsInfo i;
  CK("parse", McDataParseBody("multipart/mixed;boundary=bb", body, i));
  CK("SDS NOTIFICATION type", i.m_iMsgType==MCDATA_MSG_SDS_NOTIFICATION && i.m_iNotifType==MCDATA_NOTIF_DELIVERED);
  CK("conv/msg id hex", i.m_strConvId=="101112131415161718191a1b1c1d1e1f" && i.m_strMsgId=="a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
  CK("resource-lists present", i.m_bHasResourceLists);
  CK("resource-lists entries (both quote styles)", i.m_vecListUris.size()==2 && i.m_vecListUris[0]=="tel:+82500000013" && i.m_vecListUris[1]=="sip:+82500000014@d");
  CK("calling-group-id", i.m_strCallingGroupId=="tel:g005");
  CK("calling-user-id name boundary", i.m_strCallingUserId.empty());
  CK("signalling part raw kept", i.m_strSignallingPart==sigPart);
  // 옛 형식 — resource-lists 없음
  const std::string old = "--bb\r\n" + sigPart + "\r\n--bb--\r\n";
  CMcDataSdsInfo j;
  CK("old form parse", McDataParseBody("multipart/mixed;boundary=bb", old, j));
  CK("old form: no resource-lists", !j.m_bHasResourceLists && j.m_vecListUris.empty());
  printf("%s (%d fail)\n",fail?"FAIL":"PASS",fail); return fail?1:0;
}
