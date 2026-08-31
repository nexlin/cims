/*
 * McDataMediaService — MCData media plane(MSRP) 시그널링 서비스 (TS 24.282 §9.2.3).
 *
 * 대용량 SDS 의 media plane 경로를 처리한다:
 *  - 발신 단말의 INVITE(SDP m=message TCP/MSRP) 를 게이트 후 cmdp 수신 세션으로 앵커링,
 *    200 OK(a=path=cmdp, a=setup:passive, a=recvonly) 응답. cmdp 가 MSRP 를 종단·저장.
 *  - cmdp MSG_RECEIVED 이벤트 수신 시 하이브리드 fan-out:
 *      · MSRP 광고 단말(CUserInfo::m_bMcDataMsrp) → 서버발 INVITE + cmdp 송신 세션
 *      · 그 외(현재 앱) → FD FILEURL C-plane MESSAGE (CSC HTTP 다운로드 폴백)
 *  - 보관(events/messages.jsonl)은 C-plane 경로와 동일 (McDataGates 공용).
 *
 * SDP 프로파일 (자체 편차 — pjsua2 단말 제약, docs mcdata_messaging.md §편차):
 *  - m=message 와 함께 더미 m=audio 라인을 유지하고 포트≠0 + a=inactive 로 응답/오퍼한다
 *    (포트 0 이면 pjsua 가 콜을 종료). CMP 할당 없음 — RTP 무흐름.
 */

#ifndef _MCDATA_MEDIA_SERVICE_H_
#define _MCDATA_MEDIA_SERVICE_H_

#include <map>
#include <mutex>
#include <string>

#include "SimpleJson.h"
#include "SipUserAgent.h"

class CMcDataMediaService {
public:
    static CMcDataMediaService &GetInstance();

    /** 활성 조건: Roles.MCDATA + McDataMedia.Enable */
    bool IsEnabled() const;

    /** CmdpClient 기동 + 이벤트 콜백 등록 (CspServer 초기화에서 호출) */
    void Init();

    /** INVITE 의 SDP 미디어 리스트에 m=message …MSRP… 가 있는지 */
    bool IsMsrpInvite( CSipCallRtp *pclsRtp );

    /** 발신 단말의 MSRP INVITE 처리 — 게이트→cmdp 세션→AcceptCall. 거부/실패 시 내부에서 StopCall */
    void OnIncomingMsrpInvite( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp,
                               CSipMessage *pclsMessage );

    /** EventCallStart 훅 — 추적 중인 콜이면 true (배포 레그면 answer a=path 를 cmdp 에 전달) */
    bool OnCallStarted( const char *pszCallId, CSipCallRtp *pclsRtp );

    /** EventCallEnd 훅 — 추적 중인 콜이면 cmdp 세션 정리 후 true */
    bool OnCallTerminated( const char *pszCallId );

    /** cmdp 비동기 이벤트 (CmdpClient RecvLoop 스레드) */
    void OnCmdpEvent( const SimpleJson::JsonNode &clsEvent );

private:
    CMcDataMediaService() {
    }

    enum EDir { DIR_RECV, DIR_SEND };

    struct McDataMediaCall {
        std::string strSessionId;  // cmdp 세션
        std::string strFrom;       // 발신자 (RECV) / 그룹 (SEND)
        std::string strGroup;
        std::string strCallee;  // SEND: 수신자
        EDir eDir;
    };

    std::mutex m_mutex;
    std::map<std::string, McDataMediaCall> m_mapCalls;      // callId → 콜 상태
    std::map<std::string, std::string> m_mapSessionToCall;  // cmdp 세션 → callId

    void HandleMsgReceived( const SimpleJson::JsonNode &clsPayload );
    void HandleSessionClosed( const std::string &strSessionId, bool bOk, const char *pszReason );

    /** 하이브리드 fan-out. @return 배포 성공 수 */
    int FanOutMediaSds( const std::string &strGroup, const std::string &strFrom,
                        const SimpleJson::JsonNode &clsPayload );

    /** 배포 레그(서버발 INVITE) 1건 생성 */
    bool InviteMsrpReceiver( const std::string &strGroup, const std::string &strFrom, const std::string &strMember,
                             const std::string &strFileId, const std::string &strContentType );

    /** 오퍼/응답 미디어 리스트에서 m=message 의 a=path / 오디오 미디어 추출 */
    static bool ExtractMsrpOffer( CSipCallRtp *pclsRtp, std::string &strRemotePath, const CSdpMedia **ppclsAudio );

    /** msrp://ip:port/... 에서 ip/port 추출 */
    static bool ParseMsrpPathHost( const std::string &strPath, std::string &strIp, int &iPort );
};

#define gclsMcDataMediaService CMcDataMediaService::GetInstance()

#endif
