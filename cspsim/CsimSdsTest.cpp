// libcsim MCData SDS 코덱 단위시험 — 그룹/1:1 SDS·NOTIFICATION 본문 왕복(TS 24.282 §15 TLV, base64 파트), conversation ID 가 단말(Java
//   UUID.nameUUIDFromBytes)·CSP 와 같은 값인지, MCData 가 아닌 본문은 false. S1-UNIT-TESTER 의 네이티브 항목.
#include <cstdio>
#include <string>

#include "McDataSds.h"

using namespace csim_mcdata;

int main() {
    bool all = true;
    auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); all = all && ok; };
    // Java UUID.nameUUIDFromBytes("cims-mcdata:g001") — python: md5 → version 3·variant 비트
    check(conversationIdOf("g001") == "5a78397f798b38f295168af7f9d22be3", "group conversation id matches Java nameUUIDFromBytes");
    check(conversationIdOneToOne("+8210002", "+8210001") == conversationIdOneToOne("+8210001", "+8210002"), "1:1 conversation id is pair-symmetric");
    std::string mid = newMessageId();
    check(mid.size() == 32 && mid[12] == '4', "message id is UUID v4 hex32");

    Body g = buildGroupSds("g001", "hello group", conversationIdOf("g001"), mid, true, 1700000000);
    SdsMsg m;
    check(parse(g.contentType, g.body, m), "group sds parses");
    check(m.requestType == "group-sds" && m.requestUri == "tel:g001", "group sds request-type/uri");
    check(m.text == "hello group" && m.msgId == mid && m.convId == conversationIdOf("g001") && m.timeSec == 1700000000 && m.dispositionReq == 1,
          "group sds fields round-trip (text·ids·time·delivery request)");
    check(!m.notification && !m.fd, "group sds is neither notification nor fd");

    Body o = buildOneToOneSds("+8210002", "hi", conversationIdOneToOne("+8210001", "+8210002"), mid, false, 1700000001);
    SdsMsg m2;
    check(parse(o.contentType, o.body, m2), "1:1 sds parses");
    check(m2.requestType == "one-to-one-sds" && m2.requestUri == "tel:+8210002" && m2.text == "hi" && m2.dispositionReq == 0, "1:1 sds fields");

    Body n = buildNotification(m.convId, m.msgId, kNotifDelivered, 1700000002);
    SdsMsg m3;
    check(parse(n.contentType, n.body, m3), "notification parses");
    check(m3.notification && m3.notifType == kNotifDelivered && m3.msgId == mid && m3.convId == m.convId, "notification fields");

    SdsMsg m4;
    check(!parse("text/plain", "hello", m4), "plain text is not mcdata");
    check(!parse("multipart/mixed;boundary=x", "--x\r\nContent-Type: text/plain\r\n\r\nhi\r\n--x--\r\n", m4), "multipart without signalling is not mcdata");
    printf(all ? "PASS\n" : "FAIL\n");
    return all ? 0 : 1;
}
