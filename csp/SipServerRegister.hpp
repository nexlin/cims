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
#include "GroupCallService.h"

/**
 * @ingroup CspServer
 * @brief SIP 응답 메시지에 WWW-Authenticate 헤더를 추가한다.
 * @param psttResponse SIP 응답 메시지
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool AddChallenge( CSipMessage * psttResponse )
{
	CSipChallenge clsChallenge;

	char	szNonce[33];

	if( gclsNonceMap.GetNewValue( szNonce, sizeof(szNonce) ) == false )
	{
		CLog::Print( LOG_ERROR, "gclsNonce.GetNewValue() error" );
		return false;
	}

	clsChallenge.m_strType = "Digest";
	clsChallenge.m_strNonce = szNonce;
	clsChallenge.m_strRealm = gclsSetup.m_strRealm;
	clsChallenge.m_strQop = "auth";

	psttResponse->m_clsWwwAuthenticateList.push_back( clsChallenge );

  return true;
}

/**
 * @ingroup CspServer
 * @brief SIP 401 응답 메시지를 전송한다.
 * @param pclsMessage SIP 요청 메시지
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool SendUnAuthorizedResponse( CSipMessage * pclsMessage )
{
	CSipMessage * pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_UNAUTHORIZED );
	if( pclsResponse == NULL ) return false;

	AddChallenge( pclsResponse );

	gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );

	return true;
}

/**
 * @ingroup CspServer
 * @brief	response 가 유효한 값인지 검사한다.
 * @param	pszUserName	사용자 아이디
 * @param	pszRealm		realm
 * @param	pszNonce		nonce
 * @param	pszUri			uri
 * @param	pszResponse	response
 * @param	pszPassWord	비밀번호
 * @param	pszMethod		SIP 메소드
 * @return	response 문자열이 유효하면 true 를 리턴한다. 그렇지 않으면 false 를 리턴한다.
 */
bool CheckAuthorizationResponse( const char * pszUserName
		, const char * pszRealm
		, const char * pszNonce
		, const char * pszUri
		, const char * pszResponse
		, const char * pszPassWord
		, const char * pszMethod
		, const char * pszQop
		, const char * pszNc
		, const char * pszCnonce )
{
	char	szA1[301], szA2[201], szMd5[33], szResponse[1024];

	snprintf( szA1, sizeof(szA1), "%s:%s:%s", pszUserName, pszRealm, pszPassWord );
	SipMd5String( szA1, szMd5 );
	snprintf( szA1, sizeof(szA1), "%s", szMd5 );

	snprintf( szA2, sizeof(szA2), "%s:%s", pszMethod, pszUri );
	SipMd5String( szA2, szMd5 );
	snprintf( szA2, sizeof(szA2), "%s", szMd5 );

	if( pszQop && strcasecmp( pszQop, "auth" ) == 0 )
	{
		// RFC 2617: qop=auth → HA1:nonce:nc:cnonce:qop:HA2
		snprintf( szResponse, sizeof(szResponse), "%s:%s:%s:%s:%s:%s",
			szA1, pszNonce, pszNc ? pszNc : "", pszCnonce ? pszCnonce : "", pszQop, szA2 );
	}
	else
	{
		// no qop → HA1:nonce:HA2
		snprintf( szResponse, sizeof(szResponse), "%s:%s:%s", szA1, pszNonce, szA2 );
	}

	SipMd5String( szResponse, szMd5 );
	snprintf( szResponse, sizeof(szResponse), "%s", szMd5 );

	if( strcmp( szResponse, pszResponse ) )
	{
		CLog::Print( LOG_ERROR, "response[%s] is not correct. correct response is [%s]", pszResponse, szResponse );
		return false;
	}

	return true;
}

/**
 * @ingroup CspServer
 * @brief SIP 인증 결과
 */
enum ECheckAuthResult
{
	E_AUTH_OK = 0,
	E_AUTH_NONCE_NOT_FOUND,
	E_AUTH_ERROR
};

/**
 * @ingroup CspServer
 * @brief
 * @param pclsCredential	SIP 인증 정보 저장 객체
 * @param pszFromId             SIP From 헤더의 사용자 식별자 (전화번호 등 파일 조회용)
 * @param pszMethod				SIP 메소드
 * @param clsXmlUser			사용자 정보 저장 객체
 * @returns 인증에 성공하면 E_AUTH_OK 를 리턴한다.
 *					존재하지 않는 nonce 인 경우 E_AUTH_NONCE_NOT_FOUND 를 리턴한다.
 *					그 외의 오류는 E_AUTH_ERROR 를 리턴한다.
 */
ECheckAuthResult CheckAuthorization( CSipCredential * pclsCredential, const char * pszFromId, const char * pszMethod, CspUser & clsXmlUser )
{
	if( pclsCredential->m_strUserName.empty() ) return E_AUTH_ERROR;
	if( gclsNonceMap.Select( pclsCredential->m_strNonce.c_str() ) == false ) return E_AUTH_NONCE_NOT_FOUND;
	// 1. Load user configuration using the From ID instead of the Auth Username
	if( gclsCspUserMap.Select( pszFromId, clsXmlUser ) == false ) return E_AUTH_ERROR;

	// 2. Cross-check: The Authorization username must match the AuthId configured for this From ID
	if( clsXmlUser.m_strAuthId != pclsCredential->m_strUserName ) return E_AUTH_ERROR;

	// 3. Digest Crypto verification (supports both plain MD5 and qop=auth)
	const char * pszQop    = pclsCredential->m_strQop.empty()        ? NULL : pclsCredential->m_strQop.c_str();
	const char * pszNc     = pclsCredential->m_strNonceCount.empty() ? NULL : pclsCredential->m_strNonceCount.c_str();
	const char * pszCnonce = pclsCredential->m_strCnonce.empty()     ? NULL : pclsCredential->m_strCnonce.c_str();

	if( CheckAuthorizationResponse( pclsCredential->m_strUserName.c_str(), pclsCredential->m_strRealm.c_str(),
				pclsCredential->m_strNonce.c_str(), pclsCredential->m_strUri.c_str(),
				pclsCredential->m_strResponse.c_str(), clsXmlUser.m_strPassWord.c_str(),
				pszMethod, pszQop, pszNc, pszCnonce ) == false ) return E_AUTH_ERROR;

	return E_AUTH_OK;
}

/**
 * @ingroup CspServer
 * @brief SIP REGISTER 요청 메시지 수신 이벤트 핸들러
 * @param iThreadId		SIP UDP 쓰레드 아이디
 * @param pclsMessage SIP 요청 메시지
 * @returns SIP 요청 메시지를 처리하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServer::RecvRequestRegister( int iThreadId, CSipMessage * pclsMessage )
{
	if( pclsMessage->m_iExpires > 0 && gclsSetup.m_iMinRegisterTimeout != 0 )
	{
		if( pclsMessage->m_iExpires < gclsSetup.m_iMinRegisterTimeout )
		{
			CSipMessage * pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_INTERVAL_TOO_BRIEF );
			if( pclsResponse == NULL ) return false;

			pclsResponse->AddHeader( "Min-Expires", gclsSetup.m_iMinRegisterTimeout );
			gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
			return true;
		}
	}

	// [UNREGISTER] Skip authentication for de-registration (expires=0)
	if( pclsMessage->GetExpires() == 0 )
	{
		gclsUserMap.Delete( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str() );
		SendResponse( pclsMessage, SIP_OK );
		return true;
	}

	// [AUTH] Check Authorization header for registration
	SIP_CREDENTIAL_LIST::iterator	itCL = pclsMessage->m_clsAuthorizationList.begin();

	if( itCL == pclsMessage->m_clsAuthorizationList.end() )
	{
		return SendUnAuthorizedResponse( pclsMessage );
	}

	CspUser	clsUser;

	ECheckAuthResult eRes = CheckAuthorization( &(*itCL), pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), pclsMessage->m_strSipMethod.c_str(), clsUser );
	switch( eRes )
	{
	case E_AUTH_NONCE_NOT_FOUND:
		SendUnAuthorizedResponse( pclsMessage );
		return true;
	case E_AUTH_ERROR:
		SendResponse( pclsMessage, SIP_FORBIDDEN );
		return true;
	default:
		break;
	}

	// [REGISTER] Process registration
	CSipFrom clsContact;

	if( gclsUserMap.Insert( pclsMessage, &clsContact, &clsUser ) )
	{
		CSipMessage * pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_OK );
		if( pclsResponse == NULL ) return false;

		pclsResponse->m_clsContactList.push_back( clsContact );
		pclsResponse->AddHeader( "Expires", 3600 );

		// P-Associated-URI: IMS 단말이 등록 완료 처리에 필요로 하는 헤더
		{
			char szPAUri[512];
			const std::string & strUser = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
			const std::string & strDomain = gclsSetup.m_strRealm;
			snprintf( szPAUri, sizeof(szPAUri), "<sip:%s@%s>", strUser.c_str(), strDomain.c_str() );
			pclsResponse->AddHeader( "P-Associated-URI", szPAUri );
		}

		gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
		
		// [FIX for P2P Call] Update CspUserMap (JSON Map) registration time.
		// Otherwise isAlive returns false (timeout) because time is 0.
		gclsCspUserMap.registerUser( clsUser.m_strId, "" ); // Password already verified during Auth
	}
	else
	{
		SendResponse( pclsMessage, SIP_BAD_REQUEST );
	}

	return true;
}
