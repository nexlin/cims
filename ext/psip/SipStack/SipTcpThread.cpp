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

#include "SipStackThread.h"
#include "SipStackListener.h"
#include "TcpSessionList.h"
#include "ServerUtility.h"
#include "SipQueue.h"
#include "Log.h"
#include <time.h>
#include "MemoryDebug.h"

// SIP 메시지를 파싱하여서 SIP stack 에 입력한다.
static bool SipMessageProcess( CSipStack * pclsSipStack, int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort )
{
	CLog::Print( LOG_NETWORK, "TcpRecv(%s:%d) \n[%.*s]", pszIp, iPort, iBufLen, pszBuf );

	if( pclsSipStack->m_clsSetup.m_iTcpCallBackThreadCount > 0 )
	{
		return gclsSipQueue.Insert( pszBuf, iBufLen, pszIp, iPort, E_SIP_TCP );
	}

	return pclsSipStack->RecvSipMessage( iThreadId, pszBuf, iBufLen, pszIp, iPort, E_SIP_TCP );
}

// TCP 세션을 위한 쓰레드 함수
THREAD_API SipTcpThread( LPVOID lpParameter )
{
	CThreadListEntry * pclsEntry = (CThreadListEntry *)lpParameter;
	CSipStack * pclsSipStack = (CSipStack *)pclsEntry->m_pUser;
	CTcpSessionList	clsSessionList( pclsSipStack, E_SIP_TCP );
	CTcpComm			clsTcpComm;
	int		n, i, iBufLen, iThreadId;
	char	szBuf[2048], *pszBuf;
	time_t	iTime, iDeleteTime;

	pclsSipStack->IncreateTcpThreadCount( iThreadId );

	if( clsSessionList.Init( pclsSipStack->m_clsSetup.m_iTcpMaxSocketPerThread + 1 ) == false ) goto FUNC_END;
	if( clsSessionList.Insert( pclsEntry->m_hRecv ) == false ) goto FUNC_END;

	time( &iDeleteTime );
	while( pclsSipStack->m_bStopEvent == false )
	{
		n = poll( clsSessionList.m_psttPollFd, clsSessionList.m_iPoolFdCount, 1000 );
		time( &iTime );
		if( n <= 0 ) goto LOOP_END;

		if( clsSessionList.m_psttPollFd[0].revents & POLLIN )
		{
			if( CThreadList::RecvCommand( clsSessionList.m_psttPollFd[0].fd, (char *)&clsTcpComm, sizeof(clsTcpComm) ) == sizeof(clsTcpComm) )
			{
				if( clsSessionList.Insert( clsTcpComm ) )
				{
					pclsSipStack->m_clsTcpSocketMap.Insert( clsTcpComm.m_szIp, clsTcpComm.m_iPort, clsTcpComm.m_hSocket );
				}
				else
				{
					pclsSipStack->TcpSessionEnd( clsTcpComm.m_szIp, clsTcpComm.m_iPort, E_SIP_TCP );
					closesocket( clsTcpComm.m_hSocket );
					pclsEntry->DecreaseSocketCount();
				}
			}
			--n;
		}

		if( n == 0 ) goto LOOP_END;

		for( i = 1; i < clsSessionList.m_iPoolFdCount; ++i )
		{
			if( !(clsSessionList.m_psttPollFd[i].revents & POLLIN) ) continue;

			n = recv( clsSessionList.m_psttPollFd[i].fd, szBuf, sizeof(szBuf), 0 );
			if( n <= 0 )
			{
CLOSE_SESSION:
				pclsSipStack->m_clsTcpSocketMap.Delete( clsSessionList.m_clsList[i].m_strIp.c_str(), clsSessionList.m_clsList[i].m_iPort );
				clsSessionList.Delete( i, pclsEntry );
				continue;
			}

			clsSessionList.m_clsList[i].m_iRecvTime = iTime;

			if( clsSessionList.m_clsList[i].m_clsSipBuf.AddBuf( szBuf, n ) == false ) goto CLOSE_SESSION;

			while( clsSessionList.m_clsList[i].m_clsSipBuf.GetSipMessage( &pszBuf, &iBufLen ) )
			{
				SipMessageProcess( pclsSipStack, iThreadId, pszBuf, iBufLen, clsSessionList.m_clsList[i].m_strIp.c_str(), clsSessionList.m_clsList[i].m_iPort );
				clsSessionList.m_clsList[i].m_clsSipBuf.ShiftBuf( iBufLen );
			}
		}

LOOP_END:
		if( ( iDeleteTime + 5 ) < iTime )
		{
			clsSessionList.DeleteTimeout( pclsSipStack->m_clsSetup.m_iTcpRecvTimeout, pclsEntry );
			iDeleteTime = iTime;
		}
	}

	clsSessionList.DeleteAll( pclsEntry );

FUNC_END:
	pclsSipStack->ThreadEnd( iThreadId );
	pclsSipStack->DecreateTcpThreadCount();

	return 0;
}

// TCP 프로토콜로 SIP 메시지 수신 및 SIP 수신 이벤트를 처리하는 쓰레드 함수
THREAD_API SipTcpListenThread( LPVOID lpParameter )
{
	CSipStack * pclsSipStack = (CSipStack *)lpParameter;
	struct pollfd arrPollFd[1];
	int		n, iThreadId;
	Socket	hConnFd;
	CTcpComm		clsTcpComm;

	if( pclsSipStack->m_hTcpSocket == INVALID_SOCKET )
	{
		CLog::Print( LOG_ERROR, "%s pclsSipStack->m_hTcpSocket == INVALID_SOCKET", __FUNCTION__ );
		goto FUNC_END;
	}

	pclsSipStack->IncreateTcpThreadCount( iThreadId );
	TcpSetPollIn( arrPollFd[0], pclsSipStack->m_hTcpSocket );

	while( pclsSipStack->m_bStopEvent == false )
	{
		n = poll( arrPollFd, 1, 1000 );
		if( n > 0 )
		{
			if( !(arrPollFd[0].revents & POLLIN) ) continue;

			hConnFd = TcpAccept( arrPollFd[0].fd, clsTcpComm.m_szIp, sizeof(clsTcpComm.m_szIp), &clsTcpComm.m_iPort, pclsSipStack->m_clsSetup.m_bIpv6 );
			if( hConnFd == INVALID_SOCKET )
			{
				continue;
			}

			clsTcpComm.m_hSocket = hConnFd;

			if( pclsSipStack->m_clsTcpThreadList.SendCommand( (char *)&clsTcpComm, sizeof(clsTcpComm) ) == false )
			{
				CLog::Print( LOG_ERROR, "%s: SendCommand failed (TCP worker pool) — close %s:%d",
				             __FUNCTION__, clsTcpComm.m_szIp, clsTcpComm.m_iPort );
				closesocket( hConnFd );
			}
		}
	}

FUNC_END:
	pclsSipStack->DecreateTcpThreadCount( );

	return 0;
}

// TCP 프로토콜로 SIP 메시지 수신 및 SIP 수신 이벤트를 처리하는 Thread Pool 을 시작한다.
bool StartSipTcpListenThread( CSipStack * pclsSipStack )
{
	return StartThread( "SipTcpListenThread", SipTcpListenThread, pclsSipStack );
}

// ── R3: per-listener TCP accept thread ─────────────────────────
/**
 * @brief 특정 CSipStackTcpListener 의 socket 을 단독 poll/accept 하여
 *        shared worker pool (m_clsTcpThreadList) 로 배분.
 * @param lpParameter CSipStackTcpListener*
 */
THREAD_API SipTcpListenerThread( LPVOID lpParameter )
{
	CSipStackTcpListener * pListener = (CSipStackTcpListener *)lpParameter;
	CSipStack * pclsSipStack = pListener->m_pclsStack;
	struct pollfd arrPollFd[1];
	int		n, iThreadId = 0;
	Socket	hConnFd;
	CTcpComm clsTcpComm;

	if( pListener->m_hSocket == INVALID_SOCKET )
	{
		CLog::Print( LOG_ERROR, "%s listener id=%d socket == INVALID_SOCKET",
		             __FUNCTION__, pListener->m_iId );
		return 0;
	}

	pclsSipStack->IncreateTcpThreadCount( iThreadId );
	pListener->m_iActiveThreads.fetch_add( 1 );

	TcpSetPollIn( arrPollFd[0], pListener->m_hSocket );

	while( pclsSipStack->m_bStopEvent == false && pListener->m_bDrain.load() == false )
	{
		n = poll( arrPollFd, 1, 1000 );
		if( n > 0 )
		{
			if( !(arrPollFd[0].revents & POLLIN) ) continue;

			hConnFd = TcpAccept( arrPollFd[0].fd,
			                     clsTcpComm.m_szIp, sizeof(clsTcpComm.m_szIp),
			                     &clsTcpComm.m_iPort,
			                     pclsSipStack->m_clsSetup.m_bIpv6 );
			if( hConnFd == INVALID_SOCKET ) continue;

			clsTcpComm.m_hSocket = hConnFd;

			if( pclsSipStack->m_clsTcpThreadList.SendCommand(
			        (char *)&clsTcpComm, sizeof(clsTcpComm) ) == false )
			{
				CLog::Print( LOG_ERROR, "%s: SendCommand failed (TCP worker pool) — close %s:%d",
				             __FUNCTION__, clsTcpComm.m_szIp, clsTcpComm.m_iPort );
				closesocket( hConnFd );
			}
		}
	}

	pListener->m_iActiveThreads.fetch_sub( 1 );
	pclsSipStack->DecreateTcpThreadCount();
	return 0;
}

bool StartSipTcpListenThreadForListener( CSipStackTcpListener * pListener )
{
	return StartThread( "SipTcpListenerThread", SipTcpListenerThread, pListener );
}
