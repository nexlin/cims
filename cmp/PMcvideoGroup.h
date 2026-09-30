#ifndef __PMCVIDEO_GROUP_H__
#define __PMCVIDEO_GROUP_H__

#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <ctime>
#include <cstdint>
#include <functional>
#include "pbase.h"
#include "PTransmissionDefs.h"
#include "PMcvMemberPort.h"

// 서버 전송 제어 타이머·카운터 (TS 24.581 §11.1.3·§11.2.3 — 정본 mcvideo_tc_defs.yaml, 생성 상수 MCV_*).
//   T1 = 그룹 on-network-hang-timer · T5 = 그룹 on-network-reception-hang-timer · 나머지 = MCVideo service configuration
//   `<tc-timers-counters-R14>`. CSP 가 PTT_GROUP_ADD.tc_timers 로 싣고, 미지정 필드는 K5 기본값이다. t1Ms·t5Ms 0 = 그 타이머 미사용.
struct McvTimers {
    int t1Ms = MCV_T1_MS;    // Inactivity
    int t2Ms = MCV_T2_MS;    // Transmission Idle
    int t3Ms = MCV_T3_MS;    // Transmission Revoke
    int t4Ms = MCV_T4_MS;    // Transmission Granted
    int t5Ms = MCV_T5_MS;    // Reception Inactivity
    int t6Ms = MCV_T6_MS;    // Reception Granted
    int t11Ms = MCV_T11_MS;  // Stream Reception Idle
    int c2 = MCV_C2;         // Transmission Idle
    int c4 = MCV_C4;         // Transmission Granted
    int c6 = MCV_C6;         // Reception Granted
    int c7 = MCV_C7;         // Reception Accepted
    int c11 = MCV_C11;       // Count of active receivers for the stream
};

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
    bool queueing = false;   // SDP mc_queueing 협상 (1차는 송출 큐 없음 — 상한이면 거절 #1)
    int maxPriority = -1;    // 협상 송출 우선순위 상한 (-1 = members 의 prio)
    int maxRxPriority = -1;  // 협상 수신 우선순위 상한 (§14.3.6)
    int maxRxStreams = MCV_C9;  // C9 — 동시 수신 스트림 상한 (user profile MaxSimultaneousVideoStreams)
};

/**
 * MCVideo 그룹 호의 미디어 평면 (TS 24.581 — cmp_media_api.md §7.9, cmp.md §3.6).
 *
 * 그룹 종류는 PMcpttGroup 과 따로다 — floor 대신 송출 제어(§6.3.4·§6.3.5)와 수신 제어(§6.3.6·§6.3.7)를 가진다.
 * 서버 자원 키는 (service, group_id) 라 같은 그룹 id 에 MCPTT 그룹 호와 함께 선다(mcvideo.md §7 D6).
 * 멤버마다 전용 포트 유닛(PMcvMemberPort — audio·video·video RTCP·전송 제어)을 쓰고 그룹 공유 포트는 없다.
 *
 * 멤버는 두 단계다 — reserveMember(유닛 바인딩 + 전송 제어 SSRC 할당, ADD 로스터·JOIN ①) → addMember(SDP 교환 뒤 주소·협상 값,
 * JOIN ②). 송출 허가 전 미디어는 분배하지 않는다(허가된 송출만 수신자의 Active SSRC List 로 나간다 — §6.3.7).
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
    void setConfig(bool prearranged, int maxTransmitters, bool receptionAutomatic, const McvTimers& timers);
    // members 로스터 — sessionId → 그룹 문서 <user-priority> · role
    void updateRoster(const std::map<std::string, int>& priorities, const std::map<std::string, std::string>& roles);
    bool prearranged();
    int maxTransmitters();
    bool receptionAutomatic();
    McvTimers timers();
    // 녹취 자리 (record_dir/session_dir — 송출마다 슬롯 트랙은 녹취 단계에서)
    void setRecording(const std::string& recordDir, const std::string& sessionDir);
    std::string recordDir();
    void setLogCallback(LogFn fn) { _logFn = fn; }

    // ── 멤버 ──
    // 선할당 — 유닛 바인딩 + 전송 제어 SSRC(tc_ssrc) 할당. 멱등(같은 sessionId 는 같은 값). 반환 = tc_ssrc.
    unsigned int reserveMember(const std::string& sessionId, PMcvMemberPort* unit);
    // 주소·협상 값 등록/갱신 — reserveMember 가 선행해야 한다(아니면 false).
    bool addMember(const std::string& sessionId, const McvMemberDecl& decl);
    void removeMember(const std::string& sessionId);
    // 전 멤버 해제 — 그룹 해제 직전. 이후 늦게 도착한 패킷은 미등록 멤버로 버린다.
    void close();
    bool hasMember(const std::string& sessionId);
    int getMemberCount();      // 주소가 등록된 멤버
    int getReservedCount();    // 예약(포트 유닛) 멤버 — 주소 등록 멤버 포함
    unsigned int tcSsrcOf(const std::string& sessionId);

    // ── 수신 (멤버 유닛 → 리액터 스레드) ──
    void onMemberPacket(const std::string& memberId, McvChannel ch, const std::string& ip, int port, char* buf, int len);

    // ── 관측 ──
    time_t getCreatedTime() const { return _created; }
    time_t getLastActivityTime();
    long getSrcDrop();
    long getNoGrantDrop();
    long getControlRx();
    void collectNatLatched(std::vector<std::tuple<std::string, std::string, int>>& out);

    // ── SSRC — 프로세스 전역 유일 (TS 24.581 §6.3.4.3.3 d · 전송 제어 채널 tc_ssrc) ──
    // preferred = 단말이 offer a=ssrc 로 준 값(0 = 없음) — 전역에서 쓰이지 않으면 그 값, 아니면 새 값.
    static unsigned int AllocSsrc(unsigned int preferred = 0);
    static void FreeSsrc(unsigned int ssrc);

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
        int64_t followLogUsec = 0;       // dest follow 로그 rate-limit
    };

    static int _declPort(const McvMemberDecl& d, McvChannel ch);
    bool _natFormatOk(const Peer& peer, McvChannel ch, const std::string& ip, const char* buf, int len) const;
    void _natLatch(Peer& peer, McvChannel ch, const std::string& ip, int port);
    void _dropSrc(const char* what, const std::string& memberId, const std::string& ip, int port);
    // 전송 제어 채널 수신 (호출자가 _mutex 보유) — compound RTCP 를 나눠 APP 만 코덱(PTransmissionCodec)으로 풀고 관측한다.
    //   메시지 처리는 송출·수신 제어 상태 머신이 받는다.
    void _onControl(Peer& peer, const char* buf, int len);
    void _onControlMessage(Peer& peer, const char* buf, int len);
    void _releasePeer(Peer& peer);
    static int64_t _nowUsec();

    std::string _groupId;
    PMutex _mutex;
    time_t _created;
    time_t _lastActivity;
    LogFn _logFn;

    std::string _sesid;
    std::string _svc = "mcvideo";
    std::string _subid;
    bool _prearranged = false;          // group_type (기본 chat — mcvideo.md §7 D5)
    int _maxTransmitters = 1;           // 동시 송출 상한 (§4.1.1.1)
    bool _receptionAutomatic = false;   // reception_mode (기본 manual — §6.3.6.3.3)
    McvTimers _timers;
    std::string _recordDir, _recordSesDir;

    std::map<std::string, Peer> _members;             // sessionId → Peer
    std::map<std::string, int> _priorities;           // sessionId → <user-priority>
    std::map<std::string, std::string> _roles;        // sessionId → role

    long _srcDrop = 0;       // 미협상 소스·미등록 멤버 드롭 누적
    long _noGrantDrop = 0;   // 송출 허가 없는 미디어 드롭 누적
    long _controlRx = 0;     // 수신한 전송 제어 메시지 누적
    time_t _lastDropWarn = 0;
};

#endif // __PMCVIDEO_GROUP_H__
