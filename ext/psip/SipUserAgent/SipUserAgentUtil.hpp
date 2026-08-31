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

// SIP Call-ID 로 통화를 검색한 후, 검색된 결과로 peer RTP 정보를 저장한다.
bool CSipUserAgent::GetRemoteCallRtp( const char * pszCallId, CSipCallRtp * pclsRtp )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		bRes = itMap->second.SelectRemoteRtp( pclsRtp );
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Call-ID 로 통화를 검색한 후, local RTP 정보(SDP 로 방출한 값 — SRTP local crypto 포함)를
// 저장한다. 응용이 offer 에 실은 a=crypto 키를 answer 수신 시점에 되찾는 용도 (media_security.md §5).
bool CSipUserAgent::GetLocalCallRtp( const char * pszCallId, CSipCallRtp * pclsRtp )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		bRes = itMap->second.SelectLocalRtp( pclsRtp );
	}
	m_clsDialogMutex.release();

	return bRes;
}

/**
 * @ingroup SipUserAgent
 * @brief SIP Call-ID 로 통화를 검색한 후, 원격 SDP(개시자=offer, 수신자=answer)의 rtpmap 에서
 *        코덱/telephone-event 의 wire payload type 을 조회한다 — CMP leg 별 PT 재작성 파라미터
 *        (user_pt/user_te_pt) 산출용. 해당 rtpmap 부재 시 각각 -1.
 * @param pszCallId    SIP Call-ID
 * @param pszEncoding  코덱 encoding 접두(예: "AMR-WB/16000")
 * @param iPt          [out] 코덱 wire PT (-1 = 미발견)
 * @param iTePt        [out] telephone-event wire PT (-1 = 미발견)
 * @returns 다이얼로그를 찾으면 true.
 */
bool CSipUserAgent::GetRemotePayloadTypes( const char * pszCallId, const char * pszEncoding,
                                           int & iPt, int & iTePt )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	iPt = -1;
	iTePt = -1;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		iPt = itMap->second.FindRemotePayloadType( pszEncoding );
		iTePt = itMap->second.FindRemotePayloadType(
			CSipCodecTable::GetTelephoneEvent().GetMatchPrefix().c_str() );
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Call-ID 로 통화를 검색한 후, 검색된 결과로 peer 아이디를 저장한다.
bool CSipUserAgent::GetToId( const char * pszCallId, std::string & strToId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	strToId.clear();

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		strToId = itMap->second.m_strToId;
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Call-ID 로 통화를 검색한 후, 검색된 결과로 my 아이디를 저장한다.
bool CSipUserAgent::GetFromId( const char * pszCallId, std::string & strFromId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	strFromId.clear();

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		strFromId = itMap->second.m_strFromId;
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Call-ID 로 통화를 검색한 후, 검색된 결과로 전화 상대방의 Contact 정보를 CSipCallRoute 객체에 저장한다.
bool CSipUserAgent::GetContact( const char * pszCallId, CSipCallRoute * pclsRoute )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		pclsRoute->m_strDestIp = itMap->second.m_strContactIp;
		pclsRoute->m_iDestPort = itMap->second.m_iContactPort;
		pclsRoute->m_eTransport = itMap->second.m_eTransport;
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Call-ID 로 통화를 검색한 후, 검색된 결과의 CDR 정보를 저장한다.
bool CSipUserAgent::GetCdr( const char * pszCallId, CSipCdr * pclsCdr )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		itMap->second.GetCdr( pclsCdr );
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP INVITE 메시지를 수신한 경우, 해당 SIP INVITE 메시지에서 헤더 이름을 검색하여서 이에 대한 값을 리턴한다. 모든 헤더를 검색하는 것은 아니고 CSipMessage 의 m_clsHeaderList 에 저장된 헤더만 검색한다.
bool CSipUserAgent::GetInviteHeaderValue( const char * pszCallId, const char * pszHeaderName, std::string & strValue )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		if( itMap->second.m_pclsInvite )
		{
			CSipHeader * pclsHeader = itMap->second.m_pclsInvite->GetHeader( pszHeaderName );
			if( pclsHeader )
			{
				strValue = pclsHeader->m_strValue;
				bRes = true;
			}
		}
	}
	m_clsDialogMutex.release();

	return bRes;
}

// Dialog 의 RSeq 값을 리턴한다.
int CSipUserAgent::GetRSeq( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	int iRSeq = -1;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		iRSeq = itMap->second.m_iRSeq;
	}
	m_clsDialogMutex.release();

	return iRSeq;
}

// Dialog 의 RSeq 값을 설정한다.
void CSipUserAgent::SetRSeq( const char * pszCallId, int iRSeq )
{
	SIP_DIALOG_MAP::iterator		itMap;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		itMap->second.m_iRSeq = iRSeq;
	}
	m_clsDialogMutex.release();
}

/**
 * @ingroup SipUserAgent
 * @brief Call-ID 에 해당하는 Dialog 에 override 도메인 설정.
 *        이후 이 Dialog 에서 생성되는 모든 SIP 메시지의 From/To/Request-URI 도메인이
 *        전역 SipStackSetup::m_strDomain 대신 이 값으로 대체된다.
 * @param pszCallId SIP Call-ID
 * @param pszDomain 설정할 도메인 (예: "ptt.mnc001.mcc001.3gppnetwork.org")
 * @returns Dialog 존재 시 true, 없으면 false
 */
bool CSipUserAgent::SetCallDomain( const char * pszCallId, const char * pszDomain )
{
	if( !pszCallId || !pszDomain ) return false;

	SIP_DIALOG_MAP::iterator itMap;
	bool bFound = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		itMap->second.m_strOverrideDomain = pszDomain;
		bFound = true;
	}
	m_clsDialogMutex.release();

	return bFound;
}

// 통화가 연결 요청 중인지 확인한다.
bool CSipUserAgent::IsRingCall( const char * pszCallId, const char * pszTo )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		if( itMap->second.IsConnected() == false )
		{
			if( pszTo )
			{
				if( !strcmp( pszTo, itMap->second.m_strToId.c_str() ) )
				{
					bRes = true;
				}
			}
			else
			{
				bRes = true;
			}
		}
	}
	m_clsDialogMutex.release();

	return bRes;
}

// 100rel 기능이 활성화 유무를 검색한다.
bool CSipUserAgent::Is100rel( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	b100rel = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		b100rel = itMap->second.m_b100rel;
		if( b100rel == false && itMap->second.m_pclsInvite )
		{
			b100rel = itMap->second.m_pclsInvite->Is100rel();
		}
	}
	m_clsDialogMutex.release();

	return b100rel;
}

// hold 인지 검사한다.
bool CSipUserAgent::IsHold( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bHold = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		if( itMap->second.m_eLocalDirection != E_RTP_SEND_RECV )
		{
			bHold = true;
		}
	}
	m_clsDialogMutex.release();

	return bHold;
}

// 통화 연결되었는지 검사한다.
bool CSipUserAgent::IsConnected( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bConnected = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		if( itMap->second.m_sttStartTime.tv_sec > 0 )
		{
			bConnected = true;
		}
	}
	m_clsDialogMutex.release();

	return bConnected;
}

// SIP 통화 요청 INVITE 메시지를 검색한다.
CSipMessage * CSipUserAgent::DeleteIncomingCall( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator		itMap;
	CSipMessage * pclsMessage = NULL;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		if( itMap->second.m_sttStartTime.tv_sec == 0 )
		{
			if( itMap->second.m_pclsInvite )
			{
				pclsMessage = itMap->second.m_pclsInvite;
				itMap->second.m_pclsInvite = NULL;
				Delete( itMap );
			}
		}
	}
	m_clsDialogMutex.release();

	return pclsMessage;
}
