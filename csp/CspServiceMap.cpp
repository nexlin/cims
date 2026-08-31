#include "CspServiceMap.h"

#include <algorithm>

#include "CspConfigCache.h"
#include "CspLocalNodeMap.h"
#include "CspUser.h"
#include "Log.h"
#include "SimpleJson.h"

CCspServiceMap gclsServiceMap;

bool CCspServiceMap::Sync() {
    // v3 (2026-04-22): access_services.jsonl 로드.
    //   - UUID string → hash int (레거시 int id 호환).
    //   - allowed_local_node_refs[] (name) → listeners[] (LocalNode hash int) 파생.
    //   - kind 는 volte/ptt 만 허용 (ibcf 는 RouteSet 으로 이관).
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_ACCESS_SERVICE );
    std::vector<ServiceInfo> newList;
    if ( items.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < items.Size(); ++i ) {
            SimpleJson::JsonNode row = items.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            ServiceInfo s;
            s.uuid = row.GetString( "id" );
            if ( !s.uuid.empty() ) {
                s.id = CspUuidToIntId( s.uuid );
            } else {
                s.id = (int)row.GetInt( "id" );
            }
            s.name = row.GetString( "name" );
            s.kind = row.GetString( "kind" );
            s.domain = row.GetString( "domain" );
            s.auth_realm = row.GetString( "auth_realm" );
            s.server_identity_uri = row.GetString( "server_identity_uri" );
            s.inbound_policy = row.GetString( "inbound_policy", "any" );
            s.media_nat_mode = row.GetString( "media_nat_mode", "off" );
            s.latch_ip_guard = row.GetString( "latch_ip_guard", "strict" );
            // 미디어 SRTP 정책 (media_security.md §4) — 미상 값은 조용히 승격/강등하지 않고 off
            s.media_srtp = row.GetString( "media_srtp", "off" );
            if ( s.media_srtp != "off" && s.media_srtp != "optional" && s.media_srtp != "required" ) {
                CLog::Print( LOG_ERROR, "AccessServiceMap: service '%s' has invalid media_srtp '%s' → off",
                             s.name.c_str(), s.media_srtp.c_str() );
                s.media_srtp = "off";
            }
            // TLS 결합 (§4) — SDES 키가 SDP 본문에 실리므로 required 는 채널 정책 TLS(또는
            //   ipsec-3gpp) 강제가 전제다. 집행은 채널 게이트(sip_transport) 몫 — 여기서는
            //   가입자별 정책을 검사할 수 없어 결합 전제를 드러내기만 한다(정책은 유지).
            if ( s.media_srtp == "required" )
                CLog::Print( LOG_ERROR,
                             "AccessServiceMap: service '%s' media_srtp=required — SDES keys ride in SDP; ensure "
                             "subscribers of this service enforce sip_transport=TLS (channel gate)",
                             s.name.c_str() );
            s.priority = (int)row.GetInt( "priority", 100 );
            std::string en = row.GetString( "enabled" );
            s.enabled = ( en != "false" && en != "0" );

            // sec_mechanisms[] — ipsec-3gpp 는 ESP transport mode 라 NAT 와 상호배제 (§8.3 정적 겹)
            SimpleJson::JsonNode mechs = row.Get( "sec_mechanisms" );
            if ( mechs.type == SimpleJson::JSON_ARRAY ) {
                for ( size_t j = 0; j < mechs.Size(); ++j ) {
                    if ( mechs.At( j ).AsString() == "ipsec-3gpp" ) s.sec_ipsec = true;
                }
            }
            if ( s.sec_ipsec && s.media_nat_mode != "off" ) {
                CLog::Print( LOG_ERROR,
                             "AccessServiceMap: service '%s' offers ipsec-3gpp with media_nat_mode=%s — ESP transport "
                             "mode cannot traverse NAT; ipsec-3gpp ignored (set media_nat_mode=off)",
                             s.name.c_str(), s.media_nat_mode.c_str() );
                s.sec_ipsec = false;
            }

            // v3: allowed_local_node_refs (string name 배열) — LocalNodeMap 으로 int id 파생.
            SimpleJson::JsonNode refs = row.Get( "allowed_local_node_refs" );
            if ( refs.type == SimpleJson::JSON_ARRAY ) {
                for ( size_t j = 0; j < refs.Size(); ++j ) {
                    std::string name = refs.At( j ).AsString();
                    if ( name.empty() ) continue;
                    s.allowed_local_node_refs.push_back( name );
                    LocalNodeInfo ln = gclsLocalNodeMap.GetByName( name );
                    if ( ln.IsValid() && !ln.id.empty() ) {
                        s.listeners.push_back( CspUuidToIntId( ln.id ) );
                    } else {
                        CLog::Print( LOG_ERROR, "AccessServiceMap: service '%s' references missing local_node '%s'",
                                     s.name.c_str(), name.c_str() );
                    }
                }
            }

            // kind 검증 — v3 는 volte/ptt 만
            if ( s.kind != "volte" && s.kind != "ptt" ) {
                if ( !s.kind.empty() ) {
                    CLog::Print(
                        LOG_ERROR,
                        "AccessServiceMap: service '%s' has unsupported kind '%s' (expected volte|ptt) — skipped",
                        s.name.c_str(), s.kind.c_str() );
                }
                continue;
            }
            if ( s.id > 0 && !s.domain.empty() ) newList.push_back( s );
        }
    }
    std::sort( newList.begin(), newList.end(), []( const ServiceInfo &a, const ServiceInfo &b ) {
        if ( a.priority != b.priority ) return a.priority < b.priority;
        return a.id < b.id;
    } );
    std::lock_guard<std::mutex> lk( m_mutex );
    m_services.swap( newList );
    CLog::Print( LOG_INFO, "ServiceMap: sync complete, %zu services", m_services.size() );
    return true;
}

ServiceInfo CCspServiceMap::GetById( int id ) const {
    // v3 (2026-04-22): access_services.jsonl 이 SOT.
    //   Setup.Realm 기반 default-compat fallback 제거. 서비스 정의 없으면 빈 값 반환.
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &s : m_services ) {
        if ( s.id == id ) return s;
    }
    return ServiceInfo();
}

ServiceInfo CCspServiceMap::GetByName( const std::string &name ) const {
    if ( name.empty() ) return ServiceInfo();
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &s : m_services ) {
        if ( s.name == name ) return s;
    }
    return ServiceInfo();
}

ServiceInfo CCspServiceMap::GetByDomain( const std::string &domain ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &s : m_services ) {
        if ( s.enabled && s.domain == domain ) return s;
    }
    return ServiceInfo();
}

std::vector<ServiceInfo> CCspServiceMap::GetAll() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_services;
}

std::string CCspServiceMap::EffectiveRealm( const ServiceInfo &svc ) {
    return svc.auth_realm.empty() ? svc.domain : svc.auth_realm;
}

ServiceInfo CCspServiceMap::GetByKind( const std::string &kind ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &s : m_services ) {
        if ( s.enabled && s.kind == kind ) return s;
    }
    return ServiceInfo();
}

std::string CCspServiceMap::GetDomainByKind( const std::string &kind ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &s : m_services ) {
        if ( s.enabled && s.kind == kind ) return s.domain;
    }
    return "";
}

bool CCspServiceMap::IsInboundAllowed( const ServiceInfo &svc, int listenerIntId ) {
    if ( !svc.enabled ) return false;
    if ( svc.inbound_policy != "restricted" ) return true;  // "any" 또는 미지정 → 허용
    if ( listenerIntId <= 0 ) return false;                 // listener 모름 + restricted → 거절
    // IPsec 접속점의 역할 리스너(port_ps UDP/TCP)는 레코드 id 로 정규화해 대조한다
    const int iRecordId = gclsLocalNodeMap.ToRecordIntId( listenerIntId );
    for ( int lid : svc.listeners ) {
        if ( lid == listenerIntId || lid == iRecordId ) return true;
    }
    return false;
}

std::map<std::string, std::string> CCspServiceMap::BuildDomainToKindMap() const {
    std::map<std::string, std::string> out;
    std::lock_guard<std::mutex> lk( m_mutex );
    // m_services 는 priority 낮은(=우선도 높은) 순 정렬되어 있음.
    // 첫 우선 항목을 남기고, 이후 중복 domain 은 무시.
    for ( const auto &s : m_services ) {
        if ( !s.enabled ) continue;
        if ( s.domain.empty() ) continue;
        if ( out.find( s.domain ) != out.end() ) continue;
        out[s.domain] = s.kind;
    }
    return out;
}

ServiceInfo CCspServiceMap::GetForUser( const std::string &userId, const std::string &fallbackKind ) const {
    if ( !userId.empty() ) {
        CspUser clsUser;
        if ( gclsCspUserMap.Select( userId.c_str(), clsUser ) && !clsUser.m_strServiceRef.empty() ) {
            ServiceInfo svc = GetByName( clsUser.m_strServiceRef );
            if ( svc.id > 0 ) return svc;
        }
    }
    return GetByKind( fallbackKind );
}

// RFC1918/링크로컬 — SDP 선언 미디어 주소가 사설이면 NAT 뒤 단말 신호.
static bool _IsPrivateIp( const std::string &ip ) {
    unsigned a = 0, b = 0;
    if ( sscanf( ip.c_str(), "%u.%u", &a, &b ) != 2 ) return false;
    if ( a == 10 ) return true;
    if ( a == 172 && b >= 16 && b <= 31 ) return true;
    if ( a == 192 && b == 168 ) return true;
    if ( a == 169 && b == 254 ) return true;
    return false;
}

bool CCspServiceMap::EvalMediaNat( const ServiceInfo &svc, const std::string &sdpIp, const std::string &sigIp,
                                   std::string &outGuardIp ) {
    outGuardIp.clear();
    bool bNat = false;
    if ( svc.media_nat_mode == "force" ) {
        bNat = true;
    } else if ( svc.media_nat_mode == "auto" ) {
        bNat = _IsPrivateIp( sdpIp ) || ( !sigIp.empty() && !sdpIp.empty() && sdpIp != sigIp );
    }
    if ( bNat && svc.latch_ip_guard != "off" ) outGuardIp = sigIp;
    return bNat;
}
