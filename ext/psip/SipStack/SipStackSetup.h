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

#ifndef _SIP_STACK_SETUP_H_
#define _SIP_STACK_SETUP_H_

// SIP stack 설정 클래스
class CSipStackSetup
{
public:
	CSipStackSetup();
	~CSipStackSetup();

	bool Check( );
	int GetLocalPort( ESipTransport eTransport );

	// SIP 메시지에 저장되는 로컬 IP 주소
	std::string m_strLocalIp;

	// SIP 메시지 전송/수신용 UDP 포트 번호
	int					m_iLocalUdpPort;

	// SIP 메시지 수신용 UDP 쓰레드 개수
	int					m_iUdpThreadCount;

	// SIP 메시지 전송/수신용 TCP 포트 번호
	int					m_iLocalTcpPort;

	// SIP 메시지 전송/수신용 TLS 포트 번호
	int					m_iLocalTlsPort;

	// SIP 메시지 수신용 TCP 쓰레드 개수
	int					m_iTcpThreadCount;

	// SIP 메시지 수신 callback 처리를 위한 TCP 쓰레드 개수. 본 개수가 0 이면 TCP 수신 쓰레드에서 callback 을 호출하고 0 보다 크면 tcp callback 쓰레드에서 callback 을 호출한다.
	int					m_iTcpCallBackThreadCount;

	// SIP 메시지 수신용 TCP 쓰레드 하나에 포함될 수 있는 최대 소켓 개수
	int					m_iTcpMaxSocketPerThread;

	// SIP 메시지 수신용 TCP 소켓의 수신 대기 시간 (초단위)
	int					m_iTcpRecvTimeout;

	// TCP 세션 연결 timeout 시간 (초단위)
	int					m_iTcpConnectTimeout;

	// TLS 세션 handshake 대기 시간 (초단위)
	int					m_iTlsAcceptTimeout;

	// TLS 세션을 위한 서버 인증서 + 개인키를 포함한 PEM 파일
	std::string	m_strCertFile;
	/** TLS 개인키 파일. 비우면 m_strCertFile 에서 읽는다(cert+key 결합 PEM).
	 *  인증서와 키를 별도 파일로 두는 배치에서 필요하다. */
	std::string	m_strKeyFile;

	// TLS 세션으로 연결한 클라이언트 인증을 위한 인증 기관 인증서 PEM 파일. 서버(리스너) 쪽에 주면 **클라이언트 인증서를 요구**한다
	//   (SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT — 상호인증). m_bTlsVerifyServer 가 켜진 클라이언트 쪽에서는 서버 인증서 검증의 앵커다.
	std::string m_strCaCertFile;

	/** TLS 클라이언트(발신 연결)가 **서버 인증서를 검증**하는가 — 앵커 = m_strTlsVerifyCaFile(비면 m_strCaCertFile, 둘 다 비면 시스템 기본
	 *  저장소). 체인 검증만 하고 호스트명(SAN)은 대조하지 않는다(IP 로 접속하는 SIP 코어 간 NNI 관례 — TS 33.310 NDS/IP 는 체인·발급자
	 *  기준). 기본 false(기존 동작 유지). m_strCaCertFile 은 리스너의 클라이언트 인증서 요구도 켜므로, 검증 앵커만 두려면 이 필드를 쓴다. */
	bool m_bTlsVerifyServer;
	std::string m_strTlsVerifyCaFile;
	/** bootstrap TLS 접속점(m_iLocalTlsPort)이 **자기 전용 SSL_CTX**(m_strCertFile/KeyFile/CaCertFile)를 갖는가. 기본 false = 전역 서버 ctx
	 *  공유(ReloadTlsServerCert 로 무중단 교체되는 종래 동작 — CSP). 한 프로세스에 TLS 스택을 여럿 두는 응용(계측기 워커의 피어 엔진들)은
	 *  나중에 뜬 스택의 SSLServerStart 가 전역 ctx 를 갈아치우므로 true 로 두어 접속점마다 인증서를 고정한다. */
	bool m_bTlsPrivateCtx;
	/** TLS 클라이언트가 핸드셰이크에 **제시하는 클라이언트 인증서**(PEM 체인)와 개인키(비면 인증서 파일에서). 상대(서버)가 상호인증을
	 *  요구할 때 필요하다. 비면 제시하지 않는다. 클라이언트 SSL_CTX 는 프로세스 전역이지만 인증서·검증은 연결(SSL)마다 적용하므로
	 *  한 프로세스의 스택마다 다르게 둘 수 있다. */
	std::string m_strClientCertFile;
	std::string m_strClientKeyFile;

	// SIP UserAgent 헤더에 저장될 문자열
	std::string	m_strUserAgent;

	/** SIP domain for URI construction (From, To host, P-Asserted-Identity).
	 *  If empty, m_strLocalIp is used as fallback. */
	std::string m_strDomain;

	// SIP 메시지를 생성할 때에 compact form 으로 생성할지 설정
	bool				m_bUseSipCompactForm;

	// SIP stack 실행 주기 (ms 단위)
	int					m_iStackExecutePeriod;

	// timer D 만료시간 (ms 단위)
	int					m_iTimerD;

	// timer J 만료시간 (ms 단위)
	int					m_iTimerJ;

	// IPv6 사용 유무
	bool				m_bIpv6;

	// Stateful SIP stack 인가?
	bool				m_bStateful;

	// TLS 클라이언트만 사용하는가? SIP 클라이언트에서 TLS 서버는 사용하지 않고 TLS 클라이언트만 사용하는 경우 true 로 설정한다.
	bool				m_bTlsClient;

	// TCP 클라이언트만 사용하는가? TCP 리스너(m_iLocalTcpPort) 없이 TCP 로 발신·수신하려면 true.
	//   m_bTlsClient 와 같은 역할 — TCP worker pool 을 기동해 연결된 소켓의 응답/요청을 수신한다.
	bool				m_bTcpClient;

	// SIP 요청 메시지를 전송할 때에 Contact 헤더에 수신 포트 번호를 사용하는 경우 true 로 설정한다.
	bool				m_bUseContactListenPort;

	// SIP REGISTER 를 전송한 후, 수신한 401 응답의 Authenticate 를 저장하여서 다음 주기의 SIP REGISTER 메시지를 생성할 때에 사용하는 경우 true 로 설정한다.
	bool				m_bUseRegisterSession;

	// INVITE 수신시 100 Trying 을 전송하면 true 로 설정하고 그렇지 않으면 false 로 설정한다.
	bool				m_bSendTrying;
};

#endif
