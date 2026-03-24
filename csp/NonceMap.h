#ifndef _NONCE_MAP_H_
#define _NONCE_MAP_H_

#include <stdio.h>

#include <map>
#include <string>

#include "SipMutex.h"

#ifndef WIN32
#include <sys/time.h>
#endif

#define PRIVATE_KEY "hotyoungsipserver"

class CNonceInfo {
public:
    /** nonce 저장 시간 */
    time_t m_iTime;
};

typedef std::map<std::string, CNonceInfo> NONCE_MAP;

class CNonceMap {
public:
    CNonceMap();

    bool GetNewValue( char *pszNonce, int iNonceSize );
    bool Select( const char *pszNonce, bool bDelete = true );
    void DeleteTimeout( int iSecond );
    int GetCount();

private:
    NONCE_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CNonceMap gclsNonceMap;

#endif
