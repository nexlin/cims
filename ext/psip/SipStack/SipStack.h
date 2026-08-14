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

#ifndef _SIP_STACK_H_
#define _SIP_STACK_H_

#include "SipStackDefine.h"
#include "SipTcp.h"
#include "SipStatusCode.h"
#include "SipStackCounter.h"
#include "SipICTList.h"
#include "SipNICTList.h"
#include "SipISTList.h"
#include "SipNISTList.h"
#include "SipStackSetup.h"
#include "SipStackCallBack.h"
#include "SipStackListener.h"
#include "TcpThreadList.h"
#include "TcpSocketMap.h"
#include "TcpConnectMap.h"

#include <vector>

typedef std::list< ISipStackCallBack * > SIP_STACK_CALLBACK_LIST;

// SipStack

// SIP stack 클래스
class CSipStack
{
public:
	CSipStack();
	~CSipStack();

	bool Start( CSipStackSetup & clsSetup );
	bool Stop( );

	// ── UDP 다중 리스너 hot-reload API (P2) ─────────────────────────
	/** UDP 리스너를 런타임에 추가. 성공 시 outId 에 실제 소켓 fd 또는 내부 인덱스 반환.
	 *  iExtId: CSP 쪽에서 발급한 논리 ID (DB csp_listener.id). 0 이면 내부 auto-assign.
	 *  strBindIp 가 빈 문자열이면 stack 의 m_strLocalIp 사용 (INADDR_ANY 는 "0.0.0.0").
	 *  포트 충돌이나 소켓 실패 시 false 반환. */
	bool AddUdpListener( int iExtId, const char* pszBindIp, int iPort,
	                     int iThreadCount, int& outId );

	/** UDP 리스너를 제거(drain 후 소켓 close, 스레드 종료 대기).
	 *  iExtId 로 찾음. 기본 리스너(Start 시 생성된 첫 번째)도 제거 가능하지만 그 경우
	 *  m_hUdpSocket alias 가 다른 살아있는 리스너로 이동한다. */
	bool RemoveUdpListener( int iExtId );

	/** 현재 등록된 UDP 리스너 스냅샷 (디버그/모니터링). */
	void GetUdpListenerInfo( std::vector<CSipStackUdpListener*>& outList );

	// ── TCP 다중 리스너 hot-reload API (R3) ─────────────────────────
	/** TCP 리스너를 런타임에 추가. 성공 시 outId 에 실제 할당된 ID 반환.
	 *  iExtId: CSP 쪽 논리 ID. 0 이면 내부 auto-assign.
	 *  strBindIp 가 빈 문자열이면 stack 의 m_strLocalIp 사용. */
	bool AddTcpListener( int iExtId, const char* pszBindIp, int iPort, int& outId );
	bool RemoveTcpListener( int iExtId );
	void GetTcpListenerInfo( std::vector<CSipStackTcpListener*>& outList );

#ifdef USE_TLS
	// ── TLS 다중 리스너 hot-reload API (R3 + R5.c) ─────────────────────────
	/** R5.c: optional per-listener cert. pszCertFile 이 NULL/빈 문자열이면 stack-global ctx 사용.
	 *  pszKeyFile 이 NULL/빈 문자열이면 pszCertFile 에서 key 도 로드 (combined PEM).
	 *  pszCaCertFile 이 유효하면 mTLS (client cert 검증) 활성화. */
	bool AddTlsListener( int iExtId, const char* pszBindIp, int iPort,
	                     const char* pszCertFile, const char* pszKeyFile, const char* pszCaCertFile,
	                     int& outId );
	bool RemoveTlsListener( int iExtId );
	void GetTlsListenerInfo( std::vector<CSipStackTlsListener*>& outList );
#endif

	bool Execute( struct timeval * psttTime );

	void IncreateUdpThreadCount( int & iThreadId );
	void DecreateUdpThreadCount();

	void IncreateTcpThreadCount( int & iThreadId );
	void DecreateTcpThreadCount();

	void GetString( CMonitorString & strBuf );
	void GetICTString( CMonitorString & strBuf );

	void Final();

	void DeleteAllTransaction();
	void GetICTMap( INVITE_TRANSACTION_MAP & clsMap );

	// SipStackCallBack.hpp
	bool AddCallBack( ISipStackCallBack * pclsCallBack );
	bool DeleteCallBack( ISipStackCallBack * pclsCallBack );
	void SetSecurityCallBack( ISipStackSecurityCallBack * pclsSecurityCallBack );

	void RecvRequest( int iThreadId, CSipMessage * pclsMessage );
	void RecvResponse( int iThreadId, CSipMessage * pclsMessage );
	void SendTimeout( int iThreadId, CSipMessage * pclsMessage );
	void TcpSessionEnd( const char * pszIp, int iPort, ESipTransport eTransport );
	void ThreadEnd( int iThreadId );

	// SipStackComm.hpp
	bool SendSipMessage( CSipMessage * pclsMessage );
	bool RecvSipMessage( int iThreadId, CSipMessage * pclsMessage );
	bool RecvSipMessage( int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort, ESipTransport eTransport );
	bool Send( CSipMessage * pclsMessage, bool bCheckMessage = true );
	bool Send( const char * pszMessage, const char * pszIp, unsigned short iPort, ESipTransport eTransport );

	bool	m_bStopEvent;						// SIP stack thread 종료 이벤트
	bool	m_bStackThreadRun;			// SIP stack thread 가 실행 중에 있는가?

	/** 기본 UDP 소켓 — 하위 호환(Send 경로에서 사용).
	 *  다중 리스너 환경에서는 m_vecUdpListeners[0].m_hSocket 과 동일.
	 *  리스너 제거 시 alias 를 다른 살아있는 리스너로 이동하거나 INVALID_SOCKET. */
	Socket m_hUdpSocket;
	Socket m_hTcpSocket;					// TCP SIP 메시지를 위한 서버 소켓 핸들

	CSipMutex m_clsUdpRecvMutex;	// SIP 메시지 수신 뮤텍스
	CSipStackSetup	m_clsSetup;		// SIP stack 설정

	/** SIP 신호 카운터 — 수신 요청/최종응답(수신·송신)/파싱 실패 누적 (SipStackCounter.h).
	 *  증가 지점은 RecvSipMessage/RecvResponse/SendSipMessage 안 수렴점 — 소비자는
	 *  GetSnapshot() 차분으로 윈도우 통계를 얻는다. */
	CSipStackCounter m_clsCounter;

	CThreadList			m_clsTcpThreadList;
	/** TCP worker pool 초기화 여부 — Start(정적 TCP 포트) 또는 AddTcpListener(런타임 추가)의
	 *  지연 초기화에서 true. UDP 전용 기동 후 런타임 TCP 리스너 추가 시 pool 부재로 accept 직후
	 *  연결을 닫던 결함의 가드. */
	bool						m_bTcpThreadListInit;
	CTcpSocketMap		m_clsTcpSocketMap;
	CTcpConnectMap	m_clsTcpConnectMap;

#ifdef USE_TLS
	Socket m_hTlsSocket;					// TLS SIP 메시지를 위한 서버 소켓 핸들

	CThreadList			m_clsTlsThreadList;
	/** TLS worker pool 초기화 여부 — Start(정적 TLS 포트/클라이언트) 또는 AddTlsListener(런타임
	 *  추가)의 지연 초기화에서 true. m_bTcpThreadListInit 과 같은 목적이다: pool 부재 상태로
	 *  리스너만 추가하면 accept 직후 SendCommand 가 실패해 연결을 즉시 닫는다. */
	bool						m_bTlsThreadListInit;
	CTcpSocketMap		m_clsTlsSocketMap;
	CTcpConnectMap	m_clsTlsConnectMap;
#endif

private:
	bool	 m_bStarted;
	int		 m_iUdpThreadRunCount;	// UDP 수신 쓰레드 개수
	int		 m_iTcpThreadRunCount;	// TCP 수신 쓰레드 개수

	CSipMutex m_clsMutex;
	CSipMutex m_clsUdpSendMutex;	// SIP 메시지 전송 뮤텍스

	CSipICTList		m_clsICT;
	CSipNICTList	m_clsNICT;
	CSipISTList		m_clsIST;
	CSipNISTList	m_clsNIST;

	SIP_STACK_CALLBACK_LIST	m_clsCallBackList;
	ISipStackSecurityCallBack * m_pclsSecurityCallBack;

	// UDP 다중 리스너 (P2: hot-reload)
	std::vector<CSipStackUdpListener*> m_vecUdpListeners;
	CSipMutex m_clsUdpListenerMutex;
	int m_iNextUdpListenerExtId;

	bool _StartUdpListenerLocked( CSipStackUdpListener* pListener );
	void _StopUdpListenerLocked( CSipStackUdpListener* pListener );
	void _RefreshPrimaryUdpSocketLocked();

	/** R5.b': outbound UDP request 의 Via[0] host/port 와 매칭되는 listener socket 선택.
	 *  매칭 실패 시 m_hUdpSocket (primary) 반환. response 는 primary 유지 (호출자에서 분기). */
	Socket _SelectUdpSocketForViaRequest( CSipMessage * pclsMessage );

	/** R5.b'': 지정된 listener id 의 UDP socket 반환. response path 에서 요청이 수신된
	 *  listener 로 응답을 돌려보내기 위해 사용. id <= 0 이거나 매칭 실패 시 primary fallback. */
	Socket _SelectUdpSocketByListenerId( int iListenerId );

	/** (C): listener id + transport 로 bind_ip/bind_port 추출.
	 *  CheckSipMessage 의 Via/Contact 자동 추가 자리에서 수신 listener 기반으로
	 *  자기 주소 결정에 사용. 매칭 실패 시 false 반환 → 호출자가 primary fallback. */
	bool _GetListenerBind( int iListenerId, ESipTransport eTransport,
	                       std::string& outIp, int& outPort );

	// TCP 다중 리스너 (R3: hot-reload)
	std::vector<CSipStackTcpListener*> m_vecTcpListeners;
	CSipMutex m_clsTcpListenerMutex;
	int m_iNextTcpListenerExtId;

	bool _StartTcpListenerLocked( CSipStackTcpListener* pListener );
	void _StopTcpListenerLocked( CSipStackTcpListener* pListener );
	void _RefreshPrimaryTcpSocketLocked();

#ifdef USE_TLS
	// TLS 다중 리스너 (R3: hot-reload)
	std::vector<CSipStackTlsListener*> m_vecTlsListeners;
	CSipMutex m_clsTlsListenerMutex;
	int m_iNextTlsListenerExtId;

	bool _StartTlsListenerLocked( CSipStackTlsListener* pListener );
	void _StopTlsListenerLocked( CSipStackTlsListener* pListener );
	void _RefreshPrimaryTlsSocketLocked();
#endif

	bool _Stop( );
	int GetTcpConnectingCount( );
	void CheckSipMessage( CSipMessage * pclsMessage );
};

#endif
