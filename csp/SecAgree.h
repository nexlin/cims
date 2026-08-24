/*
 * SecAgree — RFC 3329 보안 메커니즘 협상 (sip_access_security.md §8.1, P2)
 *
 * TS 24.229 §5.1.1.5.1 프로파일: 초기 REGISTER 의 Security-Client(+Require/Proxy-Require: sec-agree)
 * → 401 에 Security-Server → 보호 채널(TLS) 위 재-REGISTER 의 Security-Verify 를 서버가 실은 원문과
 * 바이트 대조. 불일치·부재는 494 (Security Agreement Required) 로 협상 재시작.
 */
#ifndef _SEC_AGREE_H_
#define _SEC_AGREE_H_

#include <time.h>

#include <map>
#include <string>

#include "SipMessage.h"
#include "SipMutex.h"

/** 서버 제안 목록 (Security-Server). 도입 시점 기준 tls 뿐 — ipsec-3gpp 는 P4 에서 추가한다. */
#define SEC_AGREE_SERVER_LIST "tls;q=0.1"

/** 요청의 sec-agree 헤더 요약 */
struct SecAgreeRequest {
    bool bRequire;    // Require 또는 Proxy-Require 에 sec-agree
    bool bHasClient;  // Security-Client 존재
    bool bHasVerify;  // Security-Verify 존재
    std::string strClient;
    std::string strVerify;

    SecAgreeRequest() : bRequire( false ), bHasClient( false ), bHasVerify( false ) {
    }
    /** 단말이 sec-agree 를 쓰고 있는가 (헤더 하나라도) */
    bool Requested() const {
        return bRequire || bHasClient || bHasVerify;
    }
};

SecAgreeRequest ParseSecAgree( CSipMessage *pclsMessage );

enum ESecAgreeVerify {
    E_SECAGREE_NONE = 0,  // 이 신원에 발급한 서버 목록이 없다 (협상 없이 Security-Verify 만 옴)
    E_SECAGREE_OK,
    E_SECAGREE_MISMATCH,  // 강등 시도 또는 변조
};

/**
 * @brief 신원별로 챌린지에 실은 Security-Server 원문을 보관하고 Security-Verify 와 대조한다.
 *   키는 From user(AoR user 파트) — 챌린지와 재-REGISTER 사이의 짧은 창만 살면 되므로 nonce 와
 *   같은 수명으로 회수한다.
 */
class CSecAgreeMap {
public:
    /** 챌린지에 실을 서버 목록을 발급·보관한다 (재발급은 덮어쓴다). */
    std::string Issue( const std::string &strUser );
    ESecAgreeVerify Verify( const std::string &strUser, const std::string &strVerify );
    void Delete( const std::string &strUser );
    void DeleteTimeout( int iSecond );

private:
    struct Entry {
        std::string strServer;
        time_t iTime;
    };
    std::map<std::string, Entry> m_clsMap;
    CSipMutex m_clsMutex;
};

extern CSecAgreeMap gclsSecAgreeMap;

#endif
