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

/**
 * @ingroup SipStack
 * @brief SIP stack ���� Ŭ����
 */
class CSipStackSetup
{
public:
	CSipStackSetup();
	~CSipStackSetup();

	bool Check( );
	int GetLocalPort( ESipTransport eTransport );

	/** SIP �޽����� ����Ǵ� ���� IP �ּ� */
	std::string m_strLocalIp;

	/** SIP �޽��� ����/���ſ� UDP ��Ʈ ��ȣ */
	int					m_iLocalUdpPort;

	/** SIP �޽��� ���ſ� UDP ������ ���� */
	int					m_iUdpThreadCount;

	/** SIP �޽��� ����/���ſ� TCP ��Ʈ ��ȣ */
	int					m_iLocalTcpPort;

	/** SIP �޽��� ����/���ſ� TLS ��Ʈ ��ȣ */
	int					m_iLocalTlsPort;

	/** SIP �޽��� ���ſ� TCP ������ ���� */
	int					m_iTcpThreadCount;

	/** SIP �޽��� ���� callback ó���� ���� TCP ������ ����. 
			�� ������ 0 �̸� TCP ���� �����忡�� callback �� ȣ���ϰ� 0 ���� ũ�� tcp callback �����忡�� callback �� ȣ���Ѵ�. */
	int					m_iTcpCallBackThreadCount;

	/** SIP �޽��� ���ſ� TCP ������ �ϳ��� ���Ե� �� �ִ� �ִ� ���� ���� */
	int					m_iTcpMaxSocketPerThread;

	/** SIP �޽��� ���ſ� TCP ������ ���� ��� �ð� (�ʴ���) */
	int					m_iTcpRecvTimeout;

	/** TCP ���� ���� timeout �ð� (�ʴ���) */
	int					m_iTcpConnectTimeout;

	/** TLS ���� handshake ��� �ð� (�ʴ���) */
	int					m_iTlsAcceptTimeout;

	/** TLS ������ ���� ���� ������ + ����Ű�� ������ PEM ���� */
	std::string	m_strCertFile;

	/** TLS �������� ������ Ŭ���̾�Ʈ ������ ���� ���� ��� ������ PEM ���� */
	std::string m_strCaCertFile;

	/** SIP UserAgent ����� ����� ���ڿ� */
	std::string	m_strUserAgent;

	/** SIP domain for URI construction (From, To host, P-Asserted-Identity).
	 *  If empty, m_strLocalIp is used as fallback. */
	std::string m_strDomain;

	/** SIP �޽����� ������ ���� compact form ���� �������� ���� */
	bool				m_bUseSipCompactForm;

	/** SIP stack ���� �ֱ� (ms ����) */
	int					m_iStackExecutePeriod;

	/** timer D ����ð� (ms ����) */
	int					m_iTimerD;

	/** timer J ����ð� (ms ����) */
	int					m_iTimerJ;

	/** IPv6 ��� ���� */
	bool				m_bIpv6;

	/** Stateful SIP stack �ΰ�? */
	bool				m_bStateful;

	/** TLS Ŭ���̾�Ʈ�� ����ϴ°�? SIP Ŭ���̾�Ʈ���� TLS ������ ������� �ʰ� TLS Ŭ���̾�Ʈ�� ����ϴ� ��� true �� �����Ѵ�. */
	bool				m_bTlsClient;

	/** SIP ��û �޽����� ������ ���� Contact ����� ���� ��Ʈ ��ȣ�� ����ϴ� ��� true �� �����Ѵ�. */
	bool				m_bUseContactListenPort;

	/** SIP REGISTER �� ������ ��, ������ 401 ������ Authenticate �� �����Ͽ��� ���� �ֱ��� SIP REGISTER �޽����� ������ ���� ����ϴ� ��� true �� �����Ѵ�. */
	bool				m_bUseRegisterSession;

	/** INVITE ���Ž� 100 Trying �� �����ϸ� true �� �����ϰ� �׷��� ������ false �� �����Ѵ�. */
	bool				m_bSendTrying;
};

#endif
