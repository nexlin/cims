#ifndef __CSP_RULE_FIELD_H__
#define __CSP_RULE_FIELD_H__

#include <string>
#include <vector>

class CSipMessage;

/**
 * Rule 의 field — SIP 메시지에서 무엇을 꺼내 비교하는가 (sip_service_model.md §2-5).
 *
 *   field 문법 = <원천>[.<부분>]
 *     원천  callee            번호 변환(다이얼 플랜) 뒤의 착신 +E.164 — Routing 에서만 값이 있다 (부분 = e164)
 *           request_uri       Request-URI (받은 그대로)
 *           from · to · contact
 *           header:<이름>     그 밖의 헤더 — 이름으로 지정(대소문자 무시, compact form 포함 — RFC 3261 §7.3.1·§7.3.3)
 *           src               패킷 원천 (부분 = ip · port)
 *           local_node        받은 Local Node 이름 · method   요청 메서드
 *     부분  user · host · port · scheme · display · param:<이름>   — 주소형(name-addr/addr-spec, RFC 3261 §20.10 ·
 * §19.1) 없으면 헤더 값 그대로
 *   - 같은 헤더가 여러 번 오거나 한 줄에 쉼표로 여러 값이 오면 값이 여럿이다 — 긍정 연산은 하나라도 맞으면,
 *     ne · not_exists 는 하나도 맞지 않으면 참이다.
 *   - host 부분은 대소문자를 가리지 않고 비교한다(RFC 3261 §19.1.4). user 부분은 %xx 를 풀어 비교한다.
 *   - P-Asserted-Identity 는 신뢰하는 상대(들어온 Route 로 식별된 피어)가 보낸 것만 값으로 쓴다(RFC 3325 §4·§5).
 *   - 옛 이름(from_uri_user · req_uri_user … 12종)은 종전 뜻 그대로다. 비어 있던 p_asserted_identity · via_host 는
 *     이제 값이 있다(dst_ip 는 여전히 빈 값).
 */

/** Rule 평가 컨텍스트 — 옛 필드 값(호출부가 채움) + 새 문법이 읽는 원본 메시지. */
struct MessageCtx {
    // 옛 이름 12종의 값 — 호출부가 종전대로 채운다 (req_uri_user 는 Routing=번역 뒤, ACL=받은 그대로)
    std::string from_uri_host;
    std::string from_uri_user;
    std::string to_uri_host;
    std::string to_uri_user;
    std::string req_uri_host;
    std::string req_uri_user;
    std::string src_ip;
    std::string dst_ip;
    std::string user_agent;
    std::string method;
    std::string p_asserted_identity;
    std::string via_host;
    // 새 문법
    const CSipMessage *msg = nullptr;  // 원본 — 없으면 새 문법 필드는 전부 빈 값
    std::string callee_e164;           // 번역 뒤 착신 — Routing 만 채운다
    std::string local_node;            // 받은 Local Node 이름
    bool trusted_peer = false;         // 들어온 Route 로 식별된 피어 — P-Asserted-Identity 를 믿는다
};

/** field 문자열이 문법에 맞는가 (옛 이름 포함). */
bool RuleFieldSyntaxOk( const std::string &strField );

/** field 의 값들. bHostPart = host 부분(대소문자 무시 비교). 문법이 틀리면 false. */
bool RuleFieldValues( const MessageCtx &ctx, const std::string &strField, std::vector<std::string> &vecOut,
                      bool &bHostPart );

/** 값들에 op 적용 — 여러 값의 결합 규칙(위)과 host 대소문자 규칙까지. bad regex 는 false. */
bool RuleApplyOp( const std::vector<std::string> &vecValues, const std::string &strOp, const std::string &strValue,
                  bool bHostPart );

/** 옛 이름 12종의 ctx 값 — 없는 이름이면 nullptr (CspRuleEvaluator 가 옛 동작 그대로 평가할 때). */
const std::string *RuleLegacyFieldValue( const MessageCtx &ctx, const std::string &strField );

/** P-Asserted-Identity 첫 값의 user — 옛 이름 p_asserted_identity 를 채울 때 (신뢰 여부는 호출부가 판단). */
std::string RulePaiUser( const CSipMessage *pclsMessage );

#endif  // __CSP_RULE_FIELD_H__
