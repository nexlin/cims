// libcimsue 내부 — 착신 MC 호의 개시 방식(TS 24.379 §10.1.1.2.1.2 7)·8) · TS 24.281 §9.2.1.2.1.2 7)·8) · RFC 5373).
#pragma once

#include <cctype>
#include <string>

#include "cimsue/types.h"

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
 * 착신 MC 호의 개시 방식 — Manual 이면 수동 개시(사용자 수락을 기다린다), 그 밖은 자동 개시(곧바로 200).
 *   - `Priv-Answer-Mode: Auto` = 권한 있는 요청자의 강제 자동(RFC 5373 §4.2 — 사용자 설정보다 우선) → ForceAuto (TS 24.379 §11.1.1.2.1.2 9)c))
 *   - `Priv-Answer-Mode: Manual` → Manual (10)c))
 *   - `Answer-Mode: Manual` → Manual (단말 설정이 자동이어도 따른다 — 10)a)·b), 그룹 호 §10.1.1.2.1.2 8))
 *   - `Answer-Mode: Auto` → 단말 설정이 자동이면 Auto(9)a)). 설정이 수동이면 자동 응답을 허용하지 않는다(9)b) 는 단말 재량) → Manual
 *   - 헤더 없음 → 단말 설정
 */
inline CommencementMode commencementOf(const std::string& answerMode, const std::string& privAnswerMode, bool settingAuto) {
    const std::string priv = answerModeToken(privAnswerMode);
    if (priv == "auto") return CommencementMode::ForceAuto;
    if (priv == "manual") return CommencementMode::Manual;
    if (answerModeToken(answerMode) == "manual") return CommencementMode::Manual;
    return settingAuto ? CommencementMode::Auto : CommencementMode::Manual;
}

/** 자동 개시(곧바로 200)인가 — [commencementOf] 가 Manual 이 아니다. */
inline bool autoCommencement(const std::string& answerMode, const std::string& privAnswerMode, bool settingAuto) {
    return commencementOf(answerMode, privAnswerMode, settingAuto) != CommencementMode::Manual;
}

/** 사용자 거절의 Warning 문구(TS 24.379 §6.2.3.2.1 1)·§6.2.3.2.2 2) · TS 24.281 같은 절 — 480 과 함께). */
constexpr int kWarnUserDeclined = 110;
constexpr const char* kWarnUserDeclinedText = "110 user declined the call invitation";

}  // namespace mcptt
}  // namespace cimsue
