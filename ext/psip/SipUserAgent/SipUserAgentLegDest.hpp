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

// 서버 발신 in-dialog 요청(BYE·re-INVITE·NOTIFY·REFER·INFO)의 목적지 재해석.
//
//   다이얼로그가 기억한 목적지(m_strContactIp/m_iContactPort/m_eTransport)는 INVITE 를 **수신한 당시의
//   소스**(top Via received/rport + 도착 transport)다. NAT 뒤 단말이 대형 INVITE 를 TCP 로 승격해
//   보내면 그 연결은 단말 스택의 유휴 타이머(pjsip 33초)로 곧 닫히고, 닫힌 뒤 그 주소로 보내는
//   요청은 "이 연결에 써라"가 아니라 "이 주소로 새로 연결해라"가 되어 NAT 뒤에서 실패한다
//   (SipStackComm.hpp Send → StartSipTcpClientThread → TcpConnect error). 그래서 서버가 먼저 거는
//   in-dialog 요청은 **생성 직전에** 응용에 현재 도달 주소(살아있는 등록 바인딩)를 묻고 다이얼로그의
//   목적지를 그 값으로 바꾼다. docs/design/features/leg_liveness.md §6.3,
//   docs/design/features/registration_binding_set.md §2.2.
//
//   적용 범위: 확립된 다이얼로그(m_sttStartTime != 0)만. Record-Route 가 있는(중간 프록시 경유)
//   다이얼로그와 상대 ID 가 없는 다이얼로그는 손대지 않는다. 응용이 false 를 돌려주면(미등록 peer —
//   제휴 노드·IBCF 등) 기존 목적지를 그대로 쓴다.

/**
 * @ingroup SipUserAgent
 * @brief 다이얼로그의 목적지를 응용이 준 도달 주소로 바꾼다 (호출자가 m_clsDialogMutex 보유).
 * @param strCallId   SIP Call-ID (로그용)
 * @param clsDialog   대상 다이얼로그
 * @param strIp       도달 IP
 * @param iPort       도달 포트
 * @param eTransport  도달 transport
 */
void CSipUserAgent::ApplyLegDest( const std::string & strCallId, CSipDialog & clsDialog,
	const std::string & strIp, int iPort, ESipTransport eTransport )
{
	if( strIp.empty() || iPort <= 0 ) return;

	if( strIp != clsDialog.m_strContactIp || iPort != clsDialog.m_iContactPort ||
		eTransport != clsDialog.m_eTransport )
	{
		CLog::Print( LOG_DEBUG, "LegDest(%s): %s:%d(%d) → %s:%d(%d)", strCallId.c_str(),
			clsDialog.m_strContactIp.c_str(), clsDialog.m_iContactPort, clsDialog.m_eTransport,
			strIp.c_str(), iPort, eTransport );
	}

	clsDialog.m_strContactIp = strIp;
	clsDialog.m_iContactPort = iPort;
	clsDialog.m_eTransport   = eTransport;
}

/**
 * @ingroup SipUserAgent
 * @brief 서버 발신 in-dialog 요청을 만들기 직전에 다이얼로그의 목적지를 응용이 아는 현재 도달 주소로
 *        갱신한다. 콜백(EventGetLegDest)은 다이얼로그 락 **밖**에서 호출한다 (psip 콜백 규약 —
 *        응용이 자기 자료구조 락을 잡으므로 락 순서 역전 여지를 없앤다).
 * @param pszCallId SIP Call-ID
 * @returns 목적지를 갱신(또는 동일 값으로 확인)했으면 true, 대상이 아니거나 응용 응답이 없으면 false
 */
bool CSipUserAgent::RefreshLegDest( const char * pszCallId )
{
	if( pszCallId == NULL || pszCallId[0] == '\0' || m_pclsCallBack == NULL ) return false;

	SIP_DIALOG_MAP::iterator	itMap;
	std::string strPeerId;

	// 1) 선별 — 락 안에서는 판정과 상대 ID 복사만 한다.
	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap == m_clsDialogMap.end() )
	{
		m_clsDialogMutex.release();
		return false;
	}

	{
		CSipDialog & clsDialog = itMap->second;

		if( clsDialog.m_sttStartTime.tv_sec == 0 ||		// 미확립 — CANCEL/응답은 요청 도착 경로를 그대로 쓴다
			clsDialog.m_sttEndTime.tv_sec != 0 ||
			clsDialog.m_clsRouteList.empty() == false ||	// Record-Route(프록시 경유) — 목적지를 바꾸지 않는다
			clsDialog.m_strToId.empty() )
		{
			m_clsDialogMutex.release();
			return false;
		}

		strPeerId = clsDialog.m_strToId;
	}
	m_clsDialogMutex.release();

	// 2) 조회 — 락 밖.
	std::string strIp;
	int iPort = 0;
	ESipTransport eTransport = E_SIP_UDP;

	if( m_pclsCallBack->EventGetLegDest( pszCallId, strPeerId.c_str(), strIp, iPort, eTransport ) == false ) return false;
	if( strIp.empty() || iPort <= 0 ) return false;

	// 3) 반영 — 다시 락. 그 사이 사라진 다이얼로그는 건너뛴다.
	bool bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		ApplyLegDest( pszCallId, itMap->second, strIp, iPort, eTransport );
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// 다이얼로그가 응답·in-dialog 요청의 Contact 에 광고할 transport 를 정한다 — 응용(CSP)이 EventIncomingCall 에서
//   발신자의 등록 바인딩 transport 를 넣는다. 저장된 INVITE 에도 심어 이후 CreateResponse(18x/2xx/4xx·487)가 계승하고,
//   서버 발신 in-dialog 요청은 CSipDialog::CreateMessage 가 다이얼로그 값을 복사한다 (registration_binding_set.md §3).
bool CSipUserAgent::SetContactTransport( const char * pszCallId, ESipTransport eTransport )
{
	bool bRes = false;

	m_clsDialogMutex.acquire();
	SIP_DIALOG_MAP::iterator itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		itMap->second.m_iContactTransport = eTransport;
		if( itMap->second.m_pclsInvite ) itMap->second.m_pclsInvite->m_iContactTransport = eTransport;
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}
