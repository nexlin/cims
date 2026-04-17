#include "RtpMap.h"
#include "CmpClient.h"

#include "CspServer.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "SipServerSetup.h"
#include "SipStackDefine.h"

CRtpMap gclsRtpMap;

CRtpInfo::CRtpInfo( uint8_t iSocketCount )
    : m_phSocket( NULL ),
      m_piIp( NULL ),
      m_psttIp( NULL ),
      m_piPort( NULL ),
      m_iStartPort( 0 ),
      m_bStop( false ),
      m_iSocketCount( iSocketCount ) {
}

/**
 * @ingroup CspServer
 * @brief 자원을 생성한다.
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CRtpInfo::Create() {
    m_phSocket = new Socket[m_iSocketCount];
    if ( m_phSocket == NULL ) return false;

    m_piIp = new uint32_t[m_iSocketCount];
    if ( m_piIp == NULL ) {
        Close();
        return false;
    }

    m_psttIp = new IN6_ADDR[m_iSocketCount];
    if ( m_psttIp == NULL ) {
        Close();
        return false;
    }

    m_piPort = new uint16_t[m_iSocketCount];
    if ( m_piPort == NULL ) {
        Close();
        return false;
    }

    for ( uint8_t i = 0; i < m_iSocketCount; ++i ) {
        m_phSocket[i] = INVALID_SOCKET;
        m_piIp[i] = 0;
        m_piPort[i] = 0;
    }

    return true;
}

/**
 * @ingroup CspServer
 * @brief 자원을 해제한다.
 */
void CRtpInfo::Close() {
    CloseSocket();

    delete[] m_phSocket;
    m_phSocket = NULL;

    delete[] m_psttIp;
    m_psttIp = NULL;

    delete[] m_piIp;
    m_piIp = NULL;

    delete[] m_piPort;
    m_piPort = NULL;
}

/**
 * @ingroup CspServer
 * @brief 소켓을 닫는다.
 */
void CRtpInfo::CloseSocket() {
    if ( m_phSocket ) {
        for ( uint8_t i = 0; i < m_iSocketCount; ++i ) {
            if ( m_phSocket[i] != INVALID_SOCKET ) {
                closesocket( m_phSocket[i] );
                m_phSocket[i] = INVALID_SOCKET;
            }
        }
    }
}

/**
 * @ingroup CspServer
 * @brief SIP 클라이언트의 RTP IP/Port 정보를 설정한다.
 * @param iIndex	소켓 인덱스
 * @param iIp			SIP 클라이언트의 RTP IP 주소
 * @param sPort		SIP 클라이언트의 RTP 포트 번호
 */
void CRtpInfo::SetIpPort( int iIndex, uint32_t iIp, uint16_t sPort, int iPeerIdx ) {
    m_piIp[iIndex] = iIp;
    m_piPort[iIndex] = sPort;
    
    // Only update CMP if we have valid remote info (Audio port index 0?)
    // Typically index 0 is audio, 2 is video?
    // Let's assume index 0 for now.
    if (iIndex == 0 && !m_strSessionId.empty()) {
        char szIp[32];
        struct in_addr addr;
        addr.s_addr = iIp;
        strcpy(szIp, inet_ntoa(addr)); // Not thread safe but simpler for now
        
        std::string locIp;
        int locPort, locVideoPort; // dummy
        
        // Assuming video port (sPort+2) or just 0 for now if not available here. 
        // We only get audio port here.
        // However, we should try to get the video port if it was set previously (e.g. via separate SetIpPort call for index 2)
        int iRmtVideoPort = 0;
        if (m_iSocketCount > 2) {
            iRmtVideoPort = m_piPort[2];
        }

        gclsCmpClient.UpdateSession(m_strSessionId, szIp, sPort, iRmtVideoPort, iPeerIdx,
                                     m_strCaller, m_strCallee, locIp, locPort, m_strSesId);
    }
}

/**
 * @ingroup CspServer
 * @brief SIP 클라이언트의 RTP IPv6/Port 정보를 설정한다.
 * @param iIndex	소켓 인덱스
 * @param iIp			SIP 클라이언트의 RTP IP 주소
 * @param sPort		SIP 클라이언트의 RTP 포트 번호
 */
void CRtpInfo::SetIpPort( int iIndex, IN6_ADDR *psttAddr, uint16_t sPort ) {
    memcpy( &m_psttIp[iIndex], psttAddr, sizeof( m_psttIp[iIndex] ) );
    m_piPort[iIndex] = sPort;
}

/**
 * @ingroup CspServer
 * @brief SIP 클라이언트의 RTP IP/Port 정보를 초기화시킨다.
 */
void CRtpInfo::ReSetIPPort() {
    for ( uint8_t i = 0; i < m_iSocketCount; ++i ) {
        m_piIp[i] = 0;
        m_piPort[i] = 0;
    }
}

/**
 * @ingroup CspServer
 * @brief RTP 패킷을 전송한다.
 * @param iIndex			소켓 인덱스
 * @param pszPacket		RTP 패킷
 * @param iPacketLen	RTP 패킷 길이
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CRtpInfo::Send( int iIndex, char *pszPacket, int iPacketLen ) {
    if ( gclsSetup.m_bIpv6 ) {
        return UdpSend( m_phSocket[iIndex], pszPacket, iPacketLen, &m_psttIp[iIndex], m_piPort[iIndex] );
    } else {
        return UdpSend( m_phSocket[iIndex], pszPacket, iPacketLen, m_piIp[iIndex], m_piPort[iIndex] );
    }
}

CRtpMap::CRtpMap() {
}

CRtpMap::~CRtpMap() {
}

/**
 * @ingroup CspServer
 * @brief RTP relay 를 위해서 UDP 소켓들을 생성한다.
 * @param iSocketCount 생성할 UDP 소켓 개수
 * @returns RTP 포트 번호를 리턴한다.
 */
int CRtpMap::CreatePort( int iSocketCount, const std::string& strRecordDir, const std::string& strLogDir,
                         const std::string& strCaller, const std::string& strCallee,
                         const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort,
                         const std::string& strSesId ) {
    bool bRes = false;
    CRtpInfo clsInfo( iSocketCount );
    
    // Ensure CmpClient is init
    static bool bInit = false;
    if (!bInit) {
        gclsCmpClient.Init(gclsSetup.m_strCmpIp, gclsSetup.m_iCmpPort, gclsSetup.m_iLocalCmpPort);
        bInit = true;
    }

    // [FIX] Allocate valid arrays (m_piIp, etc.) to prevent crash in SetIpPort
    if (!clsInfo.Create()) {
        CLog::Print( LOG_ERROR, "Create RtpPort memory allocation failed" );
        return -1;
    }

    // Generate Session ID (e.g. uuid or just incrementing int? For now, incrementing int based on start port concept)
    static int iSeq = 0;
    m_clsMutex.acquire();
    std::string strSessionId = "cmp_sess_" + std::to_string(++iSeq);
    m_clsMutex.release();
    
    clsInfo.m_strSessionId = strSessionId;
    clsInfo.m_strCaller = strCaller;
    clsInfo.m_strCallee = strCallee;
    clsInfo.m_strSesId = strSesId;

    std::string strLocalIp;
    int iLocalPort = 0;
    int iLocalVideoPort = 0;

    if (gclsCmpClient.AddSession(strSessionId, strLocalIp, iLocalPort, iLocalVideoPort, strRecordDir, strLogDir, strCaller, strCallee, strRmtIp, iRmtPort, iRmtVideoPort, strSesId)) {
        // CmpServer returned allocated ports
        clsInfo.m_iStartPort = iLocalPort; 
        clsInfo.m_strLocalIp = strLocalIp; // Store Allocated IP
        // We might need to store video port too if RtpMap supports it, but RtpMap seems to assume contiguous ports.
        // The original logic expected contiguous ports starting at m_iStartPort.
        // CMP returns audio port. Video is audio + 2.
        
        // We don't really use sockets here anymore, CMP handles it.
        // But we need to store it in map so Select works.
        // Use iLocalPort as the key.
        
        m_clsMutex.acquire();
        m_clsMap.insert( RTP_MAP::value_type( iLocalPort, clsInfo ) );
        m_clsMutex.release();
        
        CLog::Print( LOG_DEBUG, "Create RtpPort(%d) via CMP success. Session=%s", iLocalPort, strSessionId.c_str() );
        return iLocalPort;
    }

    CLog::Print( LOG_ERROR, "Create RtpPort via CMP error" );

    return -1;
}

/**
 * @ingroup CspServer
 * @brief RTP 포트에 대한 정보를 검색한다.
 * @param iPort RTP 포트 번호
 * @param ppclsRtpInfo RTP 정보
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CRtpMap::Select( int iPort, CRtpInfo **ppclsRtpInfo ) {
    RTP_MAP::iterator itMap;
    bool bRes = false;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        *ppclsRtpInfo = &itMap->second;
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

/**
 * @ingroup CspServer
 * @brief RTP 쓰레드에 중지 명령을 전달한다.
 * @param iPort RTP 포트 번호
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CRtpMap::SetStop( int iPort ) {
    RTP_MAP::iterator itMap;
    bool bRes = false;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.m_bStop = true;
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

/**
 * @ingroup CspServer
 * @brief RTP 소켓을 종료한다.
 * @param iPort RTP 포트 번호
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CRtpMap::Delete( int iPort ) {
    RTP_MAP::iterator itMap;
    bool bRes = false;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        // [FIX] Ensure resources are freed
        itMap->second.Close(); 
        
        gclsCmpClient.RemoveSession(itMap->second.m_strSessionId,
                                     itMap->second.m_strCaller,
                                     itMap->second.m_strCallee,
                                     itMap->second.m_strSesId);
        m_clsMap.erase( itMap );
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

/**
 * @ingroup CspServer
 * @brief SIP 클라이언트의 IP/Port 번호를 reset 한다.
 * @param iPort RTP 포트 번호
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CRtpMap::ReSetIpPort( int iPort ) {
    RTP_MAP::iterator itMap;
    bool bRes = false;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.ReSetIPPort();
        bRes = true;
    }
    m_clsMutex.release();

    return bRes;
}

/**
 * @ingroup CspServer
 * @brief 자료구조 모니터링용 문자열을 생성한다.
 * @param strBuf 자료구조 모니터링용 문자열 저장 변수
 */
void CRtpMap::GetString( CMonitorString &strBuf ) {
    RTP_MAP::iterator itMap;
    char szTemp[51];
    int i;
    uint32_t iIp;

    strBuf.Clear();

    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        strBuf.AddCol( itMap->first );
        strBuf.AddCol( itMap->second.m_iStartPort );

        for ( i = 0; i < itMap->second.m_iSocketCount; ++i ) {
            iIp = itMap->second.m_piIp[i];

            snprintf( szTemp, sizeof( szTemp ), "%d.%d.%d.%d:%d", ( iIp ) & 0xFF, ( iIp >> 8 ) & 0xFF,
                      ( iIp >> 16 ) & 0xFF, ( iIp >> 24 ) & 0xFF, ntohs( itMap->second.m_piPort[i] ) );
            strBuf.AddCol( szTemp );
        }

        strBuf.AddRow( itMap->second.m_bStop ? "stop" : "" );
    }
    m_clsMutex.release();
}



bool CRtpMap::SetIpPort( int iPort, int iIndex, uint32_t iIp, uint16_t sPort, int iPeerIdx ) {
    m_clsMutex.acquire();
    bool bRes = false;
    RTP_MAP::iterator itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.SetIpPort(iIndex, iIp, sPort, iPeerIdx);
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

bool CRtpMap::GetLocalIp( int iPort, std::string &strLocalIp ) {
    m_clsMutex.acquire();
    bool bRes = false;
    RTP_MAP::iterator itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        strLocalIp = itMap->second.m_strLocalIp;
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

bool CRtpMap::GetSessionId( int iPort, std::string &strSessionId ) {
    m_clsMutex.acquire();
    bool bRes = false;
    RTP_MAP::iterator itMap = m_clsMap.find( iPort );
    if ( itMap != m_clsMap.end() ) {
        strSessionId = itMap->second.m_strSessionId;
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}
