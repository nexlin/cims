#ifndef __PRTP_HANDLER_H__
#define __PRTP_HANDLER_H__

#include <string>
#include <iostream>

#include "pbase.h"
#include "pmodule.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "CmpLog.h"

#define SOCKET_ERROR -1

#include "PMPBase.h"

#define PT_AUDIO1   98  // amr-nb
#define PT_AUDIO2   100 //dtmf  

#define INVALID_SOCKET           (-1)

class McpttGroup; // Forward declaration
class RtpRecorder;

class CRtpSocket 
{
private: 
   int _fd;
   std::string _ipLoc;
   unsigned int _portLoc;
   std::string _ipRmt;
   unsigned int _portRmt;

   struct sockaddr_in _addrLoc;
   struct sockaddr_in _addrRmt;
   struct sockaddr_in _addrRmtLast;

public : 
   CRtpSocket() { _fd = INVALID_SOCKET; };
   ~CRtpSocket() { final(); };
   
   int getFd() const { return _fd; }

   bool init(const std::string ip, unsigned int port) 
   {  
      _fd = socket(AF_INET, SOCK_DGRAM, 0);
      if(_fd == INVALID_SOCKET) {
	      LOG_ERROR("CRtpSocket", "Can't create socket (ip=%s port=%d)", ip.c_str(), port);
	      return false;
      }

      memset(&_addrLoc, 0, sizeof(_addrLoc));
      _addrLoc.sin_family = AF_INET;
      _addrLoc.sin_addr.s_addr = inet_addr(ip.c_str());
      _addrLoc.sin_port = htons((unsigned short)port);
   
      int ret =  bind(_fd, (sockaddr*)&_addrLoc, sizeof(_addrLoc));
      if(ret == -1) {
	      LOG_ERROR("CRtpSocket", "Can't bind socket (ip=%s port=%d): %s", ip.c_str(), port, strerror(errno));
	      return false;
      }

      int flags;
      flags = fcntl(_fd, F_GETFL, 0);
      fcntl(_fd, F_SETFL, flags | O_NONBLOCK);

      int nRet = 256 * 1024;  // 256KB
      setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, (char*)&nRet, sizeof(nRet));

      nRet = 256 * 1024;  // 256KB (영상 FU-A 버스트 대응)
      setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, (char*)&nRet, sizeof(nRet));

      memset(&_addrRmt, 0, sizeof(_addrRmt));
      memset(&_addrRmtLast, 0, sizeof(_addrRmtLast));
      return true;
   }

   bool final() {
      if(_fd != INVALID_SOCKET) {
         // close(_fd); // Defined in unistd.h
         ::close(_fd);
         _fd = INVALID_SOCKET;
         return true;
      }
      return false;
   }
 
   bool setRmt(const std::string ip, unsigned int port) { 
      _ipRmt = ip;
      _portRmt = port;
      
      _addrRmt.sin_family = AF_INET;
      _addrRmt.sin_addr.s_addr = inet_addr(ip.c_str());
      _addrRmt.sin_port = htons((unsigned short)port);

      return true;
   }

   int send(char * pkt, int len)
   {
      int ret = sendto(_fd, pkt, len, 0, (sockaddr*)&_addrRmt, sizeof(struct sockaddr));
      if (ret < 0) {
          LOG_ERROR("CRtpSocket", "sendto failed: %s (fd=%d)", strerror(errno), _fd);
      }
      return ret;
   }

   int sendTo(char * pkt, int len, struct sockaddr_in* pAddr)
   {
      int ret = sendto(_fd, pkt, len, 0, (sockaddr*)pAddr, sizeof(struct sockaddr));
      if (ret < 0) {
          // fprintf(stderr, "sendTo failed: %s (fd=%d)\n", strerror(errno), _fd);
      }
      return ret;
   }

   int recv(char * pkt, int len, std::string & ipRmt, int &portRmt) 
   {
      socklen_t nSockAddrLen = sizeof(struct sockaddr);
     
      int nLen = recvfrom(_fd, pkt, len, 0, (struct sockaddr *)&_addrRmtLast, (socklen_t*)&nSockAddrLen); 
      if(nLen == SOCKET_ERROR) 
      {
         return -1;
      }
      portRmt = ntohs(_addrRmtLast.sin_port);
      // ipRmt = (char *) inet_ntoa(_addrRmtLast.sin_addr);
      // Inet_ntoa isn't thread safe potentially, but usually ok in single thread per worker.
      // Better to use static buffer or inet_ntop? inet_ntoa returns static buffer.
      ipRmt = inet_ntoa(_addrRmtLast.sin_addr);

      return nLen;
   }
   
   struct sockaddr_in& getLastAddr() { return _addrRmtLast; }

};

class PRtpTrans : public PHandler
{
private: 
  CRtpSocket _rtpSock;
  CRtpSocket _rtcpSock;
  CRtpSocket _videoRtpSock;
  CRtpSocket _videoRtcpSock;
  
  char       _pkt[1600];

  PMutex     _mutex;
  McpttGroup* _group;
  std::string _myName;
  std::string _sessionId;
  
  // Stored local ports (set during init)
  unsigned int _localPort;
  unsigned int _localVideoPort;
  
  // Relay Peers
  struct PeerInfo {
      std::string ip;
      unsigned int port;
      unsigned int videoPort;
      struct sockaddr_in addrRtp;
      struct sockaddr_in addrRtcp;
      struct sockaddr_in addrVideoRtp;
      struct sockaddr_in addrVideoRtcp;
      bool active;
  };
  
  PeerInfo _peers[2];
  
  PRtpTrans* _bridgePeer; // [P2P Bridge Support]

public:
  PRtpTrans(const std::string & name);
  virtual ~PRtpTrans();

  bool init(const std::string & ipLoc, unsigned int portLoc, unsigned int videoPortLoc = 0);
  bool final();
  bool setRmt(const std::string & ipRmt, unsigned int portRmt, unsigned int videoPortRmt = 0, int peerIdx = -1);
  
  unsigned int getLocalPort() const { return _localPort; }
  unsigned int getLocalVideoPort() const { return _localVideoPort; }

  void setSessionId(const std::string& id) { _sessionId = id; }
  std::string getSessionId() const { return _sessionId; }

  void setGroup(McpttGroup* group);
  void sendRtcp(char* data, int len);
  void sendRtp(char* data, int len);
  void sendVideoRtp(char* data, int len);

  // [Shared Session Support]
  void sendTo(const std::string& ip, int port, char* data, int len);
  void sendVideoTo(const std::string& ip, int port, char* data, int len);
  
  void setBridgePeer(PRtpTrans* peer) { _bridgePeer = peer; }
  PRtpTrans* getBridgePeer() const { return _bridgePeer; }

  bool proc(); 
  bool proc(int id, const std::string & name, PEvent::Ptr spEvent);

  void reset();
  void setWorkerName(const std::string& name) { _workerName = name; }
  std::string getWorkerName() const { return _workerName; }

  /** 마지막 RTP 패킷 수신 시간 갱신 */
  void touchActivity() { time(&_lastActivityTime); }
  /** 마지막 activity 시간 반환 */
  time_t getLastActivityTime() const { return _lastActivityTime; }

  // 녹취
  void startRecording(const std::string& rawDir, const std::string& sessionId);
  void stopRecording();
  bool isRecording() const { return _recording; }

private:
  std::string _workerName;
  time_t _lastActivityTime;

  // 녹취 (양방향)
  bool _recording;
  RtpRecorder* _recorderA;      // peer[0] → peer[1] 방향 음성
  RtpRecorder* _recorderB;      // peer[1] → peer[0] 방향 음성
  RtpRecorder* _recorderVA;     // peer[0] 영상
  RtpRecorder* _recorderVB;     // peer[1] 영상
  std::string _recordSessionId;
  std::string _recordRawDir;
};

// ─── PTT 전용 핸들러 ─────────────────────────────────────────
// audio RTP (+ RTCP 비활성) + floor control (m=application UDP MCPTT)
class PPttTrans : public PHandler
{
private:
  CRtpSocket _rtpSock;           // audio RTP
  CRtpSocket _floorSock;         // floor control (m=application)

  PMutex     _mutex;
  McpttGroup* _group;
  std::string _sessionId;

  unsigned int _localRtpPort;
  unsigned int _localFloorPort;

public:
  PPttTrans(const std::string& name);
  virtual ~PPttTrans();

  bool init(const std::string& ip, unsigned int rtpPort, unsigned int floorPort);
  bool final();

  unsigned int getLocalRtpPort() const { return _localRtpPort; }
  unsigned int getLocalFloorPort() const { return _localFloorPort; }

  void setSessionId(const std::string& id) { _sessionId = id; }
  std::string getSessionId() const { return _sessionId; }

  void setGroup(McpttGroup* group);
  McpttGroup* getGroup() const { return _group; }

  // Floor control 소켓 — McpttGroup에서 직접 사용
  void sendFloorTo(const std::string& ip, int port, char* data, int len);

  bool proc();
  bool proc(int id, const std::string& name, PEvent::Ptr spEvent);

  void reset();
  void setWorkerName(const std::string& name) { _workerName = name; }
  std::string getWorkerName() const { return _workerName; }

  void touchActivity() { time(&_lastActivityTime); }
  time_t getLastActivityTime() const { return _lastActivityTime; }

private:
  std::string _workerName;
  time_t _lastActivityTime;
};

#endif // __PRTP_HANDLER_H__
