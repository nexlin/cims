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

/**
 * @ingroup SipUserAgent
 * @brief RTP IP/Port �� �����Ѵ�.
 * @param pszIp IP �ּ�
 * @param iPort Port ��ȣ
 * @param iSocketCountPerMedia �̵��� �����ϴ� ���� ����
 */
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
		itMedia->DeleteAttribute( "rtcp" );

		++iIndex;
	}
#endif
}

/**
 * @ingroup SipUserAgent
 * @brief RTP ����/���� ������ �����Ѵ�.
 * @param eDirection RTP ����/���� ����
 */
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

/**
 * @ingroup SipUserAgent
 * @brief �̵�� ������ �����Ѵ�.
 * @returns �̵�� ������ �����Ѵ�.
 */
int CSipCallRtp::GetMediaCount( )
{
	int iCount = 1;

#ifdef USE_MEDIA_LIST
	iCount = (int)m_clsMediaList.size();
	if( iCount == 0 ) iCount = 1;
#endif

	return iCount;
}

/**
 * @ingroup SipUserAgent
 * @brief �̵�� ����Ʈ���� audio media �� �˻��� ��, audio media �� ���� ��Ʈ ��ȣ�� �����Ѵ�.
 * @returns �����ϸ� audio media �� ���� ��Ʈ ��ȣ�� �����ϰ� �����ϸ� -1 �� �����Ѵ�.
 */
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

/**
 * @ingroup SipUserAgent
 * @brief �̵�� ����Ʈ���� video media �� �˻��� ��, video media �� ���� ��Ʈ ��ȣ�� �����Ѵ�.
 * @returns �����ϸ� video media �� ���� ��Ʈ ��ȣ�� �����ϰ� �����ϸ� -1 �� �����Ѵ�.
 */
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
	// ���������� ������ floor ��Ʈ�� ������ �켱 (media list �̻�� �۽� ��ο�).
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
