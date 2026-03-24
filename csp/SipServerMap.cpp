#include "SipServerMap.h"

#include "CspServer.h"
#include "Directory.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "SipServer.h"
#include "SipServerSetup.h"

CSipServerMap gclsSipServerMap;

CSipServerMap::CSipServerMap() {
}

CSipServerMap::~CSipServerMap() {
}

// JSON 파일 또는 MySQL 에서 IP-PBX 정보를 읽는다. 
bool CSipServerMap::Load() {
    bool bRes = false;
    SIP_SERVER_MAP::iterator itMap;

    // 추가/수정/변화없음이 아닌 IP-PBX 정보를 삭제 대상으로 저장한다.
    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        itMap->second.m_iFlag = FLAG_DELETE;
    }
    m_clsMutex.release();

    if ( gclsSetup.m_strSipServerDataFolder.length() > 0 ) {
        CLog::Print( LOG_DEBUG, "SipServerMap Load" );
        bRes = ReadDir( gclsSetup.m_strSipServerDataFolder.c_str() );
    }

    return bRes;
}

// 폴더에서 IP-PBX 정보를 저장한 JSON 파일을 읽어서 자료구조에 저장한다.
bool CSipServerMap::ReadDir( const char *pszDirName ) {
    FILE_LIST clsFileList;
    FILE_LIST::iterator itFL;
    std::string strFileName;

    if ( CDirectory::FileList( pszDirName, clsFileList ) == false ) return false;

    for ( itFL = clsFileList.begin(); itFL != clsFileList.end(); ++itFL ) {
        CspSipServer clsSipServer;

        strFileName = pszDirName;
        CDirectory::AppendName( strFileName, itFL->c_str() );

        if ( clsSipServer.Parse( strFileName.c_str() ) ) {
            clsSipServer.m_strName = itFL->c_str();
            Insert( clsSipServer );
        }
    }

    return true;
}

// IP-PBX 정보 저장 자료구조의 정보를 SipUserAgent 라이브러리에 전달한다.
bool CSipServerMap::SetSipUserAgentRegisterInfo() {
    SIP_SERVER_MAP::iterator itMap, itNext;

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
    LOOP_START:
        if ( itMap->second.m_iFlag == FLAG_INSERT ) {
            gclsUserAgent.InsertRegisterInfo( itMap->second );
        } else if ( itMap->second.m_iFlag == FLAG_UPDATE ) {
            gclsUserAgent.UpdateRegisterInfo( itMap->second );
        } else if ( itMap->second.m_iFlag == FLAG_DELETE ) {
            gclsUserAgent.DeleteRegisterInfo( itMap->second );

            itNext = itMap;
            ++itNext;

            m_clsMap.erase( itMap );
            if ( itNext == m_clsMap.end() ) break;

            itMap = itNext;
            goto LOOP_START;
        }
    }
    m_clsMutex.release();

    return true;
}

// IP-PBX 정보 저장 자료구조에 존재하는지 검사한다.
// pszIp			IP-PBX 의 IP 주소
// pszUserId IP-PBX 에 로그인한 사용자 아이디
// @returns IP-PBX 정보 저장 자료구조에 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
bool CSipServerMap::Select( const char *pszIp, const char *pszUserId ) {
    std::string strKey;
    bool bRes = false;
    SIP_SERVER_MAP::iterator itMap;

    GetKey( pszIp, pszUserId, strKey );

    m_clsMutex.acquire();
    itMap = m_clsMap.find( strKey );
    if ( itMap != m_clsMap.end() ) {
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

// IP-PBX 정보 저장 자료구조를 검색하여서 수신자 전화번호에 대한 routing 정보를 가지고 있는 IP-PBX 정보를
// 저장한다.
// @param pszTo						수신자 전화번호
// @param clsCspSipServer IP-PBX 정보 저장 객체
// @param strTo						INVITE 메시지를 전송할 때 사용할 수신자 전화번호
// @returns 검색에 성공하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
bool CSipServerMap::SelectRoutePrefix( const char *pszTo, CspSipServer &clsCspSipServer, std::string &strTo ) {
    SIP_SERVER_MAP::iterator itMap;
    ROUTE_PREFIX_LIST::iterator itList;
    bool bRes = false;

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        if ( itMap->second.m_clsRoutePrefixList.size() > 0 ) {
            for ( itList = itMap->second.m_clsRoutePrefixList.begin();
                  itList != itMap->second.m_clsRoutePrefixList.end(); ++itList ) {
                if ( !strncmp( itList->m_strPrefix.c_str(), pszTo, itList->m_strPrefix.length() ) ) {
                    clsCspSipServer = itMap->second;

                    if ( itList->m_bDeletePrefix ) {
                        strTo = pszTo + itList->m_strPrefix.length();
                    } else {
                        strTo = pszTo;
                    }

                    bRes = true;
                    break;
                }
            }

            if ( bRes ) break;
        }
    }
    m_clsMutex.release();

    return bRes;
}

// IP-PBX 에서 수신한 전화 요청과 매핑되는 CspServer 아이디를 검색한다.
// @parma pszIp	IP-PBX IP 주소
// @param pszTo IP-PBX 에서 전송한 SIP 메시지의 TO 아이디
// @param strTo CspServer 아이디
// @returns 검색에 성공하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
bool CSipServerMap::SelectIncomingRoute( const char *pszIp, const char *pszTo, std::string &strTo ) {
    SIP_SERVER_MAP::iterator itMap;
    INCOMING_ROUTE_LIST::iterator itList;
    bool bRes = false;

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        if ( pszIp ) {
            if ( strcmp( itMap->second.m_strIp.c_str(), pszIp ) ) continue;
        }

        if ( itMap->second.m_clsIncomingRouteList.size() > 0 ) {
            for ( itList = itMap->second.m_clsIncomingRouteList.begin();
                  itList != itMap->second.m_clsIncomingRouteList.end(); ++itList ) {
                if ( !strcmp( itList->m_strToId.c_str(), pszTo ) ) {
                    strTo = itList->m_strDestId;
                    bRes = true;
                    break;
                }
            }

            if ( bRes ) break;
        }
    }
    m_clsMutex.release();

    return bRes;
}

// IP-PBX 정보 저장 자료구조의 key 문자열을 생성한다.
// @param clsCspSipServer IP-PBX 정보 저장 객체
// @param strKey					key 문자열을 저장할 변수
void CSipServerMap::GetKey( CspSipServer &clsCspSipServer, std::string &strKey ) {
    strKey = clsCspSipServer.m_strIp;
    strKey.append( "_" );
    strKey.append( clsCspSipServer.m_strUserId );
}

// IP-PBX 정보 저장 자료구조의 key 문자열을 생성한다.
// @param pszIp			IP-PBX IP 주소
// @param pszUserId 로그인 아이디
// @param strKey		key 문자열을 저장할 변수
void CSipServerMap::GetKey( const char *pszIp, const char *pszUserId, std::string &strKey ) {
    strKey = pszIp;
    strKey.append( "_" );
    strKey.append( pszUserId );
}

// IP-PBX 정보 저장 자료구조에 존재하지 않으면 추가하고
// IP-PBX 정보 저장 자료구조에 존재하면 정보 수정 여부를 검사하여서 수정된 경우 IP-PBX 정보를 수정한다.
// @param clsCspSipServer IP-PBX 정보 저장 객체
// @returns true 를 리턴한다.
bool CSipServerMap::Insert( CspSipServer &clsCspSipServer ) {
    SIP_SERVER_MAP::iterator itMap;
    std::string strKey;

    GetKey( clsCspSipServer, strKey );

    m_clsMutex.acquire();
    itMap = m_clsMap.find( strKey );
    if ( itMap == m_clsMap.end() ) {
        clsCspSipServer.m_iFlag = FLAG_INSERT;
        m_clsMap.insert( SIP_SERVER_MAP::value_type( strKey, clsCspSipServer ) );
    } else {
        if ( strcmp( itMap->second.m_strDomain.c_str(), clsCspSipServer.m_strDomain.c_str() ) ||
             strcmp( itMap->second.m_strPassWord.c_str(), clsCspSipServer.m_strPassWord.c_str() ) ||
             itMap->second.m_iPort != clsCspSipServer.m_iPort ) {
            clsCspSipServer.m_iFlag = FLAG_UPDATE;
        } else {
            itMap->second.m_iFlag = FLAG_NO_CHANGE;
        }

        itMap->second = clsCspSipServer;
    }
    m_clsMutex.release();

    return true;
}

// CSipUserAgent 의 EventRegister 이벤트 정보를 저장한다.
// @param pclsInfo	CSipUserAgent 의 EventRegister 이벤트 정보
// @param iStatus		SIP 상태값
// @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
bool CSipServerMap::Set( CSipServerInfo *pclsInfo, int iStatus ) {
    std::string strKey;
    bool bRes = false;
    SIP_SERVER_MAP::iterator itMap;

    GetKey( pclsInfo->m_strIp.c_str(), pclsInfo->m_strUserId.c_str(), strKey );

    m_clsMutex.acquire();
    itMap = m_clsMap.find( strKey );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.m_bLogin = pclsInfo->m_bLogin;
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

// 자료구조 모니터링을 위한 문자열을 가져온다.
// @param strBuf 자료구조 모니터링을 위한 문자열 저장 변수
void CSipServerMap::GetString( CMonitorString &strBuf ) {
    SIP_SERVER_MAP::iterator itMap;

    strBuf.Clear();

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        strBuf.AddCol( itMap->first );
        strBuf.AddCol( itMap->second.m_strName );
        strBuf.AddCol( itMap->second.m_iFlag );
        strBuf.AddCol( itMap->second.m_strIp, itMap->second.m_iPort );
        strBuf.AddCol( itMap->second.m_strDomain );
        strBuf.AddCol( itMap->second.m_strUserId );
        strBuf.AddCol( itMap->second.m_iLoginTimeout );
        strBuf.AddRow( itMap->second.m_bLogin ? "login" : "logout" );
    }
    m_clsMutex.release();
}
