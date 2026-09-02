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

#ifndef _RTP_THREAD_H_
#define _RTP_THREAD_H_

#include "SipUdp.h"
#include <string>
#include <vector>
#include <atomic>
#include <set>
#include <mutex>

// libsrtp 불투명 핸들 전방선언 (srtp2/srtp.h 는 RtpThread.cpp 에서만 포함)
struct srtp_ctx_t_;

class CRtpThread
{
public:
	CRtpThread();
	~CRtpThread();

	bool Create( );
	bool Destroy( );
	bool Start( const char * pszDestIp, int iDestPort );
	bool Stop( );

    bool SendFloorControl(int iOpCode);

    /** 미디어 파일 경로 (AMR-WB raw 프레임 파일) 설정 — 비어있으면 합성 RTP */
    void SetMediaFile(const std::string& strPath) { m_strMediaFile = strPath; }

    /** 비디오 파일 경로 (H.264 Annex B raw NAL 파일) 설정 */
    void SetVideoFile(const std::string& strPath) { m_strVideoFile = strPath; }

	/** 협상된 오디오 wire PT (SDP 오퍼/answer 확정값) — 파일 미디어(AMR-WB) 송신 시 스탬핑.
	 *  -1 = 미협상(레거시 99 폴백). 합성 PCMU 는 정적 PT 0 고정. */
	int		m_iAudioPt = -1;

	// ── 미디어 SRTP (SDES — media_security.md §8.2). a=crypto 는 m-line 단위(RFC 4568 §5)라
	//    오디오·비디오가 각자 독립 컨텍스트(키)를 가진다. ──
	/** 협상 키 주입 — inline 키는 base64(key16||salt14). local=자기 선언(tx), remote=상대 선언(rx).
	 *  기존 컨텍스트는 폐기 후 재생성. 실패 시 false — 호출자가 호를 정리한다(평문 조용 폴백 금지). */
	bool SetSrtpKeys( const std::string & strSuite, const std::string & strLocalInlineB64,
	                  const std::string & strRemoteInlineB64 );
	bool SetVideoSrtpKeys( const std::string & strSuite, const std::string & strLocalInlineB64,
	                       const std::string & strRemoteInlineB64 );
	void ClearSrtp();          // 오디오+비디오 모두 해제
	void ClearVideoSrtp();
	bool SrtpEnabled() const { return m_clsSrtpAudio.pTx != NULL; }
	bool VideoSrtpEnabled() const { return m_clsSrtpVideo.pTx != NULL; }
	/** in-place 변환 — 성공 시 iLen 갱신. protect 는 iCap ≥ iLen+16 필요. */
	bool SrtpProtect( char * pszBuf, int & iLen, int iCap );
	bool SrtpUnprotect( char * pszBuf, int & iLen );
	bool SrtpVideoProtect( char * pszBuf, int & iLen, int iCap );
	bool SrtpVideoUnprotect( char * pszBuf, int & iLen );

	Socket	m_hSocket;
	Socket	m_hRtcpSocket;       // RTCP 소켓 (RTP 포트 + 1)
	Socket  m_hFloorRecvSocket;  // floor 수신 소켓 (m=application)
	int		m_iPort;
	int     m_iFloorRecvPort;    // 로컬 floor 수신 포트
	bool	m_bStopEvent;
	bool	m_bSendThreadRun;
	bool	m_bRecvThreadRun;
    bool    m_bFloorRecvThreadRun;
	std::string	m_strDestIp;
	int		m_iDestPort;
	int		m_iDestFloorPort;    // 서버 floor 포트 (m=application, 0이면 미학습)
	int		m_iDestVideoPort;    // 서버 비디오 포트 (0=미협상 — 비디오 미송신)
	std::string m_strUserId;     // floor 메시지 FF_USER_ID (TS 24.380 §8.2.3.6 — NAT 에서 멤버 식별)
    std::string m_strMediaFile;
    std::atomic<int>  m_iLastFloorOp;   // 마지막 수신 floor subtype (TS 24.380: 1=GRANTED,2=TAKEN,5=IDLE,...)
    std::atomic<bool> m_bGrantReceived; // GRANTED(subtype=1) 수신 여부 — TAKEN이 덮어써도 보존
    std::atomic<int>  m_iFloorDenyCount{0};   // DENY(subtype=3) 수신 누계 — 청취 leg 의 floor 요청 거절 판정(ptt_listen)
    std::atomic<int>  m_iFloorTakenCount{0};  // TAKEN(subtype=2) 수신 누계 — 청취자가 발언자 통지를 받는지 판정
    // 누적 수신 RTP 패킷 수 (리셋 없음) — 전달·당겨받기 후 재고정된 leg 로 미디어가 실제로
    //   흐르는지 검증하는 표식 (S3-SCN-XFER/PICKUP). recv 스레드가 unprotect 통과분만 센다.
    std::atomic<unsigned long long> m_ullRecvTotal{0};
    // 수신 audio RTP 의 서로 다른 SSRC 집합 — 청취(감청) leg 가 한 m-line 에서 SSRC 2개(caller/callee)
    //   를 받는지 검증(S3-SCN-MONITOR). recv 스레드가 헤더 SSRC 를 넣는다.
    std::set<unsigned int> m_setRecvSsrc;
    std::mutex m_mtxSsrc;
    size_t RecvSsrcCount() { std::lock_guard<std::mutex> lk(m_mtxSsrc); return m_setRecvSsrc.size(); }

    // Video RTP
    Socket  m_hVideoSocket;
    int     m_iVideoPort;
    bool    m_bVideoSendThreadRun;
    std::string m_strVideoFile;

private:
    // 미디어 SRTP 컨텍스트 (libsrtp) — tx=ssrc_any_outbound / rx=ssrc_any_inbound, m-line 별 1쌍
    struct SrtpSession {
        srtp_ctx_t_ * pTx = NULL;
        srtp_ctx_t_ * pRx = NULL;
    };
    SrtpSession m_clsSrtpAudio;
    SrtpSession m_clsSrtpVideo;
    static bool SetSessionKeys( SrtpSession & clsSes, const char * pszMedia, const std::string & strSuite,
                                const std::string & strLocalInlineB64, const std::string & strRemoteInlineB64 );
    static void ClearSession( SrtpSession & clsSes );
    static bool Protect( SrtpSession & clsSes, char * pszBuf, int & iLen, int iCap );
    static bool Unprotect( SrtpSession & clsSes, char * pszBuf, int & iLen );
};

#endif
