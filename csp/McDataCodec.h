/*
 * MCData SDS message codec (TS 24.282 §15)
 *
 * SIP MESSAGE 본문(multipart/mixed: mcdata-info+xml / mcdata-signalling / mcdata-payload)을
 * 파싱해 게이트·로깅에 필요한 필드만 추출한다. 바이너리 TLV 파트는 단말이
 * Content-Transfer-Encoding: base64 로 실어 보낸다 (PJSIP Java 바인딩의 String 본문 제약 —
 * docs/design/features/mcdata_messaging.md §편차 참조).
 */

#ifndef _MCDATA_CODEC_H_
#define _MCDATA_CODEC_H_

#include <string>
#include <vector>

// TS 24.282 §15.2.2 message types
#define MCDATA_MSG_SDS_SIGNALLING 0x01
#define MCDATA_MSG_FD_SIGNALLING 0x02
#define MCDATA_MSG_DATA_PAYLOAD 0x03
#define MCDATA_MSG_SDS_NOTIFICATION 0x05

// TS 24.282 §15.2.3 SDS disposition request type
#define MCDATA_DISP_REQ_DELIVERY 0x01
#define MCDATA_DISP_REQ_READ 0x02
#define MCDATA_DISP_REQ_DELIVERY_READ 0x03

// TS 24.282 §15.2.5 SDS disposition notification type
#define MCDATA_NOTIF_UNDELIVERED 0x01
#define MCDATA_NOTIF_DELIVERED 0x02
#define MCDATA_NOTIF_READ 0x03
#define MCDATA_NOTIF_DELIVERED_READ 0x04

/**
 * @brief MESSAGE 본문에서 추출한 MCData SDS 정보 (게이트·flow 로깅용)
 */
class CMcDataSdsInfo {
public:
    CMcDataSdsInfo()
        : m_iMsgType( 0 ), m_tSentTime( 0 ), m_iDispositionReq( 0 ), m_iNotifType( 0 ), m_iPayloadSize( 0 ) {
    }

    /** mcdata-signalling 파트의 message type (MCDATA_MSG_*) */
    int m_iMsgType;

    /** Conversation ID / Message ID — UUID 16 octets 의 hex 32자 표기 */
    std::string m_strConvId;
    std::string m_strMsgId;

    /** Date and time IE (UTC seconds) */
    time_t m_tSentTime;

    /** SDS SIGNALLING PAYLOAD 의 disposition 요청 (0=없음) */
    int m_iDispositionReq;

    /** SDS NOTIFICATION 의 통지 유형 (MCDATA_NOTIF_*) */
    int m_iNotifType;

    /** DATA PAYLOAD 의 payload 순수 크기 합 (max-data-size-for-SDS 게이트 기준) */
    int m_iPayloadSize;

    /** 첫 TEXT payload (UTF-8) — flow 이벤트 로깅용 */
    std::string m_strText;

    /** mcdata-info <request-type> — 요청 종류(TS 24.282 Annex D.2: group-sds·one-to-one-sds·group-fd·one-to-one-fd …).
     *  참여·제어 기능이 이것으로 절차를 가른다(§9.2.2.3.1 4) · §9.2.2.4.2 5)·6)) */
    std::string m_strRequestType;

    /** mcdata-info <mcdata-request-uri> (그룹 요청이면 MCData group ID) */
    std::string m_strGroupUri;

    /** mcdata-info <mcdata-calling-user-id>·<mcdata-calling-group-id> — disposition 통지의 그룹 문맥
     *  (TS 24.282 §12.2.1.1 5)·§12.2.3 15)) */
    std::string m_strCallingUserId;
    std::string m_strCallingGroupId;

    /** application/vnd.3gpp.mcdata-info+xml · application/vnd.3gpp.mcdata-payload 파트 유무 — 제어 기능의 본문 검사
     *  (McDataMissingBodies) */
    bool m_bHasInfo = false;
    bool m_bHasPayload = false;

    /** application/resource-lists+xml 파트 유무와 <entry uri> 목록 — disposition 통지 대상 MCData ID(§12.2.1.1 3)) */
    bool m_bHasResourceLists = false;
    std::vector<std::string> m_vecListUris;

    /** mcdata-signalling 파트 원문(파트 헤더 포함, 전송 인코딩 그대로) — 통지 중계가 그대로 옮긴다(§12.2.3 15)d)·16))
     */
    std::string m_strSignallingPart;

    // ── FD SIGNALLING (msg type 0x02) 전용 ──
    /** Payload IE 수와 FILEURL 이 아닌 내용 형식이 있었는가 — 제어 기능 검사(TS 24.282 §10.2.4.4.2 6)·7)a)) */
    int m_iFdPayloadCount = 0;
    bool m_bFdNonFileUrlPayload = false;
    /** Payload IE(FILEURL) 의 다운로드 URL */
    std::string m_strFileUrl;
    /** Metadata IE(file-selector, RFC 5547) 의 name/size/type */
    std::string m_strFileName;
    long long m_llFileSize = 0;
    std::string m_strFileType;
};

/** mcdata-info+xml 본문 하나를 읽는다 — <request-type>·<mcdata-request-uri>·<mcdata-calling-user-id>·
 *  <mcdata-calling-group-id>. 미디어 평면 INVITE(SDP 와 같이 오는 mcdata-info — TS 24.282 §9.2.3.2.3)도 같은 함수로 */
void McDataParseInfo( const std::string &strXml, CMcDataSdsInfo &clsInfo );

/**
 * @brief MCData 요청의 대상 — Request-URI 는 참여 기능 PSI 이고(TS 24.282 §6.2.4.1 4)) 대상은 본문이다.
 *        request-type 이 절차를 가른다(§9.2.2.3.1 4) · §9.2.2.4.2 5)·6) · §10.2.4.4.2 10)·12) · §9.2.3.3.3 4)):
 *        group-sds·group-fd = <mcdata-request-uri> 의 group ID, one-to-one-sds·one-to-one-fd = resource-lists 의
 *        entry 하나(없거나 둘 이상이면 403 + 204·205). 그 밖(request-type 없음·ad hoc 그룹 등 이 서버가 맡지 않는
 *        요청)은 제어 기능을 정하지 못한다 — 404 + 142(§9.2.2.3.1 5)).
 * @param bGroup [out] 그룹 요청이면 true
 * @param strTargetId [out] 대상 식별자(맨 값 — group ID 또는 MCData ID)
 * @param piWarn [out] 거절이면 Warning 번호
 * @return 0 = 대상을 정함, 아니면 보낼 SIP 상태
 */
int McDataRequestTarget( const CMcDataSdsInfo &clsInfo, bool &bGroup, std::string &strTargetId, int *piWarn );

/** Content-Type 이 multipart/mixed 인지 (MCData SDS 판별 1차 조건) */
bool McDataIsMultipartMixed( const std::string &strContentType );

/** FD 의 FILEURL 이 이 서버의 media storage function(CSC 콘텐츠 서버) 파일을 가리키는가 — TS 24.282 §10.2.4.4.2 7)b).
 *  strBase = 콘텐츠 서버 base URL(scheme://host[:port], 후행 '/' 없음 — 단말에 내주는 MCData FD URL 의 base). 같은
 * scheme· host(대소문자 무시)·port(생략 = scheme 기본)이고 경로가 정확히 /mcdata/fd/<32 hex>(질의·조각 없음)여야 한다.
 * 다른 호스트를 가리키는 URL 은 우리 저장소에 없는 파일이다 — 규격 단말이 그 URL 로 Bearer 토큰을 실어 GET 하므로
 * 배포하지 않는다. */
bool McDataFdUrlIsOurs( const std::string &strUrl, const std::string &strBase );

/** 그룹 상시 대화 Conversation ID — UUID v3(MD5) 결정적 발급 (앱 conversationIdOf 와 동일 규칙) */
std::string McDataConversationIdOf( const std::string &strGroupId );

/** Message ID — UUID v4 hex 32자 발급 */
std::string McDataNewMessageId();

/**
 * @brief FD SIGNALLING PAYLOAD(0x02) multipart/mixed 본문 생성 — FILEURL 폴백 배포용.
 *        앱 McDataCodec.kt buildGroupFd 와 바이트 호환 (base64 CTE, mcdata-info + signalling 2파트).
 * @param strContentTypeOut [out] boundary 포함 Content-Type
 * @return SIP MESSAGE 본문
 */
std::string McDataBuildFdSignallingBody( std::string &strContentTypeOut, const std::string &strGroupUri,
                                         const std::string &strFileUrl, const std::string &strFileName,
                                         long long llFileSize, const std::string &strFileType,
                                         const std::string &strConvId, const std::string &strMsgId );

/**
 * @brief MCData SDS multipart 본문 파싱.
 * @param strContentType Content-Type 헤더 원문 (boundary 파라미터 포함; 없으면 본문 첫 줄에서 유도)
 * @param strBody SIP MESSAGE 본문
 * @param clsInfo [out] 추출 결과
 * @return mcdata-signalling 파트를 찾아 파싱했으면 true
 */
bool McDataParseBody( const std::string &strContentType, const std::string &strBody, CMcDataSdsInfo &clsInfo );

/**
 * SDS·FD MESSAGE 에 있어야 할 MIME 본문이 빠졌는가 — 빠졌으면 제어 기능이 403 + 199 "expected MIME bodies not in the
 * request" 로 거절한다. SDS = mcdata-info·mcdata-signalling·mcdata-payload(TS 24.282 §9.2.2.4.2 2)), FD = mcdata-info·
 * mcdata-signalling(§10.2.4.4.2 3)). bParsed = McDataParseBody 의 결과(signalling 파트를 읽었는가). signalling 이
 * 없으면 어느 쪽이든 빠진 것이다.
 */
bool McDataMissingBodies( bool bParsed, const CMcDataSdsInfo &clsInfo );

#endif
