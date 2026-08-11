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

#ifndef _TCP_STACK_CALLBACK_H_
#define _TCP_STACK_CALLBACK_H_

#include "TcpSessionList.h"

// CTcpStack 을 사용하는 응용 callback 인터페이스
class ITcpStackCallBack
{
public:
	virtual ~ITcpStackCallBack(){};

	// TCP 클라이언트가 연결 이벤트 핸들러
	virtual bool InComingConnected( CTcpSessionInfo * pclsSessionInfo ) = 0;

	// TCP 클라이언트 세션이 종료 이벤트 핸들러
	virtual void SessionClosed( CTcpSessionInfo * pclsSessionInfo ) = 0;

	// TCP 패킷 수신 이벤트 핸들러
	virtual bool RecvPacket( char * pszPacket, int iPacketLen, CTcpSessionInfo * pclsSessionInfo ) = 0;

	// SendAll 로 전송해도 되는 세션인지 검사한다.
	virtual bool IsSendAll( CTcpSessionInfo * pclsSessionInfo ){ return false; };

	// SendAll 로 전송할 때에 세션당 Send 함수 호출후, 호출되는 이벤트 핸들러
	virtual void AfterSendAllPerSession( CTcpSessionInfo * pclsSessionInfo, const char * pszPacket, int iPacketLen ){};
};

#endif
