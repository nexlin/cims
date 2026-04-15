#ifndef __PRTP_SOCKET_H__
#define __PRTP_SOCKET_H__

#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "PLog.h"

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)

/**
 * Non-blocking UDP 소켓 래퍼.
 * RTP/RTCP/Floor control 등 모든 UDP 미디어 소켓의 공통 기반.
 */
class PRtpSocket
{
private:
   int _fd;
   struct sockaddr_in _addrLoc;
   struct sockaddr_in _addrRmt;
   struct sockaddr_in _addrRmtLast;

public:
   PRtpSocket() : _fd(INVALID_SOCKET) {
       memset(&_addrLoc, 0, sizeof(_addrLoc));
       memset(&_addrRmt, 0, sizeof(_addrRmt));
       memset(&_addrRmtLast, 0, sizeof(_addrRmtLast));
   }
   ~PRtpSocket() { close(); }

   int getFd() const { return _fd; }

   bool open(const std::string& ip, unsigned int port) {
      _fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (_fd == INVALID_SOCKET) {
          LOG_ERROR("PRtpSocket", "socket() failed: %s (ip=%s port=%d)", strerror(errno), ip.c_str(), port);
          return false;
      }

      _addrLoc.sin_family = AF_INET;
      _addrLoc.sin_addr.s_addr = inet_addr(ip.c_str());
      _addrLoc.sin_port = htons((unsigned short)port);

      if (bind(_fd, (sockaddr*)&_addrLoc, sizeof(_addrLoc)) == -1) {
          LOG_ERROR("PRtpSocket", "bind() failed: %s (ip=%s port=%d)", strerror(errno), ip.c_str(), port);
          ::close(_fd);
          _fd = INVALID_SOCKET;
          return false;
      }

      // Non-blocking
      fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL, 0) | O_NONBLOCK);

      // 256KB 버퍼 (영상 FU-A 버스트 대응)
      int bufSize = 256 * 1024;
      setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
      setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));

      return true;
   }

   void close() {
      if (_fd != INVALID_SOCKET) {
          ::close(_fd);
          _fd = INVALID_SOCKET;
      }
   }

   void setRemote(const std::string& ip, unsigned int port) {
      _addrRmt.sin_family = AF_INET;
      _addrRmt.sin_addr.s_addr = inet_addr(ip.c_str());
      _addrRmt.sin_port = htons((unsigned short)port);
   }

   int send(const char* pkt, int len) {
      return sendto(_fd, pkt, len, 0, (sockaddr*)&_addrRmt, sizeof(sockaddr));
   }

   int sendTo(const char* pkt, int len, const std::string& ip, int port) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = inet_addr(ip.c_str());
      addr.sin_port = htons(port);
      return sendto(_fd, pkt, len, 0, (sockaddr*)&addr, sizeof(addr));
   }

   int sendTo(const char* pkt, int len, struct sockaddr_in* addr) {
      return sendto(_fd, pkt, len, 0, (sockaddr*)addr, sizeof(sockaddr));
   }

   int recv(char* pkt, int maxLen, std::string& srcIp, int& srcPort) {
      socklen_t addrLen = sizeof(sockaddr);
      int n = recvfrom(_fd, pkt, maxLen, 0, (sockaddr*)&_addrRmtLast, &addrLen);
      if (n <= 0) return -1;
      srcPort = ntohs(_addrRmtLast.sin_port);
      srcIp = inet_ntoa(_addrRmtLast.sin_addr);
      return n;
   }

   struct sockaddr_in& getLastAddr() { return _addrRmtLast; }
};

#endif // __PRTP_SOCKET_H__
