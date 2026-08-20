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

#ifndef _SIP_REGISTER_INFO_H_
#define _SIP_REGISTER_INFO_H_

#include "SipMessage.h"
#include "SipStack.h"
#include <vector>
#include <utility>

// SIP 로그인 정보를 저장하는 클래스
class CSipServerInfo
{
public:
	CSipServerInfo();
	~CSipServerInfo();

	bool Equal( CSipServerInfo & clsInfo );
	bool Equal( const char * pszIp, int iPort, ESipTransport eTransport );
	void Update( CSipServerInfo & clsInfo );
	void ClearLogin();
	void MakeA1( const CSipCredential & clsCredential, char * pszA1, int iA1Size );

	// SIP 서버의 IP 주소
	std::string		m_strIp;

	// SIP 서버의 포트 번호
	int						m_iPort;

	// SIP 서버의 도메인
	std::string		m_strDomain;

	// 로그인 아이디
	std::string		m_strUserId;

	// 인증 아이디
	std::string		m_strAuthId;

	// 로그인 비밀번호
	std::string		m_strPassWord;

	/** SIP Digest H(A1)=MD5(authId:realm:password) hex(32). 비어 있지 않으면 m_strPassWord 대신
	 *  이 값으로 response 를 계산한다 — 원문 비밀번호 없이 등록하는 클라이언트용. */
	std::string		m_strHa1;

	/** P-Preferred-Identity 헤더 값 (비어있으면 추가 안함) */
	std::string		m_strPPreferredIdentity;

	/** P-Access-Network-Info 헤더 값 (비어있으면 추가 안함) */
	std::string		m_strPAccessNetworkInfo;

	/** Contact feature tag 목록 — {name, value} 쌍. value 빈 문자열이면 플래그 파라미터 */
	std::vector<std::pair<std::string,std::string>> m_vecContactFeatureTags;

	// 로그인 만료 시간 (초단위)
	int						m_iLoginTimeout;

	/** transport */
	ESipTransport	m_eTransport;

	// NAT 만료 시간 (초단위)
	int						m_iNatTimeout;

	bool					m_bLogin;
	time_t				m_iLoginTime;
	time_t				m_iSendTime;
	time_t				m_iResponseTime;
	time_t				m_iNextSendTime;
	CSipCallId		m_clsCallId;
	int						m_iSeqNo;
	bool					m_bAuth;
	CSipChallenge	m_clsChallenge;
	int						m_iChallengeStatusCode;
	int						m_iNonceCount;
	
	bool					m_bDelete;

	CSipMessage * CreateRegister( CSipStack * pclsSipStack, CSipMessage * pclsResponse );

	bool SetChallenge( CSipMessage * pclsResponse );
	bool AddAuth( CSipMessage * pclsRequest, CSipMessage * pclsResponse );
	bool AddAuth( CSipMessage * pclsRequest, const CSipChallenge * pclsChallenge, int iStatusCode, int iNonceCount );
};

typedef std::list< CSipServerInfo > SIP_SERVER_INFO_LIST;

#endif
