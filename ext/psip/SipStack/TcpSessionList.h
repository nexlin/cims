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

#ifndef _TCP_SESSION_LIST_H_
#define _TCP_SESSION_LIST_H_

#include "SipTcp.h"
#include "SipBuffer.h"
#include "TcpThreadList.h"
#include "TlsFunction.h"
#include <vector>
#include <string>

/**
 * @ingroup SipStack
 * @brief TCP �����忡 TCP ������ ������ ���� ���Ǵ� Ŭ����
 */
class CTcpComm
{
public:
#ifdef USE_TLS
	CTcpComm() : m_iPort(0), m_psttSsl(NULL), m_pSslCtx(NULL), m_cUseTimeout(1)
#else
	CTcpComm() : m_iPort(0), m_psttSsl(NULL), m_cUseTimeout(1)
#endif
	{
		m_szIp[0] = '\0';
	}

	void SetUseTimeout( bool bUseTimeout )
	{
		if( bUseTimeout )
		{
			m_cUseTimeout = 1;
		}
		else
		{
			m_cUseTimeout = 0;
		}
	}

	bool GetUseTimeout( )
	{
		if( m_cUseTimeout ) return true;

		return false;
	}

	Socket	m_hSocket;
	char		m_szIp[INET6_ADDRSTRLEN];	// ��Ŷ���� ���۵ǹǷ� std::string �� ����� �� ����.
	int			m_iPort;
	SSL			* m_psttSsl;
#ifdef USE_TLS
	/** R5.c: accept thread 가 per-listener SSL_CTX 를 worker 로 전달.
	 *  NULL 이면 worker 는 global gpsttServerCtx 사용 (하위 호환). */
	SSL_CTX		* m_pSslCtx;
#endif

private:
	char		m_cUseTimeout;
};

/**
 * @ingroup SipStack
 * @brief TCP ������ ���� �����ϴ� TCP ���� ����
 */
class CTcpSessionListInfo
{
public:
	CTcpSessionListInfo();
	void Clear();

	std::string		m_strIp;
	int						m_iPort;

#ifdef USE_TLS
	SSL						* m_psttSsl;
#endif

	CSipBuffer		m_clsSipBuf;

	time_t				m_iConnectTime;
	time_t				m_iRecvTime;

	bool					m_bUseTimeout;
};

typedef std::vector< CTcpSessionListInfo > SESSION_LIST;
class CSipStack;

/**
 * @ingroup SipStack
 * @brief TCP ������ ���� �����ϴ� TCP ���� ������ �����ϴ� Ŭ����
 */
class CTcpSessionList
{
public:
	CTcpSessionList( CSipStack * pclsSipStack, ESipTransport eTransport );
	~CTcpSessionList();

	bool Init( int iPollFdMax );
	bool Insert( Socket hSocket );
	bool Insert( CTcpComm & clsTcpComm, SSL * psttSsl = NULL );
	bool Delete( int iIndex, CThreadListEntry * pclsEntry );
	void DeleteAll( CThreadListEntry * pclsEntry );
	void DeleteTimeout( int iTimeout, CThreadListEntry * pclsEntry );

	struct pollfd * m_psttPollFd;
	SESSION_LIST	m_clsList;

	int	m_iPollFdMax;
	int m_iPoolFdCount;

private:
	CSipStack			* m_pclsSipStack;
	ESipTransport m_eTransport;
};

#endif
