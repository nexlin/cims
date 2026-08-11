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

#ifndef _HTTP_STACK_CALLBACK_H_
#define _HTTP_STACK_CALLBACK_H_

#include "HttpMessage.h"

class CHttpStackSession;

// HTTP 서버 callback 인터페이스
class IHttpStackCallBack
{
public:
	IHttpStackCallBack(){};
	virtual ~IHttpStackCallBack(){};

	// HTTP 요청 수신 이벤트 callback
	virtual bool RecvHttpRequest( CHttpMessage * pclsRequest, CHttpMessage * pclsResponse ) = 0;

	// WebSocket 클라이언트 TCP 연결 시작 이벤트 callback
	virtual void WebSocketConnected( const char * pszClientIp, int iClientPort ) = 0;

	// WebSocket 클라이언트 TCP 연결 종료 이벤트 callback
	virtual void WebSocketClosed( const char * pszClientIp, int iClientPort ) = 0;

	// WebSocket 클라이언트 데이터 수신 이벤트 callback
	virtual bool WebSocketData( const char * pszClientIp, int iClientPort, std::string & strData, CHttpStackSession * pclsSession ) = 0;
};

#endif
