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

#ifndef _CSP_USER_H_
#define _CSP_USER_H_

#include <strings.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "CspServerDefine.h"
#include "SipMutex.h"

/**
 * @ingroup CspServer
 * @brief 사용자 MCPTT 프로파일 (ptt_user_profile — TS 24.484 / TS 24.379 §6.3.3.1.13.2).
 *        행 부재 시 기본값 = 모드 DedicatedGroup + 긴급그룹 미지정(긴급 미인가) + 인가 전부 허용.
 */
struct CspUserProfile {
    bool m_bAllowEmergencyCall = true;      ///< allow-emergency-group-call (긴급 그룹콜 개시 인가)
    bool m_bAllowEmergencyAlert = true;     ///< allow-activate-emergency-alert (경보 개시 인가)
    bool m_bAllowAdhocCall = true;          ///< ad hoc 개시 인가 (Setup.PttAdhocEnabled 와 AND)
    std::string m_strEmergencyGroupMode = "DedicatedGroup";  ///< entry-info: DedicatedGroup|UseCurrentlySelectedGroup
    std::string m_strEmergencyGroupId;      ///< 전용 긴급그룹 (mcptt_group_id, 빈 값=미지정)
};

/**
 * @ingroup CspServer
 * @brief SIP 사용자 정보 저장 클래스
 */
class CspUser {
public:
    CspUser() : m_bDnd( false ) {
        m_iCreateTime = 0;
        m_iUpdateTime = 0;
        m_iRegisterTime = 0;
        m_iLogoutTime = 0;
    };
    ~CspUser() {};

    std::string m_strId;

    // 표시 이름
    std::string m_strName;

    // SIP 인증용 아이디 (IMS 등에서 전화번호와 분리된 단말기 고유 ID. 없으면 m_strId와 동일시함)
    std::string m_strAuthId;

    // SIP 비밀번호 (평문). 가입자 경로는 m_strHa1 로 전환되어 과도기 fallback 으로만 남는다.
    //   원격 노드(peer) outbound 인증 자격(ModuleDispatcher 의 RouteConfig.auth_password)은 계속 사용.
    std::string m_strPassWord;

    // SIP Digest H(A1) = MD5(impi:realm:password) — 인증 자료 SoT (sip_access_security.md §4).
    //   비어 있으면 m_strPassWord 로 종전 계산(과도기).
    std::string m_strHa1;

    // 채널 정책 (sip_access_security.md §3.1) — DB sip_transport ENUM('UDP','TCP','TLS') / NULL.
    //   "TLS" 만 서버가 집행한다(비-TLS 채널의 이 신원 요청은 403). 나머지는 프로비저닝 힌트.
    std::string m_strSipTransport;

    /** TLS 채널 강제 대상 가입자인가 (sip_transport=TLS). */
    bool requiresTls() const {
        return strcasecmp( m_strSipTransport.c_str(), "TLS" ) == 0;
    }

    // v3 (2026-04-22): 서비스 귀속을 name 기반 참조로 이전.
    //   - m_strServiceRef = access_services.name (빈 문자열이면 REGISTER 거부)
    //   - service.domain 과 결합하여 Digest username (full IMPI) 구성
    //   - m_strImsi 가 비면 m_strAuthId 를 fallback 으로 사용
    std::string m_strServiceRef;  // access_services.name 참조
    std::string m_strImsi;

    // 착신거부 ( Do Not Disturb )
    bool m_bDnd;

    // 개별 착신 거부
    std::vector<std::string> m_vecReject;

    // 착신전환 ( Call Forward )
    std::string m_strForward;

    // 서비스 타입: "volte" | "ptt" | "both"
    std::string m_strServiceType;

    // 소속 아이디
    std::string m_strOrganizationId;

    // 가입자가 생성된 시간
    time_t m_iCreateTime;
    // 가입자 정보가 마지막으로 수정된 시간
    time_t m_iUpdateTime;

    // 마지막 Register 시간
    time_t m_iRegisterTime;
    // 마지막 Logout 시간
    time_t m_iLogoutTime;

    bool isDnd() {
        return m_bDnd;
    };
    bool isCallForward() {
        return m_strForward.empty() == false;
    };
    bool isReject( std::string strFromId ) {
        return std::find( m_vecReject.begin(), m_vecReject.end(), strFromId ) != m_vecReject.end();
    }

    // bool Parse( const char *pszFileName );
    void clear();

    friend class CspUserMap;
    friend class CDbManager;

private:
    time_t _loadTime;
    // bool IsDnd();
    // bool IsCallForward();
};

// 가입자 정보를 관리하는 클래스
// Caching User Data
typedef std::map<std::string, CspUser> CSP_USER_MAP;

class CspUserMap {
public:
    // isUser : alive user
    bool isAlive( std::string strToId, CspUser &clsUser );
    bool select( std::string strToId, CspUser &clsUser );

    bool registerUser( std::string strUserId, std::string strPassWord );
    bool unregisterUser( std::string strUserId );
    bool Select( const char *pszUserId, CspUser &clsXmlUser );
    void Insert( CspUser &clsXmlUser );
    bool Load( const char *pszDirName );
    bool LoadFromDb();
    bool Remove( std::string strUserId );
    bool ReloadFromDb( std::string strUserId );

private:
    CSP_USER_MAP m_clsMap;
    CSipMutex m_clsMutex;
    bool _loadUserFromFile( std::string strUserId, CspUser &clsUser );

    bool _remove( std::string strUserId );
    bool _update( CspUser &clsUser );
};

extern CspUserMap gclsCspUserMap;

#endif
