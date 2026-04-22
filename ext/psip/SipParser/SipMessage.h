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
#include "SipFrom.h"
#include "SipVia.h"
#include "SipAcceptData.h"
#include "SipCredential.h"
#include "SipChallenge.h"
#include "SipHeader.h"
#include "SipCSeq.h"
#include "SipCallId.h"
#include "SipContentType.h"

/**
 * @ingroup SipParser
 * @brief SIP �޽��� ������ �����ϴ� Ŭ����
 */
class CSipMessage
{
public:
	CSipMessage();
	~CSipMessage();

	/** SIP �޼ҵ� ( INVITE, CANCEL, ACK, BYE, REFER �� ) */
	std::string		m_strSipMethod;

	/** SIP request URI */
	CSipUri				m_clsReqUri;

	/** SIP version ( SIP/2.0 ) */
	std::string		m_strSipVersion;

	/** SIP ���� �ڵ�. SIP ���� �޽����� ��쿡�� 0 ���� ū ���� ������ �ִ�. */
	int						m_iStatusCode;

	/** SIP ���� �޽��� */
	std::string		m_strReasonPhrase;

	/** SIP From ��� */
	CSipFrom			m_clsFrom;

	/** SIP To ��� */
	CSipFrom			m_clsTo;

	/** SIP Via ��� ����Ʈ */
	SIP_VIA_LIST	m_clsViaList;

	/** SIP Contact ��� ����Ʈ */
	SIP_FROM_LIST	m_clsContactList;

	/** SIP Record-Route ��� ����Ʈ */
	SIP_FROM_LIST	m_clsRecordRouteList;

	/** SIP Route ��� ����Ʈ */
	SIP_FROM_LIST	m_clsRouteList;

#ifdef USE_ACCEPT_HEADER
	/** SIP Accept ��� ����Ʈ */
	SIP_CONTENT_TYPE_LIST	m_clsAcceptList;

	/** SIP Accept-Encoding ��� ����Ʈ */
	SIP_ACCEPT_DATA_LIST	m_clsAcceptEncodingList;

	/** SIP Accept-Language ��� ����Ʈ */
	SIP_ACCEPT_DATA_LIST	m_clsAcceptLanguageList;
#endif

	/** SIP Authorization ��� ����Ʈ */
	SIP_CREDENTIAL_LIST		m_clsAuthorizationList;

	/** SIP Www-Authenticate ��� ����Ʈ */
	SIP_CHALLENGE_LIST		m_clsWwwAuthenticateList;

	/** SIP Proxy-Authorization ��� ����Ʈ */
	SIP_CREDENTIAL_LIST		m_clsProxyAuthorizationList;

	/** SIP Proxy-Authenticate ��� ����Ʈ */
	SIP_CHALLENGE_LIST		m_clsProxyAuthenticateList;

	/** SIP ��� ����Ʈ. CSipMessage ���� �����Ͽ��� ������ ����� ������� �ʴ� ������� �����Ѵ�. */
	SIP_HEADER_LIST				m_clsHeaderList;

	/** SIP CSeq ��� */
	CSipCSeq				m_clsCSeq;

	/** SIP Call-ID ��� */
	CSipCallId			m_clsCallId;

	/** SIP Content-Type ��� */
	CSipContentType	m_clsContentType;

	/** SIP Content-Length ����� �� */
	int							m_iContentLength;

	/** SIP Expires ����� �� */
	int							m_iExpires;

	/** SIP Max-Forwards ����� �� */
	int							m_iMaxForwards;

	/** SIP User-Agent ��� */
	std::string			m_strUserAgent;

	/** SIP body �޽��� */
	std::string			m_strBody;

	/** ��Ʈ��ũ�� ������ SIP �޽��� */
	std::string			m_strPacket;

	/** ��Ʈ��ũ�� ����/���ŵ� SIP �޽����� transport */
	ESipTransport		m_eTransport;

	/** SIP �޽����� ������ Ŭ���̾�Ʈ�� IP �ּ� */
	std::string			m_strClientIp;

	/** SIP 메시지를 전송한 클라이언트의 포트 번호 */
	int							m_iClientPort;

	/** 수신 리스너 식별자 (CSP v3 확장, 2026-04-22).
	 *  UDP 수신 시 SipStack 이 해당 CSipStackUdpListener.m_iId 값을 세팅.
	 *  CSP LocalNodeMap 은 이 int 로 LocalNode 역조회 가능.
	 *  송신/미지정 시 -1. */
	int							m_iListenerId;

	/** SIP �޽����� compact form ���� �����ϴ��� ���� */
	bool						m_bUseCompact;

	/** ��ü ��� ���� */
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

	int GetExpires();

	CSipHeader * GetHeader( const char * pszName );

	CSipMessage * CreateResponse( int iStatus, const char * pszToTag = NULL );
	CSipMessage * CreateResponseWithToTag( int iStatus );

private:
	int ParseStatusLine( const char * pszText, int iTextLen );
	int ParseRequestLine( const char * pszText, int iTextLen );
};

#endif
