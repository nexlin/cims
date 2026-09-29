// DeviceHub — 시험 모드 실기기의 계측 링크 수신점 (ue_voice_quality.md §5·§6, test_instrument.md device 풀).
//
//   단말(앱 시험 모드·cimsue-cli link)이 Device.Ip:Port 로 TLS 연결 → hello{proto, device_id, pair_key, accounts[...]} → 워커가 검사해
//   welcome{worker, accepted} 또는 bye{reason} → 이후 cimsue/drive.h 줄 프로토콜. 시험 대상 서버(CSP·CSC·CMP)를 거치지 않는다.
//   연결 하나 = DeviceConn(DriveLink 구현) — 입출력 스레드 하나가 읽기와 쓰기 큐 비우기를 번갈아 한다(OpenSSL SSL 객체는 동시
//   읽기·쓰기에 안전하지 않다). device 풀이 번호로 연결을 골라 bind 하면 이벤트가 워커(onRealEvent)로 간다 — real-ue 와 같은 경로.
//   같은 device_id 가 다시 붙으면 옛 연결을 닫는다. 15 s 무수신이면 ping, 45 s 면 끊는다.
#ifndef _CIMS_TESTER_DEVICE_HUB_H_
#define _CIMS_TESTER_DEVICE_HUB_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Json.h"
#include "RealUe.h"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

struct DeviceAccount {
    std::string service, aor, msisdn;
    bool registered = false;
};

struct DeviceInfo {
    std::string deviceId, app, version, platform, model, engine, addr;
    long long connectedMs = 0;       // epoch ms
    std::vector<DeviceAccount> accounts;
    std::string boundPool;           // 이 연결을 쓰는 device 풀(비면 대기)
    bool alive = false;
};

class DeviceConn : public DriveLink {
public:
    using EventFn = std::function<void(const Json&)>;
    DeviceConn(int fd, SSL* ssl, const std::string& addr);
    ~DeviceConn() override;

    Json request(const std::string& cmd, int timeoutMs) override;
    bool send(const std::string& cmd) override;
    bool alive() const override { return m_alive.load(); }

    /** device 풀이 이 연결을 잡는다 — 이후 이벤트(result 제외)가 fn 으로. 링크가 끊기면 fn 에 process_exit(error=device link lost). */
    void bind(EventFn fn, const std::string& pool);
    void unbind();
    /** 서비스 회선의 등록 상태(hello·reg 이벤트로 갱신). */
    bool registered(const std::string& service) const;
    DeviceInfo info() const;
    /** 입출력 스레드에 닫기를 요청하고 끝날 때까지 기다린다. quiet=false 면 바인딩 풀에 링크 끊김(process_exit)을 알린다(재접속으로 교체). */
    void close(bool quiet = true);

private:
    friend class DeviceHub;
    void start(const Json& hello);   // welcome 을 보낸 뒤 입출력 스레드
    void ioLoop();
    void deliver(const Json& ev);
    bool writeRaw(const std::string& line);

    int m_fd;
    SSL* m_ssl;
    std::atomic<bool> m_alive{true};
    std::atomic<bool> m_closing{false};
    std::atomic<bool> m_quiet{false};   // 닫기 요청이 끊김 통지를 막는다(워커 정지·풀 정리)
    std::thread m_io;
    mutable std::mutex m_mtx;        // 결과 큐·쓰기 큐·정보·바인딩
    std::condition_variable m_cv;
    std::deque<Json> m_results;
    std::deque<std::string> m_out;
    EventFn m_onEvent;
    DeviceInfo m_info;
};

struct DeviceHubConfig {
    std::string ip = "0.0.0.0";
    int port = 7120;                 // 0 = 끔 (7110 은 컨트롤러 관측 수신 Tester.WorkerStreamPort)
    std::string certFile, keyFile;   // 없으면 기동 때 자체 서명(EC P-256)을 만들어 이 경로에 둔다 — 단말은 TOFU 로 지문 고정
    std::string pairKey;             // 비면 검사 안 함
    int maxDevices = 32;
    std::string workerName;
};

class DeviceHub {
public:
    explicit DeviceHub(const DeviceHubConfig& cfg);
    ~DeviceHub();
    /** 수신 시작. 실패(포트 사용 중 등)는 워커 기동을 막지 않는다 — health 가 listening=false 로 알린다. */
    bool start(std::string& err);
    void stop();
    bool listening() const { return m_listenFd >= 0; }
    const DeviceHubConfig& config() const { return m_cfg; }
    std::string fingerprint() const { return m_fingerprint; }   // 수신점 인증서 SHA-256(hex) — 단말 TOFU 대조용
    std::vector<DeviceInfo> list();
    /** 번호·서비스로 살아 있는 연결을 찾는다(서비스가 비면 번호만). */
    std::shared_ptr<DeviceConn> find(const std::string& msisdn, const std::string& service);
    int connected();

private:
    void acceptLoop();
    void handshake(int fd, std::string addr);
    bool loadOrCreateCert(std::string& err);

    DeviceHubConfig m_cfg;
    SSL_CTX* m_ctx = nullptr;
    int m_listenFd = -1;
    std::atomic<bool> m_stop{false};
    std::thread m_accept;
    std::mutex m_mtx;
    std::vector<std::shared_ptr<DeviceConn>> m_conns;
    struct Handshake { std::thread t; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Handshake> m_handshakes;   // 끝난 것은 다음 수락 때 join 해 거둔다
    std::string m_fingerprint;
};

#endif
