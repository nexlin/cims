#include "XfrmSa.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/xfrm.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <vector>

// ── 주소 ──────────────────────────────────────────────────────────────────────

struct XfrmAddr {
    xfrm_address_t addr;
    uint16_t iFamily = 0;
    uint8_t iPrefixLen = 0;
};

static bool _ParseAddr( const std::string &strIp, XfrmAddr &clsOut ) {
    memset( &clsOut.addr, 0, sizeof( clsOut.addr ) );
    if ( inet_pton( AF_INET, strIp.c_str(), &clsOut.addr.a4 ) == 1 ) {
        clsOut.iFamily = AF_INET;
        clsOut.iPrefixLen = 32;
        return true;
    }
    if ( inet_pton( AF_INET6, strIp.c_str(), clsOut.addr.a6 ) == 1 ) {
        clsOut.iFamily = AF_INET6;
        clsOut.iPrefixLen = 128;
        return true;
    }
    return false;
}

// ── SA 4개의 방향/주소/SPI 전개 ───────────────────────────────────────────────

struct XfrmSaLeg {
    bool bIn;
    XfrmAddr clsSrc, clsDst;
    int iSrcPort, iDstPort;
    uint32_t iSpi;
};

static bool _ExpandLegs( const CXfrmSaSet &s, XfrmSaLeg legs[4], std::string &strError ) {
    XfrmAddr l, r;
    if ( !_ParseAddr( s.strLocalIp, l ) || !_ParseAddr( s.strRemoteIp, r ) ) {
        strError = "invalid ip";
        return false;
    }
    if ( l.iFamily != r.iFamily ) {
        strError = "address family mismatch";
        return false;
    }
    if ( s.iLocalPortS <= 0 || s.iLocalPortC <= 0 || s.iRemotePortS <= 0 || s.iRemotePortC <= 0 ) {
        strError = "port missing";
        return false;
    }
    if ( s.iSpiLocalS == 0 || s.iSpiLocalC == 0 || s.iSpiRemoteS == 0 || s.iSpiRemoteC == 0 ) {
        strError = "spi missing";
        return false;
    }
    if ( s.iReqId == 0 ) {
        strError = "reqid missing";
        return false;
    }
    legs[0] = { true, r, l, s.iRemotePortC, s.iLocalPortS, s.iSpiLocalS };
    legs[1] = { false, l, r, s.iLocalPortS, s.iRemotePortC, s.iSpiRemoteC };
    legs[2] = { false, l, r, s.iLocalPortC, s.iRemotePortS, s.iSpiRemoteS };
    legs[3] = { true, r, l, s.iRemotePortS, s.iLocalPortC, s.iSpiLocalC };
    return true;
}

// ── netlink 메시지 조립 ────────────────────────────────────────────────────────

class CNlMsg {
public:
    std::string m_strBuf;

    void Header( uint16_t iType, uint16_t iFlags, uint32_t iSeq ) {
        nlmsghdr h;
        memset( &h, 0, sizeof( h ) );
        h.nlmsg_len = NLMSG_HDRLEN;
        h.nlmsg_type = iType;
        h.nlmsg_flags = iFlags;
        h.nlmsg_seq = iSeq;
        m_strBuf.assign( (const char *)&h, sizeof( h ) );
        m_strBuf.resize( NLMSG_HDRLEN, '\0' );
    }
    void Payload( const void *p, size_t n ) {
        m_strBuf.append( (const char *)p, n );
        m_strBuf.resize( NLMSG_ALIGN( m_strBuf.size() ), '\0' );
        _Fix();
    }
    void Attr( uint16_t iType, const void *p, size_t n ) {
        nlattr a;
        a.nla_len = (uint16_t)( NLA_HDRLEN + n );
        a.nla_type = iType;
        m_strBuf.append( (const char *)&a, sizeof( a ) );
        m_strBuf.resize( m_strBuf.size() + ( NLA_HDRLEN - sizeof( a ) ), '\0' );
        m_strBuf.append( (const char *)p, n );
        m_strBuf.resize( NLMSG_ALIGN( m_strBuf.size() ), '\0' );
        _Fix();
    }

private:
    void _Fix() {
        ( (nlmsghdr *)&m_strBuf[0] )->nlmsg_len = (uint32_t)m_strBuf.size();
    }
};

static void _Lifetime( xfrm_lifetime_cfg &lft, int iSec ) {
    lft.soft_byte_limit = XFRM_INF;
    lft.hard_byte_limit = XFRM_INF;
    lft.soft_packet_limit = XFRM_INF;
    lft.hard_packet_limit = XFRM_INF;
    lft.soft_add_expires_seconds = 0;
    lft.hard_add_expires_seconds = iSec > 0 ? (uint64_t)iSec : 0;
    lft.soft_use_expires_seconds = 0;
    lft.hard_use_expires_seconds = 0;
}

/** 정책 selector — (src ip/port → dst ip/port, proto) */
static void _Selector( xfrm_selector &sel, const XfrmSaLeg &leg, int iProto ) {
    memset( &sel, 0, sizeof( sel ) );
    sel.saddr = leg.clsSrc.addr;
    sel.daddr = leg.clsDst.addr;
    sel.prefixlen_s = leg.clsSrc.iPrefixLen;
    sel.prefixlen_d = leg.clsDst.iPrefixLen;
    sel.family = leg.clsSrc.iFamily;
    sel.sport = htons( (uint16_t)leg.iSrcPort );
    sel.dport = htons( (uint16_t)leg.iDstPort );
    sel.sport_mask = 0xffff;
    sel.dport_mask = 0xffff;
    sel.proto = (uint8_t)iProto;
}

bool CXfrmSa::IsAlgSupported( const std::string &strAuthAlg, const std::string &strEncAlg ) {
    const bool bAuth = strAuthAlg == XFRM_AUTH_HMAC_SHA1_96 || strAuthAlg == XFRM_AUTH_HMAC_MD5_96;
    const bool bEnc = strEncAlg == XFRM_ENC_AES_CBC || strEncAlg == XFRM_ENC_NULL;
    return bAuth && bEnc;
}

bool CXfrmSa::ExpandKeys( const std::string &strAuthAlg, const std::string &strEncAlg, const std::string &strIk,
                          const std::string &strCk, std::string &strIkEsp, std::string &strCkEsp,
                          std::string &strKernelAuth, std::string &strKernelEnc ) {
    if ( !IsAlgSupported( strAuthAlg, strEncAlg ) || strIk.size() != 16 ) return false;
    if ( strAuthAlg == XFRM_AUTH_HMAC_SHA1_96 ) {
        strIkEsp = strIk + strIk.substr( 0, 4 );  // IK ‖ IK[0..31] → 160 bit (TS 33.203 §6.3)
        strKernelAuth = "hmac(sha1)";
    } else {
        strIkEsp = strIk;
        strKernelAuth = "hmac(md5)";
    }
    if ( strEncAlg == XFRM_ENC_AES_CBC ) {
        if ( strCk.size() != 16 ) return false;
        strCkEsp = strCk;
        strKernelEnc = "cbc(aes)";
    } else {
        strCkEsp.clear();
        strKernelEnc = "ecb(cipher_null)";
    }
    return true;
}

bool CXfrmSa::BuildSaMessage( uint16_t iType, const CXfrmSaSet &clsSet, int iIndex, uint32_t iSeq, std::string &strOut,
                              std::string &strError ) {
    XfrmSaLeg legs[4];
    if ( iIndex < 0 || iIndex > 3 || !_ExpandLegs( clsSet, legs, strError ) ) return false;
    const XfrmSaLeg &leg = legs[iIndex];
    CNlMsg m;

    if ( iType == XFRM_MSG_DELSA ) {
        m.Header( iType, NLM_F_REQUEST | NLM_F_ACK, iSeq );
        xfrm_usersa_id id;
        memset( &id, 0, sizeof( id ) );
        id.daddr = leg.clsDst.addr;
        id.spi = htonl( leg.iSpi );
        id.family = leg.clsDst.iFamily;
        id.proto = IPPROTO_ESP;
        m.Payload( &id, sizeof( id ) );
        strOut = m.m_strBuf;
        return true;
    }

    std::string strIkEsp, strCkEsp, strKAuth, strKEnc;
    if ( !ExpandKeys( clsSet.strAuthAlg, clsSet.strEncAlg, clsSet.strIk, clsSet.strCk, strIkEsp, strCkEsp, strKAuth,
                      strKEnc ) ) {
        strError = "unsupported alg or key length";
        return false;
    }

    m.Header( iType, NLM_F_REQUEST | NLM_F_ACK | ( iType == XFRM_MSG_NEWSA ? NLM_F_EXCL | NLM_F_CREATE : 0 ), iSeq );
    xfrm_usersa_info info;
    memset( &info, 0, sizeof( info ) );
    info.id.daddr = leg.clsDst.addr;
    info.id.spi = htonl( leg.iSpi );
    info.id.proto = IPPROTO_ESP;
    info.saddr = leg.clsSrc.addr;
    info.family = leg.clsDst.iFamily;
    info.mode = XFRM_MODE_TRANSPORT;
    info.reqid = clsSet.iReqId;
    info.replay_window = 32;  // RFC 4303 anti-replay — TS 33.203 §6.3 이 요구
    _Lifetime( info.lft, clsSet.iLifetimeSec );
    m.Payload( &info, sizeof( info ) );

    // XFRMA_ALG_AUTH_TRUNC — xfrm_algo_auth + key
    {
        std::string a( sizeof( xfrm_algo_auth ) + strIkEsp.size(), '\0' );
        xfrm_algo_auth *p = (xfrm_algo_auth *)&a[0];
        snprintf( p->alg_name, sizeof( p->alg_name ), "%s", strKAuth.c_str() );
        p->alg_key_len = (unsigned int)strIkEsp.size() * 8;
        p->alg_trunc_len = 96;
        memcpy( p->alg_key, strIkEsp.data(), strIkEsp.size() );
        m.Attr( XFRMA_ALG_AUTH_TRUNC, a.data(), a.size() );
    }
    // XFRMA_ALG_CRYPT — xfrm_algo + key (null 은 키 길이 0)
    {
        std::string a( sizeof( xfrm_algo ) + strCkEsp.size(), '\0' );
        xfrm_algo *p = (xfrm_algo *)&a[0];
        snprintf( p->alg_name, sizeof( p->alg_name ), "%s", strKEnc.c_str() );
        p->alg_key_len = (unsigned int)strCkEsp.size() * 8;
        if ( !strCkEsp.empty() ) memcpy( p->alg_key, strCkEsp.data(), strCkEsp.size() );
        m.Attr( XFRMA_ALG_CRYPT, a.data(), a.size() );
    }
    strOut = m.m_strBuf;
    return true;
}

bool CXfrmSa::BuildPolicyMessage( uint16_t iType, const CXfrmSaSet &clsSet, int iIndex, int iProto, uint32_t iSeq,
                                  std::string &strOut, std::string &strError ) {
    XfrmSaLeg legs[4];
    if ( iIndex < 0 || iIndex > 3 || !_ExpandLegs( clsSet, legs, strError ) ) return false;
    const XfrmSaLeg &leg = legs[iIndex];
    CNlMsg m;

    if ( iType == XFRM_MSG_DELPOLICY ) {
        m.Header( iType, NLM_F_REQUEST | NLM_F_ACK, iSeq );
        xfrm_userpolicy_id id;
        memset( &id, 0, sizeof( id ) );
        _Selector( id.sel, leg, iProto );
        id.dir = leg.bIn ? XFRM_POLICY_IN : XFRM_POLICY_OUT;
        m.Payload( &id, sizeof( id ) );
        strOut = m.m_strBuf;
        return true;
    }

    m.Header( iType, NLM_F_REQUEST | NLM_F_ACK | ( iType == XFRM_MSG_NEWPOLICY ? NLM_F_EXCL | NLM_F_CREATE : 0 ),
              iSeq );
    xfrm_userpolicy_info info;
    memset( &info, 0, sizeof( info ) );
    _Selector( info.sel, leg, iProto );
    _Lifetime( info.lft, clsSet.iLifetimeSec );
    info.priority = 1000;
    info.dir = leg.bIn ? XFRM_POLICY_IN : XFRM_POLICY_OUT;
    info.action = XFRM_POLICY_ALLOW;
    info.share = XFRM_SHARE_ANY;
    m.Payload( &info, sizeof( info ) );

    xfrm_user_tmpl tmpl;
    memset( &tmpl, 0, sizeof( tmpl ) );
    tmpl.id.proto = IPPROTO_ESP;  // transport mode — 주소는 패킷의 것 (id.daddr/saddr 비움)
    tmpl.family = leg.clsDst.iFamily;
    tmpl.reqid = clsSet.iReqId;
    tmpl.mode = XFRM_MODE_TRANSPORT;
    tmpl.share = XFRM_SHARE_ANY;
    tmpl.optional = 0;
    tmpl.aalgos = ~0u;
    tmpl.ealgos = ~0u;
    tmpl.calgos = ~0u;
    m.Attr( XFRMA_TMPL, &tmpl, sizeof( tmpl ) );
    strOut = m.m_strBuf;
    return true;
}

// ── netlink 소켓 ──────────────────────────────────────────────────────────────

class CNlSock {
public:
    CNlSock() : m_hSock( -1 ), m_iSeq( 1 ) {
    }
    ~CNlSock() {
        if ( m_hSock >= 0 ) close( m_hSock );
    }
    bool Open( std::string &strError ) {
        m_hSock = socket( AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_XFRM );
        if ( m_hSock < 0 ) {
            strError = std::string( "socket(NETLINK_XFRM): " ) + strerror( errno );
            return false;
        }
        sockaddr_nl sa;
        memset( &sa, 0, sizeof( sa ) );
        sa.nl_family = AF_NETLINK;
        if ( bind( m_hSock, (sockaddr *)&sa, sizeof( sa ) ) < 0 ) {
            strError = std::string( "bind(NETLINK_XFRM): " ) + strerror( errno );
            return false;
        }
        return true;
    }
    uint32_t NextSeq() {
        return m_iSeq++;
    }

    /** 요청 하나를 보내고 ACK(NLMSG_ERROR error=0) 를 기다린다. 커널 오류는 -errno 를 돌려준다(0=성공). */
    int Transact( const std::string &strMsg, std::string &strError ) {
        if ( !_Send( strMsg, strError ) ) return -EIO;
        const uint32_t iSeq = ( (const nlmsghdr *)strMsg.data() )->nlmsg_seq;
        std::string strBuf;
        for ( ;; ) {
            if ( !_Recv( strBuf, strError ) ) return -EIO;
            const nlmsghdr *h = (const nlmsghdr *)strBuf.data();
            size_t iLen = strBuf.size();
            for ( ; NLMSG_OK( h, iLen ); h = NLMSG_NEXT( h, iLen ) ) {
                if ( h->nlmsg_seq != iSeq ) continue;
                if ( h->nlmsg_type == NLMSG_ERROR ) {
                    const nlmsgerr *e = (const nlmsgerr *)NLMSG_DATA( h );
                    if ( e->error != 0 ) strError = strerror( -e->error );
                    return e->error;
                }
            }
        }
    }

    /** 덤프 요청 — 응답 메시지마다 cb 호출. */
    template <typename F> bool Dump( const std::string &strMsg, F cb, std::string &strError ) {
        if ( !_Send( strMsg, strError ) ) return false;
        const uint32_t iSeq = ( (const nlmsghdr *)strMsg.data() )->nlmsg_seq;
        std::string strBuf;
        for ( ;; ) {
            if ( !_Recv( strBuf, strError ) ) return false;
            const nlmsghdr *h = (const nlmsghdr *)strBuf.data();
            size_t iLen = strBuf.size();
            for ( ; NLMSG_OK( h, iLen ); h = NLMSG_NEXT( h, iLen ) ) {
                if ( h->nlmsg_seq != iSeq ) continue;
                if ( h->nlmsg_type == NLMSG_DONE ) return true;
                if ( h->nlmsg_type == NLMSG_ERROR ) {
                    const nlmsgerr *e = (const nlmsgerr *)NLMSG_DATA( h );
                    strError = strerror( -e->error );
                    return e->error == 0;
                }
                cb( h );
            }
        }
    }

private:
    int m_hSock;
    uint32_t m_iSeq;

    bool _Send( const std::string &strMsg, std::string &strError ) {
        sockaddr_nl sa;
        memset( &sa, 0, sizeof( sa ) );
        sa.nl_family = AF_NETLINK;
        if ( sendto( m_hSock, strMsg.data(), strMsg.size(), 0, (sockaddr *)&sa, sizeof( sa ) ) < 0 ) {
            strError = std::string( "sendto: " ) + strerror( errno );
            return false;
        }
        return true;
    }
    bool _Recv( std::string &strBuf, std::string &strError ) {
        strBuf.resize( 65536 );
        ssize_t n = recv( m_hSock, &strBuf[0], strBuf.size(), 0 );
        if ( n < 0 ) {
            strError = std::string( "recv: " ) + strerror( errno );
            return false;
        }
        strBuf.resize( (size_t)n );
        return true;
    }
};

// ── 셋 단위 연산 ──────────────────────────────────────────────────────────────

static const int g_arrProto[2] = { IPPROTO_UDP, IPPROTO_TCP };

/** DEL 에서 "이미 없음" — state 는 ESRCH, policy 는 ENOENT */
static bool _IsMissing( int r ) {
    return r == -ESRCH || r == -ENOENT;
}

/** 셋의 state 4 + policy 8 을 순서대로 적용. iSaType/iPolType 에 NEW/UPD/DEL. bIgnoreMissing 은 DEL 용. */
static bool _Apply( CNlSock &nl, const CXfrmSaSet &clsSet, uint16_t iSaType, uint16_t iPolType, bool bIgnoreMissing,
                    std::string &strError ) {
    bool bOk = true;
    for ( int i = 0; i < 4; ++i ) {
        std::string msg, err;
        if ( !CXfrmSa::BuildSaMessage( iSaType, clsSet, i, nl.NextSeq(), msg, err ) ) {
            strError = err;
            return false;
        }
        int r = nl.Transact( msg, err );
        if ( r != 0 && !( bIgnoreMissing && _IsMissing( r ) ) ) {
            strError = "sa[" + std::to_string( i ) + "]: " + err;
            bOk = false;
            if ( !bIgnoreMissing ) return false;
        }
        for ( int p = 0; p < 2; ++p ) {
            if ( !CXfrmSa::BuildPolicyMessage( iPolType, clsSet, i, g_arrProto[p], nl.NextSeq(), msg, err ) ) {
                strError = err;
                return false;
            }
            r = nl.Transact( msg, err );
            if ( r != 0 && !( bIgnoreMissing && _IsMissing( r ) ) ) {
                strError = "policy[" + std::to_string( i ) + "/" + ( p == 0 ? "udp" : "tcp" ) + "]: " + err;
                bOk = false;
                if ( !bIgnoreMissing ) return false;
            }
        }
    }
    return bOk;
}

bool CXfrmSa::Add( const CXfrmSaSet &clsSet, std::string &strError ) {
    CNlSock nl;
    if ( !nl.Open( strError ) ) return false;
    if ( _Apply( nl, clsSet, XFRM_MSG_NEWSA, XFRM_MSG_NEWPOLICY, false, strError ) ) return true;
    // 부분 설치 되돌리기 — 원 오류를 보존한다
    std::string strIgnore;
    _Apply( nl, clsSet, XFRM_MSG_DELSA, XFRM_MSG_DELPOLICY, true, strIgnore );
    return false;
}

bool CXfrmSa::Update( const CXfrmSaSet &clsSet, std::string &strError ) {
    CNlSock nl;
    if ( !nl.Open( strError ) ) return false;
    return _Apply( nl, clsSet, XFRM_MSG_UPDSA, XFRM_MSG_UPDPOLICY, false, strError );
}

bool CXfrmSa::Delete( const CXfrmSaSet &clsSet, std::string &strError ) {
    CNlSock nl;
    if ( !nl.Open( strError ) ) return false;
    return _Apply( nl, clsSet, XFRM_MSG_DELSA, XFRM_MSG_DELPOLICY, true, strError );
}

int CXfrmSa::FlushByReqId( uint32_t iMin, uint32_t iMax, std::string &strError ) {
    CNlSock nl;
    if ( !nl.Open( strError ) ) return -1;

    // 1) state 덤프 → reqid 범위 내 삭제
    struct SaKey {
        xfrm_usersa_id id;
    };
    std::vector<SaKey> vecSa;
    {
        CNlMsg m;
        m.Header( XFRM_MSG_GETSA, NLM_F_REQUEST | NLM_F_DUMP, nl.NextSeq() );
        xfrm_usersa_id id;
        memset( &id, 0, sizeof( id ) );
        m.Payload( &id, sizeof( id ) );
        if ( !nl.Dump(
                 m.m_strBuf,
                 [&]( const nlmsghdr *h ) {
                     if ( h->nlmsg_type != XFRM_MSG_NEWSA ) return;
                     const xfrm_usersa_info *p = (const xfrm_usersa_info *)NLMSG_DATA( h );
                     if ( p->reqid < iMin || p->reqid > iMax ) return;
                     SaKey k;
                     memset( &k, 0, sizeof( k ) );
                     k.id.daddr = p->id.daddr;
                     k.id.spi = p->id.spi;
                     k.id.family = p->family;
                     k.id.proto = p->id.proto;
                     vecSa.push_back( k );
                 },
                 strError ) )
            return -1;
    }
    // 2) policy 덤프 → 템플릿 reqid 범위 내 삭제 (index 로)
    struct PolKey {
        uint32_t iIndex;
        uint8_t iDir;
    };
    std::vector<PolKey> vecPol;
    {
        CNlMsg m;
        m.Header( XFRM_MSG_GETPOLICY, NLM_F_REQUEST | NLM_F_DUMP, nl.NextSeq() );
        xfrm_userpolicy_id id;
        memset( &id, 0, sizeof( id ) );
        m.Payload( &id, sizeof( id ) );
        if ( !nl.Dump(
                 m.m_strBuf,
                 [&]( const nlmsghdr *h ) {
                     if ( h->nlmsg_type != XFRM_MSG_NEWPOLICY ) return;
                     const xfrm_userpolicy_info *p = (const xfrm_userpolicy_info *)NLMSG_DATA( h );
                     // 속성 순회 — XFRMA_TMPL 의 첫 템플릿 reqid
                     const char *a = (const char *)p + NLMSG_ALIGN( sizeof( *p ) );
                     const char *end = (const char *)h + h->nlmsg_len;
                     while ( a + NLA_HDRLEN <= end ) {
                         const nlattr *na = (const nlattr *)a;
                         if ( na->nla_len < NLA_HDRLEN || a + na->nla_len > end ) break;
                         if ( na->nla_type == XFRMA_TMPL && na->nla_len >= NLA_HDRLEN + sizeof( xfrm_user_tmpl ) ) {
                             const xfrm_user_tmpl *t = (const xfrm_user_tmpl *)( a + NLA_HDRLEN );
                             if ( t->reqid >= iMin && t->reqid <= iMax ) vecPol.push_back( { p->index, p->dir } );
                             break;
                         }
                         a += NLA_ALIGN( na->nla_len );
                     }
                 },
                 strError ) )
            return -1;
    }

    int iDeleted = 0;
    for ( size_t i = 0; i < vecPol.size(); ++i ) {
        CNlMsg m;
        m.Header( XFRM_MSG_DELPOLICY, NLM_F_REQUEST | NLM_F_ACK, nl.NextSeq() );
        xfrm_userpolicy_id id;
        memset( &id, 0, sizeof( id ) );
        id.index = vecPol[i].iIndex;
        id.dir = vecPol[i].iDir;
        m.Payload( &id, sizeof( id ) );
        std::string err;
        nl.Transact( m.m_strBuf, err );
    }
    for ( size_t i = 0; i < vecSa.size(); ++i ) {
        CNlMsg m;
        m.Header( XFRM_MSG_DELSA, NLM_F_REQUEST | NLM_F_ACK, nl.NextSeq() );
        m.Payload( &vecSa[i].id, sizeof( vecSa[i].id ) );
        std::string err;
        if ( nl.Transact( m.m_strBuf, err ) == 0 ) ++iDeleted;
    }
    return iDeleted;
}

bool CXfrmSa::SelfCheck( uint32_t iReqId, std::string &strError ) {
    CXfrmSaSet s;
    s.strLocalIp = "127.0.0.1";
    s.strRemoteIp = "127.0.0.1";
    s.iLocalPortS = 65001;
    s.iLocalPortC = 65002;
    s.iRemotePortS = 65003;
    s.iRemotePortC = 65004;
    s.iSpiLocalS = 0x7fff0001;
    s.iSpiLocalC = 0x7fff0002;
    s.iSpiRemoteS = 0x7fff0003;
    s.iSpiRemoteC = 0x7fff0004;
    s.strAuthAlg = XFRM_AUTH_HMAC_SHA1_96;
    s.strEncAlg = XFRM_ENC_AES_CBC;
    s.strIk.assign( 16, '\x11' );
    s.strCk.assign( 16, '\x22' );
    s.iReqId = iReqId;
    s.iLifetimeSec = 5;
    if ( !Add( s, strError ) ) return false;
    std::string strIgnore;
    Delete( s, strIgnore );
    return true;
}
