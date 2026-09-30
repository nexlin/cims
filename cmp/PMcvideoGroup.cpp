#include "PMcvideoGroup.h"
#include "PLog.h"
#include "PTransmissionCodec.h"

#include <chrono>
#include <cstring>
#include <sys/time.h>
#include <arpa/inet.h>

// RTP 가 payload 를 싣는가 — 헤더만(keepalive, 단말 NAT 경로 유지)이면 송출 판정 밖이다(mcvideo.md §5.3.1 «미디어» 행).
static bool _rtpHasPayload(const char* buf, int len) {
    if (len < 12) return false;
    unsigned char b0 = (unsigned char)buf[0];
    int hdr = 12 + 4 * (b0 & 0x0F);
    if (b0 & 0x10) {   // 헤더 확장 (RFC 3550 §5.3.1)
        if (len < hdr + 4) return false;
        uint16_t words;
        memcpy(&words, buf + hdr + 2, 2);
        hdr += 4 + 4 * (int)ntohs(words);
    }
    int pad = (b0 & 0x20) ? (unsigned char)buf[len - 1] : 0;
    return len - hdr - pad > 0;
}

std::atomic<int> PMcvideoGroup::_activeGroups{0};

int64_t PMcvideoGroup::_nowUsec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

int64_t PMcvideoGroup::_nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

PMcvideoGroup::PMcvideoGroup(const std::string& groupId) : _groupId(groupId) {
    time(&_created);
    _lastActivity = _created;
    // 상태 머신 훅 — 전부 그룹 _mutex 아래에서 불린다(_ctl 호출이 모두 락 안이다).
    PMcvControl::Hooks h;
    h.send = [this](const std::string& member, int app, int subtype, const std::vector<McvTlv>& fields) {
        _sendControl(member, app, subtype, fields);
    };
    h.inactivity = [this](const char* timer) {
        LOG_INFO("PMcvideoGroup", "[%s] %s expired — TRANSMISSION_INACTIVITY", _groupId.c_str(), timer);
        if (_onInactivity) _onInactivity(_groupId, timer, _sesid, _svc);
    };
    h.transmittersChanged = [this](const std::vector<McvTransmitter>& v) {
        std::string who;
        for (const auto& t : v) who += (who.empty() ? "" : ",") + t.memberId;
        LOG_INFO("PMcvideoGroup", "[%s] transmitters [%s]", _groupId.c_str(), who.c_str());
        if (_onTransmitters) _onTransmitters(_groupId, v, _sesid, _svc);
    };
    h.log = [this](const std::string& line) { LOG_INFO("PMcvideoGroup", "[%s] tc: %s", _groupId.c_str(), line.c_str()); };
    _ctl.setHooks(h);
    ++_activeGroups;
    LOG_INFO("PMcvideoGroup", "[%s] created", _groupId.c_str());
}

PMcvideoGroup::~PMcvideoGroup() {
    close();
    --_activeGroups;
    LOG_INFO("PMcvideoGroup", "[%s] destroyed", _groupId.c_str());
}

// ── 세션 속성 ──────────────────────────────────────────────────────────

void PMcvideoGroup::setSessionMeta(const std::string& sesid, const std::string& svc, const std::string& subid) {
    PAutoLock lock(_mutex);
    _sesid = sesid;
    if (!svc.empty()) _svc = svc;
    if (!subid.empty()) _subid = subid;
}

std::string PMcvideoGroup::sessionSesid() {
    PAutoLock lock(_mutex);
    return _sesid;
}

void PMcvideoGroup::setConfig(bool prearranged, int maxTransmitters, bool receptionAutomatic, McvCallType callType,
                              const McvTimers& timers) {
    PAutoLock lock(_mutex);
    _prearranged = prearranged;
    _maxTransmitters = maxTransmitters;
    _receptionAutomatic = receptionAutomatic;
    _callType = callType;
    _timers = timers;
    _ctl.configure(maxTransmitters, receptionAutomatic, callType, timers);
}

void PMcvideoGroup::updateRoster(const std::map<std::string, int>& priorities,
                                 const std::map<std::string, std::string>& roles) {
    PAutoLock lock(_mutex);
    for (const auto& kv : priorities) _priorities[kv.first] = kv.second;
    for (const auto& kv : roles) _roles[kv.first] = kv.second;
    // 참가 중인 멤버의 기본 우선순위·chair 를 갱신한다(상태는 둔다 — 다음 요청부터 반영)
    int64_t now = _nowMs();
    for (const auto& kv : _members)
        if (kv.second.addressed) _ctl.addParticipant(kv.first, _ctlDecl(kv.first, kv.second.decl), now);
}

// 전송 제어가 쓰는 선언 — 로스터(<user-priority>·role)와 JOIN 협상 값을 합친다.
McvParticipantDecl PMcvideoGroup::_ctlDecl(const std::string& sessionId, const McvMemberDecl& d) const {
    McvParticipantDecl c;
    c.userId = d.userUri.empty() ? sessionId : d.userUri;
    c.recvOnly = d.recvOnly;
    c.queueing = d.queueing;
    auto ip = _priorities.find(sessionId);
    c.rosterPriority = ip != _priorities.end() ? ip->second : 0;
    c.maxPriority = d.maxPriority;
    auto ir = _roles.find(sessionId);
    c.chair = (ir != _roles.end() && ir->second == "chair") || d.role == "chair";
    c.maxRxStreams = d.maxRxStreams;
    c.preferredAudioSsrc = d.userAudioSsrc;
    c.preferredVideoSsrc = d.userVideoSsrc;
    return c;
}

bool PMcvideoGroup::prearranged() {
    PAutoLock lock(_mutex);
    return _prearranged;
}

int PMcvideoGroup::maxTransmitters() {
    PAutoLock lock(_mutex);
    return _maxTransmitters;
}

bool PMcvideoGroup::receptionAutomatic() {
    PAutoLock lock(_mutex);
    return _receptionAutomatic;
}

McvCallType PMcvideoGroup::callType() {
    PAutoLock lock(_mutex);
    return _callType;
}

McvTimers PMcvideoGroup::timers() {
    PAutoLock lock(_mutex);
    return _timers;
}

void PMcvideoGroup::setRecording(const std::string& recordDir, const std::string& sessionDir) {
    PAutoLock lock(_mutex);
    _recordDir = recordDir;
    _recordSesDir = sessionDir;
}

std::string PMcvideoGroup::recordDir() {
    PAutoLock lock(_mutex);
    return _recordDir;
}

// ── 멤버 ──────────────────────────────────────────────────────────────

unsigned int PMcvideoGroup::reserveMember(const std::string& sessionId, PMcvMemberPort* unit) {
    PAutoLock lock(_mutex);
    auto it = _members.find(sessionId);
    if (it != _members.end()) {
        it->second.unit = unit;
        return it->second.tcSsrc;
    }
    Peer p;
    p.id = sessionId;
    p.unit = unit;
    p.tcSsrc = PMcvControl::AllocSsrc();
    _members[sessionId] = p;
    LOG_INFO("PMcvideoGroup", "[%s] reserve member=%s tc_ssrc=%08x", _groupId.c_str(), sessionId.c_str(), p.tcSsrc);
    return p.tcSsrc;
}

int PMcvideoGroup::_declPort(const McvMemberDecl& d, McvChannel ch) {
    switch (ch) {
        case MCV_CH_AUDIO:      return d.port;
        case MCV_CH_VIDEO:      return d.videoPort;
        case MCV_CH_VIDEO_RTCP: return d.videoPort > 0 ? d.videoPort + 1 : 0;  // a=rtcp 없음 = RTP + 1 (RFC 3550 §11)
        case MCV_CH_CONTROL:    return d.controlPort;
    }
    return 0;
}

bool PMcvideoGroup::addMember(const std::string& sessionId, const McvMemberDecl& decl, bool implicitRequest,
                              PMcvControl::ImplicitResult* res) {
    PAutoLock lock(_mutex);
    auto it = _members.find(sessionId);
    if (it == _members.end()) return false;
    Peer& p = it->second;
    // 재-JOIN 이 선언(주소·nat·guard)을 바꾸지 않았으면 latch 로 추종한 목적지를 유지한다 — JOIN ② 재전송·refresh 가 활성
    //   latch 를 풀지 않도록 (PMcpttGroup 과 같은 규칙).
    bool same = p.addressed && p.decl.ip == decl.ip && p.decl.port == decl.port && p.decl.videoPort == decl.videoPort &&
                p.decl.controlPort == decl.controlPort && p.decl.nat == decl.nat && p.decl.sigIp == decl.sigIp;
    p.decl = decl;
    if (!same) {
        for (int ch = 0; ch < 4; ++ch) {
            p.dstIp[ch] = decl.ip;
            p.dstPort[ch] = _declPort(decl, (McvChannel)ch);
            p.latched[ch] = false;
        }
    }
    if (decl.nat && decl.sigIp.empty())
        LOG_WARN("PMcvideoGroup", "[%s] NAT member without sig-guard ip — latch IP guard disabled (session=%s)",
                 _groupId.c_str(), sessionId.c_str());
    bool first = !p.addressed;
    p.addressed = true;
    time(&_lastActivity);
    LOG_INFO("PMcvideoGroup", "[%s] %s member=%s uri=%s %s:%d video=%d control=%d nat=%d rx_streams=%d%s%s%s",
             _groupId.c_str(), first ? "add" : "update", sessionId.c_str(), decl.userUri.c_str(), decl.ip.c_str(),
             decl.port, decl.videoPort, decl.controlPort, decl.nat ? 1 : 0, decl.maxRxStreams,
             decl.recvOnly ? " recv_only" : "", implicitRequest ? " implicit_request" : "",
             same ? " (unchanged — keep latch)" : "");
    // 전송 제어 참가자 — 주소가 등록된 때부터(§6.3.5.1 — 200 OK 송수신 시점). 이미 있으면 협상 값만 갱신한다.
    _ctl.addParticipant(sessionId, _ctlDecl(sessionId, decl), _nowMs(), implicitRequest, res);
    return true;
}

void PMcvideoGroup::_releasePeer(Peer& peer) {
    PMcvControl::FreeSsrc(peer.tcSsrc);
    peer.tcSsrc = 0;
    peer.unit = nullptr;
}

void PMcvideoGroup::removeMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    auto it = _members.find(sessionId);
    if (it == _members.end()) return;
    // 송출·수신 정리가 먼저 — 남은 멤버에게 End Notify·Idle 이 이 멤버 포트로 나간다(떠나는 멤버에게는 보내지 않는다)
    _ctl.removeParticipant(sessionId, _nowMs());
    _releasePeer(it->second);
    _members.erase(it);
    _priorities.erase(sessionId);
    _roles.erase(sessionId);
    LOG_INFO("PMcvideoGroup", "[%s] remove member=%s (remaining %lu)", _groupId.c_str(), sessionId.c_str(),
             _members.size());
}

void PMcvideoGroup::close() {
    PAutoLock lock(_mutex);
    _ctl.close();   // 호 해제 — 메시지 없이 타이머·송출 SSRC 를 푼다(§6.3.4.6.2 · §6.3.4.7.2)
    for (auto& kv : _members) _releasePeer(kv.second);
    _members.clear();
}

void PMcvideoGroup::tick() {
    PAutoLock lock(_mutex);
    _ctl.tick(_nowMs());
}

bool PMcvideoGroup::hasMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    auto it = _members.find(sessionId);
    return it != _members.end() && it->second.addressed;
}

int PMcvideoGroup::getMemberCount() {
    PAutoLock lock(_mutex);
    int n = 0;
    for (const auto& kv : _members)
        if (kv.second.addressed) ++n;
    return n;
}

int PMcvideoGroup::getReservedCount() {
    PAutoLock lock(_mutex);
    return (int)_members.size();
}

unsigned int PMcvideoGroup::tcSsrcOf(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    auto it = _members.find(sessionId);
    return it != _members.end() ? it->second.tcSsrc : 0;
}

// ── 수신 ──────────────────────────────────────────────────────────────

// nat 멤버 수신 형식 검사 — 유닛 포트가 곧 멤버 신원이라 latch 는 신원 판정이 아니라 송신 목적지 학습이다
//   (ue_nat_traversal.md §5, PMcpttGroup::_natFormatOk 와 같은 규칙): 버전 2 + 최소 길이 + (guard) 소스 IP == sigIp
//   + RTP 는 (선언 시) 기대 ingress PT, RTCP 채널은 RTCP 패킷 종류(PT 192~223).
bool PMcvideoGroup::_natFormatOk(const Peer& peer, McvChannel ch, const std::string& ip, const char* buf,
                                 int len) const {
    bool rtcp = (ch == MCV_CH_VIDEO_RTCP || ch == MCV_CH_CONTROL);
    if (len < (rtcp ? 8 : 12) || (((unsigned char)buf[0]) >> 6) != 2) return false;
    if (!peer.decl.sigIp.empty() && peer.decl.sigIp != ip) return false;
    unsigned char b1 = (unsigned char)buf[1];
    if (rtcp) return b1 >= 192 && b1 <= 223;
    if (ch == MCV_CH_AUDIO && peer.decl.srcPt > 0 && (b1 & 0x7F) != (peer.decl.srcPt & 0x7F)) return false;
    return true;
}

void PMcvideoGroup::_natLatch(Peer& peer, McvChannel ch, const std::string& ip, int port) {
    bool changed = peer.dstIp[ch] != ip || peer.dstPort[ch] != port || !peer.latched[ch];
    peer.dstIp[ch] = ip;
    peer.dstPort[ch] = port;
    peer.latched[ch] = true;
    if (changed) {
        // 소스 경합(두 소스가 번갈아 유입) 시 로그 폭주 방지 — 멤버당 2 s 간격 요약.
        int64_t now = _nowUsec();
        if (now - peer.followLogUsec >= 2000000LL) {
            peer.followLogUsec = now;
            static const char* kName[] = { "audio RTP", "video RTP", "video RTCP", "control" };
            LOG_INFO("PMcvideoGroup", "[%s] %s dest follow (NAT) member=%s %s:%d", _groupId.c_str(), kName[ch],
                     peer.id.c_str(), ip.c_str(), port);
        }
    }
}

void PMcvideoGroup::_dropSrc(const char* what, const std::string& memberId, const std::string& ip, int port) {
    ++_srcDrop;
    time_t now;
    time(&now);
    if (now - _lastDropWarn >= 5) {
        _lastDropWarn = now;
        LOG_WARN("PMcvideoGroup", "[%s] drop %s member=%s src=%s:%d (total=%ld)", _groupId.c_str(), what,
                 memberId.c_str(), ip.c_str(), port, _srcDrop);
    }
}

void PMcvideoGroup::onMemberPacket(const std::string& memberId, McvChannel ch, const std::string& ip, int port,
                                   char* buf, int len) {
    PAutoLock lock(_mutex);
    auto it = _members.find(memberId);
    if (it == _members.end() || !it->second.addressed) {
        // JOIN(주소 전달) 전에 단말이 먼저 보낸 경우 — 정상 과도 상태. 해제 뒤 늦은 패킷도 여기로 온다.
        _dropSrc("pre-join", memberId, ip, port);
        return;
    }
    Peer& peer = it->second;
    // 선언(SDP) 주소 일치는 latch 상태와 무관하게 늘 수락 — 협상된 신원은 유지한다.
    int declPort = _declPort(peer.decl, ch);
    bool srcOk = (peer.dstIp[ch] == ip && peer.dstPort[ch] == port) || (peer.decl.ip == ip && declPort == port);
    if (!srcOk) {
        if (!peer.decl.nat || !_natFormatOk(peer, ch, ip, buf, len)) {
            _dropSrc(ch == MCV_CH_CONTROL ? "control" : "media", memberId, ip, port);
            return;
        }
        _natLatch(peer, ch, ip, port);
    }
    time(&_lastActivity);

    switch (ch) {
        case MCV_CH_CONTROL:
            _onControl(peer, buf, len);
            break;
        case MCV_CH_AUDIO:
        case MCV_CH_VIDEO: {
            // 허가된 송출만 분배한다(수신자별 Active SSRC List — TS 24.581 §6.3.7). 헤더만인 keepalive 는 판정 밖 — 버린다.
            if (!_rtpHasPayload(buf, len)) break;
            unsigned int audioSsrc = 0, videoSsrc = 0;
            if (_ctl.onMedia(memberId, _nowMs(), audioSsrc, videoSsrc))
                _distribute(peer, ch, ch == MCV_CH_AUDIO ? audioSsrc : videoSsrc, buf, len);
            else
                ++_noGrantDrop;   // 허가 없는 미디어 — 상태 머신이 Revoked #3 을 보낸다(§6.3.5.4.6)
            break;
        }
        case MCV_CH_VIDEO_RTCP:
            // 수신자 PLI·FIR 는 media SSRC 가 가리키는 송출자에게 넘긴다(RFC 4585·5104) — 송출이 없으면 받을 곳이 없다.
            break;
    }
}

// 전송 제어 채널 수신 (TS 24.581 §9.1). 한 datagram 에 RTCP 패킷이 여럿(compound) 올 수 있어 헤더 length 로 나눠 APP 만 푼다.
//   APP 이 아닌 RTCP — 단말의 빈 RR keepalive(NAT 하향 경로 유지, RFC 3550 §6.4.2) — 는 해석하지 않고 버린다(드롭으로 세지 않는다).
void PMcvideoGroup::_onControl(Peer& peer, const char* buf, int len) {
    int off = 0;
    while (off + 4 <= len) {
        uint16_t words;
        memcpy(&words, buf + off + 2, 2);
        int plen = ((int)ntohs(words) + 1) * 4;
        if ((((unsigned char)buf[off]) >> 6) != 2 || plen > len - off) {
            _dropSrc("control(malformed)", peer.id, peer.dstIp[MCV_CH_CONTROL], peer.dstPort[MCV_CH_CONTROL]);
            return;
        }
        if ((unsigned char)buf[off + 1] == MCV_RTCP_PT_APP) _onControlMessage(peer, buf + off, plen);
        off += plen;
    }
}

// 전송 제어 메시지 하나(RTCP APP MCV0/1/2). 멤버 → CMP 헤더 SSRC 는 JOIN 응답 tc_ssrc 여야 하지만(§4.3.3.1) 1차는 멤버 전용 포트라
//   다른 값이 와도 받고 기록만 한다(cmp_media_api.md §7.9 SSRC 규칙).
void PMcvideoGroup::_onControlMessage(Peer& peer, const char* buf, int len) {
    ParsedTransmission msg;
    if (!ParseTransmissionMessage(buf, len, msg)) {
        // MCV0~2 APP 이 아니거나 이 판본이 모르는 subtype — 메시지 전체를 버린다(§9.1.4 1)
        _dropSrc("control(unknown message)", peer.id, peer.dstIp[MCV_CH_CONTROL], peer.dstPort[MCV_CH_CONTROL]);
        return;
    }
    if (msg.ssrc != peer.uaTcSsrc) {
        if (msg.ssrc != peer.tcSsrc)
            LOG_INFO("PMcvideoGroup", "[%s] member=%s control header SSRC %08x != tc_ssrc %08x (accepted)",
                     _groupId.c_str(), peer.id.c_str(), msg.ssrc, peer.tcSsrc);
        peer.uaTcSsrc = msg.ssrc;
    }
    ++_controlRx;
    const char* name = McvMessageName(msg.app, msg.subtype);
    LOG_DEBUG("PMcvideoGroup", "[%s] control rx member=%s MCV%d %s (subtype=0x%02x fields=%lu)", _groupId.c_str(),
              peer.id.c_str(), msg.app, name, msg.subtype, msg.fields.size());
    if (_logFn) {
        std::string label = std::string("MCV") + char('0' + msg.app) + " " + name;
        _logFn(peer.id.c_str(), "cmp", "MCVIDEO", label.c_str(), "", _sesid, _svc, _subid);
    }
    _ctl.onMessage(peer.id, msg, _nowMs());
}

// 허가된 송출 하나의 RTP 를 그 송출을 받는 멤버(Active SSRC List)에게 — SSRC = 송출 할당값(§6.3.4.3.3 d, cmp_media_api.md §7.9
//   SSRC 규칙: 단말이 SSRC 를 바꾸지 못해도 분배·수신자 구분이 맞는다), PT = 수신 leg 의 egress PT(선언 시).
void PMcvideoGroup::_distribute(const Peer& sender, McvChannel ch, unsigned int ssrc, const char* buf, int len) {
    char out[2048];
    if (len > (int)sizeof(out)) return;
    memcpy(out, buf, len);
    uint32_t nssrc = htonl(ssrc);
    memcpy(out + 8, &nssrc, 4);
    const unsigned char origPt = (unsigned char)buf[1];
    for (auto& kv : _members) {
        Peer& r = kv.second;
        if (!r.addressed || !r.unit || r.id == sender.id || r.dstPort[ch] <= 0) continue;
        if (!_ctl.receives(r.id, sender.id)) continue;
        int pt = ch == MCV_CH_AUDIO ? r.decl.ptOut : r.decl.videoPtOut;
        out[1] = pt > 0 ? (char)((origPt & 0x80) | (pt & 0x7F)) : (char)origPt;
        r.unit->sendTo(ch, r.dstIp[ch], r.dstPort[ch], out, len);
    }
}

// 전송 제어 메시지 송신 — 헤더 SSRC = 멤버가 SDP 에 광고한 mc_transmission_ssrc(user_tc_ssrc, TS 24.581 §4.3.3.1 «defined by the
//   receiving entity»), 없으면 tc_ssrc. 목적지 = 제어 채널(nat 멤버는 latch 한 소스).
void PMcvideoGroup::_sendControl(const std::string& memberId, int app, int subtype, const std::vector<McvTlv>& fields) {
    auto it = _members.find(memberId);
    if (it == _members.end() || !it->second.addressed || !it->second.unit) return;
    Peer& p = it->second;
    const char* name = McvMessageName(app, subtype);
    if (p.dstPort[MCV_CH_CONTROL] <= 0) {
        LOG_WARN("PMcvideoGroup", "[%s] member=%s no control port — MCV%d %s not sent", _groupId.c_str(), memberId.c_str(),
                 app, name);
        return;
    }
    char buf[512];
    unsigned int hdrSsrc = p.decl.userTcSsrc ? p.decl.userTcSsrc : p.tcSsrc;
    int n = BuildTransmissionMessage(buf, sizeof(buf), app, (unsigned char)subtype, hdrSsrc, fields);
    if (n <= 0) {
        LOG_ERROR("PMcvideoGroup", "[%s] member=%s MCV%d %s encode failed", _groupId.c_str(), memberId.c_str(), app, name);
        return;
    }
    p.unit->sendTo(MCV_CH_CONTROL, p.dstIp[MCV_CH_CONTROL], p.dstPort[MCV_CH_CONTROL], buf, n);
    LOG_DEBUG("PMcvideoGroup", "[%s] control tx member=%s MCV%d %s (subtype=0x%02x fields=%lu)", _groupId.c_str(),
              memberId.c_str(), app, name, subtype, fields.size());
    if (_logFn) {
        std::string label = std::string("MCV") + char('0' + app) + " " + name;
        _logFn("cmp", memberId.c_str(), "MCVIDEO", label.c_str(), "", _sesid, _svc, _subid);
    }
}

// ── 관측 ──────────────────────────────────────────────────────────────

time_t PMcvideoGroup::getLastActivityTime() {
    PAutoLock lock(_mutex);
    return _lastActivity;
}

long PMcvideoGroup::getSrcDrop() {
    PAutoLock lock(_mutex);
    return _srcDrop;
}

long PMcvideoGroup::getNoGrantDrop() {
    PAutoLock lock(_mutex);
    return _noGrantDrop;
}

long PMcvideoGroup::getControlRx() {
    PAutoLock lock(_mutex);
    return _controlRx;
}

int PMcvideoGroup::getTransmitterCount() {
    PAutoLock lock(_mutex);
    return _ctl.transmitterCount();
}

int PMcvideoGroup::getReceptionCount() {
    PAutoLock lock(_mutex);
    return _ctl.receptionCount();
}

void PMcvideoGroup::collectNatLatched(std::vector<std::tuple<std::string, std::string, int>>& out) {
    PAutoLock lock(_mutex);
    for (const auto& kv : _members) {
        const Peer& p = kv.second;
        if (p.decl.nat && p.latched[MCV_CH_AUDIO]) out.emplace_back(kv.first, p.dstIp[MCV_CH_AUDIO], p.dstPort[MCV_CH_AUDIO]);
    }
}
