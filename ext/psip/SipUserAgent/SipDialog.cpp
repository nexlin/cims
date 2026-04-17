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
#include "SipDialog.h"
#include "SipUserAgent.h"
#include "SipUtility.h"
#include "MemoryDebug.h"

/**
 * @ingroup SipUserAgent
 * @brief ������
 */
CSipDialog::CSipDialog( CSipStack * pclsSipStack ) : m_iSeq(0), m_iNextSeq(0), m_iContactPort(-1), m_eTransport(E_SIP_UDP)
	, m_iLocalRtpPort(-1), m_eLocalDirection(E_RTP_SEND_RECV), m_iRemoteRtpPort(-1), m_eRemoteDirection(E_RTP_SEND_RECV), m_iCodec(-1), m_iRSeq(-1), m_b100rel(false)
	, m_pclsInvite(NULL), m_pclsSipStack( pclsSipStack )
	, m_iSessionVersion(0)
	, m_bSendCall(true)
{
	memset( &m_sttInviteTime, 0, sizeof(m_sttInviteTime) );
	memset( &m_sttCancelTime, 0, sizeof(m_sttCancelTime) );
	memset( &m_sttStartTime, 0, sizeof(m_sttStartTime) );
	memset( &m_sttEndTime, 0, sizeof(m_sttEndTime) );
}

/**
 * @ingroup SipUserAgent
 * @brief �Ҹ���.
 */
CSipDialog::~CSipDialog()
{
}

/**
 * @ingroup SipUserAgent
 * @brief INVITE �޽����� �����Ѵ�.
 * @returns �����ϸ� INVITE �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateInvite( )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_INVITE );
	if( pclsMessage == NULL ) return NULL;

	char	szBranch[SIP_BRANCH_MAX_SIZE];

	SipMakeBranch( szBranch, sizeof(szBranch) );

	pclsMessage->AddVia( m_pclsSipStack->m_clsSetup.m_strLocalIp.c_str(), m_pclsSipStack->m_clsSetup.GetLocalPort( m_eTransport ), szBranch, m_eTransport );
	m_strViaBranch = szBranch;

	if( m_b100rel )
	{
		pclsMessage->AddHeader( "Allow", "PRACK, INVITE, ACK, BYE, CANCEL, REFER, NOTIFY, MESSAGE" );
		pclsMessage->AddHeader( "Supported", "100rel" );
		pclsMessage->AddHeader( "Require", "100rel" );
	}

	AddSdp( pclsMessage );

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief ACK �޽����� �����Ѵ�.
 * @param iStatusCode SIP INVITE ���� �ڵ�
 * @returns �����ϸ� ACK �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateAck( int iStatusCode )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_ACK );
	if( pclsMessage == NULL ) return NULL;

	if( iStatusCode / 100 != 2 )
	{
		pclsMessage->AddVia( m_pclsSipStack->m_clsSetup.m_strLocalIp.c_str(), m_pclsSipStack->m_clsSetup.GetLocalPort( m_eTransport ), m_strViaBranch.c_str(), m_eTransport );
	}

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief CANCEL �޽����� �����Ѵ�.
 * @returns �����ϸ� CANCEL �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateCancel( )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_CANCEL );
	if( pclsMessage == NULL ) return NULL;

	pclsMessage->AddVia( m_pclsSipStack->m_clsSetup.m_strLocalIp.c_str(), m_pclsSipStack->m_clsSetup.GetLocalPort( m_eTransport ), m_strViaBranch.c_str(), m_eTransport );

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief BYE �޽����� �����Ѵ�.
 * @returns �����ϸ� BYE �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateBye( )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_BYE );
	if( pclsMessage == NULL ) return NULL;

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief NOTIFY �޽����� �����Ѵ�.
 * @returns �����ϸ� NOTIFY �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateNotify( )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_NOTIFY );
	if( pclsMessage == NULL ) return NULL;

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief REFER �޽����� �����Ѵ�.
 * @returns �����ϸ� REFER �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateRefer( )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_REFER );
	if( pclsMessage == NULL ) return NULL;

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief PRACK �޽����� �����Ѵ�.
 * @returns �����ϸ� PRACK �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreatePrack( )
{
	if( m_iRSeq == -1 ) return NULL;

	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_PRACK );
	if( pclsMessage == NULL ) return NULL;

	char	szRAck[101];

	snprintf( szRAck, sizeof(szRAck), "%d %d INVITE", m_iRSeq, m_iSeq );

	pclsMessage->AddHeader( "RAck", szRAck );

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief INFO �޽����� �����Ѵ�.
 * @returns �����ϸ� PRACK �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateInfo( )
{
	CSipMessage * pclsMessage = CreateMessage( SIP_METHOD_INFO );
	if( pclsMessage == NULL ) return NULL;

	return pclsMessage;
}

/**
 * @ingroup SipUserAgent
 * @brief SIP �޽����� SDP �޽����� �߰��Ѵ�.
 * @param pclsMessage SDP �޽����� �߰��� SIP �޽��� ��ü
 * @returns true �� �����Ѵ�.
 */
bool CSipDialog::AddSdp( CSipMessage * pclsMessage )
{
	char	szSdp[4096];
	int		iLen = 0;
	const char * pszAddrType = "IP4";

	if( strstr( m_strLocalRtpIp.c_str(), ":" ) )
	{
		pszAddrType = "IP6";
	}

	++m_iSessionVersion;

	iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "v=0\r\n"
					"o=CSS 4 %d IN %s %s\r\n"
					"s=hak\r\n", m_iSessionVersion, pszAddrType, m_strLocalRtpIp.c_str() );

	iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "c=IN %s %s\r\n", pszAddrType, m_strLocalRtpIp.c_str() );
	iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "t=0 0\r\n" );

#ifdef USE_MEDIA_LIST
	if( m_clsLocalMediaList.size() > 0 )
	{
		SDP_MEDIA_LIST::iterator	itList;

		for( itList = m_clsLocalMediaList.begin(); itList != m_clsLocalMediaList.end(); ++itList )
		{
			iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=" );
			iLen += itList->ToString( szSdp + iLen, sizeof(szSdp)-iLen );
		}
	}
	else
#endif
	{
		if( pclsMessage->IsRequest() && m_clsCodecList.empty() == false )
		{
			CODEC_LIST::iterator	itList;

			iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP", m_iLocalRtpPort );

			for( itList = m_clsCodecList.begin(); itList != m_clsCodecList.end(); ++itList )
			{
				if( IsUseCodec( *itList ) )
				{
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, " %d", *itList );
				}
			}

			iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, " 101\r\n" );

			for( itList = m_clsCodecList.begin(); itList != m_clsCodecList.end(); ++itList )
			{
				switch( *itList )
				{
				case 99:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:99 AMR-WB/16000/1\r\n" );
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=fmtp:99 octet-align=1\r\n" );
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=ptime:20\r\n" );
					break;
				case 98:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:98 AMR/8000/1\r\n" );
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=fmtp:98 octet-align=1\r\n" );
					break;
				case 0:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:0 PCMU/8000\r\n" );
					break;
				case 3:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:3 GSM/8000\r\n" );
					break;
				case 4:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:4 G723/8000\r\n" );
					break;
				case 8:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:8 PCMA/8000\r\n" );
					break;
				case 18:
					iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:18 G729/8000\r\n" );
					break;
					break;
				}
			}
		}
		else
		{
			switch( m_iCodec )
			{
			case 99:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 99 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:99 AMR-WB/16000/1\r\n" );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=fmtp:99 octet-align=1\r\n" );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=ptime:20\r\n" );
				break;
			case 98:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 98 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:98 AMR/8000/1\r\n" );
				break;
			case 0:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 0 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:0 PCMU/8000\r\n" );
				break;
			case 3:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 3 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:3 GSM/8000\r\n" );
				break;
			case 4:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 4 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:4 G723/8000\r\n" );
				break;
			case 8:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 8 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:8 PCMA/8000\r\n" );
				break;
			case 18:
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP 18 101\r\n", m_iLocalRtpPort );
				iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:18 G729/8000\r\n" );
				break;
			}
		}

		iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:101 telephone-event/8000\r\n"
			"a=fmtp:101 0-15\r\n" );
		iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=%s\r\n", GetRtpDirectionString( m_eLocalDirection ) );
	}

	pclsMessage->m_strBody = szSdp;
	pclsMessage->m_iContentLength = iLen;
	pclsMessage->m_clsContentType.Set( "application", "sdp" );
	
	return true;
}

/**
 * @ingroup SipUserAgent
 * @brief local RTP ������ �����Ѵ�.
 * @param pclsRtp RTP ������ ������ ��ü
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipDialog::SetLocalRtp( CSipCallRtp * pclsRtp )
{
	if( pclsRtp == NULL ) return false;
	
	m_strLocalRtpIp = pclsRtp->m_strIp;
	m_iLocalRtpPort = pclsRtp->m_iPort;
	m_iCodec = pclsRtp->m_iCodec;
	m_clsCodecList = pclsRtp->m_clsCodecList;
	m_eLocalDirection = pclsRtp->m_eDirection;

	switch( m_eLocalDirection )
	{
	case E_RTP_SEND_RECV:
	case E_RTP_INACTIVE:
	case E_RTP_NULL:
		m_eRemoteDirection = m_eLocalDirection;
		break;
	case E_RTP_SEND:
		m_eRemoteDirection = E_RTP_RECV;
		break;
	case E_RTP_RECV:
		m_eRemoteDirection = E_RTP_SEND;
		break;
	}

#ifdef USE_MEDIA_LIST
	m_clsLocalMediaList = pclsRtp->m_clsMediaList;
#endif

	return true;
}

/**
 * @ingroup SipUserAgent
 * @brief remote RTP ������ �����Ѵ�.
 * @param pclsRtp RTP ������ ������ ��ü
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipDialog::SetRemoteRtp( CSipCallRtp * pclsRtp )
{
	if( pclsRtp == NULL ) return false;

	// ReINVITE ���� hold �� ��� IP �ּҰ� 0.0.0.0 ���� ���ŵǾ Transfer �� ���� ���������� SDP IP �ּҰ� ���޵��� �ʱ� ���ؼ� ������.
	if( strcmp( pclsRtp->m_strIp.c_str(), "0.0.0.0" ) )
	{
		m_strRemoteRtpIp = pclsRtp->m_strIp;
	}

	m_iRemoteRtpPort = pclsRtp->m_iPort;
	m_iCodec = pclsRtp->m_iCodec;
	m_eRemoteDirection = pclsRtp->m_eDirection;

	switch( m_eRemoteDirection )
	{
	case E_RTP_SEND_RECV:
	case E_RTP_INACTIVE:
	case E_RTP_NULL:
		m_eLocalDirection = m_eRemoteDirection;
		break;
	case E_RTP_SEND:
		m_eLocalDirection = E_RTP_RECV;
		break;
	case E_RTP_RECV:
		m_eLocalDirection = E_RTP_SEND;
		break;
	}

#ifdef USE_MEDIA_LIST
	m_clsRemoteMediaList = pclsRtp->m_clsMediaList;
#endif

	return true;
}

/**
 * @ingroup SipUserAgent
 * @brief my SIP Ŭ���̾�Ʈ�� RTP ������ �����´�.
 * @param pclsRtp SIP Ŭ���̾�Ʈ�� RTP ������ ������ ����
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipDialog::SelectLocalRtp( CSipCallRtp * pclsRtp )
{
	if( pclsRtp == NULL ) return false;

	pclsRtp->m_strIp = m_strLocalRtpIp;
	pclsRtp->m_iPort = m_iLocalRtpPort;
	pclsRtp->m_iCodec = m_iCodec;
	pclsRtp->m_eDirection = m_eLocalDirection;

#ifdef USE_MEDIA_LIST
	pclsRtp->m_clsMediaList = m_clsLocalMediaList;
#endif

	return true;
}

/**
 * @ingroup SipUserAgent
 * @brief SIP Ŭ���̾�Ʈ�� RTP ������ �����´�.
 * @param pclsRtp SIP Ŭ���̾�Ʈ�� RTP ������ ������ ����
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipDialog::SelectRemoteRtp( CSipCallRtp * pclsRtp )
{
	if( pclsRtp == NULL ) return false;

	pclsRtp->m_strIp = m_strRemoteRtpIp;
	pclsRtp->m_iPort = m_iRemoteRtpPort;
	pclsRtp->m_iCodec = m_iCodec;
	pclsRtp->m_eDirection = m_eRemoteDirection;

#ifdef USE_MEDIA_LIST
	pclsRtp->m_clsMediaList = m_clsRemoteMediaList;
#endif

	return true;
}

/**
 * @ingroup SipUserAgent
 * @brief CDR ���� ��ü�� �����͸� �����Ѵ�.
 * @param pclsCdr CDR ���� ��ü
 */
void CSipDialog::GetCdr( CSipCdr * pclsCdr )
{
	if( m_bSendCall )
	{
		pclsCdr->m_strFromId = m_strFromId;
		pclsCdr->m_strToId = m_strToId;
	}
	else
	{
		// CSipDialog �� From, To �� SipUserAgent �� SIP ��û �޽����� �����ϴ� ���忡�� ����Ǿ� �����Ƿ� CDR �� ���ؼ��� �ݴ�� ������ �ش�.
		pclsCdr->m_strFromId = m_strToId;
		pclsCdr->m_strToId = m_strFromId;
	}

	pclsCdr->m_strCallId = m_strCallId;
	pclsCdr->m_sttInviteTime = m_sttInviteTime;
	pclsCdr->m_sttStartTime = m_sttStartTime;
	pclsCdr->m_sttEndTime = m_sttEndTime;
}

/**
 * @ingroup SipUserAgent
 * @brief ��ȭ ����� Dialog �ΰ�?
 * @returns ��ȭ ����� Dialog �̸� true �� �����ϰ� �׷��� ������ false �� �����Ѵ�.
 */
bool CSipDialog::IsConnected( )
{
	if( m_sttStartTime.tv_sec != 0 && m_sttEndTime.tv_sec == 0 ) return true;

	return false;
}

/**
 * @ingroup SipUserAgent
 * @brief Dialog ���� ����ϴ� �ڵ����� �˻��Ѵ�.
 * @param iCodec �ڵ� payload type
 * @returns Dialog ���� ����ϴ� �ڵ��̸� true �� �����ϰ� �׷��� ������ false �� �����Ѵ�.
 */
bool CSipDialog::IsUseCodec( int iCodec )
{
	bool bFound = false;

	switch( iCodec )
	{
	case 0:
	case 3:
	case 4:
	case 8:
	case 18:
	case 99:
	case 98:
		bFound = true;
		break;
	}

	return bFound;
}

/**
 * @ingroup SipUserAgent
 * @brief SIP ��û �޽����� �����Ѵ�.
 * @param pszSipMethod SIP �޼ҵ�
 * @returns �����ϸ� SIP ��û �޽����� �����ϰ� �׷��� ������ NULL �� �����Ѵ�.
 */
CSipMessage * CSipDialog::CreateMessage( const char * pszSipMethod )
{
	CSipMessage * pclsMessage = new CSipMessage();
	if( pclsMessage == NULL ) return NULL;

	if( pclsMessage->m_clsCallId.Parse( m_strCallId.c_str(), (int)m_strCallId.length() ) == -1 )
	{
		delete pclsMessage;
		return NULL;
	}

	pclsMessage->m_eTransport = m_eTransport;
	pclsMessage->m_strSipMethod = pszSipMethod;

	if( m_strContactUri.empty() == false )
	{
		pclsMessage->m_clsReqUri.Parse( m_strContactUri.c_str(), (int)m_strContactUri.length() );
	}
	else
	{
		// per-dialog override 도메인이 있으면 우선 사용 (예: MCPTT → mcptt realm)
		const std::string& strReqDomain = !m_strOverrideDomain.empty()
			? m_strOverrideDomain
			: m_strContactIp;
		pclsMessage->m_clsReqUri.Set( SIP_PROTOCOL, m_strToId.c_str(), strReqDomain.c_str(), m_iContactPort );
		pclsMessage->m_clsReqUri.InsertTransport( m_eTransport );
	}

	int iSeq;

	if( !strcmp( pszSipMethod, SIP_METHOD_PRACK ) )
	{
		m_iNextSeq = m_iSeq + 2;
		iSeq = m_iSeq + 1;
	}
	else if( strcmp( pszSipMethod, SIP_METHOD_ACK ) && strcmp( pszSipMethod, SIP_METHOD_CANCEL ) )
	{
		if( m_iNextSeq != 0 )
		{
			m_iSeq = m_iNextSeq;
			m_iNextSeq = 0;
		}
		else
		{
			++m_iSeq;
		}
		iSeq = m_iSeq;
	}
	else
	{
		iSeq = m_iSeq;
	}

	pclsMessage->m_clsCSeq.Set( iSeq, pszSipMethod );

	// per-dialog override 가 있으면 From 도메인에 우선 적용
	const std::string& strLocalDomain = !m_strOverrideDomain.empty()
		? m_strOverrideDomain
		: ( m_pclsSipStack->m_clsSetup.m_strDomain.empty()
			? m_pclsSipStack->m_clsSetup.m_strLocalIp
			: m_pclsSipStack->m_clsSetup.m_strDomain );

	pclsMessage->m_clsFrom.m_clsUri.Set( SIP_PROTOCOL, m_strFromId.c_str(), strLocalDomain.c_str(), 0 );
	pclsMessage->m_clsFrom.InsertParam( SIP_TAG, m_strFromTag.c_str() );

	// To: use domain (AOR) when available; override 도메인 최우선
	const std::string& strToDomain = !m_strOverrideDomain.empty()
		? m_strOverrideDomain
		: ( m_pclsSipStack->m_clsSetup.m_strDomain.empty()
			? m_strContactIp
			: m_pclsSipStack->m_clsSetup.m_strDomain );
	pclsMessage->m_clsTo.m_clsUri.Set( SIP_PROTOCOL, m_strToId.c_str(), strToDomain.c_str(), 0 );
	if( m_strToTag.empty() == false )
	{
		pclsMessage->m_clsTo.InsertParam( SIP_TAG, m_strToTag.c_str() );
	}

	// P-Asserted-Identity: use domain when configured (RFC 3325)
	char szUri[1024];
	const char * pszProtocol = ( m_eTransport == E_SIP_TLS ) ? "sips" : "sip";
	if( strstr( strLocalDomain.c_str(), ":" ) )
		snprintf( szUri, sizeof(szUri), "<%s:%s@[%s]>", pszProtocol, m_strFromId.c_str(), strLocalDomain.c_str() );
	else
		snprintf( szUri, sizeof(szUri), "<%s:%s@%s>", pszProtocol, m_strFromId.c_str(), strLocalDomain.c_str() );

	pclsMessage->AddHeader( "P-Asserted-Identity", szUri );
	pclsMessage->AddHeader( "Diversion", szUri );

	if( m_clsRouteList.size() > 0 )
	{
		// ���ŵ� Record-Route �� �����ϸ� �̸� Route �� ���Խ�Ų��.
		pclsMessage->m_clsRouteList = m_clsRouteList;
	}
	else
	{
		pclsMessage->AddRoute( m_strContactIp.c_str(), m_iContactPort, m_eTransport );
	}

	return pclsMessage;
}
