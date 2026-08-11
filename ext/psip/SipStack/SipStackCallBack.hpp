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

// SIP stack 에 callback 인터페이스를 추가한다.
bool CSipStack::AddCallBack( ISipStackCallBack * pclsCallBack )
{
	if( pclsCallBack == NULL ) return false;

	SIP_STACK_CALLBACK_LIST::iterator	it;
	bool	bFound = false;

	for( it = m_clsCallBackList.begin(); it != m_clsCallBackList.end(); ++it )
	{
		if( *it == pclsCallBack )
		{
			bFound = true;
			break;
		}
	}

	if( bFound == false )
	{
		m_clsCallBackList.push_back( pclsCallBack );
	}

	return true;
}

// SIP stack 에 callback 인터페이스를 삭제한다.
bool CSipStack::DeleteCallBack( ISipStackCallBack * pclsCallBack )
{
	SIP_STACK_CALLBACK_LIST::iterator	it;
	bool	bFound = false;

	for( it = m_clsCallBackList.begin(); it != m_clsCallBackList.end(); ++it )
	{
		if( *it == pclsCallBack )
		{
			m_clsCallBackList.erase( it );
			bFound = true;
			break;
		}
	}

	return bFound;
}

// SIP stack 의 보안 기능을 수행할 callback 인터페이스를 등록한다.
void CSipStack::SetSecurityCallBack( ISipStackSecurityCallBack * pclsSecurityCallBack )
{
	m_pclsSecurityCallBack = pclsSecurityCallBack;
}

// 수신된 요청 SIP 메시지에 대한 callback 메소드를 호출한다. 만약 요청 SIP 메시지를 처리할 callback 이 존재하지 않으면 501 응답 메시지를 전송한다.
void CSipStack::RecvRequest( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;
	bool	bSendResponse = false;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		if( (*itList)->RecvRequest( iThreadId, pclsMessage ) )
		{
			bSendResponse = true;
			break;
		}
	}

	if( bSendResponse == false )
	{
		CSipMessage * psttResponse = pclsMessage->CreateResponseWithToTag( SIP_NOT_IMPLEMENTED );
		if( psttResponse )
		{
			SendSipMessage( psttResponse );
		}
	}
}

// 수신된 응답 SIP 메시지에 대한 callback 메소드를 호출한다.
void CSipStack::RecvResponse( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	// 수신 최종응답 카운터 — 와이어 응답(트랜잭션 dedup 후)과 트랜잭션 로컬 합성 응답
	//   (408 Timer B/Ring timeout, 660 connect error)이 전부 이 팬아웃을 지난다.
	//   합성 응답은 와이어에 없어 flow 로그 사각 — 성공률 집계에 여기 포함이 필수.
	if( pclsMessage->m_iStatusCode >= CSipStackCounter::FINAL_MIN )
		m_clsCounter.OnRecvFinal( pclsMessage->m_clsCSeq.m_strMethod.c_str(), pclsMessage->m_iStatusCode );

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		if( (*itList)->RecvResponse( iThreadId, pclsMessage ) ) break;
	}
}

// 전송 SIP 메시지에 대한 timeout callback 메소드를 호출한다.
void CSipStack::SendTimeout( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		if( (*itList)->SendTimeout( iThreadId, pclsMessage ) ) break;
	}
}

// TCP/TLS 세션 종료에 대한 callback 메소드를 호출한다.
void CSipStack::TcpSessionEnd( const char * pszIp, int iPort, ESipTransport eTransport )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		(*itList)->TcpSessionEnd( pszIp, iPort, eTransport );
	}
}

// 쓰레드 종료 이벤트를 전달한다.
void CSipStack::ThreadEnd( int iThreadId )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		(*itList)->ThreadEnd( iThreadId );
	}
}
