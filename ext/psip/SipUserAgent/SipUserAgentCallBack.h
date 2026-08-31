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

	// ── 미디어 SRTP (SDES, RFC 4568 — media_security.md §5.1) ──
	// local: 이 측이 SDP 로 방출할 a=crypto(자기 송신 키 선언). suite 가 설정되면
	//   AddSdp 의 m=audio 는 RTP/SAVP 가 된다. key = base64(master key16||salt14) 원문.
	// remote: 수신 SDP 의 a=crypto — 지원 suite 중 첫 항목 (GetSipCallRtp 가 채움).
	//   m_bRemoteSavp 인데 remote suite 가 비면 유효한 crypto 없는 SAVP offer —
	//   응용이 협상 실패(488)로 처리한다. MKI 미사용.
	std::string	m_strLocalCryptoTag;      // a=crypto tag (기본 "1", answer 는 offer tag echo)
	std::string	m_strLocalCryptoSuite;    // 예: AES_CM_128_HMAC_SHA1_80 (비면 SRTP 미사용)
	std::string	m_strLocalCryptoKey;      // base64(key||salt)
	std::string	m_strRemoteCryptoTag;
	std::string	m_strRemoteCryptoSuite;
	std::string	m_strRemoteCryptoKey;
	bool				m_bRemoteSavp = false;    // 수신 m=audio protocol 이 RTP/SAVP(F) 였는지

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

	/** 서버가 먼저 거는 in-dialog 요청(세션 갱신·만료 BYE)의 **현재 도달 주소**를 응용에 묻는다.
	 *  다이얼로그가 기억한 주소는 요청 수신 당시의 소스라, NAT 뒤 단말에서는 이미 죽어 있을 수
	 *  있다 (대형 INVITE 를 TCP 로 승격해 보낸 뒤 그 연결이 닫힌 경우 등). 응용이 등록 바인딩
	 *  (IP·포트·transport 한 세트)을 알고 있으면 채우고 true 를 리턴한다. false 면 psip 은
	 *  다이얼로그가 기억한 주소를 그대로 쓴다.
	 *  @param pszCallId  대상 다이얼로그의 Call-ID
	 *  @param pszPeerId  상대(원격) 사용자 ID — 응용의 등록 자료구조 조회 키 */
	virtual bool EventGetLegDest( const char * pszCallId, const char * pszPeerId,
		std::string & strIp, int & iPort, ESipTransport & eTransport ){ return false; };

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
