#include "McpttGroup.h"
#include "CmpLog.h"
#include "PRtpHandler.h"
#include "RtpRecorder.h"
#include <cstring>
#include <arpa/inet.h>
#include <cstdio>

unsigned int McpttGroup::_nextSsrc = 1000;

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

McpttGroup::McpttGroup(const std::string& groupId)
    : _groupId(groupId), _sharedSession(NULL), _floorTaken(false), _floorOwnerSsrc(0),
      _recordEnable(false), _segmentSeq(0), _segRecorderAudio(NULL), _segRecorderVideo(NULL)
{
}

McpttGroup::~McpttGroup() {
    stopSegment();
    PAutoLock lock(_mutex);
    _members.clear();
}

void McpttGroup::setSharedSession(PRtpTrans* session) {
    PAutoLock lock(_mutex);
    _sharedSession = session;
}

void McpttGroup::addMember(const std::string& sessionId, const std::string& ip, int port, int videoPort) {
    LOG_INFO("McpttGroup", "[%s] addMember session=%s ip=%s port=%d videoPort=%d", _groupId.c_str(), sessionId.c_str(), ip.c_str(), port, videoPort);
    PAutoLock lock(_mutex);
    Peer peer;
    peer.id = sessionId;
    peer.ip = ip;
    peer.port = port;
    peer.videoPort = videoPort;
    peer.ssrc = _nextSsrc++;
    peer.audioSeqOut = 0;
    peer.videoSeqOut = 0;
    peer.audioSsrcOut = 1000 + _nextSsrc;  // 수신자별 고정 SSRC
    peer.videoSsrcOut = 2000 + _nextSsrc;
    _members[sessionId] = peer;
    LOG_INFO("McpttGroup", "[%s] Member added session=%s (total=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());
    
    // If floor is taken, notify new member
    if (_floorTaken) {
         char pktBuf[256];
         int pktLen = BuildFloorPacket(pktBuf, sizeof(pktBuf), FLOOR_TAKEN,
                                       _floorOwnerSsrc, _floorOwnerSessionId);
         if (pktLen > 0)
             sendToMember(sessionId, pktBuf, pktLen);
         LOG_DEBUG("McpttGroup", "[%s] Notified new member %s about floor taken by %s",
                   _groupId.c_str(), sessionId.c_str(), _floorOwnerSessionId.c_str());
    }
}

void McpttGroup::removeMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    _members.erase(sessionId);
    // Throttled: suppress repeated "Member left" within 2 seconds
    LOG_THROTTLE(2, "McpttGroup", "[%s] Member %s left. (remaining=%lu)", _groupId.c_str(), sessionId.c_str(), _members.size());

    if ( _floorTaken && (_floorOwnerSessionId == sessionId) ) {
        // 녹취: 발언 세그먼트 종료
        if (_recordEnable) stopSegment();

        // Owner left, release floor
        _floorTaken = false;
        _floorOwnerSessionId = "";
        _floorOwnerSsrc = 0;
        broadcastFloorStatus(FLOOR_IDLE, 0, "");
        LOG_INFO("McpttGroup", "[%s] Floor owner %s left. Floor -> IDLE", _groupId.c_str(), sessionId.c_str());
    }
}

void McpttGroup::updatePriorities(const std::map<std::string, int>& priorities) {
    PAutoLock lock(_mutex);
    _priorities = priorities;
    LOG_INFO("McpttGroup", "[%s] Priorities updated for %lu members", _groupId.c_str(), _priorities.size());
}

void McpttGroup::setDtmfConfig(bool enable, const std::string& pushDigit, const std::string& releaseDigit) {
    PAutoLock lock(_mutex);
    _dtmfEnable = enable;
    _dtmfPushDigit = pushDigit;
    _dtmfReleaseDigit = releaseDigit;
    LOG_INFO("McpttGroup", "[%s] DTMF config: enable=%d push=%s release=%s", _groupId.c_str(), enable, pushDigit.c_str(), releaseDigit.c_str());
}

bool McpttGroup::hasMember(const std::string& sessionId) {
    PAutoLock lock(_mutex);
    return _members.find(sessionId) != _members.end();
}

void McpttGroup::onRtcpPacket(const std::string& ip, int port, char* buf, int len) {
    if (len < (int)sizeof(FloorControlPacket)) {
        LOG_DEBUG("McpttGroup", "[%s] RTCP too short len=%d from %s:%d", _groupId.c_str(), len, ip.c_str(), port);
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

    // Port latching for RTCP: IP 일치하지만 RTCP 포트 불일치 시 오디오 포트 업데이트
    if (sessionId == "") {
        std::string candidateId = "";
        unsigned int candidateSsrc = 0;
        int matchCount = 0;
        {
            PAutoLock lock(_mutex);
            for(auto const& [sid, peer] : _members) {
                if (peer.ip == ip) {
                    candidateId = sid;
                    candidateSsrc = peer.ssrc;
                    matchCount++;
                }
            }
            if (matchCount == 1) {
                int expectedRtpPort = port - 1;  // RTCP port = RTP port + 1
                LOG_INFO("McpttGroup", "[%s] RTCP port latching: %s audio %d -> %d (session=%s)",
                         _groupId.c_str(), ip.c_str(), _members[candidateId].port, expectedRtpPort, candidateId.c_str());
                _members[candidateId].port = expectedRtpPort;
                sessionId = candidateId;
                senderSsrc = candidateSsrc;
            }
        }
    }

    if (sessionId == "") {
        LOG_INFO("McpttGroup", "[%s] RTCP from unknown sender %s:%d (members=%lu)",
                  _groupId.c_str(), ip.c_str(), port, _members.size());
        return;
    }

    FloorControlPacket* pkt = (FloorControlPacket*)buf;

    if (pkt->type != RTCP_PT_APP) {
        LOG_DEBUG("McpttGroup", "[%s] RTCP pt=%d (not APP=204), skip", _groupId.c_str(), pkt->type);
        return;
    }
    if (memcmp(pkt->name, "MCPT", 4) != 0) {
        LOG_DEBUG("McpttGroup", "[%s] RTCP APP name mismatch, skip", _groupId.c_str());
        return;
    }

    unsigned char opcode = pkt->opcode;
    unsigned int pktSsrc = ntohl(pkt->ssrc);

    static const char* opcodeStr[] = {"?","REQUEST","GRANT","REJECT","RELEASE","IDLE","TAKEN","REVOKE"};
    const char* opName = (opcode < 8) ? opcodeStr[opcode] : "?";
    LOG_INFO("McpttGroup", "[%s] Floor RTCP opcode=%d(%s) ssrc=%u session=%s from %s:%d",
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

void McpttGroup::onRtpPacket(const std::string& ip, int port, char* buf, int len) {
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

        // Port latching: IP 일치하지만 포트 불일치 시 포트 업데이트
        if (senderId == "") {
            std::string candidateId = "";
            unsigned int candidateSsrc = 0;
            int matchCount = 0;
            for(auto const& [sid, peer] : _members) {
                if (peer.ip == ip) {
                    candidateId = sid;
                    candidateSsrc = peer.ssrc;
                    matchCount++;
                }
            }
            if (matchCount == 1) {
                LOG_INFO("McpttGroup", "[%s] Audio port latching: %s:%d -> %s:%d (session=%s)",
                         _groupId.c_str(), ip.c_str(), _members[candidateId].port, ip.c_str(), port, candidateId.c_str());
                _members[candidateId].port = port;
                senderId = candidateId;
                senderSsrc = candidateSsrc;
            } else {
                LOG_INFO("McpttGroup", "[%s] RTP from unknown sender %s:%d (members=%lu)",
                         _groupId.c_str(), ip.c_str(), port, _members.size());
            }
        }
        
        unsigned char pt = (unsigned char)(buf[1] & 0x7F);
        LOG_DEBUG("McpttGroup", "[%s] RTP ip=%s port=%d len=%d pt=%d sender=%s", _groupId.c_str(), ip.c_str(), port, len, pt, senderId.c_str());

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
                            LOG_INFO("McpttGroup", "[%s] DTMF Push '%c' from %s", _groupId.c_str(), digitChar, senderId.c_str());
                            action = "REQUEST";
                            actionSenderId = senderId;
                            actionSsrc = senderSsrc;
                        } else if (dStr == _dtmfReleaseDigit) {
                            LOG_INFO("McpttGroup", "[%s] DTMF Release '%c' from %s", _groupId.c_str(), digitChar, senderId.c_str());
                            action = "RELEASE";
                            actionSenderId = senderId;
                            actionSsrc = senderSsrc;
                        }
                    }
                }
            }

            if (_floorTaken && _floorOwnerSessionId == senderId) {
                sendAudioToAll(buf, len, ip, port);
                // 녹취: 발언 음성 기록
                if (_recordEnable && _segRecorderAudio && _segRecorderAudio->IsRecording()) {
                    _segRecorderAudio->WritePacket(buf, len);
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

void McpttGroup::onVideoRtpPacket(const std::string& ip, int port, char* buf, int len) {
    PAutoLock lock(_mutex);

    if (!_floorTaken) return;

    std::string senderId = "";

    // 1차: 정확한 IP:videoPort 매칭
    for (auto const& [sid, peer] : _members) {
        if (peer.ip == ip && peer.videoPort == port) {
            senderId = sid;
            break;
        }
    }

    // 2차: IP 매칭 + floor owner 체크 (NAT/동적 포트 대응)
    if (senderId.empty()) {
        // floor owner의 IP와 일치하면 latching
        auto itOwner = _members.find(_floorOwnerSessionId);
        if (itOwner != _members.end() && itOwner->second.ip == ip) {
            LOG_INFO("McpttGroup", "[%s] Video port latching (floor owner): %s:%d -> %s:%d (session=%s)",
                     _groupId.c_str(), ip.c_str(), itOwner->second.videoPort, ip.c_str(), port, _floorOwnerSessionId.c_str());
            itOwner->second.videoPort = port;
            senderId = _floorOwnerSessionId;
        }
    }

    // 3차: IP만으로 단일 매칭 (후보가 1명)
    if (senderId.empty()) {
        std::string candidateId;
        int matchCount = 0;
        for (auto const& [sid, peer] : _members) {
            if (peer.ip == ip) {
                candidateId = sid;
                matchCount++;
            }
        }
        if (matchCount == 1) {
            LOG_INFO("McpttGroup", "[%s] Video port latching (single IP): %s:%d (session=%s)",
                     _groupId.c_str(), ip.c_str(), port, candidateId.c_str());
            _members[candidateId].videoPort = port;
            senderId = candidateId;
        }
    }

    if (senderId.empty() || _floorOwnerSessionId != senderId) return;

    sendVideoToAll(buf, len, ip, port);
}

void McpttGroup::handleFloorRequest(const std::string& sessionId, unsigned int ssrc) {
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

        LOG_INFO("McpttGroup", "[%s] Floor GRANTED to session=%s ssrc=%u prio=%d",
                 _groupId.c_str(), sessionId.c_str(), ssrc, requesterPrio);
        if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "floor", "MCPTT", "floor-grant",
                               ("speaker=" + sessionId).c_str());

        // 녹취: 새 세그먼트 시작
        if (_recordEnable) startSegment(sessionId);
    } else {
        if (_floorOwnerSessionId == sessionId) return;

        int ownerPrio = 999;
        if (_priorities.find(_floorOwnerSessionId) != _priorities.end()) ownerPrio = _priorities[_floorOwnerSessionId];

        if (requesterPrio < ownerPrio) {
            // PREEMPTION
            LOG_INFO("McpttGroup", "[%s] Floor PREEMPTED by %s (prio=%d) from %s (prio=%d)",
                   _groupId.c_str(), sessionId.c_str(), requesterPrio, _floorOwnerSessionId.c_str(), ownerPrio);

            // Revoke Current
            char revBuf[256];
            int revLen = BuildFloorPacket(revBuf, sizeof(revBuf), FLOOR_REVOKE, _floorOwnerSsrc, _floorOwnerSessionId);
            if (revLen > 0) sendToMember(_floorOwnerSessionId, revBuf, revLen);

            // 녹취: 이전 발언 세그먼트 종료 + 새 세그먼트 시작
            if (_recordEnable) { stopSegment(); startSegment(sessionId); }

            // Grant New
            _floorOwnerSessionId = sessionId;
            _floorOwnerSsrc = ssrc;

            char grantBuf[256];
            int grantLen = BuildFloorPacket(grantBuf, sizeof(grantBuf), FLOOR_GRANT, ssrc, sessionId);
            if (grantLen > 0) sendToMember(sessionId, grantBuf, grantLen);

            // Broadcast Taken (New Owner)
            broadcastFloorStatus(FLOOR_TAKEN, ssrc, sessionId);
        } else {
            // REJECT
            char rejBuf[256];
            int rejLen = BuildFloorPacket(rejBuf, sizeof(rejBuf), FLOOR_REJECT, ssrc, sessionId);
            if (rejLen > 0) sendToMember(sessionId, rejBuf, rejLen);
            LOG_INFO("McpttGroup", "[%s] Floor REJECTED session=%s (prio=%d). Owner=%s (prio=%d)",
                   _groupId.c_str(), sessionId.c_str(), requesterPrio, _floorOwnerSessionId.c_str(), ownerPrio);
            if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "floor", "MCPTT", "floor-reject", "");
        }
    }
}

void McpttGroup::handleFloorRelease(const std::string& sessionId, unsigned int ssrc) {
    PAutoLock lock(_mutex);
    if (_floorTaken && _floorOwnerSessionId == sessionId) {
        // 녹취: 발언 세그먼트 종료
        if (_recordEnable) stopSegment();

        _floorTaken = false;
        _floorOwnerSessionId = "";
        _floorOwnerSsrc = 0;

        broadcastFloorStatus(FLOOR_IDLE, 0, "");
        LOG_INFO("McpttGroup", "[%s] Floor RELEASED by session=%s", _groupId.c_str(), sessionId.c_str());
        if (_logFlow) _logFlow(_groupId, sessionId.c_str(), "floor", "MCPTT", "floor-release",
                               ("speaker=" + sessionId).c_str());
    }
}

void McpttGroup::broadcastFloorStatus(unsigned char opcode, unsigned int ssrc, const std::string& speakerId) {
    static const char* opcodeStr[] = {"?","REQUEST","GRANT","REJECT","RELEASE","IDLE","TAKEN","REVOKE"};
    const char* opName = (opcode < 8) ? opcodeStr[opcode] : "?";
    LOG_INFO("McpttGroup", "[%s] broadcastFloorStatus opcode=%d(%s) speaker=%s ssrc=%u → %lu members",
             _groupId.c_str(), opcode, opName, speakerId.c_str(), ssrc, _members.size());

    char pktBuf[256];
    int pktLen = BuildFloorPacket(pktBuf, sizeof(pktBuf), opcode, ssrc, speakerId);
    if (pktLen > 0)
        sendAudioRtcpToAll(pktBuf, pktLen, "", 0);
}

void McpttGroup::sendAudioToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (!_sharedSession || len < 12) return;
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
        _sharedSession->sendTo(peer.ip, peer.port, pkt, len);
    }
}

void McpttGroup::sendAudioRtcpToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (_sharedSession) {
        for (auto const& [sid, peer] : _members) {
            if (peer.ip == excludeIp && peer.port == excludePort) continue;
            // Audio RTCP: Peer Port + 1
            _sharedSession->sendTo(peer.ip, peer.port + 1, (char*)data, len); 
        }
    }
}

void McpttGroup::sendVideoToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (!_sharedSession || len < 12) return;
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
            _sharedSession->sendVideoTo(peer.ip, peer.videoPort, pkt, len);
        }
    }
}

void McpttGroup::sendVideoRtcpToAll(const char* data, int len, const std::string& excludeIp, int excludePort) {
    if (_sharedSession) {
        for (auto const& [sid, peer] : _members) {
            if (peer.ip == excludeIp && peer.videoPort + 1 == excludePort) continue;
            if (peer.videoPort > 0) {
                _sharedSession->sendVideoTo(peer.ip, peer.videoPort + 1, (char*)data, len);
            }
        }
    }
}

void McpttGroup::sendToMember(const std::string& sessionId, const char* data, int len) {
    if (_sharedSession && _members.find(sessionId) != _members.end()) {
        const Peer& peer = _members[sessionId];
        LOG_INFO("McpttGroup", "[%s] sendToMember session=%s → %s:%d (RTCP)",
                 _groupId.c_str(), sessionId.c_str(), peer.ip.c_str(), peer.port + 1);
        _sharedSession->sendTo(peer.ip, peer.port + 1, (char*)data, len);
    } else {
        LOG_ERROR("McpttGroup", "[%s] sendToMember session=%s not found or no sharedSession",
                  _groupId.c_str(), sessionId.c_str());
    }
}

// ──────────────────────────────────────────────────────────────
//  녹취 — 발언 단위 세그먼트
// ──────────────────────────────────────────────────────────────

void McpttGroup::setRecording(bool enable, const std::string& dir) {
    _recordEnable = enable;
    _recordDir = dir;
    if (enable && !dir.empty()) {
        std::string mkdirCmd = "mkdir -p " + dir;
        system(mkdirCmd.c_str());
    }
}

void McpttGroup::startSegment(const std::string& speakerId) {
    stopSegment();

    _segmentSeq++;
    _segSpeakerId = speakerId;

    char seqStr[32];
    snprintf(seqStr, sizeof(seqStr), "seg_%04d", _segmentSeq);

    // record_dir은 CSP가 결정한 전체 경로 (groupId 하위 디렉터리 불필요)
    std::string basePath = _recordDir + "/" + seqStr;

    _segRecorderAudio = new RtpRecorder();
    _segRecorderAudio->Start(basePath + "_audio.rtp");

    _segRecorderVideo = new RtpRecorder();
    // 영상은 lazy start (onVideoRtpPacket에서 시작)

    LOG_INFO("McpttGroup", "[%s] Segment %d started: speaker=%s",
             _groupId.c_str(), _segmentSeq, speakerId.c_str());
}

void McpttGroup::stopSegment() {
    if (!_segRecorderAudio && !_segRecorderVideo) return;

    std::string audioRaw, videoRaw;

    if (_segRecorderAudio) {
        _segRecorderAudio->Stop();
        audioRaw = _segRecorderAudio->GetRawPath();
        delete _segRecorderAudio;
        _segRecorderAudio = NULL;
    }
    if (_segRecorderVideo) {
        if (_segRecorderVideo->IsRecording()) {
            _segRecorderVideo->Stop();
            videoRaw = _segRecorderVideo->GetRawPath();
        }
        delete _segRecorderVideo;
        _segRecorderVideo = NULL;
    }

    LOG_INFO("McpttGroup", "[%s] Segment %d stopped: speaker=%s",
             _groupId.c_str(), _segmentSeq, _segSpeakerId.c_str());

    // TODO: 트랜스코딩 큐 등록 + DB 업데이트
    // TranscodeQueue::Instance().Enqueue(audioRaw, videoRaw, outputPath, callback);
}
