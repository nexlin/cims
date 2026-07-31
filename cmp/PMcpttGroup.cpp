#include "PMcpttGroup.h"
#include "PLog.h"
#include "PRtpMulticast.h"
#include "PPttMemberPort.h"
#include "PSyncRtpRecorder.h"
#include <cstring>
#include <arpa/inet.h>
#include <cstdio>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>

unsigned int PMcpttGroup::_nextSsrc = 1000;

// 세션 로컬 floor 이벤트 기록 (_recordDir/floor.jsonl). 크래시-세이프 append.
void PMcpttGroup::_logFloorLocal(const char* op, const std::string& user, unsigned int ssrc, int prio,
                                 const char* extraJson)
{
    if (_recordDir.empty()) return;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char ts[48];
    int n = (int)strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(ts + n, sizeof(ts) - n, ".%06ld", (long)tv.tv_usec);

    // 시간버킷 {base}/{YYYY}/{MM}/{DD}/{HH}/floor.jsonl (mkdir -p)
    char hb[32];
    snprintf(hb, sizeof(hb), "/%04d/%02d/%02d/%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour);
    std::string hourDir = _recordDir + hb;
    {
        std::string p = hourDir;
        for (size_t i = 1; i < p.size(); ++i)
            if (p[i] == '/') { p[i] = '\0'; mkdir(p.c_str(), 0755); p[i] = '/'; }
        mkdir(p.c_str(), 0755);
    }
    std::string path = hourDir + "/floor.jsonl";
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return;
    fprintf(f, "{\"ts\":\"%s\",\"op\":\"%s\",\"user\":\"%s\",\"ssrc\":%u,\"prio\":%d%s%s}\n",
            ts, op, user.c_str(), ssrc, prio,
            (extraJson && extraJson[0]) ? "," : "",
            (extraJson && extraJson[0]) ? extraJson : "");
    fclose(f);
}

// Floor 패킷 빌드/파싱 헬퍼(TS 24.380 §8.2 RTCP APP "MCPT" + TLV)는 PFloorCodec.cpp
// 에 분리되어 있다(단말 floor/FloorCodec.kt 와 동일 규약, 단위테스트 대상).

PMcpttGroup::PMcpttGroup(const std::string& groupId)
    : _groupId(groupId), _pttSession(NULL),
      _recordEnable(false), _recorder(NULL)
{
}

// ── floor 정책 (floor_control 유무 × floor_policy 동시성) ─────────────────────
int ParseFloorPolicy(const std::string& s) {
    if (s == "dual")  return FLOOR_POLICY_DUAL;
    if (s == "multi") return FLOOR_POLICY_MULTI;
    return FLOOR_POLICY_SINGLE;
}

void PMcpttGroup::setFloorPolicy(bool floorControl, int policy, int maxTalkers, bool privateCall) {
    PAutoLock lock(_mutex);
    _floorControl = floorControl;
    _privateCall = privateCall;
    // private call 은 2인 세션이라 동시성 정책(dual/multi)이 성립하지 않는다 —
    //   TS 24.380 §7 private-call floor 절차(정원 1, 큐 없음)를 적용한다.
    _floorPolicy = privateCall ? FLOOR_POLICY_SINGLE : policy;

    if (!floorControl) {
        _talkerCapacity = 0;          // 중재 없음 — grant 자체가 없다(full-duplex)
    } else if (_floorPolicy == FLOOR_POLICY_MULTI) {
        int n = maxTalkers > 0 ? maxTalkers : 2;
        if (n < 2) n = 2;
        if (n > MCPTT_MAX_TALKER_SLOTS) n = MCPTT_MAX_TALKER_SLOTS;
        _talkerCapacity = n;
    } else if (_floorPolicy == FLOOR_POLICY_DUAL) {
        _talkerCapacity = 2;          // 2번째 자리는 override 전용(§B.1)
    } else {
        _talkerCapacity = 1;
    }
    // private call 은 큐잉 절차가 없다 — 점유 중 요청은 Deny(another client).
    //   비-private 로 되돌아오면 큐잉도 함께 복구된다(정책이 sticky 하지 않도록).
    _queueEnable = !privateCall;

    LOG_INFO("PMcpttGroup", "[%s] floor policy: control=%s policy=%s capacity=%d%s",
             _groupId.c_str(), floorControl ? "on" : "off", _policyName(), _talkerCapacity,
             privateCall ? " (private call)" : "");

    // 정책 변경(MODIFY)으로 정원이 줄면 초과 발언자를 회수한다 — 정원과 실제 발언자 수가
    //   어긋난 채로 남으면 이후 판정(선점/승급)이 정책과 다르게 동작한다.
    bool revoked = false;
    while ((int)_talkers.size() > _talkerCapacity) {
        Talker* weakest = _weakestTalker();
        if (!weakest) break;
        revoked = true;
        const std::string owner = weakest->sessionId;
        unsigned int ssrc = weakest->ssrc;
        int prio = weakest->prio;
        if (_floorControl) {   // floor 유지 중이면 규격대로 REVOKE 통지
            char revBuf[256];
            std::vector<FloorTlv> f{ FloorTlv(FF_REJECT_CAUSE, FloorU16(CAUSE_OTHER)),
                                     FloorTlv(FF_GRANTED_PARTY, owner) };
            int revLen = BuildFloorMessage(revBuf, sizeof(revBuf), FLOOR_REVOKE, ssrc, f);
            if (revLen > 0) sendToMember(owner, revBuf, revLen);
        }
        char ex[96];
        snprintf(ex, sizeof(ex), "\"reason\":\"policy_change\",\"cause\":%d", CAUSE_OTHER);
        _logFloorLocal("REVOKE", owner, ssrc, prio, ex);
        LOG_INFO("PMcpttGroup", "[%s] Floor revoked by policy change: %s (capacity=%d)",
                 _groupId.c_str(), owner.c_str(), _talkerCapacity);
        _dropTalker(owner);
    }
    if (!floorControl) {
        // floor 중재를 끄면 발언 개념이 사라진다 — 대기열도 비우고, 멤버마다 상향 스트림
        //   슬롯을 부여한다(그러지 않으면 전원이 슬롯 0 을 공유해 수신자에게 SSRC 가 겹친다).
        _floorQueue.clear();
        _streamSlotNext = 0;
        for (auto& [sid, peer] : _members)
            peer.streamSlot = (_streamSlotNext++) % MCPTT_MAX_TALKER_SLOTS;
        if ((int)_members.size() > MCPTT_MAX_TALKER_SLOTS)
            LOG_WARN("PMcpttGroup", "[%s] floor_control=off with %lu members — 슬롯 %d개를 넘어 "
                     "상향 스트림 SSRC 가 겹친다(1:1 private call 용도)",
                     _groupId.c_str(), _members.size(), MCPTT_MAX_TALKER_SLOTS);
    }
    if (revoked) {
        if (_talkers.empty()) broadcastFloorStatus(FLOOR_IDLE, 0, "");
        _notifyTalkers();
    }

    // 정원이 늘면 녹취 슬롯 트랙을 미리 등록(세그먼트 시작 시 파일이 열린다).
    if (_recordEnable && _recorder) _recEnsureTracks(_talkerCapacity > 0 ? _talkerCapacity : 1);
}

const char* PMcpttGroup::_policyName() const {
    if (!_floorControl) return "off";
    if (_privateCall)   return "private";
    return _floorPolicy == FLOOR_POLICY_MULTI ? "multi"
         : _floorPolicy == FLOOR_POLICY_DUAL  ? "dual" : "single";
}

std::string PMcpttGroup::_slotTrack(int slot, bool video) {
    std::string base = video ? "video" : "audio";
    return slot <= 0 ? base : base + std::to_string(slot);
}

PMcpttGroup::Talker* PMcpttGroup::_talkerOf(const std::string& sessionId) {
    for (auto& t : _talkers) if (t.sessionId == sessionId) return &t;
    return nullptr;
}

bool PMcpttGroup::_isTalker(const std::string& sessionId) const {
    for (const auto& t : _talkers) if (t.sessionId == sessionId) return true;
    return false;
}

void PMcpttGroup::getFloorHolders(std::vector<std::string>& out) {
    PAutoLock lock(_mutex);
    for (const auto& t : _talkers) out.push_back(t.sessionId);
}

std::string PMcpttGroup::getFloorPolicyName() {
    PAutoLock lock(_mutex);
    return _policyName();
}

PMcpttGroup::~PMcpttGroup() {
    // 녹취 종료 먼저 — stopRecording() 이 _recorder 포인터를 lock 하에서 swap 하여
    // onRtpPacket/onVideoRtpPacket 의 writePacket 경합이 차단된다. 이후 멤버 정리.
    stopRecording();
    PAutoLock lock(_mutex);
    _members.clear();
}

void PMcpttGroup::addMember(const std::string& sessionId, const std::string& ip, int port, int floorPort, int videoPort,
                            const std::string& role, PPttMemberPort* unit, bool nat, const std::string& sigIp,
                            int ptOut, int srcPt, int tePtOut, int srcTePt, const std::string& codec,
                            bool recvOnly, bool floorSuppress) {
    if (ptOut || srcPt || tePtOut || srcTePt)
        LOG_INFO("PMcpttGroup", "[%s] addMember session=%s ip=%s rtp=%d floor=%d video=%d role=%s nat=%d pt=%d/%d te=%d/%d",
                 _groupId.c_str(), sessionId.c_str(), ip.c_str(), port, floorPort, videoPort, role.c_str(),
                 nat ? 1 : 0, ptOut, srcPt, tePtOut, srcTePt);
    else
        LOG_INFO("PMcpttGroup", "[%s] addMember session=%s ip=%s rtp=%d floor=%d video=%d role=%s nat=%d", _groupId.c_str(), sessionId.c_str(), ip.c_str(), port, floorPort, videoPort, role.c_str(), nat ? 1 : 0);
    // NAT 멤버인데 guard IP(user_sig_ip)가 비면 latch IP guard(_acceptNatRtp)가 이 멤버에
    //   한해 무력화된다(SSRC 핀만 방어) — CSP UserMap 미조회(미등록 등)가 원인. 조용히
    //   약화되지 않도록 드러낸다.
    if (nat && sigIp.empty())
        LOG_WARN("PMcpttGroup", "[%s] NAT member without sig-guard ip — latch IP guard disabled (session=%s)",
                 _groupId.c_str(), sessionId.c_str());
    PAutoLock lock(_mutex);
    // 재-JOIN(주소 갱신) 시 기존 SSRC/seq 카운터 보존 — 주소·역할만 갱신 + NAT latch 리셋.
    auto itExist = _members.find(sessionId);
    if (itExist != _members.end()) {
        Peer& peer = itExist->second;
        // 동일 선언 재수신(주소·nat·guard 불변) — latch/학습 목적지 유지 (JOIN ② 재전송·refresh 가
        //   활성 latch 를 풀지 않도록). 비교는 선언 원본(decl*) 기준 — peer.ip/port 는 latch 시
        //   학습 주소로 덮인다 (PRtpRelay::setRemote 와 동일 규칙).
        bool sameDecl = (peer.declIp == ip && peer.declPort == port && peer.declVideoPort == videoPort &&
                         peer.natEnabled == nat && peer.sigIp == sigIp);
        // PT 재작성 파라미터는 주소 불변 재-JOIN(재협상)에서도 항상 최신 선언을 따른다.
        peer.ptOut = ptOut;
        peer.srcPt = srcPt;
        peer.tePtOut = tePtOut;
        peer.srcTePt = srcTePt;
        if (!codec.empty()) peer.codec = codec;
        peer.declIp = ip;
        peer.declPort = port;
        peer.declVideoPort = videoPort;
        peer.recvOnly = recvOnly;              // ambient 플래그도 최신 선언을 따른다
        peer.floorSuppress = floorSuppress;
        if (floorPort > 0) peer.floorPort = floorPort;
        if (!role.empty()) { peer.role = role; _roles[sessionId] = role; }
        if (unit) peer.unit = unit;
        if (sameDecl) {
            // 재전송·세션 refresh — latch(추종 학습된) 목적지 유지. 목적지 갱신은 추종
            //   모델(_acceptNatRtp)이 미디어 소스로부터 계속 수행한다.
            LOG_INFO("PMcpttGroup", "[%s] Member unchanged session=%s — keep latch (total=%lu)", _groupId.c_str(),
                     sessionId.c_str(), _members.size());
            return;
        }
        peer.ip = ip;
        peer.port = port;
        peer.videoPort = videoPort;
        peer.natEnabled = nat;
        peer.sigIp = sigIp;
        peer.natLatched = peer.natLatchedVideo = false;
        LOG_INFO("PMcpttGroup", "[%s] Member updated session=%s (total=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());
        return;
    }
    Peer peer;
    peer.id = sessionId;
    peer.declIp = ip;
    peer.declPort = port;
    peer.declVideoPort = videoPort;
    peer.ip = ip;
    peer.port = port;
    peer.floorPort = floorPort;
    peer.videoPort = videoPort;
    peer.role = role.empty() ? "participant" : role;
    if (!role.empty()) _roles[sessionId] = role;
    peer.natEnabled = nat;
    peer.sigIp = sigIp;
    peer.ptOut = ptOut;
    peer.srcPt = srcPt;
    peer.tePtOut = tePtOut;
    peer.srcTePt = srcTePt;
    peer.codec = codec;
    // SSRC 배정 공간 분리 — 한 카운터의 근접 오프셋(+1000/+2000)이면 누적 발행 시
    //   멤버 ssrc 와 송출 SSRC 범위가 겹치므로 상위 비트로 격리한다.
    peer.ssrc = _nextSsrc++;
    peer.unit = unit;
    peer.recvOnly = recvOnly;
    peer.floorSuppress = floorSuppress;
    // floor off(full-duplex) 그룹의 상향 스트림 슬롯 — 멤버마다 달라야 수신자가 두 송신을
    //   서로 다른 SSRC 로 구분한다 (floor 有 그룹은 발언 슬롯이 이 역할을 한다).
    peer.streamSlot = _floorControl ? 0 : (_streamSlotNext++ % MCPTT_MAX_TALKER_SLOTS);
    _members[sessionId] = peer;
    LOG_INFO("PMcpttGroup", "[%s] Member added session=%s (total=%lu)%s%s", _groupId.c_str(), sessionId.c_str(),
             _members.size(), recvOnly ? " recv_only" : "", floorSuppress ? " floor_suppress" : "");
    if (_logFlow) _logFlow(_groupId, "ue", "cmp", "MCPTT", "MEMBER_JOIN", sessionId.c_str());

    // 발언 중인 화자가 있으면 신규 멤버에게 Floor Taken 통지 (화자 identity + Indicator).
    //   동시 발언(dual/multi)이면 화자마다 1건. floor 억제(ambient) 멤버에겐 보내지 않는다.
    if (!floorSuppress) {
        for (const auto& t : _talkers) {
            char pktBuf[256];
            std::vector<FloorTlv> f{ FloorTlv(FF_GRANTED_PARTY, t.sessionId),
                                     FloorTlv(FF_FLOOR_INDICATOR, FloorU16(_indicatorFor(t.sessionId))) };
            int pktLen = BuildFloorMessage(pktBuf, sizeof(pktBuf), FLOOR_TAKEN, t.ssrc, f);
            if (pktLen > 0)
                sendToMember(sessionId, pktBuf, pktLen);
            LOG_DEBUG("PMcpttGroup", "[%s] Notified new member %s about floor taken by %s",
                      _groupId.c_str(), sessionId.c_str(), t.sessionId.c_str());
        }
    }

    // Private call (TS 24.380 §7): 개시자는 호 성립 시점에 발언권을 가진다 —
    //   개시자 leg 가 붙는 즉시 초기 부여(1회). floor 없는 private(full-duplex)은 해당 없음.
    if (_privateCall && _floorControl && !_initialGrantDone && _talkers.empty() &&
        !_initiatorSessionId.empty() && sessionId == _initiatorSessionId && !recvOnly && !floorSuppress) {
        _initialGrantDone = true;
        int prio = 0;
        auto itP = _priorities.find(sessionId);
        if (itP != _priorities.end()) prio = itP->second;
        LOG_INFO("PMcpttGroup", "[%s] Private call — initial floor to initiator %s",
                 _groupId.c_str(), sessionId.c_str());
        _grantFloorTo(sessionId, _members[sessionId].ssrc, prio, false, "");
    }
}

void PMcpttGroup::removeMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    _members.erase(sessionId);
    _roles.erase(sessionId);
    // 떠난 멤버를 floor 대기열에서 제거.
    for (auto it = _floorQueue.begin(); it != _floorQueue.end(); )
        it = (it->sessionId == sessionId) ? _floorQueue.erase(it) : it + 1;
    LOG_INFO("PMcpttGroup", "[%s] Member %s left. (remaining=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());
    if (_logFlow) _logFlow(_groupId, "ue", "cmp", "MCPTT", "MEMBER_LEAVE", sessionId.c_str());

    // 발언 중이던 멤버가 떠났으면 그 발언만 걷어내고 상태를 수렴시킨다
    //   (동시 발언 중이면 나머지 화자는 그대로 유지).
    if (_dropTalker(sessionId)) {
        LOG_INFO("PMcpttGroup", "[%s] Floor holder %s left.", _groupId.c_str(), sessionId.c_str());
        _advanceFloorOrIdle();
        _notifyTalkers();
    }
}

void PMcpttGroup::updatePriorities(const std::map<std::string, int>& priorities) {
    PAutoLock lock(_mutex);
    _priorities = priorities;
    LOG_INFO("PMcpttGroup", "[%s] Priorities updated for %lu members", _groupId.c_str(), _priorities.size());
}

void PMcpttGroup::updateRoles(const std::map<std::string, std::string>& roles) {
    PAutoLock lock(_mutex);
    _roles = roles;
    LOG_INFO("PMcpttGroup", "[%s] Roles updated for %lu members", _groupId.c_str(), _roles.size());
}

// ── Floor condition tier (TS 24.380): emergency > imminent > normal ──
int ParseFloorTier(const std::string& s) {
    if (s == "emergency" || s == "2") return TIER_EMERGENCY;
    if (s == "imminent" || s == "imminent_peril" || s == "1") return TIER_IMMINENT;
    return TIER_NORMAL;
}
static const char* _tierName(int t) {
    return t >= TIER_EMERGENCY ? "emergency" : t == TIER_IMMINENT ? "imminent" : "normal";
}
void PMcpttGroup::updateTiers(const std::map<std::string, int>& tiers) {
    PAutoLock lock(_mutex);
    _tier = tiers;
    LOG_INFO("PMcpttGroup", "[%s] Tiers updated for %lu members", _groupId.c_str(), _tier.size());
}
void PMcpttGroup::setTier(const std::string& sessionId, int tier) {
    PAutoLock lock(_mutex);
    if (tier <= TIER_NORMAL) _tier.erase(sessionId);
    else _tier[sessionId] = tier;
    LOG_INFO("PMcpttGroup", "[%s] Tier set session=%s tier=%s", _groupId.c_str(), sessionId.c_str(), _tierName(tier));
}
int PMcpttGroup::tierOf(const std::string& sessionId) const {
    auto it = _tier.find(sessionId);
    return it != _tier.end() ? it->second : TIER_NORMAL;
}

bool PMcpttGroup::isChair(const std::string& sessionId) const {
    auto it = _roles.find(sessionId);
    return ( it != _roles.end() && it->second == "chair" );
}

void PMcpttGroup::setDtmfConfig(bool enable, const std::string& pushDigit, const std::string& releaseDigit) {
    PAutoLock lock(_mutex);
    _dtmfEnable = enable;
    _dtmfPushDigit = pushDigit;
    _dtmfReleaseDigit = releaseDigit;
    LOG_INFO("PMcpttGroup", "[%s] DTMF config: enable=%d push=%s release=%s", _groupId.c_str(), enable, pushDigit.c_str(), releaseDigit.c_str());
}

bool PMcpttGroup::hasMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    return _members.find(sessionId) != _members.end();
}

// Floor 메시지 타입명 (로그용) — TS 24.380 subtype.
static const char* _floorOpName(int subtype) {
    switch (subtype) {
        case FLOOR_REQUEST: return "REQUEST";
        case FLOOR_GRANT:   return "GRANT";
        case FLOOR_TAKEN:   return "TAKEN";
        case FLOOR_REJECT:  return "DENY";
        case FLOOR_RELEASE: return "RELEASE";
        case FLOOR_IDLE:    return "IDLE";
        case FLOOR_REVOKE:  return "REVOKE";
        case FLOOR_QUEUE_POS_REQ:  return "QUEUE_POS_REQ";
        case FLOOR_QUEUE_POS_INFO: return "QUEUE_POS_INFO";
        case FLOOR_ACK:     return "ACK";
        case FLOOR_RELEASE_MULTI: return "RELEASE_MULTI";
        default:            return "?";
    }
}

void PMcpttGroup::onFloorPacket(const std::string& ip, int port, char* buf, int len) {
    // SRTCP 보호가 켜져 있으면 인증·복호 후 파싱 (TS 33.180 — floor 만 보호, 미디어는 투명).
    char plain[2048];
    if (_floorCrypto.enabled()) {
        int plainLen = 0;
        if (!_floorCrypto.unprotect(buf, len, plain, sizeof(plain), plainLen)) {
            long total = ++_floorCryptoDrop;   // atomic — 수신 스레드와 STATS 조회가 경합
            LOG_WARN("PMcpttGroup", "[%s] floor SRTCP reject from %s:%d (len=%d total=%ld)",
                     _groupId.c_str(), ip.c_str(), port, len, total);
            return;
        }
        buf = plain;
        len = plainLen;
    }

    ParsedFloor msg;
    if (!ParseFloorMessage(buf, len, msg)) return;

    // floor 제어 없는 그룹(floor_control:"off", private full-duplex)은 floor 를 중재하지
    //   않는다 — 수신 floor 메시지는 무시한다 (포트도 광고하지 않음).
    if (!_floorControl) {
        LOG_DEBUG("PMcpttGroup", "[%s] Floor msg ignored (floor_control=off) from %s:%d",
                  _groupId.c_str(), ip.c_str(), port);
        return;
    }

    // 멤버의 floorPort로 매칭
    std::string sessionId = "";
    unsigned int senderSsrc = 0;
    {
        PAutoLock lock(_mutex);
        for (auto const& [sid, peer] : _members) {
            if (peer.ip == ip && peer.floorPort == port) {
                sessionId = sid;
                senderSsrc = peer.ssrc;
                break;
            }
        }
        // (구) symmetric floor — "소스 포트가 선언 floorPort 와 일치하면 IP 학습" 분기는
        //   제거됨: 포트 번호만으로 IP 를 갈아치우는 건 우연/제3자 소스에 의한 하향 탈취
        //   소지가 있고, NAT 케이스는 아래 User ID 식별 latch 가 이미 커버한다.
        // NAT(포트변환) 단말: 주소 매칭 실패 → TS 24.380 User ID 필드(§8.2.3.6)로
        // 멤버를 식별하고 관측 소스 주소를 latch (symmetric floor). 이후 GRANT/TAKEN/IDLE 도달 가능.
        if (sessionId.empty()) {
            std::string uid = msg.userId();
            if (!uid.empty()) {
                size_t colon = uid.find(':');                 // "tel:+82..@dom" → "+82.."
                if (colon != std::string::npos) uid = uid.substr(colon + 1);
                size_t at = uid.find('@');
                if (at != std::string::npos) uid = uid.substr(0, at);
                auto it = _members.find(uid);
                if (it != _members.end()) {
                    // latch 자격은 제어평면이 nat 지정한 멤버만 (ue_nat_traversal.md §4) —
                    //   no-NAT 멤버는 주소 매칭 실패 = 미협상 소스.
                    if (!it->second.natEnabled) {
                        _dropSrc("floor(no-nat user-id)", uid, ip, port);
                    } else if (!it->second.sigIp.empty() && it->second.sigIp != ip) {
                        // IP guard — User ID 를 아는 제3자가 임의 주소로 그 멤버의 floor
                        //   하향을 가로채는 것을 차단 (RTP _acceptNatRtp 와 대칭).
                        _dropSrc("floor(ip-guard)", uid, ip, port);
                    } else {
                        LOG_INFO("PMcpttGroup", "[%s] Floor addr latched (NAT) %s: %s:%d -> %s:%d",
                                 _groupId.c_str(), uid.c_str(),
                                 it->second.ip.c_str(), it->second.floorPort, ip.c_str(), port);
                        it->second.ip = ip;
                        it->second.floorPort = port;
                        it->second.floorNatLatched = true;
                        sessionId = uid;
                        senderSsrc = it->second.ssrc;
                    }
                }
            }
        }
    }

    if (sessionId.empty()) {
        LOG_INFO("PMcpttGroup", "[%s] Floor from unknown %s:%d", _groupId.c_str(), ip.c_str(), port);
        return;
    }

    LOG_INFO("PMcpttGroup", "[%s] Floor %s from session=%s %s:%d (prio=%d ind=0x%x)",
             _groupId.c_str(), _floorOpName(msg.subtype), sessionId.c_str(), ip.c_str(), port,
             msg.priority(), msg.indicator() < 0 ? 0 : msg.indicator());

    // 수신 Floor 메시지 Flow 기록 (UE → CMP) — REQUEST/RELEASE 만 (QUEUE_POS/ACK 노이즈 제외)
    if (_logFlow && (msg.subtype == FLOOR_REQUEST || msg.subtype == FLOOR_RELEASE ||
                     msg.subtype == FLOOR_RELEASE_MULTI)) {
        const char* opName = _floorOpName(msg.subtype);
        int prio = 0;
        {
            PAutoLock lock(_mutex);
            auto itP = _priorities.find(sessionId);
            if (itP != _priorities.end()) prio = itP->second;
        }
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"%s\",\"user\":\"%s\",\"ssrc\":%u,\"prio\":%d}",
                 opName, sessionId.c_str(), senderSsrc, prio);
        std::string label = std::string("FLOOR_") + opName;
        _logFlow(_groupId, "ue", "cmp", "MCPTT", label.c_str(), detail);
    }

    switch (msg.subtype) {
        case FLOOR_REQUEST:        handleFloorRequest(sessionId, senderSsrc, msg.indicator()); break;
        // Floor Release / Floor Release Multi Talker(Rel-16) — 둘 다 "이 화자의 발언 종료".
        //   집합에서 그 화자만 걷어내므로 잔여 화자 유무는 _advanceFloorOrIdle 이 판정한다.
        case FLOOR_RELEASE:
        case FLOOR_RELEASE_MULTI:  handleFloorRelease(sessionId, senderSsrc); break;
        case FLOOR_QUEUE_POS_REQ: {
            PAutoLock lock(_mutex);
            _sendQueuePos(sessionId, senderSsrc);
            break;
        }
        case FLOOR_ACK:
            // 신뢰성 Ack — 재전송을 하지 않으므로 수신 확인만(no-op).
            LOG_DEBUG("PMcpttGroup", "[%s] Floor ACK from %s", _groupId.c_str(), sessionId.c_str());
            break;
        default:
            break;
    }
}

// 멤버 전용 포트 유닛(PPttMemberPort) 수신 — 수신 소켓이 곧 멤버 신원.
// 소스 주소는 신원 판정이 아니라 검증용이다 (선언 주소 불일치 = 드롭 + 카운터).
void PMcpttGroup::onMemberRtpPacket(const std::string& memberId, const std::string& ip, int port, char* buf, int len) {
    std::string action = "NONE";
    std::string actionSenderId = "";
    unsigned int actionSsrc = 0;

    {
        PAutoLock lock(_mutex);

        auto itM = _members.find(memberId);
        if (itM == _members.end()) {
            // JOIN(주소 전달) 전에 UE 가 먼저 송신 시작한 경우 — 정상 과도 상태
            _dropSrc("rtp(pre-join)", memberId, ip, port);
            return;
        }
        Peer& sender = itM->second;
        // 선언(SDP) 주소 일치는 latch 상태와 무관하게 항상 수락 — 미디어별 NAT 경로가
        //   갈리는 멤버에서 다른 미디어의 latch 가 ip 를 덮어써도 협상된 신원은 유지.
        if ((sender.ip != ip || sender.port != port) &&
            !(sender.declIp == ip && sender.declPort == port)) {
            if (!sender.natEnabled || !_acceptNatRtp(sender, false, ip, port, buf, len)) {
                _dropSrc("rtp", memberId, ip, port);
                return;
            }
        }
        const std::string& senderId = memberId;
        unsigned int senderSsrc = sender.ssrc;

        // 그룹 세션 활성 갱신 (그룹 timeout sweep 기준)
        if (_pttSession) _pttSession->touchActivity();

        unsigned char pt = (unsigned char)(buf[1] & 0x7F);
        LOG_DEBUG("PMcpttGroup", "[%s] RTP ip=%s port=%d len=%d pt=%d sender=%s", _groupId.c_str(), ip.c_str(), port, len, pt, senderId.c_str());

        {
            // [DTMF Check] — RFC 2833/4733 telephone-event.
            //   leg 별 TE PT(srcTePt) 선언 시 그 값으로 판독, 미선언은 관례 PT 101.
            unsigned char tePt = (sender.srcTePt > 0) ? (unsigned char)(sender.srcTePt & 0x7F) : 101;
            if (len > 12 && pt == tePt) {
                unsigned char digitCode = (unsigned char)buf[12];
                bool endBit = (buf[13] & 0x80) != 0;
                unsigned char volume = buf[13] & 0x3F;
                unsigned short duration = (((unsigned char)buf[14]) << 8) | ((unsigned char)buf[15]);

                char digitChar = 0;
                if (digitCode >= 0 && digitCode <= 9) digitChar = '0' + digitCode;
                else if (digitCode == 10) digitChar = '*';
                else if (digitCode == 11) digitChar = '#';
                else if (digitCode >= 12 && digitCode <= 15) digitChar = 'A' + (digitCode - 12);

                if (digitChar != 0 && endBit) {
                    // Flow 로깅: END bit 시점에만 기록 (중복 방지)
                    _dtmfFlowLog(senderId, digitChar, duration, volume);

                    // PTT DTMF push/release 판별 (설정된 경우)
                    if (_dtmfEnable) {
                        std::string dStr(1, digitChar);
                        if (dStr == _dtmfPushDigit) {
                            LOG_INFO("PMcpttGroup", "[%s] DTMF Push '%c' from %s", _groupId.c_str(), digitChar, senderId.c_str());
                            action = "REQUEST";
                            actionSenderId = senderId;
                            actionSsrc = senderSsrc;
                        } else if (dStr == _dtmfReleaseDigit) {
                            LOG_INFO("PMcpttGroup", "[%s] DTMF Release '%c' from %s", _groupId.c_str(), digitChar, senderId.c_str());
                            action = "RELEASE";
                            actionSenderId = senderId;
                            actionSsrc = senderSsrc;
                        }
                    }
                }
            }

            // 중계 자격: floor 有 = 현재 발언자만, floor off = 전원(full-duplex).
            //   ambient 청취 leg(recv_only)의 상향은 어느 경우에도 중계하지 않는다.
            Talker* tk = _floorControl ? _talkerOf(senderId) : nullptr;
            bool relay = sender.recvOnly ? false : (_floorControl ? (tk != nullptr) : true);
            if (relay) {
                int slot = _floorControl ? tk->slot : sender.streamSlot;
                if (tk) tk->lastRtpUsec = _nowUsec();
                sendAudioToAll(buf, len, senderId, slot);
                // 녹취: 화자 슬롯 트랙에 기록 (동시 발언이면 화자마다 별도 트랙)
                if (!_floorControl && (!_recorder || !_recorder->isActive())) {
                    // floor 없는 세션은 발언 경계가 없다 — 첫 미디어에서 세그먼트를 열고
                    //   그룹 해제까지 유지한다(VoIP relay 녹취와 동형).
                    _recEnsureTracks(_streamSlotNext > 0 ? _streamSlotNext : 1);
                    _recStartSegment(senderId, 0, false, "");
                }
                if (_recordEnable && _recorder && _recorder->isActive()) {
                    _recorder->writePacket(_slotTrack(slot, false), buf, len);
                }
            }
        }
    } // Lock releases here

    if (action == "REQUEST") {
        handleFloorRequest(actionSenderId, actionSsrc);
    } else if (action == "RELEASE") {
        handleFloorRelease(actionSenderId, actionSsrc);
    }
}

void PMcpttGroup::onMemberVideoRtpPacket(const std::string& memberId, const std::string& ip, int port, char* buf, int len) {
    PAutoLock lock(_mutex);

    auto itM = _members.find(memberId);
    if (itM == _members.end()) {
        _dropSrc("video rtp(pre-join)", memberId, ip, port);
        return;
    }
    Peer& sender = itM->second;
    if ((sender.ip != ip || sender.videoPort != port) &&
        !(sender.declIp == ip && sender.declVideoPort == port)) {
        if (!sender.natEnabled || !_acceptNatRtp(sender, true, ip, port, buf, len)) {
            _dropSrc("video rtp", memberId, ip, port);
            return;
        }
    }

    if (_pttSession) _pttSession->touchActivity();

    // 오디오와 같은 중계 자격 판정 (발언자만 / floor off 는 전원, recv_only 제외)
    Talker* tk = _floorControl ? _talkerOf(memberId) : nullptr;
    if (sender.recvOnly || (_floorControl && !tk)) return;

    int slot = _floorControl ? tk->slot : sender.streamSlot;
    if (tk) tk->lastRtpUsec = _nowUsec();
    sendVideoToAll(buf, len, memberId, slot);

    // 녹취: 화자 슬롯의 비디오 트랙에 기록
    if (_recordEnable && _recorder && _recorder->isActive()) {
        _recorder->writePacket(_slotTrack(slot, true), buf, len);
    }
}

// nat 멤버의 RTP 수신 판정 — 유닛 포트가 곧 멤버 신원이므로 latch 는 신원 판정이 아니라
//   송신 목적지 학습이다. 멤버 전용 포트가 곧 신원(수신 소켓=멤버, 포트는 그 멤버에게만
//   SDP 로 광고)이므로 형식 검사를 통과한 소스로 목적지를 **연속 추종**한다:
//   RTP v2 + 최소 길이 + (guard) 소스 IP == sigIp + (선언 시) 기대 ingress PT 일치.
//   SSRC 핀·스테일 창은 두지 않는다 — 핀이 잘못된 소스에 걸리면 정당한 단말이 영구
//   차단되는 고착이 더 해악이고, 추종 모델은 선점 소스 소멸 즉시 자가 복구된다.
//   호출자가 _mutex 보유.
bool PMcpttGroup::_acceptNatRtp(Peer& peer, bool isVideo, const std::string& ip, int port, const char* buf, int len) {
    if (len < 12 || (((unsigned char)buf[0]) >> 6) != 2) return false;
    if (!peer.sigIp.empty() && peer.sigIp != ip) return false;
    // 기대 ingress PT 검사 (JOIN user_src_pt 선언 시) — KA(empty RTP)도 협상 PT 를 실어
    //   보내므로 동일 기준으로 통과한다. TE 는 srcTePt(미선언=관례 101)도 허용.
    if (!isVideo && peer.srcPt > 0) {
        unsigned char pt = (unsigned char)(buf[1] & 0x7F);
        unsigned char te = (unsigned char)((peer.srcTePt > 0 ? peer.srcTePt : 101) & 0x7F);
        if (pt != (unsigned char)(peer.srcPt & 0x7F) && pt != te) return false;
    }

    bool& latched = isVideo ? peer.natLatchedVideo : peer.natLatched;
    bool changed = (peer.ip != ip) ||
                   (isVideo ? peer.videoPort != port : peer.port != port) || !latched;
    if (isVideo) peer.videoPort = port;
    else         peer.port = port;
    peer.ip = ip;
    latched = true;
    if (changed) {
        // 소스 경합(두 소스가 번갈아 유입) 시 로그 폭주 방지 — 멤버당 2s 간격 요약.
        int64_t now = _nowUsec();
        if (now - peer.followLogUsec >= 2000000LL) {
            peer.followLogUsec = now;
            LOG_INFO("PMcpttGroup", "[%s] %s dest follow (NAT) member=%s %s:%d",
                     _groupId.c_str(), isVideo ? "video RTP" : "RTP", peer.id.c_str(), ip.c_str(), port);
        }
    }
    return true;
}

void PMcpttGroup::collectNatLatched(std::vector<std::tuple<std::string, std::string, int>>& out) {
    PAutoLock lock(_mutex);
    for (auto const& [sid, peer] : _members) {
        if (peer.natEnabled && peer.natLatched)
            out.emplace_back(sid, peer.ip, peer.port);
    }
}

void PMcpttGroup::_dropSrc(const char* what, const std::string& memberId, const std::string& ip, int port) {
    ++_srcDrop;
    time_t now; time(&now);
    if (now - _lastDropWarn >= 5) {
        _lastDropWarn = now;
        LOG_WARN("PMcpttGroup", "[%s] drop %s member=%s src=%s:%d (total=%ld)",
                 _groupId.c_str(), what, memberId.c_str(), ip.c_str(), port, _srcDrop);
    }
}

void PMcpttGroup::handleFloorRequest(const std::string& sessionId, unsigned int ssrc, int indicatorBits) {
    PAutoLock lock(_mutex);
    if (!_floorControl) return;   // 중재 없는 그룹 — 요청 자체가 성립하지 않는다
    int requesterPrio = 0;
    if (_priorities.find(sessionId) != _priorities.end()) requesterPrio = _priorities[sessionId];

    // 수신 Floor Indicator(emergency/imminent) → tier 승격(단말 개시 긴급/임박).
    // CSP PTT_FLOOR_TIER 로 설정된 tier 와 max 결합(영속 → 무활동 자동회수 제외 등에 반영).
    if (indicatorBits > 0) {
        int indTier = (indicatorBits & FI_EMERGENCY)      ? TIER_EMERGENCY
                    : (indicatorBits & FI_IMMINENT_PERIL) ? TIER_IMMINENT
                    : TIER_NORMAL;
        if (indTier > tierOf(sessionId)) _tier[sessionId] = indTier;
    }

    // Ambient listening 청취 leg — 발언 불가(수신 전용). recv_only/floor_suppress 어느 쪽이든
    //   발언권을 주지 않는다 (cmp_media_api.md §7.3).
    {
        auto itReq = _members.find(sessionId);
        if (itReq != _members.end() && (itReq->second.recvOnly || itReq->second.floorSuppress)) {
            _sendDeny(sessionId, ssrc, CAUSE_DENY_RECEIVE_ONLY);
            LOG_INFO("PMcpttGroup", "[%s] Floor DENY (ambient recv-only) session=%s", _groupId.c_str(), sessionId.c_str());
            char ex[96];
            snprintf(ex, sizeof(ex), "\"reason\":\"recv_only\",\"cause\":%d", CAUSE_DENY_RECEIVE_ONLY);
            _logFloorLocal("DENY", sessionId, ssrc, requesterPrio, ex);
            return;
        }
    }

    // Broadcast 그룹 (TS 24.380 §10.3): 개시자(initiator)만 floor 보유.
    //   비개시자의 floor REQUEST 는 floor 점유 여부와 무관하게 항상 Deny(receive only).
    if (_groupType == "broadcast" && !_initiatorSessionId.empty() && sessionId != _initiatorSessionId) {
        _sendDeny(sessionId, ssrc, CAUSE_DENY_RECEIVE_ONLY);
        LOG_INFO("PMcpttGroup", "[%s] Floor DENY (broadcast) session=%s — initiator=%s only",
                 _groupId.c_str(), sessionId.c_str(), _initiatorSessionId.c_str());
        {
            char ex[160];
            snprintf(ex, sizeof(ex), "\"reason\":\"broadcast\",\"cause\":%d,\"initiator\":\"%s\"",
                     CAUSE_DENY_RECEIVE_ONLY, _initiatorSessionId.c_str());
            _logFloorLocal("DENY", sessionId, ssrc, requesterPrio, ex);
        }
        return;
    }

    if (_talkers.empty()) {
        _grantFloorTo(sessionId, ssrc, requesterPrio, false, "");
        return;
    }

    if (_isTalker(sessionId)) return;   // 이미 발언 중

    // 선점 비교 대상 = 현재 발언자 중 서열이 가장 낮은 화자 (동시 발언이면 그 중 최약자).
    Talker* weakest = _weakestTalker();
    const std::string ownerId = weakest->sessionId;
    const unsigned int ownerSsrc = weakest->ssrc;
    int ownerPrio = weakest->prio;
    int reqTier = tierOf(sessionId);
    int ownTier = tierOf(ownerId);
    bool bPreempt = _preempts(sessionId, requesterPrio, ownerId, ownerPrio);
    bool hasRoom = (int)_talkers.size() < _talkerCapacity;

    // 동시 발언 정원이 남은 경우.
    //   multi-talker: 서열 비교 없이 즉시 동시 GRANT (동시 최대 max_talkers 명).
    //   dual floor  : 2번째 자리는 override 전용 — 선점 자격이 있는 요청만 REVOKE 없이
    //                 동시 GRANT 한다(TS 24.380 dual floor). 자격 없으면 아래 큐/Deny 경로.
    if (hasRoom && _floorPolicy == FLOOR_POLICY_MULTI) {
        _grantFloorTo(sessionId, ssrc, requesterPrio, false, "");
        return;
    }
    if (hasRoom && _floorPolicy == FLOOR_POLICY_DUAL && bPreempt) {
        LOG_INFO("PMcpttGroup", "[%s] Floor DUAL grant to %s (prio=%d tier=%s) — %s keeps floor",
                 _groupId.c_str(), sessionId.c_str(), requesterPrio, _tierName(reqTier), ownerId.c_str());
        _grantFloorTo(sessionId, ssrc, requesterPrio, false, "");
        return;
    }

    if (bPreempt) {
        // PREEMPTION — 최약 화자 REVOKE(cause=preempted) 후 신규 화자 grant.
        LOG_INFO("PMcpttGroup", "[%s] Floor PREEMPTED by %s (prio=%d chair=%d) from %s (prio=%d chair=%d)",
               _groupId.c_str(), sessionId.c_str(), requesterPrio, isChair(sessionId),
               ownerId.c_str(), ownerPrio, isChair(ownerId));

        // Revoke Current (Reject Cause 필드 = Media Burst pre-empted)
        {
            char revBuf[256];
            std::vector<FloorTlv> f{ FloorTlv(FF_REJECT_CAUSE, FloorU16(CAUSE_REVOKE_PREEMPTED)),
                                     FloorTlv(FF_GRANTED_PARTY, ownerId) };
            int revLen = BuildFloorMessage(revBuf, sizeof(revBuf), FLOOR_REVOKE, ownerSsrc, f);
            if (revLen > 0) sendToMember(ownerId, revBuf, revLen);
        }
        {
            char ex[160];
            snprintf(ex, sizeof(ex), "\"preempted_by\":\"%s\",\"cause\":%d", sessionId.c_str(), CAUSE_REVOKE_PREEMPTED);
            _logFloorLocal("REVOKE", ownerId, ownerSsrc, ownerPrio, ex);
        }

        // 선점당한 화자 제거 — 마지막 화자였으면 _dropTalker 가 녹취 세그먼트를 종료한다
        //   (동시 발언 중이면 나머지 화자의 세그먼트는 이어진다).
        _dropTalker(ownerId);
        _grantFloorTo(sessionId, ssrc, requesterPrio, true, ownerId);
        return;
    }

    // 비선점 — SDP `mc_queueing` 광고 시 큐잉(TS 24.380 §8). 큐가 가득 차면 Deny.
    //   private call(§7)은 큐잉 절차가 없어 _queueEnable=false → 곧바로 Deny.
    if (_queueEnable && (int)_floorQueue.size() < _queueMax) {
        // 중복 제거 후 큐 추가
        for (auto it = _floorQueue.begin(); it != _floorQueue.end(); )
            it = (it->sessionId == sessionId) ? _floorQueue.erase(it) : it + 1;
        QueuedReq q{ sessionId, ssrc, requesterPrio, reqTier, isChair(sessionId), _nowUsec() };
        _floorQueue.push_back(q);
        LOG_INFO("PMcpttGroup", "[%s] Floor QUEUED session=%s pos=%d/%zu (talkers=%zu)",
                 _groupId.c_str(), sessionId.c_str(), _queuePositionOf(sessionId), _floorQueue.size(),
                 _talkers.size());
        _sendQueuePos(sessionId, ssrc);
        {
            char ex[160];
            snprintf(ex, sizeof(ex), "\"owner\":\"%s\",\"pos\":%d,\"qsize\":%zu",
                     ownerId.c_str(), _queuePositionOf(sessionId), _floorQueue.size());
            _logFloorLocal("QUEUE", sessionId, ssrc, requesterPrio, ex);
        }
        return;
    }

    // 큐 비활성/포화 → Deny.
    _sendDeny(sessionId, ssrc, _queueEnable ? CAUSE_DENY_QUEUE_FULL : CAUSE_DENY_ANOTHER_CLIENT);
    LOG_INFO("PMcpttGroup", "[%s] Floor DENY session=%s (prio=%d). Owner=%s (prio=%d)",
           _groupId.c_str(), sessionId.c_str(), requesterPrio, ownerId.c_str(), ownerPrio);
    {
        char ex[224];
        snprintf(ex, sizeof(ex), "\"owner\":\"%s\",\"owner_prio\":%d,\"tier\":\"%s\",\"owner_tier\":\"%s\"",
                 ownerId.c_str(), ownerPrio, _tierName(reqTier), _tierName(ownTier));
        _logFloorLocal("DENY", sessionId, ssrc, requesterPrio, ex);
    }
}

// 현재 발언자 중 서열 최약자 — 선점·정원 축소 시 회수 대상.
PMcpttGroup::Talker* PMcpttGroup::_weakestTalker() {
    Talker* weakest = nullptr;
    for (auto& t : _talkers) {
        if (!weakest) { weakest = &t; continue; }
        // 최약자 = 상대를 선점하지 못하는 쪽. 서열이 같으면 **나중에 grant 된 화자**를
        //   최약자로 본다 — 정원이 줄 때 먼저 말하던 화자의 발언이 끊기지 않도록.
        if (_preempts(weakest->sessionId, weakest->prio, t.sessionId, t.prio)) weakest = &t;
        else if (!_preempts(t.sessionId, t.prio, weakest->sessionId, weakest->prio) &&
                 t.grantUsec > weakest->grantUsec) weakest = &t;
    }
    return weakest;
}

// TS 24.380 선점 서열: condition tier(emergency>imminent>normal) > chair > 수치 priority.
//   1) emergency/imminent 발언자는 하위 tier 점유자를 선점(반대는 불가).
//   2) 동tier 면 chair override(chair 가 participant 선점, 역은 불가). private call 은
//      chair 개념이 없어 이 단계를 건너뛴다(TS 24.380 §7 — 2인 대등).
//   3) 동tier·동role 이면 수치 priority(0~255, 클수록 우선, 미지정=0 최저).
bool PMcpttGroup::_preempts(const std::string& reqId, int reqPrio,
                            const std::string& ownerId, int ownerPrio) const {
    int reqTier = tierOf(reqId);
    int ownTier = tierOf(ownerId);
    if (reqTier != ownTier) return reqTier > ownTier;
    if (!_privateCall) {
        bool reqChair = isChair(reqId);
        bool ownChair = isChair(ownerId);
        if (reqChair != ownChair) return reqChair;
    }
    return reqPrio > ownerPrio;
}

// ── Floor 송신 헬퍼 (caller 가 _mutex 보유) ─────────────────────────────────

int PMcpttGroup::_indicatorFor(const std::string& sessionId) const {
    int t = tierOf(sessionId);
    int bits = (t >= TIER_EMERGENCY) ? FI_EMERGENCY
             : (t == TIER_IMMINENT)  ? FI_IMMINENT_PERIL
                                     : FI_NORMAL;
    // 동시 발언 표식 (TS 24.380 §8.2.3.13) — 단말이 여러 화자를 동시에 재생해야 함을 알린다.
    if (_floorPolicy == FLOOR_POLICY_MULTI) bits |= FI_MULTI_TALKER;
    else if (_floorPolicy == FLOOR_POLICY_DUAL && _talkers.size() > 1) bits |= FI_DUAL_FLOOR;
    return bits;
}

// 동시 발언 슬롯 배정 — 슬롯은 수신자별 하향 스트림(SSRC/seq)과 녹취 트랙을 가른다.
//   한 세그먼트 안에서 슬롯을 재사용하면 트랙 파일에 두 화자가 섞이므로, 재사용이
//   필요하면 녹취 세그먼트를 먼저 끊고 잔여 화자로 새 세그먼트를 연다.
int PMcpttGroup::_allocSlot() {
    int cap = _talkerCapacity > 0 ? _talkerCapacity : 1;
    if (cap > MCPTT_MAX_TALKER_SLOTS) cap = MCPTT_MAX_TALKER_SLOTS;
    unsigned int occupied = 0;
    for (const auto& t : _talkers) occupied |= (1u << t.slot);

    for (int s = 0; s < cap; ++s)
        if (!(occupied & (1u << s)) && !(_slotUsedMask & (1u << s))) { _slotUsedMask |= (1u << s); return s; }

    for (int s = 0; s < cap; ++s) {
        if (occupied & (1u << s)) continue;
        if (_recordEnable && _recorder && _recorder->isActive()) {
            _recorder->finishSegment();
            _slotUsedMask = 0;
            if (!_talkers.empty()) {
                // 계속 발언 중인 화자들은 새 세그먼트에서 같은 슬롯 트랙으로 이어 쓴다.
                _recStartSegment(_talkers.front().sessionId, _talkers.front().prio, false, "");
                for (const auto& t : _talkers) _slotUsedMask |= (1u << t.slot);
            }
        } else {
            _slotUsedMask = 0;
        }
        _slotUsedMask |= (1u << s);
        return s;
    }
    return -1;   // 정원 초과 (호출부가 정원을 먼저 검사하므로 도달하지 않는다)
}

// 발언자 제거 (RELEASE/REVOKE/이탈 공통). 마지막 화자면 녹취 세그먼트를 종료한다.
bool PMcpttGroup::_dropTalker(const std::string& sessionId) {
    for (auto it = _talkers.begin(); it != _talkers.end(); ++it) {
        if (it->sessionId != sessionId) continue;
        _talkers.erase(it);
        if (_talkers.empty()) {
            if (_recordEnable && _recorder && _recorder->isActive()) _recorder->finishSegment();
            _slotUsedMask = 0;
        }
        return true;
    }
    return false;
}

void PMcpttGroup::_notifyTalkers() {
    if (!_onTalkers) return;
    std::vector<std::string> ids;
    for (const auto& t : _talkers) ids.push_back(t.sessionId);
    _onTalkers(_groupId, _policyName(), ids, _sesid, _service);
}

bool PMcpttGroup::setFloorCrypto(const std::string& alg, const std::string& key, const std::string& salt,
                                 const std::string& mki, std::string& err) {
    // 미디어 RTP 는 투명 relay 로 남는다 — 보호 대상은 floor RTCP 뿐이다(TS 33.180).
    bool ok = _floorCrypto.init(alg, key, salt, mki, err);
    if (ok)
        LOG_INFO("PMcpttGroup", "[%s] floor crypto enabled: alg=%s mki=%s",
                 _groupId.c_str(), _floorCrypto.alg().c_str(), mki.empty() ? "-" : "yes");
    else
        LOG_WARN("PMcpttGroup", "[%s] floor crypto rejected: %s", _groupId.c_str(), err.c_str());
    return ok;
}

void PMcpttGroup::setSessionMeta(const std::string& sesid, const std::string& service) {
    PAutoLock lock(_mutex);
    if (!sesid.empty()) _sesid = sesid;
    if (!service.empty()) _service = service;
}

void PMcpttGroup::_grantFloorTo(const std::string& sessionId, unsigned int ssrc, int prio,
                                bool preempt, const std::string& prevOwner) {
    int slot = _allocSlot();
    if (slot < 0) {
        LOG_WARN("PMcpttGroup", "[%s] grant aborted — no talker slot (capacity=%d)", _groupId.c_str(), _talkerCapacity);
        return;
    }
    Talker tk{ sessionId, ssrc, prio, slot, _nowUsec(), 0 };
    _talkers.push_back(tk);

    // Floor Granted → 요청자(Duration + Granted Party + Floor Indicator).
    {
        char buf[256];
        std::vector<FloorTlv> f{ FloorTlv(FF_DURATION, FloorU16(_grantDurationSec)),
                                 FloorTlv(FF_GRANTED_PARTY, sessionId),
                                 FloorTlv(FF_FLOOR_INDICATOR, FloorU16(_indicatorFor(sessionId))) };
        int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_GRANT, ssrc, f);
        if (n > 0) sendToMember(sessionId, buf, n);
    }

    // Floor Taken → 전체(화자 identity + Indicator).
    broadcastFloorStatus(FLOOR_TAKEN, ssrc, sessionId);

    LOG_INFO("PMcpttGroup", "[%s] Floor GRANTED to session=%s ssrc=%u prio=%d preempt=%d slot=%d talkers=%zu",
             _groupId.c_str(), sessionId.c_str(), ssrc, prio, preempt, slot, _talkers.size());
    if (_logFlow) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"GRANT\",\"user\":\"%s\",\"ssrc\":%u,\"prio\":%d,\"preempt\":%s,\"policy\":\"%s\",\"talkers\":%zu}",
                 sessionId.c_str(), ssrc, prio, preempt ? "true" : "false", _policyName(), _talkers.size());
        _logFlow(_groupId, "cmp", "ue", "MCPTT", "FLOOR_GRANT", detail);
    }
    {
        char ex[288];
        if (preempt)
            snprintf(ex, sizeof(ex), "\"preempt\":true,\"preempted_from\":\"%s\",\"tier\":\"%s\",\"slot\":%d,\"talkers\":%zu",
                     prevOwner.c_str(), _tierName(tierOf(sessionId)), slot, _talkers.size());
        else
            snprintf(ex, sizeof(ex), "\"slot\":%d,\"talkers\":%zu,\"policy\":\"%s\"",
                     slot, _talkers.size(), _policyName());
        _logFloorLocal("GRANT", sessionId, ssrc, prio, ex);
    }

    // 녹취: 첫 화자면 세그먼트 시작, 동시 발언 합류면 기존 세그먼트에 슬롯 트랙만 귀속.
    if (_recordEnable) {
        if (!_recorder) startRecording();
        if (_recorder && !_recorder->isActive()) {
            _recStartSegment(sessionId, prio, preempt, prevOwner);
        } else if (_recorder) {
            _recorder->setTrackSpeaker(_slotTrack(slot, false), sessionId);
            _recorder->setTrackSpeaker(_slotTrack(slot, true), sessionId);
        }
    }
    _notifyTalkers();
}

// 녹취 세그먼트 시작 — 대표 화자(speaker_id)의 PT/코덱 메타를 실어 연다.
//   동시 발언 슬롯 트랙의 화자는 speaker_id_{track} 으로 따로 남긴다.
void PMcpttGroup::_recStartSegment(const std::string& speakerId, int prio,
                                   bool preempt, const std::string& prevOwner) {
    if (!_recordEnable) return;
    if (!_recorder) startRecording();
    if (!_recorder) return;
    int spkPt = 0;
    std::string spkCodec;
    auto itSpk = _members.find(speakerId);
    if (itSpk != _members.end()) { spkPt = itSpk->second.srcPt; spkCodec = itSpk->second.codec; }
    _recorder->startPttSegment(speakerId, prio, preempt, prevOwner, spkPt, spkCodec);
    for (const auto& t : _talkers) {
        _recorder->setTrackSpeaker(_slotTrack(t.slot, false), t.sessionId);
        _recorder->setTrackSpeaker(_slotTrack(t.slot, true), t.sessionId);
    }
    if (!_floorControl) {
        // floor 없는 세션은 멤버 슬롯이 곧 화자 — 슬롯 트랙 귀속을 멤버로 채운다.
        for (const auto& [sid, peer] : _members) {
            if (peer.recvOnly) continue;
            _recorder->setTrackSpeaker(_slotTrack(peer.streamSlot, false), sid);
            _recorder->setTrackSpeaker(_slotTrack(peer.streamSlot, true), sid);
        }
    }
}

// 슬롯 트랙 등록 (0..slots-1). 트랙 파일은 세그먼트 시작 시 열리므로 세그먼트 전에 부른다.
void PMcpttGroup::_recEnsureTracks(int slots) {
    if (!_recordEnable || !_recorder) return;
    if (slots > MCPTT_MAX_TALKER_SLOTS) slots = MCPTT_MAX_TALKER_SLOTS;
    for (int s = _recTrackSlots; s < slots; ++s) {
        _recorder->addTrack(_slotTrack(s, false));
        _recorder->addTrack(_slotTrack(s, true));
    }
    if (slots > _recTrackSlots) _recTrackSlots = slots;
}

void PMcpttGroup::_sendDeny(const std::string& sessionId, unsigned int ssrc, int cause) {
    char buf[256];
    std::vector<FloorTlv> f{ FloorTlv(FF_REJECT_CAUSE, FloorU16(cause)) };
    int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_REJECT, ssrc, f);
    if (n > 0) sendToMember(sessionId, buf, n);
    if (_logFlow) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"DENY\",\"user\":\"%s\",\"ssrc\":%u,\"cause\":%d}",
                 sessionId.c_str(), ssrc, cause);
        _logFlow(_groupId, "cmp", "ue", "MCPTT", "FLOOR_DENY", detail);
    }
}

int PMcpttGroup::_queuePositionOf(const std::string& sessionId) const {
    // 정렬 기준(우선순위 높을수록 앞): tier desc, chair, prio desc, ts asc.
    auto better = [](const QueuedReq& a, const QueuedReq& b) {
        if (a.tier != b.tier) return a.tier > b.tier;
        if (a.chair != b.chair) return a.chair;
        if (a.prio != b.prio) return a.prio > b.prio;
        return a.ts < b.ts;
    };
    int pos = 1;
    const QueuedReq* self = nullptr;
    for (const auto& q : _floorQueue) if (q.sessionId == sessionId) { self = &q; break; }
    if (!self) return 0;
    for (const auto& q : _floorQueue)
        if (q.sessionId != sessionId && better(q, *self)) pos++;
    return pos;
}

void PMcpttGroup::_sendQueuePos(const std::string& sessionId, unsigned int ssrc) {
    int pos = _queuePositionOf(sessionId);
    int prio = 0;
    if (_priorities.find(sessionId) != _priorities.end()) prio = _priorities[sessionId];
    char buf[256];
    std::vector<FloorTlv> f{ FloorTlv(FF_QUEUE_INFO, FloorQueueInfo(pos, prio)),
                             FloorTlv(FF_QUEUE_SIZE, FloorU16((int)_floorQueue.size())) };
    int n = BuildFloorMessage(buf, sizeof(buf), FLOOR_QUEUE_POS_INFO, ssrc, f);
    if (n > 0) sendToMember(sessionId, buf, n);
    if (_logFlow) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"QUEUE_POS_INFO\",\"user\":\"%s\",\"pos\":%d,\"qsize\":%zu}",
                 sessionId.c_str(), pos, _floorQueue.size());
        _logFlow(_groupId, "cmp", "ue", "MCPTT", "FLOOR_QUEUE_POS_INFO", detail);
    }
}

std::string PMcpttGroup::_popBestQueued(unsigned int& outSsrc, int& outPrio) {
    if (_floorQueue.empty()) return "";
    auto better = [](const QueuedReq& a, const QueuedReq& b) {
        if (a.tier != b.tier) return a.tier > b.tier;
        if (a.chair != b.chair) return a.chair;
        if (a.prio != b.prio) return a.prio > b.prio;
        return a.ts < b.ts;
    };
    size_t best = 0;
    for (size_t i = 1; i < _floorQueue.size(); ++i)
        if (better(_floorQueue[i], _floorQueue[best])) best = i;
    QueuedReq q = _floorQueue[best];
    _floorQueue.erase(_floorQueue.begin() + best);
    outSsrc = q.ssrc;
    outPrio = q.prio;
    return q.sessionId;
}

void PMcpttGroup::_advanceFloorOrIdle() {
    // 해제된 화자를 걷어낸 상태에서 호출. 여유 정원만큼 대기자를 승급시키고,
    //   화자가 남으면 잔여 화자 TAKEN 갱신, 아무도 없으면 IDLE 브로드캐스트.
    // dual 의 2번째 자리는 **override 전용**이라 큐 승급으로는 채우지 않는다 —
    //   대기자는 발언자가 모두 빠졌을 때만 올라간다(§7.7). multi 는 정원까지 승급.
    int promoteCap = (_floorPolicy == FLOOR_POLICY_DUAL) ? 1 : _talkerCapacity;
    while ((int)_talkers.size() < promoteCap && !_floorQueue.empty()) {
        unsigned int qssrc = 0; int qprio = 0;
        std::string next = _popBestQueued(qssrc, qprio);
        if (next.empty()) break;
        if (_members.find(next) == _members.end()) continue;   // 이미 떠난 멤버 skip
        _grantFloorTo(next, qssrc, qprio, false, "");
    }
    if (_talkers.empty()) {
        broadcastFloorStatus(FLOOR_IDLE, 0, "");
        return;
    }
    // 동시 발언 중 일부만 빠진 경우 — Floor Idle 은 성립하지 않으므로 잔여 화자를
    //   Taken 으로 다시 알려 단말의 화자 목록을 정합화한다(TS 24.380 Rel-16 multi-talker).
    for (const auto& t : _talkers)
        broadcastFloorStatus(FLOOR_TAKEN, t.ssrc, t.sessionId);
}

void PMcpttGroup::handleFloorRelease(const std::string& sessionId, unsigned int ssrc) {
    PAutoLock lock(_mutex);
    if (!_floorControl) return;
    if (!_dropTalker(sessionId)) return;   // 발언 중이 아니면 무시(멱등)

    int ownerPrio = 0;
    if (_priorities.find(sessionId) != _priorities.end()) ownerPrio = _priorities[sessionId];
    {
        char ex[96];
        snprintf(ex, sizeof(ex), "\"talkers\":%zu", _talkers.size());
        _logFloorLocal("RELEASE", sessionId, ssrc, ownerPrio, ex);
    }
    LOG_INFO("PMcpttGroup", "[%s] Floor RELEASED by session=%s (remaining talkers=%zu)",
             _groupId.c_str(), sessionId.c_str(), _talkers.size());

    // 대기열 승급 + 잔여 화자 갱신/IDLE.
    _advanceFloorOrIdle();
    _notifyTalkers();
    // RX FLOOR_RELEASE 는 onFloorPacket 에서 이미 기록됨 (중복 방지)
}

int64_t PMcpttGroup::_nowUsec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

bool PMcpttGroup::checkFloorInactivity(int idleSec) {
    if (idleSec <= 0) return false;
    PAutoLock lock(_mutex);
    if (!_floorControl || _talkers.empty()) return false;

    int64_t now = _nowUsec();
    bool reclaimed = false;

    // 동시 발언이면 화자마다 독립 판정 — 조용해진 화자만 회수한다.
    //   목록 복사 후 순회: _dropTalker 가 _talkers 를 변경한다.
    std::vector<Talker> snapshot = _talkers;
    for (const auto& t : snapshot) {
        // 긴급(emergency) 발언자는 무활동 자동 회수 제외 — 권한자 RELEASE/취소로만 해제(TS 24.380).
        if (tierOf(t.sessionId) >= TIER_EMERGENCY) continue;
        // 판정 기준: 마지막 RTP 수신 시각(있으면) 또는 grant 시각(RTP 한 번도 안 온 경우).
        int64_t ref = (t.lastRtpUsec > 0) ? t.lastRtpUsec : t.grantUsec;
        if (ref == 0) continue;
        if ((now - ref) < (int64_t)idleSec * 1000000LL) continue;

        const std::string owner = t.sessionId;
        unsigned int ssrc = t.ssrc;
        int ownerPrio = 0;
        if (_priorities.find(owner) != _priorities.end()) ownerPrio = _priorities[owner];
        int idleMs = (int)((now - ref) / 1000);

        // 응답 없는 화자에게 REVOKE 송출 (Reject Cause 필드 = Other reason).
        {
            char revBuf[256];
            std::vector<FloorTlv> f{ FloorTlv(FF_REJECT_CAUSE, FloorU16(CAUSE_OTHER)),
                                     FloorTlv(FF_GRANTED_PARTY, owner) };
            int revLen = BuildFloorMessage(revBuf, sizeof(revBuf), FLOOR_REVOKE, ssrc, f);
            if (revLen > 0) sendToMember(owner, revBuf, revLen);
        }
        {
            char ex[96];
            snprintf(ex, sizeof(ex), "\"reason\":\"inactivity\",\"cause\":%d,\"idle_ms\":%d", CAUSE_OTHER, idleMs);
            _logFloorLocal("REVOKE", owner, ssrc, ownerPrio, ex);
        }

        // 녹취 세그먼트 종료는 마지막 화자 제거 시 _dropTalker 가 수행
        //   (발언시간은 last-RTP 기준으로 이미 한정됨).
        _dropTalker(owner);
        reclaimed = true;

        LOG_INFO("PMcpttGroup", "[%s] Floor auto-REVOKED (inactivity %dms, no RELEASE) owner=%s",
                 _groupId.c_str(), idleMs, owner.c_str());
        if (_logFlow) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "{\"op\":\"REVOKE\",\"reason\":\"inactivity\",\"user\":\"%s\",\"ssrc\":%u,\"idle_ms\":%d}",
                     owner.c_str(), ssrc, idleMs);
            _logFlow(_groupId, "cmp", "ue", "MCPTT", "FLOOR_REVOKE", detail);
        }
    }

    if (!reclaimed) return false;
    // 대기열 승급 + 잔여 화자 갱신/IDLE.
    _advanceFloorOrIdle();
    _notifyTalkers();
    return true;
}

void PMcpttGroup::broadcastFloorStatus(unsigned char opcode, unsigned int ssrc, const std::string& speakerId) {
    const char* opName = _floorOpName(opcode);
    LOG_INFO("PMcpttGroup", "[%s] broadcastFloorStatus subtype=%d(%s) speaker=%s ssrc=%u → %lu members",
             _groupId.c_str(), opcode, opName, speakerId.c_str(), ssrc, _members.size());

    // 메시지별 TLV 필드 (TS 24.380): TAKEN=Granted Party+Indicator, IDLE=필드 없음.
    std::vector<FloorTlv> fields;
    if (opcode == FLOOR_TAKEN && !speakerId.empty()) {
        fields.push_back(FloorTlv(FF_GRANTED_PARTY, speakerId));
        fields.push_back(FloorTlv(FF_FLOOR_INDICATOR, FloorU16(_indicatorFor(speakerId))));
    } else if (opcode != FLOOR_IDLE && !speakerId.empty()) {
        fields.push_back(FloorTlv(FF_GRANTED_PARTY, speakerId));
    }

    char pktBuf[256];
    int pktLen = BuildFloorMessage(pktBuf, sizeof(pktBuf), opcode, ssrc, fields);
    if (pktLen > 0)
        sendFloorToAll(pktBuf, pktLen);

    // CMP → UE 전체 브로드캐스트 Flow 기록 (TAKEN/IDLE/REVOKE 등)
    if (_logFlow) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"%s\",\"speaker\":\"%s\",\"ssrc\":%u,\"members\":%zu}",
                 opName, speakerId.c_str(), ssrc, _members.size());
        std::string label = std::string("FLOOR_") + opName;
        _logFlow(_groupId, "cmp", "ue", "MCPTT", label.c_str(), detail);
    }
    // 세션 로컬 floor.jsonl 에는 IDLE 만 추가 기록 (GRANT/REVOKE/TAKEN 은 호출부에서 기록)
    if (opcode == FLOOR_IDLE)
        _logFloorLocal("IDLE", speakerId, ssrc, -1);
}

// 수신자별 하향 스트림 식별 — 슬롯 0 은 종전과 같은 고정 SSRC(단일 화자 정책에서 화자가
//   바뀌어도 하나의 연속 스트림), 슬롯 1..N 은 동시 발언용 별도 SSRC 공간이다.
//   audio/video/멤버 SSRC 공간(0x1/0x2)과 겹치지 않도록 상위 비트로 분리한다.
static inline uint32_t _egressSsrc(unsigned int memberSsrc, int slot, bool video) {
    if (slot <= 0) return (video ? 0x20000000u : 0x10000000u) + memberSsrc;
    return (video ? 0x50000000u : 0x40000000u) + ((uint32_t)slot << 24) + memberSsrc;
}

// 하향 분배는 각 멤버의 전용 유닛 소켓에서 송신한다 — 멤버가 보는 소스 포트 =
// SDP 에 광고한 포트 (symmetric RTP 정합).
void PMcpttGroup::sendAudioToAll(const char* data, int len, const std::string& excludeSessionId, int slot) {
    if (len < 12) return;
    // leg 별 PT 재작성 분류 — 화자(sender) leg 의 srcTePt 로 이 패킷이 audio/TE 인지 판정.
    //   srcTePt 미지정(0) 시 관례 PT 101 을 TE 로 간주(기존 DTMF 판독과 동일 기준).
    bool isTe;
    {
        unsigned char inPt = (unsigned char)(data[1] & 0x7F);
        int srcTePt = 0;
        auto itSrc = _members.find(excludeSessionId);
        if (itSrc != _members.end()) srcTePt = itSrc->second.srcTePt;
        isTe = (srcTePt > 0) ? (inPt == (unsigned char)(srcTePt & 0x7F)) : (inPt == 101);
    }
    if (slot < 0 || slot >= MCPTT_MAX_TALKER_SLOTS) slot = 0;
    for (auto& [sid, peer] : _members) {
        if (sid == excludeSessionId) continue;
        if (!peer.unit || peer.port <= 0) continue;
        // 수신자별 SSRC + 시퀀스 번호 재작성 (화자 슬롯별 스트림)
        char pkt[4096];
        if (len > (int)sizeof(pkt)) continue;
        memcpy(pkt, data, len);
        peer.audioSeqOut[slot]++;
        uint16_t netSeq = htons(peer.audioSeqOut[slot]);
        memcpy(pkt + 2, &netSeq, 2);
        uint32_t netSsrc = htonl(_egressSsrc(peer.ssrc, slot, false));
        memcpy(pkt + 8, &netSsrc, 4);
        // egress PT 스탬프 (0=재작성 없음 — 현행 PT-blind 통과). marker bit(0x80) 보존.
        //   TE 인데 수신 leg TE PT 미지정이면 원본 유지(오디오 PT 로 뭉개면 DTMF 파손).
        int stampPt = isTe ? peer.tePtOut : peer.ptOut;
        if (stampPt > 0)
            pkt[1] = (char)((pkt[1] & 0x80) | (stampPt & 0x7F));
        peer.unit->sendAudioTo(peer.ip, peer.port, pkt, len);
    }
}

void PMcpttGroup::sendFloorToAll(const char* data, int len) {
    if (!_pttSession) return;
    // SRTCP 보호는 브로드캐스트 1회만 적용한다 — 같은 SSRC·같은 패킷이므로
    //   수신자마다 index 를 소모하면 재전송 방지 창이 무의미해진다.
    char sec[2048];
    if (_floorCrypto.enabled()) {
        int secLen = 0;
        if (!_floorCrypto.protect(data, len, sec, sizeof(sec), secLen)) {
            LOG_ERROR("PMcpttGroup", "[%s] floor SRTCP protect failed (len=%d)", _groupId.c_str(), len);
            return;
        }
        data = sec;
        len = secLen;
    }
    for (auto const& [sid, peer] : _members) {
        if (peer.floorSuppress) continue;   // ambient 청취 leg — floor 상태 미노출
        if (peer.floorPort > 0)
            _pttSession->sendFloorTo(peer.ip, peer.floorPort, (char*)data, len);
    }
}

void PMcpttGroup::sendVideoToAll(const char* data, int len, const std::string& excludeSessionId, int slot) {
    if (len < 12) return;
    if (slot < 0 || slot >= MCPTT_MAX_TALKER_SLOTS) slot = 0;
    for (auto& [sid, peer] : _members) {
        if (sid == excludeSessionId) continue;
        if (!peer.unit || peer.videoPort <= 0) continue;
        char pkt[4096];
        if (len > (int)sizeof(pkt)) continue;
        memcpy(pkt, data, len);
        peer.videoSeqOut[slot]++;
        uint16_t netSeq = htons(peer.videoSeqOut[slot]);
        memcpy(pkt + 2, &netSeq, 2);
        uint32_t netSsrc = htonl(_egressSsrc(peer.ssrc, slot, true));
        memcpy(pkt + 8, &netSsrc, 4);
        peer.unit->sendVideoTo(peer.ip, peer.videoPort, pkt, len);
    }
}

void PMcpttGroup::sendToMember(const std::string& sessionId, const char* data, int len) {
    if (_members.find(sessionId) == _members.end()) {
        LOG_ERROR("PMcpttGroup", "[%s] sendToMember session=%s not found", _groupId.c_str(), sessionId.c_str());
        return;
    }
    const Peer& peer = _members[sessionId];
    if (peer.floorSuppress) return;   // ambient 청취 leg — floor 메시지 미송신
    // PPttTrans의 floor 소켓으로 전송 (규격: m=application 포트)
    if (_pttSession && peer.floorPort > 0) {
        char sec[2048];
        if (_floorCrypto.enabled()) {
            int secLen = 0;
            if (!_floorCrypto.protect(data, len, sec, sizeof(sec), secLen)) {
                LOG_ERROR("PMcpttGroup", "[%s] floor SRTCP protect failed (session=%s len=%d)",
                          _groupId.c_str(), sessionId.c_str(), len);
                return;
            }
            data = sec;
            len = secLen;
        }
        LOG_INFO("PMcpttGroup", "[%s] sendFloor session=%s → %s:%d",
                 _groupId.c_str(), sessionId.c_str(), peer.ip.c_str(), peer.floorPort);
        _pttSession->sendFloorTo(peer.ip, peer.floorPort, (char*)data, len);
    }
}

// ──────────────────────────────────────────────────────────────
//  녹취 — Floor 단위 세그먼트 (SegmentedRecorder)
// ──────────────────────────────────────────────────────────────

void PMcpttGroup::setRecording(bool enable, const std::string& dir) {
    _recordEnable = enable;
    _recordDir = dir;
    if (enable && !dir.empty()) {
        std::string mkdirCmd = "mkdir -p " + dir;
        system(mkdirCmd.c_str());
        startRecording();
    }
}

void PMcpttGroup::startRecording() {
    if (_recorder) return;
    if (_recordDir.empty()) return;

    _recorder = new PSyncRtpRecorder(_recordDir, "ptt");
    _recTrackSlots = 0;
    // 동시 발언 정원만큼 슬롯 트랙 등록 (슬롯 0 = "audio"/"video" — 종전 파일명 그대로).
    //   floor 없는 세션은 멤버 슬롯 수만큼 필요하므로 첫 미디어에서 추가 등록한다.
    _recEnsureTracks(_talkerCapacity > 0 ? _talkerCapacity : 1);

    LOG_INFO("PMcpttGroup", "[%s] Recording initialized: dir=%s slots=%d",
             _groupId.c_str(), _recordDir.c_str(), _recTrackSlots);
}

void PMcpttGroup::stopRecording() {
    // 포인터 swap 은 lock 하에 수행 — onRtpPacket/onVideoRtpPacket/handleFloor* 의
    // 동시 writePacket/finishSegment 호출과 경합하지 않도록 보호.
    // 실제 finishSegment + delete 는 lock 바깥에서 수행해 파일 I/O 가 RTP 경로를
    // 블로킹하지 않게 한다. finishSegment → _closeTrack 이 fclose + rename 을
    // 수행하므로 .recording 임시 파일이 최종 파일로 승격되어 녹취가 온전히 마감된다.
    PSyncRtpRecorder* oldRecorder = nullptr;
    {
        PAutoLock lock(_mutex);
        oldRecorder = _recorder;
        _recorder = NULL;
    }
    if (!oldRecorder) return;

    if (oldRecorder->isActive())
        oldRecorder->finishSegment();

    delete oldRecorder;
    _recTrackSlots = 0;

    LOG_INFO("PMcpttGroup", "[%s] Recording stopped: dir=%s",
             _groupId.c_str(), _recordDir.c_str());
}

void PMcpttGroup::_dtmfFlowLog(const std::string& senderId, char digit,
                                unsigned short duration, unsigned char volume) {
    if (!_logFlow) return;
    // detail JSON: {"digit":"X","duration_ms":N,"volume":V,"user":"..."}
    // duration 단위: 시간단위(8kHz 샘플). ms 변환 = duration / 8
    char detail[256];
    snprintf(detail, sizeof(detail),
             "{\"digit\":\"%c\",\"duration_ms\":%u,\"volume\":%u,\"user\":\"%s\"}",
             digit, (unsigned)(duration / 8), (unsigned)volume, senderId.c_str());
    _logFlow(_groupId, "ue", "cmp", "DTMF", "DTMF", detail);
}
