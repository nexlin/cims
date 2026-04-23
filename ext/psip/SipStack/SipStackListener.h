/*
 * SipStackListener: 개별 SIP 리스너(소켓+스레드 묶음)의 런타임 표현
 *
 * 기존 CSipStack 은 UDP/TCP/TLS 당 단일 소켓을 보유한 구조였다. 다중 리스너
 * (동일 프로세스에서 여러 IP/포트로 listen) 와 무중단 add/remove 를 위해
 * 각 리스너를 이 클래스로 분리 관리한다.
 *
 * R3 (2026-04-23): UDP 에 더해 TCP/TLS 도 다중 리스너 지원. TCP/TLS 는 리스너당
 * accept thread 1개 + shared worker thread pool (m_clsTcpThreadList / m_clsTlsThreadList)
 * 을 공유하는 구조. UDP 는 리스너당 recv thread N개 (per-listener pool).
 */

#ifndef _SIP_STACK_LISTENER_H_
#define _SIP_STACK_LISTENER_H_

#include <string>
#include <atomic>
#include "SipTcp.h"
#include "SipMutex.h"

class CSipStack;

/** 단일 UDP 리스너. */
class CSipStackUdpListener
{
public:
    CSipStackUdpListener()
        : m_iId(0),
          m_hSocket(INVALID_SOCKET),
          m_iPort(0),
          m_iThreadCount(1),
          m_iActiveThreads(0),
          m_bDrain(false),
          m_bIpv6(false),
          m_pclsStack(NULL)
    {}

    /** CSP 에서 발급한 논리 ID (DB 의 csp_listener.id 등). */
    int           m_iId;

    /** 바인드된 소켓. Drain 중이거나 제거 대기 중이어도 스레드 종료 전까지 유효. */
    Socket        m_hSocket;

    /** 설정 값 — 정보용. */
    std::string   m_strBindIp;
    int           m_iPort;
    std::string   m_strDomain;   // 해당 리스너로 들어오는 요청이 속하는 서비스 도메인 (옵션)
    std::string   m_strService;  // volte / mcptt / system / console (옵션)
    int           m_iThreadCount;

    /** 현재 실행 중인 recv 스레드 수. RemoveListener 가 drain + 0이 될 때까지 대기. */
    std::atomic<int> m_iActiveThreads;

    /** true 로 설정되면 해당 리스너의 recv 스레드는 루프 탈출 후 소켓 정리. */
    std::atomic<bool> m_bDrain;

    /** 기존 전역 m_bIpv6 플래그를 복사해 보유. */
    bool          m_bIpv6;

    /** 부모 stack back-pointer (스레드가 RecvSipMessage 호출 시 필요). */
    CSipStack*    m_pclsStack;

    /** 이 리스너의 recv 스레드들이 서로 경합하지 않도록 per-listener mutex.
     *  전역 m_clsUdpRecvMutex 대신 사용해 다른 리스너의 poll 을 차단하지 않는다. */
    CSipMutex     m_clsRecvMutex;
};

/** 단일 TCP 리스너 (R3).
 *  accept thread 1개가 m_hSocket 을 poll/accept → 받은 connection 을
 *  stack 의 shared m_clsTcpThreadList (worker pool) 로 SendCommand 배분. */
class CSipStackTcpListener
{
public:
    CSipStackTcpListener()
        : m_iId(0),
          m_hSocket(INVALID_SOCKET),
          m_iPort(0),
          m_iActiveThreads(0),
          m_bDrain(false),
          m_bIpv6(false),
          m_pclsStack(NULL)
    {}

    /** CSP 에서 발급한 논리 ID (0 = Start 시 생성된 primary 리스너). */
    int           m_iId;
    Socket        m_hSocket;
    std::string   m_strBindIp;
    int           m_iPort;
    std::atomic<int>  m_iActiveThreads;  // accept thread 1개 기준
    std::atomic<bool> m_bDrain;
    bool          m_bIpv6;
    CSipStack*    m_pclsStack;
};

#ifdef USE_TLS
// forward-declare openssl SSL_CTX 를 최소 의존성으로 잡기 위해 원타입 별칭 사용
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;

/** 단일 TLS 리스너 (R3, R5.c 에서 per-listener cert 확장).
 *  TCP 와 동일 구조, worker pool 은 m_clsTlsThreadList 공유.
 *  m_pSslCtx 가 유효하면 accept 시 per-listener SSL_CTX 사용, NULL 이면 stack-global. */
class CSipStackTlsListener
{
public:
    CSipStackTlsListener()
        : m_iId(0),
          m_hSocket(INVALID_SOCKET),
          m_iPort(0),
          m_iActiveThreads(0),
          m_bDrain(false),
          m_bIpv6(false),
          m_pclsStack(NULL),
          m_pSslCtx(NULL)
    {}

    int           m_iId;
    Socket        m_hSocket;
    std::string   m_strBindIp;
    int           m_iPort;
    std::atomic<int>  m_iActiveThreads;
    std::atomic<bool> m_bDrain;
    bool          m_bIpv6;
    CSipStack*    m_pclsStack;

    // R5.c: per-listener TLS 인증서
    std::string   m_strCertFile;
    std::string   m_strKeyFile;
    std::string   m_strCaCertFile;
    SSL_CTX*      m_pSslCtx;   // NULL → stack-global gpsttServerCtx 사용
};
#endif

#endif // _SIP_STACK_LISTENER_H_
