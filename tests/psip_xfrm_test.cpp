// psip XFRM SA 모듈 하네스 — TS 33.203 §6.3 키 확장 + netlink 메시지 인코딩 + (특권 있을 때) 실설치 왕복
//   (sip_access_security.md §8.3 P4-1).
//   빌드: g++ -std=c++17 -I ext/psip/SipStack tests/psip_xfrm_test.cpp ext/psip/SipStack/XfrmSa.cpp -o build/psip_xfrm_test
//   실행: build/psip_xfrm_test          — 인코딩 검사. 실설치 구간은 CAP_NET_ADMIN 없으면 SKIP
//         sudo build/psip_xfrm_test     — 실설치 왕복까지 (Add → FlushByReqId=4 → Delete 멱등)
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/xfrm.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "XfrmSa.h"

static int g_fail = 0;
#define CHECK( name, cond )                                  \
    do {                                                     \
        if ( !( cond ) ) {                                   \
            printf( "FAIL %s (%s:%d)\n", name, __FILE__, __LINE__ ); \
            ++g_fail;                                        \
        } else {                                             \
            printf( "ok   %s\n", name );                     \
        }                                                    \
    } while ( 0 )

static const nlattr *FindAttr( const nlmsghdr *h, size_t iPayload, uint16_t iType ) {
    const char *a = (const char *)NLMSG_DATA( h ) + NLMSG_ALIGN( iPayload );
    const char *end = (const char *)h + h->nlmsg_len;
    while ( a + NLA_HDRLEN <= end ) {
        const nlattr *na = (const nlattr *)a;
        if ( na->nla_type == iType ) return na;
        a += NLA_ALIGN( na->nla_len );
    }
    return NULL;
}

static uint32_t Ip4( const char *s ) {
    in_addr a;
    inet_pton( AF_INET, s, &a );
    return a.s_addr;
}

int main() {
    // ── 키 확장 ──
    std::string ik( 16, '\0' ), ck( 16, '\0' );
    for ( int i = 0; i < 16; ++i ) {
        ik[i] = (char)( 0xa0 + i );
        ck[i] = (char)( 0x10 + i );
    }
    std::string ikEsp, ckEsp, ka, ke;
    CHECK( "expand sha1/aes", CXfrmSa::ExpandKeys( XFRM_AUTH_HMAC_SHA1_96, XFRM_ENC_AES_CBC, ik, ck, ikEsp, ckEsp, ka, ke ) );
    CHECK( "sha1 key = IK||IK[0..3] (160bit)", ikEsp.size() == 20 && ikEsp.substr( 0, 16 ) == ik && ikEsp.substr( 16 ) == ik.substr( 0, 4 ) );
    CHECK( "aes key = CK", ckEsp == ck && ka == "hmac(sha1)" && ke == "cbc(aes)" );
    CHECK( "expand md5/null", CXfrmSa::ExpandKeys( XFRM_AUTH_HMAC_MD5_96, XFRM_ENC_NULL, ik, "", ikEsp, ckEsp, ka, ke ) );
    CHECK( "md5 key = IK, null enc", ikEsp == ik && ckEsp.empty() && ka == "hmac(md5)" && ke == "ecb(cipher_null)" );
    CHECK( "3des rejected", !CXfrmSa::IsAlgSupported( XFRM_AUTH_HMAC_SHA1_96, "des-ede3-cbc" ) );

    // ── 셋 (CSP 관점: local_s=port_ps, local_c=port_pc) ──
    CXfrmSaSet s;
    s.strLocalIp = "10.0.0.1";
    s.strRemoteIp = "10.0.0.2";
    s.iLocalPortS = 5062;   // port_ps
    s.iLocalPortC = 5063;   // port_pc
    s.iRemotePortS = 6062;  // port_us
    s.iRemotePortC = 6063;  // port_uc
    s.iSpiLocalS = 0x10000001;
    s.iSpiLocalC = 0x10000002;
    s.iSpiRemoteS = 0x20000003;
    s.iSpiRemoteC = 0x20000004;
    s.strAuthAlg = XFRM_AUTH_HMAC_SHA1_96;
    s.strEncAlg = XFRM_ENC_AES_CBC;
    s.strIk = ik;
    s.strCk = ck;
    s.iReqId = 0x43490007;
    s.iLifetimeSec = 3630;

    std::string msg, err;
    // SA[0] in: UE:port_uc → CSP:port_ps, spi_ps
    CHECK( "build NEWSA[0]", CXfrmSa::BuildSaMessage( XFRM_MSG_NEWSA, s, 0, 7, msg, err ) );
    {
        const nlmsghdr *h = (const nlmsghdr *)msg.data();
        const xfrm_usersa_info *p = (const xfrm_usersa_info *)NLMSG_DATA( h );
        CHECK( "NEWSA hdr", h->nlmsg_type == XFRM_MSG_NEWSA && h->nlmsg_seq == 7 && h->nlmsg_len == msg.size() &&
                                ( h->nlmsg_flags & ( NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE ) ) ==
                                    ( NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE ) );
        CHECK( "SA[0] src=UE dst=CSP spi=spi_ps", p->saddr.a4 == Ip4( "10.0.0.2" ) && p->id.daddr.a4 == Ip4( "10.0.0.1" ) &&
                                                       p->id.spi == htonl( 0x10000001 ) && p->id.proto == IPPROTO_ESP );
        CHECK( "SA transport/reqid/replay/lifetime", p->mode == XFRM_MODE_TRANSPORT && p->reqid == 0x43490007 &&
                                                          p->replay_window == 32 && p->lft.hard_add_expires_seconds == 3630 &&
                                                          p->lft.hard_byte_limit == XFRM_INF && p->family == AF_INET &&
                                                          p->sel.family == 0 );
        const nlattr *a = FindAttr( h, sizeof( *p ), XFRMA_ALG_AUTH_TRUNC );
        const xfrm_algo_auth *aa = a ? (const xfrm_algo_auth *)( (const char *)a + NLA_HDRLEN ) : NULL;
        CHECK( "auth attr hmac(sha1) 160/96", aa && strcmp( aa->alg_name, "hmac(sha1)" ) == 0 && aa->alg_key_len == 160 &&
                                                   aa->alg_trunc_len == 96 && memcmp( aa->alg_key, ikEsp.data(), 0 ) == 0 &&
                                                   memcmp( aa->alg_key, ik.data(), 16 ) == 0 );
        const nlattr *c = FindAttr( h, sizeof( *p ), XFRMA_ALG_CRYPT );
        const xfrm_algo *ca = c ? (const xfrm_algo *)( (const char *)c + NLA_HDRLEN ) : NULL;
        CHECK( "crypt attr cbc(aes) 128", ca && strcmp( ca->alg_name, "cbc(aes)" ) == 0 && ca->alg_key_len == 128 &&
                                               memcmp( ca->alg_key, ck.data(), 16 ) == 0 );
    }
    // SA[2] out: CSP:port_pc → UE:port_us, spi_us
    CHECK( "build NEWSA[2]", CXfrmSa::BuildSaMessage( XFRM_MSG_NEWSA, s, 2, 8, msg, err ) );
    {
        const xfrm_usersa_info *p = (const xfrm_usersa_info *)NLMSG_DATA( (const nlmsghdr *)msg.data() );
        CHECK( "SA[2] src=CSP dst=UE spi=spi_us", p->saddr.a4 == Ip4( "10.0.0.1" ) && p->id.daddr.a4 == Ip4( "10.0.0.2" ) &&
                                                       p->id.spi == htonl( 0x20000003 ) );
    }
    // DELSA[3]
    CHECK( "build DELSA[3]", CXfrmSa::BuildSaMessage( XFRM_MSG_DELSA, s, 3, 9, msg, err ) );
    {
        const xfrm_usersa_id *p = (const xfrm_usersa_id *)NLMSG_DATA( (const nlmsghdr *)msg.data() );
        CHECK( "DELSA[3] dst=CSP spi=spi_pc", p->daddr.a4 == Ip4( "10.0.0.1" ) && p->spi == htonl( 0x10000002 ) &&
                                                   p->proto == IPPROTO_ESP && p->family == AF_INET );
    }
    // 정책 [0] udp in
    CHECK( "build NEWPOLICY[0] udp", CXfrmSa::BuildPolicyMessage( XFRM_MSG_NEWPOLICY, s, 0, IPPROTO_UDP, 10, msg, err ) );
    {
        const nlmsghdr *h = (const nlmsghdr *)msg.data();
        const xfrm_userpolicy_info *p = (const xfrm_userpolicy_info *)NLMSG_DATA( h );
        CHECK( "policy[0] in, sel UE:port_uc→CSP:port_ps udp /32", p->dir == XFRM_POLICY_IN && p->action == XFRM_POLICY_ALLOW &&
                                                                       p->sel.saddr.a4 == Ip4( "10.0.0.2" ) &&
                                                                       p->sel.daddr.a4 == Ip4( "10.0.0.1" ) &&
                                                                       p->sel.sport == htons( 6063 ) && p->sel.dport == htons( 5062 ) &&
                                                                       p->sel.sport_mask == 0xffff && p->sel.proto == IPPROTO_UDP &&
                                                                       p->sel.prefixlen_s == 32 && p->sel.prefixlen_d == 32 &&
                                                                       p->lft.hard_add_expires_seconds == 3630 );
        const nlattr *t = FindAttr( h, sizeof( *p ), XFRMA_TMPL );
        const xfrm_user_tmpl *tp = t ? (const xfrm_user_tmpl *)( (const char *)t + NLA_HDRLEN ) : NULL;
        CHECK( "tmpl esp transport reqid", tp && tp->id.proto == IPPROTO_ESP && tp->mode == XFRM_MODE_TRANSPORT &&
                                               tp->reqid == 0x43490007 && tp->id.spi == 0 && tp->optional == 0 );
    }
    // 정책 [1] tcp out (DEL)
    CHECK( "build DELPOLICY[1] tcp", CXfrmSa::BuildPolicyMessage( XFRM_MSG_DELPOLICY, s, 1, IPPROTO_TCP, 11, msg, err ) );
    {
        const xfrm_userpolicy_id *p = (const xfrm_userpolicy_id *)NLMSG_DATA( (const nlmsghdr *)msg.data() );
        CHECK( "delpolicy[1] out CSP:port_ps→UE:port_uc tcp", p->dir == XFRM_POLICY_OUT && p->sel.sport == htons( 5062 ) &&
                                                                  p->sel.dport == htons( 6063 ) && p->sel.proto == IPPROTO_TCP );
    }
    // 입력 검증
    CXfrmSaSet bad = s;
    bad.iSpiLocalC = 0;
    CHECK( "spi missing rejected", !CXfrmSa::BuildSaMessage( XFRM_MSG_NEWSA, bad, 0, 1, msg, err ) );
    bad = s;
    bad.strRemoteIp = "::1";
    CHECK( "family mismatch rejected", !CXfrmSa::BuildSaMessage( XFRM_MSG_NEWSA, bad, 0, 1, msg, err ) );

    // ── 실설치 (CAP_NET_ADMIN) ──
    std::string e;
    if ( !CXfrmSa::SelfCheck( 0x43490001, e ) ) {
        printf( "SKIP live xfrm (%s)\n", e.c_str() );
    } else {
        CXfrmSaSet live = s;
        live.strLocalIp = "127.0.0.1";
        live.strRemoteIp = "127.0.0.1";
        live.iLifetimeSec = 30;
        CHECK( "live Add", CXfrmSa::Add( live, e ) );
        CHECK( "live Add again = EEXIST", !CXfrmSa::Add( live, e ) );
        CHECK( "live Update", CXfrmSa::Update( live, e ) );
        int n = CXfrmSa::FlushByReqId( 0x43490000, 0x4349ffff, e );
        CHECK( "live FlushByReqId == 4", n == 4 );
        CHECK( "live Delete idempotent", CXfrmSa::Delete( live, e ) );
        CHECK( "live Add after flush", CXfrmSa::Add( live, e ) );
        CHECK( "live Delete", CXfrmSa::Delete( live, e ) );
    }

    printf( "%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail );
    return g_fail ? 1 : 0;
}
