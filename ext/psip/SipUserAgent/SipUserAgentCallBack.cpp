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

#include "SipUserAgent.h"
#include "MemoryDebug.h"

// RTP IP/Port 를 수정한다.
void CSipCallRtp::SetIpPort( const char * pszIp, int iPort, int iSocketCountPerMedia )
{
	m_strIp = pszIp;
	m_iPort = iPort;

#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator itMedia;
	int iIndex = 0;

	for( itMedia = m_clsMediaList.begin(); itMedia != m_clsMediaList.end(); ++itMedia )
	{
		itMedia->m_iPort = m_iPort + iIndex * iSocketCountPerMedia;
		// 미디어 레벨 c= 는 세션 레벨 c= 를 덮어쓰므로(RFC 4566) relay 치환 시 제거한다.
		// 남겨두면 상대가 원본 사설 IP 로 RTP 를 보내 relay 에 미디어가 도달하지 않는다.
		itMedia->m_clsConnection.Clear();
		itMedia->DeleteAttribute( "rtcp" );

		++iIndex;
	}
#endif
}

// RTP 전송/수신 방향을 설정한다.
void CSipCallRtp::SetDirection( ERtpDirection eDirection )
{
	m_eDirection = eDirection;

#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator itMedia;
	const char * pszDirection = GetRtpDirectionString( eDirection );

	for( itMedia = m_clsMediaList.begin(); itMedia != m_clsMediaList.end(); ++itMedia )
	{
		itMedia->SetDirection( pszDirection );
	}
#endif
}

// 미디어 개수를 리턴한다.
int CSipCallRtp::GetMediaCount( )
{
	int iCount = 1;

#ifdef USE_MEDIA_LIST
	iCount = (int)m_clsMediaList.size();
	if( iCount == 0 ) iCount = 1;
#endif

	return iCount;
}

// 미디어 리스트에서 audio media 를 검색한 후, audio media 에 대한 포트 번호를 리턴한다.
int CSipCallRtp::GetAudioPort( )
{
	int iPort = -1;

#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator	itList;

	for( itList = m_clsMediaList.begin(); itList != m_clsMediaList.end(); ++itList )
	{
		if( !strcmp( itList->m_strMedia.c_str(), "audio" ) )
		{
			iPort = itList->m_iPort;
			break;
		}
	}
#endif

	return iPort;
}

// 미디어 리스트에서 video media 를 검색한 후, video media 에 대한 포트 번호를 리턴한다.
int CSipCallRtp::GetVideoPort( )
{
	int iPort = -1;

#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator	itList;

	for( itList = m_clsMediaList.begin(); itList != m_clsMediaList.end(); ++itList )
	{
		if( !strcmp( itList->m_strMedia.c_str(), "video" ) )
		{
			iPort = itList->m_iPort;
			break;
		}
	}
#endif

	return iPort;
}

/**
 * @ingroup SipUserAgent
 * @brief 미디어 리스트에서 application media(MCPTT floor control) 의 포트 번호를 검색한다.
 * @returns 성공하면 application media 의 포트 번호, 실패하면 -1.
 */
int CSipCallRtp::GetApplicationPort( )
{
	// 명시 설정된 floor 포트가 우선 (media list 파싱값보다 신뢰).
	if( m_iApplicationPort > 0 ) return m_iApplicationPort;

	int iPort = -1;

#ifdef USE_MEDIA_LIST
	SDP_MEDIA_LIST::iterator	itList;

	for( itList = m_clsMediaList.begin(); itList != m_clsMediaList.end(); ++itList )
	{
		if( !strcmp( itList->m_strMedia.c_str(), "application" ) )
		{
			iPort = itList->m_iPort;
			break;
		}
	}
#endif

	return iPort;
}
