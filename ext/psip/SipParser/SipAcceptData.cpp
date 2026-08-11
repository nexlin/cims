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

#include "SipParserDefine.h"
#include "SipAcceptData.h"
#include "MemoryDebug.h"

CSipAcceptData::CSipAcceptData()
{
}

CSipAcceptData::~CSipAcceptData()
{
}

// SIP 헤더 문자열을 파싱하여 CSipAcceptData 클래스의 멤버 변수에 저장한다.
int CSipAcceptData::Parse( const char * pszText, int iTextLen )
{
	Clear();
	if( pszText == NULL || iTextLen <= 0 ) return -1;

	int		iPos, iCurPos;
	bool	bParam = false;

	for( iPos = 0; iPos < iTextLen; ++iPos )
	{
		if( pszText[iPos] == ';' )
		{
			m_strName.append( pszText, iPos );
			bParam = true;
			break;
		}
		else if( pszText[iPos] == ',' )
		{
			m_strName.append( pszText, iPos );
			break;
		}
	}

	iCurPos = iPos;

	if( m_strName.empty() )
	{
		m_strName.append( pszText, iCurPos );
	}

	if( bParam )
	{
		int iRet = HeaderListParamParse( pszText + iCurPos, iTextLen - iCurPos );
		if( iRet == -1 ) return -1;
		iCurPos += iRet;
	}

	return iCurPos;
}

// SIP 메시지에 포함된 문자열을 작성한다.
int CSipAcceptData::ToString( char * pszText, int iTextSize )
{
	if( pszText == NULL || iTextSize <= 0 ) return -1;

	int iLen, iPos;

	iLen = snprintf( pszText, iTextSize, "%s", m_strName.c_str() );

	iPos = ParamToString( pszText + iLen, iTextSize - iLen );
	if( iPos == -1 ) return -1;
	iLen += iPos;

	return iLen;
}

// 멤버 변수를 초기화시킨다.
void CSipAcceptData::Clear()
{
	m_strName.clear();
	ClearParam();
}

// SIP Accept-Data 헤더를 파싱한다.
int ParseSipAcceptData( SIP_ACCEPT_DATA_LIST & clsList, const char * pszText, int iTextLen )
{
	int iPos, iCurPos = 0;
	CSipAcceptData	clsAccept;

	while( iCurPos < iTextLen )
	{
		if( pszText[iCurPos] == ' ' || pszText[iCurPos] == '\t' || pszText[iCurPos] == ',' )
		{
			++iCurPos;
			continue;
		}

		iPos = clsAccept.Parse( pszText + iCurPos, iTextLen - iCurPos );
		if( iPos == -1 ) return -1;
		iCurPos += iPos;

		clsList.push_back( clsAccept );
	}

	return iCurPos;
}
