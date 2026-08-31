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

#include "SipUserAgent.h"
#include "SipRegisterThread.h"
#include "SipUtility.h"
#include "SipCodecTable.h"
#include "SdpMessage.h"
#include "SdpAttributeCrypto.h"
#include "StringUtility.h"
#include "TimeString.h"
#include "TimeUtility.h"
#include "Log.h"
#include "Random.h"
#include "MemoryDebug.h"

#include "SipUserAgentLogin.hpp"
#include "SipUserAgentSipStack.hpp"
#include "SipUserAgentCall.hpp"
#include "SipUserAgentSend.hpp"
#include "SipUserAgentSms.hpp"
#include "SipUserAgentUtil.hpp"
#include "SipUserAgentMonitor.hpp"

#include "SipUserAgentRegister.hpp"
#include "SipUserAgentInvite.hpp"
#include "SipUserAgentBye.hpp"
#include "SipUserAgentCancel.hpp"
#include "SipUserAgentRefer.hpp"
#include "SipUserAgentNotify.hpp"
#include "SipUserAgentMessage.hpp"
#include "SipUserAgentPrack.hpp"
#include "SipUserAgentOptions.hpp"
#include "SipUserAgentSessionTimer.hpp"

// 생성자
CSipUserAgent::CSipUserAgent() : m_bStopEvent(false), m_pclsCallBack(NULL), m_iSeq(0), m_bStart(false)
	, m_bSessionTimer(false), m_iSessionTimerSE(SIP_SESSION_TIMER_ABS_MIN)
	, m_iSessionTimerMinSE(SIP_SESSION_TIMER_ABS_MIN), m_iSessionTimerRefresher(E_SESSION_REFRESHER_LOCAL)
{
}

// 소멸자
CSipUserAgent::~CSipUserAgent()
{
}

// SIP stack 을 시작하고 SIP 로그인 쓰레드를 시작한다.
bool CSipUserAgent::Start( CSipStackSetup & clsSetup, ISipUserAgentCallBack * pclsCallBack, ISipStackSecurityCallBack * pclsSecurityCallBack )
{
	if( m_bStart ) return false;

	m_clsSipStack.AddCallBack( this );

	m_pclsCallBack = pclsCallBack;
	m_clsSipStack.SetSecurityCallBack( pclsSecurityCallBack );

	if( m_clsSipStack.Start( clsSetup ) == false ) return false;

	StartSipRegisterThread( this );

	m_bStart = true;

	return true;
}

// SIP stack 을 종료하고 SIP 로그인 쓰레드를 종료한다.
bool CSipUserAgent::Stop( )
{
	if( m_bStart == false ) return false;

	SIP_SERVER_INFO_LIST::iterator	it;
	int	iCount;

	DeRegister();

	for( int i = 0; i < 100; ++i )
	{
		m_clsRegisterMutex.acquire();
		iCount = (int)m_clsRegisterList.size();
		if( iCount > 0 )
		{
			iCount = 0;

			// 로그인된 개수를 계산한다.
			for( it = m_clsRegisterList.begin(); it != m_clsRegisterList.end(); ++it )
			{
				if( it->m_bLogin ) ++iCount;
			}
		}
		m_clsRegisterMutex.release();

		if( iCount <= 0 ) break;
		MiliSleep(100);
	}

	m_bStopEvent = true;
	m_clsSipStack.Stop();

	// SipRegisterThread 가 종료할 때까지 대기한다.
	for( int i = 0; i < 100; ++i )
	{
		if( m_bStopEvent == false ) break;
		MiliSleep(100);
	}

	DeleteRegisterInfoAll( );

	m_clsDialogMutex.acquire();
	m_clsDialogMap.clear();
	m_clsDialogMutex.release();

	m_bStart = false;

	return true;
}

// CSipDialog 에서 SIP INVITE 메시지를 생성하여 전송한다.
bool CSipUserAgent::SendInvite( CSipDialog & clsDialog )
{
	if( clsDialog.m_strFromId.empty() || clsDialog.m_strToId.empty() ) return false;
	
	SIP_DIALOG_MAP::iterator			itMap;
	char	szTag[SIP_TAG_MAX_SIZE], szBranch[SIP_BRANCH_MAX_SIZE], szCallIdName[SIP_CALL_ID_NAME_MAX_SIZE];
	bool	bInsert = false;
	CSipMessage * pclsMessage = NULL;

	SipMakeTag( szTag, sizeof(szTag) );
	SipMakeBranch( szBranch, sizeof(szBranch) );

	clsDialog.m_strFromTag = szTag;
	clsDialog.m_strViaBranch = szBranch;

	gettimeofday( &clsDialog.m_sttInviteTime, NULL );

	while( 1 )
	{
		SipMakeCallIdName( szCallIdName, sizeof(szCallIdName) );

		clsDialog.m_strCallId = szCallIdName;
		clsDialog.m_strCallId.append( "@" );
		clsDialog.m_strCallId.append( m_clsSipStack.m_clsSetup.m_strLocalIp );

		m_clsDialogMutex.acquire();
		itMap = m_clsDialogMap.find( clsDialog.m_strCallId );
		if( itMap == m_clsDialogMap.end() )
		{
			pclsMessage = clsDialog.CreateInvite();
			if( pclsMessage )
			{
				// 세션 타이머 제안 (RFC 4028 §7.1) — dialog 상태를 먼저 채운 뒤 맵에 넣는다.
				SessionTimerAddToRequest( clsDialog, pclsMessage, true );

				m_clsDialogMap.insert( SIP_DIALOG_MAP::value_type( clsDialog.m_strCallId, clsDialog ) );
				bInsert = true;
			}
		}
		m_clsDialogMutex.release();

		if( bInsert ) break;
	}

	if( m_clsSipStack.SendSipMessage( pclsMessage ) == false )
	{
		Delete( clsDialog.m_strCallId.c_str() );
		return false;
	}

	return true;
}

// SIP Dialog 에 통화 종료 정보를 저장한다.
bool CSipUserAgent::SetCallEnd( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator			itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		gettimeofday( &itMap->second.m_sttEndTime, NULL );
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Dialog 를 삭제한다.
bool CSipUserAgent::Delete( const char * pszCallId )
{
	SIP_DIALOG_MAP::iterator			itMap;
	bool	bRes = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( pszCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		Delete( itMap );
		bRes = true;
	}
	m_clsDialogMutex.release();

	return bRes;
}

// SIP Dialog 를 삭제한다.
void CSipUserAgent::Delete( SIP_DIALOG_MAP::iterator & itMap )
{
	if( itMap->second.m_pclsInvite )
	{
		delete itMap->second.m_pclsInvite;
		itMap->second.m_pclsInvite = NULL;
	}

	m_clsDialogMap.erase( itMap );
}

// SIP INVITE 응답 메시지에 포함된 정보를 CSipDialog 에 저장한다.
bool CSipUserAgent::SetInviteResponse( std::string & strCallId, CSipMessage * pclsMessage, CSipCallRtp * pclsRtp, bool & bReInvite )
{
	SIP_DIALOG_MAP::iterator		itMap;
	bool	bFound = false, bStopCall = false;
	CSipMessage *pclsAck = NULL, *pclsInvite = NULL;

	bReInvite = false;

	m_clsDialogMutex.acquire();
	itMap = m_clsDialogMap.find( strCallId );
	if( itMap != m_clsDialogMap.end() )
	{
		pclsMessage->m_clsTo.SelectParam( SIP_TAG, itMap->second.m_strToTag );

		if( pclsRtp )
		{
			itMap->second.SetRemoteRtp( pclsRtp );
		}

		if( pclsMessage->m_iStatusCode == SIP_SESSION_PROGRESS || pclsMessage->m_iStatusCode == SIP_RINGING )
		{
			CSipHeader * pclsHeader = pclsMessage->GetHeader( "RSeq" );
			if( pclsHeader && pclsHeader->m_strValue.empty() == false )
			{
				itMap->second.m_iRSeq = atoi( pclsHeader->m_strValue.c_str() );
			}
		}

		if( pclsMessage->m_iStatusCode >= SIP_OK )
		{
			if( pclsMessage->m_iStatusCode != SIP_CONNECT_ERROR )
			{
				pclsAck = itMap->second.CreateAck( pclsMessage->m_iStatusCode );
			}

			if( pclsMessage->m_iStatusCode >= SIP_OK && pclsMessage->m_iStatusCode < SIP_MULTIPLE_CHOICES )
			{
				bool bCreateAck = false;

				if( pclsMessage->m_clsRecordRouteList.size() > 0 )
				{
					SIP_FROM_LIST::reverse_iterator itRL;

					itMap->second.m_clsRouteList.clear();

					for( itRL = pclsMessage->m_clsRecordRouteList.rbegin(); itRL != pclsMessage->m_clsRecordRouteList.rend(); ++itRL )
					{
						itMap->second.m_clsRouteList.push_back( *itRL );
					}

					bCreateAck = true;
				}

				SIP_FROM_LIST::iterator	itContact = pclsMessage->m_clsContactList.begin();
				if( itContact != pclsMessage->m_clsContactList.end() )
				{
					char	szUri[255];

					itContact->m_clsUri.ToString( szUri, sizeof(szUri) );
					itMap->second.m_strContactUri = szUri;
					bCreateAck = true;	
				}

				if( bCreateAck )
				{
					if( pclsAck ) delete pclsAck;
					pclsAck = itMap->second.CreateAck( pclsMessage->m_iStatusCode );
				}

				if( itMap->second.m_sttStartTime.tv_sec == 0 )
				{
					gettimeofday( &itMap->second.m_sttStartTime, NULL );
				}
				else
				{
					bReInvite = true;
				}

				// 세션 타이머 협상 결과 반영 (RFC 4028 §7.2) — 초기 2xx·갱신 2xx 공통.
				SessionTimerOnResponse( itMap->second, pclsMessage );

				if( itMap->second.m_sttCancelTime.tv_sec > 0 )
				{
					bStopCall = true;
				}
			}
			else if( pclsMessage->m_iStatusCode == SIP_UNAUTHORIZED || pclsMessage->m_iStatusCode == SIP_PROXY_AUTHENTICATION_REQUIRED )
			{
				if( itMap->second.m_sttCancelTime.tv_sec == 0 )
				{
					itMap->second.m_strToTag.clear();

					pclsInvite = itMap->second.CreateInvite();
					if( pclsInvite )
					{
						SIP_SERVER_INFO_LIST::iterator itSL;
						const char * pszUserId = pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str();

						m_clsRegisterMutex.acquire();
						for( itSL = m_clsRegisterList.begin(); itSL != m_clsRegisterList.end(); ++itSL )
						{
							if( !strcmp( itSL->m_strUserId.c_str(), pszUserId ) )
							{
								itSL->AddAuth( pclsInvite, pclsMessage );
								break;
							}
						}
						m_clsRegisterMutex.release();
					}
				}
			}
			else if( m_bSessionTimer && pclsMessage->m_iStatusCode == SIP_SESSION_INTERVAL_TOO_SMALL )
			{
				// RFC 4028 §7.3 — 상대가 요구한 Min-SE 로 올려 새 트랜잭션으로 1회 재시도한다.
				int iMinSE = SessionTimerGetMinSE( pclsMessage );
				if( iMinSE > itMap->second.m_iPeerMinSE ) itMap->second.m_iPeerMinSE = iMinSE;

				bool bInitial = ( itMap->second.m_sttStartTime.tv_sec == 0 );

				if( itMap->second.m_iPeerMinSE > 0 && itMap->second.m_bSessionTimerRetried == false )
				{
					itMap->second.m_bSessionTimerRetried = true;
					itMap->second.m_iSessionExpires = 0;		// Min-SE 반영해 재산출
					if( bInitial ) itMap->second.m_strToTag.clear();

					pclsInvite = itMap->second.CreateInvite( bInitial ? false : true );
					if( pclsInvite ) SessionTimerAddToRequest( itMap->second, pclsInvite, bInitial );
				}
				else if( bInitial )
				{
					gettimeofday( &itMap->second.m_sttEndTime, NULL );
				}
				else
				{
					bReInvite = true;
				}
			}
			else
			{
				if( itMap->second.m_sttStartTime.tv_sec == 0 )
				{
					gettimeofday( &itMap->second.m_sttEndTime, NULL );

					if( pclsMessage->m_iStatusCode == SIP_MOVED_TEMPORARILY )
					{
						if( itMap->second.m_sttCancelTime.tv_sec == 0 )
						{
							SIP_FROM_LIST::iterator	itContact = pclsMessage->m_clsContactList.begin();
							if( itContact != pclsMessage->m_clsContactList.end() )
							{
								itMap->second.m_strToId = itContact->m_clsUri.m_strUser;
								itMap->second.m_strToTag.clear();
								pclsInvite = itMap->second.CreateInvite();
								if( pclsInvite )
								{
									pclsInvite->m_clsReqUri = itContact->m_clsUri;
								}
							}
						}
					}
				}
				else
				{
					bReInvite = true;

					// RFC 4028 §10 — 세션 갱신 요청이 408/481 이면 세션이 죽은 것으로 확정하고
					//   CheckSessionTimer 가 BYE + teardown 을 태운다.
					if( m_bSessionTimer && itMap->second.m_iSessionExpires > 0 &&
						( pclsMessage->m_iStatusCode == SIP_REQUEST_TIME_OUT ||
						  pclsMessage->m_iStatusCode == SIP_CALL_TRANSACTION_DOES_NOT_EXIST ) )
					{
						itMap->second.m_bSessionTimerDead = true;
					}
					// 그 밖의 실패 응답은 m_iRefreshSentTime 을 유지해 다음 시도가 트랜잭션
					//   수명(32초) 뒤에 오게 한다 — 연속 재시도 폭주 방지 (§10).
				}
			}
		}

		bFound = true;
	}
	m_clsDialogMutex.release();

	if( pclsAck )
	{
		m_clsSipStack.SendSipMessage( pclsAck );
	}

	if( pclsInvite )
	{
		m_clsSipStack.SendSipMessage( pclsInvite );

		// 인증 정보를 포함한 INVITE 메시지를 전송한 경우, 응용으로 callback 호출하지 않는다.
		return false;
	}

	if( bStopCall )
	{
		// CANCEL 전송 후, INVITE 200 OK 수신하였으면 BYE 를 전송한다.
		StopCall( strCallId.c_str() );
		return false;
	}

	return bFound;
}

// SIP 메시지에서 RTP 정보를 가져온다.
bool CSipUserAgent::GetSipCallRtp( CSipMessage * pclsMessage, CSipCallRtp & clsRtp )
{
	// For multipart/mixed bodies (e.g. OMA PoC group INVITE), extract the application/sdp part
	std::string strSdpBody;
	if( pclsMessage->m_clsContentType.IsEqual( "multipart", "mixed" ) && !pclsMessage->m_strBody.empty() )
	{
		const std::string& body = pclsMessage->m_strBody;
		// Find a MIME part with Content-Type: application/sdp
		size_t ctPos = body.find( "application/sdp" );
		if( ctPos != std::string::npos )
		{
			// Skip past the part headers (find the blank line after the content-type header)
			size_t hdEnd = body.find( "\r\n\r\n", ctPos );
			if( hdEnd != std::string::npos )
			{
				size_t sdpStart = hdEnd + 4;
				// Part ends at next boundary (--) or end of body
				size_t sdpEnd = body.find( "\r\n--", sdpStart );
				if( sdpEnd == std::string::npos ) sdpEnd = body.length();
				strSdpBody = body.substr( sdpStart, sdpEnd - sdpStart );
				// 경계 탐색이 마지막 라인의 CRLF 를 소비한다 — 종결 CRLF 를 복원하지 않으면
				// 라인 단위 SDP 파서가 마지막 라인(예: a=fmtp:MCPTT ...)을 버린다.
				if( strSdpBody.length() >= 2 && strSdpBody.compare( strSdpBody.length()-2, 2, "\r\n" ) != 0 )
					strSdpBody += "\r\n";
			}
		}
		if( strSdpBody.empty() ) return false;
	}
	else if( pclsMessage->m_clsContentType.IsEqual( "application", "sdp" ) && !pclsMessage->m_strBody.empty() )
	{
		strSdpBody = pclsMessage->m_strBody;
	}

	if( !strSdpBody.empty() )
	{
		CSdpMessage clsSdp;

		if( clsSdp.Parse( strSdpBody.c_str(), (int)strSdpBody.length() ) == -1 )
		{
			CLog::Print( LOG_ERROR, "GetSipCallRtp sdp parse error [%s]", strSdpBody.c_str() );
			return false;
		}

		clsRtp.m_strIp = clsSdp.m_clsConnection.m_strAddr;
		SipIpv6Parse( clsRtp.m_strIp );

		SDP_MEDIA_LIST::iterator itMedia = clsSdp.m_clsMediaList.begin();
		if( itMedia == clsSdp.m_clsMediaList.end() )
		{
			CLog::Print( LOG_ERROR, "GetSipCallRtp media is not found" );
			return false;
		}

		if( clsRtp.m_strIp.empty() )
		{
			clsRtp.m_strIp = itMedia->m_clsConnection.m_strAddr;
			SipIpv6Parse( clsRtp.m_strIp );
		}

		clsRtp.m_iPort = itMedia->m_iPort;

		// 코덱 테이블 기반 오퍼 코덱 매칭 (RFC 3264/3551):
		//  - 동적 PT(>=96)는 번호가 세션별 임의 계약이므로 a=rtpmap encoding 이름으로 식별한다.
		//    (구 PT 번호 화이트리스트는 같은 코덱이라도 다른 번호를 쓰면 거부하거나 다른 코덱을
		//     오인식했다 — 예: telephone-event 를 96 으로 오퍼하면 AMR-WB 로 오인.)
		//  - 정적 PT(<96)는 RFC 3551 고정 번호로 식별.
		//  - m_clsCodecList 에는 테이블 PT 로 정규화해 보관(코덱 identity). 실 wire PT 는
		//    answer 생성(AddSdp)이 오퍼 rtpmap 에서 다시 echo 한다.
		//  - m_iCodec(선택 코덱) = 오퍼∩테이블 중 테이블 우선순위(배열 순서) 최상위.
		SDP_FMT_LIST::iterator itFmt;
		int iBestRank = -1;

		for( itFmt = itMedia->m_clsFmtList.begin(); itFmt != itMedia->m_clsFmtList.end(); ++itFmt )
		{
			int iPt = atoi( itFmt->c_str() );
			const CSipCodecEntry * pclsEntry = NULL;

			if( iPt >= 96 )
			{
				const char * pszEncoding = NULL;
				SDP_ATTRIBUTE_LIST::iterator itRtpmap;

				for( itRtpmap = itMedia->m_clsAttributeList.begin(); itRtpmap != itMedia->m_clsAttributeList.end(); ++itRtpmap )
				{
					if( strcasecmp( itRtpmap->m_strName.c_str(), "rtpmap" ) ) continue;
					if( atoi( itRtpmap->m_strValue.c_str() ) != iPt ) continue;

					const char * pszSp = strchr( itRtpmap->m_strValue.c_str(), ' ' );
					if( pszSp ) pszEncoding = pszSp + 1;
					break;
				}

				if( pszEncoding )
				{
					pclsEntry = CSipCodecTable::FindByRtpmap( pszEncoding );
				}
				else
				{
					// rtpmap 없는 동적 PT (비규격 오퍼) — 구 화이트리스트 호환으로 번호 매칭 관용
					pclsEntry = CSipCodecTable::FindByPt( iPt );
				}
			}
			else
			{
				pclsEntry = CSipCodecTable::FindByPt( iPt );
			}

			if( pclsEntry == NULL ) continue;

			// 중복 제거 (같은 코덱을 여러 PT 로 광고한 오퍼)
			bool bDup = false;
			CODEC_LIST::iterator itCodec;
			for( itCodec = clsRtp.m_clsCodecList.begin(); itCodec != clsRtp.m_clsCodecList.end(); ++itCodec )
			{
				if( *itCodec == pclsEntry->m_iPt )
				{
					bDup = true;
					break;
				}
			}
			if( bDup ) continue;

			clsRtp.m_clsCodecList.push_back( pclsEntry->m_iPt );

			int iRank = CSipCodecTable::GetRank( pclsEntry->m_iPt );
			if( iBestRank < 0 || ( iRank >= 0 && iRank < iBestRank ) )
			{
				iBestRank = iRank;
				clsRtp.m_iCodec = pclsEntry->m_iPt;
			}
		}

		clsRtp.m_eDirection = E_RTP_SEND_RECV;

		// 미디어 SRTP (SDES — media_security.md §5.1): m=audio protocol + a=crypto 파싱.
		//   지원 suite(AES_CM_128_HMAC_SHA1_80|_32) 중 첫 항목을 채택한다 (RFC 4568 §5.1.1).
		//   RTP/AVP 에 a=crypto 병기(best-effort)도 파싱한다 — 수락 여부는 응용 정책(§4).
		clsRtp.m_bRemoteSavp = ( strncasecmp( itMedia->m_strProtocol.c_str(), "RTP/SAVP", 8 ) == 0 );
		{
			SDP_ATTRIBUTE_LIST::iterator	itCryptoAttr;

			for( itCryptoAttr = itMedia->m_clsAttributeList.begin(); itCryptoAttr != itMedia->m_clsAttributeList.end(); ++itCryptoAttr )
			{
				if( strcasecmp( itCryptoAttr->m_strName.c_str(), "crypto" ) ) continue;

				CSdpAttributeCrypto clsCrypto;
				if( clsCrypto.Parse( itCryptoAttr->m_strValue.c_str(), (int)itCryptoAttr->m_strValue.length() ) == -1 ) continue;
				if( clsCrypto.Empty() ) continue;
				if( strcmp( clsCrypto.m_strCryptoSuite.c_str(), "AES_CM_128_HMAC_SHA1_80" ) &&
						strcmp( clsCrypto.m_strCryptoSuite.c_str(), "AES_CM_128_HMAC_SHA1_32" ) ) continue;

				clsRtp.m_strRemoteCryptoTag = clsCrypto.m_strTag;
				clsRtp.m_strRemoteCryptoSuite = clsCrypto.m_strCryptoSuite;
				clsRtp.m_strRemoteCryptoKey = clsCrypto.m_strKey;
				break;
			}
		}

		SDP_ATTRIBUTE_LIST::iterator	itAttr;

		for( itAttr = itMedia->m_clsAttributeList.begin(); itAttr != itMedia->m_clsAttributeList.end(); ++itAttr )
		{
			if( !strcmp( itAttr->m_strName.c_str(), "sendrecv" ) )
			{
				clsRtp.m_eDirection = E_RTP_SEND_RECV;
				break;
			}
			else if( !strcmp( itAttr->m_strName.c_str(), "sendonly" ) )
			{
				clsRtp.m_eDirection = E_RTP_SEND;
				break;
			}
			else if( !strcmp( itAttr->m_strName.c_str(), "recvonly" ) )
			{
				clsRtp.m_eDirection = E_RTP_RECV;
				break;
			}
			else if( !strcmp( itAttr->m_strName.c_str(), "inactive" ) )
			{
				clsRtp.m_eDirection = E_RTP_INACTIVE;
				break;
			}
		}

#ifdef USE_MEDIA_LIST
		clsRtp.m_clsMediaList = clsSdp.m_clsMediaList;
#endif

		return true;
	}

	return false;
}

// SIP CSeq 헤더에 저장할 번호를 리턴한다.
int CSipUserAgent::GetSeqNum( )
{
	int iSeq;

	m_clsMutex.acquire();
	++m_iSeq;
	if( m_iSeq > 1000000000 )
	{
		m_iSeq = 1;
	}

	iSeq = m_iSeq;
	m_clsMutex.release();

	return iSeq;
}

// 프로세스가 종료될 때에 최종적으로 실행하여서 openssl 메모리 누수를 출력하지 않는다.
void CSipUserAgent::Final()
{
	m_clsSipStack.Final();
}
