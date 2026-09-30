#ifndef __PMCVIDEO_GROUP_H__
#define __PMCVIDEO_GROUP_H__

#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <ctime>
#include <cstdint>
#include <functional>
#include <atomic>
#include "pbase.h"
#include "PTransmissionDefs.h"
#include "PMcvControl.h"
#include "PMcvMemberPort.h"
#include "PFloorCrypto.h"
#include "PMediaCrypto.h"

class PSyncRtpRecorder;

// 멤버 선언 — PTT_JOIN(service:"mcvideo") 의 주소·협상 값 (cmp_media_api.md §7.9).
struct McvMemberDecl {
    std::string ip;          // user_ip
    int port = 0;            // user_port        — audio RTP
    int videoPort = 0;       // user_video_port  — video RTP (RTCP = +1)
    int controlPort = 0;     // user_control_port — 전송 제어 채널 (m=application … udp MCVideo)
    bool nat = false;        // user_nat — 목적지 latch 허용 (ue_nat_traversal.md §5)
    std::string sigIp;       // user_sig_ip — latch IP guard
    int ptOut = 0;           // user_pt       — egress audio PT (0 = 재작성 없음)
    int srcPt = 0;           // user_src_pt   — ingress audio PT (NAT 형식 검사)
    int videoPtOut = 0;      // user_video_pt — egress video PT
    std::string codec;       // user_codec (녹취 메타)
    std::string role = "participant";
    std::string userUri;     // MCVideo ID — 전송 제어 User ID 필드 값 (비면 sessionId)
    unsigned int userTcSsrc = 0;  // user_tc_ssrc — CMP → 멤버 전송 제어 RTCP 헤더 SSRC (0 = CMP 가 정한 값)
    unsigned int userAudioSsrc = 0;  // user_audio_ssrc — 멤버 offer 의 audio a=ssrc (송출 SSRC 할당 선호값, 0 = 없음)
    unsigned int userVideoSsrc = 0;  // user_video_ssrc — 멤버 offer 의 video a=ssrc
    bool queueing = false;   // SDP mc_queueing 협상 (§14.2.2) — 상한에서 대기열(Queue Position Info), 미협상이면 거절 #1
    bool recvOnly = false;   // recv_only — 그룹 문서 <on-network-recvonly> (송출 요청 거절 #5)
    int maxPriority = -1;    // 협상 송출 우선순위 상한 (-1 = members 의 prio)
    int maxRxPriority = -1;  // 협상 수신 우선순위 상한 (§14.3.6)
    int maxRxStreams = MCV_C9;  // C9 — 동시 수신 스트림 상한 (user profile MaxSimultaneousVideoStreams)
    int videoFb = -1;        // user_video_fb — 멤버 영상 SDP 가 협상한 키프레임 요청(MCV_FB_* 비트, -1 = 선언 없음 → PLI)
};

// 영상 키프레임 요청 종류 (RFC 4585 §4.2 `a=rtcp-fb … nack pli` · RFC 5104 §7.1 `ccm fir`) — 협상한 것만 보낸다.
enum { MCV_FB_PLI = 1, MCV_FB_FIR = 2 };

/**
 * MCVideo 그룹 호의 미디어 평면 (TS 24.581 — cmp_media_api.md §7.9, cmp.md §3.6).
 *
 * 그룹 종류는 PMcpttGroup 과 따로다 — floor 대신 송출 제어(§6.3.4·§6.3.5)와 수신 제어(§6.3.6·§6.3.7)를 가진다.
 * 서버 자원 키는 (service, group_id) 라 같은 그룹 id 에 MCPTT 그룹 호와 함께 선다(mcvideo.md §7 D6).
 * 멤버마다 전용 포트 유닛(PMcvMemberPort — audio·video·video RTCP·전송 제어)을 쓰고 그룹 공유 포트는 없다.
 *
 * 멤버는 두 단계다 — reserveMember(유닛 바인딩 + 전송 제어 SSRC 할당, ADD 로스터·JOIN ①) → addMember(SDP 교환 뒤 주소·협상 값,
 * JOIN ②). 주소가 등록되면 전송 제어 참가자가 된다 — 송출·수신 제어 상태 머신은 PMcvControl(순수 로직)이고 그룹이 락 아래 부른다.
 * 허가된 송출만 수신자의 Active SSRC List 대로 나간다(§6.3.7) — 내보낼 때 SSRC 를 그 송출의 할당값으로, PT 를 수신 leg 값으로 찍는다.
 *
 * 락 순서 = 그룹 → 유닛. 그룹 락을 쥔 채 서버(PCmpServer) 락을 잡지 않는다.
 */
class PMcvideoGroup {
public:
    // flow 로그 — 세션 상관 값(sesid·service·subid)을 그룹이 직접 싣는다(리액터 스레드에서 서버 캐시를 읽지 않게).
    using LogFn = std::function<void(const char* from, const char* to, const char* proto, const char* label,
                                     const char* body, const std::string& sesid, const std::string& svc,
                                     const std::string& subid)>;

    explicit PMcvideoGroup(const std::string& groupId);
    ~PMcvideoGroup();

    const std::string& groupId() const { return _groupId; }

    // ── 세션 속성 (PTT_GROUP_ADD·MODIFY) ──
    void setSessionMeta(const std::string& sesid, const std::string& svc, const std::string& subid);
    std::string sessionSesid();
    void setConfig(bool prearranged, int maxTransmitters, bool receptionAutomatic, McvCallType callType,
                   const McvTimers& timers);
    // members 로스터 — sessionId → 그룹 문서 <user-priority> · role
    void updateRoster(const std::map<std::string, int>& priorities, const std::map<std::string, std::string>& roles);
    bool prearranged();
    int maxTransmitters();
    bool receptionAutomatic();
    McvCallType callType();
    McvTimers timers();
    // 녹취 자리 (record_dir/session_dir — 송출마다 슬롯 트랙은 녹취 단계에서)
    void setRecording(const std::string& recordDir, const std::string& sessionDir);
    std::string recordDir();
    // 전송 제어 SRTCP 그룹 키 (PTT_GROUP_ADD.tc_crypto — floor_crypto 와 같은 형식, TS 33.180). key/salt/mki = 디코드된 바이트열.
    //   같은 구성 재선언은 컨텍스트(SRTCP index·재전송 창)를 유지한다. 실패 시 err.
    bool setTcCrypto(const std::string& alg, const std::string& key, const std::string& salt, const std::string& mki,
                     std::string& err);
    void setLogCallback(LogFn fn) { _logFn = fn; }
    // 송출자 집합 변경 → TRANSMITTERS 이벤트 · T1/T5 만료 → TRANSMISSION_INACTIVITY 이벤트 (cmp_media_api.md §8).
    //   그룹 _mutex 를 쥔 채 부른다(PMcpttGroup 콜백과 같은 규약 — PCmpServer::_mutex 를 다시 잡지 않는다, sesid/service 는 그룹이 싣는다).
    using TransmittersFn = std::function<void(const std::string& groupId, const std::vector<McvTransmitter>& transmitters,
                                              const std::string& sesid, const std::string& svc)>;
    using InactivityFn = std::function<void(const std::string& groupId, const char* timer, const std::string& sesid,
                                            const std::string& svc)>;
    void setTransmittersCallback(TransmittersFn fn) { _onTransmitters = fn; }
    void setInactivityCallback(InactivityFn fn) { _onInactivity = fn; }

    // ── 멤버 ──
    // 선할당 — 유닛 바인딩 + 전송 제어 SSRC(tc_ssrc) 할당. 멱등(같은 sessionId 는 같은 값). 반환 = tc_ssrc.
    unsigned int reserveMember(const std::string& sessionId, PMcvMemberPort* unit);
    // 주소·협상 값 등록/갱신 — reserveMember 가 선행해야 한다(아니면 false). implicitRequest = JOIN implicit_request
    //   (새 prearranged 세션 개시의 암묵적 송출 요청) — res 에 허가 여부·SSRC 쌍을 채운다(JOIN 응답 granted·audio_ssrc·video_ssrc).
    bool addMember(const std::string& sessionId, const McvMemberDecl& decl, bool implicitRequest = false,
                   PMcvControl::ImplicitResult* res = nullptr);
    // 멤버 보호 키 — addMember 보다 먼저 건다(참가 등록이 곧 Idle·Notification 을 보낸다). reserveMember 가 선행해야 한다.
    //   tc_crypto = 이 멤버의 전송 제어 SRTCP 키(CSK — 없으면 그룹 키, TS 33.180 §9.4) · media_crypto[_video] = 멤버 SRTP
    //   (media_security.md §6.3 — rx = UE 상향, tx = CMP 하향). 같은 구성 재선언은 세션 유지, 변경은 재생성. 실패 시 err(평문 폴백 없음).
    bool setMemberTcCrypto(const std::string& sessionId, const std::string& alg, const std::string& key,
                           const std::string& salt, const std::string& mki, std::string& err);
    bool setMemberMediaCrypto(const std::string& sessionId, bool video, const std::string& alg, const std::string& rxKey,
                              const std::string& rxSalt, const std::string& txKey, const std::string& txSalt,
                              std::string& err);
    void removeMember(const std::string& sessionId);
    // 전 멤버 해제 — 그룹 해제 직전. 이후 늦게 도착한 패킷은 미등록 멤버로 버린다.
    void close();
    bool hasMember(const std::string& sessionId);
    int getMemberCount();      // 주소가 등록된 멤버
    int getReservedCount();    // 예약(포트 유닛) 멤버 — 주소 등록 멤버 포함
    unsigned int tcSsrcOf(const std::string& sessionId);

    // ── 수신 (멤버 유닛 → 리액터 스레드) ──
    void onMemberPacket(const std::string& memberId, McvChannel ch, const std::string& ip, int port, char* buf, int len);
    // 전송 제어 타이머 틱 (T1~T6·T11 — PCmpServer 의 100 ms 클록)
    void tick();
    // 살아 있는 MCVideo 그룹 수 — 틱 클록이 그룹 표를 잡을지 판단한다(락 없음)
    static int activeGroups() { return _activeGroups.load(); }

    // ── 관측 ──
    time_t getCreatedTime() const { return _created; }
    time_t getLastActivityTime();
    long getSrcDrop();
    long getNoGrantDrop();
    long getControlRx();
    long getCryptoDrop();      // SRTP·SRTCP 인증 실패/재전송으로 버린 패킷
    long getKeyframeRequests();   // 송출자에게 보낸 키프레임 요청(PLI·FIR — 수신자 피드백 전달 + 수신 시작)
    int getTransmitterCount();
    int getReceptionCount();
    void collectNatLatched(std::vector<std::tuple<std::string, std::string, int>>& out);

private:
    struct Peer {
        std::string id;
        PMcvMemberPort* unit = nullptr;  // PCmpServer 소유 — 그룹은 참조만
        unsigned int tcSsrc = 0;         // CMP 가 이 멤버에게서 기대하는 RTCP 헤더 SSRC (JOIN 응답 tc_ssrc)
        bool addressed = false;          // addMember 로 주소 등록됨
        McvMemberDecl decl;              // 마지막 선언 원본 (latch 와 무관하게 보존)
        // 채널별 현재 목적지 — 선언 값에서 시작해 nat 멤버는 형식 검사를 통과한 소스로 추종한다
        std::string dstIp[4];
        int dstPort[4] = {0, 0, 0, 0};
        bool latched[4] = {false, false, false, false};
        unsigned int uaTcSsrc = 0;       // 멤버가 전송 제어 헤더에 실어 보낸 SSRC (관측)
        std::shared_ptr<PFloorCrypto> tcCrypto;       // 전송 제어 SRTCP (CSK) — 없으면 그룹 키
        std::string tcCryptoSig;                      // 같은 구성 재선언 판정 (alg·key·salt·mki)
        std::shared_ptr<PMediaCrypto> mediaCrypto;       // audio SRTP (null = 평문 leg)
        std::shared_ptr<PMediaCrypto> mediaCryptoVideo;  // video SRTP
        int64_t followLogUsec = 0;       // dest follow 로그 rate-limit
        unsigned int rxVideoSsrc = 0;    // 멤버가 보낸 영상 RTP 의 원래 SSRC — 키프레임 요청의 media source(분배 때 찍는 할당값이 아님)
        int64_t keyReqMs = 0;            // 이 멤버(송출자)에게 마지막으로 키프레임을 요청한 시각 (kKeyReqMinMs)
        unsigned char firSeq = 0;        // CMP → 이 멤버 FIR 의 Seq nr (RFC 5104 §4.3.1.1 — 새 요청마다 +1)
    };

    // 송출자 한 명에게 보내는 키프레임 요청의 최소 간격(ms) — 수신자 여럿의 PLI·FIR 를 하나로 모은다(CIMS 값, RFC 4585 는 정하지 않는다).
    static const int kKeyReqMinMs = 500;

    static int _declPort(const McvMemberDecl& d, McvChannel ch);
    bool _natFormatOk(const Peer& peer, McvChannel ch, const std::string& ip, const char* buf, int len) const;
    void _natLatch(Peer& peer, McvChannel ch, const std::string& ip, int port);
    void _dropSrc(const char* what, const std::string& memberId, const std::string& ip, int port);
    // 전송 제어 채널 수신 (호출자가 _mutex 보유) — compound RTCP 를 나눠 APP 만 코덱(PTransmissionCodec)으로 풀고 관측한다.
    //   메시지 처리는 송출·수신 제어 상태 머신이 받는다.
    void _onControl(Peer& peer, const char* buf, int len);
    void _onControlMessage(Peer& peer, const char* buf, int len);
    PFloorCrypto* _tcCryptoFor(Peer& peer);   // 멤버 키(CSK) > 그룹 키 > 평문(null)
    void _cryptoDropLog(const char* what, const Peer& peer);
    // 영상 RTCP 수신 (호출자가 _mutex 보유) — 수신자의 PSFB PLI·FIR(RFC 4585 §6.3.1 · RFC 5104 §4.3.1)만 가리키는 송출자에게 넘긴다.
    //   RR·SDES·SR 등 나머지는 해석하지 않고 버린다(보고는 CMP 가 끝낸다 — 송출자에게 수신자별 보고를 옮기지 않는다).
    void _onVideoRtcp(Peer& peer, char* buf, int len);
    // 할당 video SSRC 가 가리키는 송출에 키프레임 요청 — 요청자가 그 송출을 받고 있을 때만
    void _forwardKeyframeRequest(const Peer& requester, unsigned int allocatedSsrc, bool fir);
    // 송출자에게 CMP 가 보내는 키프레임 요청 — RR + SDES CNAME + PSFB(PLI 또는 FIR) 복합 패킷(RFC 4585 §3.1), 송출자 영상 SRTCP 키로 보호
    void _requestKeyframe(Peer& sender, bool fir, const char* why);
    // 녹취 (recording.md §3.3 — 세션 디렉터리·슬롯 트랙, 호출자가 _mutex 보유). 세그먼트 = 송출이 이어지는 구간(송출자 0 → 1 에서 열고
    //   다시 0 이면 닫는다), 송출자마다 슬롯 하나(audio/video, audioK/videoK) — 동시 송출은 슬롯이 여럿이다. 송출자 집합 변경(훅)이 계기다.
    void _recOnTransmitters(const std::vector<McvTransmitter>& v);
    void _recStop();
    static std::string _recTrack(int slot, bool video);
    // 허가된 송출의 미디어를 Active SSRC List 대로 분배한다 (호출자가 _mutex 보유)
    void _distribute(const Peer& sender, McvChannel ch, unsigned int ssrc, const char* buf, int len);
    // PMcvControl → 멤버 제어 채널 (호출자가 _mutex 보유)
    void _sendControl(const std::string& memberId, int app, int subtype, const std::vector<McvTlv>& fields);
    McvParticipantDecl _ctlDecl(const std::string& sessionId, const McvMemberDecl& d) const;
    void _releasePeer(Peer& peer);
    static int64_t _nowUsec();
    static int64_t _nowMs();   // 단조 시계 — 전송 제어 타이머

    static std::atomic<int> _activeGroups;

    std::string _groupId;
    PMutex _mutex;
    time_t _created;
    time_t _lastActivity;
    LogFn _logFn;
    TransmittersFn _onTransmitters;
    InactivityFn _onInactivity;
    PMcvControl _ctl;                   // 송출·수신 제어 상태 머신 (TS 24.581 §6.3.4~§6.3.7)

    std::string _sesid;
    std::string _svc = "mcvideo";
    std::string _subid;
    bool _prearranged = false;          // group_type (기본 chat — mcvideo.md §7 D5)
    int _maxTransmitters = 1;           // 동시 송출 상한 (§4.1.1.1)
    bool _receptionAutomatic = false;   // reception_mode (기본 manual — §6.3.6.3.3)
    McvCallType _callType = MCV_CALL_NORMAL;   // call_type (긴급·임박 = Transmission Indicator · automatic 수신)
    McvTimers _timers;
    std::string _recordDir, _recordSesDir;
    PSyncRtpRecorder* _recorder = nullptr;        // record_dir 이 있고 첫 송출이 허가될 때 만든다
    std::map<std::string, int> _recSlots;         // 송출 멤버 → 녹취 슬롯
    int _recTrackSlots = 0;                       // 등록한 슬롯 트랙 수

    std::map<std::string, Peer> _members;             // sessionId → Peer
    std::map<std::string, int> _priorities;           // sessionId → <user-priority>
    std::map<std::string, std::string> _roles;        // sessionId → role

    long _srcDrop = 0;       // 미협상 소스·미등록 멤버 드롭 누적
    long _noGrantDrop = 0;   // 송출 허가 없는 미디어 드롭 누적 (payload 있는 RTP)
    long _controlRx = 0;     // 수신한 전송 제어 메시지 누적
    long _cryptoDrop = 0;    // SRTP·SRTCP 해제 실패 누적
    long _keyReq = 0;        // 송출자에게 보낸 키프레임 요청 누적
    unsigned int _fbSsrc = 0;   // CMP 가 영상 RTP 세션에서 쓰는 자기 SSRC(RR·PLI·FIR 의 packet sender — 첫 요청 때 할당, close 에 반환)
    PFloorCrypto _tcCrypto;          // 전송 제어 SRTCP 그룹 키 (tc_crypto)
    std::string _tcCryptoSig;
    time_t _lastDropWarn = 0;
};

#endif // __PMCVIDEO_GROUP_H__
