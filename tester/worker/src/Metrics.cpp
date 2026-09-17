#include "Metrics.h"

// 로그 스케일 상한 (ms 또는 지표 단위) — 컨트롤러가 백분위를 근사할 때 상한값을 쓴다.
static const double kBounds[] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 60000 };

void Histogram::add(double v) {
    if (count == 0) { min = max = v; } else { if (v < min) min = v; if (v > max) max = v; }
    count++;
    sum += v;
    std::string key = "inf";
    for (double b : kBounds) if (v <= b) { key = std::to_string((long long)b); break; }
    buckets[key]++;
}

Json Histogram::toJson() const {
    Json j = Json::Object();
    j["count"] = Json(count);
    j["sum"] = Json(sum);
    if (count > 0) { j["min"] = Json(min); j["max"] = Json(max); }
    Json b = Json::Object();
    for (auto& kv : buckets) b[kv.first] = Json(kv.second);
    j["buckets"] = b;
    return j;
}

void Metrics::counter(const std::string& name, long long n) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_counters[name] += n;
    m_totals[name] += n;
}

void Metrics::gauge(const std::string& name, double v) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_gauges[name] = v;
}

void Metrics::timer(const std::string& name, double v) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_timers[name].add(v);
}

Json Metrics::flush(const std::string& runId, const std::string& worker, double t, int bucketS) {
    std::lock_guard<std::mutex> lk(m_mtx);
    Json j = Json::Object();
    j["kind"] = Json("agg");
    j["t"] = Json(t);
    j["bucket_s"] = Json(bucketS);
    j["run_id"] = Json(runId);
    j["worker"] = Json(worker);
    Json c = Json::Object();
    for (auto& kv : m_counters) c[kv.first] = Json(kv.second);
    Json g = Json::Object();
    for (auto& kv : m_gauges) g[kv.first] = Json(kv.second);
    Json tm = Json::Object();
    for (auto& kv : m_timers) tm[kv.first] = kv.second.toJson();
    j["counters"] = c;
    j["gauges"] = g;
    j["timers"] = tm;
    m_counters.clear();
    m_timers.clear();
    return j;
}

Json Metrics::totals() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    Json c = Json::Object();
    for (auto& kv : m_totals) c[kv.first] = Json(kv.second);
    Json g = Json::Object();
    for (auto& kv : m_gauges) g[kv.first] = Json(kv.second);
    Json j = Json::Object();
    j["counters"] = c;
    j["gauges"] = g;
    return j;
}
