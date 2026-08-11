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
 * @brief SIP stack �� SIP �޽����� �������� ������ SIP stack �� SIP �޽����� �����ϰ� SIP �޽����� ��Ʈ��ũ�� �����Ѵ�.
 * @param pclsMessage SIP �޽��� ���� ����ü
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipStack::SendSipMessage( CSipMessage * pclsMessage )
{
	if( pclsMessage == NULL ) return false;

	if( m_clsSetup.m_bStateful )
	{
		CheckSipMessage( pclsMessage );
		++pclsMessage->m_iUseCount;

		if( pclsMessage->IsRequest() )
		{
			if( pclsMessage->IsMethod( SIP_METHOD_INVITE ) || pclsMessage->IsMethod( SIP_METHOD_ACK ) )
			{
				if( m_clsICT.Insert( pclsMessage ) )
				{
					Send( pclsMessage, false );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
			else
			{
				if( m_clsNICT.Insert( pclsMessage ) )
				{
					Send( pclsMessage, false );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
		}
		else
		{
			if( pclsMessage->IsMethod( SIP_METHOD_INVITE ) )
			{
				if( m_clsIST.Insert( pclsMessage ) )
				{
					// 송신 최종응답 카운터 — 트랜잭션 삽입 성공 시 1회 (IST 계층 재전송 미계수)
					if( pclsMessage->m_iStatusCode >= CSipStackCounter::FINAL_MIN )
						m_clsCounter.OnSendFinal( pclsMessage->m_clsCSeq.m_strMethod.c_str(), pclsMessage->m_iStatusCode );
					Send( pclsMessage, false );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
			else
			{
				if( m_clsNIST.Insert( pclsMessage ) )
				{
					if( pclsMessage->m_iStatusCode >= CSipStackCounter::FINAL_MIN )
						m_clsCounter.OnSendFinal( pclsMessage->m_clsCSeq.m_strMethod.c_str(), pclsMessage->m_iStatusCode );
					Send( pclsMessage, false );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
		}

		delete pclsMessage;
	}
	else
	{
		if( pclsMessage->IsRequest() == false && pclsMessage->m_iStatusCode >= CSipStackCounter::FINAL_MIN )
			m_clsCounter.OnSendFinal( pclsMessage->m_clsCSeq.m_strMethod.c_str(), pclsMessage->m_iStatusCode );
		Send( pclsMessage, false );

		if( pclsMessage->m_iUseCount == 0 )
		{
			delete pclsMessage;
		}

		return true;
	}

	return false;
}

/**
 * @ingroup SipStack
 * @brief ��Ʈ��ũ���� ���ŵ� SIP �޽����� SIP stack �� �����ϰ� callback �޼ҵ带 ȣ���Ͽ� ���뿡 �˷� �ش�.
 *				�� �޼ҵ忡�� true �� �����ϸ� ���������� CSipMessage �޸𸮸� �����ϰ� �׷��� ������ ȣ���� �κп��� CSipMessage �޸𸮸� ������ �־�� �Ѵ�.
 * @param iThreadId		������ ���̵� ( 0 ���� ������ ���� )
 * @param pclsMessage SIP �޽��� ���� ����ü
 * @returns SIP stack �� �����ϸ� true �� �����ϰ� �׷��� ������ false �� �����Ѵ�.
 */
bool CSipStack::RecvSipMessage( int iThreadId, CSipMessage * pclsMessage )
{
	if( m_clsSetup.m_bStateful )
	{
		++pclsMessage->m_iUseCount;

		if( pclsMessage->IsRequest() )
		{
			if( pclsMessage->IsMethod( SIP_METHOD_INVITE ) || pclsMessage->IsMethod( SIP_METHOD_ACK ) )
			{
				if( m_clsIST.Insert( pclsMessage ) )
				{
					// 수신 요청 카운터 — 트랜잭션 삽입 성공 후(재전송 미계수). 신규/re-INVITE 는
					//   To tag 유무로 구분 (신규 INVITE 가 CPS 의 분자).
					if( pclsMessage->IsMethod( SIP_METHOD_INVITE ) )
						m_clsCounter.OnRecvInvite( pclsMessage->m_clsTo.SelectParam( SIP_TAG ) == false );
					RecvRequest( iThreadId, pclsMessage );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
			else
			{
				if( m_clsNIST.Insert( pclsMessage ) )
				{
					if( pclsMessage->IsMethod( SIP_METHOD_REGISTER ) )
						m_clsCounter.OnRecvRegister();
					else
						m_clsCounter.OnRecvOtherRequest();
					RecvRequest( iThreadId, pclsMessage );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
		}
		else
		{
			if( pclsMessage->IsMethod( SIP_METHOD_INVITE ) )
			{
				// INVITE �޽����� ���� CANCEL �޽����� �����ϸ� �̸� SIP stack ���� �����Ѵ�.
				if( pclsMessage->m_iStatusCode >= 200 )
				{
					m_clsNICT.DeleteCancel( pclsMessage );
				}

				if( m_clsICT.Insert( pclsMessage ) )
				{
					RecvResponse( iThreadId, pclsMessage );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
			else
			{
				if( m_clsNICT.Insert( pclsMessage ) )
				{
					RecvResponse( iThreadId, pclsMessage );
					--pclsMessage->m_iUseCount;
					return true;
				}
			}
		}

		delete pclsMessage;
	}
	else
	{
		++pclsMessage->m_iUseCount;

		if( pclsMessage->IsRequest() )
		{
			// stateless 모드 — 트랜잭션 dedup 없이 원시 계수 (재전송 포함)
			if( pclsMessage->IsMethod( SIP_METHOD_INVITE ) )
				m_clsCounter.OnRecvInvite( pclsMessage->m_clsTo.SelectParam( SIP_TAG ) == false );
			else if( pclsMessage->IsMethod( SIP_METHOD_REGISTER ) )
				m_clsCounter.OnRecvRegister();
			else if( pclsMessage->IsMethod( SIP_METHOD_ACK ) == false )
				m_clsCounter.OnRecvOtherRequest();
			RecvRequest( iThreadId, pclsMessage );
		}
		else
		{
			RecvResponse( iThreadId, pclsMessage );
		}

		--pclsMessage->m_iUseCount;
		if( pclsMessage->m_iUseCount == 0 )
		{
			delete pclsMessage;
		}

		return true;
	}

	return false;
}

/**
 * @brief ��Ʈ��ũ���� ������ SIP �޽����� �Ľ��� ��, SIP stack �� �Է��Ѵ�.
 * @param iThreadId		������ ��ȣ
 * @param pszBuf			��Ʈ��ũ���� ���ŵ� SIP �޽���
 * @param iBufLen			��Ʈ��ũ���� ���ŵ� SIP �޽����� ����
 * @param pszIp				IP �ּ�
 * @param iPort				��Ʈ ��ȣ
 * @param eTransport	Transport
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipStack::RecvSipMessage( int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort, ESipTransport eTransport )
{
	CSipMessage	* pclsMessage = new CSipMessage();
	if( pclsMessage == NULL ) return false;

	if( pclsMessage->Parse( pszBuf, iBufLen ) == -1 )
	{
		// SIP 수신 이상 카운터 — 파싱 실패는 무응답 폐기라 여기가 유일한 관측 지점
		//   (소스 IP 동반 집계 — A-QOS-011 rx_error 의 원천)
		m_clsCounter.OnParseError( pszIp );
		delete pclsMessage;
		return false;
	}

	if( m_pclsSecurityCallBack )
	{
		const char * pszUserAgent = pclsMessage->m_strUserAgent.c_str();
		bool bDelete = false;

		if( m_pclsSecurityCallBack->IsDenyUserAgent( pszUserAgent ) ||
				m_pclsSecurityCallBack->IsAllowUserAgent( pszUserAgent ) == false ||
				m_pclsSecurityCallBack->IsDenyIp( pszIp ) ||
				m_pclsSecurityCallBack->IsAllowIp( pszIp ) == false )
		{
			bDelete = true;
		}

		if( bDelete )
		{
			m_clsCounter.OnSecurityDrop();
			delete pclsMessage;
			return false;
		}
	}

	if( pclsMessage->IsRequest() )
	{
		pclsMessage->AddIpPortToTopVia( pszIp, iPort );
	}

	pclsMessage->m_strClientIp = pszIp;
	pclsMessage->m_iClientPort = iPort;
	pclsMessage->m_eTransport = eTransport;
	// UDP 수신 경로에서 SipUdpListenerThread 가 세팅한 thread-local id 를 메시지로 전달.
	// (TCP/TLS 경로는 현재 미지원 — 단일 리스너 구조이므로 -1 유지)
	extern thread_local int t_iCurrentListenerId;
	if (eTransport == E_SIP_UDP && t_iCurrentListenerId > 0) {
		pclsMessage->m_iListenerId = t_iCurrentListenerId;
	}

	RecvSipMessage( iThreadId, pclsMessage );

	return true;
}

/**
 * @ingroup SipStack
 * @brief SIP �޽����� ��Ʈ��ũ�� �����Ѵ�.
 * @param pclsMessage		SIP �޽��� ���� ����ü
 * @param bCheckMessage	SIP �޽����� �˻��Ͽ��� �ʼ� ����� �߰�/�����ϴ°�?
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipStack::Send( CSipMessage * pclsMessage, bool bCheckMessage )
{
	const char * pszIp = NULL;
	int iPort = -1;
	ESipTransport eTransport = E_SIP_UDP;

	if( bCheckMessage )
	{
		CheckSipMessage( pclsMessage );
	}

	if( pclsMessage->IsRequest() )
	{
		SIP_FROM_LIST::iterator itList = pclsMessage->m_clsRouteList.begin();
		if( pclsMessage->m_strSendDestIp.empty() == false && pclsMessage->m_iSendDestPort > 0 )
		{
			// 전송 목적지 오버라이드 — Route 헤더 없이 지정 주소로 전송 (NAT 뒤 단말 등)
			pszIp = pclsMessage->m_strSendDestIp.c_str();
			iPort = pclsMessage->m_iSendDestPort;
			eTransport = pclsMessage->m_eTransport;
		}
		else if( itList == pclsMessage->m_clsRouteList.end() )
		{
			if( pclsMessage->m_clsReqUri.m_strHost.empty() ) return false;

			pszIp = pclsMessage->m_clsReqUri.m_strHost.c_str();
			iPort = pclsMessage->m_clsReqUri.m_iPort;
			eTransport = pclsMessage->m_clsReqUri.SelectTransport();
		}
		else
		{
			pszIp = itList->m_clsUri.m_strHost.c_str();
			iPort = itList->m_clsUri.m_iPort;
			eTransport = itList->m_clsUri.SelectTransport();
		}

		pclsMessage->m_eTransport = eTransport;
	}
	else
	{
		SIP_VIA_LIST::iterator itViaList = pclsMessage->m_clsViaList.begin();
		if( itViaList == pclsMessage->m_clsViaList.end() ) return false;

		const char * pszTemp;

		pszTemp = SearchSipParameter( itViaList->m_clsParamList, SIP_RPORT );
		if( pszTemp )
		{
			iPort = atoi( pszTemp );
		}
		else
		{
			iPort = itViaList->m_iPort;
		}

		pszIp = SearchSipParameter( itViaList->m_clsParamList, SIP_RECEIVED );
		if( pszIp == NULL )
		{
			pszIp = itViaList->m_strHost.c_str();
		}

		pszTemp = SearchSipParameter( itViaList->m_clsParamList, SIP_TRANSPORT );
		if( pszTemp )
		{
			if( !strcasecmp( pszTemp, SIP_TRANSPORT_TCP ) )
			{
				eTransport = E_SIP_TCP;
			}
			else if( !strcasecmp( pszTemp, SIP_TRANSPORT_TLS ) )
			{
				eTransport = E_SIP_TLS;
			}
		}
		else
		{
			const char * pszTransport = itViaList->m_strTransport.c_str();

			if( !strcasecmp( pszTransport, SIP_TRANSPORT_TCP ) )
			{
				eTransport = E_SIP_TCP;
			}
			else if( !strcasecmp( pszTransport, SIP_TRANSPORT_TLS ) )
			{
				eTransport = E_SIP_TLS;
			}
		}
	}

	if( iPort <= 0 ) iPort = 5060;

	if( pszIp[0] == '\0' ) return false;

	if( pclsMessage->m_strPacket.empty() )
	{
		if( pclsMessage->MakePacket() == false ) return false;
	}

	bool bRes = false;

	if( eTransport == E_SIP_UDP )
	{
		// R5.b': Request 의 경우 Via[0] (우리 source) 와 매칭되는 listener socket 사용.
		// R5.b'': Response 는 요청이 수신된 listener (m_iListenerId) 로 회신.
		Socket hSendSocket;
		if( pclsMessage->IsRequest() )
		{
			hSendSocket = _SelectUdpSocketForViaRequest( pclsMessage );
		}
		else
		{
			hSendSocket = _SelectUdpSocketByListenerId( pclsMessage->m_iListenerId );
		}

		m_clsUdpSendMutex.acquire();
		bRes = UdpSend( hSendSocket, pclsMessage->m_strPacket.c_str(), (int)pclsMessage->m_strPacket.length(), pszIp, iPort );
		m_clsUdpSendMutex.release();

		// 억제 소스(drop 확정 IP)로 나가는 응답(트랜잭션 계층 자동 100 Trying 등)의
		//   원본 덤프를 생략 — 인바운드 억제와 대칭. 전송 자체는 수행(로깅만 건너뜀).
		if( !CLog::IsNetworkSourceSuppressed( pszIp ) )
			CLog::Print( LOG_NETWORK, "UdpSend(%s:%d) \n[%s]", pszIp, iPort, pclsMessage->m_strPacket.c_str() );
	}
	else if( eTransport == E_SIP_TCP )
	{
		Socket	hSocket;

		if( m_clsTcpSocketMap.Select( pszIp, iPort, hSocket ) )
		{
			SipTcpSend( hSocket, pszIp, iPort, pclsMessage, m_clsSetup.m_bUseContactListenPort ? m_clsSetup.m_iLocalTcpPort : 0 );
		}
		else
		{
			bRes = StartSipTcpClientThread( this, pszIp, iPort, pclsMessage );
		}
	}
	else if( eTransport == E_SIP_TLS )
	{
#ifdef USE_TLS
		if( m_clsTlsSocketMap.SendTls( pszIp, iPort, pclsMessage, m_clsSetup.m_bUseContactListenPort ? m_clsSetup.m_iLocalTlsPort : 0 ) )
		{
			bRes = true;
		}
		else
		{
			bRes = StartSipTlsClientThread( this, pszIp, iPort, pclsMessage );
		}
#else
		CLog::Print( LOG_ERROR, "TLS is not supported. rebuild with USE_TLS option" );
#endif
	}
	
	return bRes;
}

/**
 * @ingroup SipStack
 * @brief SIP �������� ���ڿ��� �����Ѵ�.
 * @param pszMessage ������ ���ڿ�
 * @param pszIp ������ IP �ּ�
 * @param iPort ������ ��Ʈ ��ȣ
 * @param eTransport ��������
 * @returns �����ϸ� true �� �����ϰ� �����ϸ� false �� �����Ѵ�.
 */
bool CSipStack::Send( const char * pszMessage, const char * pszIp, unsigned short iPort, ESipTransport eTransport )
{
	if( pszMessage == NULL || pszIp == NULL ) return false;

	bool bRes = false;
	int iMessageSize = (int)strlen( pszMessage );

	if( eTransport == E_SIP_UDP )
	{
		m_clsUdpSendMutex.acquire();
		bRes = UdpSend( m_hUdpSocket, pszMessage, iMessageSize, pszIp, iPort );
		m_clsUdpSendMutex.release();
	}
	else if( eTransport == E_SIP_TCP )
	{
		Socket	hSocket;

		if( m_clsTcpSocketMap.Select( pszIp, iPort, hSocket ) )
		{
			if( TcpSend( hSocket, pszMessage, iMessageSize ) == iMessageSize )
			{
				bRes = true;
			}
		}
	}
	else if( eTransport == E_SIP_TLS )
	{
#ifdef USE_TLS
		bRes = m_clsTlsSocketMap.SendTls( pszIp, iPort, pszMessage, iMessageSize );
#else
		CLog::Print( LOG_ERROR, "TLS is not supported. rebuild with USE_TLS option" );
#endif
	}
	
	return bRes;
}

/**
 * @ingroup SipStack
 * @brief ������ SIP �޽������� �ʿ��� ����� �������� ���� ��� default ����� �����Ѵ�.
 * @param pclsMessage ������ SIP �޽���
 */
void CSipStack::CheckSipMessage( CSipMessage * pclsMessage )
{
	if( pclsMessage->IsRequest() )
	{
		if( pclsMessage->m_clsViaList.size() == 0 )
		{
			int iPort = m_clsSetup.GetLocalPort( pclsMessage->m_eTransport );

			if( iPort == 0 ) iPort = 5060;
			pclsMessage->AddVia( m_clsSetup.m_strLocalIp.c_str(), iPort );
		}
	}

	if( pclsMessage->m_strSipVersion.empty() )
	{
		pclsMessage->m_strSipVersion = SIP_VERSION;
	}

	// REGISTER 응답의 Contact 은 등록된 바인딩 에코 전용 (RFC 3261 §10.3) —
	//   401/403 등엔 Contact 을 싣지 않고, 200 OK 는 응용이 명시적으로 채운다.
	const bool bRegisterResponse =
		( pclsMessage->IsRequest() == false &&
		  strcasecmp( pclsMessage->m_clsCSeq.m_strMethod.c_str(), SIP_METHOD_REGISTER ) == 0 );

	if( pclsMessage->m_clsContactList.size() == 0 && bRegisterResponse == false )
	{
		ESipTransport eTransport = E_SIP_UDP;

		if( pclsMessage->IsRequest() )
		{
			SIP_FROM_LIST::iterator itList = pclsMessage->m_clsRouteList.begin();
			if( itList == pclsMessage->m_clsRouteList.end() )
			{
				eTransport = pclsMessage->m_clsReqUri.SelectTransport();
			}
			else
			{
				eTransport = itList->m_clsUri.SelectTransport();
			}
		}
		else
		{
			SIP_VIA_LIST::iterator itViaList = pclsMessage->m_clsViaList.begin();
			if( itViaList != pclsMessage->m_clsViaList.end() )
			{
				const char * pszTemp;

				pszTemp = SearchSipParameter( itViaList->m_clsParamList, SIP_TRANSPORT );
				if( pszTemp == NULL )
				{
					pszTemp = itViaList->m_strTransport.c_str();
				}

				if( pszTemp )
				{
					if( !strcasecmp( pszTemp, SIP_TRANSPORT_TCP ) )
					{
						eTransport = E_SIP_TCP;
					}
					else if( !strcasecmp( pszTemp, SIP_TRANSPORT_TLS ) )
					{
						eTransport = E_SIP_TLS;
					}
				}
			}
		}

		CSipFrom clsContact;

		clsContact.m_clsUri.m_strProtocol = SipGetProtocol( eTransport );

		if( pclsMessage->IsRequest() )
		{
			clsContact.m_clsUri.m_strUser = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
		}
		else
		{
			clsContact.m_clsUri.m_strUser = pclsMessage->m_clsTo.m_clsUri.m_strUser;
		}

		// (C): 응답의 경우 incoming listener id 가 carry-over 되어있다면 그 listener 의
		//   bind_ip:bind_port 를 자기 주소로 사용 → 단말이 다른 NIC/listener 로 보낸 메시지에
		//   응답이 그 listener 자기 주소로 박힘. 매칭 실패 또는 listener id 없으면 primary fallback.
		std::string strBindIp;
		int iBindPort = 0;
		if( pclsMessage->m_iListenerId > 0 &&
		    _GetListenerBind( pclsMessage->m_iListenerId, eTransport, strBindIp, iBindPort ) )
		{
			clsContact.m_clsUri.m_strHost = ( strBindIp.empty() || strBindIp == "0.0.0.0" )
			                                    ? m_clsSetup.m_strLocalIp
			                                    : strBindIp;
			clsContact.m_clsUri.m_iPort = iBindPort;
		}
		else
		{
			clsContact.m_clsUri.m_strHost = m_clsSetup.m_strLocalIp;

			if( eTransport == E_SIP_UDP )
			{
				clsContact.m_clsUri.m_iPort = m_clsSetup.m_iLocalUdpPort;
			}
			else if( eTransport == E_SIP_TCP )
			{
				clsContact.m_clsUri.m_iPort = m_clsSetup.m_iLocalTcpPort;
			}
			else if( eTransport == E_SIP_TLS )
			{
				clsContact.m_clsUri.m_iPort = m_clsSetup.m_iLocalTlsPort;
			}
		}

		clsContact.m_clsUri.InsertTransport( eTransport );

		pclsMessage->m_clsContactList.push_back( clsContact );
	}

	// User-Agent 헤더는 요청 메시지에만 추가 (RFC 3261: 서버 응답에는 Server 헤더 사용)
	// m_strUserAgent는 Check()에서 이미 최종값으로 설정됨 (empty이면 SIP_USER_AGENT, 아니면 설정값 그대로)
	if( pclsMessage->IsRequest() )
	{
		pclsMessage->m_strUserAgent = m_clsSetup.m_strUserAgent;
	}

	// Max-Forwards 는 요청 전용 헤더 (RFC 3261 §8.1.1.6) — 응답에는 싣지 않는다
	if( pclsMessage->m_iMaxForwards == -1 && pclsMessage->IsRequest() )
	{
		pclsMessage->m_iMaxForwards = SIP_MAX_FORWARDS;
	}

	pclsMessage->m_bUseCompact = m_clsSetup.m_bUseSipCompactForm;
}
