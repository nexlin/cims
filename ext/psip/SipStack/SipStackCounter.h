#ifndef _SIP_STACK_COUNTER_H_
#define _SIP_STACK_COUNTER_H_

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

/**
 * @ingroup SipStack
 * @brief SIP 신호 카운터 — 수신 요청 / 최종응답(수신·송신, 응답코드별) / 파싱 실패 누적.
 *
 * 응답코드별 성공률·CPS·수신 이상 감시의 관측 원천이다 (CIMS 기능 알람
 * A-QOS-006/007/009/011 — docs/design/alarm_catalog.csv, 평가는 응용 소관).
 *
 * 증가 지점 (스택 안 수렴점):
 *   - 수신 요청: CSipStack::RecvSipMessage — 트랜잭션 삽입 성공 후 (재전송 미포함)
 *   - 수신 최종응답: CSipStack::RecvResponse 팬아웃 — 와이어 응답(트랜잭션 dedup 후)과
 *     트랜잭션 로컬 합성 응답(408 Timer B/Ring timeout, 660 connect error)이 전부
 *     통과하는 유일 지점. 합성 응답은 와이어에 나가지 않아 flow 로그로는 관측 불가 —
 *     여기 포함이 성공률 집계의 필수 조건이다.
 *   - 송신 최종응답: CSipStack::SendSipMessage 응답 분기 — 트랜잭션 삽입 성공 시
 *     (IST/NIST 계층의 재전송은 이 지점을 지나지 않으므로 중복 미계수)
 *   - 파싱 실패/보안 차단: CSipStack::RecvSipMessage(버퍼) — 파싱 실패는 소스 IP 동반
 *
 * 전부 누적(cumulative) 카운터 — 윈도우 델타 계산은 소비자가 GetSnapshot() 2회 차분으로.
 */
class CSipStackCounter
{
public:
	/** 메서드 그룹 — 성공률 축이 필요한 INVITE/REGISTER 만 분리, 나머지는 OTHER */
	enum EGroup { E_GRP_INVITE = 0, E_GRP_REGISTER = 1, E_GRP_OTHER = 2, E_GRP_MAX = 3 };

	static const int FINAL_MIN = 200;
	static const int FINAL_MAX = 699;		// 표준 2xx~6xx + 로컬 합성 660(connect error) 포함
	static const int FINAL_SLOTS = FINAL_MAX - FINAL_MIN + 1;
	static const int PARSE_ERROR_SRC_MAX = 32;	// 파싱 실패 소스 IP 추적 상한 (Take 시 리셋)

	/** 소비자 차분용 plain 복사본 */
	struct Snapshot
	{
		uint64_t arrRecvFinal[E_GRP_MAX][FINAL_SLOTS];
		uint64_t arrSendFinal[E_GRP_MAX][FINAL_SLOTS];
		uint64_t ulRecvInviteInitial;	// To tag 없는 신규 INVITE — CPS 의 분자
		uint64_t ulRecvInviteRe;		// re-INVITE (To tag 있음)
		uint64_t ulRecvRegister;
		uint64_t ulRecvOtherRequest;	// ACK/CANCEL 포함 기타 요청
		uint64_t ulParseError;			// SIP 파싱 실패 (무응답 폐기분)
		uint64_t ulSecurityDrop;		// 보안 콜백(deny/allow) 차단분

		Snapshot() { memset( this, 0, sizeof( *this ) ); }
	};

	CSipStackCounter()
		: m_ulRecvInviteInitial( 0 ), m_ulRecvInviteRe( 0 ), m_ulRecvRegister( 0 ),
		  m_ulRecvOtherRequest( 0 ), m_ulParseError( 0 ), m_ulSecurityDrop( 0 )
	{
		for ( int g = 0; g < E_GRP_MAX; ++g )
		{
			for ( int i = 0; i < FINAL_SLOTS; ++i )
			{
				m_arrRecvFinal[g][i].store( 0, std::memory_order_relaxed );
				m_arrSendFinal[g][i].store( 0, std::memory_order_relaxed );
			}
		}
	}

	/** CSeq 메서드 → 그룹 (응답은 CSeq 메서드가 SoT — CSipMessage::IsMethod 규칙 동일) */
	static EGroup GroupOf( const char * pszCSeqMethod )
	{
		if ( pszCSeqMethod == NULL ) return E_GRP_OTHER;
		if ( strcmp( pszCSeqMethod, "INVITE" ) == 0 ) return E_GRP_INVITE;
		if ( strcmp( pszCSeqMethod, "REGISTER" ) == 0 ) return E_GRP_REGISTER;
		return E_GRP_OTHER;
	}

	void OnRecvFinal( const char * pszCSeqMethod, int iStatusCode )
	{
		if ( iStatusCode < FINAL_MIN || iStatusCode > FINAL_MAX ) return;
		m_arrRecvFinal[GroupOf( pszCSeqMethod )][iStatusCode - FINAL_MIN].fetch_add( 1, std::memory_order_relaxed );
	}

	void OnSendFinal( const char * pszCSeqMethod, int iStatusCode )
	{
		if ( iStatusCode < FINAL_MIN || iStatusCode > FINAL_MAX ) return;
		m_arrSendFinal[GroupOf( pszCSeqMethod )][iStatusCode - FINAL_MIN].fetch_add( 1, std::memory_order_relaxed );
	}

	void OnRecvInvite( bool bInitial )
	{
		( bInitial ? m_ulRecvInviteInitial : m_ulRecvInviteRe ).fetch_add( 1, std::memory_order_relaxed );
	}

	void OnRecvRegister() { m_ulRecvRegister.fetch_add( 1, std::memory_order_relaxed ); }
	void OnRecvOtherRequest() { m_ulRecvOtherRequest.fetch_add( 1, std::memory_order_relaxed ); }
	void OnSecurityDrop() { m_ulSecurityDrop.fetch_add( 1, std::memory_order_relaxed ); }

	void OnParseError( const char * pszIp )
	{
		m_ulParseError.fetch_add( 1, std::memory_order_relaxed );
		if ( pszIp == NULL || pszIp[0] == '\0' ) return;
		std::lock_guard<std::mutex> lock( m_clsSrcMutex );
		std::map<std::string, uint64_t>::iterator it = m_mapParseErrorSrc.find( pszIp );
		if ( it != m_mapParseErrorSrc.end() )
		{
			++it->second;
		}
		else if ( m_mapParseErrorSrc.size() < PARSE_ERROR_SRC_MAX )
		{
			m_mapParseErrorSrc[pszIp] = 1;
		}
	}

	void GetSnapshot( Snapshot & clsOut ) const
	{
		for ( int g = 0; g < E_GRP_MAX; ++g )
		{
			for ( int i = 0; i < FINAL_SLOTS; ++i )
			{
				clsOut.arrRecvFinal[g][i] = m_arrRecvFinal[g][i].load( std::memory_order_relaxed );
				clsOut.arrSendFinal[g][i] = m_arrSendFinal[g][i].load( std::memory_order_relaxed );
			}
		}
		clsOut.ulRecvInviteInitial = m_ulRecvInviteInitial.load( std::memory_order_relaxed );
		clsOut.ulRecvInviteRe = m_ulRecvInviteRe.load( std::memory_order_relaxed );
		clsOut.ulRecvRegister = m_ulRecvRegister.load( std::memory_order_relaxed );
		clsOut.ulRecvOtherRequest = m_ulRecvOtherRequest.load( std::memory_order_relaxed );
		clsOut.ulParseError = m_ulParseError.load( std::memory_order_relaxed );
		clsOut.ulSecurityDrop = m_ulSecurityDrop.load( std::memory_order_relaxed );
	}

	/** 파싱 실패 소스 집계를 꺼내며 리셋 — 윈도우 의미는 호출 주기가 정한다 */
	void TakeParseErrorSources( std::map<std::string, uint64_t> & clsOut )
	{
		std::lock_guard<std::mutex> lock( m_clsSrcMutex );
		clsOut.swap( m_mapParseErrorSrc );
		m_mapParseErrorSrc.clear();
	}

private:
	std::atomic<uint64_t> m_arrRecvFinal[E_GRP_MAX][FINAL_SLOTS];
	std::atomic<uint64_t> m_arrSendFinal[E_GRP_MAX][FINAL_SLOTS];
	std::atomic<uint64_t> m_ulRecvInviteInitial;
	std::atomic<uint64_t> m_ulRecvInviteRe;
	std::atomic<uint64_t> m_ulRecvRegister;
	std::atomic<uint64_t> m_ulRecvOtherRequest;
	std::atomic<uint64_t> m_ulParseError;
	std::atomic<uint64_t> m_ulSecurityDrop;

	mutable std::mutex m_clsSrcMutex;
	std::map<std::string, uint64_t> m_mapParseErrorSrc;
};

#endif
