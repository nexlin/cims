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

#ifndef _SIP_DIALOG_H_
#define _SIP_DIALOG_H_

#include "SipCdr.h"
#include "SipMessage.h"
#include "SipUserAgentCallBack.h"

// SIP Dialog 정보를 저장하는 클래스
class CSipDialog
{
public:
	CSipDialog( CSipStack * pclsSipStack );
	~CSipDialog();

	// SIP From 헤더에 저장되는 사용자 아이디
	std::string	m_strFromId;

	// SIP From 헤더에 저장되는 tag
	std::string	m_strFromTag;

	// SIP To 헤더에 저장되는 사용자 아이디
	std::string	m_strToId;

	// SIP To 헤더에 저장되는 tag
	std::string	m_strToTag;

	/** SIP Call-ID */
	std::string m_strCallId;

	// SIP Top Via 헤더의 branch
	std::string m_strViaBranch;

	// SIP CSeq 헤더의 번호
	int					m_iSeq;

	// SIP CSeq 헤더의 다음 번호 ( PRACK 다음에 전송할 메시지를 위해서 사용됨 )
	int					m_iNextSeq;

	/** 가장 최근 송신한 INVITE 의 CSeq 번호. ACK/CANCEL 은 RFC 3261 상 자신이 ACK/CANCEL
	 *  하는 INVITE 와 동일 CSeq 를 써야 한다. 다이얼로그 내 NOTIFY/INFO 등이 m_iSeq 를
	 *  증가시킨 뒤 2xx-ACK 가 m_iSeq 를 그대로 쓰면 INVITE 와 CSeq 가 어긋나(예: PTT 그룹콜의
	 *  conference NOTIFY → ACK CSeq 불일치) UAS 가 ACK 미매칭으로 Timer H(~32s) teardown 됨.
	 *  이를 막기 위해 INVITE 송신 시 그 CSeq 를 보관해 ACK/CANCEL 이 재사용한다. */
	int					m_iInviteSeq;

	// SIP 요청 메시지를 전송할 IP 주소
	std::string	m_strContactIp;

	// SIP 요청 메시지를 전송할 포트 번호
	int					m_iContactPort;

	// SIP 요청 메시지를 전송할 transport
	ESipTransport	m_eTransport;

	/** Per-dialog override domain for From/To/Request-URI.
	 *  Empty → 전역 CSipStackSetup::m_strDomain fallback.
	 *  예) MCPTT 그룹 콜에서 mcptt 도메인으로 강제 사용. */
	std::string	m_strOverrideDomain;

	/** Per-dialog outbound Via 자기 IP override.
	 *  Empty → 전역 CSipStackSetup::m_strLocalIp fallback (현 primary local_node).
	 *  CSP 가 route 결정 또는 access_service binding 에 따라 listener 의 bind_ip 를 설정.
	 *  IBCF multi-peer / multi-listener 환경에서 leg 별로 다른 listener 의 IP 가
	 *  Via/Contact 자기 주소가 되도록 함. */
	std::string	m_strOutboundLocalIp;

	/** Per-dialog outbound Via 자기 port override.
	 *  <=0 → 전역 CSipStackSetup::GetLocalPort(transport) fallback. */
	int			m_iOutboundLocalPort;

	// local RTP IP 주소
	std::string	m_strLocalRtpIp;

	// local RTP 포트 번호
	int					m_iLocalRtpPort;

	// local MCPTT floor control (m=application) 포트. -1 이면 SDP 에 floor media 미사용.
	int					m_iLocalApplicationPort;

	/** local RTP direction ( sendrecv, sendonly, recvonly, inactive ) */
	ERtpDirection	m_eLocalDirection;

#ifdef USE_MEDIA_LIST
	/** local media list */
	SDP_MEDIA_LIST	m_clsLocalMediaList;
#endif

	// remote RTP IP 주소
	std::string	m_strRemoteRtpIp;

	// remote RTP 포트 번호
	int					m_iRemoteRtpPort;

	/** remote RTP direction ( sendrecv, sendonly, recvonly, inactive ) */
	ERtpDirection	m_eRemoteDirection;

#ifdef USE_MEDIA_LIST
	/** remote media list */
	SDP_MEDIA_LIST	m_clsRemoteMediaList;
#endif

	// 코덱
	int					m_iCodec;

	CODEC_LIST	m_clsCodecList;

	// SIP 요청 메시지에 저장될 Request Uri
	std::string	m_strContactUri;

	/** RSeq */
	int					m_iRSeq;

	/** 100rel */
	bool				m_b100rel;

	// INVITE 전송/수신 시간
	struct timeval m_sttInviteTime;

	// CANCEL 전송 시간
	struct timeval m_sttCancelTime;

	// 통화 시작 시간
	struct timeval m_sttStartTime;

	// 통화 종료 시간
	struct timeval m_sttEndTime;

	// 수신된 INVITE 메시지
	CSipMessage * m_pclsInvite;

	// 수신된 INVITE 메시지에 저장된 Record-Route 리스트로 생성한 Route 리스트
	SIP_FROM_LIST	m_clsRouteList;

	CSipStack		* m_pclsSipStack;

	/** SDP session version */
	int	m_iSessionVersion;

	// 발신 전화인가?
	bool m_bSendCall;

	CSipMessage * CreateInvite( );
	CSipMessage * CreateAck( int iStatusCode );
	CSipMessage * CreateCancel( );
	CSipMessage * CreateBye( );
	CSipMessage * CreateNotify( );
	CSipMessage * CreateRefer( );
	CSipMessage * CreatePrack( );
	CSipMessage * CreateInfo( );

	bool AddSdp( CSipMessage * pclsMessage );

	/** 상대(offer) SDP 에 m=application(MCPTT floor) 미디어가 있었는가 — RFC 3264 m= 미러링 판정용. */
	bool HasRemoteApplicationMedia( );

	bool SetLocalRtp( CSipCallRtp * pclsRtp );
	bool SetRemoteRtp( CSipCallRtp * pclsRtp );

	bool SelectLocalRtp( CSipCallRtp * pclsRtp );
	bool SelectRemoteRtp( CSipCallRtp * pclsRtp );

	void GetCdr( CSipCdr * pclsCdr );
	bool IsConnected( );

	static bool IsUseCodec( int iCodec );

	/** 원격(오퍼) 미디어의 rtpmap 에서 코덱 encoding-name(대소문자 무시, 예 "AMR-WB/16000")에
	 *  해당하는 payload type 을 찾는다 — RFC 3264: answer 는 코덱을 rtpmap 이름으로 식별하고
	 *  오퍼의 PT 를 그대로 echo 해야 한다(PT 번호는 dynamic 96-127, RFC 3551). 없으면 -1
	 *  (호출측이 하드코딩 fallback → 기존 VoLTE 동작 보존). */
	int FindRemotePayloadType( const char * pszEncoding );

private:
	CSipMessage * CreateMessage( const char * pszSipMethod );
};

#endif
