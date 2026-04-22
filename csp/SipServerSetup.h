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

#ifndef _SIP_SERVER_SETUP_H_
#define _SIP_SERVER_SETUP_H_

#include <list>
#include <map>
#include <string>
#include <vector>

#include "StringMap.h"
#include "XmlElement.h"

/**
 * @ingroup CspServer
 * @brief CspServer 설정 파일의 내용을 저장하는 클래스
 */
class CSipServerSetup {
public:
    CSipServerSetup();
    ~CSipServerSetup();

    /** SIP 통신을 위한 UDP 포트 번호 */
    int m_iUdpPort;

    /** SIP 통신을 위한 로컬 IP 주소 */
    std::string m_strLocalIp;

    /** SIP 통신을 위한 UDP 수신 쓰레드 개수 */
    int m_iUdpThreadCount;

    /** SIP 통신을 위한 TCP 포트 번호 */
    int m_iTcpPort;

    /** SIP 통신을 위한 TCP 수신 쓰레드 개수 */
    int m_iTcpThreadCount;

    /** SIP 메시지 수신 callback 처리를 위한 TCP 쓰레드 개수.
            본 개수가 0 이면 TCP 수신 쓰레드에서 callback 을 호출하고 0 보다 크면 tcp callback 쓰레드에서 callback 을
       호출한다. */
    int m_iTcpCallBackThreadCount;

    /** SIP 통신을 위한 TCP 수신 최대 대기 시간 ( 초단위 ) */
    int m_iTcpRecvTimeout;

    /** SIP 통신을 위한 TLS 포트 번호 */
    int m_iTlsPort;

    /** SIP 통신을 위한 TLS handshake timeout 시간 (초단위) */
    int m_iTlsAcceptTimeout;

    /** TLS 프로토콜을 위한 서버 인증서 + 키를 포함한 PEM 파일 full path */
    std::string m_strCertFile;

    /** TLS 세션으로 연결한 클라이언트 인증을 위한 인증 기관 인증서 PEM 파일 */
    std::string m_strCaCertFile;

    /** Call Pickup 을 위한 아이디 ( 전화번호 ) */
    std::string m_strCallPickupId;

    /** SIP stack 실행 주기 (ms 단위) */
    int m_iStackExecutePeriod;

    /** timer D 만료시간 (ms 단위) */
    int m_iTimerD;

    /** timer J 만료시간 (ms 단위) */
    int m_iTimerJ;

    /** IPv6 사용 유무 */
    bool m_bIpv6;

    /** IMS Digest 인증용 realm (WWW-Authenticate 의 realm 파라미터).
     *  미지정 시 m_mapDomainToService 의 첫 도메인으로 fallback. */
    std::string m_strAuthRealm;

    /** Realm 설정 배열을 flatten 한 도메인 → service 매핑.
     *  config:  "Realm": [{"service":"volte","domains":[...]}, {"service":"mcptt","domains":[...]}]
     *  예: "ims.mnc001..." → "volte" / "ptt.mnc001..." → "mcptt" */
    std::map<std::string, std::string> m_mapDomainToService;

    /** service → domains 역매핑 (SIP INVITE/REGISTER 생성 시 서비스별 대표 도메인 조회) */
    std::map<std::string, std::vector<std::string>> m_mapServiceToDomains;

    /** service 에 대한 대표 도메인 반환 (첫 도메인). 없으면 빈 문자열. */
    std::string GetDomainForService(const std::string& strService) const;

    /** 도메인이 어느 service 에 속하는지 반환. 미매칭 시 빈 문자열. */
    std::string GetServiceForDomain(const std::string& strDomain) const;

    /** SIP REGISTER timeout 최소 시간 */
    int m_iMinRegisterTimeout;

    /** RTP relay 기능 사용 여부 */
    bool m_bUseRtpRelay;



    /** 로그인된 사용자에게 OPTIONS 메시지를 전송하는 주기 (초단위) */
    int m_iSendOptionsPeriod;

    /** 사용자 계정 정보 저장 폴더 - 비어 있으면 DB 를 사용한다. */
    std::string m_strUserDataFolder;

    /** IP-PBX 정보 저장 폴더 */
    std::string m_strSipServerDataFolder;

    /** 그룹 정보 저장 폴더 - 비어 있으면 DB 를 사용한다. */
    std::string m_strGroupDataFolder;

    /** CDR 파일 저장 폴더 */
    std::string m_strCdrFolder;

    /** SIP REGISTER 를 전송한 후, 수신한 401 응답의 Authenticate 를 저장하여서 다음 주기의 SIP REGISTER 메시지를 생성할
     * 때에 사용하는 경우 true 로 설정한다. */
    bool m_bUseRegisterSession;

    /** User Alive Check Timeout (Seconds) */
    int m_iUserTimeout;

    /** Stale Call Timeout (Seconds) — 마지막 SIP activity 이후 무응답 통화 자동 종료 (0=비활성) */
    int m_iStaleCallTimeout;

    // ================================================================
    // DB 연동 설정 (UserDataFolder / GroupDataFolder 가 비어 있을 때 사용)

    /** DB 서버 호스트 */
    std::string m_strDbHost;

    /** DB 서버 포트 */
    int m_iDbPort;

    /** DB 접속 계정 */
    std::string m_strDbUser;

    /** DB 접속 패스워드 */
    std::string m_strDbPasswd;

    /** DB 이름 */
    std::string m_strDbName;

    // ================================================================
    // 서비스 모드: "voip" | "ptt" | "both" (기본값: "both")

    /** 서비스 모드 */
    std::string m_strServiceMode;

    // ================================================================
    // IMS 역할 활성화 (기본값: 모두 true)

    bool m_bRoleCscf;
    bool m_bRoleTas;
    bool m_bRolePttAs;
    bool m_bRoleIbcf;

    // ================================================================
    // 로그 기능

    /** 로그 폴더 */
    std::string m_strLogFolder;

    /** 메시지 로그 디렉터리 — 인터페이스별 통계용 (빈 값이면 비활성화) */
    std::string m_strMsgLogDir;

    /** 서비스 로그 디렉터리 — 통화 이력/Flow/녹취용 (빈 값이면 비활성화) */
    std::string m_strServiceLogDir;

    /** 시스템 식별자 (로그 디렉터리 하위 구분) — 기본값: "csp_01" */
    std::string m_strSystemId;

    /** 로그 레벨 */
    int m_iLogLevel;

    /** 로그 파일의 최대 크기 */
    int m_iLogMaxSize;

    // ================================================================
    // 모니터링 기능

    /** 모니터링 TCP 포트 번호 */
    int m_iMonitorPort;

    /** 모니터링 TCP 포트에 접속 허용할 IP 주소 맵 */
    CStringMap m_clsMonitorIpMap;

    // ================================================================
    // CMP 연동 설정
    std::string m_strCmpIp;
    int m_iCmpPort;
    int m_iLocalCmpPort; // Local port to receive CMP messages

    // ================================================================
    // 런타임 설정 jsonl 디렉토리 (agent 관리)
    std::string m_strConfigJsonlDir;   // agent 관리 config/ 디렉토리

    // Deployment config overlay 추적 (Read() 는 CLog 초기화 전 호출되므로 로그는 SIPServerStart 에서 출력)
    std::string m_strOverlayPath;      // 적용된 overlay 파일 경로 (빈 문자열 = 없음)
    int         m_iOverlayKeys = 0;    // 적용된 키 개수

    // ================================================================
    // 녹취 설정
    bool m_bRecordEnable;
    std::string m_strRecordDir;       // NAS 마운트 경로 (raw + converted 공유)

    // ================================================================
    // 보안 기능

    /** SIP transaction list 에 저장하지 않을 SIP User Agent 맵 */
    CStringMap m_clsDenySipUserAgentMap;

    /** SIP transaction list 에 저장할 SIP User Agent 맵 */
    CStringMap m_clsAllowSipUserAgentMap;

    /** 로그인을 허용하는 클라이언트 IP 주소 맵 */
    CStringMap m_clsAllowClientIpMap;

    bool Read( const char *pszFileName );
    bool Read();

    bool IsCallPickupId( const char *pszId );

    bool IsMonitorIp( const char *pszIp );

    bool IsAllowUserAgent( const char *pszSipUserAgent );
    bool IsDenyUserAgent( const char *pszSipUserAgent );

    bool IsAllowClientIp( const char *pszClientIp );

    bool IsChange();

private:
    bool Read( CXmlElement &clsXml );
    void SetFileSizeTime();

    std::string m_strFileName;  // 설정 파일 이름
    time_t m_iFileTime;         // 설정 파일 저장 시간
    int m_iFileSize;            // 설정 파일 크기
};

extern CSipServerSetup gclsSetup;

#endif
