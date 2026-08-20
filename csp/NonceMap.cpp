#include "SipParserDefine.h"

#ifndef WIN32
#include <sys/time.h>
#endif

#include <openssl/rand.h>
#include <time.h>

#include "MemoryDebug.h"
#include "NonceMap.h"
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
    char szNonce[33];
    bool bFound = false;

    if ( iNonceSize < 33 ) return false;

    // nonce = CSPRNG 16 바이트의 hex (RFC 7616 §3.3 — 예측 불가 값). 시각·고정 상수 유도는 쓰지 않는다.
    for ( int i = 0;; ++i ) {
        unsigned char arrRand[16];
        if ( RAND_bytes( arrRand, sizeof( arrRand ) ) != 1 ) return false;
        for ( size_t j = 0; j < sizeof( arrRand ); ++j ) snprintf( szNonce + j * 2, 3, "%02x", arrRand[j] );

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
 * @brief 해시 검증 통과 후 nc(nonce count) 재사용 검사 — RFC 7616 nonce 재사용 지원.
 *   nonce 가 존재하고 nc 가 마지막 통과값보다 크면 갱신 후 true (replay 차단).
 * @param pszNonce	nonce 문자열
 * @param uiNc			요청의 nc 값 (16진수 파싱 결과)
 * @returns 통과하면 true, nonce 미존재 또는 nc 역행(replay)이면 false
 */
bool CNonceMap::CheckAndUpdateNc( const char *pszNonce, unsigned int uiNc ) {
    bool bOk = false;

    m_clsMutex.acquire();
    NONCE_MAP::iterator it = m_clsMap.find( pszNonce );
    if ( it != m_clsMap.end() && uiNc > it->second.m_uiLastNc ) {
        it->second.m_uiLastNc = uiNc;
        // 사용 중인 nonce 는 수명 연장 (sliding TTL) — 재등록 갱신 주기가 TTL 보다 길어도
        // 단말이 nc 증가 재사용을 계속하는 한 stale 재챌린지가 발생하지 않는다.
        time( &it->second.m_iTime );
        bOk = true;
    }
    m_clsMutex.release();

    return bOk;
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
