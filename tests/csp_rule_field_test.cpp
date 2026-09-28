/*
 * csp_rule_field_test — Rule field `<원천>.<부분>` 해석·비교 단위시험
 * (csp/CspRuleField.cpp — sip_service_model.md §2-5, RFC 3261 §7.3 · §19.1.4 ·
 * §20.10, RFC 3325, RFC 3966)
 *
 * 빌드 (레포 루트, S1-UNIT-CSP 가 같은 명령을 낸다):
 *   g++ -std=c++17 -Icsp -Iinclude -Iext/psip/SipParser -Iext/psip/SipPlatform
 * \
 *       tests/csp_rule_field_test.cpp csp/CspRuleField.cpp csp/CspDialPlan.cpp
 * \
 *       build/csp/psip_build/libSipParser.a
 * build/csp/psip_build/libSipPlatform.a -lpthread \ -o
 * build/csp_rule_field_test && build/csp_rule_field_test
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CspRuleField.h"
#include "SipMessage.h"

static int g_iPass = 0;

#define CHECK(cond, name)                                                      \
  do {                                                                         \
    if (cond) {                                                                \
      ++g_iPass;                                                               \
      printf("PASS %s\n", name);                                               \
    } else {                                                                   \
      printf("FAIL %s (line %d)\n", name, __LINE__);                           \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static const char *INVITE =
    "INVITE sip:02-2104-5012@pbx.local;user=phone SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 10.50.0.91:5060;branch=z9hG4bK-7f\r\n"
    "f: \"Branch 3F\" <sip:1001@PBX.HQ.test>;tag=a1\r\n"
    "To: <sip:02-2104-5012@pbx.local>\r\n"
    "Call-ID: 7f3a@10.50.0.91\r\n"
    "CSeq: 1 INVITE\r\n"
    "Max-Forwards: 70\r\n"
    "Contact: <sip:1001@10.50.0.91:5060;transport=udp>\r\n"
    "P-Asserted-Identity: <sip:+82215551001@pbx.hq.test>, "
    "<tel:+82215551001>\r\n"
    "Diversion: <sip:+82215550000@pbx.hq.test>;reason=unconditional\r\n"
    "diversion: <sip:%2B82215550009@pbx.hq.test>;reason=user-busy\r\n"
    "User-Agent: Avaya-CM/8.1\r\n"
    "Content-Length: 0\r\n\r\n";

static bool Eval(const MessageCtx &ctx, const char *field, const char *op,
                 const char *val) {
  std::vector<std::string> v;
  bool host;
  if (!RuleFieldValues(ctx, field, v, host))
    return false;
  return RuleApplyOp(v, op, val, host);
}

int main() {
  CSipMessage msg;
  CHECK(msg.Parse(INVITE, (int)strlen(INVITE)) > 0, "INVITE 해석");
  msg.m_strClientIp = "10.50.0.91";
  msg.m_iClientPort = 5060;

  MessageCtx ctx;
  ctx.msg = &msg;
  ctx.local_node = "peering-udp";
  ctx.callee_e164 = "+82221045012";
  ctx.req_uri_user = "+82221045012"; // 옛 이름 — 호출부가 채운 값 그대로
  ctx.method =
      "INVITE"; // method 는 옛 이름과 같다 — 호출부(ModuleDispatcher)가 채운다
  ctx.trusted_peer = true;

  // 문법
  CHECK(RuleFieldSyntaxOk("req_uri_user"), "옛 이름 허용");
  CHECK(RuleFieldSyntaxOk("header:P-Asserted-Identity.user"),
        "header:<이름>.<부분>");
  CHECK(RuleFieldSyntaxOk("header:Diversion.param:reason"), "param:<이름>");
  CHECK(RuleFieldSyntaxOk("callee.e164") && RuleFieldSyntaxOk("src.ip") &&
            RuleFieldSyntaxOk("local_node"),
        "callee · src · local_node");
  CHECK(!RuleFieldSyntaxOk("callee.user") && !RuleFieldSyntaxOk("from.nope") &&
            !RuleFieldSyntaxOk("header:") &&
            !RuleFieldSyntaxOk("header:Bad Name.user") &&
            !RuleFieldSyntaxOk("unknown_field"),
        "문법 오류 거절");

  // 원천별 값
  CHECK(Eval(ctx, "request_uri.user", "eq", "02-2104-5012"),
        "request_uri.user = 받은 그대로");
  CHECK(Eval(ctx, "callee.e164", "eq", "+82221045012"),
        "callee.e164 = 번역 뒤");
  CHECK(Eval(ctx, "request_uri.param:user", "eq", "phone"), "URI param");
  CHECK(Eval(ctx, "from.user", "eq", "1001"), "compact form f: = From");
  CHECK(Eval(ctx, "from.display", "eq", "Branch 3F"), "display — 따옴표 벗김");
  CHECK(Eval(ctx, "from.host", "eq", "pbx.hq.test"),
        "host 대소문자 무시 (PBX.HQ.test)");
  CHECK(Eval(ctx, "from.param:tag", "eq", "a1"), "header param");
  CHECK(Eval(ctx, "contact.port", "eq", "5060"), "Contact port");
  CHECK(Eval(ctx, "header:Via.host", "eq", "10.50.0.91"), "Via host");
  CHECK(Eval(ctx, "header:user-agent", "prefix", "Avaya"),
        "헤더 이름 대소문자 무시");
  CHECK(Eval(ctx, "src.ip", "in_cidr", "10.50.0.0/16") &&
            Eval(ctx, "src.port", "eq", "5060"),
        "src");
  CHECK(Eval(ctx, "local_node", "eq", "peering-udp") &&
            Eval(ctx, "method", "eq", "INVITE"),
        "local_node · method");

  // 여러 값 — 쉼표 · 같은 헤더 여러 줄
  CHECK(Eval(ctx, "header:P-Asserted-Identity.scheme", "eq", "tel"),
        "PAI 두 번째 값(tel:) 도 본다");
  CHECK(Eval(ctx, "header:P-Asserted-Identity.user", "eq", "+82215551001"),
        "PAI user");
  CHECK(Eval(ctx, "header:Diversion.param:reason", "eq", "user-busy"),
        "같은 헤더 두 번째 줄");
  CHECK(Eval(ctx, "header:Diversion.user", "eq", "+82215550009"),
        "user %2B 풀어 비교");
  CHECK(Eval(ctx, "header:Diversion", "exists", ""), "헤더 있음");
  CHECK(Eval(ctx, "header:History-Info", "not_exists", ""), "헤더 없음");
  CHECK(!Eval(ctx, "header:Diversion.param:reason", "ne", "user-busy") &&
            Eval(ctx, "header:Diversion.param:reason", "ne", "no-answer"),
        "ne = 어느 값도 같지 않을 때");

  // P-Asserted-Identity 신뢰 (RFC 3325 §4) — 식별된 피어가 아니면 값이 없다
  ctx.trusted_peer = false;
  CHECK(Eval(ctx, "header:P-Asserted-Identity", "not_exists", ""),
        "믿지 않는 원천의 PAI 는 무시");
  CHECK(RulePaiUser(&msg) == "+82215551001",
        "RulePaiUser (신뢰 판단은 호출부)");

  // 옛 이름 — 호출부가 채운 값 그대로 (종전 동작)
  CHECK(Eval(ctx, "req_uri_user", "eq", "+82221045012"),
        "옛 이름 req_uri_user");
  CHECK(Eval(ctx, "from_uri_user", "not_exists", ""),
        "옛 이름 — 호출부가 비워 두면 빈 값");

  // 원본이 없는 컨텍스트 — 새 문법은 빈 값
  MessageCtx bare;
  CHECK(Eval(bare, "from.user", "not_exists", "") &&
            !Eval(bare, "from.user", "eq", "1001"),
        "원본 없음");

  printf("\n%d passed\n", g_iPass);
  return 0;
}
