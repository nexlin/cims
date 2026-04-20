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
	m_iNextUdpListenerExtId = 0;

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

	// 기본 UDP 리스너 — setup 의 포트/IP 로 생성. 스레드는 후속 StartSipUdpThread 에서 기동.
	if( m_clsSetup.m_iLocalUdpPort > 0 || m_clsSetup.m_iUdpThreadCount > 0 )
	{
		CSipStackUdpListener * pListener = new CSipStackUdpListener();
		pListener->m_iId          = 0;   // 기본 리스너 = 0
		pListener->m_strBindIp    = m_clsSetup.m_strLocalIp;
		pListener->m_iPort        = m_clsSetup.m_iLocalUdpPort;
		pListener->m_iThreadCount = m_clsSetup.m_iUdpThreadCount;
		pListener->m_bIpv6        = m_clsSetup.m_bIpv6;
		pListener->m_pclsStack    = this;

		if( !_StartUdpListenerLocked( pListener ) )
		{
			CLog::Print( LOG_ERROR, "UdpListen(%d) error", m_clsSetup.m_iLocalUdpPort );
			delete pListener;
			return false;
		}

		// port 0 자동할당 시 실제 포트를 반영
		if( m_clsSetup.m_iLocalUdpPort == 0 )
		{
			m_clsSetup.m_iLocalUdpPort = pListener->m_iPort;
			CLog::Print( LOG_INFO, "UDP auto-assigned port %d", m_clsSetup.m_iLocalUdpPort );
		}

		m_clsUdpListenerMutex.acquire();
		m_vecUdpListeners.push_back( pListener );
		m_hUdpSocket = pListener->m_hSocket;   // 하위 호환 alias
		m_clsUdpListenerMutex.release();
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

	// UDP 리스너 전체 정리 (m_hUdpSocket 은 alias 이므로 별도 close 불필요)
	m_clsUdpListenerMutex.acquire();
	for( auto* pListener : m_vecUdpListeners )
	{
		if( pListener->m_hSocket != INVALID_SOCKET )
		{
			closesocket( pListener->m_hSocket );
			pListener->m_hSocket = INVALID_SOCKET;
		}
		delete pListener;
	}
	m_vecUdpListeners.clear();
	m_hUdpSocket = INVALID_SOCKET;
	m_clsUdpListenerMutex.release();

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

// ─────────────────────────────────────────────────────────────
//  P2: UDP 다중 리스너 hot-reload API
// ─────────────────────────────────────────────────────────────

/**
 * 단일 UDP 리스너를 bind 하고 내부 상태만 초기화.
 * 스레드 기동은 호출자(Start 경로 또는 AddUdpListener)가 별도 수행.
 * 호출 시점에 m_clsUdpListenerMutex 보유 상태가 아니어야 함(내부에서 UdpListen 은 block-on-bind).
 */
bool CSipStack::_StartUdpListenerLocked( CSipStackUdpListener * pListener )
{
	if( !pListener ) return false;

	const char * pszBindIp = pListener->m_strBindIp.empty() ? NULL : pListener->m_strBindIp.c_str();
	Socket hSock = UdpListen( (unsigned short)pListener->m_iPort, pszBindIp, pListener->m_bIpv6 );
	if( hSock == INVALID_SOCKET )
	{
		return false;
	}

	// 자동 할당 포트 반영
	if( pListener->m_iPort == 0 )
	{
		pListener->m_iPort = GetSocketPort( hSock );
	}
	pListener->m_hSocket = hSock;
	pListener->m_bDrain  = false;
	return true;
}

/**
 * Drain → socket close → active threads 0 대기.
 * 호출 전에 m_clsUdpListenerMutex 보유 상태에서 pListener 가 벡터에서 제거되어 있어야 함.
 * 여기서는 스레드 종료 루프만 수행.
 */
void CSipStack::_StopUdpListenerLocked( CSipStackUdpListener * pListener )
{
	if( !pListener ) return;
	pListener->m_bDrain = true;

	// 스레드들이 poll 에서 깨어나 drain flag 확인 후 종료할 시간을 줌
	Socket hSock = pListener->m_hSocket;
	pListener->m_hSocket = INVALID_SOCKET;
	if( hSock != INVALID_SOCKET )
	{
		// 스레드가 poll 에 블록돼 있을 수 있으므로 self-send 로 깨움
		Socket hWake = UdpSocket();
		if( hWake != INVALID_SOCKET )
		{
			for( int i = 0; i < pListener->m_iThreadCount; ++i )
			{
				UdpSend( hWake, "\r\n", 2, "127.0.0.1", pListener->m_iPort );
			}
			closesocket( hWake );
		}
		closesocket( hSock );
	}

	int iWaitMs = 0;
	while( pListener->m_iActiveThreads.load() > 0 && iWaitMs < 3000 )
	{
		MiliSleep( 20 );
		iWaitMs += 20;
	}
}

/** m_hUdpSocket alias 를 살아있는 첫 번째 리스너로 갱신. 모두 제거됐으면 INVALID_SOCKET. */
void CSipStack::_RefreshPrimaryUdpSocketLocked()
{
	for( auto* pL : m_vecUdpListeners )
	{
		if( pL && pL->m_hSocket != INVALID_SOCKET )
		{
			m_hUdpSocket = pL->m_hSocket;
			return;
		}
	}
	m_hUdpSocket = INVALID_SOCKET;
}

bool CSipStack::AddUdpListener( int iExtId, const char* pszBindIp, int iPort,
                                 int iThreadCount, int& outId )
{
	if( !m_bStarted ) return false;

	CSipStackUdpListener * pListener = new CSipStackUdpListener();
	pListener->m_iId          = (iExtId != 0) ? iExtId : (++m_iNextUdpListenerExtId);
	pListener->m_strBindIp    = (pszBindIp && *pszBindIp) ? pszBindIp : m_clsSetup.m_strLocalIp;
	pListener->m_iPort        = iPort;
	pListener->m_iThreadCount = (iThreadCount > 0) ? iThreadCount : 1;
	pListener->m_bIpv6        = m_clsSetup.m_bIpv6;
	pListener->m_pclsStack    = this;

	if( !_StartUdpListenerLocked( pListener ) )
	{
		CLog::Print( LOG_ERROR, "AddUdpListener: bind failed ip=%s port=%d",
		             pListener->m_strBindIp.c_str(), pListener->m_iPort );
		delete pListener;
		return false;
	}

	if( !StartSipUdpThreadForListener( this, pListener, pListener->m_iThreadCount ) )
	{
		CLog::Print( LOG_ERROR, "AddUdpListener: thread start failed id=%d", pListener->m_iId );
		closesocket( pListener->m_hSocket );
		delete pListener;
		return false;
	}

	m_clsUdpListenerMutex.acquire();
	m_vecUdpListeners.push_back( pListener );
	if( m_hUdpSocket == INVALID_SOCKET ) m_hUdpSocket = pListener->m_hSocket;
	m_clsUdpListenerMutex.release();

	outId = pListener->m_iId;
	CLog::Print( LOG_INFO, "AddUdpListener id=%d %s:%d threads=%d",
	             pListener->m_iId, pListener->m_strBindIp.c_str(),
	             pListener->m_iPort, pListener->m_iThreadCount );
	return true;
}

bool CSipStack::RemoveUdpListener( int iExtId )
{
	if( !m_bStarted ) return false;

	CSipStackUdpListener * pTarget = NULL;
	m_clsUdpListenerMutex.acquire();
	for( auto it = m_vecUdpListeners.begin(); it != m_vecUdpListeners.end(); ++it )
	{
		if( *it && (*it)->m_iId == iExtId )
		{
			pTarget = *it;
			m_vecUdpListeners.erase( it );
			break;
		}
	}
	if( pTarget && m_hUdpSocket == pTarget->m_hSocket )
	{
		// alias 를 다른 살아있는 리스너로
		_RefreshPrimaryUdpSocketLocked();
	}
	m_clsUdpListenerMutex.release();

	if( !pTarget )
	{
		CLog::Print( LOG_ERROR, "RemoveUdpListener: id=%d not found", iExtId );
		return false;
	}

	_StopUdpListenerLocked( pTarget );
	CLog::Print( LOG_INFO, "RemoveUdpListener id=%d %s:%d stopped",
	             pTarget->m_iId, pTarget->m_strBindIp.c_str(), pTarget->m_iPort );
	delete pTarget;
	return true;
}

void CSipStack::GetUdpListenerInfo( std::vector<CSipStackUdpListener*>& outList )
{
	m_clsUdpListenerMutex.acquire();
	outList = m_vecUdpListeners;
	m_clsUdpListenerMutex.release();
}
