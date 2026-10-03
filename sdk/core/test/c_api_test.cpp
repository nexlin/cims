// libcimsue 단위시험 — C API 평탄화 층 (cimsue_c.h · ue_sdk.md §6.4) (S1-UE-UNIT)
// 프로토콜은 시험하지 않는다 — 타입 변환·기본값 규약·수명 규약·콜백 전달이 C++ 표면과 1:1 인지만 본다.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
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
    EXPECT_EQ((int)CIMSUE_MC_SERVICE_MCVIDEO, (int)McService::McVideo);
    EXPECT_EQ((int)CIMSUE_TX_QUEUED, (int)TransmissionState::Queued);
    EXPECT_EQ((int)CIMSUE_RX_ENDED, (int)ReceptionState::Ended);
    EXPECT_EQ((int)CIMSUE_TXEV_REQUEST_TIMEOUT, (int)TransmissionEvent::Kind::RequestTimeout);
    EXPECT_EQ((int)CIMSUE_TXEV_OTHER, (int)TransmissionEvent::Kind::Other);
    EXPECT_EQ((int)CIMSUE_RXEV_REQUEST_TIMEOUT, (int)ReceptionEvent::Kind::RequestTimeout);
    EXPECT_EQ((int)CIMSUE_RXEV_OTHER, (int)ReceptionEvent::Kind::Other);
    EXPECT_STREQ(cimsue_mc_service_str(CIMSUE_MC_SERVICE_MCVIDEO), "mcvideo");
    EXPECT_EQ((int)CIMSUE_MC_SERVICE_MCDATA, (int)McService::McData);
    EXPECT_EQ((int)CIMSUE_SERVICE_AUTH_AUTHORIZED, (int)ServiceAuthState::Authorized);
    EXPECT_STREQ(cimsue_service_auth_state_str(CIMSUE_SERVICE_AUTH_PENDING), "pending");
    EXPECT_STREQ(cimsue_transmission_kind_str(CIMSUE_TXEV_GRANTED), toString(TransmissionEvent::Kind::Granted));
    EXPECT_STREQ(cimsue_reception_state_str(CIMSUE_RX_RECEIVING), toString(ReceptionState::Receiving));
}

// ── ABI 자기검사 — 바인딩이 대조할 sizeof 가 실제 구조체와 같고, 등록된 id 는 전부 답하며, 모르는 id 는 -1 ──
TEST(CApi, StructSizesForBindings) {
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_ENGINE_CONFIG), (int32_t)sizeof(cimsue_engine_config_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_ACCOUNT_CONFIG), (int32_t)sizeof(cimsue_account_config_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_CALL_INFO), (int32_t)sizeof(cimsue_call_info_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_FLOOR_EVENT), (int32_t)sizeof(cimsue_floor_event_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_LISTENER), (int32_t)sizeof(cimsue_listener_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_PROFILE), (int32_t)sizeof(cimsue_profile_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_TLS_PEER_EXPIRY), (int32_t)sizeof(cimsue_tls_peer_expiry_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_VIDEO_GROUP_CALL_OPTIONS), (int32_t)sizeof(cimsue_video_group_call_options_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_TRANSMISSION_EVENT), (int32_t)sizeof(cimsue_transmission_event_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_RECEPTION_EVENT), (int32_t)sizeof(cimsue_reception_event_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_TRANSMISSION_INFO), (int32_t)sizeof(cimsue_transmission_info_t));
    for (int i = 0; i < (int)CIMSUE_STRUCT_COUNT_; ++i) EXPECT_GT(cimsue_struct_size((cimsue_struct_id_t)i), 0) << "id " << i;
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_COUNT_), -1);
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
    EXPECT_EQ(c.auto_answer_mcvideo != 0, d.autoAnswerMcvideo);
    EXPECT_EQ(c.mcvideo_enabled != 0, d.mcvideoEnabled);
    EXPECT_EQ(c.mcvideo_server_uri, nullptr);
    cimsue_video_group_call_options_t vo;
    cimsue_video_group_call_options_default(&vo);
    const VideoGroupCallOptions vd;
    EXPECT_EQ(vo.max_priority, vd.maxPriority);
    EXPECT_EQ(vo.max_reception_priority, vd.maxReceptionPriority);
    EXPECT_EQ(vo.prearranged != 0, vd.prearranged);
    EXPECT_EQ(vo.session_uri, nullptr);

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

    // 단말 속성 헬퍼(types.h 자유 함수) — 코어 규칙 그대로(mcptt_management_views.md §4.1)
    cimsue_user_agent_of("CIMS-Dispatch", "0.1.0", "Windows 11", "Standard PC (Q35)", buf, sizeof buf);
    EXPECT_EQ(std::string(buf), userAgentOf("CIMS-Dispatch", "0.1.0", "Windows 11", "Standard PC (Q35)"));
    EXPECT_EQ(cimsue_user_agent_of("P", nullptr, nullptr, nullptr, buf, sizeof buf), 1);   // NULL = 빈 값
    EXPECT_EQ(std::string(buf), "P");
    cimsue_imei_urn("490154203237518", buf, sizeof buf);
    EXPECT_EQ(std::string(buf), "urn:gsma:imei:49015420-323751-0");   // 셋째 칸 = spare 0(RFC 7254 §4.2.3)
    EXPECT_EQ(cimsue_imei_urn("490154203237517", buf, sizeof buf), 0);
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
    EXPECT_EQ(cimsue_profile_phone_service(&p), v);            // voip 없음 → volte 폴백 (Profile::phoneService)
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
    EXPECT_EQ(cimsue_engine_handle_network_change(e), -1);  // 미기동 — 망 변경도 같은 오류 경로
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
    // 망 변경 — 계정이 없어도 전송 재수립은 돈다. 곧바로 한 번 더 부르면(처리 중) 접혀서 역시 성공이다.
    EXPECT_EQ(cimsue_engine_handle_network_change(e), CIMSUE_OK) << cimsue_last_error();
    EXPECT_EQ(cimsue_engine_handle_network_change(e), CIMSUE_OK) << cimsue_last_error();
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
    // MCVideo — 없는 호는 C++ 결과 코드·사유 그대로, 조회는 기본값(NULL/0 배열)
    EXPECT_EQ(ci.service, CIMSUE_MC_SERVICE_MCPTT);
    EXPECT_NE(ci.session_uri, nullptr);
    EXPECT_EQ(cimsue_engine_request_transmission(e, 7, -1), -2);
    EXPECT_STREQ(cimsue_last_error(), "no such call");
    EXPECT_EQ(cimsue_engine_accept_reception(e, 7, "tel:+82510002001", -1), -2);
    // 덧붙인 함수(구조체 배치는 그대로) — 없는 호·계정은 C++ 결과 그대로, NULL 엔진은 -1
    EXPECT_EQ(cimsue_engine_confirm_transmission(e, 7, 1), -2);
    EXPECT_EQ(cimsue_engine_request_queue_position(e, 7), -2);
    EXPECT_EQ(cimsue_engine_set_mcvideo_enabled(e, 99, 0), -2);
    EXPECT_STREQ(cimsue_last_error(), "no such account");
    EXPECT_EQ(cimsue_engine_set_tc_timers(e, 99, 2000, 0, 0, 0, 0), -2);
    EXPECT_EQ(cimsue_engine_set_mcvideo_enabled(nullptr, 0, 1), -1);
    // 서비스 인가(TS 24.379 §7.2.2) — 토큰은 계정에, 상태는 조회. 등록 전이라 보내지 않는다(인가 안 됨, code 0)
    EXPECT_EQ(cimsue_engine_set_access_token(e, 99, "t"), -2);
    EXPECT_EQ(cimsue_engine_set_access_token(e, acc, "t"), 0);
    int32_t code = -1, warn = -1, multi = -1;
    EXPECT_EQ(cimsue_engine_service_auth(e, acc, CIMSUE_MC_SERVICE_MCPTT, &code, &warn, &multi), CIMSUE_SERVICE_AUTH_UNAUTHORIZED);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(warn, 0);
    EXPECT_EQ(multi, 0);
    EXPECT_EQ(cimsue_engine_service_auth(nullptr, 0, CIMSUE_MC_SERVICE_MCDATA, nullptr, nullptr, nullptr), CIMSUE_SERVICE_AUTH_UNAUTHORIZED);
    EXPECT_EQ(cimsue_csc_get_group_excluding_members(nullptr, "t", "tel:g1", nullptr), -1);
    cimsue_transmission_info_t ti{};
    cimsue_engine_transmission_info(e, 7, &ti);
    EXPECT_EQ(ti.state, CIMSUE_TX_NO_PERMISSION);
    EXPECT_EQ(ti.transmitter_count, 0);
    EXPECT_EQ(ti.transmitters, nullptr);
    EXPECT_EQ(cimsue_engine_join_video_group_call(e, acc, "g101", nullptr), -1);   // 계정에 MCVideo PSI 가 없다
    EXPECT_EQ(cimsue_engine_affiliate_service(e, acc, "g101", 1, CIMSUE_MC_SERVICE_MCVIDEO), -1);
    cimsue_stream_stats_t ss{};
    cimsue_engine_stream_stats(e, 7, &ss);
    EXPECT_EQ(ss.valid, 0);
    cimsue_call_quality_t cq{};
    cimsue_engine_call_quality(e, 7, &cq);
    EXPECT_EQ(cq.valid, 0);
    EXPECT_NE(cq.codec, nullptr);
    EXPECT_EQ(cq.mos_cq, -1);
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

// ── 그룹 문서 평탄화 — C 입력(멤버 배열) → XML → C 산출 왕복, 새 구조체 id 등록 ──
// 0 으로 채운 구조체 = .NET 이 넘기는 기본값. 새 다섯 필드는 has_* = 0 이라 **싣지 않아야** 한다 —
//   그래야 폼에서 이 칸을 다루지 않는 Windows 관제 앱이 저장해도 콘솔이 정한 T4·최대 시간이 남는다.
TEST(CApi, GroupDocZeroFilledLeavesServerValues) {
    cimsue_group_doc_t d{};
    d.uri = "sip:g1@ptt.example.org";
    d.display_name = "g1";
    char buf[8192];
    ASSERT_GT(cimsue_group_doc_to_xml(&d, buf, sizeof buf), 0);
    const std::string x = buf;
    for (const char* tag : {"on-network-hang-timer", "on-network-maximum-duration", "on-network-allow-conference-state",
                            "max-data-size-for-SDS", "max-data-size-auto-recv"})
        EXPECT_EQ(x.find(tag), std::string::npos) << tag;

    // has_* = 1 이면 값 0 도 싣는다(미사용·무제한)
    d.has_hang_timer = 1; d.hang_timer_sec = 0;
    d.has_conference_state = 1; d.allow_conference_state = 0;
    d.has_max_sds_size = 1; d.max_sds_size = 2048;
    ASSERT_GT(cimsue_group_doc_to_xml(&d, buf, sizeof buf), 0);
    const std::string y = buf;
    EXPECT_NE(y.find("<mcpttgi:on-network-hang-timer>PT0S</mcpttgi:on-network-hang-timer>"), std::string::npos);
    EXPECT_NE(y.find("<mcpttgi:on-network-allow-conference-state>false"), std::string::npos);
    EXPECT_NE(y.find(">2048</mcpttgi:mcdata-on-network-max-data-size-for-SDS>"), std::string::npos);

    // 파싱 결과는 has_* 로 존재를 알린다
    cimsue_group_doc_t out{};
    ASSERT_EQ(cimsue_group_doc_parse(y.c_str(), &out), CIMSUE_OK);
    EXPECT_EQ(out.has_hang_timer, 1); EXPECT_EQ(out.hang_timer_sec, 0);
    EXPECT_EQ(out.has_conference_state, 1); EXPECT_EQ(out.allow_conference_state, 0);
    EXPECT_EQ(out.has_max_sds_size, 1); EXPECT_EQ(out.max_sds_size, 2048);
    EXPECT_EQ(out.has_max_duration, 0);                        // 싣지 않은 것은 없다고 알린다
    EXPECT_EQ(out.has_max_auto_recv, 0);
}

TEST(CApi, GroupDocRoundTripAndAbi) {
    cimsue_group_member_t mem[2] = {{"tel:+82510001001", "관제1석", "chair", 7}, {"tel:+82510001002", nullptr, nullptr, 5}};
    cimsue_group_doc_t d{};
    d.uri = "sip:g002@ptt.example.org"; d.display_name = "음성그룹2"; d.members = mem; d.member_count = 2;
    d.session_type = nullptr;                                  // NULL = prearranged
    d.allow_sds = 1; d.emergency_call = 1; d.emergency_alert = 1; d.require_affiliation = 1; d.priority = 5; d.max_participants = 12;
    int32_t need = cimsue_group_doc_to_xml(&d, nullptr, 0);
    ASSERT_GT(need, 0);
    std::string xml(need + 1, '\0');
    cimsue_group_doc_to_xml(&d, &xml[0], need + 1);
    xml.resize(need);
    EXPECT_NE(xml.find("<mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>"), std::string::npos);
    EXPECT_EQ(xml.find("session-type"), std::string::npos);    // 그룹 종류는 규격 요소로만(TS 24.481 §7.2.2 a)
    EXPECT_NE(xml.find("<entry uri=\"tel:+82510001002\">"), std::string::npos);

    cimsue_group_doc_t out{};
    ASSERT_EQ(cimsue_group_doc_parse(xml.c_str(), &out), CIMSUE_OK) << cimsue_last_error();
    EXPECT_STREQ(out.uri, "sip:g002@ptt.example.org");
    EXPECT_STREQ(out.display_name, "음성그룹2");
    ASSERT_EQ(out.member_count, 2);
    EXPECT_STREQ(out.members[0].role, "chair"); EXPECT_EQ(out.members[0].priority, 7);
    EXPECT_STREQ(out.members[1].role, "participant"); EXPECT_STREQ(out.members[1].display_name, "");
    EXPECT_EQ(out.max_participants, 12); EXPECT_EQ(out.allow_sds, 1); EXPECT_EQ(out.allow_fd, 0);
    EXPECT_STREQ(out.session_type, "prearranged");
    EXPECT_NE(cimsue_group_doc_parse("<other/>", &out), CIMSUE_OK);
    EXPECT_EQ(out.member_count, 0);

    // 새 구조체는 ABI 표에 등록돼 있어야 한다(바인딩 단위시험이 대조)
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_GROUP_DOC), (int32_t)sizeof(cimsue_group_doc_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_GROUP_MEMBER), (int32_t)sizeof(cimsue_group_member_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_DISPATCH_MEMBER), (int32_t)sizeof(cimsue_dispatch_member_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_DISPATCH_TARGET), (int32_t)sizeof(cimsue_dispatch_target_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_FD_FILE), (int32_t)sizeof(cimsue_fd_file_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_FD_UPLOAD), (int32_t)sizeof(cimsue_fd_upload_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_QUALITY_DIRECTION), (int32_t)sizeof(cimsue_quality_direction_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_CALL_QUALITY), (int32_t)sizeof(cimsue_call_quality_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_COUNT_), -1);

    // 프로파일 dispatch 확장 평탄화
    cimsue_profile_t p{};
    ASSERT_EQ(cimsue_csc_parse_profile(R"({"services":[],"ptt":{"allowCreateGroup":true},"dispatch":{"groupId":"dg-1",
        "members":[{"userId":1,"name":"A","volteAor":"tel:+8231","extension":"1001"}],"pttTargets":[{"id":"g1","uri":"sip:g1@d","name":"G1"}]}})", &p), CIMSUE_OK);
    EXPECT_EQ(p.allow_group_creation, 1);
    ASSERT_EQ(p.dispatch.member_count, 1); EXPECT_STREQ(p.dispatch.members[0].extension, "1001");
    ASSERT_EQ(p.dispatch.ptt_target_count, 1); EXPECT_STREQ(p.dispatch.ptt_targets[0].name, "G1");
}

// ── MCVideo 설정 문서(C7) — 계약 K2 골든을 C 표면으로 읽는다(값 = mcvideo_config_test 와 같다). 그룹 문서 MCVideo 몫은
// present = 0(0 으로 채운 .NET 기본값)이면 PUT 에 싣지 않는다 — MCVideo 를 모르는 앱이 저장해도 서버의 MCVideo 설정이 남는다(전환기).
static std::string mcvFixture(const char* name) {
    std::ifstream f(std::string(CIMS_SOURCE_ROOT) + "/tests/fixtures/mcvideo/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST(CApi, McVideoGroupDoc) {
    const std::string xml = mcvFixture("group_g101.xml");
    ASSERT_FALSE(xml.empty());
    cimsue_group_doc_t d{};
    ASSERT_EQ(cimsue_group_doc_parse(xml.c_str(), &d), CIMSUE_OK) << cimsue_last_error();
    const cimsue_mcvideo_group_attrs_t& v = d.mcvideo;
    EXPECT_EQ(v.present, 1);
    EXPECT_EQ(v.invite_members, 0);                            // chat(D5)
    EXPECT_EQ(v.max_duration_sec, 1800);
    EXPECT_EQ(v.protect_media, 0);
    EXPECT_EQ(v.protect_transmission_control, 0);
    ASSERT_EQ(v.audio_encoding_count, 1); EXPECT_STREQ(v.audio_encodings[0], "AMR-WB");
    ASSERT_EQ(v.video_encoding_count, 1); EXPECT_STREQ(v.video_encodings[0], "H264");
    EXPECT_STREQ(v.video_resolutions, "1280x720,640x480");
    EXPECT_STREQ(v.video_frame_rate, "30,15");
    EXPECT_EQ(v.non_urgent_real_time_video_mode, 1);
    EXPECT_EQ(v.urgent_real_time_video_mode, -1);
    EXPECT_STREQ(v.active_real_time_video_mode, "non-urgent-real-time");
    EXPECT_EQ(v.max_transmitters, 2);
    EXPECT_EQ(v.group_priority, 100);
    EXPECT_EQ(v.reception_hang_timer_sec, 30);
    EXPECT_EQ(v.allow_conference_state, 1);
    EXPECT_EQ(v.allow_emergency_call, 0);
    ASSERT_EQ(d.member_count, 3);
    for (int i = 0; i < d.member_count; ++i) EXPECT_STREQ(d.members[i].mcvideo_id, d.members[i].uri);
    EXPECT_EQ(d.max_duration_sec, 3600);                       // MCPTT 몫과 따로 읽힌다

    // C 입력 → XML: MCVideo 몫이 실리고 다시 읽으면 같은 값. 호출자 배열은 산출 버퍼와 따로 둔다(parse 가 산출을 덮는다).
    std::vector<cimsue_group_member_t> mem(d.members, d.members + d.member_count);
    std::vector<std::string> keep;
    for (auto& m : mem) { keep.push_back(m.uri); }
    for (size_t i = 0; i < mem.size(); ++i) { mem[i].uri = keep[i].c_str(); mem[i].display_name = nullptr; mem[i].role = nullptr;
                                              mem[i].title = nullptr; mem[i].mcvideo_id = nullptr; }
    const char* enc[] = {"H264"};
    cimsue_group_doc_t in{};
    in.uri = "tel:g101"; in.display_name = "g101"; in.members = mem.data(); in.member_count = (int32_t)mem.size();
    cimsue_mcvideo_group_attrs_default(&in.mcvideo);
    EXPECT_EQ(in.mcvideo.present, 0);
    EXPECT_EQ(in.mcvideo.protect_media, 1);                    // 요소가 없으면 true(TS 24.481 §7.2.8) — 기본값도 같다
    EXPECT_EQ(in.mcvideo.max_duration_sec, -1);
    in.mcvideo.present = 1;
    in.mcvideo.protect_media = 0; in.mcvideo.protect_transmission_control = 0;
    in.mcvideo.max_duration_sec = 600;
    in.mcvideo.video_encodings = enc; in.mcvideo.video_encoding_count = 1;
    in.mcvideo.reception_hang_timer_sec = 20;
    char buf[16384];
    ASSERT_GT(cimsue_group_doc_to_xml(&in, buf, sizeof buf), 0);
    const std::string x = buf;
    EXPECT_NE(x.find("<mcpttgi:mcvideo-mcvideo-id uri=\"" + keep[0] + "\"/>"), std::string::npos);   // 비우면 entry uri(D1)
    EXPECT_NE(x.find("<mcpttgi:mcvideo-protect-media>false</mcpttgi:mcvideo-protect-media>"), std::string::npos);
    EXPECT_NE(x.find("<mcpttgi:encoding name=\"H264\"/>"), std::string::npos);
    EXPECT_NE(x.find("enabler=\"urn:urn-7:3gpp-service.ims.icsi.mcvideo\""), std::string::npos);
    cimsue_group_doc_t back{};
    ASSERT_EQ(cimsue_group_doc_parse(x.c_str(), &back), CIMSUE_OK) << cimsue_last_error();
    EXPECT_EQ(back.mcvideo.present, 1);
    EXPECT_EQ(back.mcvideo.max_duration_sec, 600);
    EXPECT_EQ(back.mcvideo.reception_hang_timer_sec, 20);
    EXPECT_EQ(back.mcvideo.protect_media, 0);

    // 0 으로 채운 구조체(.NET 기본값) — MCVideo 를 싣지 않는다
    cimsue_group_doc_t zero{};
    zero.uri = "tel:g101"; zero.display_name = "g101"; zero.members = mem.data(); zero.member_count = (int32_t)mem.size();
    ASSERT_GT(cimsue_group_doc_to_xml(&zero, buf, sizeof buf), 0);
    EXPECT_EQ(std::string(buf).find("mcvideo"), std::string::npos);
    ASSERT_EQ(cimsue_group_doc_parse(mcvFixture("group_g102_mcptt_only.xml").c_str(), &back), CIMSUE_OK);
    EXPECT_EQ(back.mcvideo.present, 0);
    EXPECT_EQ(back.mcvideo.protect_media, 1);                  // 없는 몫은 기본값
}

TEST(CApi, McVideoProfileAndServiceConfig) {
    cimsue_mcvideo_user_profile_doc_t up{};
    ASSERT_EQ(cimsue_mcvideo_user_profile_parse(mcvFixture("mcvideo_user_profile.xml").c_str(), &up), CIMSUE_OK) << cimsue_last_error();
    EXPECT_STREQ(up.user_uri, "tel:+82510002001");
    EXPECT_STREQ(up.mcvideo_id, "tel:+82510002001");
    ASSERT_EQ(up.group_count, 1); EXPECT_STREQ(up.groups[0], "tel:g101");
    EXPECT_EQ(up.max_simultaneous_video_streams, 1);
    EXPECT_EQ(up.max_simultaneous_calls_n6, 1);
    EXPECT_EQ(up.max_affiliations_n2, 4); // MCVideo N2 — 회선 값(기본 4)
    EXPECT_STREQ(up.emergency_group.uri, "tel:g101");
    EXPECT_STREQ(up.emergency_group.mode, "UseCurrentlySelectedGroup");
    EXPECT_EQ(up.allow_revoke_transmit, 0);
    EXPECT_EQ(up.allow_adhoc_group_call, 0);
    EXPECT_NE(cimsue_mcvideo_user_profile_parse("<mcptt-user-profile/>", &up), CIMSUE_OK);
    EXPECT_EQ(up.group_count, 0);
    ASSERT_EQ(cimsue_mcvideo_user_profile_parse("<mcvideo-user-profile XUI-URI=\"tel:1\"/>", &up), CIMSUE_OK);
    EXPECT_EQ(up.allow_revoke_transmit, 0);                   // 요소가 없으면 false(TS 24.484 표 9.3.2.7)
    EXPECT_EQ(up.max_simultaneous_video_streams, -1);

    cimsue_mcvideo_service_config_doc_t sc{};
    ASSERT_EQ(cimsue_mcvideo_service_config_parse(mcvFixture("mcvideo_service_config.xml").c_str(), &sc), CIMSUE_OK);
    EXPECT_STREQ(sc.domain, "ptt.cims.example.kr");
    EXPECT_STREQ(sc.rp_emergency, "mcpttp.15");
    EXPECT_EQ(sc.confidentiality_protection, 0);
    EXPECT_EQ(sc.integrity_protection, 0);
    EXPECT_EQ(sc.t100_sec, 1); EXPECT_EQ(sc.t104_sec, 1);
    ASSERT_EQ(cimsue_mcvideo_service_config_parse(
        "<service-configuration-info><service-configuration-params domain=\"d\"><on-network/></service-configuration-params>"
        "</service-configuration-info>", &sc), CIMSUE_OK);
    EXPECT_EQ(sc.confidentiality_protection, 1);               // 없으면 켜진 것(TS 24.281 §6.6.2.1)
    EXPECT_EQ(sc.t100_sec, -1);

    EXPECT_EQ(cimsue_csc_fetch_mcvideo_user_profile(nullptr, "tok", "tel:1", nullptr, &up), -1);
    EXPECT_EQ(cimsue_csc_fetch_mcvideo_service_config(nullptr, "tok", nullptr, &sc), -1);

    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_MCVIDEO_GROUP_ATTRS), (int32_t)sizeof(cimsue_mcvideo_group_attrs_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_MCVIDEO_USER_PROFILE_DOC), (int32_t)sizeof(cimsue_mcvideo_user_profile_doc_t));
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_MCVIDEO_SERVICE_CONFIG_DOC), (int32_t)sizeof(cimsue_mcvideo_service_config_doc_t));
}

// ── 서버 인증서 만료 관측 — 관측 전에는 valid=0·days_left=0, NULL 핸들도 안전. C++ daysLeft 규약과 같다 ──
TEST(CApi, TlsPeerExpiryDefaults) {
    cimsue_tls_peer_expiry_t t;
    cimsue_engine_tls_peer_expiry(nullptr, &t);
    EXPECT_EQ(t.valid, 0);
    EXPECT_EQ(t.days_left, 0);
    EXPECT_EQ(t.not_after_epoch, 0);
    EXPECT_STREQ(t.subject, "");
    cimsue_csc_tls_peer_expiry(nullptr, &t);
    EXPECT_EQ(t.valid, 0);
    TlsPeerExpiry e;
    EXPECT_EQ(e.daysLeft(1000), 0);                          // valid 아니면 0
    e.valid = true; e.notAfterEpoch = 1000 + 86400 * 45;
    EXPECT_EQ(e.daysLeft(1000), 45);
    e.notAfterEpoch = 1000 - 86400 * 2;
    EXPECT_EQ(e.daysLeft(1000), -2);                          // 만료 = 음수
}

// ── P0b 노출분 — 설정 기본값·to_account 가 새 필드를 옮기는지, CMS 해석·게이트가 C++ 과 같은지, 프로파일 능력·MCData 상한 ──
TEST(CApi, McpttFieldsAndCmsDocs) {
    const EngineConfig de;
    cimsue_engine_config_t ec;
    cimsue_engine_config_default(&ec);
    EXPECT_EQ(ec.grant_mic_delay_ms, de.grantMicDelayMs);
    EXPECT_EQ(ec.udp_no_tcp_switch != 0, de.udpNoTcpSwitch);
    const AccountConfig da;
    cimsue_account_config_t ac;
    cimsue_account_config_default(&ac);
    EXPECT_EQ(ac.max_sds_cplane_bytes, da.maxSdsCplaneBytes);
    EXPECT_EQ(ac.rp_emergency, nullptr);                        // NULL → C++ 기본(mcpttp.15)

    // 서버 산출 모양 — PTT 서비스의 mcdata 블록 · capabilities.smsGateway
    const char* prof = R"({"user":{"loginId":"d1"},"services":[
        {"kind":"voip","capabilities":{"smsGateway":true},"sip":{"host":"h","port":5061,"transport":"TLS","domain":"ims.example.org","udpNoTcpSwitch":true},
         "account":{"msisdn":"+821310001001","imsi":"1","sipHa1":"0123456789abcdef0123456789abcdef"}},
        {"kind":"ptt","capabilities":{"smsGateway":false},"sip":{"host":"h","port":5060,"domain":"ptt.example.org"},
         "account":{"msisdn":"+82500000001","imsi":"2","sipHa1":"0123456789abcdef0123456789abcdef"},
         "mcdata":{"maxPayloadSdsCplaneBytes":1500}}]})";
    cimsue_profile_t p{};
    ASSERT_EQ(cimsue_csc_parse_profile(prof, &p), CIMSUE_OK) << cimsue_last_error();
    const cimsue_service_profile_t* v = cimsue_profile_service(&p, "voip");
    const cimsue_service_profile_t* t = cimsue_profile_service(&p, "ptt");
    ASSERT_NE(v, nullptr); ASSERT_NE(t, nullptr);
    EXPECT_EQ(v->sms_gateway, 1);
    EXPECT_EQ(v->udp_no_tcp_switch, 1);
    EXPECT_EQ(t->sms_gateway, 0);
    EXPECT_EQ(t->max_payload_sds_cplane_bytes, 1500);
    cimsue_account_config_t a{};
    cimsue_service_profile_to_account(t, nullptr, &a);
    EXPECT_EQ(a.max_sds_cplane_bytes, 1500);                    // toAccount 가 옮긴다 — 넘는 그룹 SDS 는 MSRP
    EXPECT_STREQ(a.rp_emergency, "mcpttp.15");

    // CMS — 규격 요소 · 요소 없음 = false(TS 24.484 표 8.3.2.7) · 게이트는 Capabilities::of
    cimsue_user_profile_doc_t up{};
    ASSERT_EQ(cimsue_user_profile_parse(
                  "<mcptt-user-profile XUI-URI=\"tel:+82500000001\"><ruleset><actions>"
                  "<allow-activate-emergency-alert>true</allow-activate-emergency-alert>"
                  "<allow-cancel-emergency-alert>false</allow-cancel-emergency-alert>"
                  "<allow-cancel-group-emergency>true</allow-cancel-group-emergency>"
                  "<allow-cancel-imminent-peril>false</allow-cancel-imminent-peril></actions></ruleset>"
                  "<OnNetwork><MCPTTGroupInfo><entry><uri-entry>sip:g1@ptt</uri-entry></entry></MCPTTGroupInfo></OnNetwork>"
                  "</mcptt-user-profile>", &up), CIMSUE_OK) << cimsue_last_error();
    EXPECT_STREQ(up.user_uri, "tel:+82500000001");
    ASSERT_EQ(up.group_count, 1);
    EXPECT_STREQ(up.groups[0], "sip:g1@ptt");
    EXPECT_EQ(up.allow_cancel_emergency_alert, 0);
    EXPECT_EQ(up.allow_activate_emergency_alert, 1);
    EXPECT_EQ(up.allow_cancel_group_emergency, 1);
    EXPECT_EQ(up.allow_cancel_imminent_peril, 0);
    EXPECT_EQ(up.max_affiliations_n2, -1);
    cimsue_capabilities_t k{};
    cimsue_capabilities_of(&up, nullptr, &k);
    EXPECT_EQ(k.user_profile_known, 1);
    EXPECT_EQ(k.service_config_known, 0);
    EXPECT_EQ(k.cancel_emergency_alert, 0);
    EXPECT_EQ(k.emergency_alert, 1);
    EXPECT_EQ(k.cancel_group_emergency, 1);
    EXPECT_EQ(k.cancel_imminent_peril, 0);
    cimsue_capabilities_of(nullptr, nullptr, &k);
    EXPECT_EQ(k.user_profile_known, 0);
    EXPECT_EQ(k.cancel_emergency_alert, 1);                     // 못 받은 문서는 허용
    EXPECT_EQ(k.cancel_imminent_peril, 1);
    EXPECT_NE(cimsue_user_profile_parse("<group/>", &up), CIMSUE_OK);

    cimsue_service_config_doc_t sc{};
    ASSERT_EQ(cimsue_service_config_parse(
                  "<service-configuration-info><service-configuration-params domain=\"ptt.example.org\"><on-network>"
                  "<emergency-resource-priority><resource-priority-namespace>mcpttp</resource-priority-namespace>"
                  "<resource-priority-priority>14</resource-priority-priority></emergency-resource-priority>"
                  "</on-network></service-configuration-params></service-configuration-info>", &sc), CIMSUE_OK) << cimsue_last_error();
    EXPECT_STREQ(sc.domain, "ptt.example.org");
    EXPECT_STREQ(sc.rp_emergency, "mcpttp.14");
    EXPECT_STREQ(sc.rp_normal, "");

    // UE initial configuration(TS 24.484 §7.2) — 참여 기능 PSI → 계정 mcptt_server_uri·mcdata_server_uri
    cimsue_ue_init_config_doc_t ui{};
    ASSERT_EQ(cimsue_ue_init_config_parse(
                  "<mcptt-UE-initial-configuration domain=\"ptt.example.org\"><on-network><anyExt>"
                  "<MCPTT-Service-Details><Server-URI>sip:mcptt_psi@ptt.example.org</Server-URI></MCPTT-Service-Details>"
                  "<MCData-Service-Details><Server-URI>sip:mcdata_psi@ptt.example.org</Server-URI></MCData-Service-Details>"
                  "</anyExt></on-network></mcptt-UE-initial-configuration>", &ui), CIMSUE_OK) << cimsue_last_error();
    EXPECT_STREQ(ui.domain, "ptt.example.org");
    EXPECT_STREQ(ui.mcptt_server_uri, "sip:mcptt_psi@ptt.example.org");
    EXPECT_STREQ(ui.mcdata_server_uri, "sip:mcdata_psi@ptt.example.org");
    EXPECT_NE(cimsue_ue_init_config_parse("<mcptt-user-profile/>", &ui), CIMSUE_OK);
    EXPECT_EQ(cimsue_struct_size(CIMSUE_STRUCT_UE_INIT_CONFIG_DOC), (int32_t)sizeof(cimsue_ue_init_config_doc_t));
    EXPECT_STREQ(cimsue_condition_cause_str(CIMSUE_COND_DENIED), toString(ConditionCause::Denied));
    EXPECT_EQ((int)CIMSUE_COND_ADVERTISED, (int)ConditionCause::Advertised);
    EXPECT_EQ((int)CIMSUE_ROUTE_LOUDSPEAKER, (int)AudioRoute::Loudspeaker);
    EXPECT_EQ(CIMSUE_MIC_AGC_TARGET_DBOV, kMicAgcTargetDbov);
}
