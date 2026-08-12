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

// SIP 세션 타이머 (RFC 4028) — BYE 없이 사라진 leg 을 시한으로 회수한다.
//   설계 정본: docs/design/features/leg_liveness.md
//   갱신 주체(refresher)를 다이얼로그마다 협상하고, 우리가 갱신자면 세션 간격의 절반마다
//   in-dialog re-INVITE 를 보내며(무응답 = SendTimeout → EventCallEnd), 상대가 갱신자면
//   만료 직전까지 갱신이 오지 않을 때 BYE 로 세션을 끊는다.

// 헤더(Supported/Require)의 콤마 구분 옵션 태그 목록에 pszOption 이 있는가.
bool CSipUserAgent::SessionTimerHasOption( CSipMessage * pclsMessage, const char * pszHeader, const char * pszOption )
{
	if( pclsMessage == NULL || pszHeader == NULL || pszOption == NULL ) return false;

	SIP_HEADER_LIST::iterator	itList;

	for( itList = pclsMessage->m_clsHeaderList.begin(); itList != pclsMessage->m_clsHeaderList.end(); ++itList )
	{
		if( strcasecmp( itList->m_strName.c_str(), pszHeader ) ) continue;

		const std::string & strValue = itList->m_strValue;
		size_t	iPos = 0;

		while( iPos <= strValue.length() )
		{
			size_t iEnd = strValue.find( ',', iPos );
			std::string strToken = ( iEnd == std::string::npos ) ? strValue.substr( iPos ) : strValue.substr( iPos, iEnd - iPos );

			size_t iHead = strToken.find_first_not_of( " \t" );
			size_t iTail = strToken.find_last_not_of( " \t" );
			if( iHead != std::string::npos )
			{
				strToken = strToken.substr( iHead, iTail - iHead + 1 );
				if( !strcasecmp( strToken.c_str(), pszOption ) ) return true;
			}

			if( iEnd == std::string::npos ) break;
			iPos = iEnd + 1;
		}
	}

	return false;
}

// Session-Expires 헤더(compact form "x" 포함) 파싱. 값(초)을 리턴하고 refresher 파라미터를
//   strRefresher 에 담는다 (없으면 빈 문자열). 헤더가 없으면 0.
int CSipUserAgent::SessionTimerGetExpires( CSipMessage * pclsMessage, std::string & strRefresher )
{
	strRefresher.clear();

	if( pclsMessage == NULL ) return 0;

	CSipHeader * pclsHeader = pclsMessage->GetHeader( "Session-Expires" );
	if( pclsHeader == NULL ) pclsHeader = pclsMessage->GetHeader( "x" );
	if( pclsHeader == NULL || pclsHeader->m_strValue.empty() ) return 0;

	const std::string & strValue = pclsHeader->m_strValue;
	int iExpires = atoi( strValue.c_str() );

	size_t iPos = strValue.find( "refresher" );
	if( iPos != std::string::npos )
	{
		iPos = strValue.find( '=', iPos );
		if( iPos != std::string::npos )
		{
			++iPos;
			while( iPos < strValue.length() && ( strValue[iPos] == ' ' || strValue[iPos] == '\t' ) ) ++iPos;
			size_t iEnd = strValue.find_first_of( " \t;", iPos );
			strRefresher = ( iEnd == std::string::npos ) ? strValue.substr( iPos ) : strValue.substr( iPos, iEnd - iPos );
		}
	}

	return iExpires;
}

// Min-SE 헤더 값(초). 없으면 0 (RFC 4028 §5 상 기본은 90 이나, "명시되지 않음"을 구분한다).
int CSipUserAgent::SessionTimerGetMinSE( CSipMessage * pclsMessage )
{
	if( pclsMessage == NULL ) return 0;

	CSipHeader * pclsHeader = pclsMessage->GetHeader( "Min-SE" );
	if( pclsHeader == NULL || pclsHeader->m_strValue.empty() ) return 0;

	return atoi( pclsHeader->m_strValue.c_str() );
}

// 수신 INVITE/re-INVITE 의 세션 타이머 협상 입력을 다이얼로그에 보관한다 (UAS 역할).
void CSipUserAgent::SessionTimerOnRequest( CSipDialog & clsDialog, CSipMessage * pclsMessage )
{
	if( m_bSessionTimer == false ) return;

	std::string strRefresher;

	clsDialog.m_bPeerSupportsTimer = SessionTimerHasOption( pclsMessage, "Supported", "timer" ) ||
	                                 SessionTimerHasOption( pclsMessage, "Require", "timer" );
	clsDialog.m_iPeerSessionExpires = SessionTimerGetExpires( pclsMessage, strRefresher );
	clsDialog.m_strPeerRefresher = strRefresher;

	int iMinSE = SessionTimerGetMinSE( pclsMessage );
	if( iMinSE > clsDialog.m_iPeerMinSE ) clsDialog.m_iPeerMinSE = iMinSE;
}

// 수신 요청의 Session-Expires 가 로컬 최소치보다 작은가 (422 판정, RFC 4028 §9).
bool CSipUserAgent::SessionTimerIsTooSmall( CSipMessage * pclsMessage )
{
	if( m_bSessionTimer == false ) return false;

	std::string strRefresher;
	int iExpires = SessionTimerGetExpires( pclsMessage, strRefresher );

	return ( iExpires > 0 && iExpires < m_iSessionTimerMinSE );
}

// 2xx 응답에 Session-Expires/Require 를 싣고 다이얼로그 타이머를 확정한다 (UAS 역할).
//   RFC 4028 §9 — 응답 값은 요청 값보다 키울 수 없고 요청 Min-SE(없으면 90) 미만으로 줄일 수 없다.
void CSipUserAgent::SessionTimerAddToResponse( CSipDialog & clsDialog, CSipMessage * pclsResponse )
{
	if( m_bSessionTimer == false || pclsResponse == NULL ) return;

	int iExpires = m_iSessionTimerSE;

	// 상대가 제안한 간격이 더 짧으면 그 값을 쓴다 (키우기 금지).
	if( clsDialog.m_iPeerSessionExpires > 0 && clsDialog.m_iPeerSessionExpires < iExpires )
	{
		iExpires = clsDialog.m_iPeerSessionExpires;
	}

	int iFloor = clsDialog.m_iPeerMinSE > 0 ? clsDialog.m_iPeerMinSE : SIP_SESSION_TIMER_ABS_MIN;
	if( iExpires < iFloor ) iExpires = iFloor;

	// refresher 선정 (§9 Table 2) — 상대가 지정했으면 뒤집을 수 없다.
	bool bLocal;
	if( clsDialog.m_bPeerSupportsTimer == false )
	{
		bLocal = true;																					// 미지원 UAC → uas 강제
	}
	else if( !strcasecmp( clsDialog.m_strPeerRefresher.c_str(), "uac" ) )
	{
		bLocal = false;																					// 상대(UAC)가 갱신
	}
	else if( !strcasecmp( clsDialog.m_strPeerRefresher.c_str(), "uas" ) )
	{
		bLocal = true;
	}
	else
	{
		bLocal = ( m_iSessionTimerRefresher == E_SESSION_REFRESHER_LOCAL );		// 미지정 → 로컬 정책
	}

	char	szValue[64];
	snprintf( szValue, sizeof(szValue), "%d;refresher=%s", iExpires, bLocal ? "uas" : "uac" );
	pclsResponse->AddHeader( "Session-Expires", szValue );

	// refresher=uac 면 MUST, uas 이고 상대가 timer 를 지원하면 SHOULD (§9).
	if( bLocal == false || clsDialog.m_bPeerSupportsTimer )
	{
		pclsResponse->AddHeader( "Require", "timer" );
	}

	clsDialog.m_iSessionExpires = iExpires;
	clsDialog.m_bLocalRefresher = bLocal;
	clsDialog.m_iLastRefreshTime = time( NULL );
	clsDialog.m_iRefreshSentTime = 0;
}

// 송신 INVITE(초기/갱신)에 Supported/Session-Expires/Min-SE 를 싣는다 (UAC 역할).
//   §7.1 — timer 를 지원하는 UAC 는 ACK 를 제외한 모든 요청에 Supported: timer 를 실어야 한다.
void CSipUserAgent::SessionTimerAddToRequest( CSipDialog & clsDialog, CSipMessage * pclsRequest, bool bInitial )
{
	if( m_bSessionTimer == false || pclsRequest == NULL ) return;

	pclsRequest->AddHeader( "Supported", "timer" );

	int iMinSE = m_iSessionTimerMinSE;
	if( clsDialog.m_iPeerMinSE > iMinSE ) iMinSE = clsDialog.m_iPeerMinSE;

	int iExpires = ( clsDialog.m_iSessionExpires > 0 ) ? clsDialog.m_iSessionExpires : m_iSessionTimerSE;
	if( iExpires < iMinSE ) iExpires = iMinSE;

	// 초기 요청은 로컬 정책대로 갱신자를 제안하고, 갱신 요청은 현재 역할을 유지한다(§7.4).
	bool bLocal = bInitial ? ( m_iSessionTimerRefresher == E_SESSION_REFRESHER_LOCAL ) : clsDialog.m_bLocalRefresher;

	char	szValue[64];
	snprintf( szValue, sizeof(szValue), "%d;refresher=%s", iExpires, bLocal ? "uac" : "uas" );
	pclsRequest->AddHeader( "Session-Expires", szValue );
	pclsRequest->AddHeader( "Min-SE", iMinSE );

	if( bInitial )
	{
		// 응답이 Session-Expires 를 싣지 않으면(상대 미지원) 이 제안값으로 우리가 갱신한다(§7.2).
		clsDialog.m_iSessionExpires = iExpires;
		clsDialog.m_bLocalRefresher = bLocal;
		clsDialog.m_iLastRefreshTime = time( NULL );
	}
}

// INVITE 2xx 응답 수신 시 세션 타이머 상태를 갱신한다 (UAC 역할, 초기·갱신 공통).
void CSipUserAgent::SessionTimerOnResponse( CSipDialog & clsDialog, CSipMessage * pclsMessage )
{
	if( m_bSessionTimer == false ) return;

	std::string strRefresher;
	int iExpires = SessionTimerGetExpires( pclsMessage, strRefresher );

	if( iExpires > 0 )
	{
		clsDialog.m_iSessionExpires = iExpires;
		// refresher=uac = 요청을 보낸 우리가 갱신 (미표기는 uac 로 간주 — §7.2)
		clsDialog.m_bLocalRefresher = strcasecmp( strRefresher.c_str(), "uas" ) ? true : false;
	}
	else if( clsDialog.m_bLocalRefresher == false )
	{
		// 상대가 갱신자였는데 응답에서 타이머가 빠졌다 → 타이머 해제(§7.2).
		clsDialog.m_iSessionExpires = 0;
	}
	// 우리가 갱신자인데 응답에 헤더가 없으면(상대 미지원) 제안값을 유지한다 — §7.2 의
	//   "UAS 미지원 시 UAC 가 갱신을 수행한다" 규정을 갱신 트랜잭션에도 그대로 적용한다.

	clsDialog.m_iLastRefreshTime = time( NULL );
	clsDialog.m_iRefreshSentTime = 0;

	if( clsDialog.m_iSessionExpires > 0 )
	{
		CLog::Print( LOG_DEBUG, "SessionTimer(%s): se=%d refresher=%s", clsDialog.m_strCallId.c_str(),
			clsDialog.m_iSessionExpires, clsDialog.m_bLocalRefresher ? "local" : "remote" );
	}
}

/**
 * @ingroup SipUserAgent
 * @brief 세션 타이머 설정. CSP 등 호출자가 기동 시 1회 설정한다.
 * @param bEnable					세션 타이머 사용 여부
 * @param iSessionExpires	제안 세션 간격(초). 90 미만은 90 으로 clamp (RFC 4028 §4)
 * @param iMinSE					로컬 최소 간격(초). 이보다 작은 요청은 422 로 거절
 * @param iRefresher			E_SESSION_REFRESHER_LOCAL(기본) / E_SESSION_REFRESHER_REMOTE
 */
void CSipUserAgent::SetSessionTimer( bool bEnable, int iSessionExpires, int iMinSE, int iRefresher )
{
	if( iSessionExpires < SIP_SESSION_TIMER_ABS_MIN ) iSessionExpires = SIP_SESSION_TIMER_ABS_MIN;
	if( iMinSE < SIP_SESSION_TIMER_ABS_MIN ) iMinSE = SIP_SESSION_TIMER_ABS_MIN;
	if( iSessionExpires < iMinSE ) iSessionExpires = iMinSE;

	m_bSessionTimer = bEnable;
	m_iSessionTimerSE = iSessionExpires;
	m_iSessionTimerMinSE = iMinSE;
	m_iSessionTimerRefresher = iRefresher;

	CLog::Print( LOG_INFO, "SessionTimer: enable=%d se=%d min_se=%d refresher=%s", bEnable ? 1 : 0,
		iSessionExpires, iMinSE, iRefresher == E_SESSION_REFRESHER_LOCAL ? "server" : "ue" );
}

// 서버 발신 in-dialog 요청의 목적지를 응용이 아는 **현재 등록 주소**로 갱신한다 (호출자가
//   m_clsDialogMutex 보유). 다이얼로그가 기억한 주소는 요청 수신 당시의 소스라, NAT 뒤
//   단말에서는 이미 죽어 있을 수 있다 — 대형 INVITE 를 TCP 로 승격해 보낸 뒤 그 연결이
//   닫히면, 그 주소로는 서버가 다시 연결할 수 없다(인바운드 불가) → 갱신 미도달 → 단말이
//   규격대로 세션을 끊는다(RFC 4028 §10). docs/design/features/leg_liveness.md §6.3.
//   Record-Route 가 있는(중간 프록시 경유) 다이얼로그는 손대지 않는다.
void CSipUserAgent::SessionTimerApplyDest( const std::string & strCallId, CSipDialog & clsDialog,
	const std::string & strIp, int iPort, ESipTransport eTransport )
{
	if( strIp.empty() || iPort <= 0 ) return;

	if( strIp != clsDialog.m_strContactIp || iPort != clsDialog.m_iContactPort ||
		eTransport != clsDialog.m_eTransport )
	{
		CLog::Print( LOG_DEBUG, "SessionTimer dest(%s): %s:%d(%d) → %s:%d(%d)", strCallId.c_str(),
			clsDialog.m_strContactIp.c_str(), clsDialog.m_iContactPort, clsDialog.m_eTransport,
			strIp.c_str(), iPort, eTransport );
	}

	clsDialog.m_strContactIp = strIp;
	clsDialog.m_iContactPort = iPort;
	clsDialog.m_eTransport   = eTransport;
}

/**
 * @ingroup SipUserAgent
 * @brief 직전 수신 re-INVITE 가 미디어 무변경이었는가 — 순수 세션 갱신이면 호출자가
 *        미디어 재협상(미디어 서버 재호출·NAT 재평가)을 생략할 수 있다.
 * @param pszCallId SIP Call-ID
 */
bool CSipUserAgent::IsSessionRefreshReInvite( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszCallId == NULL ) return false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		bRes = itMap->second.m_bLastReInviteMediaSame;
	}
	m_clsDialogMutex.release();

	return bRes;
}

/**
 * @ingroup SipUserAgent
 * @brief 세션 타이머 점검 — 호출자가 1초 주기로 호출한다. 갱신자면 세션 간격의 절반에서
 *        갱신 re-INVITE 를 보내고, 갱신자가 아니면 만료 직전에 세션을 종료한다 (RFC 4028 §10).
 */
void CSipUserAgent::CheckSessionTimer( )
{
	if( m_bSessionTimer == false ) return;

	SIP_DIALOG_MAP::iterator	itMap;
	std::list< CSipMessage * >	clsRefreshList;
	SIP_CALL_ID_LIST						clsExpiredList;
	time_t	iNow = time( NULL );

	// 대상 leg — 도달 주소 조회(응용 콜백)는 다이얼로그 락 **밖**에서 한다 (psip 콜백 규약).
	typedef struct { std::string strCallId, strPeerId; bool bRefresh, bAskDest, bHaveDest;
	                 std::string strIp; int iPort; ESipTransport eTransport; } SESSION_TIMER_LEG;
	std::list< SESSION_TIMER_LEG >						clsLegList;
	std::list< SESSION_TIMER_LEG >::iterator	itLeg;

	m_clsDialogMutex.acquire();
	for( itMap = m_clsDialogMap.begin(); itMap != m_clsDialogMap.end(); ++itMap )
	{
		CSipDialog & clsDialog = itMap->second;

		if( clsDialog.m_iSessionExpires <= 0 ) continue;
		if( clsDialog.m_sttStartTime.tv_sec == 0 ) continue;		// 미확립 leg 은 INVITE 트랜잭션이 담당
		if( clsDialog.m_sttEndTime.tv_sec != 0 ) continue;

		SESSION_TIMER_LEG clsLeg;
		clsLeg.strCallId = itMap->first;
		clsLeg.strPeerId = clsDialog.m_strToId;
		clsLeg.bHaveDest = false;
		clsLeg.iPort = 0;
		clsLeg.eTransport = clsDialog.m_eTransport;
		// Record-Route 가 있는(중간 프록시 경유) 다이얼로그는 목적지를 바꾸지 않는다.
		clsLeg.bAskDest = clsDialog.m_clsRouteList.empty() && clsDialog.m_strToId.empty() == false;

		if( clsDialog.m_bSessionTimerDead )
		{
			clsLeg.bRefresh = false;
			clsLegList.push_back( clsLeg );
			continue;
		}

		if( clsDialog.m_iLastRefreshTime == 0 )
		{
			clsDialog.m_iLastRefreshTime = iNow;
			continue;
		}

		if( clsDialog.m_bLocalRefresher )
		{
			// 갱신 트랜잭션이 진행 중이면(또는 직전 시도가 실패했으면) 트랜잭션 수명만큼 기다린다.
			if( clsDialog.m_iRefreshSentTime > 0 && ( iNow - clsDialog.m_iRefreshSentTime ) < SIP_SESSION_TIMER_TX_SEC ) continue;
			if( ( iNow - clsDialog.m_iLastRefreshTime ) < ( clsDialog.m_iSessionExpires / 2 ) ) continue;

			clsLeg.bRefresh = true;
			clsLegList.push_back( clsLeg );
		}
		else
		{
			// 갱신자가 상대일 때는 만료 직전에 종료한다 — 선행 시간 min(32, SE/3) (§10).
			int iLead = clsDialog.m_iSessionExpires / 3;
			if( iLead > SIP_SESSION_TIMER_TX_SEC ) iLead = SIP_SESSION_TIMER_TX_SEC;

			if( ( iNow - clsDialog.m_iLastRefreshTime ) >= ( clsDialog.m_iSessionExpires - iLead ) )
			{
				clsLeg.bRefresh = false;
				clsLegList.push_back( clsLeg );
			}
		}
	}
	m_clsDialogMutex.release();

	if( clsLegList.empty() ) return;

	// 현재 도달 주소 조회 — 락 밖 (응용이 등록 자료구조를 조회한다).
	for( itLeg = clsLegList.begin(); itLeg != clsLegList.end(); ++itLeg )
	{
		if( itLeg->bAskDest == false || m_pclsCallBack == NULL ) continue;
		itLeg->bHaveDest = m_pclsCallBack->EventGetLegDest( itLeg->strCallId.c_str(), itLeg->strPeerId.c_str(),
			itLeg->strIp, itLeg->iPort, itLeg->eTransport );
		if( itLeg->strIp.empty() || itLeg->iPort <= 0 ) itLeg->bHaveDest = false;
	}

	// 목적지 반영 + 갱신 요청 생성 (다시 락 — 그 사이 사라진 다이얼로그는 건너뛴다).
	m_clsDialogMutex.acquire();
	for( itLeg = clsLegList.begin(); itLeg != clsLegList.end(); ++itLeg )
	{
		itMap = m_clsDialogMap.find( itLeg->strCallId );
		if( itMap == m_clsDialogMap.end() ) continue;

		CSipDialog & clsDialog = itMap->second;

		if( itLeg->bHaveDest ) SessionTimerApplyDest( itLeg->strCallId, clsDialog, itLeg->strIp, itLeg->iPort,
			itLeg->eTransport );

		if( itLeg->bRefresh == false )
		{
			clsExpiredList.push_back( itLeg->strCallId );
			continue;
		}

		CSipMessage * pclsInvite = clsDialog.CreateInvite( true );		// SDP 는 직전과 동일(o= 버전 유지)
		if( pclsInvite )
		{
			SessionTimerAddToRequest( clsDialog, pclsInvite, false );
			clsDialog.m_iRefreshSentTime = iNow;
			clsRefreshList.push_back( pclsInvite );
		}
	}
	m_clsDialogMutex.release();

	std::list< CSipMessage * >::iterator	itRefresh;
	for( itRefresh = clsRefreshList.begin(); itRefresh != clsRefreshList.end(); ++itRefresh )
	{
		m_clsSipStack.SendSipMessage( *itRefresh );
	}

	SIP_CALL_ID_LIST::iterator	itExpired;
	for( itExpired = clsExpiredList.begin(); itExpired != clsExpiredList.end(); ++itExpired )
	{
		CLog::Print( LOG_INFO, "SessionTimer expired: CallId(%s) — 세션 갱신 없음, 세션 종료(BYE)", itExpired->c_str() );

		StopCall( itExpired->c_str() );

		// StopCall 은 로컬 종료라 EventCallEnd 를 발생시키지 않는다 — 상위 teardown 연쇄를 위해 직접 통보한다.
		if( m_pclsCallBack ) m_pclsCallBack->EventCallEnd( itExpired->c_str(), SIP_REQUEST_TIME_OUT );
	}
}
