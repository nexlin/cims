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

#include "SipServerSetup.h"

#include <arpa/inet.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "CspServer.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "SimpleJson.h"
#include "SipStackDefine.h"

CSipServerSetup gclsSetup;

/**
 * @brief XML 에 저장된 element 리스트를 문자열 맵 자료구조에 저장한다.
 * @param pclsElement		리스트를 저장한 XML element
 * @param pszTagName		문자열 리스트 tag 이름
 * @param pszSubTagName 문자열 리스트의 항목 tag 이름
 * @param clsMap				문자열 맵 자료구조
 */
void InsertStringMap( CXmlElement *pclsElement, const char *pszTagName, const char *pszSubTagName,
                      CStringMap &clsMap ) {
    CXmlElement *pclsClient;

    pclsClient = pclsElement->SelectElement( pszTagName );
    if ( pclsClient ) {
        XML_ELEMENT_LIST clsList;
        XML_ELEMENT_LIST::iterator itList;

        if ( pclsClient->SelectElementList( pszSubTagName, clsList ) ) {
            for ( itList = clsList.begin(); itList != clsList.end(); ++itList ) {
                if ( itList->IsDataEmpty() ) continue;

                clsMap.Insert( itList->GetData(), "" );
            }
        }
    }
}

/**
 * @ingroup CspServer
 * @brief 생성자
 */
CSipServerSetup::CSipServerSetup()
    : m_iUdpPort( 0 ),  // local_nodes.jsonl 의 primary UDP record 가 set. 0=fail-fast.
      m_iUdpThreadCount( 10 ),
      m_iTcpPort( 0 ),  // local_nodes.jsonl 의 primary TCP record 가 set. 0=disabled.
      m_iTcpThreadCount( 10 ),
      m_iTcpCallBackThreadCount( 0 ),
      m_iTcpRecvTimeout( SIP_TCP_RECV_TIMEOUT ),
      m_iTlsPort( 0 ),  // local_nodes.jsonl 의 primary TLS record 가 set. 0=disabled.
      m_iTlsAcceptTimeout( SIP_TLS_ACCEPT_TIMEOUT ),
      m_iStackExecutePeriod( 20 ),
      m_iTimerD( 32000 ),
      m_iTimerJ( 32000 ),
      m_bIpv6( false ),
      m_iMinRegisterTimeout( 300 ),
      m_bUseRtpRelay( false ),
      m_iSendOptionsPeriod( 0 ),
      m_bUseRegisterSession( false ),
      m_iUserTimeout( 3600 ),
      m_iStaleCallTimeout( 300 ),
      m_iDispatchMaxTapsPerSession( 2 ),
      m_iDispatchMaxForkTargets( 32 ),
      m_iDispatchForkRingTimeoutSec( 60 ),
      m_bSessionTimer( true ),  // 비정상 종료 leg 감지 (RFC 4028) — 기본 활성
      m_iSessionExpires( 180 ),
      m_iSessionMinSE( 90 ),
      m_strSessionRefresher( "server" ),
      m_iDbPort( 3306 ),
      m_iRedisPort( 0 ),
      m_strServiceMode( "both" ),
      m_bTestEnvOpenTermination( false ),  // ⚠️ 상용 기본 false — 테스트망에서만 true
      m_bPttAdhocEnabled( true ),          // ad hoc 그룹콜 허용 (기존 동작 보존 — 끄면 합성 거부)
      m_strServiceLogSpoolDir( "spool" ),
      m_iServiceLogStallSec( 5 ),
      m_iServiceLogSpoolMaxMb( 1024 ),
      m_iLogLevel( 0 ),
      m_iLogMaxSize( 20000000 ),
      m_iMonitorPort( 6000 ),
      m_strCmpIp( "127.0.0.1" ),
      m_iCmpPort( 9000 ),
      m_iLocalCmpPort( 9001 ),
      m_bUseMcDataMedia( false ),
      m_strCmdpIp( "127.0.0.1" ),
      m_iCmdpPort( 9100 ),
      m_iLocalCmdpPort( 9101 ),
      m_bFmEnable( false ),
      m_strFmOamIp( "127.0.0.1" ),
      m_iFmOamPort( 9010 ),
      m_iFmSyncSec( 60 ),
      m_iSipStatsEvalSec( 60 ),
      m_iSipStatsMinFinals( 20 ),
      m_iSipStatsCallRateMinor( 90 ),
      m_iSipStatsCallRateMajor( 70 ),
      m_iSipStatsCallRateCritical( 50 ),
      m_iSipStatsRegRateMinor( 90 ),
      m_iSipStatsRegRateMajor( 70 ),
      m_iSipStatsRegRateCritical( 50 ),
      m_iSipStatsCpsMinor( 0 ),
      m_iSipStatsCpsMajor( 0 ),
      m_iSipStatsCpsCritical( 0 ),
      m_iSipStatsRxErrorMinor( 10 ),
      m_iSipStatsChannelPolicyMajor( 10 ),
      m_iSipStatsSecAgreeRejectMajor( 10 ),
      m_bSecAgreeRequire( false ),
      m_iIpsecSpiMin( 0x10000000 ),
      m_iIpsecSpiMax( 0x1FFFFFFF ),
      m_iIpsecReqIdBase( 0x43490000 ),
      m_iIpsecTempSaTimeoutSec( 32 ),
      m_strIpsecEalgPreference( "aes-cbc" ),
      m_iMaxSdsCplaneBytes( 0 ),
      m_strCscHost( "" ),
      m_iCscPort( 4421 ),
      m_strCscScheme( "https" ),
      m_strCscInternalToken( "" ),
      m_iCscTimeoutMs( 2000 ),
      m_bDeprecatedXcapSeen( false ),
      m_bRoleCscf( true ),
      m_bRoleTas( true ),
      m_bRolePttAs( true ),
      m_bRoleIbcf( true ),
      m_bRoleMcData( true ),
      m_bRecordEnable( false ),
      m_strRecordDir( "/mnt/nas/cims/recordings" ),
      m_iFileSize( 0 ) {
}

/**
 * @ingroup CspServer
 * @brief 소멸자
 */
CSipServerSetup::~CSipServerSetup() {
}

/**
 * @ingroup CspServer
 * @brief 설정 파일을 읽어서 멤버 변수에 저장한다.
 * @param pszFileName 설정 파일 full path
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
// config.json overlay: flat 키 ("Setup.Sip.AuthRealm": "csp") 를 root 의 중첩 경로에 set.
// 같은 경로가 이미 있으면 덮어씀. 템플릿 렌더링 결과가 이 형태.
static void _setByDotPath( SimpleJson::JsonNode &parent, const std::string &dotPath,
                           const SimpleJson::JsonNode &value ) {
    size_t pos = dotPath.find( '.' );
    if ( pos == std::string::npos ) {
        parent.Set( dotPath, value );
        return;
    }
    std::string head = dotPath.substr( 0, pos );
    std::string rest = dotPath.substr( pos + 1 );
    SimpleJson::JsonNode sub = parent.Has( head ) ? parent.Get( head ) : SimpleJson::JsonNode();
    if ( sub.type != SimpleJson::JSON_OBJECT ) {
        sub = SimpleJson::JsonNode();
        sub.type = SimpleJson::JSON_OBJECT;
    }
    _setByDotPath( sub, rest, value );
    parent.Set( head, sub );
}

// install_path 기준으로 overlay 파일 경로 탐색. 시도 순서:
//   1) CIMS_DEPLOYMENT_CONFIG 환경변수
//   2) <csp.json 디렉토리>/../../config.json     (install_path/config.json, 배포 배치)
//   3) (없음) — overlay 생략
static std::string _findDeploymentConfig( const std::string &cspJsonPath ) {
    if ( const char *env = getenv( "CIMS_DEPLOYMENT_CONFIG" ) ) {
        if ( *env ) {
            std::ifstream f( env );
            if ( f ) return env;
        }
    }
    // csp.json 경로에서 ../../config.json 유도
    std::string dir = cspJsonPath;
    size_t s = dir.find_last_of( '/' );
    if ( s != std::string::npos ) dir = dir.substr( 0, s );
    // dir = install_path/csp/config  →  ../.. = install_path
    std::string cand = dir + "/../../config.json";
    std::ifstream f( cand );
    if ( f ) {
        // 정규화는 하지 않음 (원본 상대 경로 보존)
        return cand;
    }
    return "";
}

void CSipServerSetup::WarnDeprecatedKeys() {
    // Read() 시점에는 로그가 아직 열려 있지 않다 — 로그 초기화 이후 이 함수가 보고한다.
    if ( m_bDeprecatedXcapSeen ) {
        CLog::Print( LOG_ERROR,
                     "Setup.Xcap.* 는 폐기된 설정입니다 — 무시합니다. 단말용 XCAP 주소의 정본은 "
                     "CSC 의 McpttServer.PublicUrl 이며 CSP 는 내부 API 로 취득합니다"
                     "(/internal/mcptt/endpoint). csp.json 에서 Setup.Xcap 블록을 제거하십시오." );
    }
}

bool CSipServerSetup::Read( const char *pszFileName ) {
    std::string strFileName = pszFileName;
    if ( strFileName.substr( strFileName.find_last_of( "." ) + 1 ) == "json" ) {
        std::ifstream t( pszFileName );
        if ( !t.is_open() ) return false;
        std::stringstream buffer;
        buffer << t.rdbuf();

        SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( buffer.str() );
        if ( root.type != SimpleJson::JSON_OBJECT ) return false;

        // Deployment overlay: install_path/config.json 을 flat key → nested 로 merge.
        // 주의: Read() 는 CLog 초기화 전에 호출되므로 여기선 로그 없이 적용만 하고,
        //       결과는 m_strOverlayPath / m_iOverlayKeys 에 기록해 CspServer 가 나중에 출력.
        std::string overlayPath = _findDeploymentConfig( pszFileName );
        if ( !overlayPath.empty() ) {
            std::ifstream of( overlayPath );
            std::stringstream ob;
            ob << of.rdbuf();
            SimpleJson::JsonNode over = SimpleJson::JsonNode::Parse( ob.str() );
            if ( over.type == SimpleJson::JSON_OBJECT ) {
                int applied = 0;
                for ( const auto &kv : over.objects ) {
                    _setByDotPath( root, kv.first, kv.second );
                    ++applied;
                }
                m_strOverlayPath = overlayPath;
                m_iOverlayKeys = applied;
            }
        }

        if ( root.Has( "Setup" ) ) {
            SimpleJson::JsonNode setup = root.Get( "Setup" );

            if ( setup.Has( "Sip" ) ) {
                SimpleJson::JsonNode sip = setup.Get( "Sip" );
                // SIP bind (LocalIp/UdpPort/TcpPort/TlsPort/CertFile) 는 local_nodes.jsonl 가 SoT.
                // 옛 csp.json 에 남아있으면 stale entry 로 무시 (Has 미평가).
                if ( sip.Has( "UdpThreadCount" ) ) m_iUdpThreadCount = (int)sip.GetInt( "UdpThreadCount" );
                if ( sip.Has( "TcpThreadCount" ) ) m_iTcpThreadCount = (int)sip.GetInt( "TcpThreadCount" );
                if ( sip.Has( "TcpRecvTimeout" ) ) m_iTcpRecvTimeout = (int)sip.GetInt( "TcpRecvTimeout" );
                if ( sip.Has( "TlsAcceptTimeout" ) ) m_iTlsAcceptTimeout = (int)sip.GetInt( "TlsAcceptTimeout" );
                if ( sip.Has( "MinRegisterTimeout" ) ) m_iMinRegisterTimeout = (int)sip.GetInt( "MinRegisterTimeout" );
                if ( sip.Has( "CallPickupId" ) ) m_strCallPickupId = sip.GetString( "CallPickupId" );
                if ( sip.Has( "StackExecutePeriod" ) ) m_iStackExecutePeriod = (int)sip.GetInt( "StackExecutePeriod" );
                if ( sip.Has( "UserTimeout" ) ) m_iUserTimeout = (int)sip.GetInt( "UserTimeout" );
                if ( sip.Has( "StaleCallTimeout" ) ) m_iStaleCallTimeout = (int)sip.GetInt( "StaleCallTimeout" );
                if ( sip.Has( "SendOptionsPeriod" ) ) m_iSendOptionsPeriod = (int)sip.GetInt( "SendOptionsPeriod" );

                // 세션 타이머 (RFC 4028) — docs/design/features/leg_liveness.md
                if ( sip.Has( "SessionTimer" ) ) {
                    SimpleJson::JsonNode st = sip.Get( "SessionTimer" );
                    if ( st.Has( "Enable" ) ) m_bSessionTimer = ( st.Get( "Enable" ).AsString() == "true" );
                    if ( st.Has( "SessionExpires" ) ) m_iSessionExpires = (int)st.GetInt( "SessionExpires" );
                    if ( st.Has( "MinSE" ) ) m_iSessionMinSE = (int)st.GetInt( "MinSE" );
                    if ( st.Has( "Refresher" ) ) m_strSessionRefresher = st.GetString( "Refresher" );
                }

                // 관제 센터 — 대표번호 병렬 호출·감청 leg 상한 (dispatch_center.md §8.3)
                if ( sip.Has( "Dispatch" ) ) {
                    SimpleJson::JsonNode dp = sip.Get( "Dispatch" );
                    if ( dp.Has( "MaxTapsPerSession" ) )
                        m_iDispatchMaxTapsPerSession = (int)dp.GetInt( "MaxTapsPerSession" );
                    if ( dp.Has( "MaxForkTargets" ) ) m_iDispatchMaxForkTargets = (int)dp.GetInt( "MaxForkTargets" );
                    if ( dp.Has( "ForkRingTimeoutSec" ) )
                        m_iDispatchForkRingTimeoutSec = (int)dp.GetInt( "ForkRingTimeoutSec" );
                    if ( m_iDispatchMaxTapsPerSession < 0 ) m_iDispatchMaxTapsPerSession = 0;
                    if ( m_iDispatchMaxForkTargets < 1 ) m_iDispatchMaxForkTargets = 1;
                    if ( m_iDispatchForkRingTimeoutSec < 5 ) m_iDispatchForkRingTimeoutSec = 5;
                }
            }

            // 미디어서버 연동 설정 (2026-04-23 rename: RtpRelay → MediaServer).
            //   신규 key 를 우선 파싱, 기존 RtpRelay 는 배포된 csp.json 호환을 위해 fallback.
            if ( setup.Has( "MediaServer" ) ) {
                SimpleJson::JsonNode ms = setup.Get( "MediaServer" );
                if ( ms.Has( "Enable" ) ) m_bUseRtpRelay = ( ms.Get( "Enable" ).AsString() == "true" );
                if ( ms.Has( "LocalPort" ) ) m_iLocalCmpPort = (int)ms.GetInt( "LocalPort" );
                // LocalIp 는 현재 C++ 바인딩 없음 (렌더링 용도). 추후 CmpClient bind 확장 시 연결.
                // 레거시 단일 Host/ControlPort — 구 배포 csp.json 호환용. Endpoints 가 비었을 때만
                //   primary 로 쓰인다 (아래 Endpoints[0] override 참조).
                if ( ms.Has( "Host" ) ) m_strCmpIp = ms.GetString( "Host" );
                if ( ms.Has( "ControlPort" ) ) m_iCmpPort = (int)ms.GetInt( "ControlPort" );
                // 미디어(CMP) 엔드포인트 풀 — 통합 리스트. MediaServer.Endpoints = [{"ip":..,"port":..}, ..].
                //   첫 행이 primary, 2개 이상이면 CmpClient consistent-hash ring 으로 Session-ID 를
                //   다중 CMP(All-Active) 에 분배한다. 비우면 위 Host/ControlPort 단일 운영(하위호환).
                if ( ms.Has( "Endpoints" ) ) {
                    SimpleJson::JsonNode eps = ms.Get( "Endpoints" );
                    if ( eps.type == SimpleJson::JSON_ARRAY ) {
                        for ( size_t i = 0; i < eps.Size(); ++i ) {
                            SimpleJson::JsonNode ep = eps.At( i );
                            std::string ip = ep.GetString( "ip" );
                            int port = (int)ep.GetInt( "port", 9000 );
                            if ( !ip.empty() && port > 0 ) m_vecCmpEndpoints.push_back( std::make_pair( ip, port ) );
                        }
                    }
                }
                // Endpoints[0] = primary (레거시 Host 보다 우선). CmpClient::Init 가 이 primary 를
                //   ring 에 등록하고, CspServer 가 나머지 endpoint 를 AddEndpoint 로 추가한다.
                if ( !m_vecCmpEndpoints.empty() ) {
                    m_strCmpIp = m_vecCmpEndpoints.front().first;
                    m_iCmpPort = m_vecCmpEndpoints.front().second;
                }
                // 세션 재조정(audit 수준2) — MediaServer.Audit.*
                if ( ms.Has( "Audit" ) ) {
                    SimpleJson::JsonNode au = ms.Get( "Audit" );
                    if ( au.Has( "Enable" ) ) m_bAuditEnable = ( au.Get( "Enable" ).AsString() == "true" );
                    if ( au.Has( "GraceSec" ) ) m_iAuditGraceSec = (int)au.GetInt( "GraceSec" );
                    if ( au.Has( "MaxPerCycle" ) ) m_iAuditMaxPerCycle = (int)au.GetInt( "MaxPerCycle" );
                    if ( au.Has( "ZombieTeardown" ) )
                        m_bAuditZombieTeardown = ( au.Get( "ZombieTeardown" ).AsString() == "true" );
                    if ( au.Has( "HaRole" ) ) m_strHaRole = au.GetString( "HaRole" );
                    if ( au.Has( "HaVip" ) ) m_strHaVip = au.GetString( "HaVip" );
                }
            } else if ( setup.Has( "RtpRelay" ) ) {
                SimpleJson::JsonNode rtp = setup.Get( "RtpRelay" );
                if ( rtp.Has( "UseRtpRelay" ) ) m_bUseRtpRelay = ( rtp.Get( "UseRtpRelay" ).AsString() == "true" );
                if ( rtp.Has( "CmpIp" ) ) m_strCmpIp = rtp.GetString( "CmpIp" );
                if ( rtp.Has( "CmpPort" ) ) m_iCmpPort = (int)rtp.GetInt( "CmpPort" );
                if ( rtp.Has( "LocalCmpPort" ) ) m_iLocalCmpPort = (int)rtp.GetInt( "LocalCmpPort" );
            }

            // SDP 미디어 코덱 테이블 — Setup.Media.Codecs (배열 순서 = 우선순위).
            //   psip CSipCodecTable 로 주입되어 오퍼/answer 의 코덱·PT·fmtp 를 결정한다.
            //   비면 psip 기본 테이블 사용 (AMR-WB 96 최우선 — 실단말 pjsua 정렬값).
            if ( setup.Has( "Media" ) ) {
                SimpleJson::JsonNode media = setup.Get( "Media" );
                if ( media.Has( "Codecs" ) ) {
                    SimpleJson::JsonNode codecs = media.Get( "Codecs" );
                    if ( codecs.type == SimpleJson::JSON_ARRAY ) {
                        for ( size_t i = 0; i < codecs.Size(); ++i ) {
                            SimpleJson::JsonNode c = codecs.At( i );
                            CspMediaCodec clsCodec;
                            clsCodec.strName = c.GetString( "Name" );
                            clsCodec.iPt = (int)c.GetInt( "Pt", -1 );
                            clsCodec.iClock = (int)c.GetInt( "Clock", 8000 );
                            clsCodec.iChannels = (int)c.GetInt( "Channels", 0 );
                            clsCodec.strFmtp = c.GetString( "Fmtp" );
                            clsCodec.iPtime = (int)c.GetInt( "Ptime", 0 );
                            if ( !clsCodec.strName.empty() && clsCodec.iPt >= 0 && clsCodec.iPt <= 127 )
                                m_vecMediaCodecs.push_back( clsCodec );
                        }
                    }
                }
            }

            // MCData media plane(cmdp, MSRP) 연동 — 기본 비활성 (cmdp 미배치 환경 무영향)
            if ( setup.Has( "McDataMedia" ) ) {
                SimpleJson::JsonNode md = setup.Get( "McDataMedia" );
                if ( md.Has( "Enable" ) ) m_bUseMcDataMedia = ( md.Get( "Enable" ).AsString() == "true" );
                if ( md.Has( "Host" ) ) m_strCmdpIp = md.GetString( "Host" );
                if ( md.Has( "ControlPort" ) ) m_iCmdpPort = (int)md.GetInt( "ControlPort" );
                if ( md.Has( "LocalPort" ) ) m_iLocalCmdpPort = (int)md.GetInt( "LocalPort" );
            }

            // FM 자기보고 (모듈 알람/이벤트 push — docs/design/alarm_self_reporting.md)
            if ( setup.Has( "Fm" ) ) {
                SimpleJson::JsonNode fm = setup.Get( "Fm" );
                if ( fm.Has( "Enable" ) ) m_bFmEnable = ( fm.Get( "Enable" ).AsString() == "true" );
                if ( fm.Has( "OamIp" ) ) m_strFmOamIp = fm.GetString( "OamIp" );
                if ( fm.Has( "OamPort" ) ) m_iFmOamPort = (int)fm.GetInt( "OamPort" );
                if ( fm.Has( "SyncSec" ) ) m_iFmSyncSec = (int)fm.GetInt( "SyncSec" );
            }

            // SIP 신호 통계/품질 알람 (SipStatsMonitor — A-QOS-006/007/009/011·A-SEC-003)
            if ( setup.Has( "SipStats" ) ) {
                SimpleJson::JsonNode st = setup.Get( "SipStats" );
                if ( st.Has( "EvalSec" ) ) m_iSipStatsEvalSec = (int)st.GetInt( "EvalSec" );
                if ( st.Has( "MinFinals" ) ) m_iSipStatsMinFinals = (int)st.GetInt( "MinFinals" );
                if ( st.Has( "CallRateMinor" ) ) m_iSipStatsCallRateMinor = (int)st.GetInt( "CallRateMinor" );
                if ( st.Has( "CallRateMajor" ) ) m_iSipStatsCallRateMajor = (int)st.GetInt( "CallRateMajor" );
                if ( st.Has( "CallRateCritical" ) ) m_iSipStatsCallRateCritical = (int)st.GetInt( "CallRateCritical" );
                if ( st.Has( "RegRateMinor" ) ) m_iSipStatsRegRateMinor = (int)st.GetInt( "RegRateMinor" );
                if ( st.Has( "RegRateMajor" ) ) m_iSipStatsRegRateMajor = (int)st.GetInt( "RegRateMajor" );
                if ( st.Has( "RegRateCritical" ) ) m_iSipStatsRegRateCritical = (int)st.GetInt( "RegRateCritical" );
                if ( st.Has( "CpsMinor" ) ) m_iSipStatsCpsMinor = (int)st.GetInt( "CpsMinor" );
                if ( st.Has( "CpsMajor" ) ) m_iSipStatsCpsMajor = (int)st.GetInt( "CpsMajor" );
                if ( st.Has( "CpsCritical" ) ) m_iSipStatsCpsCritical = (int)st.GetInt( "CpsCritical" );
                if ( st.Has( "RxErrorMinor" ) ) m_iSipStatsRxErrorMinor = (int)st.GetInt( "RxErrorMinor" );
                if ( st.Has( "ChannelPolicyMajor" ) )
                    m_iSipStatsChannelPolicyMajor = (int)st.GetInt( "ChannelPolicyMajor" );
                if ( st.Has( "SecAgreeRejectMajor" ) )
                    m_iSipStatsSecAgreeRejectMajor = (int)st.GetInt( "SecAgreeRejectMajor" );
            }

            // RFC 3329 sec-agree (P2)
            if ( setup.Has( "SecAgree" ) ) {
                SimpleJson::JsonNode sa = setup.Get( "SecAgree" );
                if ( sa.Has( "Require" ) ) m_bSecAgreeRequire = ( sa.GetString( "Require" ) == "true" );
            }

            // IMS AKA + IPsec (P4) — 16진/10진 모두 허용
            if ( setup.Has( "Ipsec" ) ) {
                SimpleJson::JsonNode ip = setup.Get( "Ipsec" );
                auto u32 = []( const std::string &v, uint32_t d ) -> uint32_t {
                    if ( v.empty() ) return d;
                    return (uint32_t)strtoul( v.c_str(), NULL, 0 );
                };
                if ( ip.Has( "SpiMin" ) ) m_iIpsecSpiMin = u32( ip.GetString( "SpiMin" ), m_iIpsecSpiMin );
                if ( ip.Has( "SpiMax" ) ) m_iIpsecSpiMax = u32( ip.GetString( "SpiMax" ), m_iIpsecSpiMax );
                if ( ip.Has( "ReqIdBase" ) ) m_iIpsecReqIdBase = u32( ip.GetString( "ReqIdBase" ), m_iIpsecReqIdBase );
                if ( ip.Has( "TempSaTimeoutSec" ) ) m_iIpsecTempSaTimeoutSec = (int)ip.GetInt( "TempSaTimeoutSec" );
                if ( ip.Has( "EalgPreference" ) ) m_strIpsecEalgPreference = ip.GetString( "EalgPreference" );
                if ( m_iIpsecTempSaTimeoutSec < 5 ) m_iIpsecTempSaTimeoutSec = 5;
                if ( m_strIpsecEalgPreference != "null" ) m_strIpsecEalgPreference = "aes-cbc";
            }

            // MCData C-plane 정책 (TS 24.484 <max-payload-size-sds-cplane-bytes> 대응)
            if ( setup.Has( "McData" ) ) {
                SimpleJson::JsonNode mc = setup.Get( "McData" );
                if ( mc.Has( "MaxPayloadSizeSdsCplaneBytes" ) )
                    m_iMaxSdsCplaneBytes = (int)mc.GetInt( "MaxPayloadSizeSdsCplaneBytes" );
                if ( mc.Has( "FdUrlBase" ) ) m_strFdUrlBase = mc.GetString( "FdUrlBase" );
            }

            // 구 Setup.Xcap.* — 폐기됨. 단말용 XCAP root 의 정본은 CSC(McpttServer.PublicUrl)
            //   이고 CSP 는 내부 API 로 취득한다(CscEndpointCache). overlay 에 잔존해도 무시.
            //   여기서 CLog::Print 를 해도 로그가 아직 열리지 않아 유실된다 — 플래그만 세우고
            //   WarnDeprecatedKeys() 가 로그 초기화 후 출력한다.
            m_bDeprecatedXcapSeen = setup.Has( "Xcap" );

            // CSC 내부 API (IMS AKA AV + 단말용 MCPTT 서비스 주소)
            if ( setup.Has( "Csc" ) ) {
                SimpleJson::JsonNode csc = setup.Get( "Csc" );
                if ( csc.Has( "Host" ) ) m_strCscHost = csc.GetString( "Host" );
                if ( csc.Has( "Port" ) ) m_iCscPort = (int)csc.GetInt( "Port" );
                if ( csc.Has( "Scheme" ) ) m_strCscScheme = csc.GetString( "Scheme" );
                if ( csc.Has( "InternalToken" ) ) m_strCscInternalToken = csc.GetString( "InternalToken" );
                if ( csc.Has( "TimeoutMs" ) ) m_iCscTimeoutMs = (int)csc.GetInt( "TimeoutMs" );
            }

            if ( setup.Has( "ConfigJsonlDir" ) ) m_strConfigJsonlDir = setup.GetString( "ConfigJsonlDir" );

            // ConfigJsonlDir fallback: 설정값이 비어있거나 존재하지 않는 경로면
            // install_path/config 로 자동 추정. install_path 는 csp.json 의 부모×3.
            // (cspJsonPath = install_path/csp/config/csp.json, 상대/절대 모두 지원)
            {
                struct stat st;
                bool exists = !m_strConfigJsonlDir.empty() && stat( m_strConfigJsonlDir.c_str(), &st ) == 0 &&
                              S_ISDIR( st.st_mode );
                if ( !exists ) {
                    char abs[PATH_MAX] = { 0 };
                    if ( realpath( pszFileName, abs ) != nullptr ) {
                        std::string p = abs;
                        for ( int i = 0; i < 3; ++i ) {
                            size_t s = p.find_last_of( '/' );
                            if ( s == std::string::npos ) {
                                p.clear();
                                break;
                            }
                            p.erase( s );
                        }
                        if ( !p.empty() ) {
                            std::string cand = p + "/config";
                            if ( stat( cand.c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) ) {
                                m_strConfigJsonlDir = cand;
                            }
                        }
                    }
                }
            }

            if ( setup.Has( "Log" ) ) {
                SimpleJson::JsonNode log = setup.Get( "Log" );
                if ( log.Has( "Folder" ) ) m_strLogFolder = log.GetString( "Folder" );
                if ( log.Has( "MaxSize" ) ) m_iLogMaxSize = (int)log.GetInt( "MaxSize" );
                if ( log.Has( "Level" ) ) {
                    SimpleJson::JsonNode level = log.Get( "Level" );
                    m_iLogLevel = 0;
                    if ( level.Has( "Debug" ) && level.Get( "Debug" ).AsString() == "true" ) m_iLogLevel |= LOG_DEBUG;
                    if ( level.Has( "Info" ) && level.Get( "Info" ).AsString() == "true" ) m_iLogLevel |= LOG_INFO;
                    if ( level.Has( "Network" ) && level.Get( "Network" ).AsString() == "true" )
                        m_iLogLevel |= LOG_NETWORK;
                }
                CLog::SetLevel( m_iLogLevel );
                CLog::SetMaxLogSize( m_iLogMaxSize );
            }

            if ( setup.Has( "DataFolder" ) ) {
                SimpleJson::JsonNode dataDir = setup.Get( "DataFolder" );
                if ( dataDir.Has( "User" ) ) m_strUserDataFolder = dataDir.GetString( "User" );
                if ( dataDir.Has( "Group" ) ) m_strGroupDataFolder = dataDir.GetString( "Group" );
                if ( dataDir.Has( "DispatchGroup" ) )
                    m_strDispatchGroupDataFolder = dataDir.GetString( "DispatchGroup" );
                // G10 (2026-04-23): DataFolder.SipServer 제거 — SipServerMap legacy 제거와 동반.
            }

            if ( setup.Has( "Database" ) ) {
                SimpleJson::JsonNode db = setup.Get( "Database" );
                if ( db.Has( "Host" ) ) m_strDbHost = db.GetString( "Host" );
                if ( db.Has( "Port" ) ) m_iDbPort = (int)db.GetInt( "Port" );
                if ( db.Has( "User" ) ) m_strDbUser = db.GetString( "User" );
                if ( db.Has( "Password" ) ) m_strDbPasswd = db.GetString( "Password" );
                if ( db.Has( "DbName" ) ) m_strDbName = db.GetString( "DbName" );
            }

            // Phase 1.D-2 — Redis (register state hot replication, optional)
            if ( setup.Has( "Redis" ) ) {
                SimpleJson::JsonNode rd = setup.Get( "Redis" );
                if ( rd.Has( "Host" ) ) m_strRedisHost = rd.GetString( "Host" );
                if ( rd.Has( "Port" ) ) m_iRedisPort = (int)rd.GetInt( "Port" );
                if ( rd.Has( "Password" ) ) m_strRedisPassword = rd.GetString( "Password" );
            }

            if ( setup.Has( "ServiceMode" ) ) {
                m_strServiceMode = setup.GetString( "ServiceMode" );
            }

            // ⚠️ 테스트 환경 전용 개방형 착신 스위치 — 상용은 미지정(false) 유지.
            if ( setup.Has( "TestEnvOpenTermination" ) ) {
                m_bTestEnvOpenTermination = ( setup.GetString( "TestEnvOpenTermination" ) == "true" );
            }

            // MCPTT ad hoc 그룹콜 시스템 정책 (미지정 시 허용).
            if ( setup.Has( "PttAdhocEnabled" ) ) {
                m_bPttAdhocEnabled = ( setup.GetString( "PttAdhocEnabled" ) != "false" );
            }

            // IMS 역할 설정 (미지정 시 모두 활성화)
            if ( setup.Has( "Roles" ) ) {
                SimpleJson::JsonNode roles = setup.Get( "Roles" );
                if ( roles.Has( "CSCF" ) ) m_bRoleCscf = ( roles.GetString( "CSCF" ) == "true" );
                if ( roles.Has( "TAS" ) ) m_bRoleTas = ( roles.GetString( "TAS" ) == "true" );
                if ( roles.Has( "PTT_AS" ) ) m_bRolePttAs = ( roles.GetString( "PTT_AS" ) == "true" );
                if ( roles.Has( "IBCF" ) ) m_bRoleIbcf = ( roles.GetString( "IBCF" ) == "true" );
                if ( roles.Has( "MCDATA" ) ) m_bRoleMcData = ( roles.GetString( "MCDATA" ) == "true" );
            }

            // 녹취 설정
            if ( setup.Has( "Recording" ) ) {
                SimpleJson::JsonNode rec = setup.Get( "Recording" );
                if ( rec.Has( "Enable" ) ) m_bRecordEnable = ( rec.GetString( "Enable" ) == "true" );
                if ( rec.Has( "Dir" ) ) m_strRecordDir = rec.GetString( "Dir" );
            }

            // G10+ (2026-04-23): Setup.Cdr.Folder 제거. service_log 의 call.json / participants.jsonl
            //   + DB call_logs 테이블이 CDR 역할을 대체.

            // ServiceLogging 설정 (신규 — Dir 통합)
            if ( setup.Has( "ServiceLogging" ) ) {
                SimpleJson::JsonNode sl = setup.Get( "ServiceLogging" );
                if ( sl.Has( "Dir" ) ) {
                    m_strServiceLogDir = sl.GetString( "Dir" );
                    m_strMsgLogDir = m_strServiceLogDir;  // 통합 디렉토리
                }
                if ( sl.Has( "Recording" ) ) {
                    std::string rv = sl.GetString( "Recording" );
                    m_bRecordEnable = ( rv == "true" || rv == "1" );
                    // record_dir = ServiceLogDir (통합)
                    if ( m_bRecordEnable && m_strRecordDir.empty() ) m_strRecordDir = m_strServiceLogDir;
                }
                // 스풀 폴백 (Dir 가 NAS 일 때 무응답 격리 — SipMessageLogger 참조)
                if ( sl.Has( "SpoolDir" ) ) m_strServiceLogSpoolDir = sl.GetString( "SpoolDir" );
                if ( sl.Has( "StallSec" ) ) m_iServiceLogStallSec = (int)sl.GetInt( "StallSec" );
                if ( sl.Has( "SpoolMaxMb" ) ) m_iServiceLogSpoolMaxMb = (int)sl.GetInt( "SpoolMaxMb" );
            }
            // 레거시 호환
            if ( m_strServiceLogDir.empty() && setup.Has( "ServiceLog" ) ) {
                SimpleJson::JsonNode svclog = setup.Get( "ServiceLog" );
                if ( svclog.Has( "Dir" ) ) m_strServiceLogDir = svclog.GetString( "Dir" );
            }
            if ( m_strMsgLogDir.empty() && setup.Has( "MsgLog" ) ) {
                SimpleJson::JsonNode msglog = setup.Get( "MsgLog" );
                if ( msglog.Has( "Dir" ) ) m_strMsgLogDir = msglog.GetString( "Dir" );
            }
            if ( m_strMsgLogDir.empty() ) m_strMsgLogDir = m_strServiceLogDir;

            if ( setup.Has( "SystemId" ) ) {
                m_strSystemId = setup.GetString( "SystemId" );
            }
            if ( m_strSystemId.empty() ) {
                m_strSystemId = "csp_01";
            }

            // Monitor
            m_clsMonitorIpMap.DeleteAll();
            if ( setup.Has( "Monitor" ) ) {
                SimpleJson::JsonNode mon = setup.Get( "Monitor" );
                if ( mon.Has( "Port" ) ) m_iMonitorPort = (int)mon.GetInt( "Port" );
                if ( mon.Has( "ClientIpList" ) ) {
                    SimpleJson::JsonNode list = mon.Get( "ClientIpList" );
                    if ( list.type == SimpleJson::JSON_ARRAY ) {
                        for ( size_t i = 0; i < list.Size(); ++i ) {
                            m_clsMonitorIpMap.Insert( list.At( i ).AsString().c_str(), "" );
                        }
                    }
                }
            }

            // Security
            m_clsDenySipUserAgentMap.DeleteAll();
            m_clsAllowSipUserAgentMap.DeleteAll();
            m_clsAllowClientIpMap.DeleteAll();

            if ( setup.Has( "Security" ) ) {
                SimpleJson::JsonNode sec = setup.Get( "Security" );

                if ( sec.Has( "DenySipUserAgentList" ) ) {
                    SimpleJson::JsonNode list = sec.Get( "DenySipUserAgentList" );
                    if ( list.type == SimpleJson::JSON_ARRAY ) {
                        for ( size_t i = 0; i < list.Size(); ++i ) {
                            m_clsDenySipUserAgentMap.Insert( list.At( i ).AsString().c_str(), "" );
                        }
                    }
                }

                if ( sec.Has( "AllowSipUserAgentList" ) ) {
                    SimpleJson::JsonNode list = sec.Get( "AllowSipUserAgentList" );
                    if ( list.type == SimpleJson::JSON_ARRAY ) {
                        for ( size_t i = 0; i < list.Size(); ++i ) {
                            m_clsAllowSipUserAgentMap.Insert( list.At( i ).AsString().c_str(), "" );
                        }
                    }
                }

                if ( sec.Has( "AllowClientIpList" ) ) {
                    SimpleJson::JsonNode list = sec.Get( "AllowClientIpList" );
                    if ( list.type == SimpleJson::JSON_ARRAY ) {
                        for ( size_t i = 0; i < list.Size(); ++i ) {
                            m_clsAllowClientIpMap.Insert( list.At( i ).AsString().c_str(), "" );
                        }
                    }
                }
            }

            // v3 (2026-04-22): Setup.Realm 파싱 제거 — access_services.jsonl 이 SOT.
        }

        m_strFileName = pszFileName;
        SetFileSizeTime();

        // Auto-detect IP logic same as below... duplicating for now or refactor.
        if ( m_strLocalIp == "0.0.0.0" ) {
            int fd = socket( AF_INET, SOCK_DGRAM, 0 );
            if ( fd >= 0 ) {
                struct ifconf ifc;
                char buf[1024];
                ifc.ifc_len = sizeof( buf );
                ifc.ifc_buf = buf;
                if ( ioctl( fd, SIOCGIFCONF, &ifc ) == 0 ) {
                    struct ifreq *it = ifc.ifc_req;
                    const struct ifreq *const end = it + ( ifc.ifc_len / sizeof( struct ifreq ) );
                    for ( ; it != end; ++it ) {
                        struct sockaddr_in *addr = (struct sockaddr_in *)&it->ifr_addr;
                        if ( addr->sin_family == AF_INET ) {
                            std::string ip = inet_ntoa( addr->sin_addr );
                            if ( ip != "127.0.0.1" && ip != "0.0.0.0" ) {
                                m_strLocalIp = ip;
                                break;
                            }
                        }
                    }
                }
                close( fd );
            }
            CLog::Print( LOG_INFO, "Auto-detected LocalIp: %s", m_strLocalIp.c_str() );
        }

        return true;
    }
    return true;
}

/**
 * @ingroup CspServer
 * @brief 수정된 설정 파일을 읽는다.
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CSipServerSetup::Read() {
    if ( m_strFileName.length() == 0 ) return false;

    // JSON 설정은 전체 재파싱 경로(overlay 병합 포함)로 위임 — scalar 리로드(SIGUSR1/mtime)가
    //   타는 이 경로가 JSON 파일을 XML 파서로 읽어 항상 무동작이던 결함의 수정.
    //   bootstrap 성 필드는 재반영돼도 기존 객체에 미적용(CspServer 리로드 주석과 동일 한계).
    if ( m_strFileName.substr( m_strFileName.find_last_of( "." ) + 1 ) == "json" ) {
        return Read( m_strFileName.c_str() );
    }

    CXmlElement clsXml;

    if ( clsXml.ParseFile( m_strFileName.c_str() ) == false ) return false;

    Read( clsXml );
    SetFileSizeTime();

    return true;
}

/**
 * @brief 설정 파일의 정보 중에서 실시간으로 변경 가능한 항목을 다시 저장한다.
 * @param clsXml 설정 파일의 내용을 저장한 변수
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CSipServerSetup::Read( CXmlElement &clsXml ) {
    CXmlElement *pclsElement;

    // 로그
    pclsElement = clsXml.SelectElement( "Log" );
    if ( pclsElement ) {
        m_iLogLevel = 0;

        CXmlElement *pclsClient = pclsElement->SelectElement( "Level" );
        if ( pclsClient ) {
            bool bTemp;

            pclsClient->SelectAttribute( "Debug", bTemp );
            if ( bTemp ) m_iLogLevel |= LOG_DEBUG;

            pclsClient->SelectAttribute( "Info", bTemp );
            if ( bTemp ) m_iLogLevel |= LOG_INFO;

            pclsClient->SelectAttribute( "Network", bTemp );
            if ( bTemp ) m_iLogLevel |= LOG_NETWORK;

            pclsClient->SelectAttribute( "Sql", bTemp );
            if ( bTemp ) m_iLogLevel |= LOG_SQL;
        }

        pclsElement->SelectElementData( "MaxSize", m_iLogMaxSize );

        CLog::SetLevel( m_iLogLevel );
        CLog::SetMaxLogSize( m_iLogMaxSize );
    }

    // RTP relay 설정
    pclsElement = clsXml.SelectElement( "RtpRelay" );
    if ( pclsElement ) {
        pclsElement->SelectElementData( "UseRtpRelay", m_bUseRtpRelay );
        pclsElement->SelectElementData( "CmpIp", m_strCmpIp );
        pclsElement->SelectElementData( "CmpIp", m_strCmpIp );
        pclsElement->SelectElementData( "CmpPort", m_iCmpPort );
        pclsElement->SelectElementData( "LocalCmpPort", m_iLocalCmpPort );
    }

    // 모니터링
    m_clsMonitorIpMap.DeleteAll();

    pclsElement = clsXml.SelectElement( "Monitor" );
    if ( pclsElement ) {
        InsertStringMap( pclsElement, "ClientIpList", "ClientIp", m_clsMonitorIpMap );
    }

    // 보안
    m_clsDenySipUserAgentMap.DeleteAll();
    m_clsAllowSipUserAgentMap.DeleteAll();
    m_clsAllowClientIpMap.DeleteAll();

    pclsElement = clsXml.SelectElement( "Security" );
    if ( pclsElement ) {
        InsertStringMap( pclsElement, "DenySipUserAgentList", "SipUserAgent", m_clsDenySipUserAgentMap );
        InsertStringMap( pclsElement, "AllowSipUserAgentList", "SipUserAgent", m_clsAllowSipUserAgentMap );
        InsertStringMap( pclsElement, "AllowClientIpList", "Ip", m_clsAllowClientIpMap );
    }

    return true;
}

/**
 * @ingroup CspServer
 * @brief 입력된 아이디가 Call PickUp 아이디인지 검사한다.
 * @param pszId 아이디
 * @returns 입력된 아이디가 Call PickUp 아이디이면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServerSetup::IsCallPickupId( const char *pszId ) {
    if ( m_strCallPickupId.empty() ) return false;

    if ( !strcmp( m_strCallPickupId.c_str(), pszId ) ) return true;

    return false;
}

/**
 * @ingroup CspServer
 * @brief 모니터링 클라이언트 IP 주소인가?
 * @param pszIp		클라이언트 IP 주소
 * @returns 모니터링 클라이언트 IP 주소이면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServerSetup::IsMonitorIp( const char *pszIp ) {
    return m_clsMonitorIpMap.Select( pszIp );
}

/**
 * @ingroup CspServer
 * @brief 허용된 SIP User-Agent 인지 검사한다.
 * @param pszSipUserAgent SIP User-Agent 문자열
 * @returns 허용된 SIP User-Agent 이면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServerSetup::IsAllowUserAgent( const char *pszSipUserAgent ) {
    // 허용된 SIP UserAgent 자료구조가 저장되어 있지 않으면 모두 허용한다.
    if ( m_clsAllowSipUserAgentMap.GetCount() == 0 ) return true;

    return m_clsAllowSipUserAgentMap.Select( pszSipUserAgent );
}

/**
 * @ingroup CspServer
 * @brief 허용되지 않은 SIP User-Agent 인지 검사한다.
 * @param pszSipUserAgent SIP User-Agent 문자열
 * @returns 허용되지 않은 SIP User-Agent 이면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServerSetup::IsDenyUserAgent( const char *pszSipUserAgent ) {
    return m_clsDenySipUserAgentMap.Select( pszSipUserAgent );
}

/**
 * @ingroup CspServer
 * @brief 허용된 SIP 클라이언트 IP 주소인지 검사한다.
 * @param pszClientIp SIP 클라이언트 IP 주소
 * @returns 허용된 SIP 클라이언트 IP 주소이면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServerSetup::IsAllowClientIp( const char *pszClientIp ) {
    // 허용된 클라이언트 IP 주소가 저장되어 있지 않으면 모두 허용한다.
    if ( m_clsAllowClientIpMap.GetCount() == 0 ) return true;

    return m_clsAllowClientIpMap.Select( pszClientIp );
}

/**
 * @ingroup CspServer
 * @brief 설정파일이 수정되었는지 확인한다.
 * @returns 설정파일이 수정되었으면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServerSetup::IsChange() {
    struct stat clsStat;

    if ( stat( m_strFileName.c_str(), &clsStat ) == 0 ) {
        if ( m_iFileSize != clsStat.st_size || m_iFileTime != clsStat.st_mtime ) return true;
    }

    return false;
}

/**
 * @ingroup CspServer
 * @brief 설정파일의 저장 시간을 저장한다.
 */
void CSipServerSetup::SetFileSizeTime() {
    struct stat clsStat;

    if ( stat( m_strFileName.c_str(), &clsStat ) == 0 ) {
        m_iFileSize = clsStat.st_size;
        m_iFileTime = clsStat.st_mtime;
    }
}

// v3 (2026-04-22): GetDomainForService / GetServiceForDomain 제거.
//   대체: gclsServiceMap.GetDomainByKind() / gclsServiceMap.GetByDomain().kind
