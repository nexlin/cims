#ifndef __CMP_CLIENT_H__
#define __CMP_CLIENT_H__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
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

    // Returns assigned local IP/Port from CMP — leg 별 전용 포트:
    //   iLocalPort/iLocalVideoPort = peer0(발신 A) leg, iLocalPortB/iLocalVideoPortB = peer1(착신 B) leg.
    //   iRemoteNat/strRemoteSigIp: 해당 peer 의 NAT 목적지 latch 허용 + latch IP guard 기준
    //   (ue_nat_traversal.md §4-5. sig ip 빈 값 = IP guard 없이 latch).
    bool AddSession( const std::string &strSessionId, std::string &strLocalIp, int &iLocalPort, int &iLocalVideoPort,
                     int &iLocalPortB, int &iLocalVideoPortB, const std::string &strRecordDir = "",
                     const std::string &strCaller = "", const std::string &strCallee = "",
                     const std::string &strRmtIp = "", int iRmtPort = 0, int iRmtVideoPort = 0,
                     const std::string &strSesId = "", int iRemoteNat = 0, const std::string &strRemoteSigIp = "" );
    bool ModifySession( const std::string &strSessionId, const std::string &strRmtIp, int iRmtPort, int iRmtVideoPort,
                        int iPeerIdx, const std::string &strCaller = "", const std::string &strCallee = "",
                        const std::string &strSesId = "", int iRemoteNat = 0, const std::string &strRemoteSigIp = "" );
    bool RemoveSession( const std::string &strSessionId, const std::string &strCaller = "",
                        const std::string &strCallee = "", const std::string &strSesId = "" );
    bool Alive();

    // VoIP relay 세션 식별자(csp_{yyyymmddHHMMSSmmm}_{n}) 발행 — 재시작 경계 포함 전역 유일.
    // teardown/MODIFY 가 포트가 아닌 이 유일 키로 CMP 세션을 지목한다.
    static std::string IssueSessionId();

    // 응답: strIp/iFloorPort(그룹 공유 floor) + mapMemberPorts(멤버별 전용 RTP 포트 — sid → {audio, video}).
    bool AddGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                   std::string &strIp, int &iFloorPort, std::map<std::string, std::pair<int, int>> &mapMemberPorts,
                   const std::string &strRecordDir = "", bool bVideoEnabled = false, int iSessionSeq = 0,
                   const std::string &strSesId = "", const std::string &strGroupType = "",
                   const std::string &strInitiator = "" );
    bool ModifyGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                      const std::string &strSesId = "" );
    // 2단 멱등 (docs/api/cmp_media_api.md §7.4): strIp 가 비면 ① 선할당(멤버 포트만 확보),
    //   주소 동반이면 ② 멤버 등록/주소 갱신. piLocalPort/piLocalVideoPort 에 멤버 전용 포트 응답.
    bool JoinGroup( const std::string &strGroupId, const std::string &strSessionId, const std::string &strIp, int iPort,
                    int iFloorPort = 0, int iVideoPort = 0, const std::string &strSesId = "",
                    const std::string &strRole = "participant", int *piLocalPort = NULL, int *piLocalVideoPort = NULL,
                    int iUserNat = 0, const std::string &strUserSigIp = "" );
    bool LeaveGroup( const std::string &strGroupId, const std::string &strSessionId, const std::string &strSesId = "" );
    bool RemoveGroup( const std::string &strGroupId, const std::string &strSesId = "" );

    // 멤버 floor condition tier(2=emergency/1=imminent/0=normal) 런타임 갱신 (TS 24.380).
    //   긴급 개시·업그레이드·취소 시 호출 — 미디어 재협상 없이 floor 우선순위만 변경.
    bool SetFloorTier( const std::string &strGroupId, const std::string &strSessionId, int iTier,
                       const std::string &strSesId = "" );

    /** 세션/그룹별 기 발행된 sesid 조회 (없으면 빈문자열) */
    std::string GetSesIdByKey( const std::string &strKey );

    // Phase 1.E — multi-endpoint dispatch (HA: CMP All Active).
    // 단일 endpoint 운영 시에는 호출 불필요 (Init 가 primary 1개를 자동 등록).
    // 운영 환경에서 csp.json 에 추가 CMP endpoint 가 있으면 main 이 AddEndpoint 호출.
    void AddEndpoint( const std::string &strIp, int iPort );

    /** Session-ID → 선택된 endpoint (consistent hash). 미등록 endpoint 면 primary 반환. */
    CmpEndpoint SelectEndpointForSession( const std::string &strSessionId );

    /** audit 수준2 설정 주입 (CspServer init 에서 gclsSetup 값 전달).
     *  strHaRole: "active"|"auto"→회수 실행, "standby"→탐지·로그만(오회수 방지). */
    void SetAuditConfig( bool bEnable, int iGraceSec, int iMaxPerCycle, bool bZombieTeardown,
                         const std::string &strHaRole );

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
        bool bLogMsg;            // 요청을 msg/flow 에 기록했는가 — 응답 기록 여부 동기화 (HB 샘플링)
        std::condition_variable cv;
        std::mutex mutex;
        bool bCompleted;
        bool bSuccess;
        Transaction() : id( 0 ), bLogMsg( true ), bCompleted( false ), bSuccess( false ) {
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

    // ── audit 수준2 (CSP↔CMP 세션 재조정) ──────────────────────────────────
    //   KeepAliveLoop 이 매 HEARTBEAT(3s) 성공 후 호출. CMP digest(Alive 가 stash)와 CSP CallMap
    //   지문을 대조해 불일치 시에만 SESSION_LIST 로 상세 diff → orphan RemoveSession(회수),
    //   zombie 는 opt-in teardown. active 역할일 때만 회수(standby 는 탐지·로그만).
    void RunAuditCycle();
    // SESSION_LIST 전량 수집(페이지) → id→age_sec (grace 는 회수 시 client-side 적용). primary endpoint.
    bool FetchSessionList( const std::string &strKind, std::map<std::string, int> &mapOut );
    static uint64_t Fnv1a64( const std::string &s );
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

    // HEARTBEAT 로그 샘플링 — 3초 주기 생존 신호의 msg/flow 노이즈 억제 (CMP 측과 동일 규칙).
    //   N 회당 1회만 기록. 응답 기록 여부는 Transaction.bLogMsg 로 요청과 동기화.
    static const unsigned int kHbLogSampleN = 100;  // 3s × 100 ≈ 5분당 1건
    unsigned int m_iHbLogCount = 0;

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
    // 단일 수신 스레드 — 공유 소켓(m_hSocket)에서 양 CMP 응답을 trans_id 로 demux 하여 dispatch.
    //   비동기 로깅(SipMessageLogger) 적용 후 수신 처리는 recvfrom→parse→enqueue→notify 의 µs 급이라
    //   단일 스레드로 충분(다중 스레드는 단일 소켓 수신큐를 공유해 실질 병렬 이득 없음).
    std::thread m_threadRecv;

    // Connection State
    bool m_bConnected;
    // 연속 HEARTBEAT 실패 횟수. 일시적 UDP 타임아웃 1회에 Disconnected 판정 → 활성 PTT
    // 그룹콜 전체 teardown 되던 과민 동작을 막기 위해, 임계(kMaxAliveFail) 연속 실패에서만 disconnect.
    int m_iAliveFailCount;
    std::function<void( bool )> m_fnConnectionCallback;

    // ── audit 수준2 설정/상태 ──
    bool m_bAuditEnable = false;
    int m_iAuditGraceSec = 30;
    int m_iAuditMaxPerCycle = 20;
    bool m_bAuditZombieTeardown = false;
    bool m_bAuditActiveRole = true;        // HaRole 해석 (standby 만 false)
    bool m_bAuditStandbyLogged = false;    // standby 불일치 로그 1회 억제
    // 최근 primary HEARTBEAT 응답의 CMP relay 세션집합 지문 (Alive 가 갱신, RunAuditCycle 이 소비)
    std::mutex m_mutexDigest;
    bool m_bCmpDigestValid = false;
    int m_iCmpRelayCount = 0;
    uint64_t m_uCmpRelayHash = 0;

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
