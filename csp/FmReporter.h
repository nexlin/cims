#ifndef __FM_REPORTER_H__
#define __FM_REPORTER_H__

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "SimpleJson.h"

// 모듈 자기보고(FM push) 클라이언트 — docs/design/alarm_self_reporting.md.
//   OAM FM ingest 로 UDP JSON envelope v2 의 FM_REGISTER/FM_ALARM/FM_EVENT/FM_SYNC 를
//   push 한다. 활성 알람의 SoT 는 이쪽(모듈)이고 OAM 은 미러 — 통지 유실·재기동은
//   주기 FM_SYNC 가 수렴시킨다.
//   신뢰성: 전 메시지 type:"event" — ack(동일 trans_id 의 response) 미수신 시 1s 간격
//   최대 5회 재전송 (cmp/cmdp 이벤트 채널과 동일 정책, cmp_media_api.md §8).
//   ERROR code=UNREGISTERED 수신 시 FM_REGISTER 부터 다시 (OAM 재기동/최초 접속).

// 알람 발생 인스턴스 — active 목록(FM_SYNC)의 단위.
struct FmActiveAlarm {
    std::string strCode;              // 알람 클래스 코드 (fm_catalog.json 의 code)
    std::string strMo;                // managedObjectInstance (예: cims/csp/db)
    std::string strOpenTs;            // 발생 시각 (ISO8601)
    SimpleJson::JsonNode nodeParams;  // msg 템플릿 치환 값
};

class CFmReporter {
public:
    static CFmReporter &GetInstance();

    // strCatalogFile: fm_catalog.json 경로 — 등록(FM_REGISTER) payload 로 push 된다.
    // strModule: FM_REGISTER 의 module 필드 (csp/cmp/pmp/… — 변종은 자기 이름).
    bool Init( const std::string &strOamIp, int iOamPort, const std::string &strNode, const std::string &strModule,
               const std::string &strCatalogFile, int iSyncSec );
    void Stop();
    bool IsEnabled() const {
        return m_bRunning;
    }

    // 알람 open/close — (code, mo) 가 활성키. 이미 같은 상태면 no-op (전이 시에만 통지).
    void AlarmOpen( const std::string &strCode, const std::string &strMo );
    void AlarmOpen( const std::string &strCode, const std::string &strMo, const SimpleJson::JsonNode &nodeParams );
    void AlarmClose( const std::string &strCode, const std::string &strMo );

    // 정상 동작 이벤트 (stateChange/audit) — 활성 상태 없음, best-effort (재전송 1s×5).
    void SendEvent( const std::string &strType, const std::string &strKind, const std::string &strMo );

private:
    CFmReporter();
    ~CFmReporter();

    void RecvLoop();   // ack/response 수신 (SO_RCVTIMEO 1s)
    void TimerLoop();  // 1s tick — pending 재전송·미등록 재시도·FM_SYNC 주기 송신

    void SendRegister();
    void SendSync();
    void FlushQueuedEvents();
    unsigned int SendFm( const std::string &strCmd, SimpleJson::JsonNode &nodePayload );
    void HandleResponse( const SimpleJson::JsonNode &nodeMsg );

    struct Pending {
        std::string strPacket;
        std::string strCmd;
        int iAttempts;
        time_t tNextSend;
    };

    std::string m_strOamIp;
    int m_iOamPort;
    std::string m_strNode;               // hdr.node — 논리 노드 ID (SystemId)
    std::string m_strModule;             // FM_REGISTER module 필드
    int m_iSyncSec;                      // FM_SYNC 주기 (FM_REGISTER 응답의 sync_interval_sec 가 덮음)
    long long m_llBootId;                // 기동 epoch — OAM 의 재기동 감지 키
    SimpleJson::JsonNode m_nodeCatalog;  // fm_catalog.json 파싱 결과

    int m_hSocket;
    std::atomic<bool> m_bRunning;
    std::atomic<bool> m_bRegistered;
    std::thread m_threadRecv;
    std::thread m_threadTimer;

    std::mutex m_mutex;                                // m_mapActive/m_mapPending/m_iNextTransId/m_iNextSeq 보호
    std::map<std::string, FmActiveAlarm> m_mapActive;  // akey(code@mo) → active
    std::map<unsigned int, Pending> m_mapPending;      // trans_id → 재전송 대기
    std::deque<SimpleJson::JsonNode> m_queueEvents;    // 미등록 구간 이벤트 버퍼 (등록 시 flush)
    unsigned int m_iNextTransId;
    unsigned int m_iNextSeq;  // boot 당 단조증가 — OAM 의 UDP 역전 방어 입력
    time_t m_tLastRegister;
    time_t m_tLastSync;
};

#define gclsFmReporter CFmReporter::GetInstance()

#endif
