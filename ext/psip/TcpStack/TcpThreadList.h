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

#ifndef _TCP_THREAD_LIST2_H_
#define _TCP_THREAD_LIST2_H_

#include "SipStackDefine.h"
#include "SipTcp.h"
#include "SipMutex.h"
#include <vector>
#include <string>
#include "MonitorString.h"
#include "TcpSessionList.h"

class CTcpStack;

/**
 * @ingroup TcpStack
 * @brief TCP �����忡 TCP ������ ������ ���� ���Ǵ� Ŭ���� - m_bUseThreadPipe �� true �� ���� ���ȴ�.
 */
class CTcpComm
{
public:
#ifdef USE_TLS
	CTcpComm() : m_hSocket(INVALID_SOCKET), m_iPort(0), m_psttSsl(NULL), m_pSslCtx(NULL), m_bClient(false)
#else
	CTcpComm() : m_hSocket(INVALID_SOCKET), m_iPort(0), m_psttSsl(NULL), m_bClient(false)
#endif
	{
		m_szIp[0] = '\0';
	}

	void Close()
	{
#ifdef USE_TLS
		if( m_psttSsl )
		{
			SSLClose( m_psttSsl );
			m_psttSsl = NULL;
		}
#endif

		if( m_hSocket != INVALID_SOCKET )
		{
			closesocket( m_hSocket );
			m_hSocket = INVALID_SOCKET;
		}
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

	/** TCP client �� ������ ����� ��쿡 true �̴�. */
	bool		m_bClient;
};

/**
 * @ingroup TcpStack
 * @brief ������ ����Ʈ�� ���ԵǴ� �ϳ��� ������ ���� ���� Ŭ����
 */
class CTcpThreadInfo
{
public:
	CTcpThreadInfo();
	~CTcpThreadInfo();

	void Close();

	int				m_iIndex;				// ������ �ε���
	Socket		m_hSend;				// �۽� pipe
	Socket		m_hRecv;				// ���� pipe

	CTcpStackSessionList	m_clsSessionList;
	CTcpStack	* m_pclsStack;
};

typedef std::vector< CTcpThreadInfo * > TCP_THREAD_LIST;

/**
 * @ingroup TcpStack
 * @brief ������ ����Ʈ �ڷᱸ��
 */
class CTcpThreadList
{
public:
	CTcpThreadList();
	~CTcpThreadList();

	bool Create( CTcpStack * pclsStack );
	void Destroy();

	bool SendCommand( const char * pszData, int iDataLen );
	bool SendCommand( const char * pszData, int iDataLen, int iThreadIndex );
	void SendCommandAll( const char * pszData, int iDataLen );
	static int RecvCommand( Socket hSocket, char * pszData, int iDataSize );

	bool Send( int iThreadIndex, int iSessionIndex, const char * pszPacket, int iPacketLen );
	bool SendAll( const char * pszPacket, int iPacketLen, ITcpStackCallBack * pclsCallBack );
	bool SendAllExcept( const char * pszPacket, int iPacketLen, ITcpStackCallBack * pclsCallBack, int iThreadIndex, int iSessionIndex );

	void DeleteNoUseThread();
	bool DeleteThread( int iThreadIndex );

	void GetString( CMonitorString & strBuf );

private:
	TCP_THREAD_LIST	m_clsList;
	int					m_iMaxSocketPerThread;
	int					m_iThreadIndex;
	CSipMutex		m_clsMutex;
	CTcpStack * m_pclsStack;

	bool AddThread();
	bool _SendCommand( Socket hSocket, const char * pszData, int iDataLen );
	int GetCount();
	int GetThreadIndex();
	bool SelectThreadIndex( int iThreadIndex );
};

#endif
