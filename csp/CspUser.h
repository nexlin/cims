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

#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "CspServerDefine.h"
#include "SipMutex.h"

/**
 * @ingroup CspServer
 * @brief SIP 사용자 정보 저장 클래스
 */
class CspUser {
public:
    CspUser() : m_bDnd(false) {
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

    // SIP 비밀번호
    std::string m_strPassWord;

    // P7: 서비스 귀속 + IMSI (user 파트)
    //   - service.domain 과 결합하여 Digest username (full IMPI) 구성
    //   - m_iServiceId == 0 이면 REGISTER 거부
    //   - m_strImsi 가 비면 m_strAuthId 를 fallback 으로 사용
    int         m_iServiceId = 0;
    std::string m_strImsi;

    // 착신거부 ( Do Not Disturb ) 
    bool m_bDnd;

    // 개별 착신 거부
    std::vector<std::string> m_vecReject;

    // 착신전환 ( Call Forward ) 
    std::string m_strForward;

    // 서비스 타입: "voip" | "ptt" | "both"
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

    bool isDnd() { return m_bDnd; };
    bool isCallForward() { return m_strForward.empty() == false; };
    bool isReject(std::string strFromId) { 
        return std::find(m_vecReject.begin(), m_vecReject.end(), strFromId) != m_vecReject.end(); 
    }
 

    //bool Parse( const char *pszFileName );
    void clear();

    friend class CspUserMap;
    friend class CDbManager;

private:
    time_t _loadTime;
    //bool IsDnd();
    //bool IsCallForward();
};


// 가입자 정보를 관리하는 클래스
// Caching User Data
typedef std::map<std::string, CspUser> CSP_USER_MAP;

class CspUserMap {
public:
    // isUser : alive user
    bool isAlive(std::string strToId, CspUser & clsUser);
    bool select(std::string strToId, CspUser & clsUser);

    bool registerUser(std::string strUserId, std::string strPassWord);
    bool unregisterUser(std::string strUserId);
    bool Select( const char *pszUserId, CspUser &clsXmlUser );
    void Insert( CspUser &clsXmlUser );
    bool Load( const char *pszDirName );
    bool LoadFromDb();
    bool Remove(std::string strUserId);
    bool ReloadFromDb(std::string strUserId);

private:
    CSP_USER_MAP m_clsMap;
    CSipMutex m_clsMutex;
    bool _loadUserFromFile(std::string strUserId, CspUser &clsUser);

    bool _remove(std::string strUserId);
    bool _update(CspUser &clsUser);

};


extern CspUserMap gclsCspUserMap;


#endif
