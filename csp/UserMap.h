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
};

typedef std::map<std::string, CUserInfo> USER_MAP;

/**
 * @ingroup CspServer
 * @brief 로그인한 사용자들의 정보를 저장하는 클래스
 */
class CUserMap {
public:
    CUserMap();
    ~CUserMap();

    bool Insert( CSipMessage *pclsMessage, CspUser *pclsXmlUser );
    bool Select( const char *pszUserId, CUserInfo &clsInfo );
    bool Select( const char *pszUserId );
    bool SelectGroup( const char *pszGroupId, USER_ID_LIST &clsList );
    bool Delete( const char *pszUserId );

    bool SetIpPort( const char *pszUserId, const char *pszIp, int iPort );

    void DeleteTimeout( int iTimeout );
    void DeleteTimeout( int iTimeout, USER_ID_LIST &clsDeletedList );
    void DeleteTimeout( int iTimeout, USER_INFO_LIST &clsDeletedInfoList );
    void SendOptions();

    void GetRegisteredUsers( USER_ID_LIST &clsList );

    void GetString( CMonitorString &strBuf );

private:
    USER_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CUserMap gclsUserMap;

#endif
