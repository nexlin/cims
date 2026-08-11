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

/**
 * @ingroup SipStack
 * @brief SIP stack ¿¡ callback ÀÎÅÍÆäÀÌ½º¸¦ Ãß°¡ÇÑ´Ù.
 * @param pclsCallBack SIP stack ÀÇ callback ÀÎÅÍÆäÀÌ½º
 * @returns ¼º°øÇÏ¸é true ¸¦ ¸®ÅÏÇÏ°í ½ÇÆÐÇÏ¸é false ¸¦ ¸®ÅÏÇÑ´Ù.
 */
bool CSipStack::AddCallBack( ISipStackCallBack * pclsCallBack )
{
	if( pclsCallBack == NULL ) return false;

	SIP_STACK_CALLBACK_LIST::iterator	it;
	bool	bFound = false;

	for( it = m_clsCallBackList.begin(); it != m_clsCallBackList.end(); ++it )
	{
		if( *it == pclsCallBack )
		{
			bFound = true;
			break;
		}
	}

	if( bFound == false )
	{
		m_clsCallBackList.push_back( pclsCallBack );
	}

	return true;
}

/**
 * @ingroup SipStack
 * @brief SIP stack ¿¡ callback ÀÎÅÍÆäÀÌ½º¸¦ »èÁ¦ÇÑ´Ù.
 * @param pclsCallBack SIP stack ÀÇ callback ÀÎÅÍÆäÀÌ½º
 * @returns ¼º°øÇÏ¸é true ¸¦ ¸®ÅÏÇÏ°í ½ÇÆÐÇÏ¸é false ¸¦ ¸®ÅÏÇÑ´Ù.
 */
bool CSipStack::DeleteCallBack( ISipStackCallBack * pclsCallBack )
{
	SIP_STACK_CALLBACK_LIST::iterator	it;
	bool	bFound = false;

	for( it = m_clsCallBackList.begin(); it != m_clsCallBackList.end(); ++it )
	{
		if( *it == pclsCallBack )
		{
			m_clsCallBackList.erase( it );
			bFound = true;
			break;
		}
	}

	return bFound;
}

/**
 * @ingroup SipStack
 * @brief SIP stack ÀÇ º¸¾È ±â´ÉÀ» ¼öÇàÇÒ callback ÀÎÅÍÆäÀÌ½º¸¦ µî·ÏÇÑ´Ù.
 * @param pclsSecurityCallBack 
 */
void CSipStack::SetSecurityCallBack( ISipStackSecurityCallBack * pclsSecurityCallBack )
{
	m_pclsSecurityCallBack = pclsSecurityCallBack;
}

/**
 * @ingroup SipStack
 * @brief ¼ö½ÅµÈ ¿äÃ» SIP ¸Þ½ÃÁö¿¡ ´ëÇÑ callback ¸Þ¼Òµå¸¦ È£ÃâÇÑ´Ù.
 *				¸¸¾à ¿äÃ» SIP ¸Þ½ÃÁö¸¦ Ã³¸®ÇÒ callback ÀÌ Á¸ÀçÇÏÁö ¾ÊÀ¸¸é 501 ÀÀ´ä ¸Þ½ÃÁö¸¦ Àü¼ÛÇÑ´Ù.
 * @param iThreadId		¾²·¹µå ¾ÆÀÌµð ( 0 ºÎÅÍ ¾²·¹µå °³¼ö )
 * @param pclsMessage SIP ¸Þ½ÃÁö ÀúÀå ±¸Á¶Ã¼
 */
void CSipStack::RecvRequest( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;
	bool	bSendResponse = false;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		if( (*itList)->RecvRequest( iThreadId, pclsMessage ) )
		{
			bSendResponse = true;
			break;
		}
	}

	if( bSendResponse == false )
	{
		CSipMessage * psttResponse = pclsMessage->CreateResponseWithToTag( SIP_NOT_IMPLEMENTED );
		if( psttResponse )
		{
			SendSipMessage( psttResponse );
		}
	}
}

/**
 * @ingroup SipStack
 * @brief ¼ö½ÅµÈ ÀÀ´ä SIP ¸Þ½ÃÁö¿¡ ´ëÇÑ callback ¸Þ¼Òµå¸¦ È£ÃâÇÑ´Ù.
 * @param iThreadId		¾²·¹µå ¾ÆÀÌµð ( 0 ºÎÅÍ ¾²·¹µå °³¼ö )
 * @param pclsMessage SIP ¸Þ½ÃÁö ÀúÀå ±¸Á¶Ã¼
 */
void CSipStack::RecvResponse( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	// ìˆ˜ì‹  ìµœì¢…ì‘ë‹µ ì¹´ìš´í„° â€” ì™€ì´ì–´ ì‘ë‹µ(íŠ¸ëžœìž­ì…˜ dedup í›„)ê³¼ íŠ¸ëžœìž­ì…˜ ë¡œì»¬ í•©ì„± ì‘ë‹µ
	//   (408 Timer B/Ring timeout, 660 connect error)ì´ ì „ë¶€ ì´ íŒ¬ì•„ì›ƒì„ ì§€ë‚œë‹¤.
	//   í•©ì„± ì‘ë‹µì€ ì™€ì´ì–´ì— ì—†ì–´ flow ë¡œê·¸ ì‚¬ê° â€” ì„±ê³µë¥  ì§‘ê³„ì— ì—¬ê¸° í¬í•¨ì´ í•„ìˆ˜.
	if( pclsMessage->m_iStatusCode >= CSipStackCounter::FINAL_MIN )
		m_clsCounter.OnRecvFinal( pclsMessage->m_clsCSeq.m_strMethod.c_str(), pclsMessage->m_iStatusCode );

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		if( (*itList)->RecvResponse( iThreadId, pclsMessage ) ) break;
	}
}

/**
 * @ingroup SipStack
 * @brief Àü¼Û SIP ¸Þ½ÃÁö¿¡ ´ëÇÑ timeout callback ¸Þ¼Òµå¸¦ È£ÃâÇÑ´Ù.
 * @param iThreadId		¾²·¹µå ¾ÆÀÌµð ( 0 ºÎÅÍ ¾²·¹µå °³¼ö )
 * @param pclsMessage SIP ¸Þ½ÃÁö ÀúÀå ±¸Á¶Ã¼
 */
void CSipStack::SendTimeout( int iThreadId, CSipMessage * pclsMessage )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		if( (*itList)->SendTimeout( iThreadId, pclsMessage ) ) break;
	}
}

/**
 * @ingroup SipStack
 * @brief TCP/TLS ¼¼¼Ç Á¾·á¿¡ ´ëÇÑ callback ¸Þ¼Òµå¸¦ È£ÃâÇÑ´Ù.
 * @param pszIp IP ÁÖ¼Ò
 * @param iPort Æ÷Æ® ¹øÈ£
 * @param eTransport ÇÁ·ÎÅäÄÝ
 */
void CSipStack::TcpSessionEnd( const char * pszIp, int iPort, ESipTransport eTransport )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		(*itList)->TcpSessionEnd( pszIp, iPort, eTransport );
	}
}

/**
 * @ingroup SipStack
 * @brief ¾²·¹µå Á¾·á ÀÌº¥Æ®¸¦ Àü´ÞÇÑ´Ù.
 * @param iThreadId ¾²·¹µå ¾ÆÀÌµð ( 0 ºÎÅÍ ¾²·¹µå °³¼ö )
 */
void CSipStack::ThreadEnd( int iThreadId )
{
	SIP_STACK_CALLBACK_LIST::iterator itList;

	for( itList = m_clsCallBackList.begin(); itList != m_clsCallBackList.end(); ++itList )
	{
		(*itList)->ThreadEnd( iThreadId );
	}
}
