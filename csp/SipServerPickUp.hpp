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

/*
 SIP Pick UP �ó�����

        INVITE                       INVITE
 (#1) ----------------> CspServer --------------> (#2)

        INVITE to **
 (#3) ----------------> CspServer

                                     CANCEL
                                                CspServer --------------> (#2)

        200 OK
 (#3) <---------------- CspServer
        200 OK
 (#1) <---------------- CspServer

 */

/**
 * @ingroup CspServer
 * @brief SIP Call Pick up ��ȭ ��û ���� �̺�Ʈ �ڵ鷯
 * @param	pszCallId	SIP Call-ID
 * @param pszFrom		SIP From ����� ���̵�
 * @param pszTo			SIP To ����� ���̵�
 * @param pclsRtp		RTP ���� ���� ��ü
 */
void CSipServer::PickUp( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp ) {
    CXmlUser xmlFrom;
    USER_ID_LIST clsUserIdList;
    bool bCallPickup = false;

    CLog::Print( LOG_DEBUG, "EventIncomingCall(%s,%s,%s)  CallPickup", pszCallId, pszFrom, pszTo );

    if ( SelectUser( pszFrom, xmlFrom ) && gclsUserMap.SelectGroup( xmlFrom.m_strGroupId.c_str(), clsUserIdList ) ) {
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
