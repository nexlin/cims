#include "SipUserAgent.h"
#include "SipServerInfo.h"
#include "SipUtility.h"
#include "SipMd5.h"
#include "StringUtility.h"
#include "MemoryDebug.h"

CSipServerInfo::CSipServerInfo() : m_iPort(5060), m_iLoginTimeout(3600)
	, m_eTransport(E_SIP_UDP), m_iNatTimeout(0)
	, m_iNextSendTime(0), m_iSeqNo(0), m_bAuth(false), m_bDelete(false)
{
	ClearLogin();
}

CSipServerInfo::~CSipServerInfo()
{
}

bool CSipServerInfo::Equal( CSipServerInfo & clsInfo )
{
	if( !strcmp( clsInfo.m_strIp.c_str(), m_strIp.c_str() ) &&
			!strcmp( clsInfo.m_strUserId.c_str(), m_strUserId.c_str() ) &&
			clsInfo.m_iPort == m_iPort &&
			clsInfo.m_eTransport == m_eTransport )
	{
		return true;
	}

	return false;
}

bool CSipServerInfo::Equal( const char * pszIp, int iPort, ESipTransport eTransport )
{
	if( !strcmp( pszIp, m_strIp.c_str() ) &&
			iPort == m_iPort &&
			eTransport == m_eTransport )
	{
		return true;
	}

	return false;
}

void CSipServerInfo::Update( CSipServerInfo & clsInfo )
{
	m_strDomain = clsInfo.m_strDomain;
	m_strPassWord = clsInfo.m_strPassWord;
	m_iLoginTimeout = clsInfo.m_iLoginTimeout;
}

void CSipServerInfo::ClearLogin()
{
	m_bLogin = false;
	m_iLoginTime = 0;
	m_iSendTime = 0;
	m_iResponseTime = 0;
	m_clsCallId.Clear();
	m_clsChallenge.Clear();
	m_iChallengeStatusCode = 0;
	m_iNonceCount = 1;
}

CSipMessage * CSipServerInfo::CreateRegister( CSipStack * pclsSipStack, CSipMessage * pclsResponse )
{
	CSipMessage * pclsRequest = new CSipMessage();
	if( pclsRequest == NULL ) return NULL;

	// REGISTER sip:127.0.0.1 SIP/2.0
	pclsRequest->m_strSipMethod = SIP_METHOD_REGISTER;
	pclsRequest->m_clsReqUri.Set( SIP_PROTOCOL, NULL, m_strDomain.c_str(), m_iPort );

	// To
	pclsRequest->m_clsTo.m_clsUri.Set( SIP_PROTOCOL, m_strUserId.c_str(), m_strDomain.c_str(), m_iPort );

	// From
	pclsRequest->m_clsFrom = pclsRequest->m_clsTo;
	pclsRequest->m_clsFrom.InsertTag();

	// Expires: 300
	pclsRequest->m_iExpires = m_iLoginTimeout;
	
	// CSeq: 1 REGISTER
	++m_iSeqNo;
	if( m_iSeqNo >= 2000000000 ) m_iSeqNo = 1;
	pclsRequest->m_clsCSeq.m_iDigit = m_iSeqNo;
	pclsRequest->m_clsCSeq.m_strMethod = SIP_METHOD_REGISTER;

	// Route
	pclsRequest->AddRoute( m_strIp.c_str(), m_iPort, m_eTransport );

	// Call-Id
	if( m_clsCallId.Empty() )
	{
		pclsRequest->m_clsCallId.Make( pclsSipStack->m_clsSetup.m_strLocalIp.c_str() );
		m_clsCallId = pclsRequest->m_clsCallId;
	}
	else
	{
		pclsRequest->m_clsCallId = m_clsCallId;
	}

	// P-Preferred-Identity: 설정된 경우 추가
	if( !m_strPPreferredIdentity.empty() )
	{
		pclsRequest->AddHeader( "P-Preferred-Identity", m_strPPreferredIdentity.c_str() );
	}

	// P-Access-Network-Info: 설정된 경우 추가
	if( !m_strPAccessNetworkInfo.empty() )
	{
		pclsRequest->AddHeader( "P-Access-Network-Info", m_strPAccessNetworkInfo.c_str() );
	}

	m_bAuth = false;

	// 3GPP IMS TS 24.229: 첫 REGISTER (challenge 없음) 에도 빈 Authorization 헤더 포함
	if( pclsResponse == nullptr && m_clsChallenge.m_strAlgorithm.empty() )
	{
		CSipCredential clsEmpty;
		clsEmpty.m_strType = "Digest";
		clsEmpty.m_strUserName = m_strAuthId.empty() ? m_strUserId : m_strAuthId;
		clsEmpty.m_strUri = "sip:" + m_strDomain;
		pclsRequest->m_clsAuthorizationList.push_front( clsEmpty );
	}

	if( pclsResponse )
	{
		m_bAuth = AddAuth( pclsRequest, pclsResponse );

		/* 
		std::string	strToTag;

		if( pclsResponse->m_clsTo.SelectParam( SIP_TAG, strToTag ) )
		{
			pclsRequest->m_clsTo.InsertParam( SIP_TAG, strToTag.c_str() );
		}
		*/
	}
	else if( m_clsChallenge.m_strAlgorithm.empty() == false )
	{
		++m_iNonceCount;
		m_bAuth = AddAuth( pclsRequest, &m_clsChallenge, m_iChallengeStatusCode, m_iNonceCount );
	}

	pclsRequest->m_eTransport = m_eTransport;

	// Contact feature tag 설정된 경우 미리 Contact 빌드 (SipStack 자동생성 억제)
	if( !m_vecContactFeatureTags.empty() )
	{
		CSipFrom clsContact;
		clsContact.m_clsUri.m_strProtocol = "sip";
		clsContact.m_clsUri.m_strUser = m_strUserId;
		clsContact.m_clsUri.m_strHost = pclsSipStack->m_clsSetup.m_strLocalIp;
		clsContact.m_clsUri.m_iPort   = pclsSipStack->m_clsSetup.m_iLocalUdpPort;
		for( const auto & tag : m_vecContactFeatureTags )
			clsContact.InsertParam( tag.first.c_str(), tag.second.empty() ? NULL : tag.second.c_str() );
		pclsRequest->m_clsContactList.push_back( clsContact );
	}

	return pclsRequest;
}


bool CSipServerInfo::SetChallenge( CSipMessage * pclsResponse )
{
	SIP_CHALLENGE_LIST::const_iterator itAT;

	if( pclsResponse->m_iStatusCode == SIP_PROXY_AUTHENTICATION_REQUIRED )
	{
		if( pclsResponse->m_clsProxyAuthenticateList.size() == 0 ) return false;
		itAT = pclsResponse->m_clsProxyAuthenticateList.begin();
	}
	else
	{
		if( pclsResponse->m_clsWwwAuthenticateList.size() == 0 ) return false;
		itAT = pclsResponse->m_clsWwwAuthenticateList.begin();
	}

	if( itAT->m_strQop.empty() ) return false;
	if( strncmp( itAT->m_strQop.c_str(), "auth", 4 ) ) return false;
	
	m_clsChallenge = *itAT;
	m_iChallengeStatusCode = pclsResponse->m_iStatusCode;

	return true;
}


bool CSipServerInfo::AddAuth( CSipMessage * pclsRequest, CSipMessage * pclsResponse )
{
	SIP_CHALLENGE_LIST::const_iterator itAT;

	if( pclsResponse->m_iStatusCode == SIP_PROXY_AUTHENTICATION_REQUIRED )
	{
		if( pclsResponse->m_clsProxyAuthenticateList.size() == 0 ) return false;
		itAT = pclsResponse->m_clsProxyAuthenticateList.begin();
	}
	else
	{
		if( pclsResponse->m_clsWwwAuthenticateList.size() == 0 ) return false;
		itAT = pclsResponse->m_clsWwwAuthenticateList.begin();
	}

	return AddAuth( pclsRequest, &(*itAT), pclsResponse->m_iStatusCode, 1 );
}


bool CSipServerInfo::AddAuth( CSipMessage * pclsRequest, const CSipChallenge * pclsChallenge, int iStatusCode, int iNonceCount )
{
	CSipCredential clsCredential;

	clsCredential.m_strType = pclsChallenge->m_strType;

	if( m_strAuthId.empty() )
	{
		clsCredential.m_strUserName = m_strUserId;
	}
	else
	{
		clsCredential.m_strUserName = m_strAuthId;
	}

	clsCredential.m_strRealm = pclsChallenge->m_strRealm;
	clsCredential.m_strNonce = pclsChallenge->m_strNonce;
	clsCredential.m_strAlgorithm = pclsChallenge->m_strAlgorithm;
	clsCredential.m_strOpaque = pclsChallenge->m_strOpaque;

	clsCredential.m_strUri = "sip:";
	clsCredential.m_strUri.append( m_strDomain );

	char	szA1[1024], szA2[1024], szMd5[33], szResponse[1024];
	const char * pszQop = pclsChallenge->m_strQop.c_str();

	if( pclsChallenge->m_strQop.empty() == false && !strncmp( pszQop, "auth", 4 ) )
	{
		STRING_LIST clsQopList;

		if( strstr( pszQop, "," ) )
		{
			SplitString( pszQop, clsQopList, ',' );
			
			STRING_LIST::iterator itSL;

			for( itSL = clsQopList.begin(); itSL != clsQopList.end(); ++itSL )
			{
				clsCredential.m_strQop = *itSL;
			}
		}
		else
		{
			clsCredential.m_strQop = pclsChallenge->m_strQop;
		}

		char szNonceCount[9];

		snprintf( szNonceCount, sizeof(szNonceCount), "%08d", iNonceCount );
		clsCredential.m_strNonceCount = szNonceCount;
		clsCredential.m_strCnonce = "1";

		snprintf( szA1, sizeof(szA1), "%s:%s:%s", clsCredential.m_strUserName.c_str(), clsCredential.m_strRealm.c_str(), m_strPassWord.c_str() );
		SipMd5String( szA1, szMd5 );
		snprintf( szA1, sizeof(szA1), "%s", szMd5 );
		
		if( !strcmp( clsCredential.m_strQop.c_str(), "auth-int" ) )
		{
			SipMd5String( pclsRequest->m_strBody.c_str(), szMd5 );
			snprintf( szA2, sizeof(szA2), "%s:%s:%s", pclsRequest->m_strSipMethod.c_str(), clsCredential.m_strUri.c_str(), szMd5 );
		}
		else
		{
			snprintf( szA2, sizeof(szA2), "%s:%s", pclsRequest->m_strSipMethod.c_str(), clsCredential.m_strUri.c_str() );
		}

		SipMd5String( szA2, szMd5 );
		snprintf( szA2, sizeof(szA2), "%s", szMd5 );
		
		snprintf( szResponse, sizeof(szResponse), "%s:%s:%s:%s:%s:%s", szA1, clsCredential.m_strNonce.c_str(), clsCredential.m_strNonceCount.c_str()
			, clsCredential.m_strCnonce.c_str(), clsCredential.m_strQop.c_str(), szA2 );
		SipMd5String( szResponse, szMd5 );
		snprintf( szResponse, sizeof(szResponse), "%s", szMd5 );

		clsCredential.m_strResponse = szMd5;
	}
	else
	{
		snprintf( szA1, sizeof(szA1), "%s:%s:%s", clsCredential.m_strUserName.c_str(), clsCredential.m_strRealm.c_str(), m_strPassWord.c_str() );
		SipMd5String( szA1, szMd5 );
		snprintf( szA1, sizeof(szA1), "%s", szMd5 );
		
		snprintf( szA2, sizeof(szA2), "%s:%s", pclsRequest->m_strSipMethod.c_str(), clsCredential.m_strUri.c_str() );
		SipMd5String( szA2, szMd5 );
		snprintf( szA2, sizeof(szA2), "%s", szMd5 );
		
		snprintf( szResponse, sizeof(szResponse), "%s:%s:%s", szA1, clsCredential.m_strNonce.c_str(), szA2 );
		SipMd5String( szResponse, szMd5 );

		clsCredential.m_strResponse = szMd5;
	}

	if( iStatusCode == SIP_PROXY_AUTHENTICATION_REQUIRED )
	{
		pclsRequest->m_clsProxyAuthorizationList.push_front( clsCredential );
	}
	else
	{
		pclsRequest->m_clsAuthorizationList.push_front( clsCredential );
	}

	return true;
}
