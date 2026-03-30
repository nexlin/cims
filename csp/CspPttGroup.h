/*
 * Group XML Parser Header
 */

#ifndef _XML_GROUP_H_
#define _XML_GROUP_H_

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "SipMutex.h"

/**
 * @ingroup CspServer
 * @brief Group Information Class
 */

class CspPttUser {
public:
    CspPttUser(std::string id, unsigned int prio) : _id(id), _priority(prio) {}
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

    /** Parsing method */
    bool load( std::string groupId );
    void Clear();
};


#endif
