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

/** 서버 제안 목록 (Security-Server) 의 기본값 — tls. ipsec-3gpp 는 AV 를 받은 뒤 파라미터와 함께
 *  BuildIpsecServerList 로 만들어 Issue(user, list) 한다 (§8.3, P4). */
#define SEC_AGREE_SERVER_LIST "tls;q=0.1"

/** 미디어 보안 서버 항목 (TS 24.229 mediasec 파라미터 — media_security.md §4.1). 단말이
 *  Security-Client 에 sdes-srtp;mediasec 를 선언했을 때만 서버 목록에 병기한다 — 채널
 *  메커니즘(tls/ipsec-3gpp) 협상과 같은 헤더를 공유하되 mediasec 파라미터로 구분된다. */
#define SEC_AGREE_MEDIASEC_ENTRY "sdes-srtp;mediasec;q=0.05"

/** Security-Client 목록에 sdes-srtp(mediasec 파라미터) 선언이 있는가 — 미디어 SRTP 능력 학습.
 *  학습 결과는 등록 바인딩(CUserInfo.m_bMediaSecSdes)에 결부된다 (media_security.md §4.1). */
bool SecAgreeHasMediaSecSdes( const std::string &strClient );

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

/** Security-Client 의 ipsec-3gpp 제안 하나 (RFC 3329 §2.2 + TS 33.203 §7.2 파라미터, 단말 관점 값) */
struct SecAgreeIpsecOffer {
    bool bValid = false;
    std::string strAlg;   // hmac-sha-1-96 | hmac-md5-96
    std::string strEalg;  // aes-cbc | null
    uint32_t iSpiC = 0;   // spi_uc
    uint32_t iSpiS = 0;   // spi_us
    int iPortC = 0;       // port_uc
    int iPortS = 0;       // port_us
    double dQ = 1.0;
};

/** Security-Client 목록에서 서버가 지원하는 최선의 ipsec-3gpp 제안 — q 높은 것, 동률이면 hmac-sha-1-96 우선,
 *  그 다음 strEalgPref. 없으면 bValid=false. bAnyIpsec: (지원 불가 포함) ipsec-3gpp 제안이 있었는가 */
SecAgreeIpsecOffer SelectIpsecOffer( const std::string &strClient, const std::string &strEalgPref, bool &bAnyIpsec );

/** ipsec-3gpp 서버 목록 — 선택된 alg/ealg 에 서버 spi/port 를 실어 첫 항목으로, tls 를 뒤에 */
std::string BuildIpsecServerList( const SecAgreeIpsecOffer &clsOffer, uint32_t iSpiPc, uint32_t iSpiPs, int iPortPc,
                                  int iPortPs );

/** 목록(발급 원문 = Security-Verify echo)의 첫 메커니즘이 ipsec-3gpp 인가 — 협상 결과 판정 */
bool SecAgreeListIsIpsec( const std::string &strList );

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
    /** 챌린지에 실을 서버 목록을 발급·보관한다 (재발급은 덮어쓴다). 기본 = tls 뿐 */
    std::string Issue( const std::string &strUser );
    /** 목록을 지정해 발급 (ipsec-3gpp 파라미터 포함 목록) */
    std::string Issue( const std::string &strUser, const std::string &strList );
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
