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

#ifndef _SIP_CODEC_TABLE_H_
#define _SIP_CODEC_TABLE_H_

#include <string>
#include <vector>

/**
 * @ingroup SipUserAgent
 * @brief SDP 오디오 코덱 1개의 정의 (설정 주입형 코덱 테이블 엔트리)
 */
class CSipCodecEntry
{
public:
	CSipCodecEntry();

	/** 오퍼 생성 시 광고할 payload type. 정적 코덱은 RFC 3551 고정 번호(0~34),
	 *  동적 코덱은 96~127 — 동적 PT 의 의미는 rtpmap 으로 계약되므로 번호 자체는 정책값이다. */
	int					m_iPt;

	/** rtpmap encoding name (예: "AMR-WB") — 대소문자 무시 비교 */
	std::string	m_strName;

	/** rtpmap clock rate (예: 16000) */
	int					m_iClockRate;

	/** rtpmap 채널 수 — 0 이면 rtpmap 에 /채널 미표기 */
	int					m_iChannels;

	/** a=fmtp 값 — 비어 있으면 fmtp 미출력 */
	std::string	m_strFmtp;

	/** a=ptime 값 — 0 이면 ptime 미출력 */
	int					m_iPtime;

	/** rtpmap 표기 생성 (예: "AMR-WB/16000/1") */
	std::string GetRtpmap() const;

	/** 오퍼 rtpmap 매칭용 prefix 생성 (예: "AMR-WB/16000" — 채널 표기 유무 무관 매칭) */
	std::string GetMatchPrefix() const;
};

typedef std::vector< CSipCodecEntry > SIP_CODEC_ENTRY_LIST;

/**
 * @ingroup SipUserAgent
 * @brief 프로세스 전역 오디오 코덱 테이블 — SDP 오퍼/answer 의 코덱·PT·fmtp·우선순위 정본.
 *
 *  - 배열 순서 = 우선순위 (첫 엔트리가 최우선 = 서비스 코덱)
 *  - telephone-event(DTMF) 는 코덱이 아니라 이벤트 슬롯이라 별도 보관하며 SDP 에 항상 부가된다.
 *  - 응용(CSP)이 기동 시 Set() 으로 주입한다. 미주입 시 기본 테이블(AMR-WB 96 최우선 +
 *    구 화이트리스트 승계 G.711 계열 + telephone-event 101).
 *  - 주입은 SIP stack 기동 전 1회 — 기동 후 변경은 미지원 (설정 변경은 재기동 반영).
 */
class CSipCodecTable
{
public:
	/** 코덱 테이블 주입. clsList 안의 name="telephone-event" 엔트리는 DTMF 슬롯으로 분리 보관한다.
	 *  코덱 엔트리가 0개인 리스트는 무시(기본 테이블 유지). */
	static void Set( const SIP_CODEC_ENTRY_LIST & clsList );

	/** 코덱 목록 (telephone-event 제외, 우선순위 순) */
	static const SIP_CODEC_ENTRY_LIST & GetList();

	/** 최우선 코덱 (테이블 첫 엔트리) */
	static const CSipCodecEntry & GetTop();

	/** telephone-event 엔트리 */
	static const CSipCodecEntry & GetTelephoneEvent();

	/** payload type 으로 검색 — 없으면 NULL */
	static const CSipCodecEntry * FindByPt( int iPt );

	/** rtpmap 값의 encoding 부분("<enc>/<clock>[/<ch>]")으로 검색 — 없으면 NULL */
	static const CSipCodecEntry * FindByRtpmap( const char * pszEncoding );

	/** 테이블 내 우선순위 rank (0=최우선). 없으면 -1 */
	static int GetRank( int iPt );
};

#endif
