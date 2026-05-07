void CSipServer::PickUp( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp ) {
    CspUser xmlFrom;
    USER_ID_LIST clsUserIdList;
    bool bCallPickup = false;

    CLog::Print( LOG_DEBUG, "EventIncomingCall(%s,%s,%s)  CallPickup", pszCallId, pszFrom, pszTo );

    if ( gclsCspUserMap.Select( pszFrom, xmlFrom ) &&
         gclsUserMap.SelectGroup( xmlFrom.m_strOrganizationId.c_str(), clsUserIdList ) ) {
        USER_ID_LIST::iterator itUIL;
        std::string strOldCallId;

        for ( itUIL = clsUserIdList.begin(); itUIL != clsUserIdList.end(); ++itUIL ) {
            if ( gclsCallMap.SelectToRing( itUIL->c_str(), strOldCallId ) == false ) continue;

            CCallInfo clsOldCallInfo;

            if ( gclsCallMap.Select( strOldCallId.c_str(), clsOldCallInfo ) &&
                 gclsCallMap.Insert( pszCallId, clsOldCallInfo ) ) {
                gclsCallMap.DeleteOne( strOldCallId.c_str() );
                gclsUserAgent.StopCall( strOldCallId.c_str() );

                CSipCallRtp clsRemoteRtp;

                if ( gclsUserAgent.GetRemoteCallRtp( clsOldCallInfo.m_strPeerCallId.c_str(), &clsRemoteRtp ) ) {
                    if ( pclsRtp ) {
                        if ( clsOldCallInfo.m_iPeerRtpPort > 0 ) {
                            pclsRtp->m_iPort = clsOldCallInfo.m_iPeerRtpPort;
                            pclsRtp->m_strIp = gclsSetup.m_strLocalIp;
                        }

                        pclsRtp->m_iCodec = clsRemoteRtp.m_iCodec;
                    }

                    // #1 ��ȭ ����
                    if ( gclsUserAgent.AcceptCall( clsOldCallInfo.m_strPeerCallId.c_str(), pclsRtp ) ) {
                        CCallInfo clsPeerCallInfo;

                        if ( gclsCallMap.Select( clsOldCallInfo.m_strPeerCallId.c_str(), clsPeerCallInfo ) ) {
                            gclsCallMap.Update( clsOldCallInfo.m_strPeerCallId.c_str(), pszCallId );

                            if ( pclsRtp ) {
                                if ( clsOldCallInfo.m_iPeerRtpPort > 0 ) {
                                    pclsRtp->m_iPort = clsPeerCallInfo.m_iPeerRtpPort;
                                } else {
                                    pclsRtp = &clsRemoteRtp;
                                }
                            }

                            // #3 ��ȭ ����
                            gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
                            bCallPickup = true;
                        }

                        if ( bCallPickup == false ) {
                            gclsUserAgent.StopCall( clsOldCallInfo.m_strPeerCallId.c_str() );
                        }
                    }
                }
            }

            break;
        }
    }

    if ( bCallPickup == false ) {
        CLog::Print( LOG_DEBUG, "EventIncomingCall CallPickup from(%s) is not found", pszFrom );
        StopCall( pszCallId, SIP_NOT_FOUND );
    }
}
