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

#include "CspUser.h"

#include "CspServer.h"
#include "Directory.h"
#include "MemoryDebug.h"
#include "Log.h"
#include "SipParserDefine.h"
#include "SipServerSetup.h"
#include "TimeUtility.h"
#include "XmlElement.h"
#include "SimpleJson.h"
#include <fstream>
#include <sstream>
#include <iomanip>

CspUserMap gclsCspUserMap;


/**
 * @ingroup CspServer
 * @brief 내부 변수를 초기화시킨다.
 */
void CspUser::clear() {
    m_strId.clear();
    m_strAuthId.clear();
    m_strPassWord.clear();
    m_bDnd = false;
    m_strOrganizationId.clear();
    m_vecReject.clear();
    m_strForward.clear();
    m_iCreateTime = 0;
    m_iUpdateTime = 0;
    m_iRegisterTime = 0;
    m_iLogoutTime = 0;
}



//std::string strFileName = gclsSetup.m_strUserDataFolder; 
/**/    
bool CspUserMap::_loadUserFromFile(std::string strUserId, CspUser &clsUser) {
    std::string strFileName = strUserId + ".json";
    
    // Assuming m_strUserDataFolder is "cims/csp/User" as requested
    std::string strFolderName = gclsSetup.m_strUserDataFolder;
    std::string strFilePath = strFolderName + "/" + strFileName;

    std::ifstream t(strFilePath);
    if (!t.is_open()) {
        // CLog::Print(LOG_DEBUG, "User File not found: %s", strFilePath.c_str());
        return false;
    }
    
    std::stringstream buffer;
    buffer << t.rdbuf();

    SimpleJson::JsonNode jsonUser = SimpleJson::JsonNode::Parse(buffer.str());

    if(jsonUser.type != SimpleJson::JSON_OBJECT) { // ID check might be redundant if we trust filename map
        CLog::Print(LOG_ERROR, "[CspUserMap] Invalid JSON format for user %s", strUserId.c_str());
        return false;
    }
    // Handle simplified JSON structure where ID might not be in file or is implied.
    // User sample: ID in file? "0000001000.json"
    
    clsUser.m_strId = strUserId;
    if (jsonUser.Has("auth_id")) {
        clsUser.m_strAuthId = jsonUser.GetString("auth_id");
    } else {
        clsUser.m_strAuthId = strUserId;
    }
    
    if (jsonUser.Has("passwd")) clsUser.m_strPassWord = jsonUser.GetString("passwd");
    if (jsonUser.Has("org_id")) clsUser.m_strOrganizationId = jsonUser.GetString("org_id");
    
    std::string dnd = jsonUser.GetString("dnd");
    clsUser.m_bDnd = (dnd == "true");
    
    if (jsonUser.Has("forward_id")) clsUser.m_strForward = jsonUser.GetString("forward_id");
    
    if (jsonUser.Has("reject_id")) {
        SimpleJson::JsonNode rejectNode = jsonUser.Get("reject_id");
        if (rejectNode.type == SimpleJson::JSON_ARRAY) {
            for(size_t i=0; i<rejectNode.Size(); i++) {
                clsUser.m_vecReject.push_back(rejectNode.At(i).AsString());
            }
        }
    }
    
    // Time parsing (assuming string format "YYYY-MM-DD HH:MM:SS" or similar)
    // SimpleJson returns string for "create_time"
    std::string sTime = jsonUser.GetString("create_time");
    // TODO: Use TimeUtility::GetTime or similar if available, else 0
    // clsUser.m_iCreateTime = ...
    
    sTime = jsonUser.GetString("update_time");
    // clsUser.m_iUpdateTime = ...
    
    // Register/Logout time might not be in static JSON config usually?
    // If they are state fields, they might be in a separate DB or dynamic file.
    // For now, let's just leave them as 0 if not present.
    
    clsUser._loadTime = time(NULL);
    return true;
}


void CspUserMap::Insert( CspUser &clsXmlUser ) {
    CSP_USER_MAP::iterator itMap;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( clsXmlUser.m_strId );
    if ( itMap == m_clsMap.end() ) {
        m_clsMap.insert( CSP_USER_MAP::value_type( clsXmlUser.m_strId, clsXmlUser ) );
    } else {
        itMap->second = clsXmlUser;
    }
    m_clsMutex.release();
}



bool CspUserMap::isAlive(std::string strToId, CspUser & clsUser) {
    CSP_USER_MAP::iterator itMap;
    bool bRes = false;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( strToId );
    if ( itMap != m_clsMap.end() ) {
        clsUser = itMap->second;
        bRes = true;
    }
    m_clsMutex.release();

    if ( bRes ) {
         time_t registerTime = clsUser.m_iRegisterTime;
         time_t curTime = time(NULL);
         
         // Timeout Check
         if ( curTime - registerTime >= gclsSetup.m_iUserTimeout ) {
             return false;
         }
         return true;
    }
    
    return false; // Not found
}

bool CspUserMap::Select( const char *pszUserId, CspUser &clsXmlUser ) {
    CSP_USER_MAP::iterator itMap;
    bool bRes = false;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszUserId );
    if ( itMap != m_clsMap.end() ) {
        clsXmlUser = itMap->second;
        bRes = true;
    }
    m_clsMutex.release();

    if (!bRes) {
         bRes = _loadUserFromFile(pszUserId, clsXmlUser);
         // Do NOT Insert here? 
         // If we don't insert, Auth loads every time until registered.
         // If we insert, we have un-registered user in map.
         // Usually ok to have them in map but with old/zero register time?
         // If RegisterTime is 0, isAlive returns false. 
         // So it is SAFE to Insert here.
         if (bRes) Insert(clsXmlUser);
    }
    return bRes;
}

bool CspUserMap::registerUser(std::string strUserId, std::string strPassWord ) {
    CSP_USER_MAP::iterator itMap;
    bool bRes = false;
    CspUser user;

    m_clsMutex.acquire();
    itMap = m_clsMap.find( strUserId );
    if ( itMap != m_clsMap.end() ) {
        user = itMap->second;
        bRes = true; // Found in map
    }
    m_clsMutex.release();

    if (bRes) {
        // #2 Found in Map
        if (user.m_strPassWord == strPassWord) { // Simple auth check if pass provided
            user.m_iRegisterTime = time(NULL);
            _update(user);
            return true;
        }
        // If password mismatch, return false? Or ignore pass if empty?
        // Method signature has password.
        if (!strPassWord.empty() && user.m_strPassWord != strPassWord) return false;
        
        user.m_iRegisterTime = time(NULL);
        _update(user);
        return true;
    } else {
        // #3 Not in Map -> Load
        if (_loadUserFromFile(strUserId, user)) {
            // Check password?
            if (!strPassWord.empty() && user.m_strPassWord != strPassWord) return false;
            
            user.m_iRegisterTime = time(NULL);
            Insert(user);
            return true;
        }
    }
    return false;
}

bool CspUserMap::unregisterUser(std::string strUserId ) {
    CSP_USER_MAP::iterator itMap;
    bool bRes = false;
    CspUser user;
    m_clsMutex.acquire();
    itMap = m_clsMap.find( strUserId );
    if ( itMap != m_clsMap.end() ) {
        user = itMap->second;
    }
    m_clsMutex.release();

    
    if(bRes) {
        if(user.m_strId != strUserId) {
            // invalid user id 
            // 정상적인 상황에서 루틴이 실행 되면 안됨
            CLog::Print(LOG_ERROR, "[CspUserMap] unregisterUser: Invalid user id(%s/%s)", 
                user.m_strId.c_str(), strUserId.c_str());

            // 비정상적인 데이터 캐쉬 및 파일에서 제거
            _remove(strUserId);
            return false;
        }
        // 캐쉬 또는 파일로 있으면 LogoutTime을 업데이트하여 캐쉬 및 파일을 업데이트한다.
        user.m_iLogoutTime = time(NULL);
        _update(user);
        return true;        
    }
    return false; 
}

bool CspUserMap::_update(CspUser &clsUser) {
    m_clsMutex.acquire();
    m_clsMap[clsUser.m_strId] = clsUser;
    m_clsMutex.release();
    return true;
}

bool CspUserMap::_remove(std::string strUserId) {
    m_clsMutex.acquire();
    m_clsMap.erase(strUserId);
    m_clsMutex.release();
    return true;
}

static bool ScanUserFiles( const char *pszDirName, std::list<std::string> &clsUserList ) {
    FILE_LIST clsFileList;
    if ( CDirectory::List( pszDirName, clsFileList ) == false ) return false;

    for ( auto const &strName : clsFileList ) {
        if ( strName == "." || strName == ".." ) continue;

        std::string strPath = pszDirName;
        CDirectory::AppendName( strPath, strName.c_str() );

        // Removed recursive directory scanning and zero-trimming.
        // Assuming a flat hierarchy where all .json files are directly under User directory.
        if ( !CDirectory::IsDirectory( strPath.c_str() ) ) {
            if ( strName.length() > 5 && strName.substr( strName.length() - 5 ) == ".json" ) {
                std::string strId = strName.substr( 0, strName.length() - 5 );
                clsUserList.push_back( strId );
            }
        }
    }
    return true;
}

bool CspUserMap::Load( const char *pszDirName ) {
    std::list<std::string> clsUserList;
    if ( ScanUserFiles( pszDirName, clsUserList ) == false ) return false;

    for ( auto const &strId : clsUserList ) {
        CspUser clsUser;
        if ( Select( strId.c_str(), clsUser ) ) {
            CLog::Print( LOG_INFO, "UserMap Loaded User(%s) Org(%s)", clsUser.m_strId.c_str(),
                         clsUser.m_strOrganizationId.c_str() );
        }
    }
    return true;
}
