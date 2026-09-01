/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com> (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "UserMap.h"

#include "CspAddressing.h"
#include "IpsecSaSet.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "SipParserDefine.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "TimeString.h"

CUserMap gclsUserMap;

CUserInfo::CUserInfo()
    : m_iPort( 0 ),
      m_eTransport( E_SIP_UDP ),
      m_iLoginTime( 0 ),
      m_iLoginTimeout( 3600 ),
      m_iOptionsSeq( 0 ),
      m_iSendOptionsTime( 0 ),
      m_iLastSeenTime( 0 ),
      m_bMcDataMsrp( false ),
      m_bMediaSecSdes( false ),
      m_iRegisterCSeq( 0 ),
      m_bIntegrityProtected( false ),
      m_iSaReqId( 0 ),
      m_iSendPort( 0 ),
      m_iSendListenerId( 0 ) {
}

void CUserInfo::GetCallRoute( CSipCallRoute &clsRoute ) {
    clsRoute.m_strDestIp = m_strIp;
    clsRoute.m_iDestPort = GetSendPort();
    clsRoute.m_eTransport = m_eTransport;
    if ( m_iSendListenerId > 0 ) {
        // IPsec: 서버 요청은 port_pc 소켓에서 (UE ip, port_us) 로 — Via 자기주소가 그 소켓을 고른다 (SA 3)
        clsRoute.m_strOutboundLocalIp = CspAddressing::GetLocalSipAddress( m_iSendListenerId );
        clsRoute.m_iOutboundLocalPort = CspAddressing::GetLocalSipPort( m_iSendListenerId, 0 );
    }
}

/** 바인딩이 사라질 때 결부 SA 셋을 회수한다 (유예 = 마지막 응답이 SA 위로 나갈 시간) */
static void _releaseBindingSa( const CUserInfo &clsBind, const char *pszWhy ) {
    if ( clsBind.m_iSaReqId == 0 ) return;
    CLog::Print( LOG_DEBUG, "ipsec: binding gone (%s) → release reqid=0x%x", pszWhy, clsBind.m_iSaReqId );
    gclsIpsecSaSetMap.Release( clsBind.m_iSaReqId, IPSEC_RELEASE_GRACE_SEC );
}

size_t CUserMap::_findBinding( const USER_BINDING_LIST &clsList, const std::string &strIp, int iPort,
                               ESipTransport eTransport ) {
    for ( size_t i = 0; i < clsList.size(); ++i ) {
        if ( clsList[i].m_iPort == iPort && clsList[i].m_eTransport == eTransport && clsList[i].m_strIp == strIp )
            return i;
    }
    return (size_t)-1;
}

size_t CUserMap::_pickBinding( const USER_BINDING_LIST &clsList ) {
    size_t iBest = 0, iNewest = 0;
    bool bFoundAlive = false;

    for ( size_t i = 0; i < clsList.size(); ++i ) {
        if ( clsList[i].m_iLoginTime > clsList[iNewest].m_iLoginTime ) iNewest = i;

        // 스트림 transport 는 연결이 살아있어야 도달한다 — 죽은 flow 로 보내면 신규 연결 시도가
        //   되어 NAT 뒤 상대에게는 실패한다. 스택에 직접 묻는다(추측하지 않는다).
        if ( !gclsUserAgent.m_clsSipStack.IsFlowAlive( clsList[i].m_strIp.c_str(), clsList[i].m_iPort,
                                                       clsList[i].m_eTransport ) )
            continue;

        if ( !bFoundAlive || clsList[i].m_iLoginTime > clsList[iBest].m_iLoginTime ) {
            iBest = i;
            bFoundAlive = true;
        }
    }

    // 살아있는 바인딩이 없으면 가장 최근 것 — 도달은 실패하겠지만 종전 동작과 같고 무해하다.
    return bFoundAlive ? iBest : iNewest;
}

CUserMap::CUserMap() {
}

CUserMap::~CUserMap() {
}

/**
 * @ingroup CspServer
 * @brief 로그인된 클라이언트 정보를 저장한다.
 * @param pclsMessage SIP REGISTER 메시지
 * @param pclsXmlUser	XML 에 저장된 사용자 정보
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CUserMap::Insert( CSipMessage *pclsMessage, CspUser *pclsXmlUser, bool bIntegrityProtected,
                       const CUserInfo *pclsIpsec, bool bMediaSecSdes ) {
    CUserInfo clsInfo;
    std::string strUserId;
    USER_MAP::iterator itMap;

    strUserId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
    if ( strUserId.empty() ) return false;

    if ( pclsMessage->GetTopViaIpPort( clsInfo.m_strIp, clsInfo.m_iPort ) == false ) return false;
    clsInfo.m_iLoginTimeout = pclsMessage->GetExpires();

    if ( clsInfo.m_iLoginTimeout == 0 && pclsMessage->IsMethod( SIP_METHOD_REGISTER ) ) {
        return false;
    }

    clsInfo.m_eTransport = pclsMessage->m_eTransport;
    clsInfo.m_bIntegrityProtected = bIntegrityProtected;
    clsInfo.m_bMediaSecSdes = bMediaSecSdes;
    if ( pclsIpsec ) {
        clsInfo.m_iSaReqId = pclsIpsec->m_iSaReqId;
        clsInfo.m_iSendPort = pclsIpsec->m_iSendPort;
        clsInfo.m_iSendListenerId = pclsIpsec->m_iSendListenerId;
    }
    time( &clsInfo.m_iLoginTime );

    // 픽업 그룹 — pickup_group 우선, 미지정이면 org 폴백 (volte_supplementary_services.md §5.1).
    //   등록 시점 스냅샷: 프로비저닝 변경은 다음 등록 갱신부터 반영된다.
    clsInfo.m_strGroupId = pclsXmlUser->EffectivePickupGroup();

    // MCData media plane capability — Contact 의 +g.3gpp.icsi-ref 에 icsi.mcdata 포함 여부
    //   (RFC 3840 feature tag; 등록 단위로 판정 — 바인딩 만료와 함께 소멸)
    if ( pclsMessage->m_clsContactList.empty() == false ) {
        std::string strIcsi;
        if ( pclsMessage->m_clsContactList.front().SelectParam( "+g.3gpp.icsi-ref", strIcsi ) &&
             strIcsi.find( "mcdata" ) != std::string::npos ) {
            clsInfo.m_bMcDataMsrp = true;
        }

        // as-registered Contact URI·파라미터 보관 (200 OK 에코·reginfo <uri>/<unknown-param> 용)
        char szContactUri[256];
        if ( pclsMessage->m_clsContactList.front().m_clsUri.ToString( szContactUri, sizeof( szContactUri ) ) > 0 ) {
            clsInfo.m_strContactUri = szContactUri;
        }
        for ( SIP_PARAMETER_LIST::iterator itParam = pclsMessage->m_clsContactList.front().m_clsParamList.begin();
              itParam != pclsMessage->m_clsContactList.front().m_clsParamList.end(); ++itParam ) {
            if ( strcasecmp( itParam->m_strName.c_str(), "expires" ) != 0 )
                clsInfo.m_clsContactParamList.push_back( *itParam );
        }
    }
    clsInfo.m_iRegisterCSeq = pclsMessage->m_clsCSeq.m_iDigit;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( strUserId );
    if ( itMap == m_clsMap.end() ) {
        // 신규 가입자 — 바인딩 1개로 시작한다. (메서드 무관: 인증된 비REGISTER 요청도 여기로
        //   들어온다 — CscfModule::CheckAuthrization)
        USER_BINDING_LIST clsList;
        clsList.push_back( clsInfo );
        m_clsMap.insert( USER_MAP::value_type( strUserId, clsList ) );
        _indexGroupAdd( clsInfo.m_strGroupId, strUserId );
        CLog::Print( LOG_DEBUG, "user(%s) is inserted (%s:%d:%d) group(%s)", strUserId.c_str(), clsInfo.m_strIp.c_str(),
                     clsInfo.m_iPort, clsInfo.m_eTransport, clsInfo.m_strGroupId.c_str() );
    } else {
        USER_BINDING_LIST &clsList = itMap->second;
        size_t iIdx = _findBinding( clsList, clsInfo.m_strIp, clsInfo.m_iPort, clsInfo.m_eTransport );

        if ( iIdx == (size_t)-1 ) {
            // 새 도달 경로다. **REGISTER 만 바인딩을 만든다**(RFC 3261 §10 — 바인딩 생성은 등록의
            //   권한이다). 비REGISTER 요청(대형 INVITE 승격 후의 ACK/BYE 등)은 새 경로로 보여도
            //   등록되지 않은 flow 이므로 무시한다 — 종전의 transport 가드가 하던 일을 권한 구분이
            //   대신한다.
            //   등록으로 들어온 flow 는 그대로 추가한다: 승격 TCP 로 온 재-REGISTER 도 바인딩이
            //   되지만, 그 연결이 닫히면 _pickBinding 의 생존 판정에서 탈락하므로 도달 주소를
            //   오염시키지 않는다. transport 종류로 추측할 필요가 없어진다
            //   (registration_binding_set.md §2).
            if ( !pclsMessage->IsMethod( SIP_METHOD_REGISTER ) ) {
                m_clsMutex.release();
                return true;
            }

            // 같은 transport 의 기존 바인딩은 **교체**한다 — 한 단말은 한 transport 에 살아있는
            //   경로가 하나뿐이므로, 새 등록이 온 시점에 그 transport 의 옛 경로는 무효다.
            //   (UDP 는 연결이 없어 죽음을 감지할 수 없으므로 이 규칙이 유일한 회수 수단이다.
            //    스트림은 sweep 의 flow 생존 판정이 추가로 회수한다.)
            //   ⚠ 멀티 디바이스를 지원하게 되면 이 규칙을 instance-id 기준으로 완화해야 한다
            //     (registration_binding_set.md §4).
            for ( size_t k = clsList.size(); k > 0; --k ) {
                if ( clsList[k - 1].m_eTransport != clsInfo.m_eTransport ) continue;
                CLog::Print( LOG_DEBUG, "user(%s) binding replaced (%s:%d:%d → %s:%d:%d)", strUserId.c_str(),
                             clsList[k - 1].m_strIp.c_str(), clsList[k - 1].m_iPort, clsList[k - 1].m_eTransport,
                             clsInfo.m_strIp.c_str(), clsInfo.m_iPort, clsInfo.m_eTransport );
                // 재인증으로 새 SA 셋을 확정한 경우 구 셋은 IpsecSaSet 이 retiring 으로 회수한다 — 여기서는
                //   다른 셋(재인증이 아닌 교체)만 회수
                if ( clsList[k - 1].m_iSaReqId != clsInfo.m_iSaReqId ) _releaseBindingSa( clsList[k - 1], "replaced" );
                clsList.erase( clsList.begin() + ( k - 1 ) );
            }

            if ( clsList.size() >= MAX_BINDING_PER_USER ) {
                size_t iOldest = 0;
                for ( size_t k = 1; k < clsList.size(); ++k ) {
                    if ( clsList[k].m_iLoginTime < clsList[iOldest].m_iLoginTime ) iOldest = k;
                }
                CLog::Print( LOG_DEBUG, "user(%s) binding cap — drop oldest (%s:%d:%d)", strUserId.c_str(),
                             clsList[iOldest].m_strIp.c_str(), clsList[iOldest].m_iPort,
                             clsList[iOldest].m_eTransport );
                _releaseBindingSa( clsList[iOldest], "cap" );
                clsList.erase( clsList.begin() + iOldest );
            }
            clsList.push_back( clsInfo );
            iIdx = clsList.size() - 1;
            CLog::Print( LOG_SYSTEM, "user(%s) binding added (%s:%d:%d) — total %d", strUserId.c_str(),
                         clsInfo.m_strIp.c_str(), clsInfo.m_iPort, clsInfo.m_eTransport, (int)clsList.size() );
        } else {
            clsList[iIdx].m_iLastSeenTime = clsInfo.m_iLoginTime;
        }

        CUserInfo &clsBind = clsList[iIdx];
        // 픽업 그룹은 가입자 단위 속성이므로 전 바인딩에 반영한다 (+ 그룹 인덱스 이동).
        if ( !clsList.empty() && clsList[0].m_strGroupId != clsInfo.m_strGroupId ) {
            _indexGroupRemove( clsList[0].m_strGroupId, strUserId );
            _indexGroupAdd( clsInfo.m_strGroupId, strUserId );
        }
        for ( size_t k = 0; k < clsList.size(); ++k ) clsList[k].m_strGroupId = clsInfo.m_strGroupId;

        // 재등록 갱신 — REGISTER 에서만 capability·Contact 재평가 (비REGISTER 갱신은 Contact 미포함)
        if ( pclsMessage->IsMethod( SIP_METHOD_REGISTER ) ) {
            // 바인딩 수명은 그 바인딩만 연장한다 — 다른 flow 의 만료를 대신 늦추지 않는다.
            clsBind.m_iLoginTime = clsInfo.m_iLoginTime;
            clsBind.m_iLoginTimeout = clsInfo.m_iLoginTimeout;
            clsBind.m_bMcDataMsrp = clsInfo.m_bMcDataMsrp;
            clsBind.m_bMediaSecSdes = clsInfo.m_bMediaSecSdes;
            if ( pclsIpsec ) {
                // 같은 flow 의 재등록 — 재인증이면 새 SA 셋으로 결부가 바뀐다 (구 셋은 IpsecSaSet 이 retiring)
                clsBind.m_iSaReqId = clsInfo.m_iSaReqId;
                clsBind.m_iSendPort = clsInfo.m_iSendPort;
                clsBind.m_iSendListenerId = clsInfo.m_iSendListenerId;
            }
            if ( clsInfo.m_strContactUri.empty() == false ) {
                clsBind.m_strContactUri = clsInfo.m_strContactUri;
                clsBind.m_clsContactParamList = clsInfo.m_clsContactParamList;
            }
            clsBind.m_iRegisterCSeq = clsInfo.m_iRegisterCSeq;
        }

        CLog::Print( LOG_DEBUG, "user(%s) is updated (%s:%d:%d) bindings(%d) group(%s)", strUserId.c_str(),
                     clsInfo.m_strIp.c_str(), clsInfo.m_iPort, clsInfo.m_eTransport, (int)clsList.size(),
                     clsInfo.m_strGroupId.c_str() );
    }
    m_clsMutex.release();

    return true;
}

/**
 * @ingroup CspServer
 * @brief 사용자 ID 에 해당하는 정보를 검색한다.
 * @param pszUserId 사용자 ID
 * @param clsInfo		사용자 정보를 저장할 변수
 * @returns 사용자 ID 가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CUserMap::Select( const char *pszUserId, CUserInfo &clsInfo ) {
    bool bRes = false;
    USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() && !itMap->second.empty() ) {
        // 소비자는 "이 가입자에게 보낼 도달 정보 하나"를 원한다 — 여기서 고른다.
        clsInfo = itMap->second[_pickBinding( itMap->second )];
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

/**
 * @ingroup CspServer
 * @brief 사용자 아이디가 존재하는지 검색한다.
 * @param pszUserId 사용자 아이디
 * @returns 사용자 아이디가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CUserMap::Select( const char *pszUserId ) {
    bool bRes = false;
    USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

bool CUserMap::IsIntegrityProtected( const char *pszUserId ) {
    bool bRes = false;
    m_clsMutex.acquire();
    USER_MAP::iterator itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        for ( size_t i = 0; i < itMap->second.size(); ++i ) {
            if ( itMap->second[i].m_bIntegrityProtected ) {
                bRes = true;
                break;
            }
        }
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief 그룹 아이디에 포함된 사용자 아이디 리스트를 저장한다.
 * @param pszGroupId	그룹 아이디
 * @param clsList			사용자 아이디 리스트 저장 객체
 * @returns 그룹 아이디에 포함된 사용자 리스트가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CUserMap::SelectGroup( const char *pszGroupId, USER_ID_LIST &clsList ) {
    clsList.clear();
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;

    m_clsMutex.acquire();
    // 그룹 인덱스 조회 — 등록 가입자 전수 스캔 없이 그룹원만 답한다 (그룹은 가입자 단위 속성).
    std::map<std::string, std::set<std::string> >::iterator itIdx = m_clsGroupIndex.find( pszGroupId );
    if ( itIdx != m_clsGroupIndex.end() ) {
        for ( std::set<std::string>::iterator it = itIdx->second.begin(); it != itIdx->second.end(); ++it ) {
            clsList.push_back( *it );
        }
    }
    m_clsMutex.release();

    if ( clsList.empty() == false ) return true;

    return false;
}

void CUserMap::_indexGroupAdd( const std::string &strGroupId, const std::string &strUserId ) {
    if ( strGroupId.empty() ) return;
    m_clsGroupIndex[strGroupId].insert( strUserId );
}

void CUserMap::_indexGroupRemove( const std::string &strGroupId, const std::string &strUserId ) {
    if ( strGroupId.empty() ) return;
    std::map<std::string, std::set<std::string> >::iterator it = m_clsGroupIndex.find( strGroupId );
    if ( it == m_clsGroupIndex.end() ) return;
    it->second.erase( strUserId );
    if ( it->second.empty() ) m_clsGroupIndex.erase( it );
}

/**
 * @ingroup CspServer
 * @brief 사용자 아이디를 자료구조에서 삭제한다.
 * @param pszUserId 사용자 아이디
 * @returns 사용자 아이디를 자료구조에서 삭제하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CUserMap::Delete( const char *pszUserId ) {
    bool bRes = false;
    USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        for ( size_t i = 0; i < itMap->second.size(); ++i ) _releaseBindingSa( itMap->second[i], "unregistered" );
        if ( !itMap->second.empty() ) _indexGroupRemove( itMap->second[0].m_strGroupId, itMap->first );
        m_clsMap.erase( itMap );
        CLog::Print( LOG_DEBUG, "user(%s) is deleted", pszUserId );
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

void CUserMap::TouchFlow( const char *pszUserId, ESipTransport eTransport ) {
    USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        for ( size_t i = 0; i < itMap->second.size(); ++i ) {
            if ( itMap->second[i].m_eTransport == eTransport ) time( &itMap->second[i].m_iLastSeenTime );
        }
    }
    m_clsMutex.release();
}

bool CUserMap::SetIpPort( const char *pszUserId, const char *pszIp, int iPort, ESipTransport eTransport ) {
    bool bRes = false;
    USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        // 비REGISTER 요청의 주소 변경 감지 경로다 — 바인딩을 **만들지는 않고**(생성은 등록의
        //   권한) 같은 transport 의 기존 바인딩 주소만 옮긴다. NAT rebind 후 첫 요청이 REGISTER 가
        //   아닌 경우의 도달을 살리는 용도.
        for ( size_t i = 0; i < itMap->second.size(); ++i ) {
            if ( itMap->second[i].m_eTransport != eTransport ) continue;
            // IPsec 바인딩은 SA selector 가 (ip, port_uc) 를 고정한다 — 다른 주소의 요청은 게이트가 이미
            //   거절했고, 옮기면 SA 와 어긋난다
            if ( itMap->second[i].m_iSaReqId != 0 ) continue;
            itMap->second[i].m_strIp = pszIp;
            itMap->second[i].m_iPort = iPort;
            time( &itMap->second[i].m_iLastSeenTime );
            CLog::Print( LOG_DEBUG, "user(%s) binding moved → %s:%d:%d", pszUserId, pszIp, iPort, eTransport );
            bRes = true;
            break;
        }
    }
    m_clsMutex.release();

    return bRes;
}

/**
 * @ingroup CspServer
 * @brief 만료된 사용자를 자료구조에서 삭제한다.
 * @param iTimeout 만료된 시간 이후에 대기 시간 (초단위)
 */
void CUserMap::DeleteTimeout( int iTimeout ) {
    USER_ID_LIST clsDummy;
    DeleteTimeout( iTimeout, clsDummy );
}

/**
 * @ingroup CspServer
 * @brief 만료된 사용자를 자료구조에서 삭제하고, 삭제된 사용자 ID 를 clsDeletedList 에 저장한다.
 * @param iTimeout       만료 이후 대기 시간 (초단위)
 * @param clsDeletedList 삭제된 사용자 ID 를 받는 리스트
 */
void CUserMap::DeleteTimeout( int iTimeout, USER_ID_LIST &clsDeletedList ) {
    USER_INFO_LIST clsInfoList;

    DeleteTimeout( iTimeout, clsInfoList );

    clsDeletedList.clear();
    for ( USER_INFO_LIST::iterator itList = clsInfoList.begin(); itList != clsInfoList.end(); ++itList ) {
        clsDeletedList.push_back( itList->first );
    }
}

/**
 * @ingroup CspServer
 * @brief 만료된 사용자를 삭제하고, (ID, 삭제 시점 바인딩) 쌍을 반환한다 — reg-event NOTIFY 용.
 * @param iTimeout           만료 이후 대기 시간 (초단위)
 * @param clsDeletedInfoList 삭제된 사용자 (ID, 바인딩) 을 받는 리스트
 */
void CUserMap::DeleteTimeout( int iTimeout, USER_INFO_LIST &clsDeletedInfoList ) {
    USER_MAP::iterator itMap, itNext;
    time_t iTime;

    clsDeletedInfoList.clear();
    time( &iTime );

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ) {
        USER_BINDING_LIST &clsList = itMap->second;
        CUserInfo clsLastRemoved;
        bool bRemoved = false;

        // 만료는 **바인딩 단위**다 — 한 flow 가 만료돼도 다른 flow 로 등록이 살아 있을 수 있다.
        for ( size_t i = clsList.size(); i > 0; --i ) {
            const CUserInfo &clsBind = clsList[i - 1];
            const bool bTimeout = iTime > ( clsBind.m_iLoginTime + clsBind.m_iLoginTimeout + iTimeout );
            // RFC 5626 — **flow 실패는 바인딩 무효**다. 스트림 transport 는 연결 생존을 스택에
            //   물어 죽은 경로를 즉시 회수한다. 등록 만료만 기다리면 Expires+grace(우리 배치
            //   기준 최대 ~77분) 동안 유령 바인딩이 남아 운영 조회·통지에 노출된다.
            //   UDP 는 연결 개념이 없어 판정 대상이 아니다 — 같은 transport 재등록으로 교체된다.
            const bool bFlowDead = ( clsBind.m_eTransport != E_SIP_UDP ) &&
                                   !gclsUserAgent.m_clsSipStack.IsFlowAlive( clsBind.m_strIp.c_str(), clsBind.m_iPort,
                                                                             clsBind.m_eTransport );
            if ( bTimeout || bFlowDead ) {
                CLog::Print( LOG_DEBUG, "user(%s) binding removed (%s:%d:%d) — %s", itMap->first.c_str(),
                             clsBind.m_strIp.c_str(), clsBind.m_iPort, clsBind.m_eTransport,
                             bTimeout ? "expired" : "flow dead" );
                // 마지막으로 남았던(가장 최근 등록) 바인딩을 통지용으로 보존한다 —
                //   reg-event NOTIFY 는 삭제 직전 바인딩으로 목적지와 본문을 만든다.
                if ( !bRemoved || clsBind.m_iLoginTime > clsLastRemoved.m_iLoginTime ) {
                    clsLastRemoved = clsBind;
                    bRemoved = true;
                }
                _releaseBindingSa( clsBind, bTimeout ? "expired" : "flow dead" );
                clsList.erase( clsList.begin() + ( i - 1 ) );
            }
        }

        // 마지막 바인딩이 사라지면 등록 해제다 — 그때만 가입자를 제거하고 통지 대상으로 넘긴다.
        if ( clsList.empty() ) {
            CLog::Print( LOG_DEBUG, "user(%s) is deleted - timeout", itMap->first.c_str() );
            _indexGroupRemove( clsLastRemoved.m_strGroupId, itMap->first );
            clsDeletedInfoList.push_back( std::make_pair( itMap->first, clsLastRemoved ) );
            itMap = m_clsMap.erase( itMap );
        } else {
            ++itMap;
        }
    }
    m_clsMutex.release();
}

/**
 * @ingroup CspServer
 * @brief 로그인된 모든 사용자에게 OPTIONS 메시지를 전송한다.
 */
void CUserMap::SendOptions() {
    USER_MAP::iterator itMap;
    USER_ID_LIST clsList;
    USER_ID_LIST::iterator itList;
    time_t iTime;

    if ( gclsSetup.m_iSendOptionsPeriod <= 0 ) return;

    time( &iTime );

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        // keepalive 는 바인딩(도달 경로) 단위 — 목적은 그 경로의 NAT 매핑 유지다.
        //   전 바인딩을 대상으로 하되 주기는 바인딩별로 센다.
        bool bDue = false;
        for ( size_t i = 0; i < itMap->second.size(); ++i ) {
            CUserInfo &clsBind = itMap->second[i];
            time_t iRef = clsBind.m_iSendOptionsTime == 0 ? clsBind.m_iLoginTime : clsBind.m_iSendOptionsTime;
            if ( ( iTime - iRef ) < gclsSetup.m_iSendOptionsPeriod ) continue;

            clsBind.m_iSendOptionsTime = iTime;
            ++clsBind.m_iOptionsSeq;
            if ( clsBind.m_iOptionsSeq > 10000000 ) clsBind.m_iOptionsSeq = 1;
            bDue = true;
        }
        if ( bDue ) clsList.push_back( itMap->first );
    }
    m_clsMutex.release();

    for ( itList = clsList.begin(); itList != clsList.end(); ++itList ) {
        CUserInfo clsUserInfo;

        if ( Select( itList->c_str(), clsUserInfo ) == false ) {
            continue;
        }

        CSipMessage *pclsMessage = new CSipMessage();
        if ( pclsMessage == NULL ) break;

        pclsMessage->m_strSipMethod = SIP_METHOD_OPTIONS;
        pclsMessage->m_clsReqUri.Set( SIP_PROTOCOL, itList->c_str(), clsUserInfo.m_strIp.c_str(),
                                      clsUserInfo.GetSendPort() );
        if ( clsUserInfo.m_iSendListenerId > 0 ) {
            // IPsec: port_pc 소켓에서 port_us 로 (SA 3) — Via 자기주소로 소켓을 고른다
            pclsMessage->AddVia( CspAddressing::GetLocalSipAddress( clsUserInfo.m_iSendListenerId ).c_str(),
                                 CspAddressing::GetLocalSipPort( clsUserInfo.m_iSendListenerId, 0 ) );
        }

        // R6: From URI 는 access_services 의 server_identity_uri 기반으로 생성.
        //   volte 서비스 default. helper 가 URI 문자열 ("sip:cspserver@domain") 반환 →
        //   CSipUri::Parse 로 user/host 분리. 파싱 실패 시 primary LocalIp fallback.
        // R5.b: Call-ID host 는 outbound access edge local addr 유지.
        const std::string strIdentity = CspAddressing::GetServerIdentityForService( "volte" );
        CSipUri clsId;
        if ( clsId.Parse( strIdentity.c_str(), (int)strIdentity.size() ) > 0 && !clsId.m_strHost.empty() ) {
            pclsMessage->m_clsFrom.m_clsUri.Set( SIP_PROTOCOL,
                                                 clsId.m_strUser.empty() ? "cspserver" : clsId.m_strUser.c_str(),
                                                 clsId.m_strHost.c_str() );
        } else {
            const std::string strSipAddr = CspAddressing::GetLocalSipAddressForOutbound( "UDP", "access" );
            pclsMessage->m_clsFrom.m_clsUri.Set( SIP_PROTOCOL, "cspserver", strSipAddr.c_str() );
        }
        pclsMessage->m_clsFrom.InsertTag();

        pclsMessage->m_clsTo.m_clsUri.Set( SIP_PROTOCOL, itList->c_str(), clsUserInfo.m_strIp.c_str(),
                                           clsUserInfo.m_iPort );

        const std::string strCallIdHost = CspAddressing::GetLocalSipAddressForOutbound( "UDP", "access" );
        pclsMessage->m_clsCallId.Make( strCallIdHost.c_str() );

        pclsMessage->m_clsCSeq.Set( clsUserInfo.m_iOptionsSeq, SIP_METHOD_OPTIONS );
        pclsMessage->AddRoute( clsUserInfo.m_strIp.c_str(), clsUserInfo.GetSendPort() );

        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsMessage );
    }
}

/**
 * @ingroup CspServer
 * @brief 자료구조 모니터링용 문자열을 생성한다.
 * @param strBuf 자료구조 모니터링용 문자열 변수
 */
void CUserMap::GetRegisteredUsers( USER_ID_LIST &clsList ) {
    USER_MAP::iterator itMap;
    clsList.clear();

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        clsList.push_back( itMap->first );
    }
    m_clsMutex.release();
}

void CUserMap::GetString( CMonitorString &strBuf ) {
    USER_MAP::iterator itMap;

    strBuf.Clear();

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        // 바인딩마다 한 행 — 가입자가 여러 도달 경로를 가질 수 있다.
        for ( size_t i = 0; i < itMap->second.size(); ++i ) {
            strBuf.AddCol( itMap->first );
            strBuf.AddCol( itMap->second[i].m_strIp, itMap->second[i].m_iPort );
            strBuf.AddCol( itMap->second[i].m_iLoginTime );
            strBuf.AddRow( itMap->second[i].m_iLoginTimeout );
        }
    }
    m_clsMutex.release();
}
