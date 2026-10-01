// libcimsue 내부 — 통화 중 영상 전환(1:1 호 — RFC 3264 §8.1 영상 추가·§8.2 제거)의 SDP 판정·응답 조각 (ue_sdk.md §4.5).
// 단위시험이 직접 검증하므로 엔진 부팅 없이 호출 가능해야 한다.
#pragma once

#include <string>

namespace cimsue {
namespace detail {

/** SDP 의 첫 m=video 줄의 포트 — 줄이 없으면 -1, 거절·제거된 줄은 0(RFC 3264 §6·§8.2). */
int sdpVideoPort(const std::string& sdp);

/** answer SDP 의 m=video 를 모두 거절로 바꾼다 — 포트 0, 그 미디어 절의 a=·b= 줄을 뺀다. 형식 목록은 남긴다(RFC 3264 §6 — 거절된
 *  스트림의 형식은 무시되지만 SDP 문법상 하나 이상 있어야 한다). 다른 미디어 절은 그대로. */
std::string rejectVideoSdp(const std::string& sdp);

/** 491 Request Pending 을 받은 re-INVITE 를 다시 보낼 때까지의 시간(ms) — RFC 3261 §14.1: 다이얼로그 Call-ID 를 만든 쪽이면
 *  2.1~4 s, 아니면 0~2 s, 10 ms 단위. r = [0, 1) 난수. */
unsigned reinviteRetryDelayMs(bool callIdOwner, double r);

}  // namespace detail
}  // namespace cimsue
