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
#include "SipCodecTable.h"
#include "Log.h"
#include "MemoryDebug.h"

/**
 * @ingroup SipUserAgent
 * @brief ������
 */
CSipDialog::CSipDialog( CSipStack * pclsSipStack ) : m_iSeq(0), m_iNextSeq(0), m_iInviteSeq(0), m_iContactPort(-1), m_eTransport(E_SIP_UDP)
	, m_iOutboundLocalPort(-1)
	, m_iLocalRtpPort(-1), m_iLocalApplicationPort(-1), m_eLocalDirection(E_RTP_SEND_RECV), m_iRemoteRtpPort(-1), m_eRemoteDirection(E_RTP_SEND_RECV), m_iCodec(-1), m_iRSeq(-1), m_b100rel(false)
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

	// Per-dialog outbound override (route 결정 / access_service binding) 우선, 없으면 stack primary.
	const char * pszVia = !m_strOutboundLocalIp.empty()
		? m_strOutboundLocalIp.c_str()
		: m_pclsSipStack->m_clsSetup.m_strLocalIp.c_str();
	int iViaPort = m_iOutboundLocalPort > 0
		? m_iOutboundLocalPort
		: m_pclsSipStack->m_clsSetup.GetLocalPort( m_eTransport );
	pclsMessage->AddVia( pszVia, iViaPort, szBranch, m_eTransport );
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
		const char * pszVia = !m_strOutboundLocalIp.empty()
			? m_strOutboundLocalIp.c_str()
			: m_pclsSipStack->m_clsSetup.m_strLocalIp.c_str();
		int iViaPort = m_iOutboundLocalPort > 0
			? m_iOutboundLocalPort
			: m_pclsSipStack->m_clsSetup.GetLocalPort( m_eTransport );
		pclsMessage->AddVia( pszVia, iViaPort, m_strViaBranch.c_str(), m_eTransport );
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

	const char * pszVia = !m_strOutboundLocalIp.empty()
		? m_strOutboundLocalIp.c_str()
		: m_pclsSipStack->m_clsSetup.m_strLocalIp.c_str();
	int iViaPort = m_iOutboundLocalPort > 0
		? m_iOutboundLocalPort
		: m_pclsSipStack->m_clsSetup.GetLocalPort( m_eTransport );
	pclsMessage->AddVia( pszVia, iViaPort, m_strViaBranch.c_str(), m_eTransport );

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

/* 코덱 1개의 a=rtpmap / a=fmtp / a=ptime 라인 출력.
 * iPt = 실제 광고 PT — answer 는 오퍼 echo 값, 오퍼는 코덱 리스트의 값(테이블 PT). */
static int AddCodecAttribute( char * pszBuf, int iBufSize, const CSipCodecEntry & clsCodec, int iPt )
{
	int iLen = 0;

	iLen += snprintf( pszBuf + iLen, iBufSize - iLen, "a=rtpmap:%d %s\r\n", iPt, clsCodec.GetRtpmap().c_str() );
	if( clsCodec.m_strFmtp.empty() == false )
	{
		iLen += snprintf( pszBuf + iLen, iBufSize - iLen, "a=fmtp:%d %s\r\n", iPt, clsCodec.m_strFmtp.c_str() );
	}
	if( clsCodec.m_iPtime > 0 )
	{
		iLen += snprintf( pszBuf + iLen, iBufSize - iLen, "a=ptime:%d\r\n", clsCodec.m_iPtime );
	}

	return iLen;
}

/**
 * @ingroup SipUserAgent
 * @brief 상대(offer) SDP 가 m=application(MCPTT floor) 미디어를 제안했는지 확인한다.
 * @returns 제안했으면 true.
 */
bool CSipDialog::HasRemoteApplicationMedia( )
{
#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator	itList;

	for( itList = m_clsRemoteMediaList.begin(); itList != m_clsRemoteMediaList.end(); ++itList )
	{
		if( !strcmp( itList->m_strMedia.c_str(), "application" ) ) return true;
	}
#endif

	return false;
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
		// 코덱 테이블(CSipCodecTable — 응용 주입, 미주입 시 기본 테이블) 기반 SDP 합성.
		// RFC 3264: answer 의 dynamic PT 는 offer 가 쓴 PT 를 그대로 echo (rtpmap 이름으로 식별).
		//   PT 번호를 서버 고정값으로 강제하면 offer 가 다른 PT(예: pjsua AMR-WB=96)를 쓴 UE 와
		//   협상이 깨져 미디어 스트림 생성 시 크래시한다. offer 에 해당 rtpmap 이 없으면(-1,
		//   예: 발신 offer 생성 시 remote 미수신) 테이블 PT 로 광고한다.
		const CSipCodecEntry & clsTe = CSipCodecTable::GetTelephoneEvent();
		int iTePt = FindRemotePayloadType( clsTe.GetMatchPrefix().c_str() );
		if( iTePt < 0 ) iTePt = clsTe.m_iPt;

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

			iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, " %d\r\n", iTePt );

			for( itList = m_clsCodecList.begin(); itList != m_clsCodecList.end(); ++itList )
			{
				const CSipCodecEntry * pclsCodec = CSipCodecTable::FindByPt( *itList );
				if( pclsCodec == NULL ) continue;

				iLen += AddCodecAttribute( szSdp + iLen, (int)sizeof(szSdp) - iLen, *pclsCodec, *itList );
			}
		}
		else
		{
			const CSipCodecEntry * pclsCodec = CSipCodecTable::FindByPt( m_iCodec );

			if( pclsCodec == NULL )
			{
				// 선택 코덱 부재/테이블 외 — m=audio 없는 구조 불량 SDP 방지용 방어 출력(최우선 코덱).
				// 협상 실패의 정식 거부(488)는 응용의 서비스별 게이트가 수행한다. VoLTE relay 는
				// media-list passthrough 분기라 여기에 오지 않는다.
				CLog::Print( LOG_ERROR, "AddSdp codec(%d) not in codec table — fallback to top codec", m_iCodec );
				pclsCodec = &CSipCodecTable::GetTop();
			}

			// answer 코덱의 실 wire PT = 오퍼 echo (없으면 테이블 PT)
			int iPt = FindRemotePayloadType( pclsCodec->GetMatchPrefix().c_str() );
			if( iPt < 0 ) iPt = pclsCodec->m_iPt;

			iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=audio %d RTP/AVP %d %d\r\n", m_iLocalRtpPort, iPt, iTePt );
			iLen += AddCodecAttribute( szSdp + iLen, (int)sizeof(szSdp) - iLen, *pclsCodec, iPt );
		}

		iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=rtpmap:%d %s\r\n", iTePt, clsTe.GetRtpmap().c_str() );
		if( clsTe.m_strFmtp.empty() == false )
		{
			iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=fmtp:%d %s\r\n", iTePt, clsTe.m_strFmtp.c_str() );
		}
		iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "a=%s\r\n", GetRtpDirectionString( m_eLocalDirection ) );
	}

	// MCPTT floor control 미디어 (3GPP TS 24.379/24.380) — local application(floor) 포트가
	//   설정된 경우에만 m=application 라인 추가. PTT 그룹콜 개시자 200 OK 등에서 floor 포트를
	//   광고해 UE 가 floor dest 를 학습하게 한다. (미설정(-1)이면 VoLTE/일반 호 SDP 무변경.)
	if( m_iLocalApplicationPort > 0 )
	{
		// fmtp: floor 협상 파라미터 (TS 24.380 §12.1.2.3) — mc_queueing 을 광고해 큐잉을
		//   협상한다 (미협상 멤버의 비선점 요청은 서버가 Deny #1). mc_priority/mc_granted 는
		//   응용 정책이라 싣지 않는다.
		iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen,
			"m=application %d UDP MCPTT\r\na=floorid:0 mstrm:audio\r\na=fmtp:MCPTT mc_queueing\r\n",
			m_iLocalApplicationPort );
	}
	else if( strstr( szSdp, "m=application" ) == NULL && HasRemoteApplicationMedia() )
	{
		// RFC 3264 §6: answer 의 m= 라인 개수·순서는 offer 와 같아야 하고, 쓰지 않는 스트림은
		//   라인을 지우는 것이 아니라 **포트 0** 으로 거절한다. floor 없는 세션(private 멀티 —
		//   mc_no_floor_ctrl 협상 결과)은 광고할 floor 포트가 없지만, 상대가 제안한 floor
		//   스트림을 라인째 생략하면 m= 개수가 어긋나 엄격한 단말이 answer 를 거부한다.
		//   세션 중 offer(re-INVITE)도 같은 규칙 — 이미 협상된 m= 라인은 제거할 수 없다.
		iLen += snprintf( szSdp + iLen, sizeof(szSdp)-iLen, "m=application 0 UDP MCPTT\r\n" );
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
	m_iLocalApplicationPort = pclsRtp->GetApplicationPort();  // MCPTT floor 포트 (없으면 -1)

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
	// 코덱 테이블(CSipCodecTable) 등재 PT 만 사용 — 구 정적 화이트리스트(0/3/4/8/18/96/98/99)는
	// 기본 테이블로 승계. 동적 PT 의 이름 기반 식별은 GetSipCallRtp 의 rtpmap 매칭이 담당한다.
	return CSipCodecTable::FindByPt( iCodec ) != NULL;
}

/* 대소문자 무시 prefix 비교 (libc strncasecmp 이식성 회피용 로컬 헬퍼). */
static bool _RtpMapPrefixIEq( const char * s, const char * prefix )
{
	if( s == NULL || prefix == NULL ) return false;
	for( ; *prefix; ++s, ++prefix )
	{
		char a = *s, b = *prefix;
		if( a >= 'A' && a <= 'Z' ) a = (char)( a - 'A' + 'a' );
		if( b >= 'A' && b <= 'Z' ) b = (char)( b - 'A' + 'a' );
		if( a != b ) return false;
	}
	return true;
}

int CSipDialog::FindRemotePayloadType( const char * pszEncoding )
{
	if( pszEncoding == NULL ) return -1;

#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator itM;
	for( itM = m_clsRemoteMediaList.begin(); itM != m_clsRemoteMediaList.end(); ++itM )
	{
		if( strcasecmp( itM->m_strMedia.c_str(), "audio" ) ) continue;

		SDP_ATTRIBUTE_LIST::iterator itA;
		for( itA = itM->m_clsAttributeList.begin(); itA != itM->m_clsAttributeList.end(); ++itA )
		{
			// a=rtpmap:<pt> <encoding>/<rate>[/<ch>]  (m_strName="rtpmap", m_strValue="<pt> <enc>/<rate>")
			if( strcasecmp( itA->m_strName.c_str(), "rtpmap" ) ) continue;

			const char * v = itA->m_strValue.c_str();
			const char * sp = strchr( v, ' ' );
			if( sp == NULL ) continue;
			if( _RtpMapPrefixIEq( sp + 1, pszEncoding ) )
				return atoi( v );
		}
	}
#endif

	return -1;
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

		// INVITE 의 CSeq 를 보관 → ACK/CANCEL 이 재사용 (RFC 3261 §13.2.2.4 / §9.1).
		// dialog 내 NOTIFY/INFO 등이 m_iSeq 를 올려도 2xx-ACK 가 INVITE 와 같은 CSeq 를 쓰게 함.
		if( !strcmp( pszSipMethod, SIP_METHOD_INVITE ) ) m_iInviteSeq = m_iSeq;
	}
	else
	{
		// ACK / CANCEL: 대상 INVITE 와 동일 CSeq 사용. (보관값 없으면 기존 동작 유지)
		iSeq = ( m_iInviteSeq != 0 ) ? m_iInviteSeq : m_iSeq;
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
