// libcimsue 단위시험 — C API 평탄화 층 (cimsue_c.h · ue_sdk.md §6.4) (S1-UE-UNIT)
// 프로토콜은 시험하지 않는다 — 타입 변환·기본값 규약·수명 규약·콜백 전달이 C++ 표면과 1:1 인지만 본다.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cimsue/cimsue.h"
#include "cimsue/cimsue_c.h"

using namespace cimsue;

static const char* kProfile = R"({
  "user": { "displayName": "테스트001", "loginId": "test001" },
  "csc": { "host": "121.161.164.48", "port": 4430 },
  "countryCode": "82",
  "services": [
    { "kind": "volte",
      "sip": { "host": "121.161.164.48", "port": 5060, "transport": "UDP",
               "transports": [ { "transport": "UDP", "port": 5060 }, { "transport": "TLS", "port": 5061 } ],
               "default": "UDP", "domain": "ims.example.org", "mediaSecurity": "optional", "security": ["tls"] },
      "account": { "msisdn": "+821300000001", "imsi": "45033821300000001", "sipHa1": "0123456789abcdef0123456789abcdef" } },
    { "kind": "ptt",
      "sip": { "host": "121.161.164.48", "port": 5061, "transport": "TLS", "transports": [ { "transport": "TLS", "port": 5061 } ],
               "default": "TLS", "enforced": true, "domain": "ptt.example.org" },
      "account": { "msisdn": "+82500000001", "imsi": "4503382500000001", "mcpttId": "tel:+82500000001",
                   "authScheme": "aka", "aka": { "k": "00112233", "opc": "44556677", "amf": "8000" } } }
  ],
  "dispatch": { "groupId": "dg-1", "groupName": "관제1", "pilotId": "+8215001000", "monitorScope": "all", "pttListen": "listed", "listenVisibility": "hidden" }
})";

// ── 열거형 값 동일성 — 바인딩이 정수를 그대로 넘기는 근거 ──
TEST(CApi, EnumValuesMatchCxx) {
    EXPECT_EQ((int)CIMSUE_TRANSPORT_TLS, (int)Transport::TLS);
    EXPECT_EQ((int)CIMSUE_AUTH_AKA, (int)AuthScheme::Aka);
    EXPECT_EQ((int)CIMSUE_SRTP_REQUIRED, (int)MediaSecurity::Required);
    EXPECT_EQ((int)CIMSUE_REG_FAILED, (int)RegState::Failed);
    EXPECT_EQ((int)CIMSUE_CALL_DISCONNECTED, (int)CallState::Disconnected);
    EXPECT_EQ((int)CIMSUE_DIR_INCOMING, (int)CallDir::Incoming);
    EXPECT_EQ((int)CIMSUE_FLOOR_QUEUED, (int)FloorState::Queued);
    EXPECT_EQ((int)CIMSUE_FLOOR_EV_TALK_LIMIT, (int)FloorEvent::Kind::TalkLimit);
    EXPECT_EQ((int)CIMSUE_FLOOR_EV_OTHER, (int)FloorEvent::Kind::Other);
    EXPECT_STREQ(cimsue_reg_state_str(CIMSUE_REG_REGISTERED), toString(RegState::Registered));
    EXPECT_STREQ(cimsue_floor_kind_str(CIMSUE_FLOOR_EV_GRANTED), toString(FloorEvent::Kind::Granted));
    EXPECT_STREQ(cimsue_version(), Engine::version().c_str());
}

// ── 설정 기본값 규약 — default() 의 숫자 필드 = C++ 기본값, 문자열 NULL = C++ 기본값 유지 ──
TEST(CApi, ConfigDefaultsFollowCxx) {
    const AccountConfig d;
    cimsue_account_config_t c;
    cimsue_account_config_default(&c);
    EXPECT_EQ(c.server_port, d.serverPort);
    EXPECT_EQ((int)c.transport, (int)d.transport);
    EXPECT_EQ(c.expires_sec, d.expiresSec);
    EXPECT_EQ(c.auto_answer_mcptt != 0, d.autoAnswerMcptt);
    EXPECT_EQ(c.aka_amf, nullptr);                          // NULL → C++ 기본 "8000"

    // 최소 필드만 채워 헬퍼가 C++ 인라인 멤버와 같은 답을 내는지
    c.server_host = "csp.example.org"; c.domain = "ims.example.org";
    c.msisdn = "+821300000001"; c.imsi = "45033821300000001"; c.ha1 = "0123456789abcdef0123456789abcdef";
    AccountConfig cxx;
    cxx.serverHost = c.server_host; cxx.domain = c.domain; cxx.msisdn = c.msisdn; cxx.imsi = c.imsi; cxx.ha1 = c.ha1;
    char buf[128];
    ASSERT_LT(cimsue_account_config_aor(&c, buf, sizeof buf), (int32_t)sizeof buf);
    EXPECT_EQ(std::string(buf), cxx.aor());
    cimsue_account_config_digest_username(&c, buf, sizeof buf);
    EXPECT_EQ(std::string(buf), cxx.digestUsername());
    cimsue_account_config_mcptt_id(&c, buf, sizeof buf);
    EXPECT_EQ(std::string(buf), cxx.effectiveMcpttId());   // 비면 tel:+msisdn
    EXPECT_EQ(cimsue_account_config_is_complete(&c), 1);
    c.ha1 = ""; c.password = nullptr;                        // 빈 문자열은 지운다 → 자격 없음
    EXPECT_EQ(cimsue_account_config_is_complete(&c), 0);

    // 문자열 산출 규약 — cap 이 모자라면 잘린 채 필요한 길이를 돌려준다, NULL/0 은 길이만
    int32_t need = cimsue_account_config_aor(&c, nullptr, 0);
    EXPECT_EQ(need, (int32_t)cxx.aor().size());
    char tiny[8];
    EXPECT_EQ(cimsue_account_config_aor(&c, tiny, sizeof tiny), need);
    EXPECT_EQ(std::string(tiny), cxx.aor().substr(0, 7));

    cimsue_dialog_info_t dlg{};
    dlg.call_id = "abc@host"; dlg.local_tag = "L1"; dlg.remote_tag = "R1";
    cimsue_dialog_info_join_header(&dlg, buf, sizeof buf);
    EXPECT_EQ(std::string(buf), "abc@host;to-tag=R1;from-tag=L1");
}

// ── 프로파일 평탄화 — 중첩 배열(services/transports/security)·dispatch·to_account ──
TEST(CApi, ProfileFlattenAndToAccount) {
    cimsue_profile_t p{};
    ASSERT_EQ(cimsue_csc_parse_profile(kProfile, &p), CIMSUE_OK) << cimsue_last_error();
    EXPECT_STREQ(p.display_name, "테스트001");
    EXPECT_EQ(p.csc_port, 4430);
    ASSERT_EQ(p.service_count, 2);
    const cimsue_service_profile_t* v = cimsue_profile_service(&p, "volte");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->sip_port, 5060);
    ASSERT_EQ(v->transport_count, 2);
    EXPECT_EQ(v->transports[1].transport, CIMSUE_TRANSPORT_TLS);
    EXPECT_EQ(v->media_security, CIMSUE_SRTP_OPTIONAL);
    ASSERT_EQ(v->sec_mechanism_count, 1);
    EXPECT_STREQ(v->sec_mechanisms[0], "tls");
    EXPECT_EQ(cimsue_profile_service(&p, "video"), nullptr);
    EXPECT_TRUE(p.dispatch.present);
    EXPECT_STREQ(p.dispatch.group_id, "dg-1");
    EXPECT_STREQ(p.dispatch.monitor_scope, "all");

    cimsue_account_config_t a{};
    cimsue_service_profile_to_account(v, nullptr, &a);
    char buf[128];
    cimsue_account_config_digest_username(&a, buf, sizeof buf);
    EXPECT_STREQ(buf, "45033821300000001@ims.example.org");
    EXPECT_STREQ(a.ha1, "0123456789abcdef0123456789abcdef");
    ASSERT_EQ(a.sec_mechanism_count, 1);
    EXPECT_STREQ(a.sec_mechanisms[0], "tls");
    EXPECT_EQ(cimsue_account_config_is_complete(&a), 1);

    const cimsue_service_profile_t* t = cimsue_profile_service(&p, "ptt");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->auth_scheme, CIMSUE_AUTH_AKA);
    cimsue_service_profile_to_account(t, nullptr, &a);
    EXPECT_STREQ(a.aka_k, "00112233");
    cimsue_account_config_mcptt_id(&a, buf, sizeof buf);
    EXPECT_STREQ(buf, "tel:+82500000001");
    EXPECT_EQ(cimsue_account_config_is_complete(&a), 1);     // AKA K 로 완성

    // 실패 경로 — 사유는 last_error
    EXPECT_NE(cimsue_csc_parse_profile("{not json", &p), CIMSUE_OK);
    EXPECT_STRNE(cimsue_last_error(), "");
    EXPECT_EQ(p.service_count, 0);
}

// ── 엔진 수명·콜백 전달 — 헤드리스(null 장치) 기동, 리스너 구조체 복사, 실패 코드 = C++ Result::code ──
namespace {
struct Seen {
    int logs = 0;
    std::vector<std::string> regReasons;
    int stopped = 0;
};
void CIMSUE_CALL onLog(void* u, int32_t, const char*) { ((Seen*)u)->logs++; }
void CIMSUE_CALL onReg(void* u, const cimsue_reg_info_t* r) { ((Seen*)u)->regReasons.push_back(r->reason ? r->reason : ""); }
void CIMSUE_CALL onStopped(void* u) { ((Seen*)u)->stopped++; }
}  // namespace

TEST(CApi, EngineLifecycleHeadless) {
    cimsue_engine_t* e = cimsue_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(cimsue_engine_running(e), 0);

    // 미기동 상태 명령 — C++ 의 Result::fail(-1, "not running") 이 그대로 코드·사유로 온다
    EXPECT_EQ(cimsue_engine_hangup(e, 0), -1);
    EXPECT_STREQ(cimsue_last_error(), "not running");
    EXPECT_EQ(cimsue_engine_dial(e, 0, "1000", nullptr), -1);

    Seen seen;
    cimsue_listener_t l{};                                   // 나머지 콜백 NULL = 무시
    l.user = &seen; l.on_log = onLog; l.on_reg_state = onReg; l.on_engine_stopped = onStopped;
    cimsue_engine_config_t cfg;
    cimsue_engine_config_default(&cfg);
    EXPECT_EQ(cfg.user_agent, nullptr);
    EXPECT_EQ(cfg.clock_rate, EngineConfig().clockRate);
    cfg.log_level = 3; cfg.null_audio_device = 1;
    ASSERT_EQ(cimsue_engine_start(e, &cfg, &l), CIMSUE_OK) << cimsue_last_error();
    l = cimsue_listener_t{};                                 // 복사 규약 — 원본을 지워도 콜백은 살아 있어야 한다
    EXPECT_EQ(cimsue_engine_running(e), 1);
    EXPECT_EQ(cimsue_engine_start(e, &cfg, nullptr), -1);   // already running
    EXPECT_GT(seen.logs, 0);

    // 계정 — 완성되지 않은 설정은 -1, 완성된 설정은 id 발급 + 조회 스냅샷
    cimsue_account_config_t ac;
    cimsue_account_config_default(&ac);
    EXPECT_EQ(cimsue_engine_add_account(e, &ac), -1);
    ac.server_host = "127.0.0.1"; ac.server_port = 65000; ac.domain = "ims.example.org";
    ac.msisdn = "+821300000001"; ac.imsi = "45033821300000001"; ac.ha1 = "0123456789abcdef0123456789abcdef";
    int32_t acc = cimsue_engine_add_account(e, &ac);
    ASSERT_GE(acc, 0) << cimsue_last_error();
    const int32_t* ids = nullptr;
    ASSERT_EQ(cimsue_engine_accounts(e, &ids), 1);
    EXPECT_EQ(ids[0], acc);
    cimsue_reg_info_t ri{};
    cimsue_engine_reg_info(e, acc, &ri);
    EXPECT_EQ(ri.account_id, acc);
    EXPECT_EQ(ri.state, CIMSUE_REG_UNREGISTERED);
    ASSERT_NE(ri.reason, nullptr);
    cimsue_engine_reg_info(e, 99, &ri);                      // 없는 계정 → 기본 RegInfo
    EXPECT_EQ(ri.account_id, -1);

    // 호 조회 — 없는 호는 기본 CallInfo(-1), 배열 포인터는 NULL/0
    cimsue_call_info_t ci{};
    cimsue_engine_call_info(e, 7, &ci);
    EXPECT_EQ(ci.call_id, -1);
    EXPECT_EQ(ci.source_count, 0);
    EXPECT_NE(ci.remote_uri, nullptr);
    EXPECT_EQ(cimsue_engine_calls(e, &ids), 0);
    cimsue_floor_info_t fi{};
    cimsue_engine_floor_info(e, 7, &fi);
    EXPECT_EQ(fi.state, CIMSUE_FLOOR_IDLE);
    cimsue_stream_stats_t ss{};
    cimsue_engine_stream_stats(e, 7, &ss);
    EXPECT_EQ(ss.valid, 0);
    EXPECT_EQ(cimsue_engine_set_call_route(e, 0, 99), -1);  // 없는 라우트 — C++ 시험과 같은 경로
    EXPECT_STREQ(cimsue_last_error(), "no such route");

    // 장치 — 헤드리스는 null 장치 하나(또는 0개)
    const cimsue_audio_device_info_t* devs = nullptr;
    int32_t n = cimsue_engine_audio_devices(e, &devs);
    for (int32_t i = 0; i < n; ++i) EXPECT_NE(devs[i].name, nullptr);
    EXPECT_EQ(cimsue_engine_refresh_audio_devices(e), CIMSUE_OK);

    EXPECT_EQ(cimsue_engine_remove_account(e, acc), CIMSUE_OK) << cimsue_last_error();
    cimsue_engine_stop(e);
    EXPECT_EQ(cimsue_engine_running(e), 0);
    EXPECT_EQ(seen.stopped, 1);
    cimsue_engine_destroy(e);                                // 정지 상태 파괴 — 재-stop 은 무해
    cimsue_engine_destroy(nullptr);
}

// ── CSC 핸들 — 엔드포인트 기본값·생성/파괴 (네트워크 없이) ──
TEST(CApi, CscHandle) {
    cimsue_csc_endpoint_t ep;
    cimsue_csc_endpoint_default(&ep);
    EXPECT_EQ(ep.port, CscEndpoint().port);
    EXPECT_EQ(ep.client_id, nullptr);
    ep.host = "127.0.0.1";
    cimsue_csc_t* c = cimsue_csc_create(&ep);
    ASSERT_NE(c, nullptr);
    char buf[64];
    cimsue_csc_enc("tel:+82 1", buf, sizeof buf);
    EXPECT_EQ(std::string(buf), CscClient::enc("tel:+82 1"));
    cimsue_csc_destroy(c);
    cimsue_csc_destroy(nullptr);
}
