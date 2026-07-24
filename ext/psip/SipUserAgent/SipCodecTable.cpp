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

#include "SipCodecTable.h"

#include <stdio.h>
#include <string.h>

#include "Log.h"

CSipCodecEntry::CSipCodecEntry() : m_iPt( -1 ), m_iClockRate( 8000 ), m_iChannels( 0 ), m_iPtime( 0 )
{
}

std::string CSipCodecEntry::GetRtpmap() const
{
	char szBuf[128];

	if( m_iChannels > 0 )
	{
		snprintf( szBuf, sizeof(szBuf), "%s/%d/%d", m_strName.c_str(), m_iClockRate, m_iChannels );
	}
	else
	{
		snprintf( szBuf, sizeof(szBuf), "%s/%d", m_strName.c_str(), m_iClockRate );
	}

	return szBuf;
}

std::string CSipCodecEntry::GetMatchPrefix() const
{
	char szBuf[128];

	snprintf( szBuf, sizeof(szBuf), "%s/%d", m_strName.c_str(), m_iClockRate );

	return szBuf;
}

static bool _IsTelephoneEvent( const CSipCodecEntry & clsEntry )
{
	return strcasecmp( clsEntry.m_strName.c_str(), "telephone-event" ) == 0;
}

static CSipCodecEntry _MakeEntry( int iPt, const char * pszName, int iClockRate, int iChannels
	, const char * pszFmtp, int iPtime )
{
	CSipCodecEntry clsEntry;

	clsEntry.m_iPt = iPt;
	clsEntry.m_strName = pszName;
	clsEntry.m_iClockRate = iClockRate;
	clsEntry.m_iChannels = iChannels;
	if( pszFmtp ) clsEntry.m_strFmtp = pszFmtp;
	clsEntry.m_iPtime = iPtime;

	return clsEntry;
}

/** 기본 코덱 테이블 — 응용이 Set() 을 호출하지 않았을 때의 CIMS 표준 동작.
 *  - AMR-WB PT=96 최우선: CSP 가 오퍼러(PTT fan-out)일 때 그룹 wire PT 가 되는 값 —
 *    실단말(pjsua)의 로컬 AMR-WB PT(96, pjmedia 동적 PT 재배정)와 정렬된 실증값이다.
 *  - 이하 정적 코덱들은 구 IsUseCodec 화이트리스트(0/3/4/8/18) 승계.
 *  - AMR(협대역) 98 은 구 하드코딩 승계 (fmtp octet-align=1 포함). */
static SIP_CODEC_ENTRY_LIST _MakeDefaultList()
{
	SIP_CODEC_ENTRY_LIST clsList;

	clsList.push_back( _MakeEntry( 96, "AMR-WB", 16000, 1, "octet-align=1", 20 ) );
	clsList.push_back( _MakeEntry( 98, "AMR", 8000, 1, "octet-align=1", 0 ) );
	clsList.push_back( _MakeEntry( 0, "PCMU", 8000, 0, NULL, 0 ) );
	clsList.push_back( _MakeEntry( 8, "PCMA", 8000, 0, NULL, 0 ) );
	clsList.push_back( _MakeEntry( 3, "GSM", 8000, 0, NULL, 0 ) );
	clsList.push_back( _MakeEntry( 4, "G723", 8000, 0, NULL, 0 ) );
	clsList.push_back( _MakeEntry( 18, "G729", 8000, 0, NULL, 0 ) );

	return clsList;
}

static CSipCodecEntry _MakeDefaultTelephoneEvent()
{
	return _MakeEntry( 101, "telephone-event", 8000, 0, "0-15", 0 );
}

static SIP_CODEC_ENTRY_LIST & _List()
{
	static SIP_CODEC_ENTRY_LIST clsList = _MakeDefaultList();
	return clsList;
}

static CSipCodecEntry & _TelephoneEvent()
{
	static CSipCodecEntry clsEntry = _MakeDefaultTelephoneEvent();
	return clsEntry;
}

void CSipCodecTable::Set( const SIP_CODEC_ENTRY_LIST & clsList )
{
	SIP_CODEC_ENTRY_LIST clsCodecList;
	CSipCodecEntry clsTelephoneEvent;
	bool bTelephoneEvent = false;

	SIP_CODEC_ENTRY_LIST::const_iterator itList;

	for( itList = clsList.begin(); itList != clsList.end(); ++itList )
	{
		if( itList->m_strName.empty() || itList->m_iPt < 0 || itList->m_iPt > 127 )
		{
			CLog::Print( LOG_ERROR, "CSipCodecTable::Set invalid entry name(%s) pt(%d) — skip"
				, itList->m_strName.c_str(), itList->m_iPt );
			continue;
		}

		if( _IsTelephoneEvent( *itList ) )
		{
			clsTelephoneEvent = *itList;
			bTelephoneEvent = true;
		}
		else
		{
			clsCodecList.push_back( *itList );
		}
	}

	if( clsCodecList.empty() )
	{
		CLog::Print( LOG_ERROR, "CSipCodecTable::Set no codec entry — keep current table" );
		return;
	}

	_List() = clsCodecList;
	if( bTelephoneEvent ) _TelephoneEvent() = clsTelephoneEvent;

	CLog::Print( LOG_INFO, "CSipCodecTable::Set %d codecs top(%s/%d pt=%d) telephone-event pt=%d"
		, (int)clsCodecList.size(), clsCodecList[0].m_strName.c_str(), clsCodecList[0].m_iClockRate
		, clsCodecList[0].m_iPt, _TelephoneEvent().m_iPt );
}

const SIP_CODEC_ENTRY_LIST & CSipCodecTable::GetList()
{
	return _List();
}

const CSipCodecEntry & CSipCodecTable::GetTop()
{
	return _List()[0];
}

const CSipCodecEntry & CSipCodecTable::GetTelephoneEvent()
{
	return _TelephoneEvent();
}

const CSipCodecEntry * CSipCodecTable::FindByPt( int iPt )
{
	SIP_CODEC_ENTRY_LIST & clsList = _List();
	SIP_CODEC_ENTRY_LIST::iterator itList;

	for( itList = clsList.begin(); itList != clsList.end(); ++itList )
	{
		if( itList->m_iPt == iPt ) return &(*itList);
	}

	return NULL;
}

const CSipCodecEntry * CSipCodecTable::FindByRtpmap( const char * pszEncoding )
{
	if( pszEncoding == NULL ) return NULL;

	SIP_CODEC_ENTRY_LIST & clsList = _List();
	SIP_CODEC_ENTRY_LIST::iterator itList;

	for( itList = clsList.begin(); itList != clsList.end(); ++itList )
	{
		std::string strPrefix = itList->GetMatchPrefix();
		size_t iLen = strPrefix.length();

		// "<enc>/<clock>" prefix 일치 + 이후는 끝 또는 채널 표기("/N")만 허용
		if( strncasecmp( pszEncoding, strPrefix.c_str(), iLen ) == 0 &&
			( pszEncoding[iLen] == '\0' || pszEncoding[iLen] == '/' ) )
		{
			return &(*itList);
		}
	}

	return NULL;
}

int CSipCodecTable::GetRank( int iPt )
{
	SIP_CODEC_ENTRY_LIST & clsList = _List();

	for( size_t i = 0; i < clsList.size(); ++i )
	{
		if( clsList[i].m_iPt == iPt ) return (int)i;
	}

	return -1;
}
