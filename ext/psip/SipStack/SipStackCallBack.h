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

#ifndef _SIP_STACK_CALLBACK_H_
#define _SIP_STACK_CALLBACK_H_

#include "SipStackDefine.h"

// SIP stack callback 인터페이스
class ISipStackCallBack
{
public:
	virtual ~ISipStackCallBack(){};

	// SIP 요청 메시지 수신 이벤트 핸들러
	virtual bool RecvRequest( int iThreadId, CSipMessage * pclsMessage ) = 0;

	// SIP 응답 메시지 수신 이벤트 핸들러
	virtual bool RecvResponse( int iThreadId, CSipMessage * pclsMessage ) = 0;

	// SIP 메시지 전송 timeout 이벤트 핸들러
	virtual bool SendTimeout( int iThreadId, CSipMessage * pclsMessage ) = 0;

	// TCP/TLS 세션 종료 이벤트 핸들러
	virtual void TcpSessionEnd( const char * pszIp, int iPort, ESipTransport eTransport ){};

	// SIP 메시지 수신 쓰레드가 종료됨을 알려주는 이벤트 핸들러
	virtual void ThreadEnd( int iThreadId ){};
};

// SIP stack 보안 callback 인터페이스
class ISipStackSecurityCallBack
{
public:
	virtual ~ISipStackSecurityCallBack(){};

	// SIP stack 에서 허용하는 SIP User Agent 인가?
	virtual bool IsAllowUserAgent( const char * pszSipUserAgent ) = 0;

	// SIP stack 에서 허용하지 않는 SIP User Agent 인가?
	virtual bool IsDenyUserAgent( const char * pszSipUserAgent ) = 0;

	// SIP stack 에서 허용하는 IP 주소인가?
	virtual bool IsAllowIp( const char * pszIp ) = 0;

	// SIP stack 에서 허용하지 않는 IP 주소인가?
	virtual bool IsDenyIp( const char * pszIp ) = 0;
};

#endif
