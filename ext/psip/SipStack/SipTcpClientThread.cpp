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
#include "SipTcpMessage.h"
#include "Log.h"
#include "MemoryDebug.h"

class CSipTcpClientArg
{
public:
	CSipStack * m_pclsSipStack;
	std::string m_strIp;
	int					m_iPort;
	CSipMessage * m_pclsSipMessage;
	std::string m_strSourceIp;  // R5.b''': outbound connect 시 bind 할 로컬 source IP (빈 값이면 OS 자동)
};

/**
 * @ingroup SipStack
 * @brief TCP Ŭ���̾�Ʈ ���� ������ ���� ������ �Լ�
 * @param lpParameter CThreadListEntry ��ü�� ������
 * @returns 0 �� �����Ѵ�.
 */
THREAD_API SipTcpClientThread( LPVOID lpParameter )
{
	CSipTcpClientArg * pclsArg = (CSipTcpClientArg *)lpParameter;
	bool bRes = false;
	int iThreadCount = 0;

	pclsArg->m_pclsSipStack->IncreateTcpThreadCount( iThreadCount );

	CLog::Print( LOG_DEBUG, "%s(%s:%d) start", __FUNCTION__, pclsArg->m_strIp.c_str(), pclsArg->m_iPort );

	// R5.b''': Via[0] 또는 per-route 로 선택된 source IP 로 bind 후 connect
	Socket hSocket = TcpConnectFrom( pclsArg->m_strSourceIp.empty() ? NULL : pclsArg->m_strSourceIp.c_str(),
	                                 pclsArg->m_strIp.c_str(), pclsArg->m_iPort,
	                                 pclsArg->m_pclsSipStack->m_clsSetup.m_iTcpConnectTimeout );
	if( hSocket != INVALID_SOCKET )
	{
		if( SipTcpSend( hSocket, pclsArg->m_strIp.c_str(), pclsArg->m_iPort, pclsArg->m_pclsSipMessage, pclsArg->m_pclsSipStack->m_clsSetup.m_bUseContactListenPort ? pclsArg->m_pclsSipStack->m_clsSetup.m_iLocalTcpPort : 0 ) )
		{
			SIP_MESSAGE_LIST clsSipMessageList;

			if( pclsArg->m_pclsSipStack->m_clsTcpConnectMap.Delete( pclsArg->m_strIp.c_str(), pclsArg->m_iPort, clsSipMessageList ) )
			{
				SIP_MESSAGE_LIST::iterator	itList;

				for( itList = clsSipMessageList.begin(); itList != clsSipMessageList.end(); ++itList )
				{
					SipTcpSend( hSocket, pclsArg->m_strIp.c_str(), pclsArg->m_iPort, *itList, pclsArg->m_pclsSipStack->m_clsSetup.m_bUseContactListenPort ? pclsArg->m_pclsSipStack->m_clsSetup.m_iLocalTcpPort : 0 );
					--(*itList)->m_iUseCount;

					if( pclsArg->m_pclsSipStack->m_clsSetup.m_bStateful == false && (*itList)->m_iUseCount == 0 )
					{
						delete *itList;
					}
				}
			}

			CTcpComm		clsTcpComm;

			clsTcpComm.m_hSocket = hSocket;
			snprintf( clsTcpComm.m_szIp, sizeof(clsTcpComm.m_szIp), "%s", pclsArg->m_strIp.c_str() );
			clsTcpComm.m_iPort = pclsArg->m_iPort;
			clsTcpComm.SetUseTimeout( false );

			if( pclsArg->m_pclsSipStack->m_clsTcpThreadList.SendCommand( (char *)&clsTcpComm, sizeof(clsTcpComm) ) == false )
			{
				closesocket( hSocket );
			}
			
			bRes = true;
		}
	}
	else
	{
		CLog::Print( LOG_ERROR, "TcpConnect(%s:%d) error", pclsArg->m_strIp.c_str(), pclsArg->m_iPort );
	}

	if( bRes == false )
	{
		CSipMessage * pclsResponse = pclsArg->m_pclsSipMessage->CreateResponse( SIP_CONNECT_ERROR );
		if( pclsResponse )
		{
			pclsResponse->m_strClientIp = pclsArg->m_strIp;
			pclsArg->m_pclsSipStack->RecvSipMessage( 0, pclsResponse );
		}

		SIP_MESSAGE_LIST clsSipMessageList;

		if( pclsArg->m_pclsSipStack->m_clsTcpConnectMap.Delete( pclsArg->m_strIp.c_str(), pclsArg->m_iPort, clsSipMessageList ) )
		{
			SIP_MESSAGE_LIST::iterator	itList;

			for( itList = clsSipMessageList.begin(); itList != clsSipMessageList.end(); ++itList )
			{
				pclsResponse = (*itList)->CreateResponse( SIP_CONNECT_ERROR );
				if( pclsResponse )
				{
					pclsResponse->m_strClientIp = pclsArg->m_strIp;
					pclsArg->m_pclsSipStack->RecvSipMessage( 0, pclsResponse );
				}
				--(*itList)->m_iUseCount;

				if( pclsArg->m_pclsSipStack->m_clsSetup.m_bStateful == false && (*itList)->m_iUseCount == 0 )
				{
					delete *itList;
				}
			}
		}

		pclsArg->m_pclsSipStack->ThreadEnd( -1 );
	}

	--pclsArg->m_pclsSipMessage->m_iUseCount;
	if( pclsArg->m_pclsSipStack->m_clsSetup.m_bStateful == false && pclsArg->m_pclsSipMessage->m_iUseCount == 0 )
	{
		delete pclsArg->m_pclsSipMessage;
	}

	CLog::Print( LOG_DEBUG, "%s(%s:%d) end", __FUNCTION__, pclsArg->m_strIp.c_str(), pclsArg->m_iPort );

	pclsArg->m_pclsSipStack->DecreateTcpThreadCount();
	delete pclsArg;

	return 0;
}

/**
 * @ingroup SipStack
 * @brief TCP �������ݷ� SIP �޽��� ���� �� SIP ���� �̺�Ʈ�� ó���ϴ� Thread Pool �� �����Ѵ�.
 * @param pclsSipStack	SIP stack ������
 * @param pszIp					SIP �޽����� ������ IP �ּ�
 * @param iPort					SIP �޽����� ������ ��Ʈ ��ȣ
 * @param pclsSipMessage	������ SIP �޽���
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool StartSipTcpClientThread( CSipStack * pclsSipStack, const char * pszIp, int iPort, CSipMessage * pclsSipMessage )
{
	if( pclsSipStack->m_clsTcpConnectMap.Insert( pszIp, iPort ) == false )
	{
		// �̹� TCP ���� ���� �߿� �����Ƿ� ���ο� TCP ���� ����õ����� �ʴ´�.
		pclsSipStack->m_clsTcpConnectMap.Insert( pszIp, iPort, pclsSipMessage );
		return true;
	}

	CSipTcpClientArg * pclsArg = new CSipTcpClientArg();
	if( pclsArg == NULL )
	{
		pclsSipStack->m_clsTcpConnectMap.Delete( pszIp, iPort );
		return false;
	}

	pclsArg->m_pclsSipStack = pclsSipStack;
	pclsArg->m_strIp = pszIp;
	pclsArg->m_iPort = iPort;
	pclsArg->m_pclsSipMessage = pclsSipMessage;

	// R5.b''': Via[0] host 가 유효하면 outbound source IP 로 bind
	if( pclsSipMessage && !pclsSipMessage->m_clsViaList.empty() )
	{
		pclsArg->m_strSourceIp = pclsSipMessage->m_clsViaList.front().m_strHost;
	}

	++pclsArg->m_pclsSipMessage->m_iUseCount;

	return StartThread( "SipTcpClientThread", SipTcpClientThread, pclsArg );
}

