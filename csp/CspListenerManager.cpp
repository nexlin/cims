#include "CspListenerManager.h"

#include "CspConfigCache.h"
#include "Log.h"
#include "SipServerSetup.h"
#include "SipStack.h"
#include "SipUserAgent.h"

extern CSipUserAgent gclsUserAgent;

CCspListenerManager gclsListenerManager;

std::string CCspListenerManager::_normalizeProtocol( const std::string &protocol ) const {
    std::string p;
    p.reserve( protocol.size() );
    for ( char c : protocol ) p.push_back( ( c >= 'a' && c <= 'z' ) ? (char)( c - 'a' + 'A' ) : c );
    if ( p == "UDP" || p == "TCP" || p == "TLS" ) return p;
    return std::string();  // WS/WSS 등 psip 미지원
}

bool CCspListenerManager::_shouldManage( const std::string &protocol ) const {
    return !_normalizeProtocol( protocol ).empty();
}

bool CCspListenerManager::_isAlreadyBound( const std::string &protocol, const std::string &ip, int port ) const {
    auto ipMatch = [&]( const std::string &existIp ) {
        if ( ip == "0.0.0.0" || ip.empty() ) return true;
        if ( existIp == "0.0.0.0" || existIp.empty() ) return true;
        return existIp == ip;
    };

    if ( protocol == "UDP" ) {
        std::vector<CSipStackUdpListener *> v;
        gclsUserAgent.m_clsSipStack.GetUdpListenerInfo( v );
        for ( auto *e : v ) {
            if ( e && e->m_iPort == port && ipMatch( e->m_strBindIp ) ) return true;
        }
    } else if ( protocol == "TCP" ) {
        std::vector<CSipStackTcpListener *> v;
        gclsUserAgent.m_clsSipStack.GetTcpListenerInfo( v );
        for ( auto *e : v ) {
            if ( e && e->m_iPort == port && ipMatch( e->m_strBindIp ) ) return true;
        }
#ifdef USE_TLS
    } else if ( protocol == "TLS" ) {
        std::vector<CSipStackTlsListener *> v;
        gclsUserAgent.m_clsSipStack.GetTlsListenerInfo( v );
        for ( auto *e : v ) {
            if ( e && e->m_iPort == port && ipMatch( e->m_strBindIp ) ) return true;
        }
#endif
    }
    return false;
}

bool CCspListenerManager::_addListenerToStack( const ManagedInfo &m, int &outId ) {
    const char *pszIp = m.bindIp.empty() ? NULL : m.bindIp.c_str();
    if ( m.protocol == "UDP" ) {
        return gclsUserAgent.m_clsSipStack.AddUdpListener( m.id, pszIp, m.port, m.threadCount, outId );
    } else if ( m.protocol == "TCP" ) {
        return gclsUserAgent.m_clsSipStack.AddTcpListener( m.id, pszIp, m.port, outId );
#ifdef USE_TLS
    } else if ( m.protocol == "TLS" ) {
        const char *pszCert = m.tlsCertPath.empty() ? NULL : m.tlsCertPath.c_str();
        const char *pszKey = m.tlsKeyPath.empty() ? NULL : m.tlsKeyPath.c_str();
        const char *pszCa = m.tlsCaPath.empty() ? NULL : m.tlsCaPath.c_str();
        return gclsUserAgent.m_clsSipStack.AddTlsListener( m.id, pszIp, m.port, pszCert, pszKey, pszCa, outId );
#endif
    }
    return false;
}

bool CCspListenerManager::_removeListenerFromStack( const ManagedInfo &m ) {
    if ( m.protocol == "UDP" ) {
        return gclsUserAgent.m_clsSipStack.RemoveUdpListener( m.id );
    } else if ( m.protocol == "TCP" ) {
        return gclsUserAgent.m_clsSipStack.RemoveTcpListener( m.id );
#ifdef USE_TLS
    } else if ( m.protocol == "TLS" ) {
        return gclsUserAgent.m_clsSipStack.RemoveTlsListener( m.id );
#endif
    }
    return false;
}

bool CCspListenerManager::Sync() {
    // R4 (2026-04-23): local_nodes.jsonl 을 소비. UDP + TCP + TLS 지원.
    // 인증서는 stack-global (Setup.Sip.CertFile), per-listener cert 는 R5+.
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_LOCAL_NODE );
    if ( items.type != SimpleJson::JSON_ARRAY ) {
        CLog::Print( LOG_ERROR, "ListenerManager: cache items not array" );
        return false;
    }

    std::vector<ManagedInfo> desired;
    for ( size_t i = 0; i < items.Size(); ++i ) {
        SimpleJson::JsonNode row = items.At( i );
        if ( row.type != SimpleJson::JSON_OBJECT ) continue;
        std::string strEnabled = row.GetString( "enabled" );
        if ( strEnabled == "false" || strEnabled == "0" ) continue;
        std::string protoRaw = row.GetString( "protocol", "UDP" );
        std::string proto = _normalizeProtocol( protoRaw );

        ManagedInfo m;
        m.id = CspUuidToIntId( row.GetString( "id" ) );
        if ( proto.empty() ) {
            CLog::Print( LOG_DEBUG, "ListenerManager: skip unsupported proto id=%d proto=%s", m.id, protoRaw.c_str() );
            continue;
        }
        m.bindIp = row.GetString( "bind_ip", "0.0.0.0" );
        m.port = (int)row.GetInt( "bind_port" );
        m.protocol = proto;

        // thread_count: UDP 만 의미. TCP/TLS 는 accept thread 고정 1.
        int iThreads = (int)row.GetInt( "thread_count", 0 );
        if ( iThreads <= 0 ) iThreads = gclsSetup.m_iUdpThreadCount;
        if ( iThreads <= 0 ) iThreads = 1;
        m.threadCount = iThreads;

        if ( m.port <= 0 || m.id == 0 ) continue;

        // R6 (2026-06-08): 옛 "_isAlreadyBound → skip" 블록 제거.
        //   부트스트랩 UDP 바인딩을 없앤 뒤로는 ListenerManager 가 primary 포함 모든 SIP 리스너를
        //   소유한다. 이 스킵을 남겨두면 자기가 올린 리스너를 desired 에서 제외 → 아래 diff 가
        //   "삭제 대상"으로 오판해 매 reload 마다 리스너를 내려버리는 치명적 flapping 발생.
        //   중복 바인딩은 아래 diff(id+bind 파라미터 비교)가 막는다.

        // R5.c: TLS per-listener cert 경로 수집. 빈 값이면 stack-global cert 사용.
        if ( m.protocol == "TLS" ) {
            m.tlsCertPath = row.GetString( "tls_cert_path" );
            m.tlsKeyPath = row.GetString( "tls_key_path" );
            m.tlsCaPath = row.GetString( "tls_ca_path" );
            if ( !m.tlsCertPath.empty() ) {
                CLog::Print( LOG_INFO, "ListenerManager: id=%d TLS per-listener cert='%s' key='%s' ca='%s'", m.id,
                             m.tlsCertPath.c_str(), m.tlsKeyPath.empty() ? "<same as cert>" : m.tlsKeyPath.c_str(),
                             m.tlsCaPath.empty() ? "<none>" : m.tlsCaPath.c_str() );
            }
        }

        desired.push_back( m );
    }

    std::lock_guard<std::mutex> lk( m_mutex );

    // R6 (2026-06-08): id 만으로 비교하던 옛 diff 는 같은 레코드의 포트/IP 만 바뀌면(=id 동일)
    //   "변화 없음"으로 보고 재바인딩하지 않았다 → 무중단 포트 변경 불가의 원인.
    //   이제 bind 파라미터(port/ip/protocol/threads/cert)까지 비교해, 바뀐 리스너는 remove+add.
    auto findDesired = [&]( int id ) -> const ManagedInfo * {
        for ( const auto &d : desired )
            if ( d.id == id ) return &d;
        return nullptr;
    };
    auto sameBind = []( const ManagedInfo &a, const ManagedInfo &b ) {
        return a.port == b.port && a.bindIp == b.bindIp && a.protocol == b.protocol &&
               a.threadCount == b.threadCount && a.tlsCertPath == b.tlsCertPath &&
               a.tlsKeyPath == b.tlsKeyPath && a.tlsCaPath == b.tlsCaPath;
    };

    // 1) 기존 managed 중: desired 에 없거나(삭제) bind 파라미터가 바뀐(rebind) 것을 stack 에서 제거.
    std::vector<ManagedInfo> stillManaged;
    for ( const auto &m : m_vecManaged ) {
        const ManagedInfo *d = findDesired( m.id );
        if ( d && sameBind( *d, m ) ) {
            stillManaged.push_back( m );  // 변화 없음 — 유지 (재바인딩하지 않음)
            continue;
        }
        if ( _removeListenerFromStack( m ) ) {
            CLog::Print( LOG_SYSTEM, "ListenerManager: removed id=%d %s %s:%d%s", m.id, m.protocol.c_str(),
                         m.bindIp.c_str(), m.port, d ? " (rebind)" : "" );
        } else {
            CLog::Print( LOG_ERROR, "ListenerManager: remove failed id=%d %s", m.id, m.protocol.c_str() );
        }
    }

    std::set<int> managedIds;
    for ( const auto &m : stillManaged ) managedIds.insert( m.id );

    for ( const auto &d : desired ) {
        if ( managedIds.find( d.id ) != managedIds.end() ) continue;
        int iOutId = 0;
        if ( _addListenerToStack( d, iOutId ) ) {
            stillManaged.push_back( d );
            if ( d.protocol == "UDP" ) {
                CLog::Print( LOG_SYSTEM, "ListenerManager: added id=%d %s %s:%d threads=%d", d.id, d.protocol.c_str(),
                             d.bindIp.c_str(), d.port, d.threadCount );
            } else {
                CLog::Print( LOG_SYSTEM, "ListenerManager: added id=%d %s %s:%d", d.id, d.protocol.c_str(),
                             d.bindIp.c_str(), d.port );
            }
        } else {
            CLog::Print( LOG_ERROR, "ListenerManager: add failed id=%d %s %s:%d", d.id, d.protocol.c_str(),
                         d.bindIp.c_str(), d.port );
        }
    }

    m_vecManaged.swap( stillManaged );
    return true;
}

void CCspListenerManager::GetManagedIds( std::vector<int> &out ) {
    std::lock_guard<std::mutex> lk( m_mutex );
    out.clear();
    for ( const auto &m : m_vecManaged ) out.push_back( m.id );
}
