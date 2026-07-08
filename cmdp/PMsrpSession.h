/*
 * MSRP 세션 상태기계 — cmdp
 *
 * CSP 가 제어채널로 할당(ADD_MSRP_RECV_SESSION / ADD_MSRP_SEND_SESSION)한 세션 1건.
 * TCP 연결은 첫 요청의 To-Path 세션부로 바인딩된다 (PCmdpServer::onMsrpFrame).
 *
 *  - RECV: 발신 단말의 SEND 청크를 조립. 메시지 세트(멀티파트 1건 또는
 *    mcdata-signalling + mcdata-payload 2건) 완성 시 서버가 저장·MSG_RECEIVED 발행.
 *  - SEND: 저장된 본문을 16KB SEND 청크로 스트리밍. 마지막 청크 200 수신 시 완료.
 */

#ifndef _P_MSRP_SESSION_H_
#define _P_MSRP_SESSION_H_

#include <ctime>
#include <string>
#include "PMsrpParser.h"

class PMsrpConnection;

class PMsrpSession {
public:
    enum Mode { MODE_RECV, MODE_SEND };

    PMsrpSession(const std::string& id, Mode mode)
        : _sessionId(id), _mode(mode) {
        _createdAt = _lastActivity = time(nullptr);
    }

    // ── 식별/컨텍스트 (ADD 시 CSP 가 지정) ─────────────────────────────
    std::string _sessionId;   // 로컬 MSRP URI 의 세션부 (md_N)
    Mode _mode;
    std::string _caller;      // 발신자 (RECV) / 표시 발신자 (SEND)
    std::string _groupId;
    std::string _callee;      // SEND: 수신자
    std::string _sesid;       // flow 상관 id (CSP 계승)
    std::string _service;     // "mcdata"
    long long _maxSize = 0;   // 수신 총량 상한 (0=무제한; cmdp 절대상한과 min 적용됨)
    std::string _localPath;   // msrp://{MsrpIp}:{MsrpPort}/{sessionId};tcp
    std::string _remotePath;  // 상대 a=path (RECV: offer / SEND: SET_REMOTE_PATH)

    // ── RECV 조립 상태 ─────────────────────────────────────────────────
    std::string _sigPart;        // application/vnd.3gpp.mcdata-signalling (raw)
    std::string _payloadPart;    // application/vnd.3gpp.mcdata-payload (raw)
    std::string _multipartBody;  // multipart/mixed 통째 수신 시
    std::string _multipartCt;    // 그 Content-Type (boundary 포함)
    long long _rxBytes = 0;

    // ── SEND 상태 ──────────────────────────────────────────────────────
    std::string _fileId;          // 재전달 원본 file id
    std::string _sendBody;        // 전송 본문 (PFdStore::LoadRaw)
    std::string _sendContentType;
    std::string _sendMsgId;       // Message-ID
    size_t _sendOffset = 0;       // 다음 청크 시작 오프셋
    std::string _lastChunkTid;    // 마지막('$') 청크의 trans-id — 200 매칭용
    bool _sendStarted = false;

    // ── 공통 ───────────────────────────────────────────────────────────
    time_t _createdAt = 0;
    time_t _lastActivity = 0;
    PMsrpConnection* _conn = nullptr;  // 바인딩된 연결 (미접속이면 null)
    bool _completed = false;           // RECV 세트 완성 / SEND 전송 완료
    bool _aborted = false;
    std::string _abortReason;

    /**
     * @brief RECV: SEND 청크 1건 수용. 조립·크기 게이트.
     * @param setComplete [out] 이 청크로 메시지 세트가 완성됨
     * @return 응답 코드 (200 / 413 초과 / 415 미지원 Content-Type)
     */
    int acceptChunk(const PMsrpMessage& m, bool& setComplete);

    /** RECV 세트 완성 여부 */
    bool recvSetComplete() const {
        return !_multipartBody.empty() || (!_sigPart.empty() && !_payloadPart.empty());
    }

    /**
     * @brief 수신 세트를 multipart/mixed 본문으로 정규화 (재전달·TLV 파싱 공용).
     *        멀티파트 통째 수신이면 그대로, 파트 2건 수신이면 합성한다.
     * @return contentType 포함 성공 여부
     */
    bool buildCombinedBody(std::string& body, std::string& contentType) const;

    /**
     * @brief SEND: 다음 청크 프레임 생성. 전송 오프셋 갱신.
     * @param out [out] 송신할 프레임 바이트
     * @return 청크 생성됨(true) / 더 없음(false)
     */
    bool nextSendChunk(std::string& out);

    /** SEND: 응답 수신 처리. 마지막 청크 200 이면 완료 마킹 */
    void onSendResponse(const PMsrpMessage& m);

    void touch() { _lastActivity = time(nullptr); }

private:
    // RECV 청크 조립 중인 현재 메시지 (Message-ID 단위)
    std::string _curMsgId;
    std::string _curContentType;
    std::string _curBuf;
};

#endif
