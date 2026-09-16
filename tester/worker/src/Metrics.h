// Metrics — 1초 버킷 집계 (test_instrument.md §5·§6.1 `agg` 레코드).
//   counters: attempts·sessions·completed·failed·codes.<n>·rtp_rx·rtp_lost … (sip_statistics 3계층 어휘)
//   gauges  : concurrent_sessions·registered·active_instances·rate_saps·cpu_pct …
//   timers  : rrd_ms·srd_ms·sdd_ms·jitter_ms·rtp_loss_pct·sdt_s — 로그 스케일 상한 버킷 히스토그램
// 스택 스레드·스케줄러 스레드가 함께 쓰므로 뮤텍스 하나로 지킨다(초당 수천 회 수준 — 충분하다).
#ifndef _CIMS_TESTER_METRICS_H_
#define _CIMS_TESTER_METRICS_H_

#include <map>
#include <mutex>
#include <string>

#include "Json.h"

struct Histogram {
    long long count = 0;
    double sum = 0, min = 0, max = 0;
    std::map<std::string, long long> buckets;   // 상한(ms 문자열) → 수
    void add(double v);
    Json toJson() const;
};

class Metrics {
public:
    void counter(const std::string& name, long long n = 1);
    void gauge(const std::string& name, double v);
    void timer(const std::string& name, double v);
    /** 현재 버킷을 agg 레코드로 꺼내고(가 아니라 복사·초기화) 카운터/타이머는 비운다. 게이지는 유지. */
    Json flush(const std::string& runId, const std::string& worker, double t, int bucketS = 1);
    /** 누적 합(run 전체) — 워커 GET /runs/{id} 스냅샷용. */
    Json totals() const;

private:
    mutable std::mutex m_mtx;
    std::map<std::string, long long> m_counters, m_totals;
    std::map<std::string, double> m_gauges;
    std::map<std::string, Histogram> m_timers;
};

#endif
