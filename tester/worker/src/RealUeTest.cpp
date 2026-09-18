// tester_real_ue_test — RealUeProcess(실단말 프로세스 관리) 단위시험. cimsue-cli 대신 파이썬 스텁(real_ue_stub.py)을 띄워
//   스폰·ready·동기 결과(request)·비동기 이벤트 전달·정상 종료·비정상 종료(process_exit)·기동 실패를 확인한다.
//   cims-verify S1-UNIT-TESTER 가 실행한다. 종료 코드 0 = 전부 통과.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RealUe.h"

#ifndef REAL_UE_STUB
#define REAL_UE_STUB "tester/worker/src/real_ue_stub.py"
#endif

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } } while (0)

struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Json> evs;
    void push(const Json& e) { { std::lock_guard<std::mutex> lk(m); evs.push_back(e); } cv.notify_all(); }
    bool waitFor(const std::string& event, const std::string& state, int timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            for (auto& e : evs) if (e["event"].asString() == event && (state.empty() || e["state"].asString() == state)) return true;
            return false;
        });
    }
    size_t count(const std::string& event) { std::lock_guard<std::mutex> lk(m); size_t n = 0; for (auto& e : evs) if (e["event"].asString() == event) n++; return n; }
};

int main() {
    std::vector<std::string> base = { "/usr/bin/python3", REAL_UE_STUB };
    // ① 정상 경로 — ready → register → dial → stats → hangup → quit
    {
        Collector c;
        RealUeProcess p("t1", [&](const Json& e) { c.push(e); });
        std::string err;
        CHECK(p.start(base, "", err), ("start: " + err).c_str());
        CHECK(p.waitReady(5000), "ready");
        Json r = p.request("register", 3000);
        CHECK(r["ok"].asBool(false), "register result ok");
        CHECK(c.waitFor("reg", "registered", 3000), "reg registered event");
        r = p.request("dial 100", 3000);
        CHECK(r["ok"].asBool(false) && r["call"].asInt(-1) == 1, "dial result call=1");
        CHECK(c.waitFor("call", "active", 3000), "call active event");
        CHECK(c.waitFor("stats", "", 3000), "stats event");
        r = p.request("bogus", 3000);
        CHECK(!r["ok"].asBool(true) && r["reason"].asString() == "unknown command", "unknown command → ok=false");
        r = p.request("hangup 1", 3000);
        CHECK(r["ok"].asBool(false), "hangup result ok");
        CHECK(c.waitFor("call", "disconnected", 3000), "call disconnected event");
        {
            std::lock_guard<std::mutex> lk(c.m);
            bool byUs = false;
            for (auto& e : c.evs) if (e["event"].asString() == "call" && e["state"].asString() == "disconnected") byUs = e["by_us"].asBool(false) && e["stats_valid"].asBool(false);
            CHECK(byUs, "disconnected carries by_us + final stats");
        }
        r = p.request("floor_request 1", 3000);
        CHECK(r["ok"].asBool(false), "floor_request ok");
        CHECK(c.waitFor("floor", "", 3000), "floor event");
        p.stop(2000);
        CHECK(!p.alive(), "stopped");
        CHECK(c.count("exit") == 1, "cli exit event once");
        CHECK(c.count("process_exit") == 1, "process_exit synthesized once");
    }
    // ② 비정상 종료 — crash 명령 뒤 process_exit, 이후 request 는 ok=false
    {
        Collector c;
        RealUeProcess p("t2", [&](const Json& e) { c.push(e); });
        std::string err;
        CHECK(p.start(base, "", err), "start t2");
        CHECK(p.waitReady(5000), "ready t2");
        Json r = p.request("crash", 3000);
        CHECK(r["ok"].asBool(false), "crash result ok");
        CHECK(c.waitFor("process_exit", "", 3000), "process_exit after crash");
        for (int i = 0; i < 50 && p.alive(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!p.alive(), "not alive after crash");
        r = p.request("register", 500);
        CHECK(!r["ok"].asBool(true), "request on dead process → ok=false");
        p.stop(200);
    }
    // ③ 기동 실패 — ready 없이 exit
    {
        Collector c;
        RealUeProcess p("t3", [&](const Json& e) { c.push(e); });
        std::string err;
        std::vector<std::string> argv = base;
        argv.push_back("--fail-start");
        CHECK(p.start(argv, "", err), "start t3");
        CHECK(!p.waitReady(3000), "waitReady false on failed start");
        CHECK(c.waitFor("exit", "", 3000), "exit event with error");
        p.stop(200);
    }
    // ④ exec 실패(없는 경로)
    {
        Collector c;
        RealUeProcess p("t4", [&](const Json& e) { c.push(e); });
        std::string err;
        CHECK(p.start({ "/nonexistent/cimsue-cli", "drive" }, "", err), "fork ok even if exec fails");
        CHECK(!p.waitReady(2000), "no ready on exec failure");
        CHECK(c.waitFor("exit", "", 2000), "exit event on exec failure");
        p.stop(200);
    }
    std::printf("tester_real_ue_test: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
