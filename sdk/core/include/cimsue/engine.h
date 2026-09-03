// libcimsue — Engine (ue_sdk.md §4.2)
//
// 프로세스당 1개. 명령은 어느 스레드에서 불러도 되며 코어 제어 스레드(`ue-ctl`)에서 직렬 실행된 뒤
// 즉시 결과(Result/id)를 돌려준다. 프로토콜 진행은 Listener 이벤트로 온다.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cimsue/listener.h"
#include "cimsue/types.h"

namespace cimsue {

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /** 엔진 기동 — transport(UDP/TCP/TLS) 생성·코덱 정합·장치 준비. listener 는 stop() 까지 유효해야 한다. */
    Result start(const EngineConfig& cfg, Listener* listener);
    /** 모든 호·계정 정리 후 종료. 이후 start() 로 재기동 가능. */
    void stop();
    bool running() const;

    // ── 계정 ──
    /** 계정 추가(등록은 하지 않음). 반환 accountId ≥ 0, 실패 -1. */
    int addAccount(const AccountConfig& cfg);
    Result registerAccount(int accountId);
    Result unregisterAccount(int accountId);
    /** 즉시 재-REGISTER(서버 재기동 등으로 등록을 잃은 경우 복구). */
    Result refreshRegistration(int accountId);
    Result removeAccount(int accountId);
    RegInfo regInfo(int accountId) const;
    std::vector<int> accounts() const;

    // ── 호 ──
    /** 발신. target 은 번호(도메인 자동 결합) 또는 sip: URI. 반환 callId ≥ 0, 실패 -1. */
    int dial(int accountId, const std::string& target, const CallOptions& opts = CallOptions());
    Result answer(int callId, const CallOptions& opts = CallOptions());
    Result reject(int callId, int statusCode = 486);
    Result hangup(int callId);
    Result hold(int callId);
    Result resume(int callId);
    /** 마이크 → 호 송신 차단/복구. */
    Result setMuted(int callId, bool muted);
    /** 호 → 스피커 청취 on/off (멀티 채널 듣기 정책). */
    Result setListen(int callId, bool listen);
    /** 수신 음량(1.0=원음, 0=무음). */
    Result setRxLevel(int callId, float level);
    Result sendDtmf(int callId, const std::string& digits);
    CallInfo callInfo(int callId) const;
    std::vector<int> calls() const;
    /** 오디오 스트림 RTP/RTCP 통계(동기 조회). 미디어 비활성이면 valid=false. */
    StreamStats streamStats(int callId) const;

    // ── 장치 ──
    std::vector<AudioDeviceInfo> audioDevices() const;
    /** 캡처/재생 장치 선택(pjmedia 장치 id). -1=기본 캡처, -2=기본 재생. */
    Result setAudioDevices(int captureDev, int playbackDev);

    static std::string version();

    /** 내부 구현(pImpl). 앱·플랫폼 SDK 는 사용하지 않는다. */
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cimsue
