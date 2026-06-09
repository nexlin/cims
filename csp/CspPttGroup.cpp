/*
 * Group Json Parser Source
 */

#include "CspPttGroup.h"

#include <fstream>
#include <sstream>

#include "Log.h"
#include "SimpleJson.h"

CspPttGroup::CspPttGroup()
    : _dbId( 0 ),
      _videoEnabled( false ),
      _priority( 5 ),
      _encryption( false ),
      _emergencyCall( false ),
      _imminentPerilCall( true ),
      _emergencyAlert( true ),
      _sessionStart( 0 ),
      _sessionEnd( 0 ),
      _sessionSeq( 0 ),
      _groupType( "prearranged" ),
      _onNetwork( true ),
      _maxMembers( 0 ),
      _requireAffiliation( true ),
      _authorizedUserId( 0 ) {
}

CspPttGroup::~CspPttGroup() {
    Clear();
}

CspPttUser::~CspPttUser() {
}

/**
 * @brief Load Group JSON file
 * @param pszFileName JSON File Path
 * @return true if success, false otherwise
 */
bool CspPttGroup::load( std::string groupId ) {
    // groupId is likely the Full Path in calling context (ReadDir passes strFileName)
    // The user decided to rename the parameter to groupId in header, but ReadDir passes a path.
    // Let's assume it's the file path.

    std::ifstream t( groupId );
    if ( !t.is_open() ) return false;

    std::stringstream buffer;
    buffer << t.rdbuf();

    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( buffer.str() );
    if ( root.type != SimpleJson::JSON_OBJECT ) return false;

    Clear();

    // Extract ID from filename if not in JSON?
    // JSON has optional "id" field?
    // User sample:
    // { "name": "Sales Team", "users": [ ... ] }
    // It does NOT have "id". ID is usually implied by filename (e.g. 2000.json).
    // Let's extract ID from filename.

    std::string baseName = groupId;
    size_t lastSlash = baseName.find_last_of( "/\\" );
    if ( lastSlash != std::string::npos ) baseName = baseName.substr( lastSlash + 1 );
    size_t lastDot = baseName.find_last_of( "." );
    if ( lastDot != std::string::npos ) baseName = baseName.substr( 0, lastDot );

    _id = baseName;

    if ( root.Has( "name" ) ) _name = root.GetString( "name" );

    if ( root.Has( "video_enabled" ) ) _videoEnabled = ( root.GetInt( "video_enabled" ) != 0 );

    // 3GPP MCPTT 그룹 속성 (JSON fallback)
    if ( root.Has( "group_type" ) ) _groupType = root.GetString( "group_type" );
    if ( root.Has( "on_network" ) ) _onNetwork = ( root.GetInt( "on_network" ) != 0 );
    if ( root.Has( "max_members" ) ) _maxMembers = root.GetInt( "max_members" );
    if ( root.Has( "require_affiliation" ) ) _requireAffiliation = ( root.GetInt( "require_affiliation" ) != 0 );
    if ( root.Has( "alias" ) ) _alias = root.GetString( "alias" );

    if ( root.Has( "users" ) ) {
        SimpleJson::JsonNode users = root.Get( "users" );
        if ( users.type == SimpleJson::JSON_ARRAY ) {
            for ( size_t i = 0; i < users.Size(); ++i ) {
                SimpleJson::JsonNode userNode = users.At( i );
                std::string uid = userNode.GetString( "id" );
                int prio = userNode.GetInt( "priority" );
                std::string role = userNode.Has( "role" ) ? userNode.GetString( "role" ) : "participant";
                std::string mcpttId = userNode.Has( "mcptt_id" ) ? userNode.GetString( "mcptt_id" ) : "";

                if ( !uid.empty() ) {
                    auto pUser = std::make_shared<CspPttUser>( uid, prio, role, mcpttId );
                    pUser->_groups.push_back( _id );  // Add self group
                    _pusers.push_back( pUser );
                }
            }
        }
    }

    CLog::Print( LOG_INFO, "CspPttGroup::load(%s) Found %d users", _id.c_str(), (int)_pusers.size() );
    return true;
}

void CspPttGroup::Clear() {
    _id.clear();
    _dbId = 0;
    _name.clear();
    _videoEnabled = false;
    _priority = 5;
    _encryption = false;
    _emergencyCall = false;
    _imminentPerilCall = true;
    _emergencyAlert = true;
    _orgCode.clear();
    _sessionStart = 0;
    _sessionEnd = 0;
    _sessionSeq = 0;
    _groupType = "prearranged";
    _onNetwork = true;
    _maxMembers = 0;
    _requireAffiliation = true;
    _alias.clear();
    _authorizedUserId = 0;
    _authorizedUser.clear();
    _createdAt.clear();
    _pusers.clear();
}
