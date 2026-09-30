#include "PMcvideoGroup.h"
#include "PLog.h"
#include "PTransmissionCodec.h"

#include <set>
#include <mutex>
#include <random>
#include <cstring>
#include <sys/time.h>
#include <arpa/inet.h>

// ── 전역 SSRC 할당 ─────────────────────────────────────────────────────
//   송출 SSRC(Audio·Video 쌍)는 송출마다 전역 유일해야 하고(TS 24.581 §6.3.4.3.3 d) 전송 제어 채널 tc_ssrc 도 한 IP·포트에
//   여러 세션을 다중화할 때의 열쇠라(§4.3.3.1) 같은 공간에서 겹치지 않게 뽑는다. 0 은 «미상» 표식이라 쓰지 않는다.
static std::mutex gSsrcMtx;
static std::set<unsigned int> gSsrcInUse;

unsigned int PMcvideoGroup::AllocSsrc(unsigned int preferred) {
    static std::mt19937 rng(std::random_device{}());
    std::lock_guard<std::mutex> lock(gSsrcMtx);
    // 단말이 offer 의 a=ssrc 로 준 값 — 전역에서 쓰이지 않으면 그대로(TS 24.581 §14.3.7·§14.3.8 «value included in the SDP offer or
    //   new ssrc value if collision is detected»). 충돌 판정이 전역인 것은 §6.3.4.3.3 d «globally unique».
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

void PMcvideoGroup::FreeSsrc(unsigned int ssrc) {
    if (ssrc == 0) return;
    std::lock_guard<std::mutex> lock(gSsrcMtx);
    gSsrcInUse.erase(ssrc);
}

int64_t PMcvideoGroup::_nowUsec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

PMcvideoGroup::PMcvideoGroup(const std::string& groupId) : _groupId(groupId) {
    time(&_created);
    _lastActivity = _created;
    LOG_INFO("PMcvideoGroup", "[%s] created", _groupId.c_str());
}

PMcvideoGroup::~PMcvideoGroup() {
    close();
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

void PMcvideoGroup::setConfig(bool prearranged, int maxTransmitters, bool receptionAutomatic, const McvTimers& timers) {
    PAutoLock lock(_mutex);
    _prearranged = prearranged;
    _maxTransmitters = maxTransmitters;
    _receptionAutomatic = receptionAutomatic;
    _timers = timers;
}

void PMcvideoGroup::updateRoster(const std::map<std::string, int>& priorities,
                                 const std::map<std::string, std::string>& roles) {
    PAutoLock lock(_mutex);
    for (const auto& kv : priorities) _priorities[kv.first] = kv.second;
    for (const auto& kv : roles) _roles[kv.first] = kv.second;
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
    p.tcSsrc = AllocSsrc();
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

bool PMcvideoGroup::addMember(const std::string& sessionId, const McvMemberDecl& decl) {
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
    LOG_INFO("PMcvideoGroup", "[%s] %s member=%s uri=%s %s:%d video=%d control=%d nat=%d rx_streams=%d%s",
             _groupId.c_str(), first ? "add" : "update", sessionId.c_str(), decl.userUri.c_str(), decl.ip.c_str(),
             decl.port, decl.videoPort, decl.controlPort, decl.nat ? 1 : 0, decl.maxRxStreams,
             same ? " (unchanged — keep latch)" : "");
    return true;
}

void PMcvideoGroup::_releasePeer(Peer& peer) {
    FreeSsrc(peer.tcSsrc);
    peer.tcSsrc = 0;
    peer.unit = nullptr;
}

void PMcvideoGroup::removeMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    auto it = _members.find(sessionId);
    if (it == _members.end()) return;
    _releasePeer(it->second);
    _members.erase(it);
    _priorities.erase(sessionId);
    _roles.erase(sessionId);
    LOG_INFO("PMcvideoGroup", "[%s] remove member=%s (remaining %lu)", _groupId.c_str(), sessionId.c_str(),
             _members.size());
}

void PMcvideoGroup::close() {
    PAutoLock lock(_mutex);
    for (auto& kv : _members) _releasePeer(kv.second);
    _members.clear();
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
        case MCV_CH_VIDEO:
            // 허가된 송출만 분배한다(수신자별 Active SSRC List — TS 24.581 §6.3.7). 허가 전 미디어는 버린다.
            ++_noGrantDrop;
            break;
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

void PMcvideoGroup::collectNatLatched(std::vector<std::tuple<std::string, std::string, int>>& out) {
    PAutoLock lock(_mutex);
    for (const auto& kv : _members) {
        const Peer& p = kv.second;
        if (p.decl.nat && p.latched[MCV_CH_AUDIO]) out.emplace_back(kv.first, p.dstIp[MCV_CH_AUDIO], p.dstPort[MCV_CH_AUDIO]);
    }
}
