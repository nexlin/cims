#ifndef __CMP_SERVER_H__
#define __CMP_SERVER_H__

#include <string>
#include <map>
#include <vector>
#include <iostream>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_set>
#include <sys/epoll.h>
//#include "pbase.h"
#include "pmodule.h"
#include "PRtpRelay.h"
#include "PRtpTap.h"
#include "PRtpMulticast.h"
#include "PPttMemberPort.h"
#include "PMcpttGroup.h"
#include "ServiceLogWriter.h"
#include "SimpleJson.h"

class PCmpServer : public PModule {
public:
    PCmpServer(const std::string& name, const std::string& configFile = "cmp.conf");
    virtual ~PCmpServer();

    bool startServer();
    void stopServer();

    void runControlLoop(); // Main loop for UDP control

protected:
    void handlePacket(char* buf, int len, const std::string& ip, int port);
    void processAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    // 청취 leg — RELAY_TAP_ADD(멱등)/MODIFY(주소·키 갱신)/REMOVE (cmp_media_api.md §6.5, dispatch_center.md §6)
    void processTapAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processTapRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    // Group Management
    void processAddGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processJoinGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    // PTT_FLOOR_TIER {group_id, session_id, tier} — 긴급/임박 floor tier 런타임 갱신(업그레이드/취소)
    void processSetFloorTier(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    // SESSION_LIST {kind:"relay"|"group", offset, limit, min_age_sec} — audit 재조정용 세션 열거(페이지).
    //   min_age_sec 이상 존재한 세션만 반환(신규 setup 중 세션 오회수 방지 = grace). CORE 명령.
    void processSessionList(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    int sendResponse(const std::string& ip, int port, const std::string& msg,
                     const char* caller = "", const char* callee = "");
    // v2 응답 빌더 (envelope: docs/api/cmp_media_api.md). sesid/svc 빈 값이면 hdr 에서 생략.
    int sendOk(const std::string& ip, int port, int transId, const std::string& cmd,
               const std::string& sesid, const std::string& svc,
               const SimpleJson::JsonNode* body = NULL,
               const char* caller = "", const char* callee = "");
    int sendErr(const std::string& ip, int port, int transId, const std::string& cmd,
                const std::string& sesid, const std::string& svc,
                const char* code, const char* reason);

    // ── cmp → CSP 이벤트 push (RELAY_ABORTED/PTT_GROUP_ABORTED) ──────────────────
    //   sweeper 가 유휴 자원을 자체 회수할 때 소유 client(CSP)에 비동기 통지한다
    //   (docs/api/cmp_media_api.md §8). ack = 동일 trans_id 의 type:"response",
    //   미ack 시 1s 간격 최대 5회 재전송(CmdpClient 이벤트 채널과 동형). digest-on-HB
    //   audit 과 상보적 — 이벤트는 회수 즉시 특정 세션을 지목해 지연을 단축하고, audit(pull)은
    //   이벤트 유실·절체까지 커버한다.
    //   호출은 sweeper 가 _mutex 를 놓은 뒤 수행(비재귀 _mutex 데드락 회피) — 상태는 _eventMtx 로 보호.
    void emitEvent(const char* name, const SimpleJson::JsonNode& payload, const std::string& sesid,
                   const std::string& service);
    void retransmitEvents();  // timeoutLoop 이 매 초 호출
    // 발언자 집합 변경 → FLOOR_TALKERS 이벤트 (PMcpttGroup 콜백. 그룹 _mutex 보유 중 호출되므로
    //   PCmpServer::_mutex 를 다시 잡지 않는다 — sesid/service 는 그룹이 실어 보낸다).
    void onFloorTalkers(const std::string& groupId, const char* policy,
                        const std::vector<std::string>& talkers,
                        const std::string& sesid, const std::string& service);
    // HEARTBEAT/STATS 공통 자원 요약 (호출측이 _mutex 보유)
    SimpleJson::JsonNode buildResourceSummary();
    // 세션집합 지문(audit 수준2) — {relay:{count,hash}, group:{count,hash}}. hash=XOR(fnv1a64(id))
    //   (순서무관). CSP 가 자기 CallMap 지문과 3초마다 대조해 불일치 시에만 SESSION_LIST 로 상세 diff.
    //   호출측이 _mutex 보유.
    SimpleJson::JsonNode buildSessionDigest();

    // Resource Management
    void loadConfig();
    void initResourcePool();
    void initPttResourcePool();
    void initPttMemberPool();
    PRtpRelay* allocResource(std::string& rtpIp, int& rtpPort, int& videoPort);
    void freeResource(PRtpRelay* rtp);
    // 청취 leg 풀 — TapStartPort 부터 4포트 블록 × TapPoolSize. freeResource 가 relay 의 tap 을 함께 회수한다.
    void initTapPool();
    PRtpTap* allocTap();
    void freeTap(PRtpTap* tap);
    void freeTapsOf(PRtpRelay* rtp);  // 호출자가 _mutex 보유
    PRtpMulticast* allocPttResource(std::string& rtpIp, int& floorPort);
    void freePttResource(PRtpMulticast* ptt);
    // 멤버 전용 포트 유닛 — (groupId, sessionId) 키로 할당/재사용 (멱등). 호출자가 _mutex 보유.
    PPttMemberPort* ensureMemberUnit(const std::string& groupId, const std::string& sessionId, PMcpttGroup* group);
    void freeMemberUnit(const std::string& groupId, const std::string& sessionId);
    void freeGroupMemberUnits(const std::string& groupId);

private:
    int _udpFd;
    bool _running;
    
    std::map<std::string, PRtpRelay*> _sessions;
    std::map<std::string, PMcpttGroup*> _groups;
    std::map<std::string, std::string> _groupSubId;  // groupId → subid(session_seq)
    std::map<std::string, std::string> _sesidMap;    // sessionId 또는 groupId → sesid (flow 상관용)
    std::map<std::string, std::string> _serviceMap;  // sessionId 또는 groupId → service (payload 계승)
    // 미협상 소스 드롭 전역 누적 — 자원 해제 시 이월 (STATS rtp_src_drop 단조 증가 보장)
    long long _srcDropTotal = 0;
    long long _floorCryptoDropTotal = 0;   // 해제된 그룹의 floor SRTCP 폐기 이월(단조 증가)
    long long _srtpDropTotal = 0;          // 해제된 자원의 미디어 SRTP 폐기 이월(단조 증가)
    PMutex _mutex;

    // ── 이벤트 push 상태 (별도 _eventMtx — sweeper 가 _mutex 보유 중 접근하지 않도록 분리) ──
    std::mutex _eventMtx;         // _cspIp/_cspPort/_pendingEvents/_eventSeq 보호
    std::string _cspIp;           // 이벤트 회신처 — 마지막 제어 요청 소스(CmpClient 소켓 주소)
    int _cspPort = 0;
    struct PendingEvent {
        std::string json;
        int attempts;
        time_t nextAt;
    };
    std::map<long, PendingEvent> _pendingEvents;  // event trans_id → 재전송 상태
    long _eventSeq = 0;           // 이벤트 trans_id 발행 카운터 — 기동 시 ms 시드(재시작 ack 오매칭 방지)

    // sesid 발행 유틸: {caller}::cmp::{us_ts}::{counter}
    static std::string issueSesid(const std::string& caller);

    // CMP flow 로그 (통합 디렉터리: {ServiceLogDir}/YYYY/MM/DD/HH/cmp_01_{service}.flow.jsonl)
    std::string _serviceLogDir;
    std::string _systemId;      // 파일명용 (cmp_01)
    std::string _nodeName;      // flow node 필드용 (cmp)

    // FM 자기보고 (alarm_self_reporting.md) — Fm.{Enable,OamIp,OamPort,SyncSec}
    bool _fmEnable = false;
    std::string _fmOamIp = "127.0.0.1";
    int _fmOamPort = 9010;
    int _fmSyncSec = 60;
    std::thread _fmMonitorThread;
    void fmMonitorLoop();
    // 5분 버킷 + open-per-write (구 시간당 persistent handle → .nfs 고아·데이터유실·대용량검색 해소; CSP SipMessageLogger 와 동일)
    std::string _currentBucketKey;   // {hourDir}/{mm5} — 버킷 회전 감지
    std::string _currentFlowHourDir;
    std::string _currentMsgHourDir;
    int _msgSeq;          // 현재 버킷 _csp.msg 줄 수 (버킷 전환 시 -1 → 다음 write 가 lazy 계수)
    int _lastRxSeq;       // 현재 요청의 원문 seq (logFlow에서 사용)

    // HEARTBEAT 로그 샘플링 (3초 주기 노이즈 억제 — N 회당 1회만 msg/flow 기록).
    //   단일 control 스레드 전제의 per-packet 플래그 (handlePacket 이 설정, sendResponse 가 참조).
    static const int kHbLogSampleN = 100;  // 3s × 100 ≈ 5분당 1건
    unsigned long _hbCount = 0;
    bool _hbLogSuppress = false;

    void logFlow(const std::string& key, const char* from, const char* to,
                 const char* proto, const char* label, const char* detail = "",
                 const char* txId = "", const char* service = "",
                 const char* sesid = "", const char* subid = "",
                 int seq = 0, const char* iface = "",
                 const char* caller = "", const char* callee = "");
    int writeMsgLine(const char* ts, const char* dir, const char* peer, const char* proto, const char* msg,
                     const char* caller = "", const char* callee = "");
    void ensureBucket();             // 버킷 전환 시 seq 리셋 — 순수 북키핑 (파일시스템 무접촉)
    std::string bucketSuffix();      // (tm_min/5)*5 → "00".."55"
    std::string flowFilePath();      // {hourDir}/{systemId}.flow.{mm5}.jsonl
    std::string msgFilePath();       // {hourDir}/{systemId}_csp.msg.{mm5}.jsonl
    std::string getFlowHourDir();
    std::string getMsgHourDir();
    static std::string getTimestamp();

    // ── 서비스 로그 writer — 공용 2단(dispatch + NAS flusher + 로컬 스풀 폴백) ────
    //   생산자(writeMsgLine/logFlow/writeLeakReclaim)는 _logMtx 보유 중 한 줄을
    //   포맷·seq 부여까지만 하고 _logWriter.Enqueue 로 적재 후 즉시 반환 — 파일시스템
    //   무접촉(디렉터리 생성·기존 줄수 계수도 flusher 몫). 저장 경로(NAS) 쓰기 실패/
    //   무응답(StallSec) 시 로컬 스풀(SpoolDir) 폴백 + A-PRC-006 자기보고, 회복 시
    //   순서 보존 재생(replay). 계약·구현: flow_logging.md §2, include/ServiceLogWriter.h.
    std::mutex _logMtx;              // producer 상태(_msgSeq/bucket/dir) + 포맷+enqueue 직렬화
    CServiceLogWriter _logWriter;
    std::string _logSpoolDir = "spool";  // ServiceLogging.SpoolDir — 로컬 디스크 경로여야 한다
    int _logStallSec = 5;                // ServiceLogging.StallSec — 저장 경로 무응답 판정(초)
    int _logSpoolMaxMb = 1024;           // ServiceLogging.SpoolMaxMb — 스풀 용량 상한
    std::string _seedBucketKey;          // 기동 시점 버킷 — 시딩(재기동 seq 연속성) 합류 판정
    void startServiceLogWriter();        // startServer 에서 기동
    void startRecStoreWriter();          // 녹취 op worker(gclsRecStoreWriter) 기동 — A-PRC-017 콜백 구성

    // msg_log body
    std::string _msgLogDir;

    // VoIP Resource Pool
    int _rtpStartPort;
    int _rtpPoolSize;
    std::string _rtpIp;

    // 청취 leg(tap) 풀 (dispatch_center.md §6) — TapStartPort 부터 4포트 블록. 0 = 기능 비활성(resource.tap 미광고)
    int _tapStartPort = 58000;
    int _tapPoolSize = 16;
    int _maxTapsPerSession = 4;   // relay.max_taps — 세션당 상한(초과 LIMIT)
    std::vector<PRtpTap*> _tapPool;
    std::vector<PRtpTap*> _freeTaps;

    // PTT Resource Pool
    int _pttRtpStartPort;     // 멤버 유닛 audio RTP 대역 시작 (stride 2)
    int _pttRtpPoolSize;      // 그룹(floor) 풀 크기
    int _pttFloorStartPort;   // 그룹 floor 대역 시작 (stride 2)
    int _pttVideoStartPort;   // 멤버 유닛 video RTP 대역 시작 (stride 2)
    int _pttMemberPoolSize;   // 멤버 유닛 풀 크기 (동시 참가 멤버 수)

    // Server Config
    std::string _serverIp;
    int _serverPort;

    // DTMF PTT Config
    bool _dtmfPttEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;

    std::string _configFile;

    // VoIP 리소스 (PRtpRelay, 4포트 블록)
    std::vector<PRtpRelay*> _resourcePool;
    std::vector<PRtpRelay*> _freeResources;

    // PTT 그룹 리소스 (PRtpMulticast, 그룹 공유 floor control)
    std::vector<PRtpMulticast*> _pttPool;
    std::vector<PRtpMulticast*> _freePttResources;

    // PTT 멤버 전용 포트 유닛 (PPttMemberPort, audio+video RTP)
    std::vector<PPttMemberPort*> _pttMemberPool;
    std::vector<PPttMemberPort*> _freePttMembers;
    std::map<std::string, PPttMemberPort*> _memberUnits;  // "groupId|sessionId" → unit

    // Worker config
    int _rtpWorkerCount;

    // ── RTP epoll 리액터 ──────────────────────────────────────────────
    // 구: RtpWorker 4개가 1ms period 로 풀 전체(논블로킹 소켓 ~550개)를 1000Hz busy-poll
    //     → 진행 호 0 에도 코어 1개 상시 점유. 이를 이벤트 구동(epoll)으로 교체.
    // 풀 소켓 fd 는 init 때 워커별 epoll 에 1회 등록(소켓은 프로세스 내내 유지, alloc/free 무관).
    // 트래픽 없으면 epoll_wait 블록 → idle CPU ≈ 0. 패킷 도착 시에만 해당 relay 의 proc() 호출.
    struct RtpReactor {
        int epfd = -1;
        std::thread thread;
    };
    std::vector<RtpReactor> _reactors;
    std::atomic<bool> _reactorRunning{false};
    void reactorLoop(int widx);
    void epollAddHandler(int widx, PHandler* h, const std::vector<int>& fds);

    // Recording config
    bool _recordEnable;
    std::string _recordDir;
    int _segmentIntervalSec;  // VoLTE 세그먼트 회전 간격 (초, 기본 60)

    // Session timeout (seconds, 0=disabled)
    int _sessionTimeout;
    // 고아(RTP 무수신) relay 회수 시간(초) — setup 실패/실패호 누수 방지. 활성/홀드 호는 _sessionTimeout 적용.
    int _orphanReclaimSec;
    // PTT floor 타이머 (TS 24.380 §11.1.3, 초 — 0=비활성). 그룹 생성 시 PMcpttGroup 에 주입하고
    //   timeoutLoop 가 1초 주기로 점검한다.
    //   T1(End of RTP media): 마지막 RTP 후 이 시간이 지나면 **발언 완료**로 보고 회수(Revoke 없음).
    //                         규격 기본 4초·최대 6초. 설정 키는 종전대로 `FloorIdleSec`.
    int _floorIdleSec;
    int _floorStopTalkSec;    // T2(Stop talking) — 최대 발언시간, Granted Duration 값. 기본 30초
    int _floorRevokeGraceSec; // T3(Stop talking grace) — Revoke 후 Release 대기 유예. 기본 3초
    int _floorRevokeRetxSec;  // T8(Floor Revoke) — 유예 중 Revoke 재전송 간격. 기본 1초
    int _floorIdleResendSec;  // T7(Floor Idle) — Floor Idle 재송신 간격(0=비활성, 최대 3회)
    int _floorGrantRetxSec;   // T20(Floor Granted) — 큐 승급 화자에게 Granted 재송신 간격. 기본 1초

    // 누수 회수(leak reclaim) 관측 — sweeper 가 고아 relay 를 회수한 누적 카운터(STATS 노출) +
    //   회수 세션 상세를 {ServiceLogDir}/leak_reclaim/YYYY/MM/DD/reclaim.jsonl 에 기록(콘솔 조회용).
    //   reason=orphan_no_rtp(setup 실패/무RTP, _orphanReclaimSec 회수) | hold_timeout(RTP 받았으나 owner 가
    //   REMOVE 미발행 = CSP crash/BYE 누락, _sessionTimeout 회수). RtpMap fix 후 이 카운터 증가=새 버그 신호.
    long _leakReclaimTotal;
    long _leakReclaimOrphan;
    long _leakReclaimHold;
    void writeLeakReclaim( const std::string& sessionId, const std::string& sesid, const std::string& service,
                           const char* reason, int heldSec );

    // Flow 로깅 enable flags (cmp.json: Logging.Flow.{floor,dtmf,rtcp})
    bool _logFlowFloor;   // MCPTT floor control RTCP APP 메시지 기록 여부
    bool _logFlowDtmf;    // RFC 2833/4733 DTMF 이벤트 기록 여부
    bool _logFlowRtcp;    // 일반 RTCP(SR/RR/SDES/BYE) 기록 여부
public:
    bool getLogFlowFloor() const { return _logFlowFloor; }
    bool getLogFlowDtmf()  const { return _logFlowDtmf; }
    bool getLogFlowRtcp()  const { return _logFlowRtcp; }

    // Timeout check thread
    std::thread _timeoutThread;
    void timeoutLoop();
};

#endif // __CMP_SERVER_H__
