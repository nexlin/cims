#ifndef __STORE_OP_WRITER_H__
#define __STORE_OP_WRITER_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// 저장 경로(NAS 가능) 연산 전담 worker — 서비스 스레드 무의존(non-blocking) 계약의
//   비-append 축(원자 rewrite·rename·디렉터리 스캔 등 RMW 연산) 공용 구현.
//   append 로그는 스풀 재생이 가능해 ServiceLogWriter.h(2단 writer)를 쓰고, 이쪽은
//   재생 불가능한 연산이라 폴백이 다르다: **상한 있는 큐 + 초과/정체 시 드롭 + 자기보고**
//   — 저장 경로 장애 구간의 기록 유실을 수용하고 서비스 스레드 생존을 우선한다.
//
// 계약:
//   - 생산자(서비스 스레드)는 순수 계산(경로/내용 문자열 조립)만 하고 Enqueue 한다.
//     op 클로저는 값(문자열)과 프로세스 수명 객체만 캡처한다 — worker 는 정지 시 저장
//     경로 op 에 갇혀 있으면 detach 되므로, 소멸 가능한 객체를 캡처하면 안 된다.
//   - worker 스레드 하나가 op 를 FIFO 로 실행한다 — 같은 파일에 대한 RMW 순서가 보존된다.
//     저장 경로 I/O 는 전부 op 안(=worker)에서만 일어난다.
//   - op 는 저장 실패 시 false 를 반환한다(연속 실패 감지 입력). 행(hard mount)은 worker
//     만 갇히고, 생산자의 Enqueue 가 in-flight 정체(StallSec)를 감지한다.
//   - 폴백 판정(정체·큐 포화 드롭·연속 실패)/회복(성공 op + 큐 해소) 전이는 degrade
//     콜백으로 통지한다 — 모듈이 알람(storage_failure 계열) open/close 로 자기보고한다.
//     콜백은 전이 시에만, 생산자 또는 worker 스레드에서 호출된다.

// op — 저장 연산 1건. 실패(쓰기/rename 실패 등) 시 false.
typedef std::function<bool()> StoreOp;

enum EnumSowLogLevel { SOW_LOG_DEBUG = 0, SOW_LOG_INFO = 1, SOW_LOG_ERROR = 2 };
typedef std::function<void( EnumSowLogLevel eLevel, const std::string &strMsg )> SowLogFn;

// 폴백 전이 통지 — bDegraded=true 진입(알람 open), false 회복(알람 close).
struct SowDegradeInfo {
    bool bDegraded;
    std::string strReason;       // 진입 사유 (회복 시 빈 값)
    unsigned long ulDroppedOps;  // 큐 상한/정지로 폐기된 누적 op 수
};
typedef std::function<void( const SowDegradeInfo &clsInfo )> SowDegradeFn;

class CStoreOpWriter {
public:
    CStoreOpWriter() {
    }
    ~CStoreOpWriter() {
        Stop();
    }

    /** worker 기동. iStallSec: op in-flight 가 이 시간을 넘으면 저장소 무응답 판정.
     *  nMaxOps/llMaxBytes: 큐 상한 — droppable op 는 초과 시 즉시 드롭, 필수 op 는
     *  4×nMaxOps 까지 수용 후 드롭(메모리 폭주 방지 최후선).
     *  fnLog/fnDegrade 는 프로세스 수명 객체만 참조해야 한다 (worker detach 대비). */
    void Init( int iStallSec, size_t nMaxOps, long long llMaxBytes, SowLogFn fnLog = SowLogFn(),
               SowDegradeFn fnDegrade = SowDegradeFn() ) {
        if ( m_ctx ) return;
        m_ctx = std::make_shared<Ctx>();
        m_ctx->iStallMs = ( iStallSec > 0 ? iStallSec : 5 ) * 1000;
        m_ctx->nMaxOps = nMaxOps > 0 ? nMaxOps : 20000;
        m_ctx->llMaxBytes = llMaxBytes > 0 ? llMaxBytes : 64LL * 1024 * 1024;
        m_ctx->fnLog = fnLog;
        m_ctx->fnDegrade = fnDegrade;
        m_thread = std::thread( &CStoreOpWriter::WorkerLoop, m_ctx );
    }

    /** op 적재 — 파일 I/O 없이 즉시 반환. nBytes = op 가 안고 있는 데이터 근사(큐 상한 계수).
     *  bDroppable: 상한 초과 시 버려도 되는 op (미디어 패킷 등 — 유실 수용).
     *  반환 false = 드롭됨(계수 반영). 미기동 상태의 op 도 드롭이다. */
    bool Enqueue( StoreOp &&op, size_t nBytes = 0, bool bDroppable = false ) {
        if ( !m_ctx || !m_ctx->bRun.load() ) return false;
        Ctx &ctx = *m_ctx;

        // 정체 감지: worker 가 저장 경로 op 에 StallSec 이상 갇혀 있으면 무응답 판정.
        long long llOpStart = ctx.llOpStartMs.load();
        if ( llOpStart != 0 && NowMs() - llOpStart > ctx.iStallMs ) {
            SetDegraded( ctx, true, "stall: store op in-flight > " + std::to_string( ctx.iStallMs ) + "ms" );
        }

        bool bQueued = false;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            bool bFull = ctx.queue.size() >= ctx.nMaxOps || ctx.llQueueBytes >= ctx.llMaxBytes;
            bool bHardFull = ctx.queue.size() >= ctx.nMaxOps * 4;
            if ( ( bDroppable && bFull ) || bHardFull ) {
                ctx.ulDroppedOps.fetch_add( 1 );
            } else {
                ctx.queue.push_back( { std::move( op ), nBytes } );
                ctx.llQueueBytes += (long long)nBytes;
                bQueued = true;
            }
        }
        if ( bQueued ) {
            ctx.cv.notify_one();
        } else {
            SetDegraded( ctx, true, "backlog: op queue full" );
        }
        return bQueued;
    }

    bool IsDegraded() const {
        return m_ctx && m_ctx->bDegraded.load();
    }
    unsigned long DroppedOps() const {
        return m_ctx ? m_ctx->ulDroppedOps.load() : 0;
    }

    /** 큐 드레인 대기 (시험/정지 보조) — 비거나 타임아웃까지. 반환 true = 드레인 완료. */
    bool Flush( int iTimeoutMs ) {
        if ( !m_ctx ) return true;
        Ctx &ctx = *m_ctx;
        long long llDeadline = NowMs() + iTimeoutMs;
        for ( ;; ) {
            bool bIdle;
            {
                std::lock_guard<std::mutex> lk( ctx.mtx );
                bIdle = ctx.queue.empty() && !ctx.bInOp.load();
            }
            if ( bIdle ) return true;
            if ( NowMs() >= llDeadline ) return false;
            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
        }
    }

    /** 정지 — 저장소가 건강하면 잔여 op 드레인을 잠시 기다리고, worker 가 저장 경로 op 에
     *  갇혀 있으면 detach 한다 (NFS killable 대기라 프로세스 종료가 회수). 잔여 op 는
     *  드롭 계수만 남는다 (재생 불가 연산이라 스풀 회수 대상이 아니다). */
    void Stop() {
        if ( !m_ctx ) return;
        Ctx &ctx = *m_ctx;
        if ( !ctx.bRun.exchange( false ) ) {
            if ( m_thread.joinable() ) m_thread.join();
            return;
        }
        if ( !ctx.bDegraded.load() ) Flush( kSowStopWaitMs );
        ctx.cv.notify_all();
        for ( int i = 0; i < kSowStopWaitMs / 100 && !ctx.bExited.load(); i++ ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }
        if ( m_thread.joinable() ) {
            if ( ctx.bExited.load() ) {
                m_thread.join();
            } else {
                m_thread.detach();
            }
        }
        size_t nRemain;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            nRemain = ctx.queue.size();
            ctx.queue.clear();
            ctx.llQueueBytes = 0;
        }
        if ( nRemain > 0 ) ctx.ulDroppedOps.fetch_add( (unsigned long)nRemain );
    }

private:
    static const int kSowStopWaitMs = 2000;    // 정지 시 드레인/worker 종료 대기 상한
    static const int kSowFailThreshold = 3;    // 연속 op 실패 → 폴백 판정 임계
    static const size_t kSowRecoverDiv = 10;   // 회복 판정: 큐 잔량 < nMaxOps/이 값

    struct Ctx {
        std::mutex mtx;  // queue/llQueueBytes 보호
        std::condition_variable cv;
        std::deque<std::pair<StoreOp, size_t>> queue;
        long long llQueueBytes = 0;
        std::atomic<bool> bRun{ true };
        std::atomic<bool> bExited{ false };
        std::atomic<bool> bInOp{ false };
        std::atomic<long long> llOpStartMs{ 0 };  // 저장 경로 op 시작 시각 (0=idle) — 정체 감지
        std::atomic<int> iConsecFail{ 0 };
        std::atomic<unsigned long> ulDroppedOps{ 0 };
        std::atomic<bool> bDegraded{ false };
        std::mutex transMtx;  // 전이 판정+콜백 직렬화
        int iStallMs = 5000;
        size_t nMaxOps = 20000;
        long long llMaxBytes = 64LL * 1024 * 1024;
        SowLogFn fnLog;
        SowDegradeFn fnDegrade;
    };

    static long long NowMs() {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch() )
            .count();
    }

    static void Log( Ctx &ctx, EnumSowLogLevel eLevel, const std::string &strMsg ) {
        if ( ctx.fnLog ) ctx.fnLog( eLevel, strMsg );
    }

    /** 폴백 상태 전이 — 전이 시에만 로그 + degrade 콜백 (생산자/worker 양쪽에서 호출 가능,
     *  transMtx 로 직렬화). */
    static void SetDegraded( Ctx &ctx, bool bDegraded, const std::string &strReason ) {
        std::lock_guard<std::mutex> lk( ctx.transMtx );
        if ( ctx.bDegraded.load() == bDegraded ) return;
        ctx.bDegraded.store( bDegraded );
        SowDegradeInfo info;
        info.bDegraded = bDegraded;
        info.strReason = strReason;
        info.ulDroppedOps = ctx.ulDroppedOps.load();
        if ( bDegraded ) {
            Log( ctx, SOW_LOG_ERROR,
                 "store op writer degraded (" + strReason + ", dropped=" + std::to_string( info.ulDroppedOps ) + ")" );
        } else {
            Log( ctx, SOW_LOG_INFO,
                 "store op writer recovered (dropped total=" + std::to_string( info.ulDroppedOps ) + ")" );
        }
        if ( ctx.fnDegrade ) ctx.fnDegrade( info );
    }

    static void WorkerLoop( std::shared_ptr<Ctx> pCtx ) {
        Ctx &ctx = *pCtx;
        while ( ctx.bRun.load() ) {
            StoreOp op;
            size_t nBytes = 0;
            {
                std::unique_lock<std::mutex> lk( ctx.mtx );
                ctx.cv.wait_for( lk, std::chrono::milliseconds( 200 ),
                                 [&] { return !ctx.queue.empty() || !ctx.bRun.load(); } );
                if ( !ctx.bRun.load() ) break;
                if ( ctx.queue.empty() ) continue;
                op = std::move( ctx.queue.front().first );
                nBytes = ctx.queue.front().second;
                ctx.queue.pop_front();
                ctx.llQueueBytes -= (long long)nBytes;
                ctx.bInOp.store( true );
            }
            // 저장 경로 I/O — 행이면 여기(worker)만 갇힌다. 생산자 Enqueue 가 정체를 감지한다.
            ctx.llOpStartMs.store( NowMs() );
            bool bOk = false;
            try {
                bOk = op();
            } catch ( ... ) {
                bOk = false;
            }
            ctx.llOpStartMs.store( 0 );
            ctx.bInOp.store( false );

            if ( bOk ) {
                ctx.iConsecFail.store( 0 );
                if ( ctx.bDegraded.load() ) {
                    size_t nRemain;
                    {
                        std::lock_guard<std::mutex> lk( ctx.mtx );
                        nRemain = ctx.queue.size();
                    }
                    if ( nRemain < ctx.nMaxOps / kSowRecoverDiv ) SetDegraded( ctx, false, "" );
                }
            } else {
                if ( ctx.iConsecFail.fetch_add( 1 ) + 1 == kSowFailThreshold ) {
                    SetDegraded( ctx, true, "store op failures (" + std::to_string( kSowFailThreshold ) + " consecutive)" );
                }
            }
        }
        ctx.bExited.store( true );
    }

    std::shared_ptr<Ctx> m_ctx;
    std::thread m_thread;
};

#endif
