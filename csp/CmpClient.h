#ifndef __CMP_CLIENT_H__
#define __CMP_CLIENT_H__

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ConsistentHashRing.h"
#include "CspPttGroup.h"
#include "SimpleJson.h"
#include "SipStackDefine.h"

struct CmpSocket {
    int iSocket;
    bool bInUse;
};

// Phase 1.E (HA — CMP All Active) — endpoint descriptor for multi-endpoint dispatch.
// 단일 endpoint 운영 시에는 m_endpoints 가 1개 element (primary) 만 가짐 → 기존 동작과 동일.
struct CmpEndpoint {
    std::string strIp;
    int iPort;
    std::string strKey;  // "ip:port" — ring 의 key
    CmpEndpoint() : iPort( 0 ) {
    }
    CmpEndpoint( const std::string &ip, int port )
        : strIp( ip ), iPort( port ), strKey( ip + ":" + std::to_string( port ) ) {
    }
};

class CCmpClient {
public:
    static CCmpClient &GetInstance();

    bool Init( const std::string &strCmpIp, int iCmpPort, int iLocalPort );

    // Returns assigned local IP/Port from CMP
    bool AddSession( const std::string &strSessionId, std::string &strLocalIp, int &iLocalPort, int &iLocalVideoPort,
                     const std::string &strRecordDir = "", const std::string &strLogDir = "",
                     const std::string &strCaller = "", const std::string &strCallee = "",
                     const std::string &strRmtIp = "", int iRmtPort = 0, int iRmtVideoPort = 0,
                     const std::string &strSesId = "" );
    bool ModifySession( const std::string &strSessionId, const std::string &strRmtIp, int iRmtPort, int iRmtVideoPort,
                        int iPeerIdx, const std::string &strCaller = "", const std::string &strCallee = "",
                        const std::string &strSesId = "" );
    bool UpdateSession( const std::string &strSessionId, const std::string &strRmtIp, int iRmtPort, int iRmtVideoPort,
                        int iPeerIdx, const std::string &strCaller, const std::string &strCallee,
                        std::string &strLocalIp, int &iLocalPort, const std::string &strSesId = "" );
    bool RemoveSession( const std::string &strSessionId, const std::string &strCaller = "",
                        const std::string &strCallee = "", const std::string &strSesId = "" );
    bool Alive();

    bool AddGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                   std::string &strIp, int &iPort, int &iFloorPort, int &iVideoPort,
                   const std::string &strRecordDir = "", const std::string &strLogDir = "", bool bVideoEnabled = false,
                   int iSessionSeq = 0, const std::string &strSesId = "" );
    bool ModifyGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                      const std::string &strSesId = "" );
    bool JoinGroup( const std::string &strGroupId, const std::string &strSessionId, const std::string &strIp, int iPort,
                    int iFloorPort = 0, int iVideoPort = 0, const std::string &strSesId = "" );
    bool LeaveGroup( const std::string &strGroupId, const std::string &strSessionId, const std::string &strSesId = "" );
    bool RemoveGroup( const std::string &strGroupId, const std::string &strSesId = "" );

    /** 세션/그룹별 기 발행된 sesid 조회 (없으면 빈문자열) */
    std::string GetSesIdByKey( const std::string &strKey );

    // Phase 1.E — multi-endpoint dispatch (HA: CMP All Active).
    // 단일 endpoint 운영 시에는 호출 불필요 (Init 가 primary 1개를 자동 등록).
    // 운영 환경에서 csp.json 에 추가 CMP endpoint 가 있으면 main 이 AddEndpoint 호출.
    void AddEndpoint( const std::string &strIp, int iPort );

    /** Session-ID → 선택된 endpoint (consistent hash). 미등록 endpoint 면 primary 반환. */
    CmpEndpoint SelectEndpointForSession( const std::string &strSessionId );

private:
    CCmpClient();
    ~CCmpClient();

    // Async Request/Response
    struct Transaction {
        unsigned int id;
        std::string strResponse;
        std::string strSesId;    // flow sesid (응답 기록용)
        std::string strService;  // flow service (응답 기록용)
        std::string strCaller;   // 발신 MSISDN (응답 기록용)
        std::string strCallee;   // 착신 MSISDN (응답 기록용)
        std::condition_variable cv;
        std::mutex mutex;
        bool bCompleted;
        bool bSuccess;
        Transaction() : id( 0 ), bCompleted( false ), bSuccess( false ) {
        }
    };

    // Phase 1.E-2 — session sticky multi-endpoint dispatch.
    // strSessionKey 가 비어있으면 primary endpoint (Alive/HEARTBEAT 등). 비어있지 않으면:
    //   1) m_mapSessionToEndpointKey 캐시 hit → 동일 endpoint 유지 (sticky)
    //   2) ring select → 캐시에 기록 후 그 endpoint
    //   3) endpoint 미등록 시 primary fallback
    bool SendRequestAndWait( const std::string &strSessionKey, const SimpleJson::JsonNode &payload,
                             std::string &strResponse );

    // 기존 caller 호환 — primary 만 사용
    bool SendRequestAndWait( const SimpleJson::JsonNode &payload, std::string &strResponse );

    // RemoveSession/RemoveGroup 후 캐시 정리
    void ReleaseEndpointForKey( const std::string &strSessionKey );

    // 내부 — 실제 sendto + recv (endpoint 별)
    bool _SendOnEndpoint( const CmpEndpoint &ep, const SimpleJson::JsonNode &payload, std::string &strResponse );
    CmpEndpoint _ResolveEndpoint( const std::string &strSessionKey );

    // Threads
    void KeepAliveLoop();
    void RecvLoop();
    void OnTransactionComplete( unsigned int transId, bool success, const std::string &response );
    // void OnPacketReceived(const std::string& strPacket, const std::string& strIp, int iPort); // Deprecated

    std::string m_strCmpIp;
    int m_iCmpPort;
    int m_iLocalCmpPort;

    // Single Socket
    int m_hSocket;

    // Transaction Map
    std::mutex m_mutexTrans;
    std::map<unsigned int, std::shared_ptr<Transaction>> m_mapTransactions;
    unsigned int m_iNextTransId;

    // session_id/group_id → sesid 캐시 (Modify/Remove 시 재사용)
    std::mutex m_mutexSesid;
    std::map<std::string, std::string> m_mapKeyToSesid;

    // Phase 1.E — multi-endpoint dispatch (단일 endpoint 시 1개 element 만)
    std::mutex m_mutexEndpoints;
    std::vector<CmpEndpoint> m_endpoints;
    CConsistentHashRing<std::string> m_ring;
    std::map<std::string, std::string> m_mapSessionToEndpointKey;  // sessionId → endpoint key

    // Threads
    std::atomic<bool> m_bKeepAliveRunning;
    std::thread m_threadKeepAlive;

    std::atomic<bool> m_bRecvRunning;
    std::thread m_threadRecv;

    // Connection State
    bool m_bConnected;
    // 연속 HEARTBEAT 실패 횟수. 일시적 UDP 타임아웃 1회에 Disconnected 판정 → 활성 PTT
    // 그룹콜 전체 teardown 되던 과민 동작을 막기 위해, 임계(kMaxAliveFail) 연속 실패에서만 disconnect.
    int  m_iAliveFailCount;
    std::function<void( bool )> m_fnConnectionCallback;

public:
    void SetConnectionCallback( std::function<void( bool )> fnCallback ) {
        m_fnConnectionCallback = fnCallback;
    }
    bool IsConnected() const {
        return m_bConnected;
    }
};

#define gclsCmpClient CCmpClient::GetInstance()

#endif
