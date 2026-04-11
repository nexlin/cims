#ifndef _RTP_MAP_H_
#define _RTP_MAP_H_

#include <map>
#include <string>

#include "SipMutex.h"
#include "SipParserDefine.h"
#include "SipUdp.h"
#include "SipUserAgentCallBack.h"

#define SOCKET_COUNT_PER_MEDIA 4

class CRtpInfo {
public:
    CRtpInfo( uint8_t iSocketCount = SOCKET_COUNT_PER_MEDIA );
    
    std::string m_strSessionId;

    Socket *m_phSocket;
    uint32_t *m_piIp;
    IN6_ADDR *m_psttIp;
    uint16_t *m_piPort;

    int m_iStartPort;
    std::string m_strLocalIp;
    bool m_bStop;
    uint8_t m_iSocketCount;

    bool Create();
    void Close();

    void CloseSocket();
    void SetIpPort( int iIndex, uint32_t iIp, uint16_t sPort, int iPeerIdx = 0 );
    void SetIpPort( int iIndex, IN6_ADDR *psttAddr, uint16_t sPort );
    void ReSetIPPort();
    bool Send( int iIndex, char *pszPacket, int iPacketLen );
};

typedef std::map<int, CRtpInfo> RTP_MAP;

/**
 * @ingroup CspServer
 * @brief RTP relay 를 위한 RTP 맵 자료구조 저장 클래스
 */
class CRtpMap {
public:
    CRtpMap();
    ~CRtpMap();

    int CreatePort( int iSocketCount, const std::string& strRecordDir = "", const std::string& strLogDir = "" );

    bool Select( int iPort, CRtpInfo **ppclsRtpInfo );
    bool SetStop( int iPort );
    bool Delete( int iPort );
    bool ReSetIpPort( int iPort );

    void GetString( CMonitorString &strBuf );
    
    // [FIX] Thread-safe wrappers
    bool SetIpPort( int iPort, int iIndex, uint32_t iIp, uint16_t sPort, int iPeerIdx = 0 );
    bool GetLocalIp( int iPort, std::string &strLocalIp );
    bool GetSessionId( int iPort, std::string &strSessionId );

private:
    RTP_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CRtpMap gclsRtpMap;

#endif
