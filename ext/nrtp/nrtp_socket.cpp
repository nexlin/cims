#include "nrtp_socket.h"
#include <iostream>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace nrtp {

UdpSocket::UdpSocket() : m_handle(INVALID_SOCKET) {
}

UdpSocket::~UdpSocket() {
    Close();
}

void UdpSocket::InitSockets() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

void UdpSocket::CleanupSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool UdpSocket::Open() {
    if (m_handle != INVALID_SOCKET) return true;

    m_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_handle == INVALID_SOCKET) {
        return false;
    }
    return true;
}

void UdpSocket::Close() {
    if (m_handle != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(m_handle);
#else
        close(m_handle);
#endif
        m_handle = INVALID_SOCKET;
    }
}

bool UdpSocket::Bind(const std::string& ip, uint16_t port) {
    if (m_handle == INVALID_SOCKET) return false;

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    }

    if (bind(m_handle, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        return false;
    }
    return true;
}

int UdpSocket::SendTo(const void* data, size_t size, const std::string& ip, uint16_t port) {
    if (m_handle == INVALID_SOCKET) return -1;

    sockaddr_in destAddr;
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &destAddr.sin_addr);

    return sendto(m_handle, (const char*)data, (int)size, 0, (struct sockaddr*)&destAddr, sizeof(destAddr));
}

int UdpSocket::RecvFrom(void* buffer, size_t size, std::string& outIp, uint16_t& outPort) {
    if (m_handle == INVALID_SOCKET) return -1;

    sockaddr_in senderAddr;
    SockLen len = sizeof(senderAddr);
    
    int bytes = recvfrom(m_handle, (char*)buffer, (int)size, 0, (struct sockaddr*)&senderAddr, &len);
    
    if (bytes > 0) {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &senderAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
        outIp = ipStr;
        outPort = ntohs(senderAddr.sin_port);
    }
    
    return bytes;
}

}
