#include "PMcpttGroup.h"
#include "PLog.h"
#include "PRtpMulticast.h"
#include "PSyncRtpRecorder.h"
#include <cstring>
#include <arpa/inet.h>
#include <cstdio>

unsigned int PMcpttGroup::_nextSsrc = 1000;

// ── Floor 패킷 빌드/파싱 헬퍼 ─────────────────────────────────────────────────

int BuildFloorPacket(char* buf, int bufSize, unsigned char opcode,
                     unsigned int ssrc, const std::string& speakerId)
{
    int idLen = (int)speakerId.size();
    int padded = (idLen + 3) & ~3;  // 4-byte 정렬
    int total = 12 + 4 + padded;    // RTCP APP 헤더(12) + opcode+id_len+reserved(4) + speakerId
    if (total > bufSize) return 0;

    memset(buf, 0, total);
    FloorControlPacket* pkt = (FloorControlPacket*)buf;
    pkt->version_subtype = 0x80;
    pkt->type   = RTCP_PT_APP;
    pkt->length = htons((uint16_t)(total / 4 - 1));
    pkt->ssrc   = htonl(ssrc);
    memcpy(pkt->name, "MCPT", 4);
    pkt->opcode = opcode;
    pkt->id_len = (unsigned char)idLen;

    if (idLen > 0)
        memcpy(buf + sizeof(FloorControlPacket), speakerId.c_str(), idLen);

    return total;
}

std::string ParseFloorSpeakerId(const char* buf, int len)
{
    if (len < (int)sizeof(FloorControlPacket)) return "";
    const FloorControlPacket* pkt = (const FloorControlPacket*)buf;
    int idLen = pkt->id_len;
    if (idLen <= 0 || (int)sizeof(FloorControlPacket) + idLen > len) return "";
    return std::string(buf + sizeof(FloorControlPacket), idLen);
}

PMcpttGroup::PMcpttGroup(const std::string& groupId)
    : _groupId(groupId), _pttSession(NULL),
      _floorTaken(false), _floorOwnerSsrc(0),
      _recordEnable(false), _recorder(NULL)
{
}

PMcpttGroup::~PMcpttGroup() {
    stopRecording();
    PAutoLock lock(_mutex);
    _members.clear();
}

void PMcpttGroup::addMember(const std::string& sessionId, const std::string& ip, int port, int floorPort, int videoPort) {
    LOG_INFO("PMcpttGroup", "[%s] addMember session=%s ip=%s rtp=%d floor=%d video=%d", _groupId.c_str(), sessionId.c_str(), ip.c_str(), port, floorPort, videoPort);
    PAutoLock lock(_mutex);
    Peer peer;
    peer.id = sessionId;
    peer.ip = ip;
    peer.port = port;
    peer.floorPort = floorPort;
    peer.videoPort = videoPort;
    peer.ssrc = _nextSsrc++;
    peer.audioSeqOut = 0;
    peer.videoSeqOut = 0;
    peer.audioSsrcOut = 1000 + _nextSsrc;  // 수신자별 고정 SSRC
    peer.videoSsrcOut = 2000 + _nextSsrc;
    _members[sessionId] = peer;
    LOG_INFO("PMcpttGroup", "[%s] Member added session=%s (total=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());
    if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "cmp", "MCPTT", "MEMBER_JOIN",
                           ("ip=" + ip + " rtp=" + std::to_string(port)).c_str());
    
    // If floor is taken, notify new member
    if (_floorTaken) {
         char pktBuf[256];
         int pktLen = BuildFloorPacket(pktBuf, sizeof(pktBuf), FLOOR_TAKEN,
                                       _floorOwnerSsrc, _floorOwnerSessionId);
         if (pktLen > 0)
             sendToMember(sessionId, pktBuf, pktLen);
         LOG_DEBUG("PMcpttGroup", "[%s] Notified new member %s about floor taken by %s",
                   _groupId.c_str(), sessionId.c_str(), _floorOwnerSessionId.c_str());
    }
}

void PMcpttGroup::removeMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    _members.erase(sessionId);
    LOG_INFO("PMcpttGroup", "[%s] Member %s left. (remaining=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());
    if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "cmp", "MCPTT", "MEMBER_LEAVE", "");

    if ( _floorTaken && (_floorOwnerSessionId == sessionId) ) {

        // Owner left, release floor — 녹취 세그먼트 종료
        if (_recordEnable && _recorder && _recorder->isActive()) {
            _recorder->finishSegment();
        }

        _floorTaken = false;
        _floorOwnerSessionId = "";
        _floorOwnerSsrc = 0;
        broadcastFloorStatus(FLOOR_IDLE, 0, "");
        LOG_INFO("PMcpttGroup", "[%s] Floor owner %s left. Floor -> IDLE", _groupId.c_str(), sessionId.c_str());
    }
}

void PMcpttGroup::updatePriorities(const std::map<std::string, int>& priorities) {
    PAutoLock lock(_mutex);
    _priorities = priorities;
    LOG_INFO("PMcpttGroup", "[%s] Priorities updated for %lu members", _groupId.c_str(), _priorities.size());
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

void PMcpttGroup::onFloorPacket(const std::string& ip, int port, char* buf, int len) {
    if (len < (int)sizeof(FloorControlPacket)) return;

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
        // Symmetric floor: port-only match with IP learning
        if (sessionId.empty()) {
            for (auto& [sid, peer] : _members) {
                if (peer.floorPort == port) {
                    LOG_INFO("PMcpttGroup", "[%s] Floor IP learned %s -> %s (port %d, session=%s)",
                             _groupId.c_str(), peer.ip.c_str(), ip.c_str(), port, sid.c_str());
                    peer.ip = ip;
                    sessionId = sid;
                    senderSsrc = peer.ssrc;
                    break;
                }
            }
        }
    }

    if (sessionId.empty()) {
        LOG_INFO("PMcpttGroup", "[%s] Floor from unknown %s:%d", _groupId.c_str(), ip.c_str(), port);
        return;
    }

    FloorControlPacket* pkt = (FloorControlPacket*)buf;
    if (pkt->type != RTCP_PT_APP) return;

    unsigned char opcode = pkt->opcode;
    static const char* opcodeStr[] = {"?","REQUEST","GRANT","REJECT","RELEASE","IDLE","TAKEN","REVOKE"};
    const char* opName = (opcode < 8) ? opcodeStr[opcode] : "?";
    LOG_INFO("PMcpttGroup", "[%s] Floor %s from session=%s %s:%d",
             _groupId.c_str(), opName, sessionId.c_str(), ip.c_str(), port);

    if (opcode == FLOOR_REQUEST) handleFloorRequest(sessionId, senderSsrc);
    else if (opcode == FLOOR_RELEASE) handleFloorRelease(sessionId, senderSsrc);
}

void PMcpttGroup::onRtcpPacket(const std::string& ip, int port, char* buf, int len) {
    if (len < (int)sizeof(FloorControlPacket)) {
        LOG_DEBUG("PMcpttGroup", "[%s] RTCP too short len=%d from %s:%d", _groupId.c_str(), len, ip.c_str(), port);
        return;
    }

    // Check sender — peer.port는 RTP 포트, RTCP source port = peer.port + 1
    std::string sessionId = "";
    unsigned int senderSsrc = 0;
    {
        PAutoLock lock(_mutex);
        for(auto const& [sid, peer] : _members) {
            if (peer.ip == ip && peer.port + 1 == port) {
                sessionId = sid;
                senderSsrc = peer.ssrc;
                break;
            }
        }
    }

    if (sessionId == "") {
        LOG_INFO("PMcpttGroup", "[%s] RTCP from unknown sender %s:%d (members=%lu)",
                  _groupId.c_str(), ip.c_str(), port, _members.size());
        return;
    }

    FloorControlPacket* pkt = (FloorControlPacket*)buf;

    if (pkt->type != RTCP_PT_APP) {
        LOG_DEBUG("PMcpttGroup", "[%s] RTCP pt=%d (not APP=204), skip", _groupId.c_str(), pkt->type);
        return;
    }
    if (memcmp(pkt->name, "MCPT", 4) != 0) {
        LOG_DEBUG("PMcpttGroup", "[%s] RTCP APP name mismatch, skip", _groupId.c_str());
        return;
    }

    unsigned char opcode = pkt->opcode;
    unsigned int pktSsrc = ntohl(pkt->ssrc);

    static const char* opcodeStr[] = {"?","REQUEST","GRANT","REJECT","RELEASE","IDLE","TAKEN","REVOKE"};
    const char* opName = (opcode < 8) ? opcodeStr[opcode] : "?";
    LOG_INFO("PMcpttGroup", "[%s] Floor RTCP opcode=%d(%s) ssrc=%u session=%s from %s:%d",
             _groupId.c_str(), opcode, opName, pktSsrc, sessionId.c_str(), ip.c_str(), port);

    // Dispatch — senderSsrc는 CMP가 할당한 SSRC
    switch(opcode) {
        case FLOOR_REQUEST:
            handleFloorRequest(sessionId, senderSsrc);
            break;
        case FLOOR_RELEASE:
            handleFloorRelease(sessionId, senderSsrc);
            break;
        default:
            break;
    }
}

void PMcpttGroup::onRtpPacket(const std::string& ip, int port, char* buf, int len) {
    std::string action = "NONE";
    std::string actionSenderId = "";
    unsigned int actionSsrc = 0;

    {
        PAutoLock lock(_mutex);

        // Find sender session
        std::string senderId = "";
        unsigned int senderSsrc = 0;
        for(auto const& [sid, peer] : _members) {
            if (peer.ip == ip && peer.port == port) {
                senderId = sid;
                senderSsrc = peer.ssrc;
                break;
            }
        }

        // 협상된 Peer의 IP:port 매칭 안 되면 drop
        if (senderId.empty()) {
            return;
        }
        
        unsigned char pt = (unsigned char)(buf[1] & 0x7F);
        LOG_DEBUG("PMcpttGroup", "[%s] RTP ip=%s port=%d len=%d pt=%d sender=%s", _groupId.c_str(), ip.c_str(), port, len, pt, senderId.c_str());

        if (senderId != "") {
            // [DTMF Check]
            if (_dtmfEnable && len > 12) { 
                if (pt == 101) { 
                    unsigned char digitCode = (unsigned char)buf[12];
                    bool endBit = (buf[13] & 0x80) != 0; 
                    
                    char digitChar = 0;
                    if (digitCode >= 0 && digitCode <= 9) digitChar = '0' + digitCode;
                    else if (digitCode == 10) digitChar = '*';
                    else if (digitCode == 11) digitChar = '#';
                    else if (digitCode >= 12 && digitCode <= 15) digitChar = 'A' + (digitCode - 12);
                    
                    if (digitChar != 0 && endBit) {
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
                sendAudioToAll(buf, len, ip, port);
                // 녹취: 세그먼트 파일에 기록
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

void PMcpttGroup::onVideoRtpPacket(const std::string& ip, int port, char* buf, int len) {
    PAutoLock lock(_mutex);

    if (!_floorTaken) return;

    // 협상된 Peer의 IP:videoPort 정확 매칭만 허용
    std::string senderId = "";
    for (auto const& [sid, peer] : _members) {
        if (peer.ip == ip && peer.videoPort == port) {
            senderId = sid;
            break;
        }
    }

    if (senderId.empty() || _floorOwnerSessionId != senderId) return;

    sendVideoToAll(buf, len, ip, port);

    // 녹취: 비디오 세그먼트 파일에 기록
    if (_recordEnable && _recorder && _recorder->isActive()) {
        _recorder->writePacket("video", buf, len);
    }
}

void PMcpttGroup::handleFloorRequest(const std::string& sessionId, unsigned int ssrc) {
    PAutoLock lock(_mutex);
    int requesterPrio = 999;
    if (_priorities.find(sessionId) != _priorities.end()) requesterPrio = _priorities[sessionId];

    if (!_floorTaken) {
        // Grant Floor
        _floorTaken = true;
        _floorOwnerSessionId = sessionId;
        _floorOwnerSsrc = ssrc;

        // Send Grant to Requestor (speakerId = 자기 자신)
        char grantBuf[256];
        int grantLen = BuildFloorPacket(grantBuf, sizeof(grantBuf), FLOOR_GRANT, ssrc, sessionId);
        if (grantLen > 0) sendToMember(sessionId, grantBuf, grantLen);

        // Broadcast Taken to all (화자 identity 포함)
        broadcastFloorStatus(FLOOR_TAKEN, ssrc, sessionId);

        LOG_INFO("PMcpttGroup", "[%s] Floor GRANTED to session=%s ssrc=%u prio=%d",
                 _groupId.c_str(), sessionId.c_str(), ssrc, requesterPrio);
        if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "floor", "MCPTT", "FLOOR_GRANT",
                               ("speaker=" + sessionId).c_str());

        // 녹취: 초기화 안됐으면 초기화 + 세그먼트 시작
        if (_recordEnable && !_recorder) startRecording();
        if (_recordEnable && _recorder) {
            int seq = _recorder->getCurrentSeq() + 1;
            _recorder->startSegment(seq, sessionId);
        }
    } else {
        if (_floorOwnerSessionId == sessionId) return;

        int ownerPrio = 999;
        if (_priorities.find(_floorOwnerSessionId) != _priorities.end()) ownerPrio = _priorities[_floorOwnerSessionId];

        if (requesterPrio < ownerPrio) {
            // PREEMPTION
            LOG_INFO("PMcpttGroup", "[%s] Floor PREEMPTED by %s (prio=%d) from %s (prio=%d)",
                   _groupId.c_str(), sessionId.c_str(), requesterPrio, _floorOwnerSessionId.c_str(), ownerPrio);

            // Revoke Current
            char revBuf[256];
            int revLen = BuildFloorPacket(revBuf, sizeof(revBuf), FLOOR_REVOKE, _floorOwnerSsrc, _floorOwnerSessionId);
            if (revLen > 0) sendToMember(_floorOwnerSessionId, revBuf, revLen);

            // 녹취: 이전 화자 세그먼트 종료
            if (_recordEnable && _recorder && _recorder->isActive()) {
                _recorder->finishSegment();
            }

            // Grant New
            _floorOwnerSessionId = sessionId;
            _floorOwnerSsrc = ssrc;

            char grantBuf[256];
            int grantLen = BuildFloorPacket(grantBuf, sizeof(grantBuf), FLOOR_GRANT, ssrc, sessionId);
            if (grantLen > 0) sendToMember(sessionId, grantBuf, grantLen);

            // Broadcast Taken (New Owner)
            broadcastFloorStatus(FLOOR_TAKEN, ssrc, sessionId);

            // 녹취: 새 화자 세그먼트 시작
            if (_recordEnable && _recorder) {
                int seq = _recorder->getCurrentSeq() + 1;
                _recorder->startSegment(seq, sessionId);
            }
        } else {
            // REJECT
            char rejBuf[256];
            int rejLen = BuildFloorPacket(rejBuf, sizeof(rejBuf), FLOOR_REJECT, ssrc, sessionId);
            if (rejLen > 0) sendToMember(sessionId, rejBuf, rejLen);
            LOG_INFO("PMcpttGroup", "[%s] Floor REJECTED session=%s (prio=%d). Owner=%s (prio=%d)",
                   _groupId.c_str(), sessionId.c_str(), requesterPrio, _floorOwnerSessionId.c_str(), ownerPrio);
            if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "floor", "MCPTT", "FLOOR_REJECT", "");
        }
    }
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

        broadcastFloorStatus(FLOOR_IDLE, 0, "");
        LOG_INFO("PMcpttGroup", "[%s] Floor RELEASED by session=%s", _groupId.c_str(), sessionId.c_str());
        if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "floor", "MCPTT", "FLOOR_RELEASE",
                               ("speaker=" + sessionId).c_str());
    }
}

void PMcpttGroup::broadcastFloorStatus(unsigned char opcode, unsigned int ssrc, const std::string& speakerId) {
    static const char* opcodeStr[] = {"?","REQUEST","GRANT","REJECT","RELEASE","IDLE","TAKEN","REVOKE"};
    const char* opName = (opcode < 8) ? opcodeStr[opcode] : "?";
    LOG_INFO("PMcpttGroup", "[%s] broadcastFloorStatus opcode=%d(%s) speaker=%s ssrc=%u → %lu members",
             _groupId.c_str(), opcode, opName, speakerId.c_str(), ssrc, _members.size());

    char pktBuf[256];
    int pktLen = BuildFloorPacket(pktBuf, sizeof(pktBuf), opcode, ssrc, speakerId);
    if (pktLen > 0)
        sendAudioRtcpToAll(pktBuf, pktLen, "", 0);
}

void PMcpttGroup::sendAudioToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (!_pttSession || len < 12) return;
    for (auto& [sid, peer] : _members) {
        if (peer.ip == excludeIp && peer.port == excludePort) continue;
        // 수신자별 SSRC + 시퀀스 번호 재작성
        char pkt[4096];
        if (len > (int)sizeof(pkt)) continue;
        memcpy(pkt, data, len);
        peer.audioSeqOut++;
        uint16_t netSeq = htons(peer.audioSeqOut);
        memcpy(pkt + 2, &netSeq, 2);
        uint32_t netSsrc = htonl(peer.audioSsrcOut);
        memcpy(pkt + 8, &netSsrc, 4);
        _pttSession->sendAudioTo(peer.ip, peer.port, pkt, len);
    }
}

void PMcpttGroup::sendAudioRtcpToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (_pttSession) {
        for (auto const& [sid, peer] : _members) {
            if (peer.ip == excludeIp && peer.port == excludePort) continue;
            _pttSession->sendFloorTo(peer.ip, peer.port + 1, (char*)data, len);
        }
    }
}

void PMcpttGroup::sendVideoToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (!_pttSession || len < 12) return;
    for (auto& [sid, peer] : _members) {
        if (peer.ip == excludeIp && peer.videoPort == excludePort) continue;
        if (peer.videoPort > 0) {
            char pkt[4096];
            if (len > (int)sizeof(pkt)) continue;
            memcpy(pkt, data, len);
            peer.videoSeqOut++;
            uint16_t netSeq = htons(peer.videoSeqOut);
            memcpy(pkt + 2, &netSeq, 2);
            uint32_t netSsrc = htonl(peer.videoSsrcOut);
            memcpy(pkt + 8, &netSsrc, 4);
            _pttSession->sendVideoTo(peer.ip, peer.videoPort, pkt, len);
        }
    }
}

void PMcpttGroup::sendVideoRtcpToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (_pttSession) {
        for (auto const& [sid, peer] : _members) {
            if (peer.ip == excludeIp && peer.videoPort + 1 == excludePort) continue;
            if (peer.videoPort > 0) {
                _pttSession->sendVideoTo(peer.ip, peer.videoPort + 1, (char*)data, len);
            }
        }
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
    if (!_recorder) return;

    if (_recorder->isActive())
        _recorder->finishSegment();

    delete _recorder;
    _recorder = NULL;

    LOG_INFO("PMcpttGroup", "[%s] Recording stopped: dir=%s",
             _groupId.c_str(), _recordDir.c_str());
}
