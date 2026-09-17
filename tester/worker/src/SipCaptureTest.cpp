// 워커 SIP 캡처 단위시험 — psip LOG_NETWORK 줄 파싱(transport·방향·상대·Call-ID, 축약 헤더, 8 KB 잘림)과 버퍼(take/drop·호당 상한).
//   S1-UNIT-TESTER 의 네이티브 항목.
#include <cstdio>
#include <string>

#include "SipCapture.h"

static int g_fail = 0;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail++; }

int main() {
    SipCapturedMessage m;
    std::string cid;
    const char* l1 = "[140234] UdpSend(10.0.0.5:5060) \n[INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP 1.1.1.1\r\nCall-ID: abc-123@host\r\nCSeq: 1 INVITE\r\n\r\nv=0\r\n]";
    check(SipCapture::Parse(l1, m, cid) && m.tx && m.transport == "udp" && m.peer == "10.0.0.5:5060" && cid == "abc-123@host" &&
              m.text.rfind("INVITE sip:b@x", 0) == 0, "UdpSend — 방향·transport·상대·Call-ID");
    const char* l2 = "TlsRecv(10.0.0.5:5061) \n[SIP/2.0 200 OK\r\ni: short-form-id\r\nl: 0\r\n\r\n]";
    check(SipCapture::Parse(l2, m, cid) && !m.tx && m.transport == "tls" && cid == "short-form-id", "TlsRecv — 축약 헤더 i:");
    const char* l3 = "[1] TcpSend(10.0.0.5:5060) \n[BYE sip:b@x SIP/2.0\r\nCall-ID:   spaced-id  \r\nCSeq: 2 BYE\r\n\r\n";   // 닫는 ] 없음(잘림)
    check(SipCapture::Parse(l3, m, cid) && cid == "spaced-id" && m.transport == "tcp", "잘린 메시지·값 앞뒤 공백");
    check(!SipCapture::Parse("[1] UdpRecv(1.1.1.1:5060) \n[\r\n\r\n]", m, cid), "keepalive(CRLF)는 버린다");
    check(!SipCapture::Parse("[1] EventCallStart(x)", m, cid), "SIP 가 아닌 로그는 버린다");
    // 본문의 Call-ID 비슷한 줄은 헤더가 아니다(빈 줄 뒤)
    const char* l4 = "UdpRecv(1.1.1.1:5060) \n[MESSAGE sip:b@x SIP/2.0\r\nFrom: a\r\n\r\nCall-ID: in-body\r\n]";
    check(!SipCapture::Parse(l4, m, cid), "본문 안의 Call-ID 는 헤더로 보지 않는다");

    SipCapture cap;   // install 하지 않고(전역 CLog 를 건드리지 않는다) Print 만 직접 — OFF 면 무시
    cap.Print(LOG_NETWORK, "%s", l1);
    check(cap.calls() == 0, "mode=off — 붙잡지 않는다");
    return g_fail == 0 ? (printf("PASS\n"), 0) : (printf("FAIL\n"), 1);
}
