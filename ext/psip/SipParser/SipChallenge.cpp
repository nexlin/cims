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
#include "SipChallenge.h"
#include "StringUtility.h"
#include "MemoryDebug.h"

CSipChallenge::CSipChallenge()
{
}

CSipChallenge::~CSipChallenge()
{
}

/**
 * @ingroup SipParser
 * @brief SIP Çì´õ ¹®ÀÚ¿­À» ÆÄ½ÌÇÏ¿© CSipChallenge Å¬·¡½ºÀÇ ¸â¹ö º¯¼ö¿¡ ÀúÀåÇÑ´Ù.
 * @param pszText		SIP Çì´õÀÇ °ªÀ» ÀúÀåÇÑ ¹®ÀÚ¿­
 * @param iTextLen	pszText ¹®ÀÚ¿­ÀÇ ±æÀÌ
 * @returns ¼º°øÇÏ¸é ÆÄ½ÌÇÑ ±æÀÌ¸¦ ¸®ÅÏÇÏ°í ±×·¸Áö ¾ÊÀ¸¸é -1 ¸¦ ¸®ÅÏÇÑ´Ù.
 */
int CSipChallenge::Parse( const char * pszText, int iTextLen )
{
	Clear();
	if( pszText == NULL || iTextLen <= 0 ) return -1;

	int iPos, iCurPos;

	for( iPos = 0; iPos < iTextLen; ++iPos )
	{
		if( pszText[iPos] == ' ' || pszText[iPos] == '\t' )
		{
			m_strType.append( pszText, iPos );
			++iPos;
			break;
		}
	}

	if( m_strType.empty() ) return -1;
	iCurPos = iPos;

	SIP_PARAMETER_LIST	clsList;
	SIP_PARAMETER_LIST::iterator	itList;
	
	while( iCurPos < iTextLen )
	{
		if( pszText[iCurPos] == ' ' || pszText[iCurPos] == '\t' || pszText[iCurPos] == ',' )
		{
			++iCurPos;
			continue;
		}

		iPos = ParseSipParameter( clsList, pszText + iCurPos, iTextLen - iCurPos );
		if( iPos == -1 ) return -1;
		iCurPos += iPos;
	}

	for( itList = clsList.begin(); itList != clsList.end(); ++itList )
	{
		const char * pszName = itList->m_strName.c_str();

		if( !strcmp( pszName, "realm" ) )
		{
			DeQuoteString( itList->m_strValue, m_strRealm );
		}
		else if( !strcmp( pszName, "domain" ) )
		{
			DeQuoteString( itList->m_strValue, m_strDomain );
		}
		else if( !strcmp( pszName, "nonce" ) )
		{
			DeQuoteString( itList->m_strValue, m_strNonce );
		}
		else if( !strcmp( pszName, "opaque" ) )
		{
			DeQuoteString( itList->m_strValue, m_strOpaque );
		}
		else if( !strcmp( pszName, "stale" ) )
		{
			m_strStale = itList->m_strValue;
		}
		else if( !strcmp( pszName, "algorithm" ) )
		{
			m_strAlgorithm = itList->m_strValue;
		}
		else if( !strcmp( pszName, "qop" ) )
		{
			DeQuoteString( itList->m_strValue, m_strQop );
		}
		else
		{
			m_clsParamList.push_back( *itList );
		}
	}

	return iCurPos;
}

/**
 * @ingroup SipParser
 * @brief SIP ¸Þ½ÃÁö¿¡ Æ÷ÇÔµÈ ¹®ÀÚ¿­À» ÀÛ¼ºÇÑ´Ù.
 * @param pszText		SIP Çì´õÀÇ °ªÀ» ÀúÀåÇÒ ¹®ÀÚ¿­ º¯¼ö
 * @param iTextSize	pszText º¯¼öÀÇ Å©±â
 * @returns ¼º°øÇÏ¸é ÀÛ¼ºÇÑ ¹®ÀÚ¿­ ±æÀÌ¸¦ ¸®ÅÏÇÏ°í ±×·¸Áö ¾ÊÀ¸¸é -1 ¸¦ ¸®ÅÏÇÑ´Ù.
 */
int CSipChallenge::ToString( char * pszText, int iTextSize )
{
	if( pszText == NULL || iTextSize <= 0 ) return -1;

	int iLen = 0;

	iLen = snprintf( pszText, iTextSize, "%s ", m_strType.c_str() );
	char * pszData = pszText + iLen;
	int	iPos = 0;
	iTextSize -= iLen;

	if( SetQuoteString( pszData, iPos, iTextSize, "realm", m_strRealm ) == false ) return -1;
	if( SetQuoteString( pszData, iPos, iTextSize, "domain", m_strDomain ) == false ) return -1;
	if( SetQuoteString( pszData, iPos, iTextSize, "nonce", m_strNonce ) == false ) return -1;
	if( SetQuoteString( pszData, iPos, iTextSize, "opaque", m_strOpaque ) == false ) return -1;
	if( SetString( pszData, iPos, iTextSize, "stale", m_strStale ) == false ) return -1;
	if( SetString( pszData, iPos, iTextSize, "algorithm", m_strAlgorithm ) == false ) return -1;
	// RFC 2617/7616: ì±Œë¦°ì§€ì˜ qop-options ëŠ” quoted-string (qop="auth").
	//   ë¹„ì¸ìš© qop=auth ëŠ” ì—„ê²©í•œ ë‹¨ë§ íŒŒì„œê°€ ì±Œë¦°ì§€ ë¬´ì‹œ â†’ ì¸ì¦ ì§„í–‰ ë¶ˆê°€.
	if( SetQuoteString( pszData, iPos, iTextSize, "qop", m_strQop ) == false ) return -1;

	SIP_PARAMETER_LIST::iterator	itList;

	for( itList = m_clsParamList.begin(); itList != m_clsParamList.end(); ++itList )
	{
		if( SetString( pszData, iPos, iTextSize, itList->m_strName.c_str(), itList->m_strValue ) == false ) return -1;
	}

	return iLen + iPos;
}

/**
 * @ingroup SipParser
 * @brief ¸â¹ö º¯¼ö¸¦ ÃÊ±âÈ­½ÃÅ²´Ù.
 */
void CSipChallenge::Clear()
{
	m_strType.clear();
	m_strRealm.clear();
	m_strDomain.clear();
	m_strNonce.clear();
	m_strOpaque.clear();
	m_strStale.clear();
	m_strAlgorithm.clear();
	m_strQop.clear();
}

/**
 * @ingroup SipParser
 * @brief SIP challenge ¹®ÀÚ¿­¿¡ ÀÌ¸§, °ªÀ» ÀúÀåÇÑ´Ù.
 * @param pszText		SIP chanllenge ¹®ÀÚ¿­
 * @param iTextPos	SIP chanllenge ¹®ÀÚ¿­ º¯¼öÀÇ ¾²±â À§Ä¡
 * @param iTextSize SIP chanllenge ¹®ÀÚ¿­ º¯¼ö Å©±â
 * @param pszName		ÀÌ¸§
 * @param strValue	°ª
 * @returns ¼º°øÇÏ¸é true ¸¦ ¸®ÅÏÇÏ°í ±×·¸Áö ¾ÊÀ¸¸é false ¸¦ ¸®ÅÏÇÑ´Ù.
 */
bool CSipChallenge::SetString( char * pszText, int & iTextPos, int iTextSize, const char * pszName, std::string & strValue )
{
	if( strValue.empty() ) return true;

	int iLen = 0;

	if( iTextPos > 0 )
	{
		iLen = snprintf( pszText + iTextPos, iTextSize, ", " );
		if( iLen != 2 ) return false;
		iTextPos += iLen;
	}

	iLen = snprintf( pszText + iTextPos, iTextSize, "%s=%s", pszName, strValue.c_str() );
	if( iLen <= 0 ) return false;
	iTextPos += iLen;

	return true;
}

/**
 * @ingroup SipParser
 * @brief SIP challenge ¹®ÀÚ¿­¿¡ ÀÌ¸§, " °¡ Æ÷ÇÔµÈ °ªÀ» ÀúÀåÇÑ´Ù.
 * @param pszText		SIP chanllenge ¹®ÀÚ¿­
 * @param iTextPos	SIP chanllenge ¹®ÀÚ¿­ º¯¼öÀÇ ¾²±â À§Ä¡
 * @param iTextSize SIP chanllenge ¹®ÀÚ¿­ º¯¼ö Å©±â
 * @param pszName		ÀÌ¸§
 * @param strValue	°ª
 * @returns ¼º°øÇÏ¸é true ¸¦ ¸®ÅÏÇÏ°í ±×·¸Áö ¾ÊÀ¸¸é false ¸¦ ¸®ÅÏÇÑ´Ù.
 */
bool CSipChallenge::SetQuoteString( char * pszText, int & iTextPos, int iTextSize, const char * pszName, std::string & strValue )
{
	if( strValue.empty() ) return true;

	int iLen = 0;

	if( iTextPos > 0 )
	{
		iLen = snprintf( pszText + iTextPos, iTextSize, ", " );
		if( iLen != 2 ) return false;
		iTextPos += iLen;
	}

	iLen = snprintf( pszText + iTextPos, iTextSize, "%s=\"%s\"", pszName, strValue.c_str() );
	if( iLen <= 0 ) return false;
	iTextPos += iLen;

	return true;
}

/**
 * @ingroup SipParser
 * @brief SIP challenge ¹®ÀÚ¿­À» ÆÄ½ÌÇÏ¿©¼­ challenge ¸®½ºÆ®¿¡ ÀúÀåÇÑ´Ù.
 * @param clsList		challenge ¸®½ºÆ®
 * @param pszText		SIP challenge ¹®ÀÚ¿­
 * @param iTextLen	SIP challenge ¹®ÀÚ¿­ ±æÀÌ 
 * @returns ¼º°øÇÏ¸é ÆÄ½ÌÇÑ ¹®ÀÚ¿­ ±æÀÌ¸¦ ¸®ÅÏÇÏ°í ±×·¸Áö ¾ÊÀ¸¸é -1 À» ¸®ÅÏÇÑ´Ù.
 */
int ParseSipChallenge( SIP_CHALLENGE_LIST & clsList, const char * pszText, int iTextLen )
{
	int iPos;
	CSipChallenge	clsChallenge;

	iPos = clsChallenge.Parse( pszText, iTextLen );
	if( iPos == -1 ) return -1;

	clsList.push_back( clsChallenge );
	return iPos;
}
