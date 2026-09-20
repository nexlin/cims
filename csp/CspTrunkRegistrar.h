#ifndef __CSP_TRUNK_REGISTRAR_H__
#define __CSP_TRUNK_REGISTRAR_H__

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "CspRouteMap.h"

class CSipMessage;

/**
 * @brief 등록형 트렁크 바인딩 — 트렁크 계정(Route `inbound_auth=digest` + `auth_user`, sip_service_model.md §2-3)이
 * REGISTER 로 남긴 도달 주소. SIPconnect 2.0 §8 등록 모드: 계정 하나가 DID 범위를 대표한다(개별 DID 등록·RFC 6140 GIN
 * 없음).
 *
 *   도달 주소 = REGISTER 의 **소스**(RFC 3261 §18.2.1 received/rport latch — NAT 뒤 IP-PBX 도 닿는다), Contact 원문은
 * 표시·200 에코용.
 */
struct TrunkBinding {
    std::string route_name;
    std::string remote_node_ref;
    std::string account;      // auth_user
    std::string contact_uri;  // 요청 Contact 원문
    std::string ip;           // 소스 주소(latch)
    int port = 0;
    std::string transport;  // UDP | TCP | TLS
    int listener_id = 0;    // 받은 접속점 — 서버 발신의 자기 주소(local_node)와 같아야 한다
    long registered_at = 0;
    long expires_at = 0;
    int expires = 0;
};

/**
 * @brief 등록형 트렁크 레지스트라 — 가입자 REGISTER 흐름(CCscfModule) 앞에서 트렁크 계정의 REGISTER 를 가려 받는다.
 *
 *   - 계정 식별: To 사용자부 == 어떤 enabled Route 의 `auth_user`(그 Route 의 local_node 로 들어왔거나 접속점 미상).
 * RemoteNode 의 주소와 무관 — 등록형 트렁크의 RemoteNode.ip 는 고정 주소일 수도, 동적(0.0.0.0)일 수도 있다.
 *   - 인증: Digest 401(realm = Route `auth_realm`, 비면 Request-URI host 의 서비스 realm) → H(A1)= Route
 * `auth_ha1`(우선) 또는 MD5(auth_user:realm:auth_password). nonce·nc 는 가입자와 같은 CNonceMap. AKA 아님.
 *   - 바인딩: Expires(요청 → 비면 Route `register_expires`) 동안 (소스 ip:port/transport, 접속점). Expires 0 = 해제.
 *   - Route 상태: 바인딩 있음 = alive, 없음(미등록·만료·해제) = dead(CCspRouteMap::SetAlive) → RouteSet 선택에서 빠지고
 * A-COM-003 (mo <node>/csp/peer/<remote_node>) 을 연다. 헬스체크 OPTIONS 는 바인딩 주소로 나간다(CCspRouteHealth).
 *   - 인바운드: 바인딩 소스에서 온 요청은 그 Route 의 것(FindBySource — FindInbound 폴백)이고 등록으로 인증된 것으로
 * 신뢰한다 (IsRegisteredSource — UE 의 등록 바인딩 신뢰와 같은 규칙, Digest 재챌린지 없음).
 *   - 발신: 그 Route 를 고른 호의 다음 홉 = 바인딩 주소(RemoteNode.ip:port 대신 — ModuleDispatcher).
 */
class CCspTrunkRegistrar {
public:
    /** REGISTER 가 트렁크 계정 것이면 여기서 끝낸다(401·403·400·200 을 직접 보낸다) → true. 아니면 false(가입자
     * 흐름으로). */
    bool HandleRegister( CSipMessage *pclsMessage, const std::string &strLocalNodeName,
                         const std::string &strRealmFallback );
    bool Get( const std::string &routeName, TrunkBinding &out ) const;
    /** 소스 주소로 바인딩을 찾아 그 Route 를 돌려준다(없으면 IsValid()==false). UDP 는 포트까지, TCP/TLS 는 IP 만. */
    RouteConfig FindBySource( const std::string &localName, const std::string &srcIp, int srcPort,
                              const std::string &transport ) const;
    /** 이 Route 의 바인딩이 이 소스에서 왔는가 — 등록형 트렁크의 요청 신뢰. */
    bool IsRegisteredSource( const std::string &routeName, const std::string &srcIp, int srcPort,
                             const std::string &transport ) const;
    /** 1초 틱 — 만료 바인딩 회수 + 바인딩 없는 트렁크 Route 를 dead 로(기동·설정 재적재 직후 포함). */
    void Tick( long now );
    std::vector<TrunkBinding> GetAll() const;

private:
    RouteConfig _account( const std::string &strUser, const std::string &strLocalNodeName ) const;
    void _bind( const RouteConfig &rc, const TrunkBinding &tb );
    void _unbind( const RouteConfig &rc, const char *pszWhy );
    static void _setAlive( const RouteConfig &rc, bool bAlive, const char *pszWhy );

    mutable std::mutex m_mutex;
    std::map<std::string, TrunkBinding> m_byRoute;  // route name → 바인딩
};

extern CCspTrunkRegistrar gclsTrunkRegistrar;

#endif  // __CSP_TRUNK_REGISTRAR_H__
