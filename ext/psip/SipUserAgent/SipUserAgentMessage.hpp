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

// SIP MESSAGE 요청 메시지 수신 이벤트 핸들러
bool CSipUserAgent::RecvMessageRequest( int iThreadId, CSipMessage * pclsMessage )
{
	int iStatus = SIP_DECLINE;

	if( m_pclsCallBack )
	{
		if( m_pclsCallBack->EventIncomingRequestAuth( pclsMessage ) == false )
		{
			return true;
		}

		iStatus = m_pclsCallBack->EventMessage( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str(), pclsMessage );
	}

	// 0 = 콜백이 스스로 응답을 보냈다. psip 가 또 보내면 최종 응답이 둘이 된다.
	if( iStatus > 0 )
	{
		m_clsSipStack.SendSipMessage( pclsMessage->CreateResponse( iStatus ) );
	}

	return true;
}
