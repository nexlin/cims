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
#include "TcpSessionList.h"
#include "ServerUtility.h"
#include "SipQueue.h"
#include "Log.h"
#include <time.h>
#include "MemoryDebug.h"

/**
 * @ingroup SipStack
 * @brief SIP �޽����� �Ľ��Ͽ��� SIP stack �� �Է��Ѵ�.
 * @param pclsSipStack SIP stack
 * @param iThreadId		UDP ������ ��ȣ
 * @param pszBuf			��Ʈ��ũ���� ���ŵ� SIP �޽���
 * @param iBufLen			��Ʈ��ũ���� ���ŵ� SIP �޽����� ����
 * @param pszIp				IP �ּ�
 * @param iPort				��Ʈ ��ȣ
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
static bool SipMessageProcess( CSipStack * pclsSipStack, int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort )
{
	CLog::Print( LOG_NETWORK, "TcpRecv(%s:%d) \n[%.*s]", pszIp, iPort, iBufLen, pszBuf );

	if( pclsSipStack->m_clsSetup.m_iTcpCallBackThreadCount > 0 )
	{
		return gclsSipQueue.Insert( pszBuf, iBufLen, pszIp, iPort, E_SIP_TCP );
	}

	return pclsSipStack->RecvSipMessage( iThreadId, pszBuf, iBufLen, pszIp, iPort, E_SIP_TCP );
}

/**
 * @ingroup SipStack
 * @brief TCP ������ ���� ������ �Լ�
 * @param lpParameter CThreadListEntry ��ü�� ������
 * @returns 0 �� �����Ѵ�.
 */
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

/** 
 * @ingroup SipStack
 * @brief TCP �������ݷ� SIP �޽��� ���� �� SIP ���� �̺�Ʈ�� ó���ϴ� ������ �Լ�
 * @param lpParameter SIP stack ������
 * @returns 0 �� �����Ѵ�.
 */
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
				closesocket( hConnFd );
			}
		}
	}

FUNC_END:
	pclsSipStack->DecreateTcpThreadCount( );

	return 0;
}

/**
 * @ingroup SipStack
 * @brief TCP �������ݷ� SIP �޽��� ���� �� SIP ���� �̺�Ʈ�� ó���ϴ� Thread Pool �� �����Ѵ�.
 * @param pclsSipStack SIP stack ������
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool StartSipTcpListenThread( CSipStack * pclsSipStack )
{
	return StartThread( "SipTcpListenThread", SipTcpListenThread, pclsSipStack );
}
