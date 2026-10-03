/*
 * Group XML Parser Header
 */

#ifndef _XML_GROUP_H_
#define _XML_GROUP_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "SipMutex.h"

/**
 * @ingroup CspServer
 * @brief Group Information Class
 */

class CspPttUser {
public:
    CspPttUser( std::string id, unsigned int prio, std::string role = "participant", std::string mcpttId = "" )
        : _id( id ), _priority( prio ), _role( role ), _mcpttId( mcpttId ) {
    }
    ~CspPttUser();

    std::string _id;
    unsigned int _priority;

    /** TS 24.380 participant type: "chair" | "participant" */
    std::string _role;

    /** 멤버 MCPTT ID URI (비면 _id 사용) */
    std::string _mcpttId;

    /** 필수 멤버 — 그룹 문서 <entry> 의 <on-network-required>(TS 24.481 §7.2.4.2). 개시자 200 OK 전에 이 멤버의 200 을
     *  기다린다(TNG1, TS 24.379 §6.3.3.3) */
    bool _onNetworkRequired = false;

    /** 암시적 제휴 대상 — user profile <OnNetwork><ImplicitAffiliations> 의 이 그룹(TS 24.484 §8.3.2 · TS 24.379 §7.3.2
     * 13)). 서비스 인가(PTT REGISTER) 성공 때 참여 기능이 이 그룹에 제휴를 기록한다(§9.2.2.2.15). 멤버마다 관리자가
     * 정한다. */
    bool _implicitAffiliation = false;

    std::vector<std::string> _groups;

    bool IsChair() const {
        return _role == "chair";
    }
};

/** MCVideo 서비스 속성 — mcvideo_group_attrs 행(sql/migrate_mcvideo.sql). 값 = TS 24.481 §7.2.2 MCVideo <list-service>
 * 요소 (docs/design/features/mcvideo.md §5.1). 송출 제어 T1 은 MCPTT 의 _hangTimerSec 를 쓴다(TS 24.581 §11.1.3). */
struct CspMcVideoGroupAttrs {
    bool bInviteMembers = false;  // mcvideo-on-network-invite-members — true = prearranged, false = chat (§7 D5)
    int iMaxDurationSec = 3600;   // mcvideo-on-network-maximum-duration — TNG3 (0 = 무제한)
    int iMaxTransmitters = 2;     // mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members
    std::string strAudioEncodings = "AMR-WB";  // 쉼표 구분 선호순 (rtpmap encoding name)
    std::string strVideoEncodings = "H264";
    int iReceptionHangTimerSec = 30;  // on-network-reception-hang-timer — T5 (0 = 미사용)
    int iMinNumberToStart = 0;        // mcvideo-on-network-minimum-number-to-start
    int iGroupPriority = -1;          // mcvideo-on-network-group-priority 0..255 (-1 = 없음)
    bool bAllowConferenceState = true;
    /** 그룹 종류 문자열 — mcvideo-info session-type 과 비교한다(TS 24.281 §6.3.5.2 5)c)·d)) */
    const char *SessionType() const {
        return bInviteMembers ? "prearranged" : "chat";
    }
};

class CspPttGroup {
public:
    CspPttGroup();
    ~CspPttGroup();

    /** MCPTT group ID (그룹 식별자, SIP 주소·CMP·로그·캐시 런타임 키) */
    std::string _id;

    /** surrogate DB 키 (ptt_groups.id) — 멤버/affiliation DB 조회 조인용 */
    long long _dbId;

    /** Group Name */
    std::string _name;

    /** Member List (List of Group Members) */
    std::vector<std::shared_ptr<CspPttUser>> _pusers;

    /** 그룹 우선순위 (1=최고, 10=최저) */
    int _priority;

    /** 암호화 여부 (SRTP) */
    bool _encryption;

    /** 긴급통화 허용 여부 (allow-MCPTT-emergency-call) — condition(긴급·임박위험) 공통 게이트 */
    bool _emergencyCall;
    /** 긴급경보 허용 (allow-MCPTT-emergency-alert) */
    bool _emergencyAlert;
    /** 멤버의 conference 이벤트(RFC 4575) 구독 허용 (on-network-allow-conference-state, TS 24.481 §7.2.4.2) —
     *  초기 SUBSCRIBE 인가(TS 24.379 §10.1.3.4.1, 불허 403 Warning 138). 관제사 청취 범위는 별도 축(dispatch_center.md
     * §5.6) */
    bool _allowConferenceState;
    /** ad hoc 동적 그룹(Rel-18) — 비영속 in-memory, 통화 종료 시 GroupMap 에서 제거 */
    bool _isAdhoc;
    /** 즉석 개별 호(private)의 착신 INVITE 에 실을 Answer-Mode — 발신 INVITE 의 값(Auto·Manual, TS 24.379 §11.1.1.3.1.1
     *  18)d)). 비면 Auto(poc-settings 를 받지 않아 자동 개시로 본다). */
    std::string _answerMode;

    // ── MCData 그룹 메시징 게이트 (TS 24.481 §7.2.4.2) ──
    /** SDS 메시징 허용 (mcdata-allow-short-data-service) */
    bool _allowSds;
    /** 파일전송 허용 (mcdata-allow-file-distribution) */
    bool _allowFd;
    /** SDS payload 최대 크기 octets (mcdata-on-network-max-data-size-for-SDS, 0=무제한) */
    int _maxSdsSize;

    /** 소속 조직 코드 */
    std::string _orgCode;

    /** 세션 시작/종료 시간 (0=즉시/무기한) */
    time_t _sessionStart;
    time_t _sessionEnd;

    /** 세션 시퀀스 (그룹 재시작마다 증가, flow subid용) */
    int _sessionSeq;

    // ── 3GPP MCPTT (TS 24.379/24.481) ──
    /** 그룹 종류 (그룹 문서 on-network-invite-members — TS 24.481 §7.2.2): "prearranged" | "chat".
     *  즉석 세션은 내부값 "private"(1:1) — ad hoc 은 "prearranged" + _isAdhoc. 일제 통화는 그룹 종류가 아니라
     *  호 속성이다(GroupCallService 세션 속성, mcptt_broadcast_group_call.md). */
    std::string _groupType;

    /** 그룹 호 T4 Inactivity 초 (on-network-hang-timer, TS 24.481 §7.2.2 o — 0=미사용) */
    int _hangTimerSec;

    /** 그룹 호 최대 시간 TNG3 초 (on-network-maximum-duration, TS 24.481 §7.2.7 — 0=무제한) */
    int _maxDurationSec;

    /** 확인 통화 설정(acknowledged call setup, TS 24.379 §6.3.3.3·§10.1.1.4.2) — TS 24.481 §7.2.2 s)t)u).
     *  _minNumberToStart = <on-network-minimum-number-to-start>(개시자 200 OK 전 멤버 200 수, 0 = 기다리지 않음),
     *  _ackTimeoutSec = <on-network-timeout-for-acknowledgement-of-required-members>(TNG1 초),
     *  _ackAction = <on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>
     *  ("proceed" | "abandon" — 그 밖의 값은 abandon, §7.2.2 u)) */
    int _minNumberToStart;
    int _ackTimeoutSec;
    std::string _ackAction;

    /** on-network 그룹 여부 */
    bool _onNetwork;

    /** 최대 참여수 (0=무제한) */
    int _maxMembers;

    /** 그룹콜 수신 전 affiliation 요구 여부 */
    bool _requireAffiliation;

    /** 그룹 별칭/단축명 */
    std::string _alias;

    // ── floor 정책 (TS 24.380 동시 발언 — docs/api/cmp_media_api.md §7.7) ──
    /** "single" | "dual" | "multi" — CMP 는 미상 값을 BAD_REQUEST 로 거절한다 */
    std::string _floorPolicy;

    /** _floorPolicy=="multi" 일 때 동시 발언 상한 (CMP 계약 범위 2..8) */
    int _maxTalkers;

    /** floor 제어 유무 — ""(미지정=on)/"on"/"off". off=full-duplex(floor_port 미광고).
     *  private call(즉석 세션) 전용 파라미터 — DB 그룹 컬럼이 아니라 발신 offer 의 발언권 제어 채널
     *  (m=application … MCPTT) 유무로 정해진다(TS 24.379 §11.1.2.3.1). 그룹콜은 항상 on. */
    std::string _floorControl;

    // ── 그룹 소유 (3GPP TS 23.280 authorized user = 생성자 = 관리주체) ──
    /** 소유자 users.id (0=미지정) */
    int _authorizedUserId;

    /** 소유자 파생 MCPTT ID = 그 user 의 PTT 가입 MSISDN (비면 미지정) */
    std::string _authorizedUser;

    /** 그룹 생성 시각 (DB created_at, ISO8601; 비면 미지정) */
    std::string _createdAt;

    // ── MCVideo (한 그룹 = 서비스 집합, TS 23.280 §3) ──
    /** MCVideo 그룹인가 — mcvideo_group_attrs 행이 있다(TS 24.481 §7.2.8 MCVideo <service>) */
    bool _mcvideo = false;
    CspMcVideoGroupAttrs _mcvideoAttrs;

    /** Parsing method */
    bool load( std::string groupId );
    void Clear();
};

#endif
