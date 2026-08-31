/*
 * CmdpClient — CSP ↔ cmdp(MCData Media Plane) UDP JSON 제어 클라이언트.
 *
 * CmpClient 와 동일한 envelope v2({hdr,payload} — cmp_media_api.md §2)·trans_id 상관·
 * 재전송 패턴의 축소 클론 (단일 endpoint — 멀티 endpoint ring 은 후속). 차이점:
 * cmdp 가 비동기 이벤트(MSRP_MSG_RECEIVED/MSRP_SEND_RESULT/MSRP_ABORTED, type:"event")를
 * 이 소켓으로 push 하므로, RecvLoop 가 event 패킷을 콜백으로 라우팅하고 동일 trans_id 의
 * type:"response" 를 ack 로 회신한다. 명령 정본: mcdata_messaging.md §4.7.
 */

#ifndef __CMDP_CLIENT_H__
#define __CMDP_CLIENT_H__

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "SimpleJson.h"

class CCmdpClient {
public:
    static CCmdpClient &GetInstance();

    bool Init( const std::string &strCmdpIp, int iCmdpPort, int iLocalPort );

    /** cmdp 이벤트 콜백 등록 (RecvLoop 스레드에서 호출됨) */
    void SetEventCallback( std::function<void( const SimpleJson::JsonNode & )> fnCallback ) {
        m_fnEventCallback = fnCallback;
    }
    void SetConnectionCallback( std::function<void( bool )> fnCallback ) {
        m_fnConnectionCallback = fnCallback;
    }
    bool IsConnected() const {
        return m_bConnected;
    }

    /** MSRP 세션 식별자(csp_{yyyymmddHHMMSSmmm}_{n}) 발행 — 재시작 경계 포함 전역 유일.
     *  MSRP URI 세션부로도 쓰인다 (영숫자+밑줄) */
    static std::string IssueSessionId();

    /** 수신(발신 단말→서버) 세션 할당. 성공 시 cmdp 의 a=path 용 msrp_path 반환 */
    bool AddRecvSession( const std::string &strSessionId, const std::string &strCaller, const std::string &strGroupId,
                         const std::string &strRemotePath, long long llMaxSize, std::string &strMsrpPath,
                         const std::string &strSesId = "" );

    /** 송신(서버→수신 단말) 세션 할당 — 저장본(file_id) 재전달 */
    bool AddSendSession( const std::string &strSessionId, const std::string &strFileId, const std::string &strCaller,
                         const std::string &strCallee, const std::string &strContentType, std::string &strMsrpPath,
                         const std::string &strSesId = "" );

    /** 수신자 200 OK answer 의 a=path 전달 (전송 개시 트리거) */
    bool SetRemotePath( const std::string &strSessionId, const std::string &strRemotePath );

    bool RemoveSession( const std::string &strSessionId );

    bool Alive();

private:
    CCmdpClient();
    ~CCmdpClient();

    struct Transaction {
        unsigned int id = 0;
        std::string strResponse;
        std::condition_variable cv;
        std::mutex mutex;
        bool bCompleted = false;
        bool bSuccess = false;
    };

    bool SendRequestAndWait( const SimpleJson::JsonNode &payload, std::string &strResponse );

    void KeepAliveLoop();
    void RecvLoop();
    void OnTransactionComplete( unsigned int transId, bool success, const std::string &response );

    std::string m_strCmdpIp;
    int m_iCmdpPort;
    int m_iLocalPort;
    int m_hSocket;

    std::mutex m_mutexTrans;
    std::map<unsigned int, std::shared_ptr<Transaction>> m_mapTransactions;
    unsigned int m_iNextTransId;

    std::atomic<bool> m_bKeepAliveRunning;
    std::thread m_threadKeepAlive;
    std::atomic<bool> m_bRecvRunning;
    std::thread m_threadRecv;

    // 이벤트는 전용 스레드에서 dispatch — 콜백(fan-out)이 AddSendSession 등 동기 요청을
    // 다시 부르는데, RecvLoop 스레드에서 직접 콜백하면 응답을 받아줄 스레드가 자신이라
    // 자기 데드락(타임아웃)이 된다.
    std::deque<SimpleJson::JsonNode> m_eventQueue;
    std::mutex m_mutexEvents;
    std::condition_variable m_cvEvents;
    std::atomic<bool> m_bEventRunning;
    std::thread m_threadEventDispatch;
    void EventDispatchLoop();

    bool m_bConnected;
    int m_iAliveFailCount;
    std::function<void( bool )> m_fnConnectionCallback;
    std::function<void( const SimpleJson::JsonNode & )> m_fnEventCallback;
};

#define gclsCmdpClient CCmdpClient::GetInstance()

#endif
