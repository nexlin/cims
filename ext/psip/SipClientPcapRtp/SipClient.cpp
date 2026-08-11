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

#include "SipClient.h"
#include "SipClientSetup.h"
#include <time.h>
#include "Log.h"
#include "RtpThread.h"
#include "MemoryDebug.h"

std::string	gstrInviteId;

// SIP REGISTER 응답 메시지 수신 이벤트 핸들러
void CSipClient::EventRegister( CSipServerInfo * pclsInfo, int iStatus )
{
	CLog::Print( LOG_DEBUG, "%s(%d)", __FUNCTION__, iStatus );
	printf( "EventRegister(%s) : %d\n", pclsInfo->m_strUserId.c_str(), iStatus );
}

// SIP 통화 요청 수신 이벤트 핸들러
void CSipClient::EventIncomingCall( const char * pszCallId, const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp )
{
	printf( "EventIncomingCall(%s,%s)\n", pszCallId, pszFrom );

	gstrInviteId = pszCallId;

	if( pclsRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort, pclsRtp->m_iCodec );

		m_clsDestRtp = *pclsRtp;

		CSipCallRtp clsRtp;

		clsRtp.m_strIp = gclsSetupFile.m_strLocalIp;
		clsRtp.m_iPort = gclsRtpThread.m_iPort;
		clsRtp.m_iCodec = giCodec;

		gclsUserAgent.AcceptCall( gstrInviteId.c_str(), &clsRtp );
		gclsRtpThread.Start( m_clsDestRtp.m_strIp.c_str(), m_clsDestRtp.m_iPort );
	}
}

// SIP Ring / Session Progress 수신 이벤트 핸들러
void CSipClient::EventCallRing( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRtp )
{
	printf( "EventCallRing(%s,%d)\n", pszCallId, iSipStatus );

	if( pclsRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort, pclsRtp->m_iCodec );
	}
}

// SIP 통화 연결 이벤트 핸들러
void CSipClient::EventCallStart( const char * pszCallId, CSipCallRtp * pclsRtp )
{
	printf( "EventCallStart(%s)\n", pszCallId );

	if( pclsRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort, pclsRtp->m_iCodec );

		gclsRtpThread.Start( pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort );
	}
}

// SIP 통화 종료 이벤트 핸들러
void CSipClient::EventCallEnd( const char * pszCallId, int iSipStatus )
{
	printf( "EventCallEnd(%s,%d)\n", pszCallId, iSipStatus );

	gclsRtpThread.Stop( );
}

// SIP ReINVITE 수신 이벤트 핸들러
void CSipClient::EventReInvite( const char * pszCallId, CSipCallRtp * pclsRemoteRtp, CSipCallRtp * pclsLocalRtp )
{
	printf( "EventReInvite(%s)\n", pszCallId );

	if( pclsRemoteRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRemoteRtp->m_strIp.c_str(), pclsRemoteRtp->m_iPort, pclsRemoteRtp->m_iCodec );
		gclsRtpThread.Start( pclsRemoteRtp->m_strIp.c_str(), pclsRemoteRtp->m_iPort );
	}
}

// Screened / Unscreened Transfer 요청 수신 이벤트 핸들러
bool CSipClient::EventTransfer( const char * pszCallId, const char * pszReferToCallId, bool bScreenedTransfer )
{
	printf( "EventTransfer(%s,%s)\n", pszCallId, pszReferToCallId );

	return false;
}

// Blind Transfer 요청 수신 이벤트 핸들러
bool CSipClient::EventBlindTransfer( const char * pszCallId, const char * pszReferToId )
{
	printf( "EventBlindTransfer(%s,%s)\n", pszCallId, pszReferToId );

	return false;
}

// SIP MESSAGE 수신 이벤트 핸들러
bool CSipClient::EventMessage( const char * pszFrom, const char * pszTo, CSipMessage * pclsMessage )
{
	char	szContentType[255];

	memset( szContentType, 0, sizeof(szContentType) );
	pclsMessage->m_clsContentType.ToString( szContentType, sizeof(szContentType) );

	printf( "EventMessage(%s,%s)\n", pszFrom, pszTo );
	printf( "content-type[%s]\n", szContentType );
	printf( "body[%s]\n", pclsMessage->m_strBody.c_str() );

	return true;
}
