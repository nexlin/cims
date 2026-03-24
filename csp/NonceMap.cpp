#include "SipParserDefine.h"

#ifndef WIN32
#include <sys/time.h>
#endif

#include <time.h>

#include "MemoryDebug.h"
#include "NonceMap.h"
#include "SipMd5.h"
#include "TimeUtility.h"

CNonceMap gclsNonceMap;

CNonceMap::CNonceMap() {
}

/**
 * @ingroup CspServer
 * @brief 최신 nonce 값을 가져온다.
 * @param pszNonce		nonce 값을 저장할 변수
 * @param iNonceSize	pszNonce 변수의 크기
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CNonceMap::GetNewValue( char *pszNonce, int iNonceSize ) {
    char szNonce[60], szMd5[33];
    bool bFound = false;

    if ( iNonceSize < 33 ) return false;

    struct timeval sttTime;

    gettimeofday( &sttTime, NULL );

    for ( int i = 0;; ++i ) {
        snprintf( szNonce, sizeof( szNonce ), "%d::%u.%06u::%s", i, (unsigned int)sttTime.tv_sec,
                  (unsigned int)sttTime.tv_usec, PRIVATE_KEY );

        SipMd5String( szNonce, szMd5 );
        snprintf( szNonce, sizeof( szNonce ), "%s", szMd5 );

        m_clsMutex.acquire();
        NONCE_MAP::const_iterator it = m_clsMap.find( szNonce );
        if ( it == m_clsMap.end() ) {
            CNonceInfo clsNonceInfo;

            time( &clsNonceInfo.m_iTime );
            m_clsMap.insert( NONCE_MAP::value_type( szNonce, clsNonceInfo ) );
            bFound = true;
        }
        m_clsMutex.release();

        if ( bFound ) break;
    }

    snprintf( pszNonce, iNonceSize, "%s", szNonce );

    return true;
}

/**
 * @ingroup CspServer
 * @brief nonce 값이 존재하는지 검색한다.
 * @param pszNonce	nonce 문자열
 * @param bDelete		검색된 nonce 를 자료구조에서 삭제하면 true 를 입력하고 그렇지 않으면 false 를 입력한다.
 * @returns nonce 값이 검색되면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CNonceMap::Select( const char *pszNonce, bool bDelete ) {
    bool bFound = false;

    m_clsMutex.acquire();
    NONCE_MAP::iterator it = m_clsMap.find( pszNonce );
    if ( it != m_clsMap.end() ) {
        bFound = true;
        if ( bDelete ) m_clsMap.erase( it );
    }
    m_clsMutex.release();

    return bFound;
}

/**
 * @ingroup CspServer
 * @brief 입력한 시간 이전에 입력된 nonce 값을 모두 삭제한다.
 * @param iSecond		timeout 시간 (초단위)
 */
void CNonceMap::DeleteTimeout( int iSecond ) {
    NONCE_MAP::iterator it, itNext;
    time_t iTime;

    time( &iTime );
    m_clsMutex.acquire();
    for ( it = m_clsMap.begin(); it != m_clsMap.end(); ++it ) {
    LOOP_START:
        if ( iTime > ( it->second.m_iTime + iSecond ) ) {
            itNext = it;
            ++itNext;
            m_clsMap.erase( it );
            if ( itNext == m_clsMap.end() ) {
                break;
            } else {
                it = itNext;
                goto LOOP_START;
            }
        }
    }
    m_clsMutex.release();
}

/**
 * @ingroup CspServer
 * @brief nonce 개수를 리턴한다.
 * @returns nonce 개수를 리턴한다.
 */
int CNonceMap::GetCount() {
    int iCount;

    m_clsMutex.acquire();
    iCount = (int)m_clsMap.size();
    m_clsMutex.release();

    return iCount;
}
