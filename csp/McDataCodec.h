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

    /** mcdata-info <mcdata-request-uri> (그룹 URI) */
    std::string m_strGroupUri;

    // ── FD SIGNALLING (msg type 0x02) 전용 ──
    /** Payload IE(FILEURL) 의 다운로드 URL */
    std::string m_strFileUrl;
    /** Metadata IE(file-selector, RFC 5547) 의 name/size/type */
    std::string m_strFileName;
    long long m_llFileSize = 0;
    std::string m_strFileType;
};

/** Content-Type 이 multipart/mixed 인지 (MCData SDS 판별 1차 조건) */
bool McDataIsMultipartMixed( const std::string &strContentType );

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

#endif
