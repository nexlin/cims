// PMcvControl.cpp — MCVideo 전송 제어 서버 상태 머신 (TS 24.581 §6.3.4~§6.3.7). 규약·상태는 PMcvControl.h, 설계 정본 mcvideo.md §5.3.1.
//
// 규격을 읽은 방식(mcvideo.md §9 에 같이 적는다):
//  - T1·T5 는 호 시작(첫 참가자)부터 돈다 — §6.3.4.3.2·§6.3.6.3.2 는 'Start-stop' 밖에서 Idle 로 들어갈 때만 적지만 hang timer 뜻대로라면
//    무송출 호도 풀려야 한다.
//  - T6 은 수신 허가마다 시작한다(§6.3.6.4.3 f 에만 있고 §6.3.6.3.6 에는 없다). 허가 Response 에 ack 비트를 세우고 Ack·수신 종료에 멈춘다.
//  - not permitted 상태의 Transmission End Request(요청·대기 취소 — §6.3.5.3.7·§6.3.5.4.5·§6.3.5.7.4)에도 Transmission End Response 를
//    함께 보낸다 — 참여자는 'U: pending end of transmission' 에서 End Response 만 기다린다(§6.2.4.4.7·§6.2.4.9.4·§6.2.4.6.4).
//  - Transmission control Ack 의 Message Type 은 받은 메시지의 5비트 subtype(첫 비트 0)이다(§9.2.3.10) — §6.3.5.3.7 등의 «'4'
//    (Transmission End Request)» 는 부호화 절을 따른다. Message Name 을 늘 싣는다(subtype 값이 name 사이에서 겹친다).
//  - Media Transmission Notification 에 Message Sequence Number 를 싣지 않는다 — 절차(§6.3.4.4.2 3c)는 싣게 하지만 메시지 표
//    (Table 9.2.13-1)에 그 필드가 없어 코덱이 거절한다. Idle 에만 싣는다.
//  - 없는 송출을 가리킨 Receive Media Request 는 Rejected #255(Other reason) — §6.3.7 본문의 #0·#1 은 원인 표(§9.2.15.2)에 없다.
//  - 무허가 미디어: 'Transmit Taken' 에서는 늘 Revoked #3(§6.3.5.4.6), 'Transmit Idle' 에서는 직전에 허가 송출을 끝낸 참가자만
//    (§6.3.5.3.8 «if Transmission End Request message was received in the previous 'U: permitted' state»).
#include "PMcvControl.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <random>
#include <set>

// ── 전역 SSRC 할당 ─────────────────────────────────────────────────────
//   송출 SSRC(Audio·Video 쌍)는 송출마다 전역 유일해야 하고(§6.3.4.3.3 d) 전송 제어 채널 tc_ssrc 도 한 IP·포트에 여러 세션을
//   다중화할 때의 열쇠라(§4.3.3.1) 같은 공간에서 겹치지 않게 뽑는다. 0 은 «미상» 표식이라 쓰지 않는다.
static std::mutex gSsrcMtx;
static std::set<unsigned int> gSsrcInUse;

unsigned int PMcvControl::AllocSsrc(unsigned int preferred) {
    static std::mt19937 rng(std::random_device{}());
    std::lock_guard<std::mutex> lock(gSsrcMtx);
    // 단말이 offer 의 a=ssrc 로 준 값 — 전역에서 쓰이지 않으면 그대로(§14.3.7·§14.3.8 «value included in the SDP offer or new ssrc
    //   value if collision is detected»). 충돌 판정이 전역인 것은 §6.3.4.3.3 d «globally unique».
    if (preferred != 0 && !gSsrcInUse.count(preferred)) {
        gSsrcInUse.insert(preferred);
        return preferred;
    }
    for (;;) {
        unsigned int v = (unsigned int)rng();
        if (v == 0 || gSsrcInUse.count(v)) continue;
        gSsrcInUse.insert(v);
        return v;
    }
}

void PMcvControl::FreeSsrc(unsigned int ssrc) {
    if (ssrc == 0) return;
    std::lock_guard<std::mutex> lock(gSsrcMtx);
    gSsrcInUse.erase(ssrc);
}

PMcvControl::PMcvControl() {}

PMcvControl::~PMcvControl() { close(); }

void PMcvControl::configure(int maxTransmitters, bool receptionAutomatic, McvCallType callType, const McvTimers& timers) {
    _maxTransmitters = maxTransmitters > 0 ? maxTransmitters : 1;
    _receptionAutomatic = receptionAutomatic;
    _callType = callType;
    _timers = timers;
}

// ── 판정 ──────────────────────────────────────────────────────────────

// Transmission Indicator(§9.2.3.11) — 긴급·임박 호면 싣는다(«if a group call is … an emergency call, an imminent peril call …
//   shall include»). 일반 호는 싣지 않는다(0).
int PMcvControl::_indicator() const {
    switch (_callType) {
    case MCV_CALL_EMERGENCY: return TI_EMERGENCY;
    case MCV_CALL_IMMINENT: return TI_IMMINENT_PERIL;
    default: return 0;
    }
}

// 수신 모드 — 긴급·임박 호는 automatic(Reception Mode '0', §6.3.6.3.3 1a), 그 밖은 그룹 설정(기본 manual).
bool PMcvControl::_automaticReception() const { return _receptionAutomatic || _callType != MCV_CALL_NORMAL; }

// 유효 우선순위(§6.3.5.4.4 1a · §4.1.1.4). 수치의 기본값은 로스터 우선순위(default priority)다. 요청의 Transmission Priority 로
//   낮추는 것은 mc_priority 를 협상한 참가자만 — 미협상 단말이 관례로 싣는 0 을 우선순위로 읽지 않는다(MCPTT floor 와 같다).
//   tier = CSP 가 정한 호 종류(요청의 Transmission Indicator 는 호 단위 표식이라 쓰지 않는다).
PMcvControl::Prio PMcvControl::_effectivePrio(const Part& p, const ParsedTransmission* req) const {
    Prio pr;
    pr.tier = (int)_callType;
    pr.chair = p.decl.chair;
    pr.value = p.decl.rosterPriority;
    if (p.decl.maxPriority >= 0 && req && req->has(TF_TRANSMISSION_PRIORITY)) {
        int rp = req->u8(TF_TRANSMISSION_PRIORITY, 0);
        pr.value = std::min(rp, p.decl.maxPriority);
    }
    return pr;
}

// a 가 b 를 선점하는가 — tier → chair → 수치 (MCPTT PMcpttGroup::_preempts 와 같은 서열).
bool PMcvControl::_outranks(const Prio& a, const Prio& b) {
    if (a.tier != b.tier) return a.tier > b.tier;
    if (a.chair != b.chair) return a.chair;
    return a.value > b.value;
}

// 선점 대상 — 회수 중이 아닌 송출 가운데 서열이 가장 낮은 것. 서열이 같으면 나중에 허가된 송출(먼저 보내던 송출을 끊지 않는다).
PMcvControl::Tx* PMcvControl::_weakestTx() {
    Tx* w = nullptr;
    for (auto& kv : _tx) {
        Tx& t = kv.second;
        if (t.revoking) continue;
        if (!w || _outranks(w->prio, t.prio) || (!_outranks(t.prio, w->prio) && t.grantMs > w->grantMs)) w = &t;
    }
    return w;
}

PMcvControl::Tx* PMcvControl::_txOf(const std::string& member) {
    auto it = _tx.find(member);
    return it != _tx.end() ? &it->second : nullptr;
}

// 메시지가 가리키는 송출 — User Id of the Transmitting User, 없으면 Video·Audio SSRC (§6.3.7.3.4 · §9.2.14).
PMcvControl::Tx* PMcvControl::_txByIdentity(const ParsedTransmission& m) {
    std::string uid = m.str(TF_TRANSMITTING_USER_ID);
    unsigned int v = m.ssrcOf(TF_VIDEO_SSRC), a = m.ssrcOf(TF_AUDIO_SSRC);
    for (auto& kv : _tx) {
        Tx& t = kv.second;
        if (!uid.empty() && t.userId == uid) return &t;
        if ((v && t.videoSsrc == v) || (a && t.audioSsrc == a)) return &t;
    }
    return nullptr;
}

PMcvControl::Part* PMcvControl::_part(const std::string& id) {
    auto it = _parts.find(id);
    return it != _parts.end() ? &it->second : nullptr;
}

int PMcvControl::_receiversOf(const std::string& sender) const {
    int n = 0;
    for (const auto& kv : _parts)
        if (kv.second.active.count(sender)) ++n;
    return n;
}

// ── 공통 송신 ──────────────────────────────────────────────────────────

void PMcvControl::_send(const std::string& member, int app, int subtype, const std::vector<McvTlv>& fields) {
    if (_hooks.send) _hooks.send(member, app, subtype, fields);
}

// Transmission control Ack(§9.2.31) — Message Type = 받은 subtype(ack 비트 뺀 값), Source = 2(controlling), Message Name 늘.
void PMcvControl::_ack(const std::string& member, const ParsedTransmission& m) {
    static const char* kName[] = { MCV_NAME_0, MCV_NAME_1, MCV_NAME_2 };
    if (m.app < MCV_APP_0 || m.app > MCV_APP_2) return;
    std::vector<McvTlv> f{ McvTlv(TF_MSG_TYPE, McvU8(m.op())), McvTlv(TF_SOURCE, McvU16(TC_SRC_CONTROLLING)),
                           McvTlv(TF_MESSAGE_NAME, McvName(kName[m.app])) };
    _send(member, MCV_APP_2, MCV2_TRANSMISSION_CONTROL_ACK, f);
}

void PMcvControl::_txFields(std::vector<McvTlv>& f, const Tx& t) const {
    f.emplace_back(TF_TRANSMITTING_USER_ID, t.userId);
    f.emplace_back(TF_AUDIO_SSRC, McvSsrc(t.audioSsrc));
    f.emplace_back(TF_VIDEO_SSRC, McvSsrc(t.videoSsrc));
}

void PMcvControl::_indicatorField(std::vector<McvTlv>& f) const {
    int bits = _indicator();
    if (bits) f.emplace_back(TF_TRANSMISSION_INDICATOR, McvU16(bits));
}

void PMcvControl::_log(const std::string& line) {
    if (_hooks.log) _hooks.log(line);
}

std::vector<McvTransmitter> PMcvControl::transmitters() const {
    std::vector<std::pair<int64_t, McvTransmitter>> v;
    for (const auto& kv : _tx) {
        McvTransmitter t;
        t.memberId = kv.second.member;
        t.userId = kv.second.userId;
        t.audioSsrc = kv.second.audioSsrc;
        t.videoSsrc = kv.second.videoSsrc;
        t.revoking = kv.second.revoking;
        v.emplace_back(kv.second.grantMs, t);
    }
    std::stable_sort(v.begin(), v.end(), [](const std::pair<int64_t, McvTransmitter>& a,
                                            const std::pair<int64_t, McvTransmitter>& b) { return a.first < b.first; });
    std::vector<McvTransmitter> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}

void PMcvControl::_transmittersChanged() {
    if (_hooks.transmittersChanged) _hooks.transmittersChanged(transmitters());
}

int PMcvControl::receptionCount() const {
    int n = 0;
    for (const auto& kv : _parts) n += (int)kv.second.active.size();
    return n;
}

McvTxState PMcvControl::txState(const std::string& id) const {
    auto it = _parts.find(id);
    return it != _parts.end() ? it->second.state : MCV_U_IDLE;
}

int PMcvControl::queuePosition(const std::string& id) const {
    for (size_t i = 0; i < _queue.size(); ++i)
        if (_queue[i].member == id) return (int)i + 1;
    return 0;
}

bool PMcvControl::receives(const std::string& receiverId, const std::string& senderId) const {
    auto it = _parts.find(receiverId);
    return it != _parts.end() && it->second.active.count(senderId) != 0;
}

// ── 참가자 ────────────────────────────────────────────────────────────

void PMcvControl::addParticipant(const std::string& id, const McvParticipantDecl& decl, int64_t nowMs,
                                 bool implicitRequest, ImplicitResult* res) {
    auto it = _parts.find(id);
    if (it != _parts.end()) {
        // JOIN ② 재전송·refresh — 협상 값만 갱신하고 상태는 둔다. 암묵적 요청의 결과는 처음 것 그대로 돌려준다.
        Part& p = it->second;
        std::string uid = p.decl.userId;
        p.decl = decl;
        if (p.decl.userId.empty()) p.decl.userId = uid;
        if (res) {
            if (const Tx* t = _txOf(id)) {
                res->granted = true;
                res->audioSsrc = t->audioSsrc;
                res->videoSsrc = t->videoSsrc;
            } else {
                res->granted = false;
                res->audioSsrc = p.implicitAudio;
                res->videoSsrc = p.implicitVideo;
            }
        }
        return;
    }

    bool first = !_started;
    Part& p = _parts[id];
    p.id = id;
    p.decl = decl;
    if (p.decl.userId.empty()) p.decl.userId = id;
    if (first) {
        _started = true;   // 호 시작 — T1·T5 무장(§5.3.1 규격 읽기)
        if (_timers.t1Ms > 0) _t1At = nowMs + _timers.t1Ms;
        _rearmT5(nowMs);
    }

    // 암묵적 송출 요청 — SSRC 쌍은 허가 여부와 무관하게 지금 예약한다(§14.3.7·§14.3.8 «irrespective of mc_granted»).
    //   다른 참가자가 있으면 곧바로 허가, 없으면 첫 초대 참가자가 수락할 때 허가한다(§6.3.2.2 — 미디어 버퍼링 없음).
    if (implicitRequest) {
        unsigned int a = AllocSsrc(decl.preferredAudioSsrc), v = AllocSsrc(decl.preferredVideoSsrc);
        bool others = _parts.size() > 1;
        bool room = (int)_tx.size() < _maxTransmitters;
        if (res) {
            res->audioSsrc = a;
            res->videoSsrc = v;
            res->granted = false;
        }
        if (!others) {
            p.implicitPending = true;
            p.implicitUntil = nowMs + kImplicitWaitMs;
            p.implicitAudio = a;
            p.implicitVideo = v;
            _log("implicit request pending (first invited participant) member=" + id);
            return;
        }
        if (room) {
            if (res) res->granted = true;
            _grant(p, _effectivePrio(p, nullptr), false, nowMs, a, v);
            return;
        }
        // 새 세션의 요청이라 자리가 차 있을 수 없지만(§14.3.5) — 차 있으면 예약을 풀고 보통 합류로 둔다.
        FreeSsrc(a);
        FreeSsrc(v);
        if (res) res->audioSsrc = res->videoSsrc = 0;
    }

    // 첫 초대 참가자가 수락했다 — 기다리던 개시자의 암묵적 요청을 허가한다(§6.3.2.2). 허가가 이 참가자에게 Notification 을 보낸다.
    //   기다린 요청이라 큐에서 내린 허가처럼 T4/C4 로 Granted 를 첫 미디어까지 다시 보낸다 — 개시자는 answer 에 mc_granted 가 없어
    //   Granted 만 기다리고, NAT 뒤 단말은 제어 채널 latch 전이면 첫 Granted 를 잃는다.
    for (auto& kv : _parts) {
        Part& q = kv.second;
        if (kv.first == id || !q.implicitPending) continue;
        q.implicitPending = false;
        unsigned int a = q.implicitAudio, v = q.implicitVideo;
        q.implicitAudio = q.implicitVideo = 0;
        _grant(q, _effectivePrio(q, nullptr), true, nowMs, a, v);
        return;
    }

    if (_tx.empty()) {
        // 진행 중 송출이 없다 — Transmission Idle 1회(§6.3.5.2.2 2a·4b), 'U: not permitted and Transmit Idle'
        p.state = MCV_U_IDLE;
        ++_msgSeq;
        _sendIdle(id);
    } else {
        // 'U: not permitted and Transmit Taken' + 송출마다 Media Transmission Notification(§6.3.5.2.2 4a · §6.3.7.2.2 2b)
        p.state = MCV_U_TAKEN;
        _notifyCurrent(p, nowMs);
    }
}

void PMcvControl::removeParticipant(const std::string& id, int64_t nowMs) {
    Part* pp = _part(id);
    if (!pp) return;
    // 수신 몫 정리(C9·C11·C7) — 그 송출이 받는 이를 잃으면 T11
    std::vector<std::string> senders;
    for (const auto& kv : pp->active) senders.push_back(kv.first);
    for (const auto& s : senders) _dropReception(*pp, s, nowMs);
    _dequeue(id);
    FreeSsrc(pp->implicitAudio);
    FreeSsrc(pp->implicitVideo);
    // 이 참가자에게는 더 보내지 않는다(§6.3.5.8.2 1) — 먼저 지우고 송출을 끝낸다(End Notify·Idle 은 남은 참가자에게만).
    _parts.erase(id);
    if (_tx.count(id)) _endTransmission(id, nowMs, "participant left");
    _rearmT5(nowMs);
}

void PMcvControl::close() {
    for (auto& kv : _tx) {
        FreeSsrc(kv.second.audioSsrc);
        FreeSsrc(kv.second.videoSsrc);
    }
    for (auto& kv : _parts) {
        FreeSsrc(kv.second.implicitAudio);
        FreeSsrc(kv.second.implicitVideo);
    }
    _tx.clear();
    _parts.clear();
    _queue.clear();
    _t1At = _t2At = _t5At = 0;
    _c2 = 0;
    _started = false;
}

// ── 송출 제어 ──────────────────────────────────────────────────────────

void PMcvControl::onMessage(const std::string& id, const ParsedTransmission& m, int64_t nowMs) {
    Part* p = _part(id);
    if (!p) return;
    bool isAck = m.app == MCV_APP_2 && m.op() == MCV2_TRANSMISSION_CONTROL_ACK;
    if (m.ackRequired() && !isAck) _ack(id, m);
    switch (m.app) {
    case MCV_APP_0:
        switch (m.op()) {
        case MCV0_TRANSMISSION_REQUEST: _onTxRequest(*p, m, nowMs); return;
        case MCV0_QUEUE_POSITION_REQUEST: _sendQueueInfo(*p); return;
        case MCV0_RECEIVE_MEDIA_REQUEST: _onRxRequest(*p, m, nowMs); return;
        default: break;
        }
        break;
    case MCV_APP_2:
        switch (m.op()) {
        case MCV2_TRANSMISSION_END_REQUEST: _onEndRequest(*p, m, nowMs); return;
        case MCV2_TRANSMISSION_END_RESPONSE: _onEndResponse(*p, nowMs); return;
        case MCV2_MEDIA_RECEPTION_END_REQUEST: _onRxEnd(*p, m, true, nowMs); return;
        case MCV2_MEDIA_RECEPTION_END_RESPONSE: _onRxEnd(*p, m, false, nowMs); return;
        case MCV2_TRANSMISSION_CONTROL_ACK: _onAck(*p, m); return;
        default: break;
        }
        break;
    default:
        break;
    }
    // 이 판본의 on-network 서버 절차가 없는 메시지(Transmission Release · Remote Transmission · MCV1) — 버린다(§6.3.4.1)
    _log(std::string("discard ") + McvMessageName(m.app, m.subtype) + " member=" + id);
}

// Transmission Request (MCV0 0) — §6.3.4.3.3 · §6.3.4.4.7A · §6.3.4.4.7 · §6.3.4.4.8 · §6.3.5.3.4 · §6.3.5.4.4.
void PMcvControl::_onTxRequest(Part& p, const ParsedTransmission& m, int64_t now) {
    if (p.state == MCV_U_PERMITTED) {
        // 이미 허가된 참가자의 재요청 — Granted 재송신(§6.3.4.4.8). Granted 가 유실된 단말이 재요청으로 복구한다.
        if (const Tx* t = _txOf(p.id)) _sendGranted(*t);
        return;
    }
    if (p.state == MCV_U_PENDING_REVOKE || p.state == MCV_U_SENDS_MEDIA) {
        _log("discard Transmission Request in " + std::string(p.state == MCV_U_PENDING_REVOKE ? "pending revoke" : "sends media") +
             " member=" + p.id);
        return;
    }
    if (p.implicitPending) {
        // 암묵적 요청이 첫 초대 참가자를 기다리는 중 — 단말의 T100 재요청은 같은 요청이다(허가 때 Granted 가 간다).
        _log("Transmission Request while implicit request pending member=" + p.id);
        return;
    }
    if (p.decl.recvOnly) {
        _sendReject(p, TC_REJECT_RECEIVE_ONLY);   // <on-network-recvonly> — #5 (§6.3.4.3.3 1b · §6.3.4.4.7A 1b)
        return;
    }
    Prio pr = _effectivePrio(p, &m);
    if (_tx.empty()) {
        // G: Transmit Idle
        if (_parts.size() <= 1) {
            _sendReject(p, TC_REJECT_ONLY_ONE_PARTICIPANT);   // #3 (§6.3.4.3.3 1a)
            return;
        }
        _grant(p, pr, false, now);
        return;
    }
    // G: Transmit Taken
    for (const auto& q : _queue) {
        if (q.member != p.id) continue;
        if (q.prio.tier == pr.tier && q.prio.chair == pr.chair && q.prio.value == pr.value) {
            if (p.decl.queueing) _sendQueueInfo(p);   // 같은 유효 우선순위로 이미 대기 — 위치만(§6.3.5.4.4 4)
            return;
        }
        break;   // 우선순위가 바뀌었다 — 아래에서 위치를 갱신한다
    }
    if ((int)_tx.size() < _maxTransmitters) {
        _grant(p, pr, false, now);   // 동시 송출 자리가 남았다 (§6.3.4.4.7A 3)
        return;
    }
    // §6.3.5.4.4 첫 단락 — 대기열도 mc_priority 도 협상하지 않은 참여자의 일반 호 요청은 우선순위를 따지지 않는다: 상한이면
    //   #1. (셋째 단락은 «queueing 또는 mc_priority 또는 둘 다» 협상한 참여자 — 첫 단락의 «or» 는 그 여집합 = 둘 다 미협상으로
    //   읽는다. mcvideo.md §9.)
    if (!p.decl.queueing && p.decl.maxPriority < 0 && _callType == MCV_CALL_NORMAL) {
        _sendReject(p, TC_REJECT_TRANSMISSION_LIMIT);
        return;
    }
    Tx* w = _weakestTx();
    if (w && _outranks(pr, w->prio)) {
        // 선점 — 가장 약한 송출에 Revoked #4, 요청은 큐 맨 앞(§6.3.4.4.7 2), 큐 협상이면 Queue Position Info
        _log("pre-empt member=" + w->member + " by " + p.id);
        _revoke(*w, TC_REVOKE_PREEMPTED, false, now);
        _enqueue(p, pr, true);
        if (p.decl.queueing) _sendQueueInfo(p);
        return;
    }
    if (p.decl.queueing) {
        _enqueue(p, pr, false);   // 같은 유효 우선순위의 대기 바로 뒤(§6.3.5.4.4 — queueing, 비선점)
        _sendQueueInfo(p);
        return;
    }
    _sendReject(p, TC_REJECT_TRANSMISSION_LIMIT);   // #1 (§6.3.5.4.4 1a)
}

// 허가 — SSRC 쌍 할당·보관 → 요청자 Granted → 다른 참가자 Media Transmission Notification(§6.3.4.4.2). audio·video 가 주어지면
//   (암묵적 요청이 예약한 쌍) 그 값을 쓴다. retransmit = 기다린 요청(큐 · 늦게 내린 암묵적 요청)의 허가 — T4/C4 재송신.
void PMcvControl::_grant(Part& p, const Prio& prio, bool retransmit, int64_t now, unsigned int audio, unsigned int video) {
    Tx t;
    t.member = p.id;
    t.userId = p.decl.userId;
    t.audioSsrc = audio ? audio : AllocSsrc(p.decl.preferredAudioSsrc);
    t.videoSsrc = video ? video : AllocSsrc(p.decl.preferredVideoSsrc);
    t.prio = prio;
    t.grantMs = now;
    if (retransmit) {
        t.t4At = now + _timers.t4Ms;   // 기다린 요청의 Granted 는 첫 미디어까지 재송신(T4/C4, §6.3.4.4.2 2)
        t.c4 = 1;
    }
    _dequeue(p.id);
    _tx[p.id] = t;
    _t1At = 0;   // G: Transmit Taken — T1·T2 정지(§6.3.4.3.3 3a·3b)
    _t2At = 0;
    p.state = MCV_U_PERMITTED;
    p.endedPermitted = false;
    p.t3At = 0;
    p.implicitPending = false;
    Tx& tr = _tx[p.id];
    _sendGranted(tr);

    bool automatic = _automaticReception();
    for (auto& kv : _parts) {
        Part& r = kv.second;
        if (r.id == p.id || r.implicitPending) continue;
        if (r.state == MCV_U_IDLE) r.state = MCV_U_TAKEN;
        _sendNotification(r.id, tr);
        if (automatic) _addReception(r, tr, false, now);   // automatic — 알림과 함께 Active SSRC List(§6.3.7.3.3 5)
    }
    if (!automatic && _timers.t11Ms > 0 && _receiversOf(p.id) == 0) tr.t11At = now + _timers.t11Ms;   // §6.3.6.3.3 1b
    char buf[160];
    snprintf(buf, sizeof(buf), "grant member=%s audio=%08x video=%08x prio=%d%s (Cx=%d/%d)", p.id.c_str(), tr.audioSsrc,
             tr.videoSsrc, prio.value, retransmit ? " (T4)" : "", (int)_tx.size(), _maxTransmitters);
    _log(buf);
    _transmittersChanged();
}

// Transmission Granted (§6.3.4.4.2 1 · §6.3.4.4.8 · §6.3.4.4.9) — 우선순위 · (호 종류) Indicator · 송출 SSRC 쌍.
void PMcvControl::_sendGranted(const Tx& t) {
    std::vector<McvTlv> f{ McvTlv(TF_TRANSMISSION_PRIORITY, McvU8(t.prio.value)) };
    _indicatorField(f);
    f.emplace_back(TF_AUDIO_SSRC, McvSsrc(t.audioSsrc));
    f.emplace_back(TF_VIDEO_SSRC, McvSsrc(t.videoSsrc));
    _send(t.member, MCV_APP_1, MCV1_TRANSMISSION_GRANTED, f);
}

void PMcvControl::_sendReject(Part& p, int cause) {
    std::vector<McvTlv> f{ McvTlv(TF_REJECT_CAUSE, McvCause(cause)) };
    _indicatorField(f);
    _send(p.id, MCV_APP_1, MCV1_TRANSMISSION_REJECTED, f);
    _log("reject member=" + p.id + " cause=" + std::to_string(cause));
}

// Transmission Idle (§9.2.30) — Message Sequence Number 는 부르는 쪽이 올린다(전원에게 같은 값).
void PMcvControl::_sendIdle(const std::string& member) {
    std::vector<McvTlv> f{ McvTlv(TF_MSG_SEQ, McvU16((int)(_msgSeq & 0xFFFF))) };
    _indicatorField(f);
    _send(member, MCV_APP_1, MCV1_TRANSMISSION_IDLE, f);
}

// Media Transmission Notification (§6.3.4.4.2 3 · §6.3.7.3.3) — 송출자 · SSRC 쌍 · Permission 1 · Indicator · Reception Mode.
void PMcvControl::_sendNotification(const std::string& to, const Tx& t) {
    std::vector<McvTlv> f;
    _txFields(f, t);
    f.emplace_back(TF_PERMISSION, McvU16(TC_PERM_ALLOWED));
    _indicatorField(f);
    f.emplace_back(TF_RECEPTION_MODE,
                   McvU16(_automaticReception() ? TC_RECEPTION_AUTOMATIC : TC_RECEPTION_MANUAL));
    _send(to, MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION, f);
}

void PMcvControl::_notifyCurrent(Part& p, int64_t now) {
    bool automatic = _automaticReception();
    for (auto& kv : _tx) {
        Tx& t = kv.second;
        if (t.member == p.id) continue;
        _sendNotification(p.id, t);
        if (automatic && !p.active.count(t.member)) _addReception(p, t, false, now);
    }
}

// Transmission End Request (MCV2 0) — §6.3.5.5.3 · §6.3.5.6.5 · §6.3.4.4.6 · §6.3.4.5.4 (허가 송출의 끝) / §6.3.5.3.7 · §6.3.5.4.5 ·
//   §6.3.5.7.4 (요청·대기 취소 — End Response 를 함께 보낸다, 파일 머리말).
void PMcvControl::_onEndRequest(Part& p, const ParsedTransmission& m, int64_t now) {
    (void)m;
    if (p.state == MCV_U_PERMITTED || p.state == MCV_U_PENDING_REVOKE) {
        if (const Tx* t = _txOf(p.id)) {
            std::vector<McvTlv> f;
            _txFields(f, *t);
            _indicatorField(f);
            _send(p.id, MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE, f);
            _endTransmission(p.id, now, "end request");
            return;
        }
    }
    // 허가가 없는 상태 — 대기·암묵 요청을 거두고 End Response, 그리고 지금 상태(Idle 또는 진행 중 송출 알림)를 알린다.
    std::vector<McvTlv> f;
    _indicatorField(f);
    _send(p.id, MCV_APP_2, MCV2_TRANSMISSION_END_RESPONSE, f);
    _dequeue(p.id);
    if (p.implicitPending) {
        p.implicitPending = false;
        FreeSsrc(p.implicitAudio);
        FreeSsrc(p.implicitVideo);
        p.implicitAudio = p.implicitVideo = 0;
    }
    p.t3At = 0;
    if (_tx.empty()) {
        p.state = MCV_U_IDLE;
        ++_msgSeq;
        _sendIdle(p.id);
    } else {
        p.state = MCV_U_TAKEN;
        _notifyCurrent(p, now);
    }
}

// Transmission End Response (MCV2 1) — 서버가 보낸 End Request(#8)·Revoked 뒤의 응답: 송출 끝(§6.3.5.6.8 · §6.3.4.5.7).
void PMcvControl::_onEndResponse(Part& p, int64_t now) {
    Tx* t = _txOf(p.id);
    if (!t || !t->revoking) return;
    _endTransmission(p.id, now, "end response");
}

// 송출 하나를 끝낸다 — 분배 중지 · SSRC 반환 · 다른 참가자 Transmission End Notify · Cx−1 → 큐 맨 앞 허가 또는 G: Idle.
void PMcvControl::_endTransmission(const std::string& member, int64_t now, const char* why) {
    auto it = _tx.find(member);
    if (it == _tx.end()) return;
    Tx t = it->second;
    _tx.erase(it);
    for (auto& kv : _parts) {
        Part& r = kv.second;
        r.active.erase(member);   // Active SSRC List · C9 (§6.3.7.4.7 3·4)
        if (r.id == member || r.implicitPending) continue;
        std::vector<McvTlv> f;
        _txFields(f, t);
        _send(r.id, MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY, f);   // §6.3.6.3.8 · §6.3.6.4.11 · §6.3.7.3.7
    }
    FreeSsrc(t.audioSsrc);
    FreeSsrc(t.videoSsrc);
    if (Part* p = _part(member)) {
        p->state = _tx.empty() ? MCV_U_IDLE : MCV_U_TAKEN;
        p->endedPermitted = true;
        p->endMs = now;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "end member=%s (%s) Cx=%d/%d queue=%d", member.c_str(), why, (int)_tx.size(),
             _maxTransmitters, (int)_queue.size());
    _log(buf);
    _rearmT5(now);
    if (!_afterSlotFreed(now)) _transmittersChanged();   // 큐에서 허가했으면 그 허가가 이미 새 집합을 알렸다
}

// 자리가 났다 — 큐 맨 앞을 허가(§6.3.4.3.2 2 · §6.3.4.5.4 5), 송출이 하나도 없으면 G: Transmit Idle(§6.3.4.3.2 1). 허가했으면 true.
bool PMcvControl::_afterSlotFreed(int64_t now) {
    bool granted = false;
    while (!_queue.empty() && (int)_tx.size() < _maxTransmitters) {
        Queued q = _queue.front();
        _queue.pop_front();
        Part* p = _part(q.member);
        if (!p || _tx.count(q.member)) continue;
        _grant(*p, q.prio, true, now);
        granted = true;
    }
    if (_tx.empty()) _enterIdle(now);
    return granted;
}

// G: Transmit Idle 진입(§6.3.4.3.2 1) — Transmission Idle 전원(Message Sequence Number +1) · T2/C2 · T1.
void PMcvControl::_enterIdle(int64_t now) {
    ++_msgSeq;
    for (auto& kv : _parts) {
        Part& p = kv.second;
        if (p.implicitPending || p.state == MCV_U_SENDS_MEDIA) continue;   // 무허가 미디어 참가자는 T3 가 끝낼 때까지 그대로
        p.state = MCV_U_IDLE;
        _sendIdle(p.id);
    }
    if (_timers.t2Ms > 0) {
        _t2At = now + _timers.t2Ms;
        _c2 = 1;
    }
    if (_timers.t1Ms > 0) _t1At = now + _timers.t1Ms;
}

// 회수 — Revoked(#4 선점 · #3 는 참가자 쪽 T3) 또는 서버 End Request(#8 받는 이 없음) → 'U: pending Transmit Revoke' · T3(§6.3.4.5.2 ·
//   §6.3.5.6.2). T4 정지(§6.3.4.4.7 2b · §6.3.4.4.13 1).
void PMcvControl::_revoke(Tx& t, int cause, bool endRequest, int64_t now) {
    t.revoking = true;
    t.endRequest = endRequest;
    t.revokeCause = cause;
    t.t4At = 0;
    t.t11At = 0;
    t.t3At = now + _timers.t3Ms;
    t.t3Count = 0;
    if (Part* p = _part(t.member)) p->state = MCV_U_PENDING_REVOKE;
    _sendRevoke(t);
}

void PMcvControl::_sendRevoke(const Tx& t) {
    std::vector<McvTlv> f{ McvTlv(TF_REJECT_CAUSE, McvCause(t.revokeCause)) };
    if (t.endRequest) {
        _txFields(f, t);
        _indicatorField(f);
        _send(t.member, MCV_APP_2, MCV2_TRANSMISSION_END_REQUEST, f);
    } else {
        _indicatorField(f);
        _send(t.member, MCV_APP_1, MCV1_TRANSMISSION_REVOKED, f);
    }
}

// active Transmission request queue — front(선점) 또는 같은 유효 우선순위의 대기 바로 뒤(§6.3.5.4.4 · §6.3.4.4.7 2e).
void PMcvControl::_enqueue(Part& p, const Prio& prio, bool front) {
    _dequeue(p.id);
    Queued q;
    q.member = p.id;
    q.prio = prio;
    if (front) {
        _queue.push_front(q);
        return;
    }
    auto pos = std::find_if(_queue.begin(), _queue.end(), [&](const Queued& e) { return _outranks(prio, e.prio); });
    _queue.insert(pos, q);
}

void PMcvControl::_dequeue(const std::string& member) {
    _queue.erase(std::remove_if(_queue.begin(), _queue.end(), [&](const Queued& e) { return e.member == member; }),
                 _queue.end());
}

// Queue Position Info (§6.3.5.4.7) — 위치·우선순위, 큐에 없으면 254.
void PMcvControl::_sendQueueInfo(Part& p) {
    int pos = queuePosition(p.id);
    int prio = _effectivePrio(p, nullptr).value;
    if (pos > 0) prio = _queue[pos - 1].prio.value;
    std::vector<McvTlv> f{ McvTlv(TF_QUEUE_INFO, McvQueueInfo(pos > 0 ? pos : TC_QUEUE_NOT_QUEUED, prio)) };
    _indicatorField(f);
    _send(p.id, MCV_APP_1, MCV1_QUEUE_POSITION_INFO, f);
}

// 미디어 (§6.3.5.5.6 · §6.3.5.6.4 · §6.3.5.4.6 · §6.3.5.3.8 · §6.3.5.7).
bool PMcvControl::onMedia(const std::string& id, int64_t nowMs, unsigned int& audioSsrc, unsigned int& videoSsrc) {
    Part* p = _part(id);
    if (!p) return false;
    switch (p->state) {
    case MCV_U_PERMITTED:
    case MCV_U_PENDING_REVOKE: {
        Tx* t = _txOf(id);
        if (!t) return false;
        t->t4At = 0;   // 첫 미디어 — T4 정지(§6.3.4.4.5 1)
        audioSsrc = t->audioSsrc;
        videoSsrc = t->videoSsrc;
        return true;
    }
    case MCV_U_SENDS_MEDIA:
        return false;
    case MCV_U_IDLE:
        if (!p->endedPermitted) return false;
        [[fallthrough]];   // 허가 송출을 끝낸 뒤에도 보내는 참가자(§6.3.5.3.8)
    case MCV_U_TAKEN: {
        if (p->endMs && nowMs - p->endMs < kEndGraceMs) return false;   // 끝나기 전에 떠난 RTP — 회수하지 않는다
        std::vector<McvTlv> f{ McvTlv(TF_REJECT_CAUSE, McvCause(TC_REVOKE_NO_PERMISSION)) };
        _indicatorField(f);
        _send(id, MCV_APP_1, MCV1_TRANSMISSION_REVOKED, f);
        p->state = MCV_U_SENDS_MEDIA;
        p->t3At = nowMs + _timers.t3Ms;
        p->t3Count = 0;
        _log("media without permission member=" + id + " — Revoked #3");
        return false;
    }
    }
    return false;
}

// ── 수신 제어 ──────────────────────────────────────────────────────────

// Receive Media Request (MCV0 4) — §6.3.6.3.6 · §6.3.6.4.3 · §6.3.7.3.4 · §6.3.7.4.10.
void PMcvControl::_onRxRequest(Part& r, const ParsedTransmission& m, int64_t now) {
    Tx* t = _txByIdentity(m);
    if (!t || t->member == r.id) {
        _sendRxResponse(r.id, nullptr, &m, false, TC_RECV_REJECT_OTHER, false);
        return;
    }
    auto it = r.active.find(t->member);
    if (it != r.active.end()) {
        // 이미 받는 중 — 허가를 다시 알린다(Response 유실 복구)
        it->second.t6At = now + _timers.t6Ms;
        it->second.c6 = 1;
        _sendRxResponse(r.id, t, &m, true, 0, true);
        return;
    }
    if ((int)r.active.size() >= r.decl.maxRxStreams) {
        _sendRxResponse(r.id, t, &m, false, TC_RECV_REJECT_MAX_STREAMS, false);   // C9 상한 — #7 (§6.3.7.4.10 1a)
        return;
    }
    _addReception(r, *t, true, now);
    _sendRxResponse(r.id, t, &m, true, 0, true);
    char buf[160];
    snprintf(buf, sizeof(buf), "receive member=%s <- %s (C9=%d/%d C11=%d)", r.id.c_str(), t->member.c_str(),
             (int)r.active.size(), r.decl.maxRxStreams, _receiversOf(t->member));
    _log(buf);
}

// Receive Media Response (§9.2.15) — Result · (거절) Reject Cause · 송출 식별자(User ID + SSRC 쌍, 모르면 요청의 값) · Indicator.
//   허가는 ack 비트를 세우고 T6 이 재송신한다(파일 머리말).
void PMcvControl::_sendRxResponse(const std::string& to, const Tx* t, const ParsedTransmission* req, bool granted,
                                  int cause, bool ackReq) {
    std::vector<McvTlv> f{ McvTlv(TF_RESULT, McvU16(granted ? TC_RESULT_GRANTED : TC_RESULT_REJECTED)) };
    if (!granted) f.emplace_back(TF_REJECT_CAUSE, McvCause(cause));
    if (t) {
        _txFields(f, *t);
    } else if (req) {
        if (req->has(TF_TRANSMITTING_USER_ID)) f.emplace_back(TF_TRANSMITTING_USER_ID, req->str(TF_TRANSMITTING_USER_ID));
        if (req->has(TF_AUDIO_SSRC)) f.emplace_back(TF_AUDIO_SSRC, McvSsrc(req->ssrcOf(TF_AUDIO_SSRC)));
        if (req->has(TF_VIDEO_SSRC)) f.emplace_back(TF_VIDEO_SSRC, McvSsrc(req->ssrcOf(TF_VIDEO_SSRC)));
    }
    _indicatorField(f);
    _send(to, MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE | (ackReq ? MCV_ACK_REQ_BIT : 0), f);
}

// Active SSRC List 에 넣는다 — C9+1 · C11+1(T11 정지) · C7+1(T5 정지). manual 허가면 T6/C6.
void PMcvControl::_addReception(Part& r, Tx& t, bool manual, int64_t now) {
    const bool added = r.active.count(t.member) == 0;
    RxGrant& g = r.active[t.member];
    if (manual) {
        g.t6At = now + _timers.t6Ms;
        g.c6 = 1;
    }
    t.t11At = 0;
    _t5At = 0;
    if (added && _hooks.receptionStarted) _hooks.receptionStarted(r.id, t.member);
}

// Active SSRC List 에서 뺀다 — C9−1 · C11−1(0 이면 manual 에서 T11) · C7−1(0 이면 Gr: Reception Idle — T5)(§6.3.6.4.4 · §6.3.7.4.9).
void PMcvControl::_dropReception(Part& r, const std::string& sender, int64_t now) {
    if (!r.active.erase(sender)) return;
    Tx* t = _txOf(sender);
    if (t && !t->revoking && !_automaticReception() && _timers.t11Ms > 0 && _receiversOf(sender) == 0)
        t->t11At = now + _timers.t11Ms;
    _rearmT5(now);
}

// Media Reception End Request (MCV2 2) → Response + 목록에서 뺀다(§6.3.6.4.4 · §6.3.7.4.9). End Response(MCV2 3)는 목록 정리만(§6.3.7.4.12).
void PMcvControl::_onRxEnd(Part& r, const ParsedTransmission& m, bool request, int64_t now) {
    Tx* t = _txByIdentity(m);
    if (request) {
        std::vector<McvTlv> f;
        if (t) {
            _txFields(f, *t);
        } else {
            if (m.has(TF_TRANSMITTING_USER_ID)) f.emplace_back(TF_TRANSMITTING_USER_ID, m.str(TF_TRANSMITTING_USER_ID));
            if (m.has(TF_AUDIO_SSRC)) f.emplace_back(TF_AUDIO_SSRC, McvSsrc(m.ssrcOf(TF_AUDIO_SSRC)));
            if (m.has(TF_VIDEO_SSRC)) f.emplace_back(TF_VIDEO_SSRC, McvSsrc(m.ssrcOf(TF_VIDEO_SSRC)));
        }
        _indicatorField(f);
        _send(r.id, MCV_APP_2, MCV2_MEDIA_RECEPTION_END_RESPONSE, f);
    }
    if (t) {
        _dropReception(r, t->member, now);
        _log("reception end member=" + r.id + " <- " + t->member);
    }
}

// Transmission control Ack — Receive Media Response 의 확인이면 T6 정지.
void PMcvControl::_onAck(Part& r, const ParsedTransmission& m) {
    if (m.messageName() != MCV_NAME_1 || m.u8(TF_MSG_TYPE) != MCV1_RECEIVE_MEDIA_RESPONSE) return;
    for (auto& kv : r.active) kv.second.t6At = 0;
}

// Gr: Reception Idle(C7 = 0)이면 T5 — 이미 돌면 그대로. 받는 이가 생기면 정지.
void PMcvControl::_rearmT5(int64_t now) {
    if (receptionCount() > 0) {
        _t5At = 0;
        return;
    }
    if (_started && _timers.t5Ms > 0 && _t5At == 0) _t5At = now + _timers.t5Ms;
}

// ── 타이머 ────────────────────────────────────────────────────────────

void PMcvControl::tick(int64_t nowMs) {
    // T1 Inactivity (G: Idle) — 만료 알림 후 재무장(§6.3.4.3.5 3a)
    if (_t1At && nowMs >= _t1At && _tx.empty()) {
        _t1At = nowMs + _timers.t1Ms;
        _log("T1 (Inactivity) expired");
        if (_hooks.inactivity) _hooks.inactivity("T1");
    }
    // T2 Transmission Idle 재송신 (§6.3.4.3.4)
    if (_t2At && nowMs >= _t2At) {
        if (_tx.empty() && _c2 < _timers.c2) {
            ++_c2;
            _t2At = nowMs + _timers.t2Ms;
            ++_msgSeq;
            for (auto& kv : _parts)
                if (kv.second.state == MCV_U_IDLE && !kv.second.implicitPending) _sendIdle(kv.first);
        } else {
            _t2At = 0;
        }
    }
    // T5 Reception Inactivity (Gr: Idle) — 만료 알림 후 재무장(§6.3.6.3.5 3a)
    if (_t5At && nowMs >= _t5At) {
        _t5At = nowMs + _timers.t5Ms;
        _log("T5 (Reception Inactivity) expired");
        if (_hooks.inactivity) _hooks.inactivity("T5");
    }

    // 송출마다 T3·T4·T11 — 끝나는 송출이 _tx 를 바꾸므로 만료를 먼저 모은다.
    std::vector<std::string> giveUp, t11;
    for (auto& kv : _tx) {
        Tx& t = kv.second;
        if (t.t3At && nowMs >= t.t3At) {
            if (t.t3Count < kT3Retries) {
                ++t.t3Count;
                t.t3At = nowMs + _timers.t3Ms;
                _sendRevoke(t);   // 같은 원인·Indicator 로 재송신(§6.3.5.6.3)
            } else {
                giveUp.push_back(t.member);
            }
        }
        if (t.t4At && nowMs >= t.t4At) {
            if (t.c4 < _timers.c4) {
                ++t.c4;
                t.t4At = nowMs + _timers.t4Ms;
                _sendGranted(t);   // §6.3.4.4.9
            } else {
                t.t4At = 0;        // §6.3.4.4.10
            }
        }
        if (t.t11At && nowMs >= t.t11At) {
            t.t11At = 0;
            if (!t.revoking) t11.push_back(t.member);
        }
    }
    for (const auto& m : t11) {
        // T11 — 받는 이 없이 지난 송출에 서버 End Request #8 (§6.3.6.3.9 · §6.3.4.4.13)
        Tx* t = _txOf(m);
        if (!t || t->revoking) continue;
        _log("T11 (Stream Reception Idle) expired member=" + m + " — End Request #8");
        _revoke(*t, TC_REVOKE_NO_RECEIVER, true, nowMs);
    }
    for (const auto& m : giveUp) {
        // 회수 응답이 없다 — 서버에서 송출을 끝낸다(§6.3.5.6.3 NOTE: 구현 선택)
        _log("T3 (Transmission Revoke) gave up member=" + m);
        _endTransmission(m, nowMs, "revoke timeout");
    }

    // 참가자마다 — 늦은 암묵 허가 한도 · 'not permitted but sends media' 의 T3 · 수신 허가 T6
    for (auto& kv : _parts) {
        Part& p = kv.second;
        if (p.implicitPending && p.implicitUntil && nowMs >= p.implicitUntil) {
            // 첫 초대 참가자가 오지 않았다 — 참여자는 이미 요청을 접었다(§6.2.4.4.4). 예약을 풀고 지금 상태(Idle)를 알린다.
            p.implicitPending = false;
            p.implicitUntil = 0;
            FreeSsrc(p.implicitAudio);
            FreeSsrc(p.implicitVideo);
            p.implicitAudio = p.implicitVideo = 0;
            p.state = _tx.empty() ? MCV_U_IDLE : MCV_U_TAKEN;
            _log("implicit request expired (no invited participant in time) member=" + p.id);
            if (_tx.empty()) {
                ++_msgSeq;
                _sendIdle(p.id);
            } else {
                _notifyCurrent(p, nowMs);
            }
        }
        if (p.state == MCV_U_SENDS_MEDIA && p.t3At && nowMs >= p.t3At) {
            if (p.t3Count < kT3Retries) {
                ++p.t3Count;
                p.t3At = nowMs + _timers.t3Ms;
                std::vector<McvTlv> f{ McvTlv(TF_REJECT_CAUSE, McvCause(TC_REVOKE_NO_PERMISSION)) };
                _indicatorField(f);
                _send(p.id, MCV_APP_1, MCV1_TRANSMISSION_REVOKED, f);   // §6.3.5.7.3
            } else {
                p.t3At = 0;
                p.state = _tx.empty() ? MCV_U_IDLE : MCV_U_TAKEN;
            }
        }
        for (auto& g : p.active) {
            RxGrant& rg = g.second;
            if (!rg.t6At || nowMs < rg.t6At) continue;
            if (rg.c6 < _timers.c6) {
                ++rg.c6;
                rg.t6At = nowMs + _timers.t6Ms;
                if (const Tx* t = _txOf(g.first)) _sendRxResponse(p.id, t, nullptr, true, 0, true);   // §6.3.6.4.8
            } else {
                rg.t6At = 0;   // §6.3.6.4.9
            }
        }
    }
}
