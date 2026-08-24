/*
 * IpsecSaSet — 등록 단위 IPsec SA 셋(TS 33.203 §7.1 의 4개)의 생명주기 (sip_access_security.md §8.3, P4)
 *
 *   임시(401 발급 시 커널 설치) → 확정(답안 REGISTER 통과, 수명 = expires+30) → 연장(갱신) → 회수
 *   (해제·만료·교체·494·임시 유예 초과). 커널 프로그래밍은 psip XfrmSa, 소유 표식은 reqid
 *   (Setup.Ipsec.ReqIdBase 로부터 채번 — 기동 시 그 범위를 일괄 회수해 잔류 상태를 지운다).
 *   재인증(새 401)은 새 셋을 임시로 만들고, 확정되면 구 셋은 retiring — 새 셋 위 첫 요청 수신 후
 *   또는 64×T1 뒤 회수한다 (TS 24.229 §5.2.2.1).
 */
#ifndef _IPSEC_SA_SET_H_
#define _IPSEC_SA_SET_H_

#include <stdint.h>
#include <time.h>

#include <map>
#include <set>
#include <string>

#include "SipMutex.h"
#include "XfrmSa.h"

struct SecAgreeIpsecOffer;

/** 임시 SA 셋 유예 기본값 — 64×T1 (TS 24.229 §5.2.2.1) */
#define IPSEC_TEMP_SA_TIMEOUT_SEC 32
/** 확정 SA 수명 여유 (TS 33.203 §7.4) */
#define IPSEC_SA_LIFETIME_GRACE_SEC 30
/** 해제 응답이 SA 위로 나갈 유예 */
#define IPSEC_RELEASE_GRACE_SEC 2

struct CIpsecSaSetInfo {
    uint32_t iReqId = 0;
    std::string strUser;
    CXfrmSaSet clsSet;              // CSP 관점: local_s=port_ps, local_c=port_pc, remote_c=port_uc, remote_s=port_us
    std::string strSecurityServer;  // 이 셋을 실은 Security-Server 원문 (확정 뒤 갱신 REGISTER 의 Verify 대조용)
    time_t iCreateTime = 0;
    bool bEstablished = false;
    time_t iDeleteAt = 0;  // 0 = 예약 없음. retiring/해제 유예의 회수 시각
};

class CIpsecSaSetMap {
public:
    /** 기동 — reqid 범위 일괄 회수 + 자기점검(더미 셋). 실패해도 기동은 계속(ipsec-3gpp 미제시). */
    void Init();
    /** 종료 — 이 프로세스가 만든 state/policy 전부 회수 */
    void Shutdown();
    bool IsAvailable() const {
        return m_bAvailable;
    }

    /** 초기 REGISTER: 단말 제안 + AV 의 IK/CK 로 임시 셋을 커널에 설치한다. 같은 user 의 기존 임시 셋은 교체.
     *  @param strUeIp  단말 실소스 IP (Via received)  @param strIk/strCk 16B 이진 */
    bool CreateTemp( const std::string &strUser, const SecAgreeIpsecOffer &clsOffer, const std::string &strUeIp,
                     const std::string &strIk, const std::string &strCk, CIpsecSaSetInfo &clsOut,
                     std::string &strError );
    /** 답안 REGISTER 가 user 의 임시 셋 위(UE ip, port_uc)로 왔는가 */
    bool MatchTemp( const std::string &strUser, const std::string &strIp, int iPort, CIpsecSaSetInfo *pclsOut = NULL );
    /** 임시 → 확정. 수명 부여(커널 갱신). 같은 user 의 기존 확정 셋은 retiring. */
    bool Establish( const std::string &strUser, uint32_t iReqId, int iLifetimeSec );
    /** 요청이 user 의 확정(또는 retiring) 셋 위(UE ip, port_uc)로 왔는가 — 게이트. 새 셋 위 첫 요청이면
     *  retiring 셋의 회수를 앞당긴다. */
    bool MatchEstablished( const std::string &strUser, const std::string &strIp, int iPort,
                           CIpsecSaSetInfo *pclsOut = NULL );
    /** 갱신 — 수명 재부여 */
    bool Extend( uint32_t iReqId, int iLifetimeSec );
    /** 회수 예약 (iGraceSec 뒤 커널에서 제거) */
    void Release( uint32_t iReqId, int iGraceSec );
    /** user 의 임시 셋 즉시 회수 (494 등) */
    void ReleaseTemp( const std::string &strUser );
    /** user 의 셋 전부 회수 예약 */
    void ReleaseUser( const std::string &strUser, int iGraceSec );
    bool Select( uint32_t iReqId, CIpsecSaSetInfo &clsOut );
    /** 이 셋을 실어 보낸 Security-Server 원문 보관 */
    void SetSecurityServer( uint32_t iReqId, const std::string &strList );
    /** 초 단위 호출 — 임시 유예 초과·회수 예약·수명 만료 처리 */
    void Sweep( time_t iNow );
    int Size();

private:
    bool m_bAvailable = false;
    uint32_t m_iNextReqId = 0;
    std::map<uint32_t, CIpsecSaSetInfo> m_clsMap;
    std::set<uint32_t> m_clsSpiInUse;
    CSipMutex m_clsMutex;

    uint32_t _allocReqIdLocked();
    uint32_t _allocSpiLocked();
    void _eraseLocked( std::map<uint32_t, CIpsecSaSetInfo>::iterator it, const char *pszWhy );
};

extern CIpsecSaSetMap gclsIpsecSaSetMap;

#endif
