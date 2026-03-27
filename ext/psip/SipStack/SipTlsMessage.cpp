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

#include "SipStackDefine.h"
#include "SipTlsMessage.h"
#include "Log.h"
#include "MemoryDebug.h"

#ifdef USE_TLS

/**
 * @brief SIP �޽����� TCP ���ǿ� �����ϰ� ������ ��, TCP �������� �����Ѵ�.
 * @param hSocket TCP ����
 * @param psttSsl SSL ����
 * @param pszIp		������ IP �ּ�
 * @param iPort		������ ��Ʈ ��ȣ
 * @param pclsMessage SIP �޽���
 * @param iLocalTlsPort ���� TLS ��Ʈ ��ȣ. ���� TLS listen port ��ȣ�� Contact �� ����ϴ� ��쿡 �Է��Ѵ�.
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool SipTlsSend( Socket hSocket, SSL * psttSsl, const char * pszIp, int iPort, CSipMessage * pclsMessage, int iLocalTlsPort )
{
	std::string	strTcpIp;
	int iTcpPort;
	bool bRes = false;

	if( iLocalTlsPort > 0 )
	{
		if( pclsMessage->IsRequest() )
		{
			// LG IP-PBX �� REGISTER �� ��, INVITE �� �����ϱ� ���ؼ��� listen port �� ����ؾ� �Ǿ ������.
			if( pclsMessage->SetTopViaTransPort( E_SIP_TLS, iLocalTlsPort ) )
			{
				pclsMessage->MakePacket();
			}
		}
	}
	else
	{
		if( GetLocalIpPort( hSocket, strTcpIp, iTcpPort ) == false )
		{
			CLog::Print( LOG_ERROR, "GetLocalIpPort(%d) error(%d)", hSocket, GetError() );
			return false;	
		}
		else
		{
			bool bVia = false, bContact = false;
			
			if( pclsMessage->IsRequest() )
			{
				bVia = pclsMessage->SetTopViaIpPort( strTcpIp.c_str(), iTcpPort, E_SIP_TLS );
			}

			bContact = pclsMessage->SetTopContactIpPort( strTcpIp.c_str(), iTcpPort, E_SIP_TLS );

			if( bVia || bContact )
			{
				pclsMessage->MakePacket();
			}
		}
	}

	int iPacketLen = (int)pclsMessage->m_strPacket.length();

	if( SSLSend( psttSsl, pclsMessage->m_strPacket.c_str(), iPacketLen ) == iPacketLen )
	{
		CLog::Print( LOG_NETWORK, "TlsSend(%s:%d) \n[%s]", pszIp, iPort, pclsMessage->m_strPacket.c_str() );
		bRes = true;
	}
	else
	{
		CLog::Print( LOG_NETWORK, "TlsSend(%s:%d) \n[%s] error(%d)", pszIp, iPort, pclsMessage->m_strPacket.c_str(), GetError() );
	}

	return bRes;
}

#endif
