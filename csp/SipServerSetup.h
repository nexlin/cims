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
 * @brief SDP 오디오 코덱 1개의 설정 (Setup.Media.Codecs[] 엔트리)
 */
struct CspMediaCodec {
    std::string strName;  // rtpmap encoding name (예: "AMR-WB", "telephone-event")
    int iPt = -1;         // payload type (정적 코덱 = RFC 3551 고정 번호, 동적 = 96~127)
    int iClock = 8000;    // rtpmap clock rate
    int iChannels = 0;    // 0 = rtpmap 에 채널 미표기
    std::string strFmtp;  // a=fmtp 값 (비면 미출력)
    int iPtime = 0;       // a=ptime 값 (0 = 미출력)
};

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

    // v3 (2026-04-22): Realm/AuthRealm 필드 제거.
    //   domain/auth_realm 은 access_services.jsonl (CspAccessServiceMap) 이 SOT.
    //   도메인→서비스 매핑은 CCspServiceMap::BuildDomainToKindMap().

    /** SIP REGISTER timeout 최소 시간 */
    int m_iMinRegisterTimeout;

    /** RTP relay 기능 사용 여부 */
    bool m_bUseRtpRelay;

    /** 로그인된 사용자에게 OPTIONS 메시지를 전송하는 주기 (초단위) */
    int m_iSendOptionsPeriod;

    /** 사용자 계정 정보 저장 폴더 - 비어 있으면 DB 를 사용한다. */
    std::string m_strUserDataFolder;

    /** 그룹 정보 저장 폴더 - 비어 있으면 DB 를 사용한다. */
    std::string m_strGroupDataFolder;

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
    // Redis (Phase 1.D-2 — register state hot replication, optional)
    //   비어있으면 RedisStore cold-mode (단말 재 REGISTER 로만 fail-over 복원)
    std::string m_strRedisHost;
    int m_iRedisPort;
    std::string m_strRedisPassword;

    // ================================================================
    // 서비스 모드: "volte" | "ptt" | "both" (기본값: "both")

    /** 서비스 모드 */
    std::string m_strServiceMode;

    // ⚠️ 테스트 환경 전용 — 상용 배포 시 반드시 false(기본값) 유지/원복.
    //   공인 IP 노출 테스트망에서 NAT 뒤 정상 단말이 미등록/외부발신으로 보이는 문제를
    //   우회하기 위한 "개방형 착신(open termination)" 정책 스위치.
    //   true 이면: INVITE 착신이 로컬 가입자/그룹이면 발신자 401 챌린지 생략 통과,
    //             비가입자 착신이면 603 Decline + 무로그 처리.
    //   상용은 내부망이라 이 문제가 없으므로 false 로 두어 표준 인증 흐름을 사용한다.
    //   설정 키: Setup.TestEnvOpenTermination = "true" (미지정 시 false).
    bool m_bTestEnvOpenTermination;

    // ================================================================
    // IMS 역할 활성화 (기본값: 모두 true)

    bool m_bRoleCscf;
    bool m_bRoleTas;
    bool m_bRolePttAs;
    bool m_bRoleIbcf;
    bool m_bRoleMcData;

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
    int m_iLocalCmpPort;  // Local port to receive CMP messages
    // 전용 미디어(CMP) 풀 — MediaServer.Endpoints. SIP remote_nodes 와 분리된 미디어 평면.
    //   primary(m_strCmpIp:m_iCmpPort) 와 함께 CmpClient consistent-hash ring 에 등록되어
    //   Session-ID 기준으로 다중 CMP(AA) 에 relay 세션을 분배한다. 비어있으면 단일 운영.
    std::vector< std::pair< std::string, int > > m_vecCmpEndpoints;

    // CSP↔CMP 세션 재조정(audit 수준2) — Setup.MediaServer.Audit.*
    //   비정상(메시지 유실·재기동·절체)으로 생긴 CMP 고아 relay 를 HEARTBEAT digest 불일치로
    //   감지해 SESSION_LIST diff 후 RemoveSession 회수(sweeper 타임아웃보다 빠른 능동 수렴).
    //   ha_design.md 수준2. 상세: docs/api/cmp_media_api.md.
    bool m_bAuditEnable = true;        // digest 대조+orphan 회수 (기본 on)
    int  m_iAuditGraceSec = 30;        // SESSION_LIST min_age — 신규 setup 세션 오회수 방지
    int  m_iAuditMaxPerCycle = 20;     // 한 cycle 회수 상한(회수 폭풍 방지, 초과분 다음 cycle)
    bool m_bAuditZombieTeardown = false;  // CSP有-CMP無(zombie) 호 강제 종료 opt-in(기본 detect+log)
    // HA 역할 — active 만 회수 실행(standby 는 탐지·로그만; hot-standby 오회수 방지).
    //   "active"|"standby"|"auto"(기본, HA 미배치=active 취급). 절체 시 승격 노드가 active.
    std::string m_strHaRole = "auto";
    // HaRole=auto 에서 이 VIP(예: VIP_csp)를 로컬 소유하면 active, 아니면 standby 로 동적 판정.
    //   hot-standby 상시기동 시 승격(VIP 인수)을 별도 훅 없이 다음 audit cycle(≤3s)에 반영.
    //   빈 값이면 auto=active(단일노드/cold-spare — 승격 시 기동된 프로세스가 곧 active).
    std::string m_strHaVip;

    // ================================================================
    // CMDP(MCData Media Plane, MSRP) 연동 설정 — Setup.McDataMedia.*
    bool m_bUseMcDataMedia;     // Enable (기본 false — cmdp 미배치 환경 무영향)
    std::string m_strCmdpIp;    // cmdp 제어 주소
    int m_iCmdpPort;            // cmdp 제어 포트 (기본 9100)
    int m_iLocalCmdpPort;       // CSP 측 수신 포트 (기본 9101) — cmdp 이벤트 도착지

    // MCData C-plane 정책 — Setup.McData.*
    //   MaxPayloadSizeSdsCplaneBytes: TS 24.484 <max-payload-size-sds-cplane-bytes>.
    //   0/미설정=무제한(현행 동작). 초과 SDS MESSAGE 는 403 + Warning 203 거부(미디어평면 유도).
    int m_iMaxSdsCplaneBytes;
    //   FdUrlBase: FILEURL 폴백 배포의 다운로드 URL base (예: https://host:4430).
    //   비면 Xcap Host:4430 로 유도.
    std::string m_strFdUrlBase;

    // ================================================================
    // SDP 미디어 코덱 테이블 — Setup.Media.Codecs (배열 순서 = 우선순위, 첫 엔트리 = 서비스 코덱)
    //   psip CSipCodecTable 로 기동 시 1회 주입되어 SDP 오퍼/answer 의 코덱·PT·fmtp·우선순위를
    //   결정한다 (재기동 반영). 비어 있으면 psip 기본 테이블
    //   (AMR-WB 96 최우선 + AMR 98 + G.711 계열 + telephone-event 101).
    //   Name="telephone-event" 엔트리는 DTMF 슬롯으로 분리 취급된다.
    std::vector<CspMediaCodec> m_vecMediaCodecs;

    // ================================================================
    // XCAP / CSC 연동 (UE↔CSC, 3GPP TS 24.484 — Phase 3)
    //   xcap-diff NOTIFY 의 xcap-root URL 로 advertise 할 CSC XCAP(MCPTT) 서버 주소.
    //   비어있으면 m_strLocalIp 로 fallback (CSP 자기 IP — 단일 노드 호환).
    //   port 기본값 4430 = CSC McpttServer (GMS/CMS/KMS 라우트가 실제 서빙되는 포트).
    //   scheme 기본값 https (CSC McpttServer 는 cert 존재 시 TLS — 3GPP CSC-2/3 규격).
    std::string m_strXcapHost;
    int m_iXcapPort;
    std::string m_strXcapScheme;

    // ================================================================
    // 런타임 설정 jsonl 디렉토리 (agent 관리)
    std::string m_strConfigJsonlDir;  // agent 관리 config/ 디렉토리

    // Deployment config overlay 추적 (Read() 는 CLog 초기화 전 호출되므로 로그는 SIPServerStart 에서 출력)
    std::string m_strOverlayPath;  // 적용된 overlay 파일 경로 (빈 문자열 = 없음)
    int m_iOverlayKeys = 0;        // 적용된 키 개수

    // ================================================================
    // 녹취 설정
    bool m_bRecordEnable;
    std::string m_strRecordDir;  // NAS 마운트 경로 (raw + converted 공유)

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
