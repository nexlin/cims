/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com>
 * (http://blog.naver.com/websearch)
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

#include "RtpThread.h"
#include "G711.h"
#include "RtpHeader.h"
#include "ServerUtility.h"
#include "SipClientSetup.h"
#include "TimeUtility.h"

#ifndef WIN32
#define ALSA_PCM_NEW_HW_PARAMS_API

#if !defined(WIN32) && !defined(NO_ALSA)
#include <alsa/asoundlib.h>
#endif
#endif

// CheckError moved from RtpThreadSend.hpp to resolve scope issues
bool CheckError(int n, const char *pszLog) {
  if (n < 0) {
#if !defined(WIN32) && !defined(NO_ALSA)
    printf("%s error - %s\n", pszLog, snd_strerror(n));
#endif
    return true;
  }

  return false;
}



#include "RtpThreadRecv.hpp"
#include "RtpThreadSend.hpp"


CRtpThread::CRtpThread()
    : m_hSocket(INVALID_SOCKET), m_hRtcpSocket(INVALID_SOCKET),
      m_hFloorRecvSocket(INVALID_SOCKET), m_iPort(0), m_iFloorRecvPort(0),
      m_bStopEvent(false), m_bSendThreadRun(false), m_bRecvThreadRun(false),
      m_bFloorRecvThreadRun(false),
      m_iDestFloorPort(0), m_iDestVideoPort(0), m_iLastFloorOp(0),
      m_hVideoSocket(INVALID_SOCKET), m_iVideoPort(0),
      m_bVideoSendThreadRun(false) {}

CRtpThread::~CRtpThread() { Destroy(); }

bool CRtpThread::Create() {
  if (m_hSocket != INVALID_SOCKET) {
    return true;
  }

  // OS 자동 포트 할당 후 RTCP=rtp+1, Video=rtp+2 순차 바인딩
  // (CSP/CMP가 포트 오프셋 관계를 전제하므로 유지)
  m_hSocket = UdpListen(0, NULL);
  if (m_hSocket == INVALID_SOCKET) {
    return false;
  }
  m_iPort = GetSocketPort(m_hSocket);

  // RTCP socket: RTP port + 1
  m_hRtcpSocket = UdpListen(m_iPort + 1, NULL);
  if (m_hRtcpSocket == INVALID_SOCKET) {
    printf("[RTP] Warning: failed to create RTCP socket on port %d\n", m_iPort + 1);
  }

  // Video socket: RTP port + 2
  if (!m_strVideoFile.empty()) {
    m_hVideoSocket = UdpListen(m_iPort + 2, NULL);
    if (m_hVideoSocket != INVALID_SOCKET) {
      m_iVideoPort = m_iPort + 2;
      printf("[RTP] Video socket created on port %d\n", m_iVideoPort);
    } else {
      printf("[RTP] Warning: failed to create video socket on port %d\n", m_iPort + 2);
    }
  }

  // Floor receive socket: OS 자동 포트 할당
  m_hFloorRecvSocket = UdpListen(0, NULL);
  if (m_hFloorRecvSocket != INVALID_SOCKET) {
    m_iFloorRecvPort = GetSocketPort(m_hFloorRecvSocket);
    printf("[RTP] Floor recv socket created on port %d\n", m_iFloorRecvPort);
  } else {
    printf("[RTP] Warning: failed to create floor recv socket\n");
  }

  return true;
}

bool CRtpThread::Destroy() {
  if (m_hSocket != INVALID_SOCKET) {
    closesocket(m_hSocket);
    m_hSocket = INVALID_SOCKET;
  }
  if (m_hRtcpSocket != INVALID_SOCKET) {
    closesocket(m_hRtcpSocket);
    m_hRtcpSocket = INVALID_SOCKET;
  }
  if (m_hVideoSocket != INVALID_SOCKET) {
    closesocket(m_hVideoSocket);
    m_hVideoSocket = INVALID_SOCKET;
  }
  if (m_hFloorRecvSocket != INVALID_SOCKET) {
    closesocket(m_hFloorRecvSocket);
    m_hFloorRecvSocket = INVALID_SOCKET;
  }

  return true;
}

bool CRtpThread::Start(const char *pszDestIp, int iDestPort) {
  if (m_hSocket == INVALID_SOCKET) {
    return false;
  }

  m_strDestIp = pszDestIp;
  m_iDestPort = iDestPort;

  if (m_bSendThreadRun || m_bRecvThreadRun) {
    return true;
  }

#ifndef WIN32
  if (StartThread("RtpThreadSend", RtpThreadSend, this) == false) {
    Stop();
    return false;
  }

  if (StartThread("RtpThreadRecv", RtpThreadRecv, this) == false) {
    Stop();
    return false;
  }

  // Launch video send thread if video file is configured and socket is ready
  if (m_hVideoSocket != INVALID_SOCKET && !m_strVideoFile.empty()) {
    if (StartThread("RtpThreadVideoSend", RtpThreadVideoSend, this) == false) {
      printf("[RTP] Warning: failed to start video send thread\n");
    }
  }

  // Floor 수신 스레드: floor 소켓이 열려있으면 항상 시작
  if (m_hFloorRecvSocket != INVALID_SOCKET) {
    if (StartThread("RtpThreadFloorRecv", RtpThreadFloorRecv, this) == false) {
      printf("[RTP] Warning: failed to start floor recv thread\n");
    }
  }
#endif

  return true;
}

bool CRtpThread::Stop() {
  m_bStopEvent = true;

  for (int i = 0; i < 100; ++i) {
    if (m_bSendThreadRun == false && m_bRecvThreadRun == false
        && m_bVideoSendThreadRun == false && m_bFloorRecvThreadRun == false) {
      break;
    }

    MiliSleep(20);
  }

  m_bStopEvent = false;

  return true;
}

bool CRtpThread::SendFloorControl(int iOpCode) {
    // floor 패킷은 SDP m=application에 광고한 동일 소켓(m_hFloorRecvSocket)으로 보내야 함.
    // CMP는 멤버의 floor 포트(JOIN_PTT_GROUP의 user_floor_port)에서 오는 패킷만 해당 멤버로 인식.
    Socket hSock = (m_hFloorRecvSocket != INVALID_SOCKET) ? m_hFloorRecvSocket : m_hSocket;
    if (hSock == INVALID_SOCKET) return false;

    // Construct RTCP APP Packet
    // Header (4 bytes) + SSRC (4 bytes) + Name (4 bytes) + Data (variable)
    // RTCP Header: V=2, P=0, Subtype=1 (Floor), PT=204 (APP), Len
    
    uint8_t buffer[1024];
    uint8_t *ptr = buffer;
    
    // 1. RTCP Header — TS 24.380 §8.2: opcode goes in Subtype(5bits), not app-data
    // V=2(10), P=0, Subtype=opcode
    *ptr++ = 0x80 | ((uint8_t)iOpCode & 0x1F);
    *ptr++ = 204; // PT=APP

    // Length: (total_bytes / 4) - 1.  Header(4)+SSRC(4)+Name(4)+Data(4) = 16 bytes = 3
    uint16_t len = 3;
    *ptr++ = (len >> 8) & 0xFF;
    *ptr++ = len & 0xFF;

    // 2. SSRC
    uint32_t ssrc = 0x12345678;
    *ptr++ = (ssrc >> 24) & 0xFF;
    *ptr++ = (ssrc >> 16) & 0xFF;
    *ptr++ = (ssrc >> 8) & 0xFF;
    *ptr++ = ssrc & 0xFF;

    // 3. Name "MCPT"
    *ptr++ = 'M'; *ptr++ = 'C'; *ptr++ = 'P'; *ptr++ = 'T';

    // 4. App-data (opcode field cleared, id_len=0, reserved=0)
    *ptr++ = 0; *ptr++ = 0; *ptr++ = 0; *ptr++ = 0;
    
    int packetLen = ptr - buffer;
    
    // Send to Dest IP/Port (RTCP port is RTP + 1 usually)
    // But verify_ptt.py uses specific server port. 
    // In SipClient, m_iDestPort is the remote audio port.
    // CMP listens for RTCP on +1.
    
    // NOTE: CmpServer logic handles multiplexed or separate. Usually RTCP is Port+1.
    
    struct sockaddr_in startAddr;
    memset(&startAddr, 0, sizeof(startAddr));
    startAddr.sin_family = AF_INET;
    startAddr.sin_addr.s_addr = inet_addr(m_strDestIp.c_str());
    int floorPort = (m_iDestFloorPort > 0) ? m_iDestFloorPort : (m_iDestPort + 1);
    startAddr.sin_port = htons(floorPort); // Floor control port (m=application)
    
    int n = sendto(hSock, (const char*)buffer, packetLen, 0, (struct sockaddr *)&startAddr, sizeof(startAddr));
    if (n < 0) {
        printf("SendFloorControl error: %s\n", strerror(errno));
        return false;
    }
    
    return true;
}
