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

#ifndef _SIP_MESSAGE_H_
#define _SIP_MESSAGE_H_

#include "SipParserDefine.h"
#include <stdint.h>
#include "SipFrom.h"
#include "SipVia.h"
#include "SipAcceptData.h"
#include "SipCredential.h"
#include "SipChallenge.h"
#include "SipHeader.h"
#include "SipCSeq.h"
#include "SipCallId.h"
#include "SipContentType.h"

// SIP 메시지 정보를 저장하는 클래스
/** Expires(delta-seconds) 조회 결과 — 값의 해석(0=해지, 없음=서버 기본값, 상한)은 호출부 몫이다 */
enum ESipExpiresResult
{
	E_SIP_EXPIRES_ABSENT = 0,	// 헤더(REGISTER 는 Contact ;expires 포함) 없음 — 서버 기본값 적용 대상 (RFC 3261 §10.2.4)
	E_SIP_EXPIRES_VALID,		// 유효한 32bit 무부호 값
	E_SIP_EXPIRES_INVALID		// 형식 오류(비숫자·2^32 초과) — 400 Bad Request 대상 (RFC 3261 §21.4.1)
};

class CSipMessage
{
public:
	CSipMessage();
	~CSipMessage();

	// SIP 메소드 ( INVITE, CANCEL, ACK, BYE, REFER 등 )
	std::string		m_strSipMethod;

	/** SIP request URI */
	CSipUri				m_clsReqUri;

	/** SIP version ( SIP/2.0 ) */
	std::string		m_strSipVersion;

	// SIP 응답 코드. SIP 응답 메시지인 경우에만 0 보다 큰 값을 가지고 있다.
	int						m_iStatusCode;

	// SIP 응답 메시지
	std::string		m_strReasonPhrase;

	// SIP From 헤더
	CSipFrom			m_clsFrom;

	// SIP To 헤더
	CSipFrom			m_clsTo;

	// SIP Via 헤더 리스트
	SIP_VIA_LIST	m_clsViaList;

	// SIP Contact 헤더 리스트
	SIP_FROM_LIST	m_clsContactList;

	// SIP Record-Route 헤더 리스트
	SIP_FROM_LIST	m_clsRecordRouteList;

	// SIP Route 헤더 리스트
	SIP_FROM_LIST	m_clsRouteList;

#ifdef USE_ACCEPT_HEADER
	// SIP Accept 헤더 리스트
	SIP_CONTENT_TYPE_LIST	m_clsAcceptList;

	// SIP Accept-Encoding 헤더 리스트
	SIP_ACCEPT_DATA_LIST	m_clsAcceptEncodingList;

	// SIP Accept-Language 헤더 리스트
	SIP_ACCEPT_DATA_LIST	m_clsAcceptLanguageList;
#endif

	// SIP Authorization 헤더 리스트
	SIP_CREDENTIAL_LIST		m_clsAuthorizationList;

	// SIP Www-Authenticate 헤더 리스트
	SIP_CHALLENGE_LIST		m_clsWwwAuthenticateList;

	// SIP Proxy-Authorization 헤더 리스트
	SIP_CREDENTIAL_LIST		m_clsProxyAuthorizationList;

	// SIP Proxy-Authenticate 헤더 리스트
	SIP_CHALLENGE_LIST		m_clsProxyAuthenticateList;

	// SIP 헤더 리스트. CSipMessage 에서 구분하여서 정의한 헤더에 저장되지 않는 헤더들을 저장한다.
	SIP_HEADER_LIST				m_clsHeaderList;

	// SIP CSeq 헤더
	CSipCSeq				m_clsCSeq;

	// SIP Call-ID 헤더
	CSipCallId			m_clsCallId;

	// SIP Content-Type 헤더
	CSipContentType	m_clsContentType;

	// SIP Content-Length 헤더의 값
	int							m_iContentLength;

	// SIP Expires 헤더 (RFC 3261 §20.19 delta-seconds = 32bit 무부호 정수)
	//   m_bExpiresPresent: 헤더 존재 여부. 존재하면 m_bExpiresValid 가 숫자 파싱 성공 여부, m_uiExpires 가 값.
	//   (종전 int + "-1=미지정" 표지 구조는 2^31 이상 값이 -1 로 넘쳐 미지정과 겹치고 0(해지) 으로 접히는 결함이 있었다.)
	bool						m_bExpiresPresent;
	bool						m_bExpiresValid;
	uint32_t					m_uiExpires;

	// SIP Max-Forwards 헤더의 값
	int							m_iMaxForwards;

	// SIP User-Agent 헤더
	std::string			m_strUserAgent;

	// SIP body 메시지
	std::string			m_strBody;

	// 네트워크로 전송할 SIP 메시지
	std::string			m_strPacket;

	// 네트워크로 전송/수신된 SIP 메시지의 transport
	ESipTransport		m_eTransport;

	// SIP 메시지를 전송한 클라이언트의 IP 주소
	std::string			m_strClientIp;

	// SIP 메시지를 전송한 클라이언트의 포트 번호
	int							m_iClientPort;

	/** 수신 리스너 식별자 (CSP v3 확장, 2026-04-22).
	 *  UDP 수신 시 SipStack 이 해당 CSipStackUdpListener.m_iId 값을 세팅.
	 *  CSP LocalNodeMap 은 이 int 로 LocalNode 역조회 가능.
	 *  송신/미지정 시 -1. */
	int							m_iListenerId;

	/** 요청 전송 목적지 오버라이드 (CSP 확장).
	 *  세팅 시 Route/Request-URI 대신 이 주소로 전송한다 — NAT 뒤 단말처럼
	 *  등록 Contact(사설 주소)과 실제 도달 주소가 다를 때 헤더에 노출 없이 라우팅.
	 *  미지정(빈 문자열/0) 시 기존 동작. */
	std::string			m_strSendDestIp;
	int							m_iSendDestPort;

	// SIP 메시지를 compact form 으로 생성하는지 설정
	bool						m_bUseCompact;

	// 객체 사용 개수
	int8_t					m_iUseCount;

	int Parse( const char * pszText, int iTextLen );
	int ToString( char * pszText, int iTextSize );
	bool MakePacket();
	void Clear();

	bool IsRequest();
	bool IsMethod( const char * pszMethod );
	bool IsEqualCallId( CSipMessage * pclsMessage );
	bool IsEqualCallIdSeq( CSipMessage * pclsMessage );
	bool Is100rel( );

	bool GetCallId( std::string & strCallId );
	bool GetCallIdSeq( std::string & strCallId );

	bool AddIpPortToTopVia( const char * pszIp, int iPort, ESipTransport eTransport = E_SIP_UDP );
	bool AddVia( const char * pszIp, int iPort, const char * pszBranch = NULL, ESipTransport eTransport = E_SIP_UDP );
	bool AddRoute( const char * pszIp, int iPort, ESipTransport eTransport = E_SIP_UDP );
	bool AddRecordRoute( const char * pszIp, int iPort, ESipTransport eTransport = E_SIP_UDP );
	bool AddHeader( const char * pszName, const char * pszValue );
	bool AddHeader( const char * pszName, int iValue );

	bool GetTopViaIpPort( std::string & strIp, int & iPort );
	bool SetTopViaIpPort( const char * pszIp, int iPort, ESipTransport eTransport );
	bool SetTopViaTransPort( ESipTransport eTransport, int iPort );

	bool SetTopContactIpPort( const char * pszIp, int iPort, ESipTransport eTransport );

	/** Expires 헤더의 delta-seconds — SUBSCRIBE/PUBLISH 등 Expires 헤더만 의미를 갖는 요청용 */
	ESipExpiresResult GetExpires( uint32_t & uiExpires );
	/** REGISTER 의 바인딩 수명 — 첫 Contact 의 ;expires 가 Expires 헤더보다 우선 (RFC 3261 §10.2.1.1) */
	ESipExpiresResult GetRegisterExpires( uint32_t & uiExpires );
	/** 송신 메시지의 Expires 헤더 설정 */
	void SetExpires( uint32_t uiExpires );

	CSipHeader * GetHeader( const char * pszName );

	CSipMessage * CreateResponse( int iStatus, const char * pszToTag = NULL );
	CSipMessage * CreateResponseWithToTag( int iStatus );

private:
	int ParseStatusLine( const char * pszText, int iTextLen );
	int ParseRequestLine( const char * pszText, int iTextLen );
};

#endif
