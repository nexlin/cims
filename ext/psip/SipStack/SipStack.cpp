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

#include "SipStack.h"
#include "SipStackThread.h"
#include "SipDeleteQueue.h"
#include "SipTcpMessage.h"
#include "SipTlsMessage.h"
#include "SipQueue.h"
#include "TimeUtility.h"
#include "Log.h"
#include "MemoryDebug.h"

#include "SipStackCallBack.hpp"
#include "SipStackComm.hpp"

/**
 * @ingroup SipStack
 * @brief ������ - ���� ������ �ʱ�ȭ��Ű�� transaction list �� SIP stack �� �����Ų��.
 */
CSipStack::CSipStack()
{
	m_bStopEvent = false;
	m_bStackThreadRun = false;
	m_hUdpSocket = INVALID_SOCKET;
	m_hTcpSocket = INVALID_SOCKET;

#ifdef USE_TLS
	m_hTlsSocket = INVALID_SOCKET;
#endif

	m_bStarted = false;
	m_iUdpThreadRunCount = 0;
	m_iTcpThreadRunCount = 0;

	m_clsICT.SetSipStack( this );
	m_clsNICT.SetSipStack( this );
	m_clsIST.SetSipStack( this );
	m_clsNIST.SetSipStack( this );

	m_pclsSecurityCallBack = NULL;
}

/**
 * @ingroup SipStack
 * @brief �Ҹ���
 */
CSipStack::~CSipStack()
{
}

/**
 * @ingroup SipStack
 * @brief SIP stack �� �����Ѵ�. SIP stack ������� network ���� �����带 �����Ѵ�.
 * @param clsSetup SIP stack ���� �׸� ���� ��ü
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipStack::Start( CSipStackSetup & clsSetup )
{
	if( m_bStarted || m_bStopEvent ) return false;
	if( clsSetup.Check() == false ) return false;

	m_clsSetup = clsSetup;
	m_clsICT.SetTimerD( m_clsSetup.m_iTimerD );
	m_clsNIST.SetTimerJ( m_clsSetup.m_iTimerJ );
	m_clsTcpConnectMap.SetStateful( m_clsSetup.m_bStateful );

#ifdef USE_TLS
	m_clsTlsConnectMap.SetStateful( m_clsSetup.m_bStateful );
#endif

	InitNetwork();

	if( m_clsSetup.m_iLocalUdpPort > 0 || m_clsSetup.m_iUdpThreadCount > 0 )
	{
		m_hUdpSocket = UdpListen( m_clsSetup.m_iLocalUdpPort, NULL, m_clsSetup.m_bIpv6 );
		if( m_hUdpSocket == INVALID_SOCKET )
		{
			CLog::Print( LOG_ERROR, "UdpListen(%d) error", m_clsSetup.m_iLocalUdpPort );
			return false;
		}

		// port 0 자동할당 시 실제 포트를 반영
		if( m_clsSetup.m_iLocalUdpPort == 0 )
		{
			m_clsSetup.m_iLocalUdpPort = GetSocketPort( m_hUdpSocket );
			CLog::Print( LOG_INFO, "UDP auto-assigned port %d", m_clsSetup.m_iLocalUdpPort );
		}
	}

	if( m_clsSetup.m_iLocalTcpPort > 0 )
	{
		m_hTcpSocket = TcpListen( m_clsSetup.m_iLocalTcpPort, 255, NULL, m_clsSetup.m_bIpv6 );
		if( m_hTcpSocket == INVALID_SOCKET ) 
		{
			CLog::Print( LOG_ERROR, "TcpListen(%d) error", m_clsSetup.m_iLocalTcpPort );
			_Stop();
			return false;
		}

		m_clsTcpThreadList.SetMaxSocketPerThread( m_clsSetup.m_iTcpMaxSocketPerThread );
		if( m_clsTcpThreadList.Init( m_clsSetup.m_iTcpThreadCount, m_clsSetup.m_iTcpThreadCount, SipTcpThread, this ) == false )
		{
			CLog::Print( LOG_ERROR, "m_clsTcpThreadList.Init() error" );
			_Stop();
			return false;
		}

		if( StartSipTcpListenThread( this ) == false )
		{
			CLog::Print( LOG_ERROR, "StartSipTcpListenThread() error" );
			_Stop();
			return false;
		}
	}

#ifdef USE_TLS
	if( m_clsSetup.m_iLocalTlsPort > 0 )
	{
		if( SSLServerStart( m_clsSetup.m_strCertFile.c_str(), m_clsSetup.m_strCaCertFile.c_str() ) == false )
		{
			CLog::Print( LOG_ERROR, "SSLServerStart() error" );
			_Stop();
			return false;
		}

		m_hTlsSocket = TcpListen( m_clsSetup.m_iLocalTlsPort, 255, NULL, m_clsSetup.m_bIpv6 );
		if( m_hTlsSocket == INVALID_SOCKET ) 
		{
				CLog::Print( LOG_ERROR, "TcpListen(%d) error", m_clsSetup.m_iLocalTlsPort );
			_Stop();
			return false;
		}

		m_clsTlsThreadList.SetMaxSocketPerThread( m_clsSetup.m_iTcpMaxSocketPerThread );
		if( m_clsTlsThreadList.Init( m_clsSetup.m_iTcpThreadCount, m_clsSetup.m_iTcpThreadCount, SipTlsThread, this ) == false )
		{
			CLog::Print( LOG_ERROR, "m_clsTlsThreadList.Init() error" );
			_Stop();
			return false;
		}

		if( StartSipTlsListenThread( this ) == false )
		{
			CLog::Print( LOG_ERROR, "StartSipTlsListenThread() error" );
			_Stop();
			return false;
		}
	}
	else if( m_clsSetup.m_bTlsClient )
	{
		if( SSLClientStart( ) == false )
		{
			CLog::Print( LOG_ERROR, "SSLClientStart() error" );
			_Stop();
			return false;
		}

		m_clsTlsThreadList.SetMaxSocketPerThread( m_clsSetup.m_iTcpMaxSocketPerThread );
		if( m_clsTlsThreadList.Init( m_clsSetup.m_iTcpThreadCount, m_clsSetup.m_iTcpThreadCount, SipTlsThread, this ) == false )
		{
			CLog::Print( LOG_ERROR, "m_clsTlsThreadList.Init() error" );
			_Stop();
			return false;
		}
	}
#endif

	if( m_hUdpSocket != INVALID_SOCKET )
	{
		if( StartSipUdpThread( this ) == false )
		{
			CLog::Print( LOG_ERROR, "StartSipUdpThread() error" );
			_Stop();
			return false;
		}
	}

	if( m_clsSetup.m_bStateful )
	{
		if( StartSipStackThread( this ) == false )
		{
			CLog::Print( LOG_ERROR, "StartSipStackThread() error" );
			_Stop();
			return false;
		}
	}

	if( m_clsSetup.m_iTcpCallBackThreadCount > 0 )
	{
		for( int i = 0; i < m_clsSetup.m_iTcpCallBackThreadCount; ++i )
		{
			if( StartSipQueueThread( this ) == false )
			{
				CLog::Print( LOG_ERROR, "StartSipQueueThread() error" );
				_Stop();
				return false;
			}
		}
	}

	m_bStarted = true;

	return true;
}

/**
 * @ingroup SipStack
 * @brief SIP stack �� ������Ų��.
 * @returns �����ϸ� true �� �����ϰ� SIP stack �� ������� �ʾҰų� ���� �̺�Ʈ ó�� ���̸� false �� �����Ѵ�.
 */
bool CSipStack::Stop( )
{
	if( m_bStarted == false || m_bStopEvent ) return false;

	_Stop();

	m_clsCallBackList.clear();

	m_bStarted = false;

	return true;
}

/**
 * @ingroup SipStack
 * @brief SIP stack �� �����Ѵ�.
 *				SIP stack �� �����ϴ� Transaction List �� �ֱ������� �����Ͽ��� Re-Transmit �Ǵ� Timeout ���� ó���ϱ� ���ؼ� �� �Լ��� 20ms �������� ȣ���� �־�� �Ѵ�.
 * @param psttTime ���� �ð�
 * @returns true �� �����Ѵ�.
 */
bool CSipStack::Execute( struct timeval * psttTime )
{
	m_clsICT.Execute( psttTime );
	m_clsIST.Execute( psttTime );
	m_clsNICT.Execute( psttTime );
	m_clsNIST.Execute( psttTime );

	return true;
}

/**
 * @ingroup SipStack
 * @brief UDP SIP �޽��� ���� ������ ������ ������Ų��.
 * @param iThreadId UDP SIP �޽��� ���� ������ ������ ������Ű�� ���� UDP SIP �޽��� ���� ������ ������ ������ ����
 */
void CSipStack::IncreateUdpThreadCount( int & iThreadId )
{
	m_clsMutex.acquire();
	iThreadId = m_iUdpThreadRunCount;
	++m_iUdpThreadRunCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipStack
 * @brief UDP SIP �޽��� ���� ������ ������ ���ҽ�Ų��.
 */
void CSipStack::DecreateUdpThreadCount()
{
	m_clsMutex.acquire();
	--m_iUdpThreadRunCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipStack
 * @brief TCP SIP �޽��� ���� ������ ������ ������Ų��.
 * @param iThreadId UDP SIP �޽��� ���� ������ ������ ������Ű�� ���� UDP SIP �޽��� ���� ������ ������ ������ ����
 */
void CSipStack::IncreateTcpThreadCount( int & iThreadId )
{
	m_clsMutex.acquire();
	iThreadId = m_iTcpThreadRunCount;
	++m_iTcpThreadRunCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipStack
 * @brief TCP SIP �޽��� ���� ������ ������ ���ҽ�Ų��.
 */
void CSipStack::DecreateTcpThreadCount()
{
	m_clsMutex.acquire();
	--m_iTcpThreadRunCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipStack
 * @brief Transaction List �� ������ ���ڿ��� �����Ѵ�.
 * @param strBuf		���ڿ� ����
 */
void CSipStack::GetString( CMonitorString & strBuf )
{
	strBuf.Clear();

	strBuf.AddCol( m_clsICT.GetSize() );
	strBuf.AddCol( m_clsNICT.GetSize() );
	strBuf.AddCol( m_clsIST.GetSize() );
	strBuf.AddCol( m_clsNIST.GetSize() );
	strBuf.AddRow( gclsSipDeleteQueue.GetSize() );
}

/**
 * @ingroup SipStack
 * @brief Invite Client Transaction ������ ���ڿ��� �����Ѵ�.
 * @param strBuf ���ڿ� ����
 */
void CSipStack::GetICTString( CMonitorString & strBuf )
{
	m_clsICT.GetString( strBuf );
}

/**
 * @ingroup SipStack
 * @brief ���μ����� ����� ���� ���������� �����Ͽ��� openssl �޸� ������ ������� �ʴ´�. 
 */
void CSipStack::Final()
{
#ifdef USE_TLS
	SSLFinal();
#endif
}

/**
 * @ingroup SipStack
 * @brief ��� SIP transaction �� �����Ѵ�.
 */
void CSipStack::DeleteAllTransaction()
{
	m_clsICT.DeleteAll();
	m_clsNICT.DeleteAll();
	m_clsIST.DeleteAll();
	m_clsNIST.DeleteAll();

	gclsSipDeleteQueue.DeleteAll();
}

/**
 * @ingroup SipStack
 * @brief ICT transcation map �� �����´�.
 * @param clsMap [out] transcation map ���� ����
 */
void CSipStack::GetICTMap( INVITE_TRANSACTION_MAP & clsMap )
{
	m_clsICT.GetTransactionMap( clsMap );
}

/**
 * @ingroup SipStack
 * @brief UDP SIP �޽��� ���� �����忡 ���� SIP �޽����� �����ϰ� SIP stack �����忡 ���� �̺�Ʈ�� ������ ��, ��� �����尡 ������ ������ ����� ��,
 *				���� �ڵ��� �����Ų��.
 * @returns true �� �����Ѵ�.
 */
bool CSipStack::_Stop( )
{
	m_bStopEvent = true;

	if( m_clsSetup.m_iLocalUdpPort > 0 )
	{
		// SIP �޽��� ���� �����尡 N �� ����ǹǷ� N �� ����ϴ� ���� �����ϱ� ���� �ڵ��̴�.
		Socket hSocket = UdpSocket();

		if( hSocket != INVALID_SOCKET )
		{
			for( int i = 0; i < m_clsSetup.m_iUdpThreadCount; ++i )
			{
				UdpSend( hSocket, "\r\n", 2, "127.0.0.1", m_clsSetup.m_iLocalUdpPort );
			}

			closesocket( hSocket );
		}
	}

	gclsSipQueue.BroadCast();

	// ��� �����尡 ������ ������ ����Ѵ�.
	while( m_iUdpThreadRunCount > 0 || m_iTcpThreadRunCount > 0 || m_bStackThreadRun || GetTcpConnectingCount() > 0 )
	{
		MiliSleep( 20 );
	}

	if( m_hUdpSocket != INVALID_SOCKET )
	{
		closesocket( m_hUdpSocket );
		m_hUdpSocket = INVALID_SOCKET;
	}

	if( m_hTcpSocket != INVALID_SOCKET )
	{
		closesocket( m_hTcpSocket );
		m_hTcpSocket = INVALID_SOCKET;
	}

	m_clsTcpThreadList.Final();
	m_clsTcpSocketMap.DeleteAll();

#ifdef USE_TLS
	if( m_hTlsSocket != INVALID_SOCKET )
	{
		closesocket( m_hTlsSocket );
		m_hTlsSocket = INVALID_SOCKET;
	}

	m_clsTlsThreadList.Final();
	m_clsTlsSocketMap.DeleteAll();
	SSLServerStop();
#endif

	DeleteAllTransaction();

	m_bStopEvent = false;

	return true;
}

/**
 * @ingroup SipStack
 * @brief TCP/TLS ���� �������� ������ ������ �����Ѵ�.
 * @returns TCP/TLS ���� �������� ������ ������ �����Ѵ�.
 */
int CSipStack::GetTcpConnectingCount( )
{
	int iCount = m_clsTcpConnectMap.GetSize();

#ifdef USE_TLS
	iCount += m_clsTlsConnectMap.GetSize();
#endif

	return iCount;
}
