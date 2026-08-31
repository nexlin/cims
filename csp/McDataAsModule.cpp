#include "McDataAsModule.h"

#include <stdio.h>
#include <string.h>

#include "CallDir.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "McDataCodec.h"
#include "McDataGates.h"
#include "ModuleDispatcher.h"
#include "SipServerSetup.h"
#include "SipStatusCode.h"
#include "UserMap.h"

bool CMcDataAsModule::IsEnabled() const {
    return gclsSetup.m_bRoleMcData;
}

/**
 * 그룹 SDS 처리 (TS 24.282 group standard SDS, controlling function).
 * 처리했으면(성공·거부 모두) true — 거부 응답(403/413)은 여기서 직접 송신하며, 이후
 * RecvMessageRequest 의 자동 200 은 SIP 트랜잭션 상 후행 최종응답이라 클라이언트가 무시한다
 * (긴급경보 경로와 동일한 기존 계약).
 */
bool CMcDataAsModule::OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
    if ( pclsMessage == NULL ) return false;
    if ( gclsGroupMap.Contains( pszTo ) == false ) return false;  // 1:1 → 디스패처 기본 경로

    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( pszTo, clsGroup ) == false ) return false;

    // Content-Type 원문 (boundary 포함) — fan-out 시 그대로 보존
    char szContentType[512];
    szContentType[0] = '\0';
    pclsMessage->m_clsContentType.ToString( szContentType, sizeof( szContentType ) );

    // MCData multipart 면 signalling TLV 파싱 (conv/msg id·disposition·payload 크기),
    // 평문(text/plain 등)이면 본문 전체를 payload 로 간주.
    CMcDataSdsInfo clsInfo;
    bool bMcData =
        McDataIsMultipartMixed( szContentType ) && McDataParseBody( szContentType, pclsMessage->m_strBody, clsInfo );
    bool bFd = bMcData && clsInfo.m_iMsgType == MCDATA_MSG_FD_SIGNALLING;
    int iPayloadSize = bMcData ? clsInfo.m_iPayloadSize : (int)pclsMessage->m_strBody.size();

    // 게이트 0 — max-payload-size-sds-cplane-bytes (TS 24.484 서비스 설정, 0/미설정=무제한).
    //   초과 SDS 는 media plane(MSRP) 을 써야 한다 — participating 검사 (TS 24.282 §9.2.2 step 8).
    if ( !bFd && gclsSetup.m_iMaxSdsCplaneBytes > 0 && iPayloadSize > gclsSetup.m_iMaxSdsCplaneBytes ) {
        CLog::Print( LOG_INFO, "McDataAs: payload %d > cplane max %d — reject 403 Warning 203 from(%s)", iPayloadSize,
                     gclsSetup.m_iMaxSdsCplaneBytes, pszFrom );
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
        if ( pclsResponse ) {
            pclsResponse->AddHeader( "Warning",
                                     "203 CIMS \"message too large to send over signalling control plane\"" );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
        }
        return true;
    }

    // 게이트 1·2 — allow_sds/allow_fd + 발신자 멤버십 (media plane 과 공용, McDataGates)
    int iGate = McDataGateCheck( clsGroup, pszFrom, bFd );
    if ( iGate != 0 ) {
        gclsDispatcher.SendResponse( pclsMessage, iGate );
        return true;
    }

    // 게이트 3 — mcdata-on-network-max-data-size-for-SDS (TS 24.481). FD 는 payload=URL 이라 제외
    //   (파일 크기 상한은 CSC 업로드 단에서 강제).
    if ( !bFd && clsGroup._maxSdsSize > 0 && iPayloadSize > clsGroup._maxSdsSize ) {
        CLog::Print( LOG_INFO, "McDataAs: group(%s) payload %d > max %d — reject 413", pszTo, iPayloadSize,
                     clsGroup._maxSdsSize );
        gclsDispatcher.SendResponse( pclsMessage, SIP_REQUEST_ENTITY_TOO_LARGE );
        return true;
    }

    // fan-out — 발신자 제외. affiliation 요구 그룹은 affiliate 멤버만 (긴급경보 경로와 동일 규칙).
    std::vector<std::string> vecTargets;
    McDataDeliveryTargets( clsGroup, pszFrom, pszTo, vecTargets );
    int iFanout = 0;
    for ( const auto &strMember : vecTargets ) {
        CUserInfo clsMemInfo;
        if ( gclsUserMap.Select( strMember.c_str(), clsMemInfo ) ) {
            CSipCallRoute clsMemRoute;
            clsMemInfo.GetCallRoute( clsMemRoute );
            if ( gclsUserAgent.SendSms( pszFrom, strMember.c_str(), pclsMessage->m_strBody.c_str(), &clsMemRoute,
                                        szContentType[0] ? szContentType : NULL ) )
                iFanout++;
        }
    }

    {
        const char *pszType = bFd ? "fd" : ( bMcData ? "sds" : "text" );
        CMcDataSdsInfo clsArcInfo = clsInfo;
        if ( !bMcData ) clsArcInfo.m_strText = pclsMessage->m_strBody;
        McDataArchiveMessage( pszTo, pszFrom, pszType, clsArcInfo, iPayloadSize, iFanout, "", "", bMcData );
    }

    CLog::Print( LOG_INFO, "McDataAs: group SDS from(%s) to(%s) mcdata=%d size=%d fanout=%d conv(%s) msg(%s)", pszFrom,
                 pszTo, bMcData, iPayloadSize, iFanout, clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str() );
    return true;  // RecvMessageRequest 가 200 OK 송신
}
