#ifndef _NONCE_MAP_H_
#define _NONCE_MAP_H_

#include <stdio.h>

#include <map>
#include <string>

#include "SipMutex.h"

#ifndef WIN32
#include <sys/time.h>
#endif


class CNonceInfo {
public:
    /** nonce 저장 시간 */
    time_t m_iTime;
    /** 마지막으로 검증 통과한 nonce count (RFC 7616 재사용 시 replay 방지, 0=미사용) */
    unsigned int m_uiLastNc = 0;

    // IMS AKA 챌린지 (sip_access_security.md §8.2, RFC 3310) — nonce = base64(RAND‖AUTN) 에 결부된 답안.
    //   m_bAka=false 면 SIP Digest nonce. XRES 는 검증(H(A1)=MD5(impi:realm:XRES)) 에만 쓰고 로그에 남기지 않는다.
    bool m_bAka = false;
    std::string m_strUser;     // 챌린지를 발급한 신원 (From user) — 다른 신원의 답안 재사용 차단
    std::string m_strRandHex;  // RAND (AUTS 재동기 요청에 함께 보낸다)
    std::string m_strXresHex;  // XRES
};

typedef std::map<std::string, CNonceInfo> NONCE_MAP;

class CNonceMap {
public:
    CNonceMap();

    bool GetNewValue( char *pszNonce, int iNonceSize );
    bool Select( const char *pszNonce, bool bDelete = true );
    /** AKA 챌린지 nonce 등록 — nonce 문자열은 호출자가 만든다(base64(RAND‖AUTN)). 같은 값이 있으면 덮어쓴다. */
    void InsertAka( const std::string &strNonce, const std::string &strUser, const std::string &strRandHex,
                    const std::string &strXresHex );
    /** nonce 조회 + 정보 복사. bDelete 는 Select 와 같은 규칙(qop 재사용 nonce 는 보존). */
    bool SelectInfo( const char *pszNonce, CNonceInfo &clsInfo, bool bDelete );
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
