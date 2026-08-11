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

#ifndef _RTP_HEADER_H_
#define _RTP_HEADER_H_

#include "SipPlatformDefine.h"
#include "SipUdp.h"

// RTP 헤더를 저장하는 구조체
typedef struct _RtpHeader_
{
  uint8_t		cFlags;				// version + padding + extension
  uint8_t		cMpt;					// marker + payload type
  uint16_t	sSeq;         // sequence number
  uint32_t	iTimeStamp;   // timestamp
  uint32_t  ssrc;					// synchronization source

  // RTP 버전 정보를 가져온다.
	inline uint8_t GetVersion() 
	{
		return cFlags >> 6; 
	}

  // RTP 버전 정보를 저장한다.
	inline void SetVersion( uint8_t cVersion )
  {    
		cFlags = (cVersion << 6) | (cFlags & 0x3F);
  }

  // Padding 을 리턴한다.
	inline uint8_t GetPadding() 
	{ 
		return (cFlags >> 5) & 0x1; 
	}

  // Padding 을 저장한다.
	inline void SetPadding( uint8_t cPadding ) 
  { 
		cFlags = (cPadding << 5) | (cFlags & 0xDF);
  }

  // Extension 를 리턴한다.
	inline uint8_t GetExtension() 
	{ 
		return (cFlags >> 4) & 0x01;
	}
    
	// Extension 을 저장한다.
	inline void SetExtension( uint8_t cExt )
  {
		cFlags = (cExt << 4) | (cFlags & 0xEF);
  }

  // CSRC count 를 리턴한다.
	inline uint8_t GetCC() 
	{ 
		return cFlags & 0x0F;
	}
    
	// CSRC count 를 저장한다.
	inline void SetCC( uint8_t cCC )
  {
		cFlags = cCC | (cFlags & 0xF0);
  }

  // Marker 를 리턴한다.
	inline uint8_t GetMarker() 
	{ 
		return cMpt >> 7; 
	}

  // Marker 를 저장한다.
	inline void SetMarker( uint8_t cMarker )
  {
		cMpt = (cMarker << 7) | (cMpt & 0x7F);
  }

  // Payload type 을 리턴한다.
	inline uint8_t GetPT() 
	{ 
		return cMpt & 0x7F;
	}
    
	// Payload type 을 저장한다.
	inline void SetPT( uint8_t cPT )
  {
		cMpt = cPT | (cMpt & 0x80);
  }

	// Sequence number 를 리턴한다.
	inline uint16_t GetSeq( )
	{
		return ntohs(sSeq);
	}

	// Sequence number 를 저장한다.
	void SetSeq( uint16_t sSeqIn )
	{
		sSeq = htons(sSeqIn);
	}

	// Timestamp 을 리턴한다.
	inline uint32_t GetTimeStamp( )
	{
		return ntohl(iTimeStamp);
	}

	// Timestamp 을 저장한다.
	void SetTimeStamp( uint32_t iTime )
	{
		iTimeStamp = htonl( iTime );
	}

} RtpHeader;


#endif
