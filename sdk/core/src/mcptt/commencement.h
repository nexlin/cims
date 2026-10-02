// libcimsue 내부 — 착신 MC 호의 개시 방식(TS 24.379 §10.1.1.2.1.2 7)·8) · TS 24.281 §9.2.1.2.1.2 7)·8) · RFC 5373).
#pragma once

#include <cctype>
#include <string>

namespace cimsue {
namespace mcptt {

/** Answer-Mode·Priv-Answer-Mode 헤더 값의 방식 토큰 — "Auto"/"Manual"(대소문자 무시, `;require` 등 파라미터는 버린다). 없으면 빈 문자열. */
inline std::string answerModeToken(const std::string& headerValue) {
    std::string t;
    for (char c : headerValue) {
        if (c == ';' || c == ',') break;
        if (!std::isspace((unsigned char)c)) t += (char)std::tolower((unsigned char)c);
    }
    return t;
}

/**
 * 자동 개시(곧바로 200)인가 — false 면 수동 개시(사용자 수락을 기다린다).
 *   - `Priv-Answer-Mode: Auto` = 권한 있는 요청자의 자동 응답 요구(RFC 5373 §4.2 — 사용자 설정보다 우선) → 자동
 *   - `Answer-Mode: Manual` → 수동(단말 설정이 자동이어도 따른다 — 8)a)·b))
 *   - `Answer-Mode: Auto` → 단말 설정이 자동이면 자동(7)a)). 설정이 수동이면 자동 응답을 허용하지 않는다(7)b) 는 단말 재량) → 수동
 *   - 헤더 없음 → 단말 설정
 */
inline bool autoCommencement(const std::string& answerMode, const std::string& privAnswerMode, bool settingAuto) {
    if (answerModeToken(privAnswerMode) == "auto") return true;
    if (answerModeToken(answerMode) == "manual") return false;
    return settingAuto;
}

/** 사용자 거절의 Warning 문구(TS 24.379 §6.2.3.2.1 1)·§6.2.3.2.2 2) · TS 24.281 같은 절 — 480 과 함께). */
constexpr int kWarnUserDeclined = 110;
constexpr const char* kWarnUserDeclinedText = "110 user declined the call invitation";

}  // namespace mcptt
}  // namespace cimsue
