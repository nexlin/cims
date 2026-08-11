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

#ifndef _SIP_USER_AGENT_CALLBACK_H_
#define _SIP_USER_AGENT_CALLBACK_H_

// 응용으로 SDP 미디어 리스트를 전달할 때에 사용된다.
#define USE_MEDIA_LIST

#include "SipStackDefine.h"
#include "SipServerInfo.h"
#include "SdpMedia.h"
#include "RtpDirection.h"

typedef std::list< int > CODEC_LIST;

// RTP 정보 저장 클래스
class CSipCallRtp
{
public:
	CSipCallRtp() : m_iPort(-1), m_iCodec(-1), m_eDirection( E_RTP_SEND_RECV ), m_iApplicationPort(-1)
	{}

	void SetIpPort( const char * pszIp, int iPort, int iSocketCountPerMedia );
	void SetDirection( ERtpDirection eDirection );
	int GetMediaCount( );
	int GetAudioPort( );
	int GetVideoPort( );
	int GetApplicationPort( );

	// IP 주소
	std::string	m_strIp;

	// 포트 번호
	int					m_iPort;

	// MCPTT floor control(m=application) 포트. >0 이면 GetApplicationPort 가 이 값을 반환.
	int					m_iApplicationPort;

	// 선택된 코덱 번호
	int					m_iCodec;

	// 전송/수신
	ERtpDirection	m_eDirection;

	// 전체 코덱 리스트
	CODEC_LIST	m_clsCodecList;

#ifdef USE_MEDIA_LIST
	// 전체 미디어 리스트
	SDP_MEDIA_LIST	m_clsMediaList;
#endif
};

// CSipUserAgent 의 이벤트를 응용 프로그램으로 전달하는 callback 인터페이스
class ISipUserAgentCallBack
{
public:
	virtual ~ISipUserAgentCallBack(){};

	// SIP REGISTER 응답 메시지 수신 이벤트 핸들러
	virtual void EventRegister( CSipServerInfo * pclsInfo, int iStatus ) = 0;

	// SIP 통화 요청 수신에 대한 인증 확인 이벤트 핸들러
	virtual bool EventIncomingRequestAuth( CSipMessage * pclsMessage ){ return true; };

	// SIP 통화 요청 수신 이벤트 핸들러
	virtual void EventIncomingCall( const char * pszCallId, const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp, CSipMessage * pclsMessage = NULL ) = 0;

	// SIP Ring / Session Progress 수신 이벤트 핸들러
	virtual void EventCallRing( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRtp ) = 0;

	// SIP 통화 연결 이벤트 핸들러
	virtual void EventCallStart( const char * pszCallId, CSipCallRtp * pclsRtp ) = 0;

	// SIP 통화 종료 이벤트 핸들러
	virtual void EventCallEnd( const char * pszCallId, int iSipStatus ) = 0;

	// SIP ReINVITE 수신 이벤트 핸들러
	virtual void EventReInvite( const char * pszCallId, CSipCallRtp * pclsRemoteRtp, CSipCallRtp * pclsLocalRtp ){};

	// SIP ReINVITE 응답 메시지 수신 이벤트 핸들러
	virtual void EventReInviteResponse( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRemoteRtp ){};

	// SIP PRACK 수신 이벤트 핸들러
	virtual void EventPrack( const char * pszCallId, CSipCallRtp * pclsRtp ){};

	// Screened / Unscreened Transfer 요청 수신 이벤트 핸들러
	virtual bool EventTransfer( const char * pszCallId, const char * pszReferToCallId, bool bScreenedTransfer ){ return false; };

	// Blind Transfer 요청 수신 이벤트 핸들러
	virtual bool EventBlindTransfer( const char * pszCallId, const char * pszReferToId ){ return false; };

	// SIP 통화 전달 응답 수신 이벤트 핸들러
	virtual void EventTransferResponse( const char * pszCallId, int iSipStatus ){};

	// SIP MESSAGE 수신 이벤트 핸들러
	virtual bool EventMessage( const char * pszFrom, const char * pszTo, CSipMessage * pclsMessage ){ return false; };

	// SIP 메시지 수신 쓰레드가 종료됨을 알려주는 이벤트 핸들러
	virtual void EventThreadEnd( int iThreadId ){};
};

#endif
