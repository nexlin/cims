// libcimsue 내부 — TCP(+TLS) 클라이언트 스트림. BSD 소켓/winsock 차이와 OpenSSL 핸드셰이크(검증·SNI·IP SAN)를 한 곳에 둔다 —
// HTTPS 전송(http/https_client)과 계측 링크(drive/device_link, ue_voice_quality.md §5.1)가 같이 쓴다.
//
// 스레드: 한 Stream 을 두 스레드가 동시에 read/write 하지 않는다(OpenSSL SSL 객체는 동시 읽기·쓰기에 안전하지 않다) — 계측 링크는
// 입출력 스레드 하나가 waitReadable 로 번갈아 읽고 쓴다. shutdownSocket 만은 다른 스레드에서 불러 막힌 read 를 풀 수 있다.
#pragma once

#include <cstdint>
#include <string>

namespace cimsue {
namespace net {

struct TlsOptions {
    bool tls = true;
    bool verify = false;           // 서버 인증서 검증(체인 + 이름/IP SAN). false 면 검증 없이 암호화만
    std::string caPem;             // 검증 앵커 PEM(여러 장 가능). 비면 시스템 기본 저장소
};

/** 핸드셰이크에서 본 서버 leaf 인증서. */
struct PeerCert {
    int64_t notAfterEpoch = 0;     // UTC epoch 초
    std::string subject;           // 한 줄
    std::string sha256;            // DER 의 SHA-256 소문자 hex — 최초 지문 고정(TOFU)용
};

class Stream {
public:
    Stream() = default;
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /** 연결 + (tls 면) 핸드셰이크. timeoutSec = 연결·송수신 시한. 실패 = false + err. */
    bool open(const std::string& host, int port, int timeoutSec, const TlsOptions& opt, std::string& err);
    bool isOpen() const;
    /** 전부 보낼 때까지. 실패 = false. */
    bool writeAll(const std::string& data);
    /** 받은 만큼(최대 n). 0 = 상대가 닫음, 음수 = 오류·시한. */
    int read(char* buf, int n);
    /** 읽을 것이 있을 때까지 최대 ms 기다린다 — TLS 버퍼에 남은 것도 센다. 1 = 읽을 것 있음, 0 = 시한, 음수 = 오류. */
    int waitReadable(int ms);
    /** 다른 스레드에서 막힌 read/waitReadable 을 깨운다(소켓 shutdown). 이후 close 는 소유 스레드가. */
    void shutdownSocket();
    void close();
    const PeerCert& peer() const { return peer_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    PeerCert peer_;
};

}  // namespace net
}  // namespace cimsue
