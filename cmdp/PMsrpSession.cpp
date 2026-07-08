#include "PMsrpSession.h"
#include "PLog.h"
#include <strings.h>

static const size_t kSendChunkSize = 16 * 1024;

static bool _ctContains(const std::string& ct, const char* needle) {
    // Content-Type 비교 (대소문자 무시, 파라미터 무시)
    std::string lower;
    lower.reserve(ct.size());
    for (char c : ct) lower += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    return lower.find(needle) != std::string::npos;
}

int PMsrpSession::acceptChunk(const PMsrpMessage& m, bool& setComplete) {
    setComplete = false;
    touch();

    std::string ct = m.GetHeader("Content-Type");
    if (ct.empty()) return 200;  // bodiless SEND — 연결 바인딩용, 조립 없음

    if (!_ctContains(ct, "multipart/mixed") && !_ctContains(ct, "mcdata-signalling") &&
        !_ctContains(ct, "mcdata-payload")) {
        LOG_WARN("PMsrpSession", "session=%s unsupported Content-Type: %s",
                 _sessionId.c_str(), ct.c_str());
        return 415;
    }

    _rxBytes += (long long)m.body.size();
    if (_maxSize > 0 && _rxBytes > _maxSize) {
        _aborted = true;
        _abortReason = "size_exceeded";
        LOG_WARN("PMsrpSession", "session=%s size exceeded rx=%lld max=%lld",
                 _sessionId.c_str(), _rxBytes, _maxSize);
        return 413;
    }

    // Message-ID 단위 청크 조립 (직결 TCP 단일 연결 — 순서 도착 전제)
    std::string msgId = m.GetHeader("Message-ID");
    if (msgId != _curMsgId) {
        _curMsgId = msgId;
        _curContentType = ct;
        _curBuf.clear();
    }
    _curBuf.append(m.body);

    if (m.contFlag == '#') {  // 발신측 중단 — 조립분 폐기
        _curBuf.clear();
        _curMsgId.clear();
        return 200;
    }
    if (m.contFlag != '$') return 200;  // '+' 후속 청크 대기

    // 메시지 1건 완성 — Content-Type 별 슬롯에 귀속
    if (_ctContains(_curContentType, "multipart/mixed")) {
        _multipartBody = _curBuf;
        _multipartCt = _curContentType;
    } else if (_ctContains(_curContentType, "mcdata-signalling")) {
        _sigPart = _curBuf;
    } else {
        _payloadPart = _curBuf;
    }
    _curBuf.clear();
    _curMsgId.clear();

    if (recvSetComplete()) {
        _completed = true;
        setComplete = true;
    }
    return 200;
}

bool PMsrpSession::buildCombinedBody(std::string& body, std::string& contentType) const {
    if (!_multipartBody.empty()) {
        body = _multipartBody;
        contentType = _multipartCt;
        return true;
    }
    if (_sigPart.empty()) return false;
    // 파트 2건 수신 — multipart/mixed 합성 (McDataCodec 파서·MSRP 재전달 공용 형식)
    std::string boundary = "cmdp-" + _sessionId;
    std::string b = "--" + boundary + "\r\n";
    b += "Content-Type: application/vnd.3gpp.mcdata-signalling\r\n\r\n";
    b += _sigPart;
    b += "\r\n--" + boundary + "\r\n";
    b += "Content-Type: application/vnd.3gpp.mcdata-payload\r\n\r\n";
    b += _payloadPart;
    b += "\r\n--" + boundary + "--\r\n";
    body = b;
    contentType = "multipart/mixed;boundary=" + boundary;
    return true;
}

bool PMsrpSession::nextSendChunk(std::string& out) {
    if (_sendOffset >= _sendBody.size() && _sendStarted) return false;
    if (_sendBody.empty()) return false;

    size_t remain = _sendBody.size() - _sendOffset;
    size_t n = remain < kSendChunkSize ? remain : kSendChunkSize;
    bool last = (_sendOffset + n >= _sendBody.size());

    std::string tid = MsrpNewTransId();
    // 본문에 이 tid 의 end-line 유사열이 있으면 재생성 (RFC 4975 §7.1)
    while (_sendBody.find("\r\n-------" + tid) != std::string::npos)
        tid = MsrpNewTransId();

    out = MsrpBuildSendChunk(tid, _remotePath, _localPath, _sendMsgId, _sendContentType,
                             _sendBody.substr(_sendOffset, n), (long long)_sendOffset + 1,
                             (long long)(_sendOffset + n), (long long)_sendBody.size(),
                             false, last ? '$' : '+');
    _sendOffset += n;
    _sendStarted = true;
    if (last) _lastChunkTid = tid;
    touch();
    return true;
}

void PMsrpSession::onSendResponse(const PMsrpMessage& m) {
    touch();
    if (m.statusCode != 200) {
        _aborted = true;
        _abortReason = "peer_" + std::to_string(m.statusCode);
        return;
    }
    if (!_lastChunkTid.empty() && m.transId == _lastChunkTid) _completed = true;
}
