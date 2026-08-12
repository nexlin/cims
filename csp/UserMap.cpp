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
#include "Log.h"
#include "MemoryDebug.h"
#include "SipParserDefine.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "TimeString.h"

CUserMap gclsUserMap;

CUserInfo::CUserInfo()
    : m_iPort( 0 ),
      m_iLoginTime( 0 ),
      m_iLoginTimeout( 3600 ),
      m_iOptionsSeq( 0 ),
      m_iSendOptionsTime( 0 ),
      m_bMcDataMsrp( false ),
      m_iRegisterCSeq( 0 ) {
}

void CUserInfo::GetCallRoute( CSipCallRoute &clsRoute ) {
    clsRoute.m_strDestIp = m_strIp;
    clsRoute.m_iDestPort = m_iPort;
    clsRoute.m_eTransport = m_eTransport;
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
bool CUserMap::Insert( CSipMessage *pclsMessage, CspUser *pclsXmlUser ) {
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
    time( &clsInfo.m_iLoginTime );

    clsInfo.m_strGroupId = pclsXmlUser->m_strOrganizationId;

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
        m_clsMap.insert( USER_MAP::value_type( strUserId, clsInfo ) );
        CLog::Print( LOG_DEBUG, "user(%s) is inserted (%s:%d:%d) group(%s)", strUserId.c_str(), clsInfo.m_strIp.c_str(),
                     clsInfo.m_iPort, clsInfo.m_eTransport, clsInfo.m_strGroupId.c_str() );
    } else {
        // SIP REGISTER 를 제외한 요청에서 IP 주소 또는 포트 번호가 변경된 경우, m_iLoginTimeout 를 0 으로 저장하지 않기
        // 위해서 각 멤버별로 저장함
        //
        // 서버→UE 도달 주소(latch)는 **상시 살아 있는 경로**여야 한다 — 서버가 먼저 거는
        // 요청(fan-out INVITE·NOTIFY)의 목적지이기 때문이다. 송신 transport 는 이 값을 그대로
        // 따르므로(CspServer/GroupCallService 의 SendDest 3곳), 여기에 무엇이 담기느냐가
        // 도달 가능성을 결정한다.
        //
        // 단말이 대형 요청을 RFC 3261 §18.1.1 로 TCP 승격하면 그 다이얼로그의 후속(ACK/BYE)·
        // 재-REGISTER(RFC 5626 ;ob 플로우 재사용)까지 같은 TCP 로 오지만, 그 TCP 연결은
        // 유휴 타이머로 곧 닫힌다(pjsip 실측). 닫힌 뒤 그 주소로는 서버가 도달할 수 없다 —
        // NAT 뒤 단말에 서버가 TCP 를 새로 걸 수는 없기 때문이다. 반면 UDP 등록 플로우는
        // keepalive 로 상시 유지된다. 그래서 latch 갱신은 UDP 소스로 한정한다(REGISTER 도
        // 예외 아님 — 0.2.84 에서 REGISTER 예외로 오염 재발 실측).
        //
        // 최초 등록(작은 REGISTER)은 UDP 라 삽입 시 UDP latch 가 수립되고, 이후 UDP 갱신·
        // keepalive 로 유지된다. TCP 로 온 REGISTER 는 바인딩 수명·Contact 만 갱신한다(아래).
        if ( pclsMessage->m_eTransport == E_SIP_UDP ) {
            itMap->second.m_strIp = clsInfo.m_strIp;
            itMap->second.m_iPort = clsInfo.m_iPort;
            itMap->second.m_eTransport = clsInfo.m_eTransport;
        }
        itMap->second.m_strGroupId = clsInfo.m_strGroupId;
        // 재등록 갱신 — REGISTER 에서만 capability·Contact 재평가 (비REGISTER 갱신은 Contact 미포함)
        if ( pclsMessage->IsMethod( SIP_METHOD_REGISTER ) ) {
            // 바인딩 수명 연장 — 재등록이 만료시각을 리셋해야 sweep 에 의한 유령 만료가 없다
            itMap->second.m_iLoginTime = clsInfo.m_iLoginTime;
            itMap->second.m_iLoginTimeout = clsInfo.m_iLoginTimeout;
            itMap->second.m_bMcDataMsrp = clsInfo.m_bMcDataMsrp;
            if ( clsInfo.m_strContactUri.empty() == false ) {
                itMap->second.m_strContactUri = clsInfo.m_strContactUri;
                itMap->second.m_clsContactParamList = clsInfo.m_clsContactParamList;
            }
            itMap->second.m_iRegisterCSeq = clsInfo.m_iRegisterCSeq;
        }

        CLog::Print( LOG_DEBUG, "user(%s) is updated (%s:%d:%d) group(%s)", strUserId.c_str(), clsInfo.m_strIp.c_str(),
                     clsInfo.m_iPort, clsInfo.m_eTransport, clsInfo.m_strGroupId.c_str() );
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
    if ( itMap != m_clsMap.end() ) {
        clsInfo = itMap->second;
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

/**
 * @ingroup CspServer
 * @brief 그룹 아이디에 포함된 사용자 아이디 리스트를 저장한다.
 * @param pszGroupId	그룹 아이디
 * @param clsList			사용자 아이디 리스트 저장 객체
 * @returns 그룹 아이디에 포함된 사용자 리스트가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CUserMap::SelectGroup( const char *pszGroupId, USER_ID_LIST &clsList ) {
    USER_MAP::iterator itMap;

    clsList.clear();

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        if ( !strcmp( pszGroupId, itMap->second.m_strGroupId.c_str() ) ) {
            clsList.push_back( itMap->first );
        }
    }
    m_clsMutex.release();

    if ( clsList.empty() == false ) return true;

    return false;
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
        m_clsMap.erase( itMap );
        CLog::Print( LOG_DEBUG, "user(%s) is deleted", pszUserId );
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

bool CUserMap::SetIpPort( const char *pszUserId, const char *pszIp, int iPort ) {
    bool bRes = false;
    USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.m_strIp = pszIp;
        itMap->second.m_iPort = iPort;
        CLog::Print( LOG_DEBUG, "user(%s) ip(%s) port(%d)", pszUserId, pszIp, iPort );
        bRes = true;
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
        if ( iTime > ( itMap->second.m_iLoginTime + itMap->second.m_iLoginTimeout + iTimeout ) ) {
            CLog::Print( LOG_DEBUG, "user(%s) is deleted - timeout", itMap->first.c_str() );
            clsDeletedInfoList.push_back( std::make_pair( itMap->first, itMap->second ) );
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
        if ( itMap->second.m_iSendOptionsTime == 0 ) {
            if ( ( iTime - itMap->second.m_iLoginTime ) < gclsSetup.m_iSendOptionsPeriod ) {
                continue;
            }
        } else {
            if ( ( iTime - itMap->second.m_iSendOptionsTime ) < gclsSetup.m_iSendOptionsPeriod ) {
                continue;
            }
        }

        itMap->second.m_iSendOptionsTime = iTime;

        ++itMap->second.m_iOptionsSeq;
        if ( itMap->second.m_iOptionsSeq > 10000000 ) itMap->second.m_iOptionsSeq = 1;

        clsList.push_back( itMap->first );
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
        pclsMessage->m_clsReqUri.Set( SIP_PROTOCOL, itList->c_str(), clsUserInfo.m_strIp.c_str(), clsUserInfo.m_iPort );

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
        pclsMessage->AddRoute( clsUserInfo.m_strIp.c_str(), clsUserInfo.m_iPort );

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
        strBuf.AddCol( itMap->first );
        strBuf.AddCol( itMap->second.m_strIp, itMap->second.m_iPort );
        strBuf.AddCol( itMap->second.m_iLoginTime );
        strBuf.AddRow( itMap->second.m_iLoginTimeout );
    }
    m_clsMutex.release();
}
