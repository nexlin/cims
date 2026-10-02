// libcimsue — 구동 세션(DriveSession)·계측 링크(DeviceLink) (ue_voice_quality.md §5, ue_sdk.md §4.7)
//
// 계측기가 단말을 단계별로 구동하는 줄 프로토콜 — 한 줄 = 명령 하나(공백 토큰), 한 줄 = JSON 이벤트 하나. 정의는 여기 하나다:
//   - cimsue-cli drive  : 줄 입출력 = stdin/stdout (계측기 real-ue 풀 — 워커가 자식 프로세스로 띄운다)
//   - DeviceLink        : 줄 입출력 = 단말이 먼저 연결하는 TLS 소켓 (계측기 device 풀 — 시험 모드 앱)
// 시험 대상 서버(CSP·CSC·CMP)를 거치지 않는다 — 단말 ↔ 계측기 워커 직접(ue_voice_quality.md §1).
//
//   명령: register | unregister | use <service> | dial <번호|URI> [video] | answer <call> [video] | reject <call> [code] | hangup <call>
//         hold <call> | resume <call> | dtmf <call> <digits> | transfer <call> <대상> | group_call <group> [listen] [emergency] [broadcast] [implicit]
//         floor_request <call> | floor_release <call> | affiliate <group> on|off [mcvideo] | mcvideo on|off | pickup <code> [number] | media mic|sample [<wav>]
//         video_call <group> [prearranged] [queueing] [implicit] | transmit_request <call> [priority] | transmit_release <call>
//         reception_accept <call> <userId> | reception_end <call> <userId>   (MCVideo 그룹 호·전송 제어 — TS 24.281 · TS 24.581)
//         stats [call] | quality <call> | quit
//   이벤트: ready{version,aor} · reg{service,state,code,reason,expires,rrd_ms} · incoming{call,from,called,video,mcptt,service,group}
//         · call{call,dir,state,code,reason,media,mcptt,service,video,by_us,group,srd_ms|sdd_ms,(disconnected: 통계 + 품질)} · floor{call,kind,subtype,t_us,...}
//         · transmission{call,kind,state,cause,t_us} · reception{call,kind,from,state,auto,cause,t_us}   (MCVideo — service = mcvideo 인 호)
//         · request{method,op,on,code,reason,ms,token} · stats{call,통계 + 품질}(활성 호마다 1 초) · quality{call,kind:callTerm|snapshot,품질}
//         · roster · dialog · sds · result{op,ok,call,code,reason}(명령마다 하나) · engine_stopped · exit
//   통계 = rx_pkts,tx_pkts,rx_loss,rx_bytes,jitter_us,stats_valid
//   품질 = codec,discard,loss_pct,discard_pct,jitter_max_ms,remote_loss_pct,remote_jitter_ms,rtd_ms,esd_ms,one_way_ms,r_lq,r_cq,mos_lq,mos_cq (없음 = -1)
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cimsue/engine.h"
#include "cimsue/export.h"
#include "cimsue/types.h"

namespace cimsue {

/** 이벤트 줄을 받는 곳. 여러 스레드(이벤트·통계·명령)에서 불린다 — 구현이 줄 단위로 직렬화한다. */
class CIMSUE_API LineSink {
public:
    virtual ~LineSink() = default;
    virtual void writeLine(const std::string& line) = 0;
};

/** 구동할 회선 — 서비스 이름(volte·voip·ptt) → 엔진 계정. */
struct DriveAccount {
    std::string service;
    int accountId = -1;
    std::string aor;
    std::string msisdn;
};

struct DriveOptions {
    std::vector<DriveAccount> accounts;   // 첫 항목이 기본 회선(`use` 로 바꾼다)
    bool appOwnedRegistration = false;    // true = 등록은 앱 소유 — register/unregister 는 거절(app_owned)
    std::string sampleFile;               // `media sample` 의 기본 WAV(기준 음원)
    int statsIntervalMs = 1000;           // 활성 호 stats 주기
    enum class Hangup { All, Driven, None };
    Hangup hangupOnStop = Hangup::All;    // stop 때 끊을 호 — All(cli) / 이 세션이 만들거나 받은 호만(Driven, 링크) / 없음
};

/** 줄 프로토콜 해석기 — Engine 관찰자로 이벤트를 줄로 내고, 명령 줄을 Engine 명령으로 옮긴다. */
class CIMSUE_API DriveSession {
public:
    DriveSession(Engine& engine, LineSink& sink, const DriveOptions& opts);
    ~DriveSession();
    DriveSession(const DriveSession&) = delete;
    DriveSession& operator=(const DriveSession&) = delete;

    /** 관찰자 등록 + 통계 스레드. emitReady 면 ready 이벤트를 먼저 낸다. */
    void start(bool emitReady);
    /** 명령 한 줄 → 명령마다 result 이벤트 하나(동기 결과 — dial/group_call/pickup 은 call id). */
    void handleLine(const std::string& line);
    /** quit 명령을 받았다. */
    bool quitRequested() const;
    /** 관찰자 해제·통계 정지·옵션에 따른 호 정리. 여러 번 불러도 된다. */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace drive {
/** 이벤트 필드 조각(앞에 "," 포함) — cli 결과 JSON 과 같은 이름을 쓰기 위한 공용 정의. */
CIMSUE_API std::string statsFields(const StreamStats& st);
CIMSUE_API std::string qualityFields(const CallQuality& q);
CIMSUE_API std::string jsonEscape(const std::string& s);
}  // namespace drive

// ── 계측 링크 (시험 모드 단말 → 계측기 워커) ──

enum class LinkState { Idle, Connecting, Connected, Disconnected, Refused };

struct DeviceLinkConfig {
    std::string host;                     // 계측기 워커 호스트(IP 또는 이름)
    int port = 7120;                      // 워커 Device.Port (컨트롤러 관측 수신 7110 과 겹치지 않게)
    std::string pairKey;                  // 연결 키(워커 Device.PairKey 와 같아야 한다, 선택)
    bool verifyServer = false;            // 워커 인증서 검증(caPem 앵커) — 끄면 최초 지문 고정(pinFile)
    std::string caPem;
    std::string pinFile;                  // TOFU 지문 저장 파일(비면 고정하지 않음)
    std::string deviceId;                 // 설치 고유 id(+sip.instance 와 같은 원천)
    std::string app, appVersion, platform, model;
    int reconnectMaxSec = 30;             // 재접속 백오프 상한(1·2·4 … 초)
};

class CIMSUE_API DeviceLinkListener {
public:
    virtual ~DeviceLinkListener() = default;
    /** 링크 상태 — detail = 워커 이름(Connected) 또는 사유(Disconnected·Refused). 링크 스레드에서 불린다. */
    virtual void onLinkState(LinkState state, const std::string& detail) { (void)state; (void)detail; }
};

/** 시험 모드 계측 링크 — 워커에 TLS 로 먼저 연결해 hello 를 보내고, 수락되면 DriveSession 으로 워커 명령을 실행한다.
 *  끊기면 이 세션이 만든 호만 끊고 백오프로 다시 붙는다. 연결 키가 틀리면(Refused) 재접속하지 않는다. */
class CIMSUE_API DeviceLink {
public:
    explicit DeviceLink(Engine& engine);
    ~DeviceLink();
    DeviceLink(const DeviceLink&) = delete;
    DeviceLink& operator=(const DeviceLink&) = delete;

    /** 링크 스레드 시작. opts.appOwnedRegistration·hangupOnStop 은 링크가 true·Driven 으로 고정한다. */
    Result start(const DeviceLinkConfig& cfg, const DriveOptions& opts, DeviceLinkListener* listener = nullptr);
    /** 링크를 닫고 스레드를 멈춘다(구동 중 호는 정리). */
    void stop();
    LinkState state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cimsue
