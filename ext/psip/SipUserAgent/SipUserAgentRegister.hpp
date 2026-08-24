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

// SIP REGISTER 응답 메시지 수신 이벤트 핸들러
bool CSipUserAgent::RecvRegisterResponse( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_SERVER_INFO_LIST::iterator itSL;
	const char * pszUserId = pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str();
	bool bRes = false;

	m_clsRegisterMutex.acquire();
	for( itSL = m_clsRegisterList.begin(); itSL != m_clsRegisterList.end(); ++itSL )
	{
		if( !strcmp( itSL->m_strUserId.c_str(), pszUserId ) && !strcmp( itSL->m_strIp.c_str(), pclsMessage->m_strClientIp.c_str() ) )
		{
			int iStatusCode = pclsMessage->m_iStatusCode;

			// RFC 3329: 응답(401/494/421)의 Security-Server 원문을 보관 — 다음 REGISTER 의 Security-Verify.
			if( itSL->m_bSecAgree )
			{
				CSipHeader * pclsSecServer = pclsMessage->GetHeader( "Security-Server" );
				if( pclsSecServer ) itSL->m_strSecurityServer = pclsSecServer->m_strValue;
			}

			if( iStatusCode == SIP_OK )
			{
				if( itSL->m_iLoginTimeout == 0 )
				{
					itSL->ClearLogin();
					itSL->m_clsIpsec.Teardown( &m_clsSipStack );
				}
				else
				{
					itSL->m_clsIpsec.OnRegistered( &m_clsSipStack );
					itSL->m_bLogin = true;
					time( &itSL->m_iLoginTime );
					itSL->m_iResponseTime = itSL->m_iLoginTime;

					int iExpires = pclsMessage->GetExpires();
					if( iExpires > 0 && iExpires != itSL->m_iLoginTimeout )
					{
						itSL->m_iLoginTimeout = iExpires;
					}
				}

				if( m_pclsCallBack ) m_pclsCallBack->EventRegister( &(*itSL), iStatusCode );

				if( itSL->m_iLoginTimeout == 0 && itSL->m_bDelete )
				{
					m_clsRegisterList.erase( itSL );
				}
			}
			else if( iStatusCode == SIP_UNAUTHORIZED || iStatusCode == SIP_PROXY_AUTHENTICATION_REQUIRED )
			{
				if( itSL->m_bAuth && itSL->m_bAkaResyncSent == false )
				{
					// 인증 정보를 포함한 REGISTER 메시지에 대한 응답인 경우 로그인 오류 처리한다.
					goto CLEAR_LOGIN;
				}
				else
				{
					// (AKA) 직전 REGISTER 가 auts 를 실었으면 이 401 은 재동기 후의 새 챌린지다 (RFC 3310 §3.4)
					if( m_clsSipStack.m_clsSetup.m_bUseRegisterSession )
					{
						itSL->SetChallenge( pclsMessage );
					}

					CSipMessage * pclsRequest = itSL->CreateRegister( &m_clsSipStack, pclsMessage );
					if( pclsRequest ) m_clsSipStack.SendSipMessage( pclsRequest );
					else goto CLEAR_LOGIN;
				}
			}
			else if( iStatusCode == SIP_INTERVAL_TOO_BRIEF )
			{
				CSipHeader * pclsHeader = pclsMessage->GetHeader( "Min-Expires" );
				if( pclsHeader == NULL )
				{
					CLog::Print( LOG_ERROR, "Min-Expires header is not found" );
					goto CLEAR_LOGIN;
				}

				int iMinExpires = atoi( pclsHeader->m_strValue.c_str() );
				if( iMinExpires <= 0 )
				{
					CLog::Print( LOG_ERROR, "Min-Expires header's value is not correct" );
					goto CLEAR_LOGIN;
				}

				itSL->m_iLoginTimeout = iMinExpires;
				CSipMessage * pclsRequest = itSL->CreateRegister( &m_clsSipStack, pclsMessage );
				m_clsSipStack.SendSipMessage( pclsRequest );
			}
			else
			{
CLEAR_LOGIN:
				itSL->ClearLogin();
				itSL->m_clsIpsec.Teardown( &m_clsSipStack );
				time( &itSL->m_iNextSendTime );
				itSL->m_iNextSendTime += 60;
				if( m_pclsCallBack ) m_pclsCallBack->EventRegister( &(*itSL), iStatusCode );
			}

			bRes = true;
			break;
		}
	}
	m_clsRegisterMutex.release();

	return bRes;
}
