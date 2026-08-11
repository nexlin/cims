#ifndef _SIP_USER_AGENT_H_
#define _SIP_USER_AGENT_H_

#include "SipStack.h"
#include "SipServerInfo.h"
#include "SipUserAgentCallBack.h"
#include "SipDialog.h"


class CSipCallRoute
{
public:
	CSipCallRoute() : m_iDestPort(0), m_eTransport( E_SIP_UDP ), m_b100rel(false), m_iOutboundLocalPort(-1)
	{}

	std::string	m_strDestIp;

	int					m_iDestPort;

	ESipTransport	m_eTransport;

	bool				m_b100rel;

	/** outbound 송신 시 Via/Contact 자기 주소 override.
	 *  Empty → CSipStackSetup::m_strLocalIp fallback.
	 *  route 결정으로 local_node 가 결정되면 그 bind_ip 를 set. */
	std::string m_strOutboundLocalIp;

	/** outbound 송신 시 Via 자기 port override. <=0 → fallback. */
	int m_iOutboundLocalPort;
};

typedef MAP< std::string, CSipDialog > SIP_DIALOG_MAP;

typedef std::list< std::string > SIP_CALL_ID_LIST;

class CSipUserAgent : public ISipStackCallBack
{
public:
	CSipUserAgent();
	~CSipUserAgent();

	// SipUserAgentLogin.hpp : 로그인 관련
	bool InsertRegisterInfo( CSipServerInfo & clsInfo );
	bool UpdateRegisterInfo( CSipServerInfo & clsInfo );
	bool DeleteRegisterInfo( CSipServerInfo & clsInfo );

	bool Start( CSipStackSetup & clsSetup, ISipUserAgentCallBack * pclsCallBack, ISipStackSecurityCallBack * pclsSecurityCallBack = NULL );
	bool Stop( );
	void Final();

	bool StartCall( const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp, CSipCallRoute * pclsRoute, std::string & strCallId );
	bool StopCall( const char * pszCallId, int iSipCode = 0 );
	bool StopCall( const char * pszCallId, const char * pszForward );
	bool RingCall( const char * pszCallId, CSipCallRtp * pclsRtp );
	bool RingCall( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRtp );
	bool AcceptCall( const char * pszCallId, CSipCallRtp * pclsRtp );
	/** 200 OK 를 생성만 하고 전송하지 않는 2단계 API (CreateCall→StartCall 패턴) — 호출자가
	 *  바디(multipart 등)를 부가한 뒤 m_clsSipStack.SendSipMessage() 로 전송한다. */
	bool AcceptCall( const char * pszCallId, CSipCallRtp * pclsRtp, CSipMessage ** ppclsResponse );

	bool HoldCall( const char * pszCallId, ERtpDirection eDirection = E_RTP_SEND );
	bool ResumeCall( const char * pszCallId );

	int GetCallCount( );
	void GetCallIdList( SIP_CALL_ID_LIST & clsList );
	void StopCallAll( );

	/** pszOverrideDomain: 지정 시 Dialog 에 per-dialog 도메인 override 설정
	 *  (INVITE 생성 전에 설정되므로 INVITE 의 From/To/Request-URI 도 반영됨) */
	bool CreateCall( const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp, CSipCallRoute * pclsRoute, std::string & strCallId, CSipMessage ** ppclsInvite,
	                 const char * pszOverrideDomain = NULL );
	bool StartCall( const char * pszCallId, CSipMessage * pclsInvite );
	bool Delete( const char * pszCallId );

	bool TransferCallBlind( const char * pszCallId, const char * pszTo );
	bool TransferCall( const char * pszCallId, const char * pszToCallId );

	// SipStackAgentSms.hpp
	bool SendSms( const char * pszFrom, const char * pszTo, const char * pszText, CSipCallRoute * pclsRoute );
	bool SendSms( const char * pszFrom, const char * pszTo, const char * pszText, CSipCallRoute * pclsRoute, const char * pszContentType );

	// SipUserAgentSend.hpp
	bool SendReInvite( const char * pszCallId, CSipCallRtp * pclsRtp );
	/** ReINVITE 를 생성만 하고 전송하지 않는 2단계 API — 확립된 다이얼로그 전용. 호출자가
	 *  바디(multipart 등)/헤더를 부가한 뒤 m_clsSipStack.SendSipMessage() 로 전송한다. */
	bool CreateReInvite( const char * pszCallId, CSipCallRtp * pclsRtp, CSipMessage ** ppclsRequest );
	bool SendNotify( const char * pszCallId, int iSipCode );
	bool SendNotifyWithBody( const char * pszCallId, const char * pszEvent,
	                         const char * pszContentType, const char * pszContentSubType,
	                         const std::string & strBody );
	bool SendDtmf( const char * pszCallId, char cDtmf );
	bool SendPrack( const char * pszCallId, CSipCallRtp * pclsRtp );

	// SipUserAgentUtil.hpp
	bool GetRemoteCallRtp( const char * pszCallId, CSipCallRtp * pclsRtp );
	bool GetRemotePayloadTypes( const char * pszCallId, const char * pszEncoding,
	                            int & iPt, int & iTePt );
	bool GetToId( const char * pszCallId, std::string & strToId );
	bool GetFromId( const char * pszCallId, std::string & strFromId );
	bool GetContact( const char * pszCallId, CSipCallRoute * pclsRoute );
	bool GetCdr( const char * pszCallId, CSipCdr * pclsCdr );
	bool GetInviteHeaderValue( const char * pszCallId, const char * pszHeaderName, std::string & strValue );
	int GetRSeq( const char * pszCallId );

	void SetRSeq( const char * pszCallId, int iRSeq );

	/** 특정 Call 의 Dialog 에 per-dialog override 도메인 지정.
	 *  From/To/Request-URI/P-Asserted-Identity 생성 시 전역 도메인 대신 사용.
	 *  주로 MCPTT 그룹콜에서 mcptt realm 강제 용도.
	 *  @returns Dialog 존재 시 true, 없으면 false */
	bool SetCallDomain( const char * pszCallId, const char * pszDomain );

	bool IsRingCall( const char * pszCallId, const char * pszTo );
	bool Is100rel( const char * pszCallId );
	bool IsHold( const char * pszCallId );
	bool IsConnected( const char * pszCallId );

	CSipMessage * DeleteIncomingCall( const char * pszCallId );

	// SipUserAgentMonitor.hpp
	void GetDialogString( CMonitorString & strBuf );
	void GetServerString( CMonitorString & strBuf );

	// SipUserAgentSipStack.hpp : ISipStackCallBack 
	virtual bool RecvRequest( int iThreadId, CSipMessage * pclsMessage );
	virtual bool RecvResponse( int iThreadId, CSipMessage * pclsMessage );
	virtual bool SendTimeout( int iThreadId, CSipMessage * pclsMessage );
	virtual void TcpSessionEnd( const char * pszIp, int iPort, ESipTransport eTransport );
	virtual void ThreadEnd( int iThreadId );

	SIP_SERVER_INFO_LIST	m_clsRegisterList;
	CSipMutex							m_clsRegisterMutex;

	CSipStack							m_clsSipStack;

	bool m_bStopEvent;

private:
	void DeleteRegisterInfoAll( );
	void DeRegister( );

	bool RecvRegisterResponse( int iThreadId, CSipMessage * pclsMessage );

	bool RecvInviteRequest( int iThreadId, CSipMessage * pclsMessage );
	bool RecvInviteResponse( int iThreadId, CSipMessage * pclsMessage );
	
	bool RecvByeRequest( int iThreadId, CSipMessage * pclsMessage );

	bool RecvCancelRequest( int iThreadId, CSipMessage * pclsMessage );

	bool RecvReferRequest( int iThreadId, CSipMessage * pclsMessage );
	bool RecvReferResponse( int iThreadId, CSipMessage * pclsMessage );

	bool RecvNotifyRequest( int iThreadId, CSipMessage * pclsMessage );

	bool RecvMessageRequest( int iThreadId, CSipMessage * pclsMessage );

	bool RecvPrackRequest( int iThreadId, CSipMessage * pclsMessage );

	bool RecvOptionsRequest( int iThreadId, CSipMessage * pclsMessage );

	bool SendInvite( CSipDialog & clsDialog );
	bool SetCallEnd( const char * pszCallId );
	void Delete( SIP_DIALOG_MAP::iterator & itMap );

	bool SetInviteResponse( std::string & strCallId, CSipMessage * pclsMessage, CSipCallRtp * pclsRtp, bool & bReInvite );
	bool GetSipCallRtp( CSipMessage * pclsMessage, CSipCallRtp & clsRtp );

	int GetSeqNum( );

	ISipUserAgentCallBack * m_pclsCallBack;

	SIP_DIALOG_MAP			m_clsDialogMap;
	CSipMutex						m_clsDialogMutex;

	int									m_iSeq;
	CSipMutex						m_clsMutex;

	bool								m_bStart;
};

#endif
