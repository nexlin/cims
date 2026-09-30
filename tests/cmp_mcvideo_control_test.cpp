// cmp_mcvideo_control_test.cpp — CMP MCVideo 전송 제어 서버 상태 머신(cmp/PMcvControl.cpp — TS 24.581 §6.3.4~§6.3.7) 단위시험.
//
// 빌드: g++ -std=c++17 -Icmp tests/cmp_mcvideo_control_test.cpp cmp/PMcvControl.cpp cmp/PTransmissionCodec.cpp -o /tmp/mcvctl
// (S1-UNIT-CMP 가 같은 방식으로 링크한다 — 외부 의존 없음)
//
// 훅 send 가 낸 메시지는 전부 BuildTransmissionMessage → ParseTransmissionMessage 로 한 번 왕복시킨다 — 상태 머신이 메시지 표 밖
// 필드를 싣거나 길이를 틀리면 Build 가 실패해 여기서 드러난다(PTransmissionCodec.h 규약).
#include "PMcvControl.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            ++g_pass;                                                      \
        } else {                                                           \
            ++g_fail;                                                      \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
        }                                                                  \
    } while (0)

struct Sent {
    std::string to;
    ParsedTransmission m;
};

struct Harness {
    PMcvControl ctl;
    std::vector<Sent> out;
    std::vector<std::string> inactivity;
    std::vector<std::vector<McvTransmitter>> txEvents;
    int buildFail = 0;
    int64_t now = 1000;

    Harness(int maxTx = 1, bool automatic = false, McvCallType ct = MCV_CALL_NORMAL, McvTimers t = McvTimers()) {
        PMcvControl::Hooks h;
        h.send = [this](const std::string& to, int app, int subtype, const std::vector<McvTlv>& fields) {
            char buf[512];
            int n = BuildTransmissionMessage(buf, sizeof(buf), app, (unsigned char)subtype, 0xC0FFEE01u, fields);
            if (n <= 0) {
                ++buildFail;
                printf("  build failed: MCV%d %s\n", app, McvMessageName(app, subtype));
                return;
            }
            Sent s;
            s.to = to;
            if (!ParseTransmissionMessage(buf, n, s.m)) {
                ++buildFail;
                return;
            }
            out.push_back(s);
        };
        h.inactivity = [this](const char* timer) { inactivity.push_back(timer); };
        h.transmittersChanged = [this](const std::vector<McvTransmitter>& v) { txEvents.push_back(v); };
        ctl.setHooks(h);
        ctl.configure(maxTx, automatic, ct, t);
    }

    McvParticipantDecl decl(const std::string& uid, int prio = 0) {
        McvParticipantDecl d;
        d.userId = uid;
        d.rosterPriority = prio;
        return d;
    }
    void join(const std::string& id, McvParticipantDecl d) { ctl.addParticipant(id, d, now); }
    void join(const std::string& id) { join(id, decl("sip:" + id + "@mcv")); }

    // 참가자 → 서버 메시지 (MCV0/MCV2) — 실제 바이트로 만들어 해석한 것을 넣는다.
    void rx(const std::string& from, int app, int subtype, const std::vector<McvTlv>& fields = {}) {
        char buf[512];
        int n = BuildTransmissionMessage(buf, sizeof(buf), app, (unsigned char)subtype, 0x0A0B0C0Du, fields);
        ParsedTransmission m;
        if (n <= 0 || !ParseTransmissionMessage(buf, n, m)) {
            ++buildFail;
            return;
        }
        ctl.onMessage(from, m, now);
    }
    void request(const std::string& from, int prio = -1) {
        std::vector<McvTlv> f;
        if (prio >= 0) f.emplace_back(TF_TRANSMISSION_PRIORITY, McvU8(prio));
        rx(from, MCV_APP_0, MCV0_TRANSMISSION_REQUEST, f);
    }
    void endRequest(const std::string& from, bool ack = false) {
        rx(from, MCV_APP_2, MCV2_TRANSMISSION_END_REQUEST | (ack ? MCV_ACK_REQ_BIT : 0));
    }
    void receive(const std::string& from, const std::string& uid) {
        rx(from, MCV_APP_0, MCV0_RECEIVE_MEDIA_REQUEST, { McvTlv(TF_TRANSMITTING_USER_ID, uid) });
    }
    void advance(int ms) {
        // 100 ms 틱(PCmpServer 클록과 같다)으로 나눠 흘린다
        for (int t = 0; t < ms; t += 100) {
            now += 100;
            ctl.tick(now);
        }
    }

    int count(const std::string& to, int app, int op) const {
        int n = 0;
        for (const auto& s : out)
            if (s.to == to && s.m.app == app && s.m.op() == op) ++n;
        return n;
    }
    const ParsedTransmission* last(const std::string& to, int app, int op) const {
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (it->to == to && it->m.app == app && it->m.op() == op) return &it->m;
        return nullptr;
    }
    void clear() { out.clear(); }
};

static void testJoinIdleAndOnlyOne() {
    printf("[join · Idle · only one participant]\n");
    Harness h;
    h.join("A");
    CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 1, "A gets Transmission Idle on join (§6.3.5.2.2 2a)");
    const ParsedTransmission* idle = h.last("A", MCV_APP_1, MCV1_TRANSMISSION_IDLE);
    CHECK(idle && idle->u16(TF_MSG_SEQ) == 1, "Idle carries Message Sequence Number 1");
    CHECK(idle && !idle->has(TF_TRANSMISSION_INDICATOR), "normal call — no Transmission Indicator");
    h.request("A");
    const ParsedTransmission* rej = h.last("A", MCV_APP_1, MCV1_TRANSMISSION_REJECTED);
    CHECK(rej && rej->cause() == TC_REJECT_ONLY_ONE_PARTICIPANT, "single participant request → Rejected #3 (§6.3.4.3.3)");
    h.join("B");
    const ParsedTransmission* idleB = h.last("B", MCV_APP_1, MCV1_TRANSMISSION_IDLE);
    CHECK(idleB && idleB->u16(TF_MSG_SEQ) == 2, "B Idle — sequence increased");
    CHECK(h.ctl.txState("A") == MCV_U_IDLE && h.ctl.txState("B") == MCV_U_IDLE, "both 'not permitted and Transmit Idle'");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testGrantNotifyReceive() {
    printf("[grant · notification · manual reception · T6]\n");
    Harness h;
    McvParticipantDecl a = h.decl("sip:A@mcv");
    a.preferredAudioSsrc = 0x11111111u;
    a.preferredVideoSsrc = 0x22222222u;
    h.join("A", a);
    h.join("B");
    h.join("C");
    h.clear();
    h.request("A");
    const ParsedTransmission* g = h.last("A", MCV_APP_1, MCV1_TRANSMISSION_GRANTED);
    CHECK(g != nullptr, "A gets Transmission Granted");
    CHECK(g && g->ssrcOf(TF_AUDIO_SSRC) == 0x11111111u && g->ssrcOf(TF_VIDEO_SSRC) == 0x22222222u,
          "Granted carries offer a=ssrc pair (no collision — §14.3.7)");
    CHECK(g && g->has(TF_TRANSMISSION_PRIORITY), "Granted carries Transmission Priority");
    const ParsedTransmission* n = h.last("B", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION);
    CHECK(n && n->str(TF_TRANSMITTING_USER_ID) == "sip:A@mcv", "B notified — Transmitting User ID");
    CHECK(n && n->ssrcOf(TF_VIDEO_SSRC) == 0x22222222u && n->ssrcOf(TF_AUDIO_SSRC) == 0x11111111u, "Notification SSRC pair");
    CHECK(n && n->u16(TF_RECEPTION_MODE) == TC_RECEPTION_MANUAL, "Reception Mode manual (1)");
    CHECK(n && n->u16(TF_PERMISSION) == TC_PERM_ALLOWED, "Permission to request = 1");
    CHECK(h.count("C", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1, "C notified too");
    CHECK(h.count("A", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 0, "transmitter not notified of itself");
    CHECK(h.ctl.txState("A") == MCV_U_PERMITTED && h.ctl.txState("B") == MCV_U_TAKEN, "A permitted, B taken");
    CHECK(h.txEvents.size() == 1 && h.txEvents.back().size() == 1 && h.txEvents.back()[0].memberId == "A",
          "TRANSMITTERS event = [A]");

    unsigned int as = 0, vs = 0;
    CHECK(h.ctl.onMedia("A", h.now, as, vs) && as == 0x11111111u && vs == 0x22222222u,
          "A media forwarded with allocated SSRCs");
    CHECK(!h.ctl.receives("B", "A"), "manual — B not in Active SSRC List before request");

    h.clear();
    h.receive("B", "sip:A@mcv");
    const ParsedTransmission* r = h.last("B", MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE);
    CHECK(r && r->u16(TF_RESULT) == TC_RESULT_GRANTED, "Receive Media Response granted");
    CHECK(r && r->ackRequired(), "granted response asks for ack (T6)");
    CHECK(r && r->str(TF_TRANSMITTING_USER_ID) == "sip:A@mcv" && r->ssrcOf(TF_VIDEO_SSRC) == 0x22222222u,
          "response names the transmission");
    CHECK(h.ctl.receives("B", "A") && h.ctl.receptionCount() == 1, "B in Active SSRC List (C7=1)");
    // Ack 없이 T6 — C6(3)까지 재송신
    h.clear();
    h.advance(5000);
    CHECK(h.count("B", MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE) == MCV_C6 - 1, "T6 retransmits until C6 reached");
    // C 는 Ack — 재송신 없음
    h.receive("C", "sip:A@mcv");
    h.rx("C", MCV_APP_2, MCV2_TRANSMISSION_CONTROL_ACK,
         { McvTlv(TF_MSG_TYPE, McvU8(MCV1_RECEIVE_MEDIA_RESPONSE)), McvTlv(TF_SOURCE, McvU16(TC_SRC_PARTICIPANT)),
           McvTlv(TF_MESSAGE_NAME, McvName(MCV_NAME_1)) });
    h.clear();
    h.advance(3000);
    CHECK(h.count("C", MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE) == 0, "Ack stops T6");

    // 수신 종료
    h.clear();
    h.rx("B", MCV_APP_2, MCV2_MEDIA_RECEPTION_END_REQUEST, { McvTlv(TF_TRANSMITTING_USER_ID, "sip:A@mcv") });
    const ParsedTransmission* er = h.last("B", MCV_APP_2, MCV2_MEDIA_RECEPTION_END_RESPONSE);
    CHECK(er && er->ssrcOf(TF_AUDIO_SSRC) == 0x11111111u, "Media Reception End Response names the transmission");
    CHECK(!h.ctl.receives("B", "A") && h.ctl.receives("C", "A"), "B removed, C still receives");
    // 없는 송출
    h.receive("B", "sip:nobody@mcv");
    const ParsedTransmission* nr = h.last("B", MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE);
    CHECK(nr && nr->u16(TF_RESULT) == TC_RESULT_REJECTED && nr->cause() == TC_RECV_REJECT_OTHER,
          "unknown transmission → rejected #255");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testEndAndIdle() {
    printf("[end request · ack · End Notify · Idle · T2]\n");
    Harness h;
    h.join("A");
    h.join("B");
    h.request("A");
    unsigned int as = 0, vs = 0;
    h.ctl.onMedia("A", h.now, as, vs);
    h.clear();
    h.endRequest("A", true);
    const ParsedTransmission* ack = h.last("A", MCV_APP_2, MCV2_TRANSMISSION_CONTROL_ACK);
    CHECK(ack && ack->u8(TF_MSG_TYPE) == MCV2_TRANSMISSION_END_REQUEST && ack->u16(TF_SOURCE) == TC_SRC_CONTROLLING &&
              ack->messageName() == MCV_NAME_2,
          "Ack: Message Type 0 · Source 2 · Message Name MCV2 (§9.2.3.10)");
    const ParsedTransmission* resp = h.last("A", MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE);
    CHECK(resp && resp->ssrcOf(TF_AUDIO_SSRC) == as, "End Response carries the SSRC pair");
    const ParsedTransmission* en = h.last("B", MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY);
    CHECK(en && en->ssrcOf(TF_VIDEO_SSRC) == vs && en->str(TF_TRANSMITTING_USER_ID) == "sip:A@mcv",
          "B gets Transmission End Notify");
    CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 1 && h.count("B", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 1,
          "G: Idle — Idle to all");
    CHECK(h.ctl.transmitterCount() == 0 && h.txEvents.back().empty(), "Cx 0 · TRANSMITTERS []");
    CHECK(!h.ctl.onMedia("A", h.now, as, vs), "media after end not forwarded");
    CHECK(h.ctl.txState("A") == MCV_U_IDLE && h.count("A", MCV_APP_1, MCV1_TRANSMISSION_REVOKED) == 0,
          "in-flight media right after end — dropped without revoke (kEndGraceMs)");
    h.advance(PMcvControl::kEndGraceMs + 100);
    CHECK(!h.ctl.onMedia("A", h.now, as, vs), "still sending after end — dropped");
    CHECK(h.ctl.txState("A") == MCV_U_SENDS_MEDIA, "keeps sending after End Request → Revoked #3 · sends media (§6.3.5.3.8)");
    // SSRC 반환 — 같은 선호값을 다시 받을 수 있다
    unsigned int again = PMcvControl::AllocSsrc(as);
    CHECK(again == as, "transmission SSRC freed at end");
    PMcvControl::FreeSsrc(again);
    // T2/C2 — Idle 재송신 C2 까지
    h.clear();
    h.advance(MCV_C2 * MCV_T2_MS + 2000);
    CHECK(h.count("B", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == MCV_C2 - 1, "T2 retransmits Idle until C2");
    CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_IDLE) >= 1, "A back to Idle after T3 gave up (sends media)");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testLimitQueuePreempt() {
    printf("[limit #1 · queue · pre-emption #4 · T4]\n");
    Harness h(1);
    h.join("A", h.decl("sip:A@mcv", 1));
    McvParticipantDecl c = h.decl("sip:C@mcv", 1);
    c.queueing = true;
    h.join("B", h.decl("sip:B@mcv", 1));
    h.join("C", c);
    h.join("D", h.decl("sip:D@mcv", 9));
    h.request("A");
    h.clear();
    h.request("B");
    const ParsedTransmission* rej = h.last("B", MCV_APP_1, MCV1_TRANSMISSION_REJECTED);
    CHECK(rej && rej->cause() == TC_REJECT_TRANSMISSION_LIMIT, "limit, no queueing → Rejected #1");
    h.request("C");
    const ParsedTransmission* q = h.last("C", MCV_APP_1, MCV1_QUEUE_POSITION_INFO);
    CHECK(q && q->queuePosition() == 1, "limit, queueing → Queue Position Info position 1");
    CHECK(h.ctl.queuePosition("C") == 1, "C queued");
    h.rx("B", MCV_APP_0, MCV0_QUEUE_POSITION_REQUEST);
    const ParsedTransmission* qb = h.last("B", MCV_APP_1, MCV1_QUEUE_POSITION_INFO);
    CHECK(qb && qb->queuePosition() == TC_QUEUE_NOT_QUEUED, "not queued → position 254");

    h.clear();
    h.request("D");   // 로스터 우선순위 9 > 1 — 선점
    const ParsedTransmission* rv = h.last("A", MCV_APP_1, MCV1_TRANSMISSION_REVOKED);
    CHECK(rv && rv->cause() == TC_REVOKE_PREEMPTED, "A revoked #4");
    CHECK(h.ctl.txState("A") == MCV_U_PENDING_REVOKE, "A pending revoke");
    CHECK(h.ctl.queuePosition("D") == 1 && h.ctl.queuePosition("C") == 2, "D in front of the queue");
    unsigned int as = 0, vs = 0;
    CHECK(h.ctl.onMedia("A", h.now, as, vs), "pending revoke — media still forwarded (§6.3.5.6.4)");
    h.clear();
    h.endRequest("A");
    CHECK(h.count("A", MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE) == 1, "A End Response");
    CHECK(h.count("D", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 1, "queue top D granted");
    CHECK(h.count("B", MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY) == 1 &&
              h.count("B", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1,
          "B: End Notify(A) then Notification(D)");
    CHECK(h.count("B", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 0, "no Idle in between (queue not empty)");
    // T4 — 큐에서 허가한 Granted 는 첫 미디어까지 재송신
    h.clear();
    h.advance(1500);
    CHECK(h.count("D", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 1, "T4 retransmits Granted");
    h.ctl.onMedia("D", h.now, as, vs);
    h.clear();
    h.advance(3000);
    CHECK(h.count("D", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 0, "first media stops T4");
    // 이미 허가된 참가자의 재요청 = Granted 재송신
    h.request("D");
    CHECK(h.count("D", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 1, "re-request from permitted → Granted again (§6.3.4.4.8)");
    // D 끝 → 큐의 C
    h.clear();
    h.endRequest("D");
    CHECK(h.count("C", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 1, "next in queue C granted");
    // 대기 취소 — End Response + Notification (파일 머리말의 규격 읽기)
    h.request("B");   // 상한, 큐 미협상 → #1
    McvParticipantDecl bq = h.decl("sip:B@mcv", 1);
    bq.queueing = true;
    h.ctl.addParticipant("B", bq, h.now);   // JOIN ② refresh — 협상 값 갱신
    h.request("B");
    CHECK(h.ctl.queuePosition("B") == 1, "B queued after renegotiation");
    h.clear();
    h.endRequest("B");
    CHECK(h.ctl.queuePosition("B") == 0, "B dequeued by End Request");
    CHECK(h.count("B", MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE) == 1 &&
              h.count("B", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1,
          "cancel → End Response + Notification of current transmission");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testMultiTransmitAndC9() {
    printf("[simultaneous transmissions · C9 #7 · leave]\n");
    Harness h(2);
    h.join("A");
    h.join("B");
    McvParticipantDecl r = h.decl("sip:R@mcv");
    r.maxRxStreams = 1;
    h.join("R", r);
    h.request("A");
    h.request("B");
    CHECK(h.ctl.transmitterCount() == 2, "Cx = 2 (max_transmitters 2)");
    CHECK(h.count("A", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1, "A notified of B");
    auto tx = h.ctl.transmitters();
    CHECK(tx.size() == 2 && tx[0].memberId == "A" && tx[1].memberId == "B", "transmitters in grant order");
    CHECK(tx.size() == 2 && tx[0].audioSsrc != tx[1].audioSsrc && tx[0].videoSsrc != tx[1].videoSsrc,
          "SSRC pairs unique");
    h.receive("R", "sip:A@mcv");
    h.clear();
    h.receive("R", "sip:B@mcv");
    const ParsedTransmission* rr = h.last("R", MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE);
    CHECK(rr && rr->u16(TF_RESULT) == TC_RESULT_REJECTED && rr->cause() == TC_RECV_REJECT_MAX_STREAMS,
          "C9 reached → rejected #7");
    CHECK(h.ctl.receives("R", "A") && !h.ctl.receives("R", "B"), "R receives A only");
    // A 이탈 — End Notify, R 의 수신도 정리
    h.clear();
    h.ctl.removeParticipant("A", h.now);
    CHECK(h.count("R", MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY) == 1 && h.count("B", MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY) == 1,
          "leave → End Notify to the rest");
    CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY) == 0, "nothing sent to the leaver");
    CHECK(!h.ctl.receives("R", "A") && h.ctl.transmitterCount() == 1, "Cx 1 · R's reception cleared");
    h.receive("R", "sip:B@mcv");
    CHECK(h.ctl.receives("R", "B"), "R can now receive B (C9 freed)");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testAutomaticEmergency() {
    printf("[emergency call — automatic reception · indicator]\n");
    Harness h(1, false, MCV_CALL_EMERGENCY);
    h.join("A");
    h.join("B");
    const ParsedTransmission* idle = h.last("B", MCV_APP_1, MCV1_TRANSMISSION_IDLE);
    CHECK(idle && (idle->u16(TF_TRANSMISSION_INDICATOR) & TI_EMERGENCY), "Idle carries emergency indicator");
    h.request("A");
    const ParsedTransmission* g = h.last("A", MCV_APP_1, MCV1_TRANSMISSION_GRANTED);
    CHECK(g && (g->u16(TF_TRANSMISSION_INDICATOR) & TI_EMERGENCY), "Granted carries emergency indicator");
    const ParsedTransmission* n = h.last("B", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION);
    CHECK(n && n->u16(TF_RECEPTION_MODE) == TC_RECEPTION_AUTOMATIC, "Reception Mode automatic (0)");
    CHECK(h.ctl.receives("B", "A"), "automatic — B in Active SSRC List at notification");
    h.join("C");
    CHECK(h.count("C", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1 && h.ctl.receives("C", "A"),
          "late joiner notified and receiving");
    CHECK(h.count("C", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 0, "late joiner gets no Idle while taken");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testTimers() {
    printf("[T1 · T5 · T11 #8 · T3 give-up]\n");
    McvTimers t;
    t.t1Ms = 3000;
    t.t5Ms = 4000;
    Harness h(1, false, MCV_CALL_NORMAL, t);
    h.join("A");
    h.join("B");
    h.advance(3000);
    CHECK(h.inactivity.size() == 1 && h.inactivity[0] == "T1", "T1 expires from call start");
    h.advance(1000);
    CHECK(h.inactivity.size() == 2 && h.inactivity[1] == "T5", "T5 expires from call start");
    h.inactivity.clear();
    h.request("A");
    h.clear();
    h.advance(MCV_T11_MS);
    const ParsedTransmission* er = h.last("A", MCV_APP_2, MCV2_TRANSMISSION_END_REQUEST);
    CHECK(er && er->cause() == TC_REVOKE_NO_RECEIVER, "T11 — server End Request #8 to the transmitter");
    CHECK(h.ctl.txState("A") == MCV_U_PENDING_REVOKE, "A pending revoke");
    CHECK(std::find(h.inactivity.begin(), h.inactivity.end(), std::string("T1")) == h.inactivity.end(),
          "T1 stopped while taken");
    h.rx("A", MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE);
    CHECK(h.ctl.transmitterCount() == 0, "End Response ends the transmission");

    // T3 포기 — 응답 없는 회수는 kT3Retries 뒤 서버에서 끝낸다
    h.request("A");
    h.clear();
    h.advance(MCV_T11_MS);
    h.advance(MCV_T3_MS * (PMcvControl::kT3Retries + 1) + 200);
    CHECK(h.count("A", MCV_APP_2, MCV2_TRANSMISSION_END_REQUEST) == PMcvControl::kT3Retries + 1,
          "End Request sent 1 + kT3Retries times");
    CHECK(h.ctl.transmitterCount() == 0 && h.count("B", MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY) == 1,
          "gave up — ended server-side, End Notify to B");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testUnauthorizedMedia() {
    printf("[media without permission — Revoked #3 · T3]\n");
    Harness h;
    h.join("A");
    h.join("B");
    unsigned int as = 0, vs = 0;
    CHECK(!h.ctl.onMedia("B", h.now, as, vs), "Idle, never permitted — dropped");
    CHECK(h.count("B", MCV_APP_1, MCV1_TRANSMISSION_REVOKED) == 0, "no revoke in Idle without prior permission");
    h.request("A");
    CHECK(!h.ctl.onMedia("B", h.now, as, vs), "Taken — B media dropped");
    const ParsedTransmission* rv = h.last("B", MCV_APP_1, MCV1_TRANSMISSION_REVOKED);
    CHECK(rv && rv->cause() == TC_REVOKE_NO_PERMISSION, "Revoked #3 (§6.3.5.4.6)");
    CHECK(h.ctl.txState("B") == MCV_U_SENDS_MEDIA, "'not permitted but sends media'");
    h.clear();
    h.ctl.onMedia("B", h.now, as, vs);
    CHECK(h.count("B", MCV_APP_1, MCV1_TRANSMISSION_REVOKED) == 0, "no second revoke while T3 runs");
    h.advance(1000);
    CHECK(h.count("B", MCV_APP_1, MCV1_TRANSMISSION_REVOKED) == 1, "T3 retransmits Revoked #3");
    h.clear();
    h.endRequest("B");
    CHECK(h.count("B", MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE) == 1 &&
              h.count("B", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1 && h.ctl.txState("B") == MCV_U_TAKEN,
          "End Request → End Response + Notification, back to taken (§6.3.5.7.4)");
    CHECK(h.buildFail == 0, "all messages encode");
}

static void testImplicitAndRecvOnly() {
    printf("[implicit request · receive only]\n");
    {
        Harness h;
        PMcvControl::ImplicitResult res;
        McvParticipantDecl a = h.decl("sip:A@mcv");
        a.preferredAudioSsrc = 0x33333333u;
        h.ctl.addParticipant("A", a, h.now, true, &res);
        CHECK(!res.granted && res.audioSsrc == 0x33333333u && res.videoSsrc != 0,
              "alone — not granted yet, SSRC pair reserved (§14.3.7 irrespective of mc_granted)");
        CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 0, "initiator gets no Idle while pending");
        h.request("A");   // 단말 T100 재요청 — 같은 요청
        CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_REJECTED) == 0, "no #3 while implicit pending");
        h.join("B");
        const ParsedTransmission* g = h.last("A", MCV_APP_1, MCV1_TRANSMISSION_GRANTED);
        CHECK(g && g->ssrcOf(TF_AUDIO_SSRC) == 0x33333333u && g->ssrcOf(TF_VIDEO_SSRC) == res.videoSsrc,
              "first invited participant accepted → Granted with the reserved pair (§6.3.2.2)");
        CHECK(h.count("B", MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION) == 1 &&
                  h.count("B", MCV_APP_1, MCV1_TRANSMISSION_IDLE) == 0,
              "B gets Notification, not Idle");
        PMcvControl::ImplicitResult again;
        h.ctl.addParticipant("A", a, h.now, true, &again);
        CHECK(again.granted && again.audioSsrc == 0x33333333u, "JOIN ② resend returns the same result");
        h.clear();
        h.advance(1500);
        CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 1, "deferred implicit grant — T4 retransmits Granted");
        unsigned int as = 0, vs = 0;
        CHECK(h.ctl.onMedia("A", h.now, as, vs) && as == 0x33333333u, "initiator media forwarded with the reserved pair");
        h.clear();
        h.advance(3000);
        CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 0, "first media stops T4");
    }
    {
        Harness h;
        h.join("B");
        PMcvControl::ImplicitResult res;
        h.ctl.addParticipant("A", h.decl("sip:A@mcv"), h.now, true, &res);
        CHECK(res.granted && res.audioSsrc && res.videoSsrc, "another participant present → granted at JOIN");
        CHECK(h.count("A", MCV_APP_1, MCV1_TRANSMISSION_GRANTED) == 1, "Granted sent (§6.3.4.2.2 3b → §6.3.4.4.2)");
    }
    {
        Harness h;
        h.join("A");
        McvParticipantDecl ro = h.decl("sip:RO@mcv");
        ro.recvOnly = true;
        h.join("RO", ro);
        h.request("RO");
        const ParsedTransmission* rej = h.last("RO", MCV_APP_1, MCV1_TRANSMISSION_REJECTED);
        CHECK(rej && rej->cause() == TC_REJECT_RECEIVE_ONLY, "<on-network-recvonly> → Rejected #5");
        CHECK(h.buildFail == 0, "all messages encode");
    }
}

int main() {
    testJoinIdleAndOnlyOne();
    testGrantNotifyReceive();
    testEndAndIdle();
    testLimitQueuePreempt();
    testMultiTransmitAndC9();
    testAutomaticEmergency();
    testTimers();
    testUnauthorizedMedia();
    testImplicitAndRecvOnly();
    printf("cmp_mcvideo_control_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
