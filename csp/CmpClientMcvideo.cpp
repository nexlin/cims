// CmpClient — MCVideo 그룹 호 명령 (cmp_media_api.md §7.9, mcvideo.md §5.2.1 A11).
//   명령은 MCPTT 와 같은 PTT_* 이고 payload.service = "mcvideo" 가 hdr.service 로 간다(_SendOnEndpoint). 세션·끝점 캐시
//   키는 McvKey(group) — 같은 그룹 id 의 MCPTT 그룹 호와 동시에 서므로 group id 만으로 두지 않는다(mcvideo.md §7 D6).
#include "CmpClient.h"
#include "Log.h"
#include "SipMessageLogger.h"

namespace {

    // JOIN·ADD 응답의 부호 없는 32비트 SSRC — JSON 정수(GetInt = long long)에서 읽는다.
    unsigned int McvSsrcField( const SimpleJson::JsonNode &o, const char *key ) {
        return o.Has( key ) ? (unsigned int)( o.GetInt( key ) & 0xFFFFFFFFLL ) : 0u;
    }

}  // namespace

bool CCmpClient::McvAddGroup( const std::string &strGroupId, const CmpMcvGroupSpec &clsSpec,
                              const std::string &strSesId, std::string &strIp,
                              std::map<std::string, CmpMcvPorts> &mapMemberPorts ) {
    const std::string strKey = McvKey( strGroupId );
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_GROUP_ADD" );
    req.Set( "service", "mcvideo" );
    req.Set( "group_id", strGroupId );
    std::string strFinalSesId = strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = GetSesIdByKey( strKey );
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid[strKey] = strFinalSesId;
    }
    req.Set( "group_type", clsSpec.strGroupType );
    req.Set( "max_transmitters", clsSpec.iMaxTransmitters > 0 ? clsSpec.iMaxTransmitters : 1 );
    req.Set( "reception_mode", clsSpec.bReceptionAutomatic ? "automatic" : "manual" );
    if ( !clsSpec.strCallType.empty() ) req.Set( "call_type", clsSpec.strCallType );
    // 서버 타이머 — T1 = 그룹 hang timer, T5 = reception hang timer(TS 24.581 §11.1.3). 나머지는 CMP 의 K5 기본값.
    if ( clsSpec.iT1Ms >= 0 || clsSpec.iT5Ms >= 0 ) {
        SimpleJson::JsonNode tt;
        if ( clsSpec.iT1Ms >= 0 ) tt.Set( "t1_ms", clsSpec.iT1Ms );
        if ( clsSpec.iT5Ms >= 0 ) tt.Set( "t5_ms", clsSpec.iT5Ms );
        req.Set( "tc_timers", tt );
    }
    req.Set( "members", clsSpec.strMembers );
    if ( !clsSpec.strRecordDir.empty() ) req.Set( "record_dir", clsSpec.strRecordDir );
    if ( !clsSpec.strSessionDir.empty() ) req.Set( "session_dir", clsSpec.strSessionDir );

    std::string strResp;
    if ( !SendRequestAndWait( strKey, req, strResp ) ) {
        CLog::Print( LOG_ERROR, "CmpClient::McvAddGroup(%s): no response", strGroupId.c_str() );
        return false;
    }
    SimpleJson::JsonNode resp = SimpleJson::JsonNode::Parse( strResp );
    if ( resp.type != SimpleJson::JSON_OBJECT || resp.GetString( "status" ) != "OK" ) {
        CLog::Print( LOG_ERROR, "CmpClient::McvAddGroup(%s) rejected: %s", strGroupId.c_str(), strResp.c_str() );
        return false;
    }
    strIp = resp.GetString( "ip" );
    mapMemberPorts.clear();
    SimpleJson::JsonNode mp = resp.Get( "member_ports" );
    for ( const auto &kv : mp.objects ) {
        CmpMcvPorts p;
        p.iPort = (int)kv.second.GetInt( "port" );
        p.iVideoPort = (int)kv.second.GetInt( "video_port" );
        p.iControlPort = (int)kv.second.GetInt( "control_port" );
        if ( p.iPort > 0 ) mapMemberPorts[kv.first] = p;
    }
    return true;
}

bool CCmpClient::McvJoin( const std::string &strGroupId, const std::string &strSessionId,
                          const CmpMcvMemberDecl *pclsDecl, const std::string &strSesId, CmpMcvJoinResult &clsResult ) {
    const std::string strKey = McvKey( strGroupId );
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_JOIN" );
    req.Set( "service", "mcvideo" );
    req.Set( "group_id", strGroupId );
    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strKey ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    req.Set( "session_id", strSessionId );
    // 2단 멱등(§7.4): 주소가 없으면 선할당(①) — 포트·tc_ssrc 만.
    if ( pclsDecl && !pclsDecl->strIp.empty() && pclsDecl->iPort > 0 ) {
        const CmpMcvMemberDecl &d = *pclsDecl;
        req.Set( "user_ip", d.strIp );
        req.Set( "user_port", d.iPort );
        if ( d.iVideoPort > 0 ) req.Set( "user_video_port", d.iVideoPort );
        if ( d.iControlPort > 0 ) req.Set( "user_control_port", d.iControlPort );
        if ( d.iNat ) {
            req.Set( "user_nat", 1 );
            if ( !d.strSigIp.empty() ) req.Set( "user_sig_ip", d.strSigIp );
        }
        if ( d.iPt > 0 ) req.Set( "user_pt", d.iPt );
        if ( d.iSrcPt > 0 ) req.Set( "user_src_pt", d.iSrcPt );
        if ( d.iVideoPort > 0 && d.iVideoPt > 0 ) req.Set( "user_video_pt", d.iVideoPt );
        if ( !d.strCodec.empty() ) req.Set( "user_codec", d.strCodec );
        req.Set( "role", d.strRole.empty() ? "participant" : d.strRole );
        if ( !d.strUri.empty() ) req.Set( "user_uri", d.strUri );
        if ( d.uTcSsrc ) req.Set( "user_tc_ssrc", (long long)d.uTcSsrc );
        if ( d.uAudioSsrc ) req.Set( "user_audio_ssrc", (long long)d.uAudioSsrc );
        if ( d.uVideoSsrc ) req.Set( "user_video_ssrc", (long long)d.uVideoSsrc );
        if ( d.iQueueing >= 0 ) req.Set( "queueing", d.iQueueing );
        if ( d.iMaxPriority >= 0 ) req.Set( "max_priority", d.iMaxPriority );
        if ( d.iMaxRxStreams > 0 ) req.Set( "max_rx_streams", d.iMaxRxStreams );
        if ( d.bImplicit ) req.Set( "implicit_request", 1 );
        if ( d.bRecvOnly ) req.Set( "recv_only", 1 );
        // 협상한 영상 피드백(RFC 4585 §4.2) — CMP 는 이 멤버(송출자)에게 이것만 보낸다
        if ( d.iVideoPort > 0 && d.iVideoFb >= 0 ) {
            SimpleJson::JsonNode fb;
            fb.type = SimpleJson::JSON_ARRAY;
            if ( d.iVideoFb & 1 ) fb.Add( SimpleJson::JsonNode( "pli" ) );
            if ( d.iVideoFb & 2 ) fb.Add( SimpleJson::JsonNode( "fir" ) );
            req.Set( "user_video_fb", fb );
        }
    }

    std::string strResp;
    if ( !SendRequestAndWait( strKey, req, strResp ) ) {
        CLog::Print( LOG_ERROR, "CmpClient::McvJoin(%s/%s): no response", strGroupId.c_str(), strSessionId.c_str() );
        return false;
    }
    SimpleJson::JsonNode resp = SimpleJson::JsonNode::Parse( strResp );
    if ( resp.type != SimpleJson::JSON_OBJECT || resp.GetString( "status" ) != "OK" ) {
        CLog::Print( LOG_ERROR, "CmpClient::McvJoin(%s/%s) rejected: %s", strGroupId.c_str(), strSessionId.c_str(),
                     strResp.c_str() );
        return false;
    }
    clsResult.strIp = resp.GetString( "ip" );
    clsResult.clsPorts.iPort = (int)resp.GetInt( "port" );
    clsResult.clsPorts.iVideoPort = (int)resp.GetInt( "video_port" );
    clsResult.clsPorts.iControlPort = (int)resp.GetInt( "control_port" );
    clsResult.uTcSsrc = McvSsrcField( resp, "tc_ssrc" );
    clsResult.bGranted = resp.GetInt( "granted", 0 ) != 0;
    clsResult.uAudioSsrc = McvSsrcField( resp, "audio_ssrc" );
    clsResult.uVideoSsrc = McvSsrcField( resp, "video_ssrc" );
    return true;
}

bool CCmpClient::McvLeave( const std::string &strGroupId, const std::string &strSessionId,
                           const std::string &strSesId ) {
    const std::string strKey = McvKey( strGroupId );
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_LEAVE" );
    req.Set( "service", "mcvideo" );
    req.Set( "group_id", strGroupId );
    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strKey ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    req.Set( "session_id", strSessionId );
    std::string strResp;
    return SendRequestAndWait( strKey, req, strResp );
}

bool CCmpClient::McvRemove( const std::string &strGroupId, const std::string &strSesId ) {
    const std::string strKey = McvKey( strGroupId );
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_GROUP_REMOVE" );
    req.Set( "service", "mcvideo" );
    req.Set( "group_id", strGroupId );
    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strKey ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid.erase( strKey );
    }
    std::string strResp;
    const bool bRet = SendRequestAndWait( strKey, req, strResp );
    ReleaseEndpointForKey( strKey );
    return bRet;
}
