#ifndef __SERVICE_LOG_WRITER_H__
#define __SERVICE_LOG_WRITER_H__

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// 서비스 로그(flow/msg jsonl) 저장 경로 무의존(non-blocking) 2단 writer —
//   docs/design/features/flow_logging.md §2 스풀 폴백 계약의 공용 구현.
//   전 모듈(cmp/cmdp/…) 공용 header-only — 모듈별 로거(PLog/CLog)와 알람 보고(FmReporter)
//   차이는 Init 의 콜백 주입으로 흡수한다 (FmReporter.h 와 동일 방식).
//
// 계약 — 저장 경로(Dir)가 NAS(NFS hard mount)여도 서비스 스레드는 막히지 않는다:
//   - 생산자는 파일시스템 호출 없이 Enqueue(경로+포맷 완료 한 줄)만 한다.
//   - dispatch 스레드가 큐를 소비해 목적지를 정한다. 저장소가 건강하면 NAS flusher 큐로,
//     아니면 로컬 스풀(SpoolDir)에 기록한다 — dispatch 도 저장 경로를 만지지 않는다.
//   - NAS flusher 스레드만 저장 경로 I/O 를 수행한다. 쓰기 실패(fail-fast)·in-flight 정체
//     (StallSec 초과, hard mount 행)·flusher 큐 포화 시 폴백이 걸리고, 회복되면 스풀을
//     경로별 줄 순서대로 재생(replay)한 뒤 직행으로 복귀한다 (seq↔줄번호 정합 보존).
//   - 폴백 진입/회복 전이는 degrade 콜백으로 통지한다 — 모듈이 A-PRC-006 storage_failure
//     알람 open/close 로 자기보고한다 (콜백은 dispatch 스레드에서 전이 시에만 호출).
//   - 시딩: flusher 가 기동 직후 지정 파일들의 기존 줄 수(+스풀 잔량)를 비동기 계수한다 —
//     생산자가 SeedDone()/SeedCount() 로 합류해 재기동 후 seq 연속성을 잇는다.

// 로그 레벨 — 콜백이 모듈 로거의 레벨로 매핑한다.
enum EnumSlwLogLevel { SLW_LOG_DEBUG = 0, SLW_LOG_INFO = 1, SLW_LOG_ERROR = 2 };
typedef std::function<void( EnumSlwLogLevel eLevel, const std::string &strMsg )> SlwLogFn;

// 폴백 전이 통지 — bDegraded=true 진입(알람 open), false 회복(알람 close).
struct SlwDegradeInfo {
    bool bDegraded;
    std::string strReason;          // 마지막 실패 사유 (빈 값 = "spool backlog")
    unsigned long ulSpooledLines;   // 폴백으로 스풀에 적재된 누적 줄 수
    unsigned long ulReplayedLines;  // 스풀→저장 경로 재생 완료 누적 줄 수
    unsigned long ulDroppedLines;   // 스풀 기록 실패/용량 폐기 누적 줄 수
};
typedef std::function<void( const SlwDegradeInfo &clsInfo )> SlwDegradeFn;

class CServiceLogWriter {
public:
    CServiceLogWriter() : m_bWriterRunning( false ), m_ulDroppedLogs( 0 ), m_bDegraded( false ), m_llLastSpoolTrimMs( 0 ) {
    }

    ~CServiceLogWriter() {
        Stop();
    }

    /** dispatch + NAS flusher 기동.
     *  strSpoolDir: 저장 경로 무응답 시 로컬 스풀 루트 (로컬 디스크 경로여야 한다).
     *  iStallSec: flusher in-flight 가 이 시간을 넘으면 저장소 무응답 판정.
     *  iSpoolMaxMb: 스풀 용량 상한 — 초과 시 오래된 스풀 파일부터 폐기(계수).
     *  vecBaseDirs: flusher 가 기동 시 보장(MkdirP)할 저장 경로 base 디렉터리.
     *  vecSeedPaths: 기동 시점 버킷의 msg 파일들 — 기존 줄 수를 비동기 계수해 SeedCount 로 노출. */
    void Init( const std::string &strSpoolDir, int iStallSec, int iSpoolMaxMb,
               const std::vector<std::string> &vecBaseDirs, const std::vector<std::string> &vecSeedPaths,
               SlwLogFn fnLog = SlwLogFn(), SlwDegradeFn fnDegrade = SlwDegradeFn() ) {
        if ( m_ctx ) return;
        m_fnLog = fnLog;
        m_fnDegrade = fnDegrade;

        // 저장 경로(NAS 가능) I/O 는 전부 flusher 스레드로 — 여기서는 로컬 스풀만 만진다.
        m_ctx = std::make_shared<StoreCtx>();
        m_ctx->strSpoolDir = strSpoolDir.empty() ? "spool" : strSpoolDir;
        m_ctx->vecBaseDirs = vecBaseDirs;
        m_ctx->vecSeedPaths = vecSeedPaths;
        m_ctx->vecSeedCounts.assign( vecSeedPaths.size(), 0 );
        m_ctx->iStallMs = ( iStallSec > 0 ? iStallSec : 5 ) * 1000;
        m_ctx->llSpoolMaxBytes = (long long)( iSpoolMaxMb > 0 ? iSpoolMaxMb : 1024 ) * 1024 * 1024;
        MkdirP( m_ctx->strSpoolDir );

        // 이전 run 스풀 잔량 스캔 (로컬) — 잔량이 있으면 드레인 전 직행 금지 (경로별 줄 순서 보존)
        long long llBytes = 0;
        bool bPending = false;
        ScanSpool( m_ctx->strSpoolDir, [&]( const std::string &, time_t, long long llSize ) {
            llBytes += llSize;
            bPending = true;
        } );
        m_ctx->llSpoolBytes.store( llBytes );
        m_ctx->bSpoolPending.store( bPending );

        m_bWriterRunning.store( true );
        m_writerThread = std::thread( &CServiceLogWriter::WriterLoop, this );
        m_nasThread = std::thread( &CServiceLogWriter::NasFlusherLoop, m_ctx );
    }

    /** 포맷 완료된 한 줄을 파일경로와 함께 큐에 적재 — 파일 I/O 없이 즉시 반환.
     *  생산자가 자기 락을 보유한 채 호출하면 enqueue 순서=seq 순서가 보존된다. */
    void Enqueue( const std::string &strPath, std::string &&strLine ) {
        if ( strPath.empty() || strLine.empty() || !m_bWriterRunning.load() ) return;
        bool bNotify = false;
        {
            std::lock_guard<std::mutex> lk( m_qMtx );
            if ( m_logQueue.size() >= kSlwMaxQueue ) {
                // backlog 상한 초과(로컬 스풀까지 막힌 극단 상황) — 가장 오래된 줄을 버려 메모리 폭주 방지.
                m_logQueue.pop_front();
                m_ulDroppedLogs.fetch_add( 1 );
            }
            m_logQueue.push_back( LogItem{ strPath, std::move( strLine ) } );
            if ( m_logQueue.size() >= kSlwNotifyThreshold ) bNotify = true;
        }
        if ( bNotify ) m_qCv.notify_one();
    }

    /** flusher 의 기동 시딩(기존 줄 계수) 완료 여부 — 생산자가 seq 합류 시점 판단에 사용. */
    bool SeedDone() const {
        return m_ctx && m_ctx->bSeedDone.load();
    }
    /** vecSeedPaths[i] 파일의 기동 시점 줄 수 (저장 경로 + 스풀 잔량). SeedDone 후 유효. */
    long long SeedCount( size_t i ) const {
        if ( !m_ctx || i >= m_ctx->vecSeedCounts.size() ) return 0;
        return m_ctx->vecSeedCounts[i];
    }
    /** 큐 상한 초과로 버려진 줄 수 (STATS 노출용). */
    unsigned long DroppedQueueLines() const {
        return m_ulDroppedLogs.load();
    }
    /** 현재 폴백(스풀 우회) 상태 여부. */
    bool IsDegraded() const {
        return m_ctx && ( !m_ctx->bNasHealthy.load() || m_ctx->bSpoolPending.load() );
    }

    /** 정지 — 잔여 큐 스풀 회수 후 dispatch 조인. flusher 는 저장 경로 op 에 갇혀 있으면
     *  detach 한다 (NFS killable 대기라 프로세스 종료가 회수). 재기동 시 스풀이 재생된다. */
    void Stop() {
        if ( !m_bWriterRunning.exchange( false ) ) return;
        m_qCv.notify_all();
        if ( m_writerThread.joinable() ) m_writerThread.join();  // dispatch 는 저장 경로 무접촉 — 항상 join 가능

        // 저장소가 건강하면 flusher 큐 잔량이 저장 경로로 나가도록 잠시 기다린다 (정상 종료 시
        //   마지막 줄까지 직행 — 스풀 잔존을 남기지 않는다). 죽은 저장소는 기다리지 않는다.
        StoreCtx &ctx = *m_ctx;
        long long llDrainDeadline = NowMs() + kSlwStopFlusherWaitMs;
        while ( NowMs() < llDrainDeadline && ctx.bNasHealthy.load() && !ctx.bSpoolPending.load() ) {
            bool bIdle;
            {
                std::lock_guard<std::mutex> lk( ctx.mtx );
                bIdle = ctx.nasQueue.empty() && ctx.inflight.empty();
            }
            if ( bIdle ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }

        ctx.bRun.store( false );
        ctx.cv.notify_all();
        for ( int i = 0; i < kSlwStopFlusherWaitMs / 100 && !ctx.bExited.load(); i++ ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }
        if ( m_nasThread.joinable() ) {
            if ( ctx.bExited.load() ) {
                m_nasThread.join();
            } else {
                m_nasThread.detach();
            }
        }

        // 미기록 잔량(nasQueue + inflight) 스풀 회수 — 다음 기동의 replay 가 저장 경로로 밀어넣는다.
        //   (inflight 는 갇힌 op 가 나중에 완료되면 중복 기록될 수 있다 — at-least-once 수용.)
        std::deque<std::deque<LogItem>> remains;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            remains.swap( ctx.nasQueue );
            if ( !ctx.inflight.empty() && !ctx.bExited.load() ) remains.push_front( std::move( ctx.inflight ) );
            ctx.inflight.clear();
        }
        for ( auto &batch : remains ) SpoolBatch( batch );
    }

private:
    // 튜닝값
    static const size_t kSlwNotifyThreshold = 128;  // 큐가 이만큼 쌓이면 즉시 flush 깨움
    static const size_t kSlwMaxQueue = 200000;      // 큐 상한 (스풀까지 막힌 극단 상황의 메모리 폭주 방지)
    static const int kSlwFlushIntervalMs = 100;     // 주기 flush (버퍼 잔여분 보장)
    static const size_t kSlwNasQueueMax = 8;        // dispatch→flusher 대기 배치 상한 (포화 = 저장 경로 지연 신호)
    static const int kSlwReplayRetryMs = 2000;      // 스풀 재생 실패 후 재시도 간격
    static const int kSlwSpoolTrimIntervalMs = 5000;  // 스풀 용량 정리 최소 간격
    static const int kSlwStopFlusherWaitMs = 2000;    // 정지 시 flusher 종료 대기 상한 (초과 시 detach)

    /** 비동기 writer 큐 항목: 목적 파일 경로 + 포맷 완료된 한 줄(개행 포함) */
    struct LogItem {
        std::string path;
        std::string line;
    };

    /** NAS flusher 스레드와 dispatch 가 공유하는 상태.
     *  flusher 는 종료 시 저장 경로 I/O 에 갇혀 join 불가할 수 있어 detach 되므로,
     *  writer 멤버가 아니라 shared_ptr 로 수명을 분리한다 — flusher 는 이 구조체만 만지고
     *  CServiceLogWriter 본체는 절대 참조하지 않는다. */
    struct StoreCtx {
        std::mutex mtx;                            // nasQueue/inflight 보호
        std::condition_variable cv;
        std::deque<std::deque<LogItem>> nasQueue;  // 직행 대기 배치 (dispatch → flusher)
        std::deque<LogItem> inflight;              // flusher 가 기록 중인 배치 (정지 시 스풀 회수용)
        std::atomic<bool> bRun{ true };
        std::atomic<bool> bExited{ false };              // flusher 스레드 종료 표식 (join/detach 판단)
        std::atomic<bool> bNasHealthy{ true };           // 저장 경로 판정 (dispatch 라우팅 기준)
        std::atomic<bool> bSpoolPending{ false };        // 스풀 잔량 존재 — 드레인 전 직행 금지 (순서 보존)
        std::atomic<long long> llOpStartMs{ 0 };         // 저장 경로 op 시작 시각 (0=idle) — 정체 감지
        std::atomic<long long> llSpoolBytes{ 0 };        // 스풀 사용량 (근사)
        std::atomic<bool> bLastOpOk{ true };             // 마지막 저장 경로 op 성공 여부 (idle 회복 판정)
        std::atomic<unsigned long> ulSpooledLines{ 0 };  // 폴백으로 스풀에 적재된 누적 줄 수
        std::atomic<unsigned long> ulReplayedLines{ 0 };  // 스풀→NAS 재생 완료 누적 줄 수
        std::atomic<unsigned long> ulDroppedLines{ 0 };   // 스풀 기록 실패/용량 폐기 줄 수
        std::string strLastError;                         // 마지막 실패 사유 (mtx 보호)
        // 시딩: flusher 가 기동 직후 시딩 대상 파일의 기존 줄 수를 계수 → 생산자가 합류
        std::vector<std::string> vecSeedPaths;
        std::vector<long long> vecSeedCounts;
        std::atomic<bool> bSeedDone{ false };
        // 불변 설정 (Init 에서 확정)
        std::string strSpoolDir;
        std::vector<std::string> vecBaseDirs;
        int iStallMs = 5000;
        long long llSpoolMaxBytes = 0;
    };

    void Log( EnumSlwLogLevel eLevel, const std::string &strMsg ) {
        if ( m_fnLog ) m_fnLog( eLevel, strMsg );
    }

    /** monotonic ms (steady_clock) — 정체 감지/백오프용 */
    static long long NowMs() {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch() )
            .count();
    }

    /** dispatch 스레드 본체: flush 주기 또는 큐 임계 도달 시 큐를 비워 저장소 건강 여부에 따라
     *  NAS flusher 큐 또는 로컬 스풀로 라우팅. 저장 경로 무접촉 — 항상 join 가능. */
    void WriterLoop() {
        const auto interval = std::chrono::milliseconds( kSlwFlushIntervalMs );
        StoreCtx &ctx = *m_ctx;
        while ( m_bWriterRunning.load() ) {
            std::deque<LogItem> batch;
            {
                std::unique_lock<std::mutex> lk( m_qMtx );
                m_qCv.wait_for( lk, interval, [this] { return !m_logQueue.empty() || !m_bWriterRunning.load(); } );
                batch.swap( m_logQueue );
            }

            // 정체 감지: flusher 가 저장 경로 op 에 StallSec 이상 갇혀 있으면 무응답 판정.
            long long llOpStart = ctx.llOpStartMs.load();
            if ( llOpStart != 0 && NowMs() - llOpStart > ctx.iStallMs && ctx.bNasHealthy.load() ) {
                ctx.bNasHealthy.store( false );
                ctx.bLastOpOk.store( false );
                std::lock_guard<std::mutex> lk( ctx.mtx );
                ctx.strLastError = "stall: store op in-flight > " + std::to_string( ctx.iStallMs ) + "ms";
            }

            if ( !batch.empty() ) {
                if ( !RouteBatch( batch, false ) ) {
                    // flusher 큐 포화 역압 — 배치는 m_logQueue 앞으로 되돌아갔다. 한 tick 쉰다
                    //   (스풀로 우회하면 큐 잔량보다 새 줄이 먼저 스풀에 앉아 경로별 순서가 깨진다).
                    std::this_thread::sleep_for( interval );
                }
            }
            ReconcileDegrade();
        }
        // 종료 — 잔여 큐 회수. 역압이면 스풀로 강제 회수해 어떤 경우에도 막히지 않는다.
        for ( ;; ) {
            std::deque<LogItem> batch;
            {
                std::lock_guard<std::mutex> lk( m_qMtx );
                batch.swap( m_logQueue );
            }
            if ( batch.empty() ) break;
            RouteBatch( batch, true );
        }
    }

    /** 배치 라우팅: 직행(nasQueue) | 폴백(회수 후 스풀). 반환 false = 큐 포화 역압
     *  (배치를 m_logQueue 앞으로 되돌림 — 호출자는 한 tick 쉬고 재시도). */
    bool RouteBatch( std::deque<LogItem> &batch, bool bForceSpoolOnBackpressure ) {
        if ( batch.empty() ) return true;
        StoreCtx &ctx = *m_ctx;
        bool bFallback = !ctx.bNasHealthy.load() || ctx.bSpoolPending.load();
        if ( !bFallback ) {
            bool bQueued = false;
            {
                std::lock_guard<std::mutex> lk( ctx.mtx );
                if ( ctx.nasQueue.size() < kSlwNasQueueMax ) {
                    ctx.nasQueue.push_back( std::move( batch ) );
                    bQueued = true;
                }
            }
            if ( bQueued ) {
                ctx.cv.notify_one();
                return true;
            }
            if ( !bForceSpoolOnBackpressure ) {
                // 큐 포화 — 순서 보존을 위해 m_logQueue 앞으로 되돌린다 (다음 tick 재시도).
                std::lock_guard<std::mutex> lk( m_qMtx );
                for ( auto it = batch.rbegin(); it != batch.rend(); ++it ) {
                    m_logQueue.push_front( std::move( *it ) );
                }
                batch.clear();
                return false;
            }
        }
        // 폴백 — flusher 큐 잔량(현재 배치보다 오래된 줄)부터 회수해 경로별 순서를 지킨다.
        ReclaimNasQueueToSpool();
        SpoolBatch( batch );
        return true;
    }

    /** 폴백 진입 시 flusher 큐 잔량(스풀 내용보다 오래된 분)을 FIFO 로 먼저 스풀에 회수. */
    void ReclaimNasQueueToSpool() {
        StoreCtx &ctx = *m_ctx;
        std::deque<std::deque<LogItem>> pending;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            pending.swap( ctx.nasQueue );
        }
        for ( auto &b : pending ) SpoolBatch( b );
    }

    /** 폴백 상태 전이 정리 — 전이 시에만 로그 + degrade 콜백 (dispatch 스레드 단독 소유). */
    void ReconcileDegrade() {
        StoreCtx &ctx = *m_ctx;
        bool bDegraded = !ctx.bNasHealthy.load() || ctx.bSpoolPending.load();
        if ( bDegraded == m_bDegraded ) return;
        m_bDegraded = bDegraded;

        SlwDegradeInfo info;
        info.bDegraded = bDegraded;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            info.strReason = ctx.strLastError;
        }
        info.ulSpooledLines = ctx.ulSpooledLines.load();
        info.ulReplayedLines = ctx.ulReplayedLines.load();
        info.ulDroppedLines = ctx.ulDroppedLines.load();

        if ( bDegraded ) {
            Log( SLW_LOG_ERROR, "service_log store fallback engaged (reason=" +
                                    ( info.strReason.empty() ? "spool backlog" : info.strReason ) +
                                    ", spool=" + ctx.strSpoolDir + ")" );
        } else {
            Log( SLW_LOG_INFO, "service_log store recovered — spool drained (spooled=" +
                                   std::to_string( info.ulSpooledLines ) +
                                   ", replayed=" + std::to_string( info.ulReplayedLines ) +
                                   ", dropped=" + std::to_string( info.ulDroppedLines ) + ")" );
        }
        if ( m_fnDegrade ) m_fnDegrade( info );
    }

    /** 한 배치를 로컬 스풀에 기록 (dispatch/정지 경로 전용 — 로컬 디스크 I/O). */
    void SpoolBatch( std::deque<LogItem> &batch ) {
        if ( batch.empty() ) return;
        StoreCtx &ctx = *m_ctx;
        // 파일경로별 병합 후 스풀 미러에 append (로컬 디스크 — 실패는 즉시 반환, 행 없음).
        std::unordered_map<std::string, std::pair<std::string, size_t>> groups;  // path → (data, lines)
        for ( auto &item : batch ) {
            auto &slot = groups[item.path];
            slot.first += item.line;
            slot.second++;
        }
        batch.clear();
        for ( auto &kv : groups ) {
            SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
        }
        TrimSpoolIfNeeded();
    }

    /** 스풀 용량 상한 초과 시 오래된 스풀 파일부터 폐기 (dispatch 전용, 주기 제한). */
    void TrimSpoolIfNeeded() {
        StoreCtx &ctx = *m_ctx;
        if ( ctx.llSpoolBytes.load() <= ctx.llSpoolMaxBytes ) return;
        long long llNow = NowMs();
        if ( llNow - m_llLastSpoolTrimMs < kSlwSpoolTrimIntervalMs ) return;
        m_llLastSpoolTrimMs = llNow;

        // 오래된 스풀 파일부터 폐기 — 재생 중(.replay) 파일은 건드리지 않는다.
        std::vector<std::pair<time_t, std::pair<std::string, long long>>> files;  // (mtime, (path, size))
        ScanSpool( ctx.strSpoolDir, [&]( const std::string &strPath, time_t tMtime, long long llSize ) {
            if ( strPath.size() > 7 && strPath.compare( strPath.size() - 7, 7, ".replay" ) == 0 ) return;
            files.push_back( { tMtime, { strPath, llSize } } );
        } );
        std::sort( files.begin(), files.end() );
        long long llTarget = ctx.llSpoolMaxBytes * 9 / 10;  // 90% 까지 정리
        for ( auto &f : files ) {
            if ( ctx.llSpoolBytes.load() <= llTarget ) break;
            int iLines = CountFileLines( f.second.first );
            if ( unlink( f.second.first.c_str() ) == 0 ) {
                ctx.llSpoolBytes.fetch_sub( f.second.second );
                ctx.ulDroppedLines.fetch_add( (unsigned long)iLines );
                Log( SLW_LOG_ERROR, "service_log spool over capacity — dropped " + f.second.first + " (" +
                                        std::to_string( iLines ) + " lines)" );
            }
        }
    }

    // ── NAS flusher (저장 경로 I/O 전담 — StoreCtx 외 무접촉) ─────────────────
    /** NAS flusher 스레드 본체 — ctx 만 참조 (writer 본체 무접촉, detach 안전).
     *  기동 시: base 디렉터리 보장 + 시딩 계수. 이후: nasQueue 배치 flush,
     *  idle 이면 스풀 재생. 실패/정체 시 ctx 상태만 갱신 (전이 통지는 dispatch 가 한다). */
    static void NasFlusherLoop( std::shared_ptr<StoreCtx> pCtx ) {
        StoreCtx &ctx = *pCtx;

        // 기동 작업 ①: base 디렉터리 보장 (저장 경로 최초 접촉 — 여기서 행이면 여기만 갇힌다)
        ctx.llOpStartMs.store( NowMs() );
        for ( const auto &dir : ctx.vecBaseDirs ) {
            if ( !dir.empty() ) MkdirP( dir );
        }
        // 기동 작업 ②: 시딩 — 저장 경로의 기존 줄 + 이전 run 스풀 잔량(재생 대기분) 계수
        for ( size_t i = 0; i < ctx.vecSeedPaths.size(); i++ ) {
            if ( ctx.vecSeedPaths[i].empty() || !ctx.bRun.load() ) continue;
            long long n = CountFileLines( ctx.vecSeedPaths[i] );
            std::string strSpool = SpoolPathFor( ctx, ctx.vecSeedPaths[i] );
            n += CountFileLines( strSpool );
            n += CountFileLines( strSpool + ".replay" );
            ctx.vecSeedCounts[i] = n;
        }
        ctx.llOpStartMs.store( 0 );
        ctx.bSeedDone.store( true );

        long long llNextReplayMs = 0;
        while ( ctx.bRun.load() ) {
            std::deque<LogItem> batch;
            {
                std::unique_lock<std::mutex> lk( ctx.mtx );
                ctx.cv.wait_for( lk, std::chrono::milliseconds( 200 ),
                                 [&] { return !ctx.nasQueue.empty() || !ctx.bRun.load(); } );
                if ( !ctx.bRun.load() ) break;
                if ( !ctx.nasQueue.empty() ) {
                    batch.swap( ctx.nasQueue.front() );
                    ctx.nasQueue.pop_front();
                    ctx.inflight = batch;  // 정지 시 회수용 사본
                }
            }
            if ( !batch.empty() ) {
                if ( ctx.bSpoolPending.load() ) {
                    // dispatch 의 회수(reclaim)와 pop 이 겹친 드문 경쟁 — 이 배치는 스풀 내용보다
                    //   오래됐을 수 있으므로 저장 경로 직행 대신 스풀로 우회해 재생 경로로 일원화.
                    SpoolBatchToCtx( ctx, batch );
                } else {
                    FlushBatchToStore( ctx, batch );
                }
                std::lock_guard<std::mutex> lk( ctx.mtx );
                ctx.inflight.clear();
                continue;
            }
            // idle — 스풀 재생 (실패 시 백오프)
            if ( ctx.bSpoolPending.load() && NowMs() >= llNextReplayMs ) {
                ReplaySpoolOne( ctx );
                if ( !ctx.bLastOpOk.load() ) llNextReplayMs = NowMs() + kSlwReplayRetryMs;
            }
            // 정체로 unhealthy 가 됐지만 스풀 유입이 없었던 경우 — 직전 op 성공이 확인되면 복귀
            if ( !ctx.bNasHealthy.load() && !ctx.bSpoolPending.load() && ctx.bLastOpOk.load() ) {
                ctx.bNasHealthy.store( true );
            }
        }
        ctx.bExited.store( true );
    }

    /** 배치를 저장 경로에 기록. 실패 그룹부터는 스풀로 우회 적재. */
    static void FlushBatchToStore( StoreCtx &ctx, std::deque<LogItem> &batch ) {
        if ( batch.empty() ) return;
        // 파일경로별 병합 — 같은 경로의 줄은 batch 순서(=enqueue/seq 순서)대로 누적.
        std::unordered_map<std::string, std::pair<std::string, size_t>> groups;
        for ( auto &item : batch ) {
            auto &slot = groups[item.path];
            slot.first += item.line;
            slot.second++;
        }
        batch.clear();

        bool bFailed = false;
        for ( auto &kv : groups ) {
            if ( bFailed ) {
                // 앞 그룹 실패 후에는 저장 경로를 더 만지지 않고 스풀로 우회 (지연 누적 방지)
                SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
                continue;
            }
            // 디렉터리 보장 후 경로당 1회 open→append→close (서로 다른 파일끼리는 순서 무관)
            std::string strDir = kv.first.substr( 0, kv.first.rfind( '/' ) );
            ctx.llOpStartMs.store( NowMs() );
            MkdirP( strDir );
            int iErr = 0;
            FILE *pFile = fopen( kv.first.c_str(), "a" );
            bool bOk = false;
            if ( pFile ) {
                bOk = fwrite( kv.second.first.data(), 1, kv.second.first.size(), pFile ) == kv.second.first.size();
                if ( !bOk ) iErr = errno;
                fclose( pFile );
            } else {
                iErr = errno;
            }
            ctx.llOpStartMs.store( 0 );
            if ( bOk ) {
                ctx.bLastOpOk.store( true );
            } else {
                bFailed = true;
                ctx.bLastOpOk.store( false );
                ctx.bNasHealthy.store( false );
                {
                    std::lock_guard<std::mutex> lk( ctx.mtx );
                    ctx.strLastError = std::string( "write failed: " ) + strerror( iErr );
                }
                SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
            }
        }
    }

    /** 배치 전체를 스풀로 우회 (flusher 전용 — 스풀 잔량 존재 중 직행 대기분의 순서 보존). */
    static void SpoolBatchToCtx( StoreCtx &ctx, std::deque<LogItem> &batch ) {
        if ( batch.empty() ) return;
        std::unordered_map<std::string, std::pair<std::string, size_t>> groups;
        for ( auto &item : batch ) {
            auto &slot = groups[item.path];
            slot.first += item.line;
            slot.second++;
        }
        batch.clear();
        for ( auto &kv : groups ) {
            SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
        }
    }

    /** 한 목적 경로 분량을 스풀 미러 파일에 append (로컬). 실패 시 폐기 계수. */
    static bool SpoolAppend( StoreCtx &ctx, const std::string &strTarget, const std::string &strData, size_t nLines ) {
        std::string strSpoolPath = SpoolPathFor( ctx, strTarget );
        std::string strDir = strSpoolPath.substr( 0, strSpoolPath.rfind( '/' ) );
        MkdirP( strDir );
        FILE *pFile = fopen( strSpoolPath.c_str(), "a" );
        bool bOk = false;
        if ( pFile ) {
            bOk = fwrite( strData.data(), 1, strData.size(), pFile ) == strData.size();
            fclose( pFile );
        }
        if ( bOk ) {
            ctx.llSpoolBytes.fetch_add( (long long)strData.size() );
            ctx.ulSpooledLines.fetch_add( (unsigned long)nLines );
            ctx.bSpoolPending.store( true );
        } else {
            // 로컬 스풀마저 실패 (디스크 풀 등) — 폐기 계수만 남긴다
            ctx.ulDroppedLines.fetch_add( (unsigned long)nLines );
        }
        return bOk;
    }

    /** 스풀에서 가장 오래된 파일 하나를 저장 경로로 재생. 스풀이 비면 bSpoolPending 해제.
     *  반환: 재생을 시도했으면 true (실패 포함), 스풀이 비어 할 일이 없었으면 false. */
    static bool ReplaySpoolOne( StoreCtx &ctx ) {
        // 재생 대상 선택: 중단분(.replay) 우선, 없으면 가장 오래된 파일을 .replay 로 rename.
        std::string strPick;
        time_t tPickMtime = 0;
        bool bPickIsReplay = false;
        ScanSpool( ctx.strSpoolDir, [&]( const std::string &strPath, time_t tMtime, long long ) {
            bool bReplay = strPath.size() > 7 && strPath.compare( strPath.size() - 7, 7, ".replay" ) == 0;
            if ( bReplay != bPickIsReplay ) {
                if ( !bReplay ) return;  // .replay 가 이미 후보면 일반 파일은 무시
                strPick.clear();         // 일반 후보를 .replay 로 교체
                bPickIsReplay = true;
            }
            if ( strPick.empty() || tMtime < tPickMtime ) {
                strPick = strPath;
                tPickMtime = tMtime;
            }
        } );

        if ( strPick.empty() ) {
            // 스풀 드레인 완료 — 직행 복귀 (전이 통지는 dispatch 가 수행)
            ctx.bSpoolPending.store( false );
            ctx.llSpoolBytes.store( 0 );
            if ( ctx.bLastOpOk.load() ) ctx.bNasHealthy.store( true );
            return false;
        }

        if ( !bPickIsReplay ) {
            std::string strRenamed = strPick + ".replay";
            if ( rename( strPick.c_str(), strRenamed.c_str() ) != 0 ) return true;
            strPick = strRenamed;
        }

        // 내용 적재 (로컬 읽기)
        std::string strData;
        long long llSize = 0;
        {
            FILE *pFile = fopen( strPick.c_str(), "r" );
            if ( !pFile ) return true;  // 경쟁 삭제 등 — 다음 tick 재평가
            char buf[65536];
            size_t r;
            while ( ( r = fread( buf, 1, sizeof( buf ), pFile ) ) > 0 ) strData.append( buf, r );
            fclose( pFile );
            llSize = (long long)strData.size();
        }
        size_t nLines = 0;
        for ( char c : strData ) {
            if ( c == '\n' ) nLines++;
        }

        std::string strBase = strPick.substr( 0, strPick.size() - 7 );  // ".replay" 제거
        std::string strTarget = TargetPathFor( ctx, strBase );
        if ( strTarget.empty() ) {
            // 매핑 불능(손상 경로) — 폐기
            unlink( strPick.c_str() );
            ctx.llSpoolBytes.fetch_sub( llSize );
            ctx.ulDroppedLines.fetch_add( (unsigned long)nLines );
            return true;
        }

        // 저장 경로 append (여기서 행이면 flusher 만 갇힌다 — dispatch 가 정체를 감지)
        std::string strDir = strTarget.substr( 0, strTarget.rfind( '/' ) );
        ctx.llOpStartMs.store( NowMs() );
        MkdirP( strDir );
        int iErr = 0;
        FILE *pFile = fopen( strTarget.c_str(), "a" );
        bool bOk = false;
        if ( pFile ) {
            bOk = fwrite( strData.data(), 1, strData.size(), pFile ) == strData.size();
            if ( !bOk ) iErr = errno;
            fclose( pFile );
        } else {
            iErr = errno;
        }
        ctx.llOpStartMs.store( 0 );

        if ( bOk ) {
            unlink( strPick.c_str() );
            ctx.llSpoolBytes.fetch_sub( llSize );
            ctx.ulReplayedLines.fetch_add( (unsigned long)nLines );
            ctx.bLastOpOk.store( true );
        } else {
            ctx.bLastOpOk.store( false );
            ctx.bNasHealthy.store( false );
            std::lock_guard<std::mutex> lk( ctx.mtx );
            ctx.strLastError = std::string( "replay failed: " ) + strerror( iErr );
        }
        return true;
    }

    /** 목적 경로 → 스풀 미러 경로 ({spool}/abs{path} | {spool}/rel/{path}) */
    static std::string SpoolPathFor( const StoreCtx &ctx, const std::string &strTarget ) {
        // 절대/상대 목적 경로를 무손실 왕복 가능한 미러 경로로: {spool}/abs{…} | {spool}/rel/{…}
        if ( !strTarget.empty() && strTarget[0] == '/' ) return ctx.strSpoolDir + "/abs" + strTarget;
        return ctx.strSpoolDir + "/rel/" + strTarget;
    }

    /** 스풀 미러 경로 → 목적 경로 (SpoolPathFor 역변환. 실패 시 빈 문자열) */
    static std::string TargetPathFor( const StoreCtx &ctx, const std::string &strSpoolFile ) {
        std::string strAbsPrefix = ctx.strSpoolDir + "/abs/";
        std::string strRelPrefix = ctx.strSpoolDir + "/rel/";
        if ( strSpoolFile.compare( 0, strAbsPrefix.size(), strAbsPrefix ) == 0 ) {
            return strSpoolFile.substr( strAbsPrefix.size() - 1 );  // 선행 '/' 유지
        }
        if ( strSpoolFile.compare( 0, strRelPrefix.size(), strRelPrefix ) == 0 ) {
            return strSpoolFile.substr( strRelPrefix.size() );
        }
        return "";
    }

    /** 스풀 트리 재귀 스캔 — 파일 (경로, mtime, size) 콜백. */
    static void ScanSpool( const std::string &strDir,
                           const std::function<void( const std::string &, time_t, long long )> &fn ) {
        DIR *pDir = opendir( strDir.c_str() );
        if ( !pDir ) return;
        struct dirent *pEnt;
        while ( ( pEnt = readdir( pDir ) ) != NULL ) {
            if ( strcmp( pEnt->d_name, "." ) == 0 || strcmp( pEnt->d_name, ".." ) == 0 ) continue;
            std::string strPath = strDir + "/" + pEnt->d_name;
            struct stat st;
            if ( stat( strPath.c_str(), &st ) != 0 ) continue;
            if ( S_ISDIR( st.st_mode ) ) {
                ScanSpool( strPath, fn );
            } else if ( S_ISREG( st.st_mode ) ) {
                fn( strPath, st.st_mtime, (long long)st.st_size );
            }
        }
        closedir( pDir );
    }

    /** 파일의 현재 줄 수(개행 계수) — 시딩/폐기 계수용. 없으면 0. */
    static int CountFileLines( const std::string &path ) {
        FILE *f = fopen( path.c_str(), "r" );
        if ( !f ) return 0;
        int n = 0;
        char buf[65536];
        size_t r;
        while ( ( r = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) {
            for ( size_t i = 0; i < r; i++ ) {
                if ( buf[i] == '\n' ) n++;
            }
        }
        fclose( f );
        return n;
    }

    /** Ensure directory exists (recursive) */
    static bool MkdirP( const std::string &path ) {
        struct stat st;
        if ( stat( path.c_str(), &st ) == 0 ) return true;
        size_t pos = path.rfind( '/' );
        if ( pos != std::string::npos && pos > 0 ) MkdirP( path.substr( 0, pos ) );
        return mkdir( path.c_str(), 0755 ) == 0 || errno == EEXIST;
    }

    SlwLogFn m_fnLog;
    SlwDegradeFn m_fnDegrade;

    // 비동기 writer 상태 — 생산자는 m_qMtx 만 잡고 enqueue, dispatch 가 소비
    std::deque<LogItem> m_logQueue;  // 기록 대기 (파일경로 + 한 줄)
    std::mutex m_qMtx;               // m_logQueue 보호
    std::condition_variable m_qCv;
    std::thread m_writerThread;  // dispatch 스레드 (항상 join 가능 — 저장 경로 무접촉)
    std::atomic<bool> m_bWriterRunning;
    std::atomic<unsigned long> m_ulDroppedLogs;  // 큐 상한 초과로 버려진 줄 수

    std::shared_ptr<StoreCtx> m_ctx;  // NAS flusher 공유 상태
    std::thread m_nasThread;          // NAS flusher (정지 시 join 불가하면 detach)
    bool m_bDegraded;                 // 폴백 상태 전이 추적 (dispatch 단독 접근)
    long long m_llLastSpoolTrimMs;    // 스풀 용량 정리 주기 제한 (dispatch 단독 접근)
};

#endif
