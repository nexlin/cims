// libcsim MCData SDS 코덱 단위시험 — 그룹/1:1 SDS·NOTIFICATION·FD SIGNALLING 본문 왕복(TS 24.282 §15 TLV, base64 파트), conversation ID 가 단말(Java
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

    // FD SIGNALLING(TS 24.282 §15.1.3) — 그룹/1:1 FD 본문 왕복(FILEURL·Metadata IE), 선택 TV IE 를 건너뛰고 읽는가, CSP 폴백 빌더(group-sds + FD TLV)도 fd 로
    Body fg = buildGroupFd("g001", "https://10.0.0.5:4430/mcdata/fd/abc123", "photo \"1\".jpg", 123456, "image/jpeg", conversationIdOf("g001"), mid, 1700000003);
    SdsMsg f1;
    check(parse(fg.contentType, fg.body, f1), "group fd parses");
    check(f1.fd && !f1.notification && f1.requestType == "group-fd" && f1.requestUri == "tel:g001", "group fd request-type/uri");
    check(f1.fileUrl == "https://10.0.0.5:4430/mcdata/fd/abc123" && f1.fileName == "photo 1.jpg" && f1.fileSize == 123456 && f1.fileType == "image/jpeg" &&
              f1.msgId == mid && f1.convId == conversationIdOf("g001") && f1.timeSec == 1700000003 && f1.text.empty(),
          "group fd FILEURL·Metadata round-trip (quotes stripped from name)");
    Body fo = buildOneToOneFd("+8210002", "http://h/mcdata/fd/x", "a.bin", 1, "application/octet-stream", conversationIdOneToOne("+8210001", "+8210002"), mid, 1);
    SdsMsg f2;
    check(parse(fo.contentType, fo.body, f2) && f2.fd && f2.requestType == "one-to-one-fd" && f2.requestUri == "tel:+8210002" && f2.fileUrl == "http://h/mcdata/fd/x" &&
              f2.fileSize == 1, "1:1 fd fields");
    {
        // 선택 IE 앞에 놓인 FD disposition 요청(0x91)·mandatory download(0xA1)·InReplyTo(0x21 + 16B)·Application ID(0x22 + 1B) 를 건너뛴다
        std::string tlv = fdSignallingTlv(conversationIdOf("g001"), mid, "http://h/f", "n", 7, "t/x", 5);
        std::string extra; extra += (char)0x91; extra += (char)0xA1; extra += (char)0x21; extra.append(16, 'z'); extra += (char)0x22; extra += (char)0x01;
        tlv.insert(38, extra);
        std::string body = "--b\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\nContent-Transfer-Encoding: base64\r\n\r\n" + base64Encode(tlv) + "\r\n--b--\r\n";
        SdsMsg f3;
        check(parse("multipart/mixed;boundary=b", body, f3) && f3.fd && f3.fileUrl == "http://h/f" && f3.fileName == "n" && f3.fileSize == 7 && f3.fileType == "t/x",
              "fd optional TV IEs are skipped before Payload/Metadata");
    }

    SdsMsg m4;
    check(!parse("text/plain", "hello", m4), "plain text is not mcdata");
    check(!parse("multipart/mixed;boundary=x", "--x\r\nContent-Type: text/plain\r\n\r\nhi\r\n--x--\r\n", m4), "multipart without signalling is not mcdata");
    printf(all ? "PASS\n" : "FAIL\n");
    return all ? 0 : 1;
}
