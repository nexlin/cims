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

LRESULT CSipSendDlg::OnSipMessage( WPARAM wParam, LPARAM lParam )
{
	
	return 0;
}

// SIP REGISTER 응답 메시지 수신 이벤트 핸들러
void CSipSendDlg::EventRegister( CSipServerInfo * pclsInfo, int iStatus )
{
}

// SIP 통화 요청 수신 이벤트 핸들러
void CSipSendDlg::EventIncomingCall( const char * pszCallId, const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp )
{
	gclsSipUserAgent.StopCall( pszCallId, SIP_BUSY_HERE );
}

// SIP Ring / Session Progress 수신 이벤트 핸들러
void CSipSendDlg::EventCallRing( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRtp )
{
}

// SIP 통화 연결 이벤트 핸들러
void CSipSendDlg::EventCallStart( const char * pszCallId, CSipCallRtp * pclsRtp )
{

}

// SIP 통화 종료 이벤트 핸들러
void CSipSendDlg::EventCallEnd( const char * pszCallId, int iSipStatus )
{
	
}

// SIP 요청 메시지 수신 이벤트 핸들러
bool CSipSendDlg::RecvRequest( int iThreadId, CSipMessage * pclsMessage )
{
	return false;
}

// SIP 응답 메시지 수신 이벤트 핸들러
bool CSipSendDlg::RecvResponse( int iThreadId, CSipMessage * pclsMessage )
{
	return false;
}

// SIP 메시지 전송 timeout 이벤트 핸들러
bool CSipSendDlg::SendTimeout( int iThreadId, CSipMessage * pclsMessage )
{
	return false;
}
