#ifndef _SIP_SERVER_MAP_H_
#define _SIP_SERVER_MAP_H_

#include <map>

#include "SipMutex.h"
#include "CspSipServer.h"

// key = IP 주소 + 사용자 아이디
typedef std::map<std::string, CspSipServer> SIP_SERVER_MAP;

/**
 * @ingroup CspServer
 * @brief IP-PBX 정보 저장하는 자료구조
 */
class CSipServerMap {
public:
    CSipServerMap();
    ~CSipServerMap();

    bool Load();

    bool SetSipUserAgentRegisterInfo();

    bool Select( const char *pszIp, const char *pszUserId );
    bool SelectRoutePrefix( const char *pszTo, CspSipServer &clsCspSipServer, std::string &strTo );
    bool SelectIncomingRoute( const char *pszIp, const char *pszTo, std::string &strTo );

    bool Insert( CspSipServer &clsCspSipServer );
    bool Set( CSipServerInfo *pclsInfo, int iStatus );

    void GetString( CMonitorString &strBuf );

private:
    SIP_SERVER_MAP m_clsMap;
    CSipMutex m_clsMutex;

    bool ReadDir( const char *pszDirName );
    void GetKey( CspSipServer &clsCspSipServer, std::string &strKey );
    void GetKey( const char *pszIp, const char *pszUserId, std::string &strKey );
};

extern CSipServerMap gclsSipServerMap;

#endif
