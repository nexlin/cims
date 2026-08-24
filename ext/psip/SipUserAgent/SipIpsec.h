/*
 * SipIpsec — 단말측 IMS AKA + IPsec (sip_access_security.md §8.3, TS 33.203 §7, TS 24.229 §5.1.1)
 *
 * 등록마다 보호 포트쌍(port_uc/port_us)과 SPI 둘을 고르고 Security-Client 에 ipsec-3gpp 로 제안한다.
 * 401 의 Security-Server(ipsec-3gpp — 서버 spi/port)와 AKA 의 CK/IK 로 SA 4개를 커널(XfrmSa)에 설치한
 * 뒤 답안 REGISTER 를 port_uc 에서 보낸다. 확정(200 OK) 되면 스택의 식별 포트를 port_uc 로 바꿔 이후
 * 모든 요청이 그 소켓(SA 1)에서 나가게 한다. 재인증은 **새 포트쌍·새 SPI** 로 제안하고(§7.4.1a) 구 셋은
 * 200 OK 뒤 회수한다. 포트쌍은 스택의 UDP 리스너로 런타임에 연다. CAP_NET_ADMIN 필요.
 */
#ifndef _SIP_IPSEC_H_
#define _SIP_IPSEC_H_

#include <stdint.h>

#include <string>

#include "XfrmSa.h"

class CSipStack;

/** 보호 포트쌍 + 단말 SPI + 그 리스너 */
struct CSipIpsecPair {
    int iPortC = 0;  // port_uc — 내 요청 송신·응답 수신
    int iPortS = 0;  // port_us — 서버 요청 수신·응답 송신
    uint32_t iSpiC = 0;
    uint32_t iSpiS = 0;
    int iExtIdC = 0;  // 스택 UDP 리스너 ext id
    int iExtIdS = 0;
    bool Valid() const {
        return iPortC > 0 && iPortS > 0;
    }
};

class CSipIpsecClient {
public:
    bool m_bEnabled = false;
    std::string m_strAlg = XFRM_AUTH_HMAC_SHA1_96;
    std::string m_strEalg = XFRM_ENC_AES_CBC;
    /** 포트쌍 시작 — 0 이면 스택 로컬 포트 + 1. 재인증마다 +2 */
    int m_iPortBase = 0;

    /** 제안할 포트쌍/SPI 를 준비한다 (리스너 개방). 이미 준비돼 있으면 그대로. */
    bool EnsureNext( CSipStack *pclsStack, std::string &strError );
    /** Security-Client 값 — 준비된 제안으로 */
    std::string SecurityClient() const;

    /** 401: 서버 목록의 ipsec-3gpp 파라미터 + CK/IK 로 SA 셋 설치 (pending). 답안은 SendPortForAnswer() 에서. */
    bool OnChallenge( CSipStack *pclsStack, const std::string &strSecurityServer, const std::string &strServerIp,
                      const std::string &strCk, const std::string &strIk, int iLifetimeSec, std::string &strError );
    /** 답안 REGISTER 의 Via 포트 (pending 셋의 port_uc). 0 = pending 없음 */
    int SendPortForAnswer() const;
    /** 200 OK: pending → 현행, 구 셋 회수, 스택 식별 포트 = port_uc */
    void OnRegistered( CSipStack *pclsStack );
    /** 등록 실패/해제/종료: 전부 회수, 스택 식별 포트 복원 */
    void Teardown( CSipStack *pclsStack );
    bool Installed() const {
        return m_bCurInstalled;
    }

private:
    CSipIpsecPair m_clsNext, m_clsPending, m_clsCur;
    CXfrmSaSet m_clsPendingSet, m_clsCurSet;
    bool m_bPendingInstalled = false, m_bCurInstalled = false;
    int m_iPairSeq = 0;
    int m_iOrigLocalPort = 0;

    bool _openPair( CSipStack *pclsStack, CSipIpsecPair &clsPair, std::string &strError );
    void _closePair( CSipStack *pclsStack, CSipIpsecPair &clsPair );
    void _deleteSet( CXfrmSaSet &clsSet, bool &bInstalled, const char *pszWhy );
};

/** Security-Server 목록에서 ipsec-3gpp 항목의 서버 파라미터를 읽는다 */
bool SipIpsecParseServer( const std::string &strList, std::string &strAlg, std::string &strEalg, uint32_t &iSpiC,
                          uint32_t &iSpiS, int &iPortC, int &iPortS );

#endif
