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
    /** 마지막으로 검증 통과한 nonce count (RFC 7616 재사용 시 replay 방지, 0=미사용) */
    unsigned int m_uiLastNc = 0;
};

typedef std::map<std::string, CNonceInfo> NONCE_MAP;

class CNonceMap {
public:
    CNonceMap();

    bool GetNewValue( char *pszNonce, int iNonceSize );
    bool Select( const char *pszNonce, bool bDelete = true );
    /** 해시 검증 통과 후 호출 — nonce 존재 && nc 가 이전보다 크면 갱신하고 true (replay 차단) */
    bool CheckAndUpdateNc( const char *pszNonce, unsigned int uiNc );
    void DeleteTimeout( int iSecond );
    int GetCount();

private:
    NONCE_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CNonceMap gclsNonceMap;

#endif
