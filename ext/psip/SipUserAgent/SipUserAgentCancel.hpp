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

// SIP CANCEL 요청 메시지 수신 이벤트 핸들러
//
// CANCEL 은 인증 챌린지 대상이 아니다 — RFC 3261 §22.1 "servers MUST NOT attempt to challenge CANCEL requests since
//   these requests cannot be resubmitted". 대신 §9.1/§17.2.3 대로 취소 대상 INVITE 와 **같은 트랜잭션**(최상위 Via 의
//   sent-by 와 branch 일치)에서 온 것만 받아들이고, 대응 INVITE 가 없거나 다른 곳에서 온 CANCEL 은 481 (§9.2).
//   종전에는 응용 인증 훅(EventIncomingRequestAuth)을 거쳐 UDP 등록 단말의 승격 TCP flow 에서 온 CANCEL 이
//   401 을 받았고, 대응 INVITE 가 없어도 200 OK 를 돌려줬다.
static bool CancelMatchesInvite( CSipMessage * pclsCancel, CSipMessage * pclsInvite )
{
	if( pclsCancel == NULL || pclsInvite == NULL ) return false;
	SIP_VIA_LIST::iterator itC = pclsCancel->m_clsViaList.begin();
	SIP_VIA_LIST::iterator itI = pclsInvite->m_clsViaList.begin();
	if( itC == pclsCancel->m_clsViaList.end() || itI == pclsInvite->m_clsViaList.end() ) return false;
	if( itC->m_strHost != itI->m_strHost || itC->m_iPort != itI->m_iPort ) return false;
	std::string strBranchC, strBranchI;
	itC->SelectParam( SIP_BRANCH, strBranchC );
	itI->SelectParam( SIP_BRANCH, strBranchI );
	return strBranchC.empty() == false && strBranchC == strBranchI;
}

bool CSipUserAgent::RecvCancelRequest( int iThreadId, CSipMessage * pclsMessage )
{
	std::string strCallId;

	if( pclsMessage->GetCallId( strCallId ) == false )
	{
		m_clsSipStack.SendSipMessage( pclsMessage->CreateResponse( SIP_BAD_REQUEST ) );
		return true;
	}

	CSipMessage * pclsResponse = NULL;
	CSipMessage * pclsInviteResponse = NULL;
	SIP_DIALOG_MAP::iterator		itMap;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( strCallId );
	if( itMap == m_clsDialogMap.end() || CancelMatchesInvite( pclsMessage, itMap->second.m_pclsInvite ) == false )
	{
		pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
	}
	else
	{
		pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_OK );
		if( itMap->second.m_sttStartTime.tv_sec == 0 )
		{
			pclsInviteResponse = itMap->second.m_pclsInvite->CreateResponse( SIP_REQUEST_TERMINATED );
			gettimeofday( &itMap->second.m_sttEndTime, NULL );
		}
	}
	m_clsDialogMutex.release();

	if( pclsResponse )
	{
		m_clsSipStack.SendSipMessage( pclsResponse );
		pclsResponse = NULL;
	}

	if( pclsInviteResponse )
	{
		m_clsSipStack.SendSipMessage( pclsInviteResponse );
		CSipHeader * pclsReason = pclsMessage->GetHeader( "Reason" );
		if( m_pclsCallBack ) m_pclsCallBack->EventCallEnd( strCallId.c_str(), SIP_REQUEST_TERMINATED, pclsReason ? pclsReason->m_strValue.c_str() : NULL );

		Delete( strCallId.c_str() );
	}

	return true;
}
