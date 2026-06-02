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

    std::vector<std::string> _groups;

    bool IsChair() const {
        return _role == "chair";
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

    /** Video relay enabled (H.264) */
    bool _videoEnabled;

    /** 그룹 우선순위 (1=최고, 10=최저) */
    int _priority;

    /** 암호화 여부 (SRTP) */
    bool _encryption;

    /** 긴급통화 허용 여부 */
    bool _emergencyCall;

    /** 소속 조직 코드 */
    std::string _orgCode;

    /** 세션 시작/종료 시간 (0=즉시/무기한) */
    time_t _sessionStart;
    time_t _sessionEnd;

    /** 세션 시퀀스 (그룹 재시작마다 증가, flow subid용) */
    int _sessionSeq;

    // ── 3GPP MCPTT (TS 24.379/24.481) ──
    /** session-type: "prearranged" | "chat" | "broadcast" */
    std::string _groupType;

    /** on-network 그룹 여부 */
    bool _onNetwork;

    /** 최대 참여수 (0=무제한) */
    int _maxMembers;

    /** 그룹콜 수신 전 affiliation 요구 여부 */
    bool _requireAffiliation;

    /** 그룹 별칭/단축명 */
    std::string _alias;

    /** Parsing method */
    bool load( std::string groupId );
    void Clear();
};

#endif
