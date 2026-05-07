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
    CspPttUser( std::string id, unsigned int prio ) : _id( id ), _priority( prio ) {
    }
    ~CspPttUser();

    std::string _id;
    unsigned int _priority;

    std::vector<std::string> _groups;
};

class CspPttGroup {
public:
    CspPttGroup();
    ~CspPttGroup();

    /** Group ID */
    std::string _id;

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

    /** Parsing method */
    bool load( std::string groupId );
    void Clear();
};

#endif
