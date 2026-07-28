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
      _floorTaken(false), _floorOwnerSsrc(0),
      _recordEnable(false), _recorder(NULL)
{
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
                            int ptOut, int srcPt, int tePtOut, int srcTePt, const std::string& codec) {
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
    peer.audioSeqOut = 0;
    peer.videoSeqOut = 0;
    peer.audioSsrcOut = 0x10000000 + peer.ssrc;  // 수신자별 고정 audio SSRC
    peer.videoSsrcOut = 0x20000000 + peer.ssrc;  // 수신자별 고정 video SSRC
    peer.unit = unit;
    _members[sessionId] = peer;
    LOG_INFO("PMcpttGroup", "[%s] Member added session=%s (total=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());
    if (_logFlow) _logFlow(_groupId, "ue", "cmp", "MCPTT", "MEMBER_JOIN", sessionId.c_str());
    
    // If floor is taken, notify new member (Floor Taken = 화자 identity + Indicator).
    if (_floorTaken) {
         char pktBuf[256];
         std::vector<FloorTlv> f{ FloorTlv(FF_GRANTED_PARTY, _floorOwnerSessionId),
                                  FloorTlv(FF_FLOOR_INDICATOR, FloorU16(_indicatorFor(_floorOwnerSessionId))) };
         int pktLen = BuildFloorMessage(pktBuf, sizeof(pktBuf), FLOOR_TAKEN, _floorOwnerSsrc, f);
         if (pktLen > 0)
             sendToMember(sessionId, pktBuf, pktLen);
         LOG_DEBUG("PMcpttGroup", "[%s] Notified new member %s about floor taken by %s",
                   _groupId.c_str(), sessionId.c_str(), _floorOwnerSessionId.c_str());
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

    if ( _floorTaken && (_floorOwnerSessionId == sessionId) ) {

        // Owner left, release floor — 녹취 세그먼트 종료
        if (_recordEnable && _recorder && _recorder->isActive()) {
            _recorder->finishSegment();
        }

        _floorTaken = false;
        _floorOwnerSessionId = "";
        _floorOwnerSsrc = 0;
        LOG_INFO("PMcpttGroup", "[%s] Floor owner %s left.", _groupId.c_str(), sessionId.c_str());
        // 대기열 있으면 다음 화자에게 넘기고, 없으면 IDLE.
        _advanceFloorOrIdle();
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
        default:            return "?";
    }
}

void PMcpttGroup::onFloorPacket(const std::string& ip, int port, char* buf, int len) {
    ParsedFloor msg;
    if (!ParseFloorMessage(buf, len, msg)) return;

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
    if (_logFlow && (msg.subtype == FLOOR_REQUEST || msg.subtype == FLOOR_RELEASE)) {
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
        case FLOOR_RELEASE:        handleFloorRelease(sessionId, senderSsrc); break;
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

            if (_floorTaken && _floorOwnerSessionId == senderId) {
                _lastRtpUsec = _nowUsec();
                sendAudioToAll(buf, len, senderId);
                // 녹취: 세그먼트 파일에 기록 — 귀속은 floor 소유자(세그먼트) 기준
                if (_recordEnable && _recorder && _recorder->isActive()) {
                    _recorder->writePacket("audio", buf, len);
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

    if (!_floorTaken || _floorOwnerSessionId != memberId) return;

    _lastRtpUsec = _nowUsec();
    sendVideoToAll(buf, len, memberId);

    // 녹취: 비디오 세그먼트 파일에 기록
    if (_recordEnable && _recorder && _recorder->isActive()) {
        _recorder->writePacket("video", buf, len);
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

    if (!_floorTaken) {
        _grantFloorTo(sessionId, ssrc, requesterPrio, false, "");
        return;
    }

    if (_floorOwnerSessionId == sessionId) return;   // 이미 보유자

    int ownerPrio = 0;
    if (_priorities.find(_floorOwnerSessionId) != _priorities.end()) ownerPrio = _priorities[_floorOwnerSessionId];

    // TS 24.380 선점 서열: condition tier(emergency>imminent>normal) > chair > 수치 priority.
    //   1) emergency/imminent 발언자는 하위 tier 점유자를 선점(반대는 불가).
    //   2) 동tier 면 chair override(chair 가 participant 선점, 역은 불가).
    //   3) 동tier·동role 이면 수치 priority(TS 24.380: 0~255, 클수록 우선, 미지정=0 최저).
    int  reqTier   = tierOf(sessionId);
    int  ownTier   = tierOf(_floorOwnerSessionId);
    bool requesterChair = isChair(sessionId);
    bool ownerChair     = isChair(_floorOwnerSessionId);
    bool bPreempt;
    if (reqTier != ownTier)                 bPreempt = (reqTier > ownTier);
    else if (requesterChair && !ownerChair) bPreempt = true;
    else if (!requesterChair && ownerChair) bPreempt = false;
    else                                    bPreempt = (requesterPrio > ownerPrio);

    if (bPreempt) {
        // PREEMPTION — 현재 화자 REVOKE(cause=preempted) 후 신규 화자 grant.
        LOG_INFO("PMcpttGroup", "[%s] Floor PREEMPTED by %s (prio=%d chair=%d) from %s (prio=%d chair=%d)",
               _groupId.c_str(), sessionId.c_str(), requesterPrio, requesterChair,
               _floorOwnerSessionId.c_str(), ownerPrio, ownerChair);

        std::string prevOwner = _floorOwnerSessionId;
        unsigned int prevSsrc = _floorOwnerSsrc;

        // Revoke Current (Reject Cause 필드 = Media Burst pre-empted)
        {
            char revBuf[256];
            std::vector<FloorTlv> f{ FloorTlv(FF_REJECT_CAUSE, FloorU16(CAUSE_REVOKE_PREEMPTED)),
                                     FloorTlv(FF_GRANTED_PARTY, prevOwner) };
            int revLen = BuildFloorMessage(revBuf, sizeof(revBuf), FLOOR_REVOKE, prevSsrc, f);
            if (revLen > 0) sendToMember(prevOwner, revBuf, revLen);
        }
        {
            char ex[160];
            snprintf(ex, sizeof(ex), "\"preempted_by\":\"%s\",\"cause\":%d", sessionId.c_str(), CAUSE_REVOKE_PREEMPTED);
            _logFloorLocal("REVOKE", prevOwner, prevSsrc, ownerPrio, ex);
        }

        // 녹취: 이전 화자 세그먼트 종료
        if (_recordEnable && _recorder && _recorder->isActive()) {
            _recorder->finishSegment();
        }

        _grantFloorTo(sessionId, ssrc, requesterPrio, true, prevOwner);
        return;
    }

    // 비선점 — SDP `mc_queueing` 광고 시 큐잉(TS 24.380 §8). 큐가 가득 차면 Deny.
    if (_queueEnable && (int)_floorQueue.size() < _queueMax) {
        // 중복 제거 후 큐 추가
        for (auto it = _floorQueue.begin(); it != _floorQueue.end(); )
            it = (it->sessionId == sessionId) ? _floorQueue.erase(it) : it + 1;
        QueuedReq q{ sessionId, ssrc, requesterPrio, reqTier, requesterChair, _nowUsec() };
        _floorQueue.push_back(q);
        LOG_INFO("PMcpttGroup", "[%s] Floor QUEUED session=%s pos=%d/%zu (owner=%s)",
                 _groupId.c_str(), sessionId.c_str(), _queuePositionOf(sessionId), _floorQueue.size(),
                 _floorOwnerSessionId.c_str());
        _sendQueuePos(sessionId, ssrc);
        {
            char ex[160];
            snprintf(ex, sizeof(ex), "\"owner\":\"%s\",\"pos\":%d,\"qsize\":%zu",
                     _floorOwnerSessionId.c_str(), _queuePositionOf(sessionId), _floorQueue.size());
            _logFloorLocal("QUEUE", sessionId, ssrc, requesterPrio, ex);
        }
        return;
    }

    // 큐 비활성/포화 → Deny(queue full).
    _sendDeny(sessionId, ssrc, _queueEnable ? CAUSE_DENY_QUEUE_FULL : CAUSE_DENY_ANOTHER_CLIENT);
    LOG_INFO("PMcpttGroup", "[%s] Floor DENY session=%s (prio=%d). Owner=%s (prio=%d)",
           _groupId.c_str(), sessionId.c_str(), requesterPrio, _floorOwnerSessionId.c_str(), ownerPrio);
    {
        char ex[224];
        snprintf(ex, sizeof(ex), "\"owner\":\"%s\",\"owner_prio\":%d,\"tier\":\"%s\",\"owner_tier\":\"%s\"",
                 _floorOwnerSessionId.c_str(), ownerPrio, _tierName(reqTier), _tierName(ownTier));
        _logFloorLocal("DENY", sessionId, ssrc, requesterPrio, ex);
    }
}

// ── Floor 송신 헬퍼 (caller 가 _mutex 보유) ─────────────────────────────────

int PMcpttGroup::_indicatorFor(const std::string& sessionId) const {
    int t = tierOf(sessionId);
    if (t >= TIER_EMERGENCY) return FI_EMERGENCY;
    if (t == TIER_IMMINENT)  return FI_IMMINENT_PERIL;
    return FI_NORMAL;
}

void PMcpttGroup::_grantFloorTo(const std::string& sessionId, unsigned int ssrc, int prio,
                                bool preempt, const std::string& prevOwner) {
    _floorTaken = true;
    _floorOwnerSessionId = sessionId;
    _floorOwnerSsrc = ssrc;
    _floorGrantUsec = _nowUsec();
    _lastRtpUsec = 0;

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

    LOG_INFO("PMcpttGroup", "[%s] Floor GRANTED to session=%s ssrc=%u prio=%d preempt=%d",
             _groupId.c_str(), sessionId.c_str(), ssrc, prio, preempt);
    if (_logFlow) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"GRANT\",\"user\":\"%s\",\"ssrc\":%u,\"prio\":%d,\"preempt\":%s}",
                 sessionId.c_str(), ssrc, prio, preempt ? "true" : "false");
        _logFlow(_groupId, "cmp", "ue", "MCPTT", "FLOOR_GRANT", detail);
    }
    if (preempt) {
        char ex[224];
        snprintf(ex, sizeof(ex), "\"preempt\":true,\"preempted_from\":\"%s\",\"tier\":\"%s\"",
                 prevOwner.c_str(), _tierName(tierOf(sessionId)));
        _logFloorLocal("GRANT", sessionId, ssrc, prio, ex);
    } else {
        _logFloorLocal("GRANT", sessionId, ssrc, prio);
    }

    // 녹취: 초기화 안됐으면 초기화 + 세그먼트 시작.
    if (_recordEnable && !_recorder) startRecording();
    if (_recordEnable && _recorder) {
        // 화자 leg 의 ingress PT/코덱 (JOIN user_src_pt/user_codec) → 세그먼트 메타 (_mutex 보유 중)
        int spkPt = 0;
        std::string spkCodec;
        auto itSpk = _members.find(sessionId);
        if (itSpk != _members.end()) { spkPt = itSpk->second.srcPt; spkCodec = itSpk->second.codec; }
        if (preempt) _recorder->startPttSegment(sessionId, prio, true, prevOwner, spkPt, spkCodec);
        else         _recorder->startPttSegment(sessionId, prio, false, "", spkPt, spkCodec);
    }
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
    // 현재 owner 는 이미 해제된 상태에서 호출. 대기자 있으면 grant, 없으면 IDLE.
    while (!_floorQueue.empty()) {
        unsigned int qssrc = 0; int qprio = 0;
        std::string next = _popBestQueued(qssrc, qprio);
        if (next.empty()) break;
        if (_members.find(next) == _members.end()) continue;   // 이미 떠난 멤버 skip
        _grantFloorTo(next, qssrc, qprio, false, "");
        return;
    }
    broadcastFloorStatus(FLOOR_IDLE, 0, "");
}

void PMcpttGroup::handleFloorRelease(const std::string& sessionId, unsigned int ssrc) {
    PAutoLock lock(_mutex);
    if (_floorTaken && _floorOwnerSessionId == sessionId) {
        // 녹취: 현재 세그먼트 종료
        if (_recordEnable && _recorder && _recorder->isActive()) {
            _recorder->finishSegment();
        }

        _floorTaken = false;
        _floorOwnerSessionId = "";
        _floorOwnerSsrc = 0;

        int ownerPrio = 0;
        if (_priorities.find(sessionId) != _priorities.end()) ownerPrio = _priorities[sessionId];
        _logFloorLocal("RELEASE", sessionId, ssrc, ownerPrio);
        LOG_INFO("PMcpttGroup", "[%s] Floor RELEASED by session=%s", _groupId.c_str(), sessionId.c_str());

        // 대기열 있으면 최우선 대기자에게 넘기고, 없으면 IDLE.
        _advanceFloorOrIdle();
        // RX FLOOR_RELEASE 는 onRtcpPacket 에서 이미 기록됨 (중복 방지)
    }
}

int64_t PMcpttGroup::_nowUsec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

bool PMcpttGroup::checkFloorInactivity(int idleSec) {
    if (idleSec <= 0) return false;
    PAutoLock lock(_mutex);
    if (!_floorTaken) return false;
    // 긴급(emergency) 발언자는 무활동 자동 회수 제외 — 권한자 RELEASE/취소로만 해제(TS 24.380).
    if (tierOf(_floorOwnerSessionId) >= TIER_EMERGENCY) return false;

    // 판정 기준: 마지막 RTP 수신 시각(있으면) 또는 grant 시각(RTP 한 번도 안 온 경우).
    int64_t ref = (_lastRtpUsec > 0) ? _lastRtpUsec : _floorGrantUsec;
    if (ref == 0) return false;
    int64_t now = _nowUsec();
    if ((now - ref) < (int64_t)idleSec * 1000000LL) return false;

    std::string owner = _floorOwnerSessionId;
    unsigned int ssrc = _floorOwnerSsrc;
    int ownerPrio = 0;
    if (_priorities.find(owner) != _priorities.end()) ownerPrio = _priorities[owner];
    int idleMs = (int)((now - ref) / 1000);

    // 녹취 세그먼트 종료 (발언시간은 last-RTP 기준으로 이미 한정됨)
    if (_recordEnable && _recorder && _recorder->isActive()) {
        _recorder->finishSegment();
    }

    // 응답 없는 owner 에게 REVOKE 송출 (Reject Cause 필드 = Other reason).
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

    _floorTaken = false;
    _floorOwnerSessionId = "";
    _floorOwnerSsrc = 0;
    _floorGrantUsec = 0;
    _lastRtpUsec = 0;

    LOG_INFO("PMcpttGroup", "[%s] Floor auto-REVOKED (inactivity %dms, no RELEASE) owner=%s",
             _groupId.c_str(), idleMs, owner.c_str());
    if (_logFlow) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "{\"op\":\"REVOKE\",\"reason\":\"inactivity\",\"user\":\"%s\",\"ssrc\":%u,\"idle_ms\":%d}",
                 owner.c_str(), ssrc, idleMs);
        _logFlow(_groupId, "cmp", "ue", "MCPTT", "FLOOR_REVOKE", detail);
    }
    // 대기열 있으면 다음 화자에게 넘기고, 없으면 IDLE.
    _advanceFloorOrIdle();
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

// 하향 분배는 각 멤버의 전용 유닛 소켓에서 송신한다 — 멤버가 보는 소스 포트 =
// SDP 에 광고한 포트 (symmetric RTP 정합).
void PMcpttGroup::sendAudioToAll(const char* data, int len, const std::string& excludeSessionId) {
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
    for (auto& [sid, peer] : _members) {
        if (sid == excludeSessionId) continue;
        if (!peer.unit || peer.port <= 0) continue;
        // 수신자별 SSRC + 시퀀스 번호 재작성
        char pkt[4096];
        if (len > (int)sizeof(pkt)) continue;
        memcpy(pkt, data, len);
        peer.audioSeqOut++;
        uint16_t netSeq = htons(peer.audioSeqOut);
        memcpy(pkt + 2, &netSeq, 2);
        uint32_t netSsrc = htonl(peer.audioSsrcOut);
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
    if (_pttSession) {
        for (auto const& [sid, peer] : _members) {
            if (peer.floorPort > 0)
                _pttSession->sendFloorTo(peer.ip, peer.floorPort, (char*)data, len);
        }
    }
}

void PMcpttGroup::sendVideoToAll(const char* data, int len, const std::string& excludeSessionId) {
    if (len < 12) return;
    for (auto& [sid, peer] : _members) {
        if (sid == excludeSessionId) continue;
        if (!peer.unit || peer.videoPort <= 0) continue;
        char pkt[4096];
        if (len > (int)sizeof(pkt)) continue;
        memcpy(pkt, data, len);
        peer.videoSeqOut++;
        uint16_t netSeq = htons(peer.videoSeqOut);
        memcpy(pkt + 2, &netSeq, 2);
        uint32_t netSsrc = htonl(peer.videoSsrcOut);
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
    // PPttTrans의 floor 소켓으로 전송 (규격: m=application 포트)
    if (_pttSession && peer.floorPort > 0) {
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
    _recorder->addTrack("audio");
    _recorder->addTrack("video");

    LOG_INFO("PMcpttGroup", "[%s] Recording initialized: dir=%s",
             _groupId.c_str(), _recordDir.c_str());
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
