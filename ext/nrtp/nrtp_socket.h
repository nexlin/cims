#ifndef NRTP_SOCKET_H
#define NRTP_SOCKET_H

#include <string>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
using SockLen = int;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
using SocketHandle = int;
using SockLen = socklen_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

namespace nrtp {

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool Open();
    void Close();
    bool Bind(const std::string& ip, uint16_t port);
    
    // Send to specific destination
    int SendTo(const void* data, size_t size, const std::string& ip, uint16_t port);
    
    // Receive and get sender info
    int RecvFrom(void* buffer, size_t size, std::string& outIp, uint16_t& outPort);

    bool IsValid() const { return m_handle != INVALID_SOCKET; }

    // Utility to initialize/cleanup WSA on Windows
    static void InitSockets();
    static void CleanupSockets();

private:
    SocketHandle m_handle;
};

}

#endif // NRTP_SOCKET_H
