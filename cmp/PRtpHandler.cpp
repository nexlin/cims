#include "PRtpHandler.h"
#include "CmpLog.h"
#include "McpttGroup.h"
#include "RtpRecorder.h"
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>

PRtpTrans::PRtpTrans(const std::string & name) : PHandler(name), _group(NULL), _myName(name), _sessionId(name), _localPort(0), _localVideoPort(0),
    _lastActivityTime(0), _recording(false), _recorderA(NULL), _recorderB(NULL), _recorderVA(NULL), _recorderVB(NULL) {
    time(&_lastActivityTime);
    for(int i=0; i<2; ++i) {
        _peers[i].active = false;
        memset(&_peers[i].addrRtp, 0, sizeof(sockaddr_in));
        memset(&_peers[i].addrRtcp, 0, sizeof(sockaddr_in));
        memset(&_peers[i].addrVideoRtp, 0, sizeof(sockaddr_in));
        memset(&_peers[i].addrVideoRtcp, 0, sizeof(sockaddr_in));
    }
}

PRtpTrans::~PRtpTrans() {
    final();
}

void PRtpTrans::setGroup(McpttGroup* group) {
    PAutoLock lock(_mutex);
    _group = group;
}

bool PRtpTrans::init(const std::string & ipLoc, unsigned int portLoc, unsigned int videoPortLoc)
{
    PAutoLock lock(_mutex);
    bool res = _rtpSock.init(ipLoc, portLoc);
    LOG_INFO("PRtpTrans", "init rtp %s:%d", ipLoc.c_str(), portLoc);
    if(res) {
        // Init RTCP on port + 1
        LOG_INFO("PRtpTrans", "init rtcp %s:%d", ipLoc.c_str(), portLoc + 1);
        res = _rtcpSock.init(ipLoc, portLoc + 1);
    }
    
    if (res && videoPortLoc > 0) {
        LOG_INFO("PRtpTrans", "init video rtp %s:%d", ipLoc.c_str(), videoPortLoc);
        res = _videoRtpSock.init(ipLoc, videoPortLoc);
        if (res) {
            LOG_INFO("PRtpTrans", "init video rtcp %s:%d", ipLoc.c_str(), videoPortLoc + 1);
            res = _videoRtcpSock.init(ipLoc, videoPortLoc + 1);
        }
    }
    
    if (res) {
        _localPort = portLoc;
        _localVideoPort = videoPortLoc;
    }
    return res;
}

bool PRtpTrans::final()
{
    PAutoLock lock(_mutex);
    _rtcpSock.final();
    _videoRtpSock.final();
    _videoRtcpSock.final();
    return _rtpSock.final();
}

bool PRtpTrans::setRmt(const std::string & ipRmt, unsigned int portRmt, unsigned int videoPortRmt, int peerIdx) {
    PAutoLock lock(_mutex);

    int idx = peerIdx;
    
    if (idx == -1) {
        // Smart Logic
        if (_peers[0].active && _peers[0].ip == ipRmt) { 
            idx = 0;
        } else if (_peers[1].active && _peers[1].ip == ipRmt) {
            idx = 1;
        }
        
        if (idx == -1) {
            if (!_peers[0].active) idx = 0;
            else if (!_peers[1].active) idx = 1;
            else {
                 if (_peers[0].ip == "0.0.0.0") idx = 0;
                 else idx = 1; // Default overwrite second
            }
        }
    }
    
    if (idx < 0 || idx > 1) {
        // Invalid index
        LOG_ERROR("PRtpTrans", "setRmt invalid index %d", idx);
        return false;
    }

    // Update Peer[idx]
    _peers[idx].ip = ipRmt;
    _peers[idx].port = portRmt;
    _peers[idx].videoPort = videoPortRmt;
    _peers[idx].active = true;

    // Helper to constructing sockaddr_in
    auto makeAddr = [](struct sockaddr_in& addr, const std::string& ip, int port) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        addr.sin_port = htons(port);
    };

    LOG_INFO("PRtpTrans", "setRmt peer[%d] = %s:%d video=%d session=%s",
             idx, ipRmt.c_str(), portRmt, videoPortRmt, _sessionId.c_str());

    makeAddr(_peers[idx].addrRtp, ipRmt, portRmt);
    makeAddr(_peers[idx].addrRtcp, ipRmt, portRmt + 1);
    if (videoPortRmt > 0) {
        makeAddr(_peers[idx].addrVideoRtp, ipRmt, videoPortRmt);
        makeAddr(_peers[idx].addrVideoRtcp, ipRmt, videoPortRmt + 1);
    }

    if (idx == 0) {
        _rtpSock.setRmt(ipRmt, portRmt);
        _rtcpSock.setRmt(ipRmt, portRmt + 1);
        if (videoPortRmt > 0) {
            _videoRtpSock.setRmt(ipRmt, videoPortRmt);
            _videoRtcpSock.setRmt(ipRmt, videoPortRmt + 1);
        }
    }
    return true;
}

void PRtpTrans::sendRtcp(char* data, int len) {
    PAutoLock lock(_mutex);
    if (_rtcpSock.getFd() != INVALID_SOCKET) {
        _rtcpSock.send(data, len);
    }
}

void PRtpTrans::sendRtp(char* data, int len) {
    PAutoLock lock(_mutex);
    if (_rtpSock.getFd() != INVALID_SOCKET) {
        _rtpSock.send(data, len);
    }
}

void PRtpTrans::sendVideoRtp(char* data, int len) {
    PAutoLock lock(_mutex);
    if (_videoRtpSock.getFd() != INVALID_SOCKET) {
        _videoRtpSock.send(data, len);
    }
}

// [Shared Session Support]
void PRtpTrans::sendTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_rtpSock.getFd() != INVALID_SOCKET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        addr.sin_port = htons(port);
        _rtpSock.sendTo(data, len, &addr);
    }
}

void PRtpTrans::sendVideoTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_videoRtpSock.getFd() != INVALID_SOCKET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        addr.sin_port = htons(port);
        _videoRtpSock.sendTo(data, len, &addr);
    }
}

bool PRtpTrans::proc() 
{
    std::string ipRmt;
    int portRmt;
    char rtcpBuf[2048];
    char pkt[1600];
    
    // ── RTCP (floor control) 먼저 처리 → floor 상태 업데이트 후 RTP 포워딩 ──

    // Process Audio RTCP (floor control)
    while(true) {
         int rtcpFd;
         {
            PAutoLock lock(_mutex);
            rtcpFd = _rtcpSock.getFd();
         }
         if (rtcpFd == INVALID_SOCKET) break;

         int len = 0;
         McpttGroup* pGroup = NULL;
         {
             PAutoLock lock(_mutex);
             len = _rtcpSock.recv(rtcpBuf, sizeof(rtcpBuf), ipRmt, portRmt);
             pGroup = _group;
         }

         if (len > 0) {
             if (pGroup) {
                 pGroup->onRtcpPacket(ipRmt, portRmt, rtcpBuf, len);
             } else {
                  PAutoLock lock(_mutex);
                  int srcIdx = -1;
                  if (_peers[0].active && portRmt == _peers[0].port + 1 && ipRmt == _peers[0].ip) srcIdx = 0;
                  else if (_peers[1].active && portRmt == _peers[1].port + 1 && ipRmt == _peers[1].ip) srcIdx = 1;
                  else if (_peers[0].active && portRmt == _peers[0].port + 1) {
                      _peers[0].ip = ipRmt;
                      _peers[0].addrRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      _peers[0].addrRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      srcIdx = 0;
                  } else if (_peers[1].active && portRmt == _peers[1].port + 1) {
                      _peers[1].ip = ipRmt;
                      _peers[1].addrRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      _peers[1].addrRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      srcIdx = 1;
                  }

                  if (srcIdx != -1) {
                      int dstIdx = (srcIdx == 0) ? 1 : 0;
                      if (_peers[dstIdx].active) {
                          _rtcpSock.sendTo(rtcpBuf, len, &_peers[dstIdx].addrRtcp);
                      }
                  } else if (_peers[0].active && !_peers[1].active) {
                       _rtcpSock.send(rtcpBuf, len);
                  }
             }
         }
         if (len <= 0) break;
    }

    // Process Video RTCP
    while(true) {
        int rtcpFd;
        {
            PAutoLock lock(_mutex);
            rtcpFd = _videoRtcpSock.getFd();
        }
        if (rtcpFd == INVALID_SOCKET) break;

        int len = 0;
        {
            PAutoLock lock(_mutex);
            len = _videoRtcpSock.recv(rtcpBuf, sizeof(rtcpBuf), ipRmt, portRmt);
            if (len > 0) {
                // Relay Video RTCP
                LOG_DEBUG("PRtpTrans", "Video RTCP rx len=%d from %s:%d", len, ipRmt.c_str(), portRmt);
                int srcIdx = -1;
                if (_peers[0].active && portRmt == _peers[0].videoPort + 1 && ipRmt == _peers[0].ip) srcIdx = 0;
                else if (_peers[1].active && portRmt == _peers[1].videoPort + 1 && ipRmt == _peers[1].ip) srcIdx = 1;
                else if (_peers[0].active && _peers[0].videoPort > 0 && portRmt == _peers[0].videoPort + 1) {
                    _peers[0].ip = ipRmt;
                    _peers[0].addrVideoRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    _peers[0].addrVideoRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    srcIdx = 0;
                } else if (_peers[1].active && _peers[1].videoPort > 0 && portRmt == _peers[1].videoPort + 1) {
                    _peers[1].ip = ipRmt;
                    _peers[1].addrVideoRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    _peers[1].addrVideoRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    srcIdx = 1;
                }

                if (srcIdx != -1) {
                    int dstIdx = (srcIdx == 0) ? 1 : 0;
                    if (_peers[dstIdx].active && _peers[dstIdx].videoPort > 0) {
                        _videoRtcpSock.sendTo(rtcpBuf, len, &_peers[dstIdx].addrVideoRtcp);
                    }
                } else if (_peers[0].active && !_peers[1].active) {
                    _videoRtcpSock.send(rtcpBuf, len);
                }
            }
        }

        if (len <= 0) break;
    }

    // ── RTP (미디어) 처리 — floor 상태가 이미 최신이므로 즉시 포워딩 가능 ──

    // Process Audio RTP
    while (true) {
        int rtpFd;
        {
            PAutoLock lock(_mutex);
            rtpFd = _rtpSock.getFd();
        }
        if (rtpFd == INVALID_SOCKET) break;

        int len = 0;
        McpttGroup* pGroup = NULL;
        {
            PAutoLock lock(_mutex);
            len = _rtpSock.recv(pkt, sizeof(pkt), ipRmt, portRmt);
            pGroup = _group;
        }

        if (len > 0) {
            touchActivity();
            if (pGroup) {
                pGroup->onRtpPacket(ipRmt, portRmt, pkt, len);
            } else {
                PAutoLock lock(_mutex);
                // Symmetric RTP: exact match first, then port-only match with IP learning
                int srcIdx = -1;
                if (_peers[0].active && portRmt == _peers[0].port && ipRmt == _peers[0].ip) srcIdx = 0;
                else if (_peers[1].active && portRmt == _peers[1].port && ipRmt == _peers[1].ip) srcIdx = 1;
                else if (_peers[0].active && portRmt == _peers[0].port && ipRmt != _peers[0].ip) {
                    // Port matches but IP differs (NAT/loopback) — learn actual source IP
                    LOG_INFO("PRtpTrans", "Symmetric RTP: peer[0] IP learned %s -> %s (port %d)",
                             _peers[0].ip.c_str(), ipRmt.c_str(), portRmt);
                    _peers[0].ip = ipRmt;
                    _peers[0].addrRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    _peers[0].addrRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    srcIdx = 0;
                } else if (_peers[1].active && portRmt == _peers[1].port && ipRmt != _peers[1].ip) {
                    LOG_INFO("PRtpTrans", "Symmetric RTP: peer[1] IP learned %s -> %s (port %d)",
                             _peers[1].ip.c_str(), ipRmt.c_str(), portRmt);
                    _peers[1].ip = ipRmt;
                    _peers[1].addrRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    _peers[1].addrRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                    srcIdx = 1;
                }

                if (srcIdx != -1) {
                    int dstIdx = (srcIdx == 0) ? 1 : 0;
                    if (_peers[dstIdx].active) {
                        _rtpSock.sendTo(pkt, len, &_peers[dstIdx].addrRtp);
                    }
                    // 녹취: 방향별 기록
                    if (_recording) {
                        if (srcIdx == 0 && _recorderA) _recorderA->WritePacket(pkt, len);
                        else if (srcIdx == 1 && _recorderB) _recorderB->WritePacket(pkt, len);
                    }
                } else if (_peers[0].active && !_peers[1].active) {
                    _rtpSock.send(pkt, len);
                }
            }
        }

        if (len <= 0) break;
    }

    // Process Video RTP
    while (true) {
        int rtpFd;
        {
            PAutoLock lock(_mutex);
            rtpFd = _videoRtpSock.getFd();
        }
        if (rtpFd == INVALID_SOCKET) break;

        int len = 0;
        McpttGroup* pGroup = NULL;
        {
            PAutoLock lock(_mutex);
            len = _videoRtpSock.recv(pkt, sizeof(pkt), ipRmt, portRmt);
            pGroup = _group;
        }

        if (len > 0) {
             if (pGroup) {
                  pGroup->onVideoRtpPacket(ipRmt, portRmt, pkt, len);
             } else {
                  PAutoLock lock(_mutex);
                  int srcIdx = -1;
                  if (_peers[0].active && portRmt == _peers[0].videoPort && ipRmt == _peers[0].ip) srcIdx = 0;
                  else if (_peers[1].active && portRmt == _peers[1].videoPort && ipRmt == _peers[1].ip) srcIdx = 1;
                  else if (_peers[0].active && _peers[0].videoPort > 0 && portRmt == _peers[0].videoPort) {
                      LOG_INFO("PRtpTrans", "Symmetric RTP: video peer[0] IP learned %s -> %s (port %d)",
                               _peers[0].ip.c_str(), ipRmt.c_str(), portRmt);
                      _peers[0].ip = ipRmt;
                      _peers[0].addrVideoRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      _peers[0].addrVideoRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      srcIdx = 0;
                  } else if (_peers[1].active && _peers[1].videoPort > 0 && portRmt == _peers[1].videoPort) {
                      LOG_INFO("PRtpTrans", "Symmetric RTP: video peer[1] IP learned %s -> %s (port %d)",
                               _peers[1].ip.c_str(), ipRmt.c_str(), portRmt);
                      _peers[1].ip = ipRmt;
                      _peers[1].addrVideoRtp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      _peers[1].addrVideoRtcp.sin_addr.s_addr = inet_addr(ipRmt.c_str());
                      srcIdx = 1;
                  }

                  if (srcIdx != -1) {
                      int dstIdx = (srcIdx == 0) ? 1 : 0;
                      if (_peers[dstIdx].active && _peers[dstIdx].videoPort > 0) {
                          _videoRtpSock.sendTo(pkt, len, &_peers[dstIdx].addrVideoRtp);
                      }
                      // 영상 녹취
                      if (_recording) {
                          RtpRecorder* vRec = (srcIdx == 0) ? _recorderVA : _recorderVB;
                          if (vRec && !vRec->IsRecording()) {
                              // Lazy start: 첫 영상 패킷에서 녹취 시작
                              vRec->Start(_recordRawDir + ((srcIdx == 0) ? "/raw_va.rtp" : "/raw_vb.rtp"));
                          }
                          if (vRec) vRec->WritePacket(pkt, len);
                      }
                  } else if (_peers[0].active && !_peers[1].active) {
                      _videoRtpSock.send(pkt, len);
                  }
             }
        }

        if (len <= 0) break;
    }

    return false;
}

bool PRtpTrans::proc(int id, const std::string & name, PEvent::Ptr spEvent) {
    return false;
}

void PRtpTrans::reset() {
    stopRecording();

    PAutoLock lock(_mutex);
    _sessionId = "";
    // _workerName 유지 — worker thread는 영구 동작
    _group = NULL;

    for(int i=0; i<2; ++i) {
        _peers[i].active = false;
        _peers[i].ip = "";
        _peers[i].port = 0;
        _peers[i].videoPort = 0;
        memset(&_peers[i].addrRtp, 0, sizeof(sockaddr_in));
        memset(&_peers[i].addrRtcp, 0, sizeof(sockaddr_in));
        memset(&_peers[i].addrVideoRtp, 0, sizeof(sockaddr_in));
        memset(&_peers[i].addrVideoRtcp, 0, sizeof(sockaddr_in));
    }
}

void PRtpTrans::startRecording(const std::string& rawDir, const std::string& sessionId) {
    _recordRawDir = rawDir;
    _recordSessionId = sessionId;

    // record_dir 디렉터리 재귀 생성
    {
        std::string path = rawDir;
        for (size_t i = 1; i < path.size(); ++i) {
            if (path[i] == '/') {
                path[i] = '\0';
                mkdir(path.c_str(), 0755);
                path[i] = '/';
            }
        }
        mkdir(path.c_str(), 0755);
    }

    _recorderA = new RtpRecorder();
    _recorderB = new RtpRecorder();
    _recorderA->Start(rawDir + "/raw_a.rtp");
    _recorderB->Start(rawDir + "/raw_b.rtp");

    _recorderVA = new RtpRecorder();
    _recorderVB = new RtpRecorder();
    // 영상 녹취는 실제 영상 패킷이 올 때만 활성화 (lazy start)

    _recording = true;
    LOG_INFO("PRtpTrans", "Recording started: dir=%s session=%s", rawDir.c_str(), sessionId.c_str());
}

void PRtpTrans::stopRecording() {
    if (!_recording) return;
    _recording = false;

    std::string audioPathA, audioPathB, videoPathA, videoPathB;

    if (_recorderA) { _recorderA->Stop(); audioPathA = _recorderA->GetRawPath(); delete _recorderA; _recorderA = NULL; }
    if (_recorderB) { _recorderB->Stop(); audioPathB = _recorderB->GetRawPath(); delete _recorderB; _recorderB = NULL; }
    if (_recorderVA) {
        if (_recorderVA->IsRecording()) { _recorderVA->Stop(); videoPathA = _recorderVA->GetRawPath(); }
        delete _recorderVA; _recorderVA = NULL;
    }
    if (_recorderVB) {
        if (_recorderVB->IsRecording()) { _recorderVB->Stop(); videoPathB = _recorderVB->GetRawPath(); }
        delete _recorderVB; _recorderVB = NULL;
    }

    LOG_INFO("PRtpTrans", "Recording stopped: session=%s, enqueueing transcode", _recordSessionId.c_str());

    // TODO: 트랜스코딩 큐에 등록하고 DB 업데이트 콜백 연결
    // 현재는 raw 파일만 보존
}

// ═══════════════════════════════════════════════════════════════
//  PPttTrans — PTT 전용 핸들러 (audio RTP + floor control)
// ═══════════════════════════════════════════════════════════════

PPttTrans::PPttTrans(const std::string& name)
    : PHandler(name), _group(NULL), _sessionId(name),
      _localRtpPort(0), _localFloorPort(0), _lastActivityTime(0)
{
    time(&_lastActivityTime);
}

PPttTrans::~PPttTrans() { final(); }

bool PPttTrans::init(const std::string& ip, unsigned int rtpPort, unsigned int floorPort) {
    PAutoLock lock(_mutex);
    bool res = _rtpSock.init(ip, rtpPort);
    if (res) {
        LOG_INFO("PPttTrans", "init rtp %s:%d", ip.c_str(), rtpPort);
    }
    if (res && floorPort > 0) {
        res = _floorSock.init(ip, floorPort);
        if (res) {
            LOG_INFO("PPttTrans", "init floor %s:%d", ip.c_str(), floorPort);
        }
    }
    if (res) {
        _localRtpPort = rtpPort;
        _localFloorPort = floorPort;
    }
    return res;
}

bool PPttTrans::final() {
    PAutoLock lock(_mutex);
    _floorSock.final();
    return _rtpSock.final();
}

void PPttTrans::setGroup(McpttGroup* group) {
    PAutoLock lock(_mutex);
    _group = group;
}

void PPttTrans::sendFloorTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_floorSock.getFd() != INVALID_SOCKET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        addr.sin_port = htons(port);
        _floorSock.sendTo(data, len, &addr);
    }
}

bool PPttTrans::proc() {
    std::string ipRmt;
    int portRmt;
    char pkt[1600];

    // ── Floor control 패킷 수신 ──
    while (true) {
        int floorFd;
        {
            PAutoLock lock(_mutex);
            floorFd = _floorSock.getFd();
        }
        if (floorFd == INVALID_SOCKET) break;

        int len = 0;
        McpttGroup* pGroup = NULL;
        {
            PAutoLock lock(_mutex);
            len = _floorSock.recv(pkt, sizeof(pkt), ipRmt, portRmt);
            pGroup = _group;
        }

        if (len > 0 && pGroup) {
            pGroup->onFloorPacket(ipRmt, portRmt, pkt, len);
        }
        if (len <= 0) break;
    }

    // ── Audio RTP 수신 ──
    while (true) {
        int rtpFd;
        {
            PAutoLock lock(_mutex);
            rtpFd = _rtpSock.getFd();
        }
        if (rtpFd == INVALID_SOCKET) break;

        int len = 0;
        McpttGroup* pGroup = NULL;
        {
            PAutoLock lock(_mutex);
            len = _rtpSock.recv(pkt, sizeof(pkt), ipRmt, portRmt);
            pGroup = _group;
        }

        if (len > 0) {
            touchActivity();
            if (pGroup) {
                pGroup->onRtpPacket(ipRmt, portRmt, pkt, len);
            }
        }
        if (len <= 0) break;
    }

    return false;
}

bool PPttTrans::proc(int id, const std::string& name, PEvent::Ptr spEvent) {
    return false;
}

void PPttTrans::reset() {
    PAutoLock lock(_mutex);
    _sessionId = "";
    _group = NULL;
}
