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

/**
 * @ingroup SipUserAgent
 * @brief text ��� SMS �޽����� �����Ѵ�.
 * @param pszFrom		�߽��� ���̵�
 * @param pszTo			������ ���̵�
 * @param pszText		SMS �޽���
 * @param pclsRoute SIP �޽��� ������ �ּ� ���� ��ü
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipUserAgent::SendSms( const char * pszFrom, const char * pszTo, const char * pszText, CSipCallRoute * pclsRoute )
{
	return SendSms( pszFrom, pszTo, pszText, pclsRoute, NULL );
}

/**
 * @ingroup SipUserAgent
 * @brief SMS message with explicit Content-Type (NULL => text/plain).
 * @param pszContentType full Content-Type value incl. parameters (e.g. "multipart/mixed;boundary=x")
 */
bool CSipUserAgent::SendSms( const char * pszFrom, const char * pszTo, const char * pszText, CSipCallRoute * pclsRoute, const char * pszContentType )
{
	CSipMessage * pclsRequest = new CSipMessage();
	if( pclsRequest == NULL ) return false;

	pclsRequest->m_strSipMethod = SIP_METHOD_MESSAGE;
	pclsRequest->m_clsReqUri.Set( SIP_PROTOCOL, pszTo, m_clsSipStack.m_clsSetup.m_strLocalIp.c_str(), m_clsSipStack.m_clsSetup.m_iLocalUdpPort );

	pclsRequest->m_clsFrom.m_clsUri.Set( SIP_PROTOCOL, pszFrom, m_clsSipStack.m_clsSetup.m_strLocalIp.c_str(), m_clsSipStack.m_clsSetup.m_iLocalUdpPort );
	pclsRequest->m_clsFrom.InsertTag();

	pclsRequest->m_clsTo.m_clsUri.Set( SIP_PROTOCOL, pszTo, m_clsSipStack.m_clsSetup.m_strLocalIp.c_str(), m_clsSipStack.m_clsSetup.m_iLocalUdpPort );

	pclsRequest->m_clsCSeq.m_iDigit = GetSeqNum();
	pclsRequest->m_clsCSeq.m_strMethod = pclsRequest->m_strSipMethod;

	pclsRequest->AddRoute( pclsRoute->m_strDestIp.c_str(), pclsRoute->m_iDestPort, pclsRoute->m_eTransport );
	pclsRequest->m_clsCallId.Make( m_clsSipStack.m_clsSetup.m_strLocalIp.c_str() );

	if( pszContentType && pszContentType[0] )
	{
		if( pclsRequest->m_clsContentType.Parse( pszContentType, (int)strlen( pszContentType ) ) == -1 )
		{
			pclsRequest->m_clsContentType.Set( "text", "plain" );
		}
	}
	else
	{
		pclsRequest->m_clsContentType.Set( "text", "plain" );
	}
	pclsRequest->m_strBody = pszText;
	pclsRequest->m_iContentLength = (int)pclsRequest->m_strBody.length();

	if( m_clsSipStack.SendSipMessage( pclsRequest ) == false ) return false;

	return true;
}
