#ifndef __CMP_CLIENT_H__
#define __CMP_CLIENT_H__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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

// 멤버 SDP 의 a=fmtp:MCPTT 협상 결과 (TS 24.380 §12.1.2.3) — PTT_JOIN 의
//   queueing/max_priority/granted 로 전달 (docs/api/cmp_media_api.md §7.4).
//   fmtp:MCPTT 부재(레거시 단말) 시 전부 미전송 → CMP 기본 동작 유지.
struct McpttFmtp {
    int iQueueing = -1;    // -1=fmtp 부재(미전송) / 0=mc_queueing 미협상(비선점 요청 Deny #1) / 1=협상
    int iMaxPriority = 0;  // mc_priority=N — 요청 가능 최대 우선순위 (0=미전송: 요청의 우선순위 필드 무시)
    int iGranted = 0;      // 1=mc_granted 협상 — 참가 시 발언자 없으면 초기 발언권 (0=미전송)
    int iNoFloorCtrl = 0;  // 1=mc_no_floor_ctrl 협상 — floor 없는 세션 제안(G17). private call 의
                           //   floor_control:"off"(full-duplex) 판정 입력 — PTT_JOIN 필드 아님
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
    //   iRemotePt/iRemoteTePt: 이 leg 가 수신 선언한 audio/TE PT(CMP egress 스탬프),
    //   iRemoteSrcPt/iRemoteSrcTePt: 이 leg 가 송신에 쓰는 PT(CMP ingress 분류·녹취 메타). 0=재작성 없음.
    //   strRemoteCodec: 협상 오디오 코덱("AMR-WB/16000") — 녹취 세그먼트 메타용.
    bool AddSession( const std::string &strSessionId, std::string &strLocalIp, int &iLocalPort, int &iLocalVideoPort,
                     int &iLocalPortB, int &iLocalVideoPortB, const std::string &strRecordDir = "",
                     const std::string &strCaller = "", const std::string &strCallee = "",
                     const std::string &strRmtIp = "", int iRmtPort = 0, int iRmtVideoPort = 0,
                     const std::string &strSesId = "", int iRemoteNat = 0, const std::string &strRemoteSigIp = "",
                     int iRemotePt = 0, int iRemoteSrcPt = 0, int iRemoteTePt = 0, int iRemoteSrcTePt = 0,
                     const std::string &strRemoteCodec = "" );
    bool ModifySession( const std::string &strSessionId, const std::string &strRmtIp, int iRmtPort, int iRmtVideoPort,
                        int iPeerIdx, const std::string &strCaller = "", const std::string &strCallee = "",
                        const std::string &strSesId = "", int iRemoteNat = 0, const std::string &strRemoteSigIp = "",
                        int iRemotePt = 0, int iRemoteSrcPt = 0, int iRemoteTePt = 0, int iRemoteSrcTePt = 0,
                        const std::string &strRemoteCodec = "" );
    bool RemoveSession( const std::string &strSessionId, const std::string &strCaller = "",
                        const std::string &strCallee = "", const std::string &strSesId = "" );

    // VoIP relay 세션 식별자(csp_{yyyymmddHHMMSSmmm}_{n}) 발행 — 재시작 경계 포함 전역 유일.
    // teardown/MODIFY 가 포트가 아닌 이 유일 키로 CMP 세션을 지목한다.
    static std::string IssueSessionId();

    // 응답: strIp/iFloorPort(그룹 공유 floor) + mapMemberPorts(멤버별 전용 RTP 포트 — sid → {audio, video}).
    //   strFloorPolicy/iMaxTalkers: 동시 발언 정책 (docs/api/cmp_media_api.md §7.7). 비면 미전송(CMP 기본 single).
    //   strFloorControl: ""(미전송=on)/"off" — off=full-duplex, 응답에 floor_port 생략 (private call 전용).
    //   strSessionDir: 세션 디렉터리 이름 S{ts}_{n} — CMP 는 record_dir/{시간버킷}/{이 이름}/ 에
    //     세그먼트·floor 를 기록한다. 세션이 곧 기록 단위라, 같은 시간대의 다음 통화가 앞
    //     통화에 섞이지 않는 근거 (docs/design/features/recording.md §3.1).
    bool AddGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                   std::string &strIp, int &iFloorPort, std::map<std::string, std::pair<int, int>> &mapMemberPorts,
                   const std::string &strRecordDir = "", bool bVideoEnabled = false, int iSessionSeq = 0,
                   const std::string &strSesId = "", const std::string &strGroupType = "",
                   const std::string &strInitiator = "", const std::string &strFloorPolicy = "", int iMaxTalkers = 0,
                   const std::string &strFloorControl = "", const std::string &strSessionDir = "" );
    // 정책 변경도 MODIFY 로 전달한다 (생성=ADD 1회, 이후 모든 상태 변경=MODIFY — 계약 §A.0).
    bool ModifyGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                      const std::string &strSesId = "", const std::string &strFloorPolicy = "", int iMaxTalkers = 0 );
    // 2단 멱등 (docs/api/cmp_media_api.md §7.4): strIp 가 비면 ① 선할당(멤버 포트만 확보),
    //   주소 동반이면 ② 멤버 등록/주소 갱신. piLocalPort/piLocalVideoPort 에 멤버 전용 포트 응답.
    //   iUserPt/iUserTePt: 이 leg 가 수신 선언한 audio/TE PT(CMP egress 스탬프),
    //   iUserSrcPt/iUserSrcTePt: 이 leg 가 송신에 쓰는 PT(CMP ingress 분류·녹취 메타). 0=재작성 없음.
    //   strUserCodec: 협상 오디오 코덱("AMR-WB/16000") — 녹취 세그먼트 메타용.
    //   clsFmtp: 멤버 SDP 의 a=fmtp:MCPTT 협상 결과 (queueing/max_priority/granted).
    bool JoinGroup( const std::string &strGroupId, const std::string &strSessionId, const std::string &strIp, int iPort,
                    int iFloorPort = 0, int iVideoPort = 0, const std::string &strSesId = "",
                    const std::string &strRole = "participant", int *piLocalPort = NULL, int *piLocalVideoPort = NULL,
                    int iUserNat = 0, const std::string &strUserSigIp = "", int iUserPt = 0, int iUserSrcPt = 0,
                    int iUserTePt = 0, int iUserSrcTePt = 0, const std::string &strUserCodec = "",
                    const McpttFmtp &clsFmtp = McpttFmtp() );
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
     *  strHaRole: "active"|"auto"→회수 실행, "standby"→탐지·로그만(오회수 방지).
     *  strHaVip: HaRole=auto 에서 이 VIP 를 로컬 소유하면 active, 아니면 standby 로 동적 판정
     *  (빈 값이면 auto=active — 단일노드/cold-spare). hot-standby 승격(VIP 이전) 자동 반영. */
    void SetAuditConfig( bool bEnable, int iGraceSec, int iMaxPerCycle, bool bZombieTeardown,
                         const std::string &strHaRole, const std::string &strHaVip = "" );

    // Phase 1.E-3 — endpoint DEAD 전이 시 그 endpoint 로 pin 된 세션/그룹 key 목록을
    //   수집하고 sticky 캐시에서 제거해 반환한다. 호출측(CSP)이 이 key 들에 해당하는
    //   VoLTE/PTT 호를 능동 teardown(BYE) 한다. 반환 key: VoLTE=relay session_id(cmp_sess_N),
    //   PTT=group_id.
    std::vector<std::string> TakeSessionKeysForEndpoint( const std::string &strKey );

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

    // per-endpoint 프로브 (KeepAliveLoop 내부용) — HEARTBEAT 한 번으로 liveness + 자원 요약을
    //   얻는다 (응답 payload 의 resource — cmp_media_api.md). iFreePorts = 가용 RTP 포트
    //   (VoIP relay + PTT 풀 합, 파싱 불가 시 -1). 응답의 session_digest 는 audit 수준2 용으로
    //   m_mapEndpointDigest 에 stash 한다 (구 Alive() 역할 흡수).
    bool _ProbeAlive( const CmpEndpoint &ep, int &iFreePorts );

    // Threads
    void KeepAliveLoop();
    void RecvLoop();
    void OnTransactionComplete( unsigned int transId, bool success, const std::string &response );

    // ── cmp → CSP 이벤트 (RELAY_ABORTED/PTT_GROUP_ABORTED, type:"event") ──────────
    //   CMP sweeper 가 유휴 자원을 자체 회수할 때 push. RecvLoop 가 event 를 ack(동일 trans_id
    //   response)한 뒤 큐에 넣고, 별도 EventDispatchLoop 스레드가 콜백을 처리한다 — RELAY_ABORTED
    //   핸들러가 RemoveSession(→SendRequestAndWait)을 부르면 RecvLoop 스레드에서 직접 처리 시
    //   자기 응답을 자기가 기다려 데드락되므로 반드시 분리한다(CmdpClient 와 동일 근거).
    //   audit(pull)과 상보적 — 이벤트는 회수 즉시 특정 세션을 지목해 수렴 지연을 단축한다.
    void EventDispatchLoop();
    void HandleEvent( const SimpleJson::JsonNode &event );

    // ── audit 수준2 (CSP↔CMP 세션 재조정) ──────────────────────────────────
    //   KeepAliveLoop 이 매 HEARTBEAT(3s) 성공 후 호출. Alive 가 endpoint 별 digest 를 stash 하면
    //   RunAuditCycle 이 CSP CallMap 세션을 담당 endpoint(consistent hash)로 분할해 샤드별로 CMP
    //   digest 와 대조 → 불일치 샤드만 SESSION_LIST diff → orphan RemoveSession(회수), zombie 는
    //   opt-in teardown. active 역할일 때만 회수(standby 는 탐지·로그만).
    void RunAuditCycle();
    // 한 CMP endpoint(샤드) 재조정 — 해당 endpoint 에서 SESSION_LIST diff 후 orphan 회수/zombie 판정.
    //   setShardCsp = 그 endpoint 로 해시되는 CSP 세션집합.
    void ReconcileShard( const CmpEndpoint &ep, const std::set<std::string> &setShardCsp );
    // SESSION_LIST 전량 수집(페이지) → id→age_sec (grace 는 회수 시 client-side 적용). 지정 endpoint.
    bool FetchSessionList( const CmpEndpoint &ep, const std::string &strKind, std::map<std::string, int> &mapOut );
    static uint64_t Fnv1a64( const std::string &s );

    // ── HaRole 동적 판정 (task 2a) — HaRole=auto + HaVip 설정 시 VIP 소유로 active/standby 결정 ──
    //   매 audit cycle 재평가 → hot-standby 승격(VIP 이전) 시 별도 훅 없이 3s 내 active 로 전환.
    //   HaVip 미설정(단일노드/cold-spare)이면 auto=active. 정적 active/standby 는 그대로 override.
    void ResolveActiveRole();
    static bool OwnsLocalIp( const std::string &strIp );  // getifaddrs 로 로컬 인터페이스 소유 확인
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

    // Phase 1.E-3 — per-endpoint 헬스 상태 (KeepAliveLoop 가 갱신, m_mutexEndpoints 보호).
    //   bLive=HEARTBEAT 응답(liveness, 집계 m_bConnected·DEAD teardown 기준),
    //   bSaturated=STATS 가용 RTP 포트 0(신규 배정만 ring 제외, 기존 세션은 유지).
    struct EndpointHealth {
        int iFailCount = 0;
        bool bLive = true;
        bool bSaturated = false;
    };
    std::map<std::string, EndpointHealth> m_mapEndpointHealth;  // endpoint key → health

    // Threads
    std::atomic<bool> m_bKeepAliveRunning;
    std::thread m_threadKeepAlive;

    std::atomic<bool> m_bRecvRunning;
    // 단일 수신 스레드 — 공유 소켓(m_hSocket)에서 양 CMP 응답을 trans_id 로 demux 하여 dispatch.
    //   비동기 로깅(SipMessageLogger) 적용 후 수신 처리는 recvfrom→parse→enqueue→notify 의 µs 급이라
    //   단일 스레드로 충분(다중 스레드는 단일 소켓 수신큐를 공유해 실질 병렬 이득 없음).
    std::thread m_threadRecv;

    // 이벤트 dispatch 전용 스레드 — RecvLoop 가 큐에 넣고 여기서 콜백(HandleEvent)을 부른다.
    //   응답(RemoveSession 등)은 RecvLoop 가 정상 수신하므로 데드락이 없다.
    std::deque<SimpleJson::JsonNode> m_eventQueue;
    std::mutex m_mutexEvents;
    std::condition_variable m_cvEvents;
    std::atomic<bool> m_bEventRunning;
    std::thread m_threadEventDispatch;

    // Connection State
    bool m_bConnected;
    // 연속 HEARTBEAT 실패 횟수. 일시적 UDP 타임아웃 1회에 Disconnected 판정 → 활성 PTT
    // 그룹콜 전체 teardown 되던 과민 동작을 막기 위해, 임계(kMaxAliveFail) 연속 실패에서만 disconnect.
    int m_iAliveFailCount;
    std::function<void( bool )> m_fnConnectionCallback;
    // endpoint 가 DEAD 로 전이하면 그 endpoint 로 pin 되어 있던 세션/그룹 key 들을 콜백한다
    //   (CSP 가 해당 VoLTE/PTT 호를 BYE 로 정리). 부분 장애(일부 endpoint down) 전용 — 전체
    //   media 평면 down 은 기존 m_fnConnectionCallback(false) 경로.
    std::function<void( const std::vector<std::string> & )> m_fnEndpointDownCallback;

    // ── audit 수준2 설정/상태 ──
    bool m_bAuditEnable = false;
    int m_iAuditGraceSec = 30;
    int m_iAuditMaxPerCycle = 20;
    bool m_bAuditZombieTeardown = false;
    std::string m_strHaRole = "auto";              // active|standby|auto (auto+HaVip 시 VIP 소유로 동적 판정)
    std::string m_strHaVip;                        // 감시할 VIP(예: VIP_csp). 소유 시 active — HaRole=auto 에서만 사용
    std::atomic<bool> m_bAuditActiveRole{ true };  // 매 cycle ResolveActiveRole 이 갱신 (event 핸들러도 참조)
    bool m_bLastActiveRole = true;                 // 역할 전환 로그용 직전값
    bool m_bAuditStandbyLogged = false;            // standby 불일치 로그 1회 억제
    // endpoint 별 CMP relay 세션집합 지문 (Alive 가 endpoint 마다 stash, RunAuditCycle 이 샤드별 소비)
    struct CmpDigest {
        bool bValid = false;
        int iCount = 0;
        uint64_t uHash = 0;
    };
    std::mutex m_mutexDigest;
    std::map<std::string, CmpDigest> m_mapEndpointDigest;  // endpoint key("ip:port") → relay digest

public:
    void SetConnectionCallback( std::function<void( bool )> fnCallback ) {
        m_fnConnectionCallback = fnCallback;
    }
    void SetEndpointDownCallback( std::function<void( const std::vector<std::string> & )> fnCallback ) {
        m_fnEndpointDownCallback = fnCallback;
    }
    bool IsConnected() const {
        return m_bConnected;
    }
};

#define gclsCmpClient CCmpClient::GetInstance()

#endif
