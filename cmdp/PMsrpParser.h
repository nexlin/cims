/*
 * MSRP (RFC 4975) 프레이머 — cmdp (CIMS MCData Media Plane)
 *
 * TCP 스트림에서 MSRP 요청(SEND/REPORT)·응답을 증분 파싱하고, 송신용 프레임을
 * 생성한다. 릴레이(RFC 4976) 미지원 — 단말↔서버 직결 전용.
 *
 * 프레임 구조:
 *   MSRP <trans-id> <SEND|REPORT|status-code [phrase]>\r\n
 *   <headers>\r\n
 *   [\r\n<body>]                     ← 본문 없는 프레임은 blank line 없이 end-line 직행
 *   -------<trans-id><$|+|#>\r\n     ← $ 완료 / + 후속 청크 / # 중단
 *
 * 본문은 바이너리 안전(std::string 사용, NUL 포함 가능). end-line 판정은
 * "\n-------<trans-id><flag>\r\n" 전체 일치로만 하므로 본문 내 유사 시퀀스
 * (다른 trans-id)는 종료로 오인하지 않는다.
 *
 * 의존성 없음(std only) — tests/cmdp_msrp_parser_test.cpp 에서 단독 컴파일.
 */

#ifndef _P_MSRP_PARSER_H_
#define _P_MSRP_PARSER_H_

#include <string>
#include <vector>
#include <utility>

/** 파싱된 MSRP 프레임 1건 */
struct PMsrpMessage {
    enum Kind { MSRP_REQUEST, MSRP_RESPONSE };

    Kind kind = MSRP_REQUEST;
    std::string transId;
    std::string method;       // 요청: SEND / REPORT / (확장)
    int statusCode = 0;       // 응답: 200 등
    std::string reason;       // 응답: OK 등 (없을 수 있음)
    char contFlag = '$';      // end-line 플래그: '$' 완료, '+' 후속 청크, '#' 중단
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;         // 바이너리 안전

    /** 헤더 조회 (이름 대소문자 무시). 없으면 빈 문자열 */
    std::string GetHeader(const std::string& name) const;
};

// Byte-Range 헤더 값 — "1-25/25" 또는 end·total 자리에 '*' (스트리밍)
struct PMsrpByteRange {
    long long start = 0;      // 1-base
    long long end = 0;        // endStar=true 면 무의미
    long long total = 0;      // totalStar=true 면 무의미
    bool endStar = false;
    bool totalStar = false;
    bool valid = false;
};

PMsrpByteRange MsrpParseByteRange(const std::string& value);

/**
 * @brief 증분 MSRP 파서. feed() 로 스트림 조각을 적재하고 next() 로 완성 프레임을 꺼낸다.
 */
class PMsrpParser {
public:
    void feed(const char* data, size_t len) { _buf.append(data, len); }

    /**
     * 완성된 프레임이 있으면 out 에 채우고 true. 프레이밍 오류 발견 시 false 를
     * 반환하고 hasError() 가 true 가 된다(연결 종료 권장).
     */
    bool next(PMsrpMessage& out);

    bool hasError() const { return _error; }
    const std::string& errorReason() const { return _errorReason; }

    /** 미소비 버퍼 크기 (오버플로 가드용) */
    size_t pendingBytes() const { return _buf.size(); }

private:
    std::string _buf;
    bool _error = false;
    std::string _errorReason;

    void setError(const std::string& reason);
};

// ── 송신 프레임 빌더 ────────────────────────────────────────────────────────

/** 응답 프레임 (본문 없음). toPath/fromPath 는 응답 관점의 값 */
std::string MsrpBuildResponse(const std::string& transId, int code, const std::string& reason,
                              const std::string& toPath, const std::string& fromPath);

/**
 * SEND 청크. byte-range 는 1-base [rangeStart..rangeEnd], total 은 전체 크기(<0 이면 '*').
 * successReport=true 면 Success-Report: yes 부여. flag: '$' 마지막 / '+' 후속 있음.
 * contentType 이 비어 있으면 본문 없는(bodiless) SEND 를 만든다.
 */
std::string MsrpBuildSendChunk(const std::string& transId, const std::string& toPath,
                               const std::string& fromPath, const std::string& messageId,
                               const std::string& contentType, const std::string& payloadSlice,
                               long long rangeStart, long long rangeEnd, long long total,
                               bool successReport, char flag);

/** REPORT 프레임 (Success-Report: yes 요청에 대한 수신 확인) */
std::string MsrpBuildReport(const std::string& transId, const std::string& toPath,
                            const std::string& fromPath, const std::string& messageId,
                            long long rangeStart, long long rangeEnd, long long total,
                            int code, const std::string& reason);

/** trans-id 생성 (영숫자 12자, seed 로 재현성 제어 없음 — 호출측이 본문 충돌 시 재생성) */
std::string MsrpNewTransId();

/** msrp URI "msrp://ip:port/session;tcp" 에서 host/port/session 분해. 실패 시 false */
bool MsrpParseUri(const std::string& uri, std::string& host, int& port, std::string& session);

#endif
