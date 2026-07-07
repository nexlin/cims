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

#ifndef _RTP_THREAD_H_
#define _RTP_THREAD_H_

#include "SipUdp.h"
#include <vector>
#include <atomic>

class CRtpThread
{
public:
	CRtpThread();
	~CRtpThread();

	bool Create( );
	bool Destroy( );
	bool Start( const char * pszDestIp, int iDestPort );
	bool Stop( );

    bool SendFloorControl(int iOpCode);

    /** 미디어 파일 경로 (AMR-WB raw 프레임 파일) 설정 — 비어있으면 합성 RTP */
    void SetMediaFile(const std::string& strPath) { m_strMediaFile = strPath; }

    /** 비디오 파일 경로 (H.264 Annex B raw NAL 파일) 설정 */
    void SetVideoFile(const std::string& strPath) { m_strVideoFile = strPath; }

	Socket	m_hSocket;
	Socket	m_hRtcpSocket;       // RTCP 소켓 (RTP 포트 + 1)
	Socket  m_hFloorRecvSocket;  // floor 수신 소켓 (m=application)
	int		m_iPort;
	int     m_iFloorRecvPort;    // 로컬 floor 수신 포트
	bool	m_bStopEvent;
	bool	m_bSendThreadRun;
	bool	m_bRecvThreadRun;
    bool    m_bFloorRecvThreadRun;
	std::string	m_strDestIp;
	int		m_iDestPort;
	int		m_iDestFloorPort;    // 서버 floor 포트 (m=application, 0이면 미학습)
	int		m_iDestVideoPort;    // 서버 비디오 포트
    std::string m_strMediaFile;
    std::atomic<int>  m_iLastFloorOp;   // 마지막 수신 floor subtype (TS 24.380: 1=GRANTED,2=TAKEN,5=IDLE,...)
    std::atomic<bool> m_bGrantReceived; // GRANTED(subtype=1) 수신 여부 — TAKEN이 덮어써도 보존

    // Video RTP
    Socket  m_hVideoSocket;
    int     m_iVideoPort;
    bool    m_bVideoSendThreadRun;
    std::string m_strVideoFile;
};

#endif
