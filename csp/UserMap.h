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

#ifndef _USER_MAP_H_
#define _USER_MAP_H_

#include <map>
#include <vector>

#include "CspUser.h"
#include "SipMessage.h"
#include "SipMutex.h"
#include "SipUserAgent.h"

typedef std::list<std::string> USER_ID_LIST;

class CUserInfo;
/** (사용자 ID, 삭제 시점 바인딩) 쌍 — 만료 sweep 이 reg-event NOTIFY 용으로 반환 */
typedef std::list<std::pair<std::string, CUserInfo> > USER_INFO_LIST;

/**
 * @ingroup CspServer
 * @brief SIP 클라이언트 정보 저장 클래스
 */
class CUserInfo {
public:
    CUserInfo();
    void GetCallRoute( CSipCallRoute &clsRoute );

    /** 클라이언트 IP 주소 */
    std::string m_strIp;

    /** 클라이언트 포트 번호 */
    int m_iPort;

    /** 클라이언트 연결 트랜스포트 */
    ESipTransport m_eTransport;

    /** 로그인 시간 */
    time_t m_iLoginTime;

    /** 로그인 timeout 시간 (초단위) */
    int m_iLoginTimeout;

    /** OPTIONS 메시지 전송 SEQ 번호 */
    int m_iOptionsSeq;

    /** OPTIONS 메시지 전송 시간 */
    time_t m_iSendOptionsTime;

    /** 저장된 도달 경로(latch)로 마지막으로 요청이 도착한 시각.
     *  수신 소스가 저장값과 일치할 때만 갱신되므로, "이 latch 가 아직 살아 있는가" 의 근거가 된다.
     *  stale latch 진단용 — 판정 로직에는 아직 쓰지 않는다. */
    time_t m_iLastSeenTime;

    /** Call Pickup 을 위한 그룹 아이디 */
    std::string m_strGroupId;

    /** REGISTER Contact 에 MCData ICSI feature tag(+g.3gpp.icsi-ref=...icsi.mcdata...) 광고 —
     *  MSRP(media plane) 배포 가능 단말 표시. fan-out 하이브리드 분기에 사용. */
    bool m_bMcDataMsrp;

    /** 단말이 REGISTER Contact 에 실은 URI 원문 (as-registered, RFC 3261 §10.3).
     *  200 OK Contact 에코·reginfo <uri> 용 — NAT 뒤 단말은 사설 주소일 수 있으므로
     *  실제 도달 주소는 m_strIp:m_iPort(received/rport latch)를 사용한다. */
    std::string m_strContactUri;

    /** REGISTER Contact 의 파라미터 목록 (expires 제외) — reginfo <unknown-param> 용 */
    SIP_PARAMETER_LIST m_clsContactParamList;

    /** 마지막 REGISTER 의 CSeq — reginfo <contact cseq=""> 용 */
    int m_iRegisterCSeq;

    /** RFC 3329 sec-agree 협상(tls)을 거쳐 Security-Verify 대조까지 통과한 등록 —
     *  3GPP 의 integrity-protected 상당 내부 플래그(sip_access_security.md §8.1). 이 바인딩이
     *  살아있는 동안 그 신원은 채널 정책 게이트(§3)의 TLS 강제 대상이 된다. */
    bool m_bIntegrityProtected;

    /** IPsec 등록(sip_access_security.md §8.3) — 이 바인딩에 결부된 SA 셋의 reqid (0 = TLS/평문 바인딩).
     *  식별 키는 여전히 (m_strIp, m_iPort=port_uc, UDP/TCP). 서버 발신은 port_us 로, port_pc 리스너 소켓에서. */
    uint32_t m_iSaReqId;
    int m_iSendPort;        // 단말 보호 서버 포트 port_us (0 = m_iPort 로 발신)
    int m_iSendListenerId;  // 발신 소켓 = IPsec 접속점 client 역할 리스너 (Via 자기주소도 그 bind)

    int GetSendPort() const {
        return m_iSendPort > 0 ? m_iSendPort : m_iPort;
    }
    bool IsIpsec() const {
        return m_iSaReqId != 0;
    }
};

/** 한 가입자(AoR)의 등록 바인딩 목록 — 도달 경로(flow) 하나가 원소 하나다.
 *  키는 (IP, 포트, transport) 이며 스트림 transport 에서는 psip 소켓맵 키와 같은 값이다.
 *  정본 설계: docs/design/features/registration_binding_set.md */
typedef std::vector<CUserInfo> USER_BINDING_LIST;

typedef std::map<std::string, USER_BINDING_LIST> USER_MAP;

/**
 * @ingroup CspServer
 * @brief 로그인한 사용자들의 정보를 저장하는 클래스
 */
class CUserMap {
public:
    CUserMap();
    ~CUserMap();

    /** bIntegrityProtected: 이 REGISTER 가 sec-agree 협상·대조를 통과했다 (바인딩에 플래그 결부).
     *  pclsIpsec: IPsec 등록이면 SA 셋 결부 정보 (m_iSaReqId/m_iSendPort/m_iSendListenerId 만 읽는다). */
    bool Insert( CSipMessage *pclsMessage, CspUser *pclsXmlUser, bool bIntegrityProtected = false,
                 const CUserInfo *pclsIpsec = NULL );
    /** 살아있는 바인딩 중 sec-agree 로 결부된 것이 하나라도 있는가 — 채널 정책 게이트의 판정 축. */
    bool IsIntegrityProtected( const char *pszUserId );
    bool Select( const char *pszUserId, CUserInfo &clsInfo );
    bool Select( const char *pszUserId );
    bool SelectGroup( const char *pszGroupId, USER_ID_LIST &clsList );
    bool Delete( const char *pszUserId );

    /** 도달 주소 갱신 — (IP, 포트, transport) 는 한 세트이므로 항상 함께 옮긴다.
     *  셋 중 하나만 바꾸면 "TCP 포트에 UDP 발송" 같은 불일치가 생겨 NAT 이 전량 폐기한다.
     *  호출자는 **수신 transport 가 저장 transport 와 같을 때만** 갱신할 것
     *  (근거: Insert() 주석의 latch 규율). */
    bool SetIpPort( const char *pszUserId, const char *pszIp, int iPort, ESipTransport eTransport );

    /** 저장된 도달 경로로 요청이 도착했음을 기록한다 (m_iLastSeenTime).
     *  수신 transport 가 저장 transport 와 다르면 무시한다 — 그 요청은 latch 의 생존 근거가 아니다. */
    void TouchFlow( const char *pszUserId, ESipTransport eTransport );

    void DeleteTimeout( int iTimeout );
    void DeleteTimeout( int iTimeout, USER_ID_LIST &clsDeletedList );
    void DeleteTimeout( int iTimeout, USER_INFO_LIST &clsDeletedInfoList );
    void SendOptions();

    void GetRegisteredUsers( USER_ID_LIST &clsList );

    void GetString( CMonitorString &strBuf );

private:
    /** 살아있는 바인딩 중 가장 최근 것의 인덱스. 살아있는 것이 없으면 가장 최근 바인딩.
     *  생존 판정은 스트림 transport 만 스택에 묻는다(UDP 는 연결 개념이 없어 항상 살아있는
     *  것으로 취급하고 등록 만료에 맡긴다) — registration_binding_set.md §2.1.
     *  호출 전 m_clsMutex 를 잡고 있어야 한다. */
    static size_t _pickBinding( const USER_BINDING_LIST &clsList );

    /** (IP, 포트, transport) 가 같은 바인딩의 인덱스. 없으면 npos. */
    static size_t _findBinding( const USER_BINDING_LIST &clsList, const std::string &strIp, int iPort,
                                ESipTransport eTransport );

    /** 가입자당 바인딩 상한 — 죽은 flow 가 만료 전에 누적되는 것을 막는다(초과 시 최고령 제거). */
    static const size_t MAX_BINDING_PER_USER = 8;

    USER_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CUserMap gclsUserMap;

#endif
