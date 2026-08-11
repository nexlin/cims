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

#include "SipPlatformDefine.h"
#include "HttpHeader.h"
#include "MemoryDebug.h"

CHttpHeader::CHttpHeader()
{
}

CHttpHeader::CHttpHeader( const char * pszName, const char * pszValue )
{
	Set( pszName, pszValue );
}

CHttpHeader::~CHttpHeader()
{
}

// HTTP 헤더 문자열을 파싱하여 CHttpHeader 클래스의 멤버 변수에 저장한다.
int CHttpHeader::Parse( const char * pszText, int iTextLen )
{
	Clear();
	if( pszText == NULL || iTextLen <= 0 ) return -1;

	int		iPos, iValuePos;
	char	cType = 0;

	// parse header name & skip prefix space of header value
	for( iPos = 0; iPos < iTextLen; ++iPos )
	{
		if( cType == 0 )
		{
			if( pszText[iPos] == ':' || pszText[iPos] == ' ' || pszText[iPos] == '\t' )
			{
				m_strName.append( pszText, iPos );
				++cType;
			}
			else if( pszText[iPos] == '\r' )
			{
				if( iPos + 1 >= iTextLen || pszText[iPos+1] != '\n' ) return -1;
				return iPos + 2;
			}
		}
		else if( pszText[iPos] == ':' || pszText[iPos] == ' ' || pszText[iPos] == '\t' )
		{
			continue;
		}
		else
		{
			break;
		}
	}

	iValuePos = iPos;

	// parse header value
	for( ; iPos < iTextLen; ++iPos )
	{
		if( cType == 10 )
		{
			if( pszText[iPos] != ' ' && pszText[iPos] != '\t' )
			{
				cType = 11;
				--iPos;
				iValuePos = iPos;
			}
		}
		else if( pszText[iPos] == '\r' )
		{
			if( iValuePos != -1 )
			{
				m_strValue.append( pszText + iValuePos, iPos - iValuePos );
				iValuePos = -1;
			}

			++iPos;
			if( iPos == iTextLen || pszText[iPos] != '\n' ) return -1;
			
			++iPos;
			if( iPos == iTextLen ) break;

			if( pszText[iPos] == ' ' || pszText[iPos] == '\t' )
			{
				cType = 10;
				m_strValue.append( " " );
			}
			else
			{
				break;
			}
		}
	}

	return iPos;
}

// HTTP 메시지에 포함된 문자열을 작성한다.
int CHttpHeader::ToString( char * pszText, int iTextSize )
{
	if( pszText == NULL || iTextSize <= 0 ) return -1;

	if( m_strValue.empty() )
	{
		return snprintf( pszText, iTextSize, "%s: \r\n", m_strName.c_str() );
	}
	else
	{
		return snprintf( pszText, iTextSize, "%s: %s\r\n", m_strName.c_str(), m_strValue.c_str() );
	}
}

// 헤더의 이름과 값을 설정한다.
void CHttpHeader::Set( const char * pszName, const char * pszValue )
{
	m_strName = pszName;
	m_strValue = pszValue;
}

// 멤버 변수를 초기화시킨다.
void CHttpHeader::Clear()
{
	m_strName.clear();
	m_strValue.clear();
}
