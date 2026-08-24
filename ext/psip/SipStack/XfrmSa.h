/*
 * XfrmSa — 커널 IPsec(XFRM) ESP transport-mode SA/정책 프로그래밍 (sip_access_security.md §8.3, P4)
 *
 * TS 33.203 §7.1 의 SA 4개(2쌍)를 한 셋으로 다룬다. 서버(CSP)와 단말(psip UA)이 같은 코드를 쓴다 —
 * 양쪽 모두 "내 서버포트/내 클라이언트포트 ↔ 상대 클라이언트포트/상대 서버포트" 로 대칭이다.
 *
 *   [0] in : remote:remote_c → local:local_s   (spi_local_s)   상대의 요청
 *   [1] out: local:local_s   → remote:remote_c (spi_remote_c)  그 응답
 *   [2] out: local:local_c   → remote:remote_s (spi_remote_s)  내 요청
 *   [3] in : remote:remote_s → local:local_c   (spi_local_c)   그 응답
 *
 * 순수 netlink(NETLINK_XFRM, libnl 미사용). SA 하나당 정책 둘(udp/tcp — selector 포트는 proto 가 있어야
 * 유효) → 셋당 state 4 + policy 8. 소유 표식은 reqid — 셋마다 고유 reqid 하나를 state/policy 가 공유하며
 * 기동·종료 시 reqid 범위로 걸러 일괄 회수한다(XFRM mark 는 패킷 매칭 조건이라 표식으로 쓸 수 없다).
 * 키 확장은 TS 33.203 §6.3. CAP_NET_ADMIN 이 필요하다 — 없으면 EPERM.
 */
#ifndef _XFRM_SA_H_
#define _XFRM_SA_H_

#include <stdint.h>

#include <string>

/** RFC 3329 ipsec-3gpp 의 alg / ealg 값 */
#define XFRM_AUTH_HMAC_SHA1_96 "hmac-sha-1-96"
#define XFRM_AUTH_HMAC_MD5_96 "hmac-md5-96"
#define XFRM_ENC_AES_CBC "aes-cbc"
#define XFRM_ENC_NULL "null"

/** SA 4개 한 셋의 파라미터 — 양측이 같은 구조를 자기 관점(local/remote)으로 채운다. */
struct CXfrmSaSet {
    std::string strLocalIp;   // IPv4/IPv6 문자열
    std::string strRemoteIp;
    int iLocalPortS = 0;      // 내 보호 서버 포트 (CSP: port_ps, UE: port_us)
    int iLocalPortC = 0;      // 내 보호 클라이언트 포트 (CSP: port_pc, UE: port_uc)
    int iRemotePortS = 0;
    int iRemotePortC = 0;
    uint32_t iSpiLocalS = 0;  // 내가 고른 SPI — local_s 로 들어오는 SA
    uint32_t iSpiLocalC = 0;  // 내가 고른 SPI — local_c 로 들어오는 SA
    uint32_t iSpiRemoteS = 0; // 상대가 고른 SPI — remote_s 로 나가는 SA
    uint32_t iSpiRemoteC = 0; // 상대가 고른 SPI — remote_c 로 나가는 SA
    std::string strAuthAlg;   // XFRM_AUTH_*
    std::string strEncAlg;    // XFRM_ENC_*
    std::string strIk;        // 16B 이진 (AKA IK)
    std::string strCk;        // 16B 이진 (AKA CK)
    uint32_t iReqId = 0;      // 셋 식별자 = 소유 표식
    int iLifetimeSec = 0;     // hard add-expire (초). 0 = 무한
};

class CXfrmSa {
public:
    /** 지원 (alg, ealg) 조합인가 */
    static bool IsAlgSupported( const std::string &strAuthAlg, const std::string &strEncAlg );

    /** TS 33.203 §6.3 키 확장 + 커널 알고리즘 이름.
     *  IK_esp = IK (md5) | IK‖IK[0..3] (sha1, 160bit).  CK_esp = CK (aes-cbc) | 빈 값 (null). */
    static bool ExpandKeys( const std::string &strAuthAlg, const std::string &strEncAlg, const std::string &strIk,
                            const std::string &strCk, std::string &strIkEsp, std::string &strCkEsp,
                            std::string &strKernelAuth, std::string &strKernelEnc );

    /** 셋 설치 (SA 4 + 정책 8). 도중 실패하면 설치한 것을 되돌리고 false. */
    static bool Add( const CXfrmSaSet &clsSet, std::string &strError );

    /** 수명 갱신 — iLifetimeSec 을 SA/정책 모두에 다시 적용 (XFRM_MSG_UPDSA / UPDPOLICY). */
    static bool Update( const CXfrmSaSet &clsSet, std::string &strError );

    /** 셋 회수. 일부가 이미 없어도(ESRCH) 계속 진행하고 그 외 오류만 보고한다. */
    static bool Delete( const CXfrmSaSet &clsSet, std::string &strError );

    /** reqid 가 [iMin, iMax] 인 state/policy 전부 회수. 회수한 state 수를 반환, 오류면 -1. */
    static int FlushByReqId( uint32_t iMin, uint32_t iMax, std::string &strError );

    /** 기동 자기점검 — 더미 셋(loopback 주소·높은 포트) 설치·회수. 특권/모듈 부재를 여기서 안다. */
    static bool SelfCheck( uint32_t iReqId, std::string &strError );

    /** 시험 훅 — iIndex(0..3) 번째 SA 의 netlink 메시지(헤더 포함) 를 만든다. iType = XFRM_MSG_NEWSA 등 */
    static bool BuildSaMessage( uint16_t iType, const CXfrmSaSet &clsSet, int iIndex, uint32_t iSeq, std::string &strOut,
                                std::string &strError );
    /** 시험 훅 — iIndex 번째 SA 의 iProto(IPPROTO_UDP|TCP) 정책 메시지 */
    static bool BuildPolicyMessage( uint16_t iType, const CXfrmSaSet &clsSet, int iIndex, int iProto, uint32_t iSeq,
                                    std::string &strOut, std::string &strError );
};

#endif
