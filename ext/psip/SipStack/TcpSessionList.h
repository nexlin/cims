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

// TCP 쓰레드에 TCP 세션을 전달할 때에 사용되는 클래스
class CTcpComm
{
public:
#ifdef USE_TLS
	CTcpComm() : m_iPort(0), m_psttSsl(NULL), m_pSslCtx(NULL), m_iListenerId(0), m_cUseTimeout(1)
#else
	CTcpComm() : m_iPort(0), m_psttSsl(NULL), m_iListenerId(0), m_cUseTimeout(1)
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
	char		m_szIp[INET6_ADDRSTRLEN];	// 패킷으로 전송되므로 std::string 을 사용할 수 없다.
	int			m_iPort;
	SSL			* m_psttSsl;
#ifdef USE_TLS
	/** R5.c: accept thread 가 per-listener SSL_CTX 를 worker 로 전달.
	 *  NULL 이면 worker 는 global gpsttServerCtx 사용 (하위 호환). */
	SSL_CTX		* m_pSslCtx;
#endif
	/** 이 연결을 수락한 listener 의 id (0 = 레거시 단일 리스너). 연결 수명 동안 불변이며
	 *  수신 메시지의 m_iListenerId 로 전파된다 (UDP 의 thread-local 과 같은 역할). */
	int			m_iListenerId;

private:
	char		m_cUseTimeout;
};

// TCP 쓰레드 별로 관리하는 TCP 세션 정보
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

	/** 연결을 수락한 listener id (CTcpComm::m_iListenerId). */
	int						m_iListenerId;
};

typedef std::vector< CTcpSessionListInfo > SESSION_LIST;
class CSipStack;

// TCP 쓰레드 별로 관리하는 TCP 세션 정보를 저장하는 클래스
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
