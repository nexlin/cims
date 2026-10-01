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

// 활성(port>0) 스트림 종류 — audio 1 · video 2 · application 4. re-INVITE 의 스트림 구성 변화(RFC 3264 §8.1 추가·§8.2 제거) 판정.
static int ReInviteStreamMask( CSipCallRtp & clsRtp )
{
	int iAudio = clsRtp.GetAudioPort();
	if( iAudio < 0 ) iAudio = clsRtp.m_iPort;
	return ( iAudio > 0 ? 1 : 0 ) | ( clsRtp.GetVideoPort() > 0 ? 2 : 0 ) | ( clsRtp.GetApplicationPort() > 0 ? 4 : 0 );
}

// SIP INVITE 요청 메시지 수신 이벤트 핸들러
bool CSipUserAgent::RecvInviteRequest( int iThreadId, CSipMessage * pclsMessage )
{
	std::string	strCallId, strLocalTag;
	bool	bReINVITE = false, bHeldBusy = false, bHeldRetransmit = false, bCloned = false;
	CSipCallRtp clsRtp, clsLocalRtp;
	char	szTag[SIP_TAG_MAX_SIZE];
	CSipMessage * pclsResponse = NULL;
	SIP_DIALOG_MAP::iterator itMap;

	if( pclsMessage->GetCallId( strCallId ) == false )
	{
		m_clsSipStack.SendSipMessage( pclsMessage->CreateResponse( SIP_BAD_REQUEST ) );
		return true;
	}

	if( GetSipCallRtp( pclsMessage, clsRtp ) == false )
	{
		pclsResponse = pclsMessage->CreateResponse( SIP_NOT_ACCEPTABLE_HERE );
		m_clsSipStack.SendSipMessage( pclsResponse );
		return true;
	}

	// 요청 세션 간격이 로컬 최소치 미만인가 — 거절할 요청은 갱신으로 계산하지 않는다 (§6/§9).
	bool bSessionTooSmall = SessionTimerIsTooSmall( pclsMessage );

	// ReINVITE 인지 검사한다.
	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( strCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		bReINVITE = true;
		strLocalTag = itMap->second.m_strFromTag;

		if( itMap->second.m_pclsHeldReInvite )
		{
			// 앞서 답을 미룬 re-INVITE 가 아직 답을 기다린다 — 같은 CSeq 면 재전송(트랜잭션 몫), 다른 요청이면 다이얼로그 상태를
			//   건드리지 않고 거절한다(아래 500).
			bHeldBusy = true;
			bHeldRetransmit = itMap->second.m_pclsHeldReInvite->m_clsCSeq.m_iDigit == pclsMessage->m_clsCSeq.m_iDigit;
		}
		else if( bSessionTooSmall == false )
		{
			// re-INVITE 는 target refresh 요청이다 — 받아들이는 요청의 Contact 로 remote target 을 바꾼다(RFC 3261 §12.2.2).
			SIP_FROM_LIST::iterator itReContact = pclsMessage->m_clsContactList.begin();
			if( itReContact != pclsMessage->m_clsContactList.end() )
			{
				char szUri[255];

				itReContact->m_clsUri.ToString( szUri, sizeof(szUri) );
				itMap->second.m_strContactUri = szUri;
			}

			// 미디어 무변경(순수 세션 갱신) 판정 — 반드시 SetRemoteRtp 로 덮어쓰기 전에 한다.
			//   방향 속성(a=sendonly/recvonly/inactive/sendrecv)만 바뀐 re-INVITE 도 미디어 변경이다
			//   (RFC 3264 §8.4 hold/resume) — 주소·포트만 비교하면 보류가 세션 갱신으로 오판된다.
			CSipCallRtp clsPrevRtp;
			itMap->second.SelectRemoteRtp( &clsPrevRtp );
			itMap->second.m_bLastReInviteMediaSame = ( clsPrevRtp.m_strIp == clsRtp.m_strIp &&
				clsPrevRtp.m_iPort == clsRtp.m_iPort &&
				clsPrevRtp.GetAudioPort() == clsRtp.GetAudioPort() &&
				clsPrevRtp.GetVideoPort() == clsRtp.GetVideoPort() &&
				clsPrevRtp.GetApplicationPort() == clsRtp.GetApplicationPort() &&
				clsPrevRtp.m_eDirection == clsRtp.m_eDirection );
			itMap->second.m_bLastReInviteStreamsChanged = ReInviteStreamMask( clsPrevRtp ) != ReInviteStreamMask( clsRtp );

			// 세션 갱신 (RFC 4028 §7.2) — 목적과 무관하게 in-dialog re-INVITE 는 갱신 효과를 갖는다.
			SessionTimerOnRequest( itMap->second, pclsMessage );
			itMap->second.m_iLastRefreshTime = time( NULL );

			itMap->second.SetRemoteRtp( &clsRtp );
			itMap->second.SelectLocalRtp( &clsLocalRtp );
			// answer 는 직전 로컬 선언 그대로가 기본이다 — SelectLocalRtp 가 옮기지 않는 합성 SDP 요소(floor
			//   m=application 포트·fmtp, 명시 video 포트, 코덱 목록)도 싣는다. 빠지면 아래 SetLocalRtp 가 이를 지워
			//   세션 갱신 answer 가 m=application 0(floor 거절, RFC 3264 §6)으로 나간다. 응용(EventReInvite)이 바꾸면 그 값.
			clsLocalRtp.m_iApplicationPort = itMap->second.m_iLocalApplicationPort;
			clsLocalRtp.m_strApplicationFmtp = itMap->second.m_strLocalApplicationFmtp;
			clsLocalRtp.m_iVideoPort = itMap->second.m_iLocalVideoPort;
			clsLocalRtp.m_eMcMediaProfile = itMap->second.m_eLocalMcMediaProfile;   // MCVideo 제어 채널·성분 표시 유지
			clsLocalRtp.m_clsCodecList = itMap->second.m_clsCodecList;
		}
	}
	m_clsDialogMutex.release();

	if( bReINVITE )
	{
		if( bHeldBusy )
		{
			// RFC 3261 §14.2 — 앞 INVITE 에 최종 응답을 보내기 전에 받은 다음 INVITE 는 500 + Retry-After(0~10 s)
			if( bHeldRetransmit == false )
			{
				pclsResponse = pclsMessage->CreateResponse( SIP_INTERNAL_SERVER_ERROR, strLocalTag.c_str() );
				if( pclsResponse )
				{
					pclsResponse->AddHeader( "Retry-After", rand() % 11 );
					m_clsSipStack.SendSipMessage( pclsResponse );
				}
			}
			return true;
		}

		if( bSessionTooSmall )
		{
			pclsResponse = pclsMessage->CreateResponse( SIP_SESSION_INTERVAL_TOO_SMALL, strLocalTag.c_str() );
			if( pclsResponse )
			{
				pclsResponse->AddHeader( "Min-SE", m_iSessionTimerMinSE );
				m_clsSipStack.SendSipMessage( pclsResponse );
			}
			return true;
		}

		// 응용이 이 re-INVITE 의 답을 미룰 수 있게(HoldReInviteAnswer — 상대 leg 의 답을 기다리는 B2BUA) 콜백 동안 사본을 둔다
		m_clsDialogMutex.acquire();
		itMap = m_clsDialogMap.find( strCallId );
		if( itMap != m_clsDialogMap.end() && itMap->second.m_pclsReInviteInProgress == NULL )
		{
			itMap->second.m_pclsReInviteInProgress = new CSipMessage();
			*itMap->second.m_pclsReInviteInProgress = *pclsMessage;
			bCloned = true;
		}
		m_clsDialogMutex.release();

		if( m_pclsCallBack ) m_pclsCallBack->EventReInvite( strCallId.c_str(), &clsRtp, &clsLocalRtp );

		m_clsDialogMutex.acquire();
		itMap = m_clsDialogMap.find( strCallId );
		if( itMap != m_clsDialogMap.end() && bCloned && itMap->second.m_pclsReInviteInProgress == NULL )
		{
			// 응용이 답을 미뤘다 — AnswerHeldReInvite 가 답한다(이미 답했을 수도 있다)
			m_clsDialogMutex.release();
			return true;
		}
		if( itMap != m_clsDialogMap.end() && bCloned )
		{
			delete itMap->second.m_pclsReInviteInProgress;
			itMap->second.m_pclsReInviteInProgress = NULL;
		}
		if( itMap != m_clsDialogMap.end() )
		{
			// 미디어가 같아도 응용이 answer 의 floor fmtp 를 다시 지었으면(TS 24.380 §14.3.1) SDP 가 바뀐 것이다 —
			//   o= 세션 버전을 올린다(RFC 3264 §8). 아무것도 바뀌지 않은 세션 갱신만 버전을 유지한다(RFC 4028 §7.4).
			const bool bKeepSdpVersion = itMap->second.m_bLastReInviteMediaSame &&
				itMap->second.m_strLocalApplicationFmtp == clsLocalRtp.m_strApplicationFmtp;
			itMap->second.SetLocalRtp( &clsLocalRtp );
			pclsResponse = pclsMessage->CreateResponse( SIP_OK );
			// 응답은 수신 요청에서 만든다 — 다이얼로그가 정한 Contact transport·파라미터(특성 태그)를 옮겨 싣는다.
			if( pclsResponse )
			{
				pclsResponse->m_iContactTransport = itMap->second.m_iContactTransport;
				pclsResponse->m_clsContactParams = itMap->second.m_clsContactParams;
				pclsResponse->m_clsContactUriParams = itMap->second.m_clsContactUriParams;
				for( const auto & clsHeader : itMap->second.m_vecNextReInviteAnswerHeaders )
				{
					pclsResponse->AddHeader( clsHeader.first.c_str(), clsHeader.second.c_str() );
				}
			}
			itMap->second.m_vecNextReInviteAnswerHeaders.clear();
			// 상대 offer 가 무변경(세션 갱신)이고 로컬 선언도 그대로면 answer 도 "변경 없음"으로 표시한다 —
			//   SDP origin(o=) 세션 버전을 유지한다 (RFC 4028 §7.4).
			itMap->second.AddSdp( pclsResponse, bKeepSdpVersion );

			// 갱신 응답에도 Session-Expires 를 실어야 한다 — 빠지면 상대가 타이머 해제로
			//   해석한다 (RFC 4028 §7.2).
			SessionTimerAddToResponse( itMap->second, pclsResponse );
		}
		m_clsDialogMutex.release();

		if( pclsResponse ) m_clsSipStack.SendSipMessage( pclsResponse );

		return true;
	}

	// 새로운 INVITE 인 경우
	SipMakeTag( szTag, sizeof(szTag) );

	// 요청 세션 간격이 로컬 최소치 미만이면 다이얼로그 생성 전에 422 + Min-SE (RFC 4028 §6/§9).
	if( bSessionTooSmall )
	{
		pclsResponse = pclsMessage->CreateResponse( SIP_SESSION_INTERVAL_TOO_SMALL, szTag );
		if( pclsResponse )
		{
			pclsResponse->AddHeader( "Min-SE", m_iSessionTimerMinSE );
			m_clsSipStack.SendSipMessage( pclsResponse );
		}
		return true;
	}

	if( m_pclsCallBack )
	{
		if( m_pclsCallBack->EventIncomingRequestAuth( pclsMessage ) == false )
		{
			return true;
		}
	}

	// 100 Trying: SIP 스택 IST(SipISTList)가 자동 전송하므로 m_bSendTrying=true이면 중복 방지
	if( !m_clsSipStack.m_clsSetup.m_bSendTrying )
	{
		pclsResponse = pclsMessage->CreateResponse( SIP_TRYING );
		if( pclsResponse ) m_clsSipStack.SendSipMessage( pclsResponse );
		pclsResponse = NULL;
	}

	// Dialog 를 생성한다.
	CSipDialog	clsDialog( &m_clsSipStack );
	bool bError = false;

	clsDialog.m_strFromId = pclsMessage->m_clsTo.m_clsUri.m_strUser;
	clsDialog.m_strFromTag = szTag;
	clsDialog.m_eTransport = pclsMessage->m_eTransport;

	clsDialog.m_strToId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
	pclsMessage->m_clsFrom.SelectParam( SIP_TAG, clsDialog.m_strToTag );

	clsDialog.m_strCallId = strCallId;
	clsDialog.SetRemoteRtp( &clsRtp );

	// 세션 타이머 협상 입력 보관 (RFC 4028 §9) — 확정은 AcceptCall 의 2xx 생성 시점.
	SessionTimerOnRequest( clsDialog, pclsMessage );

	pclsMessage->GetTopViaIpPort( clsDialog.m_strContactIp, clsDialog.m_iContactPort );

	SIP_FROM_LIST::iterator	itContact = pclsMessage->m_clsContactList.begin();
	if( itContact != pclsMessage->m_clsContactList.end() )
	{
		char	szUri[255];

		itContact->m_clsUri.ToString( szUri, sizeof(szUri) );
		clsDialog.m_strContactUri = szUri;
	}

	gettimeofday( &clsDialog.m_sttInviteTime, NULL );

	clsDialog.m_pclsInvite = new CSipMessage();
	if( clsDialog.m_pclsInvite )
	{
		*clsDialog.m_pclsInvite = *pclsMessage;
		clsDialog.m_pclsInvite->m_clsTo.InsertParam( SIP_TAG, szTag );

		if( clsDialog.m_pclsInvite->m_clsRecordRouteList.size() > 0 )
		{
			clsDialog.m_clsRouteList = clsDialog.m_pclsInvite->m_clsRecordRouteList;
		}
	}
	clsDialog.m_bSendCall = false;

	// Dialog 를 저장한다.
	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( strCallId );
	if( itMap == m_clsDialogMap.end() )
	{
		m_clsDialogMap.insert( SIP_DIALOG_MAP::value_type( strCallId, clsDialog ) );
	}
	else
	{
		bError = true;
	}
	m_clsDialogMutex.release();

	if( bError )
	{
		if( clsDialog.m_pclsInvite )
		{
			delete clsDialog.m_pclsInvite;
		}
	}
	else
	{
		if( m_pclsCallBack )
		{
			m_pclsCallBack->EventIncomingCall( strCallId.c_str(), pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str()
				, pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str(), &clsRtp, pclsMessage );
		}
	}

	return true;
}

// SIP INVITE 응답 메시지 수신 이벤트 핸들러
bool CSipUserAgent::RecvInviteResponse( int iThreadId, CSipMessage * pclsMessage )
{
	if( pclsMessage->m_iStatusCode == SIP_TRYING ) return true;

	CSipCallRtp clsRtp;
	bool bRtp = false, bReInvite = false, bRefreshResponse = false;
	std::string	strCallId;

	pclsMessage->GetCallId( strCallId );

	if( GetSipCallRtp( pclsMessage, clsRtp ) ) bRtp = true;

	if( SetInviteResponse( strCallId, pclsMessage, bRtp ? &clsRtp : NULL, bReInvite, bRefreshResponse ) )
	{
		if( bReInvite )
		{
			// 세션 갱신(스택이 보낸 re-INVITE)의 응답은 응용의 re-INVITE 결과가 아니다 — 응용이 미뤄 둔 다른 leg 의 답으로 읽지 않게
			if( m_pclsCallBack && bRefreshResponse == false )
				m_pclsCallBack->EventReInviteResponse( strCallId.c_str(), pclsMessage->m_iStatusCode, bRtp ? &clsRtp : NULL );
		}
		else
		{
			if( m_pclsCallBack ) m_pclsCallBack->EventInviteResponse( strCallId.c_str(), pclsMessage );
			if( pclsMessage->m_iStatusCode > SIP_TRYING && pclsMessage->m_iStatusCode < SIP_OK )
			{
				if( m_pclsCallBack ) m_pclsCallBack->EventCallRing( strCallId.c_str(), pclsMessage->m_iStatusCode, bRtp ? &clsRtp : NULL );
			}
			else if( pclsMessage->m_iStatusCode >= SIP_OK && pclsMessage->m_iStatusCode < SIP_MULTIPLE_CHOICES )
			{
				if( m_pclsCallBack ) m_pclsCallBack->EventCallStart( strCallId.c_str(), bRtp ? &clsRtp : NULL );
			}
			else
			{
				CSipHeader * pclsReason = pclsMessage->GetHeader( "Reason" );
				if( m_pclsCallBack ) m_pclsCallBack->EventCallEnd( strCallId.c_str(), pclsMessage->m_iStatusCode, pclsReason ? pclsReason->m_strValue.c_str() : NULL );

				Delete( strCallId.c_str() );
			}
		}
	}

	return true;
}

// ── 답을 미룬 re-INVITE (B2BUA — 상대 leg 의 답을 기다린다, RFC 3261 §14.2) ──

bool CSipUserAgent::IsStreamSetChangeReInvite( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszCallId == NULL ) return false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() ) bRes = itMap->second.m_bLastReInviteStreamsChanged;
	m_clsDialogMutex.release();

	return bRes;
}

bool CSipUserAgent::HoldReInviteAnswer( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszCallId == NULL ) return false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() && itMap->second.m_pclsReInviteInProgress && itMap->second.m_pclsHeldReInvite == NULL )
	{
		itMap->second.m_pclsHeldReInvite = itMap->second.m_pclsReInviteInProgress;
		itMap->second.m_pclsReInviteInProgress = NULL;
		itMap->second.m_iHeldReInviteTime = time( NULL );
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

bool CSipUserAgent::HasHeldReInvite( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszCallId == NULL ) return false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() ) bRes = itMap->second.m_pclsHeldReInvite != NULL;
	m_clsDialogMutex.release();

	return bRes;
}

bool CSipUserAgent::AnswerHeldReInvite( const char * pszCallId, int iStatus, CSipCallRtp * pclsLocalRtp )
{
	SIP_DIALOG_MAP::iterator	itMap;
	CSipMessage * pclsRequest = NULL, * pclsResponse = NULL;

	if( pszCallId == NULL ) return false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() && itMap->second.m_pclsHeldReInvite )
	{
		pclsRequest = itMap->second.m_pclsHeldReInvite;
		itMap->second.m_pclsHeldReInvite = NULL;

		if( iStatus >= SIP_OK && iStatus < SIP_MULTIPLE_CHOICES && pclsLocalRtp )
		{
			itMap->second.SetLocalRtp( pclsLocalRtp );
			pclsResponse = pclsRequest->CreateResponse( iStatus );
			if( pclsResponse )
			{
				// 자동 200 과 같다 — 다이얼로그 Contact transport·파라미터, 한 번 싣는 헤더, 세션 타이머(RFC 4028 §7.2)
				pclsResponse->m_iContactTransport = itMap->second.m_iContactTransport;
				pclsResponse->m_clsContactParams = itMap->second.m_clsContactParams;
				pclsResponse->m_clsContactUriParams = itMap->second.m_clsContactUriParams;
				for( const auto & clsHeader : itMap->second.m_vecNextReInviteAnswerHeaders )
				{
					pclsResponse->AddHeader( clsHeader.first.c_str(), clsHeader.second.c_str() );
				}
				itMap->second.AddSdp( pclsResponse );
				SessionTimerAddToResponse( itMap->second, pclsResponse );
			}
			itMap->second.m_vecNextReInviteAnswerHeaders.clear();
		}
		else
		{
			// 실패는 세션을 바꾸지 않는다(RFC 3261 §14.2) — 상대 leg 의 최종 응답 코드를 그대로, 코드가 없으면 500
			pclsResponse = pclsRequest->CreateResponse( iStatus >= SIP_MULTIPLE_CHOICES ? iStatus : SIP_INTERNAL_SERVER_ERROR );
		}
	}
	m_clsDialogMutex.release();

	if( pclsRequest ) delete pclsRequest;
	if( pclsResponse == NULL ) return false;

	m_clsSipStack.SendSipMessage( pclsResponse );
	return true;
}

void CSipUserAgent::CheckHeldReInvite( int iMaxSec )
{
	SIP_DIALOG_MAP::iterator	itMap;
	std::list< CSipMessage * >	clsResponseList;
	std::list< std::string >		clsCallIdList;
	time_t	iNow = time( NULL );

	m_clsDialogMutex.acquire();
	for( itMap = m_clsDialogMap.begin(); itMap != m_clsDialogMap.end(); ++itMap )
	{
		CSipDialog & clsDialog = itMap->second;

		if( clsDialog.m_pclsHeldReInvite == NULL || iNow - clsDialog.m_iHeldReInviteTime < iMaxSec ) continue;

		CSipMessage * pclsResponse = clsDialog.m_pclsHeldReInvite->CreateResponse( SIP_INTERNAL_SERVER_ERROR );
		delete clsDialog.m_pclsHeldReInvite;
		clsDialog.m_pclsHeldReInvite = NULL;
		if( pclsResponse ) clsResponseList.push_back( pclsResponse );
		clsCallIdList.push_back( itMap->first );
	}
	m_clsDialogMutex.release();

	for( const auto & strCallId : clsCallIdList )
	{
		CLog::Print( LOG_INFO, "held re-INVITE expired: CallId(%s) — 상대 leg 무응답, 500", strCallId.c_str() );
	}
	for( auto * pclsResponse : clsResponseList ) m_clsSipStack.SendSipMessage( pclsResponse );
}
