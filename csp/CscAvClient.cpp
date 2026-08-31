#include "CscAvClient.h"

#include <stdio.h>

#include "CscEndpointCache.h"
#include "HttpClient.h"
#include "Log.h"
#include "SimpleJson.h"
#include "SipServerSetup.h"

CCscAvClient gclsCscAvClient;

std::string CCscAvClient::Url() {
    return CscEndpoint::AdminBaseUrl() + "/internal/aka/av";
}

static bool IsHexLen( const std::string &s, size_t iBytes ) {
    if ( s.size() != iBytes * 2 ) return false;
    for ( size_t i = 0; i < s.size(); ++i ) {
        const char c = s[i];
        if ( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ) ) ) return false;
    }
    return true;
}

ECscAvResult CCscAvClient::Request( const std::string &strMsisdn, const std::string &strService,
                                    const std::string &strRandHex, const std::string &strAutsHex, CscAv &clsOut ) {
    if ( gclsSetup.m_strCscInternalToken.empty() ) {
        CLog::Print( LOG_ERROR, "[auc] Setup.Csc.InternalToken 미설정 — AV 요청 불가 (msisdn=%s)", strMsisdn.c_str() );
        return E_CSC_AV_UNAVAILABLE;
    }

    std::string strBody = "{\"msisdn\":\"" + strMsisdn + "\",\"service\":\"" + strService + "\"";
    if ( !strAutsHex.empty() ) strBody += ",\"rand\":\"" + strRandHex + "\",\"auts\":\"" + strAutsHex + "\"";
    strBody += "}";

    HTTP_HEADER_LIST clsHeaders;
    clsHeaders.push_back( CHttpHeader( "Authorization", ( "Bearer " + gclsSetup.m_strCscInternalToken ).c_str() ) );

    CHttpClient clsClient;
    int iSec = ( gclsSetup.m_iCscTimeoutMs + 999 ) / 1000;
    clsClient.SetRecvTimeout( iSec < 1 ? 1 : iSec );

    const std::string strUrl = Url();
    std::string strOutType, strOut;
    if ( clsClient.DoPost( strUrl.c_str(), &clsHeaders, "application/json", strBody.c_str(), strOutType, strOut ) ==
         false ) {
        CLog::Print( LOG_ERROR, "[auc] AV request failed url=%s msisdn=%s (connect/timeout)", strUrl.c_str(),
                     strMsisdn.c_str() );
        return E_CSC_AV_UNAVAILABLE;
    }

    const int iStatus = clsClient.GetStatusCode();
    if ( iStatus != 200 ) {
        std::string strErr;
        SimpleJson::JsonNode err = SimpleJson::JsonNode::Parse( strOut );
        if ( err.Has( "error" ) ) strErr = err.GetString( "error" );
        CLog::Print( iStatus >= 500 ? LOG_ERROR : LOG_INFO, "[auc] AV response %d msisdn=%s error=%s", iStatus,
                     strMsisdn.c_str(), strErr.c_str() );
        if ( iStatus == 404 ) return E_CSC_AV_UNKNOWN_SUB;
        if ( iStatus == 409 ) return E_CSC_AV_SCHEME_MISMATCH;
        if ( iStatus == 422 ) return E_CSC_AV_AUTS_INVALID;
        return E_CSC_AV_UNAVAILABLE;
    }

    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( strOut );
    if ( !root.Has( "av" ) ) {
        CLog::Print( LOG_ERROR, "[auc] AV response without av object (msisdn=%s)", strMsisdn.c_str() );
        return E_CSC_AV_UNAVAILABLE;
    }
    SimpleJson::JsonNode av = root.Get( "av" );
    clsOut.strRandHex = av.GetString( "rand" );
    clsOut.strAutnHex = av.GetString( "autn" );
    clsOut.strXresHex = av.GetString( "xres" );
    clsOut.strCkHex = av.GetString( "ck" );
    clsOut.strIkHex = av.GetString( "ik" );
    clsOut.bResynced = root.GetString( "resynced" ) == "true";
    if ( !IsHexLen( clsOut.strRandHex, 16 ) || !IsHexLen( clsOut.strAutnHex, 16 ) || clsOut.strXresHex.size() < 8 ||
         !IsHexLen( clsOut.strXresHex, clsOut.strXresHex.size() / 2 ) ) {
        CLog::Print( LOG_ERROR, "[auc] AV response malformed (msisdn=%s)", strMsisdn.c_str() );
        return E_CSC_AV_UNAVAILABLE;
    }
    return E_CSC_AV_OK;
}
