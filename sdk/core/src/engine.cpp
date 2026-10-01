// libcimsue — Engine 구현 (ue_sdk.md §4.3 스레딩·수명 규칙)
//
//  - 모든 pjsua2 호출은 제어 스레드 `ue-ctl` 에서만 한다. libCreate 도 이 스레드에서 하므로 pjlib 의
//    "메인 스레드" 가 곧 ue-ctl 이다. 공개 명령은 runSync 로 ue-ctl 에 넘기고 결과를 받아 돌려준다.
//  - pjsua 콜백(pjsip 워커 스레드)은 상태 스냅샷을 갱신하고 이벤트를 큐에 넣기만 한다. 리스너는
//    이벤트 스레드 `ue-evt` 가 부른다 — 리스너 안에서 명령을 다시 불러도(ue-ctl 로 감) 교착 없음.
//  - pj::Account/pj::Call 은 엔진이 강참조 테이블로 보관하고 DISCONNECTED 뒤 ue-ctl 에서 해제한다.
//    콜백 안에서 자기 객체를 지우지 않는다.
//  - MCPTT 세션(그룹콜·사설콜)은 호마다 floor participant(별도 UDP 소켓)를 갖고, SDP 의 m=application 을
//    송신 SDP 에 주입·수신 SDP 에서 학습한다(android SipController/CimsCall 의 규칙 승계).
//  - MCVideo 그룹 호(TS 24.281)는 MCPTT 세션과 독립 다이얼로그다 — 호마다 전송 제어 participant(mcvideo/tc_participant)와
//    `m=application <port> udp MCVideo` 제어 채널을 갖는다. 송출(마이크·카메라)은 송출 허가('U: has permission')에서만 연다.
#include "cimsue/engine.h"

#include <pjsua2.hpp>
#include <pjsua-lib/pjsua_internal.h>                    // 호 다이얼로그(pjsua_var.calls) — setDialogContactParams, setAudioTx

#include <algorithm>
#include <cctype>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#if defined(__ANDROID__)
#include <android/native_window.h>                        // setVideoWindow 의 창 참조(ANativeWindow)
#endif

#include "account_map.h"
#include "floor/floor_participant.h"
#include "mcdata/msrp.h"
#include "mcdata/sds_codec.h"
#include "mcptt/mcptt_xml.h"
#include "mcvideo/mcvideo_sip.h"
#include "mcvideo/tc_participant.h"
#include "quality/call_quality.h"
#include "reg_recovery.h"

// 창 없는 프레임 렌더(Windows 엔진 — config_site PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME): 디코드 프레임(BGRA)이 렌더 장치 콜백으로 와서
//   Listener::onVideoFrame 으로 나간다(ue_sdk.md §4.5). 수신 창 = 그 호를 가리키는 토큰, 셀프뷰 = 미리보기 창 토큰.
#if PJSUA_HAS_VIDEO && defined(PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME) && PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME
#define CIMSUE_FRAME_SINK 1
#include <pjmedia-videodev/cims_frame_dev.h>
#else
#define CIMSUE_FRAME_SINK 0
#endif

#define CIMSUE_VERSION "0.2.0"

namespace cimsue {

namespace {

/** 단일 워커 스레드 + 작업 큐. */
class Worker {
public:
    void start() {
        stop_ = false;
        th_ = std::thread([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                    if (stop_ && q_.empty()) return;
                    job = std::move(q_.front());
                    q_.pop_front();
                }
                try { job(); } catch (...) {}
            }
        });
    }
    void post(std::function<void()> fn) {
        { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(fn)); }
        cv_.notify_one();
    }
    template <typename F>
    auto runSync(F&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        if (std::this_thread::get_id() == th_.get_id()) return fn();   // 재진입 — 직접 실행
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto fut = task->get_future();
        post([task] { (*task)(); });
        return fut.get();
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

private:
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
};

Result fromError(const pj::Error& e) { return Result::fail((int)e.status, e.info(false)); }

/** SDP 에서 m=application 의 (ip, port). 섹션 c= 우선, 없으면 세션 c=. */
bool parseApplication(const std::string& sdp, std::string& ip, int& port) {
    size_t m = sdp.find("m=application ");
    if (m == std::string::npos) return false;
    port = std::atoi(sdp.c_str() + m + 14);
    if (port <= 0) return false;
    size_t next = sdp.find("\nm=", m + 1);
    std::string section = sdp.substr(m, next == std::string::npos ? std::string::npos : next - m);
    auto conn = [](const std::string& s) -> std::string {
        size_t c = s.find("c=IN IP4 ");
        if (c == std::string::npos) return std::string();
        size_t e = s.find_first_of("\r\n", c);
        return s.substr(c + 9, e == std::string::npos ? std::string::npos : e - c - 9);
    };
    ip = conn(section);
    if (ip.empty()) ip = conn(sdp);
    return !ip.empty();
}

/** 주입 섹션에 c= 라인 보장 — pjmedia_sdp_validate EMISSINGCONN 방지. */
std::string withConnLine(const std::string& whole, const std::string& extra) {
    std::string section = extra;
    while (!section.empty() && (section.back() == '\r' || section.back() == '\n')) section.pop_back();
    if (section.find("c=IN ") != std::string::npos) return section;
    size_t c = whole.find("c=IN IP4 ");
    if (c == std::string::npos) return section;
    size_t e = whole.find_first_of("\r\n", c);
    std::string cline = whole.substr(c, e == std::string::npos ? std::string::npos : e - c);
    size_t nl = section.find("\r\n");
    if (nl == std::string::npos) return section + "\r\n" + cline;
    return section.substr(0, nl + 2) + cline + section.substr(nl);
}

/** [whole] 의 [prefix] 미디어 섹션(다음 m= 전까지)을 [extra] 로 교체 — media_count 불변(med_prov_cnt 정합).
 *  prefix 가 없으면 끝에 덧붙인다. */
std::string replaceMediaSection(const std::string& whole, const std::string& prefix, const std::string& extra) {
    size_t s = whole.find(prefix);
    std::string body = withConnLine(whole, extra) + "\r\n";
    if (s == std::string::npos) {
        std::string w = whole;
        while (!w.empty() && (w.back() == '\r' || w.back() == '\n')) w.pop_back();
        return w + "\r\n" + body;
    }
    size_t e = whole.find("\nm=", s + 1);
    if (e == std::string::npos) return whole.substr(0, s) + body;
    return whole.substr(0, s) + body + whole.substr(e + 1);
}

uint32_t ssrcOf(const std::string& id) {
    uint32_t v = (uint32_t)(std::hash<std::string>{}(id) & 0xffffffffu);
    return v ? v : 1;
}

std::string sipBody(const std::string& whole) {
    size_t p = whole.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : whole.substr(p + 4);
}

/**
 * 다이얼로그 로컬 Contact 의 헤더 파라미터를 바꾼다 — URI 는 pjsua 가 고른 그대로(계정 Contact · NAT 재작성), 파라미터 = params.
 *
 * 서비스 호는 Contact 에 자기 서비스 특성 태그를 싣는다(MCVideo 개시 INVITE §9.2.1.2.1.1 · 180/200 §6.2.3.1.1 3)·4)). pjsua 에 호별 Contact
 * 파라미터 API 가 없어(msg_data.contact_uri 는 URI 만 받는다 — pjsip_parse_uri) 다이얼로그를 직접 고친다. 이 다이얼로그의 모든 요청·
 * 응답(개시 INVITE·180·200·re-INVITE·BYE)이 이 Contact 를 싣는다. 부르는 곳 = 개시 INVITE 를 만들기 전(UAC onCallSdpCreated —
 * pjsua_call_make_call 은 다이얼로그를 만든 뒤 offer 를 만들고 그다음 INVITE 를 만든다) · 180 전(UAS onIncomingCall). pjsua 콜백 안이라
 * 호·다이얼로그 락을 이미 잡고 있다. 공개 API 에 호의 다이얼로그 조회가 없어 pjsua 내부 표(`pjsua_var`)를 읽는다 — ext/pjproject 는
 * 모든 플랫폼이 같은 트리다(ue_sdk.md §6).
 */
bool setDialogContactParams(int callId, const std::string& params) {
    if (callId < 0 || callId >= (int)PJSUA_MAX_CALLS) return false;
    pjsua_call* call = &pjsua_var.calls[callId];
    pjsip_dialog* dlg = call->inv ? call->inv->dlg : call->async_call.dlg;
    if (!dlg || !dlg->local.contact) return false;
    char buf[PJSIP_MAX_URL_SIZE + 64];
    int n = pjsip_hdr_print_on(dlg->local.contact, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    std::string v(buf, (size_t)n);
    const size_t colon = v.find(':');
    v = mcvideo::contactUriPart(colon == std::string::npos ? v : v.substr(colon + 1)) + params;
    pj_str_t name = pj_str(const_cast<char*>("Contact")), text;
    pj_strdup2_with_null(dlg->pool, &text, v.c_str());
    auto* h = static_cast<pjsip_contact_hdr*>(pjsip_parse_hdr(dlg->pool, &name, text.ptr, (pj_size_t)text.slen, nullptr));
    if (!h) return false;
    dlg->local.contact = h;
    return true;
}

/**
 * 오디오 송신(인코더) 멈춤·재개 — 멈추면 RTP 를 내지 않는다(무음 프레임도 — noVad 면 코덱이 무음을 부호화해 보낸다). 스트림 keep-alive
 * (빈 RTP, PJMEDIA_STREAM_ENABLE_KA)와 RTCP 는 그대로 나가 NAT 매핑·CMP latch 는 유지된다.
 *
 * MCVideo 는 송출 허가 밖에서 payload 있는 미디어를 보내면 제어 기능이 버리고 회수(#3)를 되풀이한다(TS 24.581 §6.3.5.3.8 — mcvideo.md
 * §5.3.1). 마이크를 브리지에서 떼는 것만으로는 무음 프레임이 나가므로 인코더를 멈춘다. 재협상으로 스트림이 새로 생기면(새 스트림은
 * 멈춤이 풀려 있다) wireMedia 가 다시 건다. 공개 API 에 스트림 멈춤이 없어 pjsua 내부 표를 pjsua 락 아래서 읽는다.
 */
void setAudioTx(int callId, bool on) {
    if (callId < 0 || callId >= (int)PJSUA_MAX_CALLS) return;
    PJSUA_LOCK();
    pjsua_call* call = &pjsua_var.calls[callId];
    for (unsigned i = 0; i < call->med_cnt; ++i) {
        pjsua_call_media* m = &call->media[i];
        if (m->type != PJMEDIA_TYPE_AUDIO || !m->strm.a.stream) continue;
        if (on) pjmedia_stream_resume(m->strm.a.stream, PJMEDIA_DIR_ENCODING);
        else pjmedia_stream_pause(m->strm.a.stream, PJMEDIA_DIR_ENCODING);
    }
    PJSUA_UNLOCK();
}

/**
 * 송신 직전 SDP 보정 모듈 — MCVideo SDP 의 m=audio·m=video 에 `i=` 성분 표시를 넣는다(TS 24.281 §6.2.1 2)c)·3)d)·§6.2.2 3)c)·4)c)).
 * MCPTT·MCVideo SDP 를 실은 INVITE 2xx 에는 `Require: timer` 도 채우고(requireTimer — 두 규격 §6.2.3.1.1 2)), 다이얼로그 안 offer 는
 * 개시 전용 fmtp 를 뺀다(mcvideo::forSubsequentOffer · mcptt::forSubsequentOffer — TS 24.581·24.380 §14.5).
 *
 * pjmedia SDP 는 미디어 수준 `i=` 를 담지 못해(파서가 버린다) onCallSdpCreated 로는 넣을 수 없다. 메시지 인쇄 모듈(mod-msg-print,
 * PJSIP_MOD_PRIORITY_TRANSPORT_LAYER) 바로 앞에서 본문 인쇄본(단일 SDP 또는 multipart 텍스트 전체)을 고쳐 같은 Content-Type(boundary
 * 포함)의 텍스트 본문으로 바꾸고 인쇄본을 무효로 한다 — multipart 파트 API 는 pjsip multipart 객체만 받아(텍스트 multipart 는 assert)
 * 쓰지 않는다. 재송신·인증 재전송은 이미 고친 본문이라 그대로 지난다. 판별은 본문(제어 채널 `udp MCVideo` m-line)으로 한다 — 호 표를
 * 보지 않으므로 pjsip 워커 스레드에서도 안전하다.
 */
/** INVITE 2xx 에 `Require: timer`(TS 24.379·24.281 §6.2.3.1.1 2)) — pjsip 은 UAS 가 갱신자면(mcRxFix) Require 를 싣지 않는다(RFC 4028 §9
 *  는 refresher=uac 일 때만 요구). 이미 timer 가 있으면 그대로. */
void requireTimer(pjsip_tx_data* tdata) {
    pjsip_msg* msg = tdata->msg;
    if (msg->type != PJSIP_RESPONSE_MSG || msg->line.status.code / 100 != 2) return;
    auto* cseq = static_cast<pjsip_cseq_hdr*>(pjsip_msg_find_hdr(msg, PJSIP_H_CSEQ, nullptr));
    if (!cseq || cseq->method.id != PJSIP_INVITE_METHOD) return;
    static const pj_str_t timer = {const_cast<char*>("timer"), 5};
    // 형식화된 Require 와 일반 헤더 둘 다 본다 — 180 에 txOption 으로 넣은 `Require: timer` 는 일반 헤더이고, pjsip 은 같은 응답 객체를
    //   200 으로 바꿔 쓰므로(pjsip_inv_answer 가 last_answer 재사용) 그대로 남아 있다
    for (pjsip_hdr* h = msg->hdr.next; h != &msg->hdr; h = h->next) {
        if (pj_stricmp2(&h->name, "Require") != 0) continue;
        if (h->type == PJSIP_H_REQUIRE) {
            auto* r = reinterpret_cast<pjsip_require_hdr*>(h);
            for (unsigned i = 0; i < r->count; ++i)
                if (pj_stricmp(&r->values[i], &timer) == 0) return;
        } else {
            auto* g = reinterpret_cast<pjsip_generic_string_hdr*>(h);
            if (pj_strstr(&g->hvalue, &timer)) return;
        }
    }
    pjsip_require_hdr* r = pjsip_require_hdr_create(tdata->pool);
    r->count = 1;
    r->values[0] = timer;
    pjsip_msg_add_hdr(msg, reinterpret_cast<pjsip_hdr*>(r));
    pjsip_tx_data_invalidate_msg(tdata);
}

pj_status_t mcTxFix(pjsip_tx_data* tdata) {
    pjsip_msg_body* body = tdata && tdata->msg ? tdata->msg->body : nullptr;
    if (!body || !body->print_body) return PJ_SUCCESS;
    const bool sdp = pj_stricmp2(&body->content_type.type, "application") == 0 && pj_stricmp2(&body->content_type.subtype, "sdp") == 0;
    if (!sdp && pj_stricmp2(&body->content_type.type, "multipart") != 0) return PJ_SUCCESS;
    std::vector<char> buf(PJSIP_MAX_PKT_LEN);
    int n = body->print_body(body, buf.data(), buf.size());
    if (n <= 0) return PJ_SUCCESS;
    const std::string text(buf.data(), (size_t)n);
    const bool mcv = text.find(" MCVideo") != std::string::npos;
    if (!mcv && !mcptt::isMcpttSdp(text)) return PJ_SUCCESS;
    requireTimer(tdata);
    std::string fixed = !mcv ? text : sdp ? mcvideo::withMediaInfo(text) : mcvideo::withMediaInfoMultipart(text);   // 파트 Content-Length 도
    // 다이얼로그 안 offer(re-INVITE·UPDATE — pjsip 세션 갱신 포함)는 개시 전용 fmtp 를 뺀다(TS 24.581·24.380 §14.5). 이어지는 offer 는
    //   단일 SDP 다 — MCPTT 긴급·임박 격상 re-INVITE(mcptt-info 를 싣는 multipart)는 거치지 않는다.
    const pjsip_msg* msg = tdata->msg;
    if (sdp && msg->type == PJSIP_REQUEST_MSG) {
        const pjsip_method& m = msg->line.req.method;
        auto* to = static_cast<const pjsip_to_hdr*>(pjsip_msg_find_hdr(msg, PJSIP_H_TO, nullptr));
        const bool inDialog = to && to->tag.slen > 0;
        if (inDialog && (m.id == PJSIP_INVITE_METHOD || pj_stricmp2(&m.name, "UPDATE") == 0))
            fixed = mcv ? mcvideo::forSubsequentOffer(fixed) : mcptt::forSubsequentOffer(fixed);
    }
    if (fixed == text) return PJ_SUCCESS;
    pj_str_t t;
    pj_strdup2_with_null(tdata->pool, &t, fixed.c_str());
    pjsip_msg_body* nb = pjsip_msg_body_create(tdata->pool, &body->content_type.type, &body->content_type.subtype, &t);
    if (!nb) return PJ_SUCCESS;
    pjsip_media_type_cp(tdata->pool, &nb->content_type, &body->content_type);   // multipart boundary 파라미터
    tdata->msg->body = nb;
    pjsip_tx_data_invalidate_msg(tdata);
    return PJ_SUCCESS;
}

pjsip_module g_txFixModule = {
    nullptr, nullptr,                                   // prev, next
    {const_cast<char*>("mod-cimsue-txfix"), 16},        // name
    -1,                                                 // id
    PJSIP_MOD_PRIORITY_TRANSPORT_LAYER + 1,             // 인쇄 모듈 바로 앞(송신은 높은 값 → 낮은 값 순)
    nullptr, nullptr, nullptr, nullptr,                 // load, start, stop, unload
    nullptr, nullptr,                                   // on_rx_request, on_rx_response
    &mcTxFix, &mcTxFix,                                 // on_tx_request, on_tx_response
    nullptr,                                            // on_tsx_state
};

/**
 * 수신 보정 모듈 — 착신 MCPTT·MCVideo 최초 INVITE(제어 기능의 멤버 초대·사설 호)의 Session-Expires 에 refresher 가 없으면 `uas` 로
 * 정한다.
 *
 * 단말의 200 OK 는 refresher = uas(TS 24.379·24.281 §6.2.3.1.1 5) — 그룹 호 §6.2.3.1.2 가 따른다, TS 24.281 §9.2.2.2.1.6 10)·TS 24.379
 * §9.2.2.2.x «요청에 없으면 uas, 있으면 그 값»)이고 제어·참여 기능은 단말 초대에 refresher 를 싣지 않는다(두 규격 §6.3.3.1.2 6) ·
 * TS 24.379 §6.3.4.1.2). pjsip UAS 는 요청에 refresher 가 없고 UAC 가 timer 를 지원하면 uac 를 골라(RFC 4028 §9 의 UAS 선택 — 설정
 * 없음) pjsip 이 요청을 처리하기 전(트랜잭션 계층 앞)에 받은 헤더에 값을 넣는다. 그러면 pjsip 이 갱신자(UAS)가 되어 200 OK 에 uas
 * 를 싣고 갱신 요청을 보낸다. 요청이 refresher 를 정했으면(옛 서버의 uac) 그 값을 따른다(RFC 4028 §9 Table 2). 다이얼로그 안 요청은
 * 협상된 갱신자를 pjsip 이 지킨다. 판별은 원문의 mcptt-info·mcvideo-info 파트 — 호 표를 보지 않으므로 워커 스레드에서도 안전하다.
 */
pj_bool_t mcRxFix(pjsip_rx_data* rdata) {
    pjsip_msg* msg = rdata ? rdata->msg_info.msg : nullptr;
    if (!msg || msg->type != PJSIP_REQUEST_MSG || msg->line.req.method.id != PJSIP_INVITE_METHOD) return PJ_FALSE;
    if (rdata->msg_info.to && rdata->msg_info.to->tag.slen) return PJ_FALSE;
    if (!rdata->msg_info.msg_buf) return PJ_FALSE;
    const pj_str_t whole = {rdata->msg_info.msg_buf, (pj_ssize_t)rdata->msg_info.len};
    const pj_str_t ctv = {const_cast<char*>(mcvideo::kCtInfo), (pj_ssize_t)std::strlen(mcvideo::kCtInfo)};
    const pj_str_t ctp = {const_cast<char*>(mcptt::kCtMcpttInfo), (pj_ssize_t)std::strlen(mcptt::kCtMcpttInfo)};
    if (!pj_strstr(&whole, &ctv) && !pj_strstr(&whole, &ctp)) return PJ_FALSE;
    static const pj_str_t se = {const_cast<char*>("Session-Expires"), 15}, sx = {const_cast<char*>("x"), 1};
    auto* h = static_cast<pjsip_sess_expires_hdr*>(pjsip_msg_find_hdr_by_names(msg, &se, &sx, nullptr));
    if (h && h->refresher.slen == 0) h->refresher = pj_str(const_cast<char*>("uas"));
    return PJ_FALSE;                                                     // 다음 모듈로 — 요청은 그대로 흐른다
}

pjsip_module g_rxFixModule = {
    nullptr, nullptr,                                   // prev, next
    {const_cast<char*>("mod-cimsue-rxfix"), 16},        // name
    -1,                                                 // id
    PJSIP_MOD_PRIORITY_TSX_LAYER - 1,                   // 트랜잭션 계층 앞(수신은 낮은 값 → 높은 값 순)
    nullptr, nullptr, nullptr, nullptr,                 // load, start, stop, unload
    &mcRxFix, nullptr,                                  // on_rx_request, on_rx_response
    nullptr, nullptr,                                   // on_tx_request, on_tx_response
    nullptr,                                            // on_tsx_state
};

class PjAccount;
class PjCall;
class PjLog;
struct MsrpLeg;

/** 이벤트 fan-out — 주 리스너(start 인자) 뒤에 관찰자들(addObserver — 구동 세션·계측 링크, ue_voice_quality.md §5.3)에게 같은
 *  이벤트를 같은 이벤트 스레드에서 차례로 준다. 재진입 락이라 콜백 안에서 관찰자를 빼도 된다. removeObserver 는 진행 중 전달이
 *  끝날 때까지 기다리므로, 돌아온 뒤에는 그 관찰자가 다시 불리지 않는다. */
class FanoutListener : public Listener {
public:
    Listener* primary = nullptr;
    void add(Listener* l) {
        if (!l) return;
        { std::lock_guard<std::recursive_mutex> lk(m_); obs_.push_back(l); }
        std::lock_guard<std::mutex> lk(vm_); vobs_.push_back(l);
    }
    void remove(Listener* l) {
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            for (auto it = obs_.begin(); it != obs_.end();) it = *it == l ? obs_.erase(it) : it + 1;
        }
        std::lock_guard<std::mutex> lk(vm_);                // 진행 중인 프레임 전달이 끝난 뒤 반환
        for (auto it = vobs_.begin(); it != vobs_.end();) it = *it == l ? vobs_.erase(it) : it + 1;
    }
    void onLog(int level, const std::string& msg) override { each([&](Listener* l) { l->onLog(level, msg); }); }
    void onRegState(const RegInfo& i) override { each([&](Listener* l) { l->onRegState(i); }); }
    void onIncomingCall(const CallInfo& i) override { each([&](Listener* l) { l->onIncomingCall(i); }); }
    void onCallState(const CallInfo& i) override { each([&](Listener* l) { l->onCallState(i); }); }
    void onCallMedia(const CallInfo& i) override { each([&](Listener* l) { l->onCallMedia(i); }); }
    void onFloor(const FloorEvent& e) override { each([&](Listener* l) { l->onFloor(e); }); }
    void onRoster(int a, const std::string& g, const std::vector<RosterEntry>& u, bool f) override {
        each([&](Listener* l) { l->onRoster(a, g, u, f); });
    }
    void onDialogInfo(const DialogInfo& d) override { each([&](Listener* l) { l->onDialogInfo(d); }); }
    void onMcpttCondition(const CallInfo& i, ConditionCause c) override { each([&](Listener* l) { l->onMcpttCondition(i, c); }); }
    void onNonAcknowledgedUsers(const CallInfo& i) override { each([&](Listener* l) { l->onNonAcknowledgedUsers(i); }); }
    void onEmergencyAlert(const EmergencyAlert& a) override { each([&](Listener* l) { l->onEmergencyAlert(a); }); }
    void onTransmission(const TransmissionEvent& e) override { each([&](Listener* l) { l->onTransmission(e); }); }
    void onReception(const ReceptionEvent& e) override { each([&](Listener* l) { l->onReception(e); }); }
    void onSds(const SdsMessage& m) override { each([&](Listener* l) { l->onSds(m); }); }
    void onRequestResult(const RequestResult& r) override { each([&](Listener* l) { l->onRequestResult(r); }); }
    void onMessage(int a, const std::string& f, const std::string& ct, const std::string& b) override {
        each([&](Listener* l) { l->onMessage(a, f, ct, b); });
    }
    void onEngineStopped() override { each([&](Listener* l) { l->onEngineStopped(); }); }
    /** 영상 스레드 — 이벤트 전달 잠금(m_)을 잡지 않는다: 이벤트 스레드의 핸들러가 엔진 명령을 부르는 중(ue-ctl 이 영상 포트를 멈추며
     *  이 스레드를 기다릴 수 있다)이면 교착한다. 관찰자 목록은 따로 든다(vm_). */
    void onVideoFrame(const VideoFrame& f) override {
        std::lock_guard<std::mutex> lk(vm_);
        if (primary) primary->onVideoFrame(f);
        for (Listener* l : vobs_) l->onVideoFrame(f);
    }

private:
    template <class F> void each(F fn) {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (primary) fn(primary);
        std::vector<Listener*> snap = obs_;              // 콜백 안의 remove 가 순회를 깨지 않게
        for (Listener* l : snap) {
            if (std::find(obs_.begin(), obs_.end(), l) != obs_.end()) fn(l);
        }
    }
    std::recursive_mutex m_;
    std::vector<Listener*> obs_;
    std::mutex vm_;                    // 영상 프레임 전달(onVideoFrame) 전용
    std::vector<Listener*> vobs_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct Engine::Impl {
    EngineConfig cfg;
    Listener* listener = nullptr;     // = &fanout (start 가 연결) — emit 은 이것만 부른다
    FanoutListener fanout;
    std::atomic<bool> running{false};
    std::atomic<bool> captureOn{true};  // 캡처 게이트(setCaptureEnabled) — 기동마다 전이중
    // 장치 단 음량(setDeviceAudioLevels) — 앱이 한 번이라도 걸었을 때만 다시 건다(기본 = 엔진 기본값 그대로)
    bool levelsSet = false;
    float spkLevel = 1.f;
    double micTarget = kMicAgcTargetDbov;
    void* videoWindow = nullptr;        // setVideoWindow — 코어가 참조 하나를 소유(Android ANativeWindow). ue-ctl·콜백이 읽는다(videoM)
    std::mutex videoM;
    int camDev = -1;                    // 캡처 카메라 — -1 = 처음 쓸 때 전면 카메라로 정한다
    bool previewWanted = false;         // setVideoPreview — 송출하는 동안 셀프뷰 프레임(프레임 렌더 빌드). ue-ctl 에서만
    int previewDev = -2;                // 셀프뷰를 연 캡처 장치(-2 = 열지 않음). ue-ctl 에서만
    std::atomic<bool> videoTickArmed{false};   // 영상 keep-alive 틱(pjsua2 util timer) 예약 중

    Worker ctl;                 // ue-ctl — pjsua2 전용
    Worker evt;                 // ue-evt — 리스너 전용
    std::unique_ptr<pj::Endpoint> ep;
    /** pjsua2 소유 — libInit 에 넘긴 뒤에는 Endpoint::libDestroy 가 delete 한다(여기서 지우면 이중 해제). */
    pj::LogWriter* logWriter = nullptr;

    // ue-ctl 에서만 접근
    std::map<int, std::unique_ptr<pj::Account>> accounts;
    std::map<int, AccountConfig> accountCfgs;
    std::map<int, std::unique_ptr<pj::Call>> calls;        // pjsua call id → Call
    /** 추가 재생 라우트(routeId ≥ 1) — 재생 전용 ExtraAudioDevice. 라우트 0 은 기본 재생 장치. */
    std::map<int, std::unique_ptr<pj::ExtraAudioDevice>> routes;
    std::unique_ptr<pj::AudioMediaPlayer> txPlayer;       // 송출 원천 = WAV 반복 재생(setTxSource) — 없으면 마이크
    int nextRouteId = 1;
    int nextAccountId = 0;
    std::atomic<int64_t> nextToken{1};
    /** 망 변경 뒤 재등록의 줄(Engine::handleNetworkChange) — ue-ctl 에서만. */
    detail::RegRecovery regRecovery;
    void reRegister(int accountId);

    // 스냅샷 — 콜백(pjsip 스레드)이 쓰고 조회(임의 스레드)가 읽는다
    std::mutex snapM;
    TlsPeerExpiry tlsPeer;                                 // 마지막 성공 TLS 핸드셰이크의 서버 인증서 만료(onTransportState)
    std::map<int, RegInfo> regInfos;
    std::map<int, CallInfo> callInfos;                     // 종료된 호도 잠시 보존(조회·최종 통계) — pruneFinished
    std::map<int, StreamStats> finalStats;                 // onStreamDestroyed 시점의 최종 RTP 통계
    std::map<int, CallQuality> finalQuality;               // 소멸한 오디오 스트림들의 누적 품질(quality::merge)
    /** 응답을 기다리는 affiliation PUBLISH — 내부 token 별. 412 초기 재발행은 새 내부 token 이고 앱에는 appToken 으로 알린다. */
    struct PendingPublish {
        int accountId = -1;
        std::string groupId;                               // MCVideo 는 비어 있다 — 게시 하나가 관심 그룹 전부다
        bool on = false;
        bool conditional = false;                          // SIP-If-Match 를 실었다(ETag 조건부 갱신)
        int64_t appToken = -1;                             // affiliate() 가 돌려준 token
        McService service = McService::Mcptt;
    };
    std::map<int64_t, PendingPublish> publishPending;
    /** MCVideo 관심 그룹(bare, 계정별) — affiliation PUBLISH 는 늘 전부를 싣는다(TS 24.281 §8.2.1.2 6)a)). ue-ctl 에서만. */
    std::map<int, std::set<std::string>> mcvideoAffiliations;
    static std::string publishKey(int accountId, const std::string& groupId, McService service) {
        return std::to_string(accountId) + ":" + (service == McService::McVideo ? std::string("mcvideo") : groupId);
    }
    // media plane SDS(MSRP) 입출력 스레드 — 분리 실행, stop() 이 취소하고 모두 끝날 때까지 기다린다.
    std::mutex msrpM;
    std::condition_variable msrpCv;
    int msrpActive = 0;
    std::vector<std::weak_ptr<std::atomic<bool>>> msrpCancels;
    std::map<std::string, std::string> publishEtag;               // "accountId:group" → SIP-ETag
    static constexpr size_t kKeepFinished = 64;
    void pruneFinished() {                                 // snapM 잡은 상태에서 호출
        while (callInfos.size() > kKeepFinished) {
            auto it = callInfos.begin();
            for (; it != callInfos.end(); ++it) if (it->second.state == CallState::Disconnected) break;
            if (it == callInfos.end()) break;
            finalStats.erase(it->first);
            finalQuality.erase(it->first);
            callInfos.erase(it);
        }
    }
    static StreamStats fromPj(const pj::StreamStat& st) {
        StreamStats s;
        s.rxPackets = st.rtcp.rxStat.pkt; s.rxBytes = st.rtcp.rxStat.bytes;
        s.rxLoss = st.rtcp.rxStat.loss; s.rxDiscard = st.rtcp.rxStat.discard;
        s.txPackets = st.rtcp.txStat.pkt; s.txBytes = st.rtcp.txStat.bytes;
        s.rxJitterUs = (unsigned)st.rtcp.rxStat.jitterUsec.mean;
        s.valid = true;
        return s;
    }

    /** 오디오 스트림 idx 의 품질 — pjsua2 RTCP·지터버퍼 통계 + RTCP-XR(pjsua_call_get_stream_stat_xr) → quality::compute
     *  (ue_voice_quality.md §3). pjsua 락 아래에서 부르는 것은 안전하다(재진입 락). 실패하면 valid=false. */
    static CallQuality measure(pj::Call* c, unsigned idx) {
        quality::QualityInput in;
        pj::StreamStat st = c->getStreamStat(idx);
        try {
            pj::StreamInfo si = c->getStreamInfo(idx);
            in.codec = si.codecName;
            in.clockRate = si.codecClockRate;
        } catch (...) {}
        const pj::RtcpStreamStat& rx = st.rtcp.rxStat;
        const pj::RtcpStreamStat& tx = st.rtcp.txStat;
        in.rxPackets = rx.pkt; in.rxLost = rx.loss; in.rxDiscard = rx.discard;
        if (rx.jitterUsec.n > 0) { in.rxJitterMeanUs = rx.jitterUsec.mean; in.rxJitterMaxUs = rx.jitterUsec.max; }
        in.remoteReports = tx.updateCount;
        in.txPackets = tx.pkt; in.remoteLost = tx.loss;
        if (tx.jitterUsec.n > 0) { in.remoteJitterMeanUs = tx.jitterUsec.mean; in.remoteJitterMaxUs = tx.jitterUsec.max; }
        if (st.rtcp.rttUsec.n > 0) in.rttMeanUs = st.rtcp.rttUsec.mean;
        in.jbAvgDelayMs = st.jbuf.avgDelayMsec;
        in.startEpochMs = (int64_t)st.rtcp.start.sec * 1000 + st.rtcp.start.msec;
        pj_time_val now;
        pj_gettimeofday(&now);
        in.nowEpochMs = (int64_t)now.sec * 1000 + now.msec;
        pjmedia_rtcp_xr_stat xr;
        if (pjsua_call_get_stream_stat_xr(c->getId(), idx, &xr) == PJ_SUCCESS) {
            auto take = [](quality::XrMetrics& m, const pjmedia_rtcp_xr_stream_stat& d) {
                if (d.voip_mtc.update.sec == 0 && d.voip_mtc.update.msec == 0) return;   // 아직 계산·수신 전
                m.valid = true;
                m.lossRate = d.voip_mtc.loss_rate; m.discardRate = d.voip_mtc.discard_rate;
                m.burstDensity = d.voip_mtc.burst_den; m.gapDensity = d.voip_mtc.gap_den;
                m.burstMs = d.voip_mtc.burst_dur; m.gapMs = d.voip_mtc.gap_dur;
                m.signalDbm = d.voip_mtc.signal_lvl; m.noiseDbm = d.voip_mtc.noise_lvl;
            };
            take(in.xrRx, xr.rx);         // 자기 수신 — XR 보고를 만들 때 계산된 값
            take(in.xrRemote, xr.tx);     // 상대가 보낸 XR(내 스트림에 대한 보고)
            if (in.rttMeanUs < 0 && xr.rtt.n > 0) in.rttMeanUs = xr.rtt.mean;   // DLRR 로만 RTT 가 잡힌 경우
        }
        return quality::compute(in);
    }

    void emit(std::function<void()> fn) {
        if (listener) evt.post(std::move(fn));
    }
    void log(int level, const std::string& msg) {
        emit([this, level, msg] { listener->onLog(level, msg); });
    }
    CallInfo snapshotCall(int callId) {
        std::lock_guard<std::mutex> lk(snapM);
        auto it = callInfos.find(callId);
        return it == callInfos.end() ? CallInfo{} : it->second;
    }
    void updateCall(int callId, const std::function<void(CallInfo&)>& f, CallInfo* out = nullptr) {
        std::lock_guard<std::mutex> lk(snapM);
        CallInfo& ci = callInfos[callId];
        ci.callId = callId;
        f(ci);
        if (out) *out = ci;
    }

    /**
     * **새 호가 시작될 때 낡은 항목을 비운다.**
     *
     * `callInfos` 는 종료된 호도 64건까지 보존하는데(조회·최종 통계) pjsua 는 call id 를 순환
     * 재사용한다(`pjsua_call.c` `alloc_call_id`). 그래서 같은 id 를 다시 쓰면 새 호가 **옛 호의 필드를
     * 물려받는다** — 새 호 경로가 명시적으로 덮지 않는 `listenOnly`·`joinedDialog`·`isMcptt`·`groupId`·
     * `mcptt`·`muted`·`video` 가 그대로 남는다.
     *
     * 실제로 이것이 관제 앱에서 «감청을 끊은 뒤 걸려 온 전화가 감청 leg 으로 분류돼 응답 버튼이
     * 사라지는» 증상을 만들었다. 새 호는 언제나 깨끗한 항목에서 시작한다.
     */
    void resetCall(int callId) {
        std::lock_guard<std::mutex> lk(snapM);
        CallInfo fresh;
        fresh.callId = callId;
        callInfos[callId] = fresh;
        finalStats.erase(callId);
    }
    void applyCodecPolicy();
    PjCall* findCall(int callId);
    static bool rxOnlyLeg(PjCall* call);
    pj::AudioMedia* activeAudio(PjCall* call, unsigned* idxOut = nullptr);
    void wireMedia(PjCall* call, int callId);
    int64_t doSendRequest(int accountId, const std::string& method, const std::string& targetUri,
                       const std::string& contentType, const std::string& body,
                       const std::map<std::string, std::string>& headers, int64_t token);
    /** affiliation PUBLISH(TS 24.379 §9) — ue-ctl 에서. allowConditional 이면 저장된 ETag 로 SIP-If-Match(RFC 3903 §4.4). */
    int64_t sendAffiliation(int accountId, const std::string& groupId, bool on, int64_t token, int64_t appToken, bool allowConditional);
    /** MCVideo affiliation PUBLISH(TS 24.281 §8.2.1.2) — 관심 그룹 전부(mcvideoAffiliations)를 한 게시로. ue-ctl 에서. */
    int64_t sendMcVideoAffiliation(int accountId, int64_t token, int64_t appToken, bool allowConditional);
    /** 영상 미디어가 활성된 호 — 수신 창 결선 + (계정 videoAutoTransmit 면) 카메라 송신 개시. 영상 없는 빌드면 아무것도 안 한다. */
    void attachVideo(PjCall* call, int accountId);
    /** 내 영상 송출 개폐 — MCVideo 호는 허용(videoSend)·송출 허가(sendOn)가 둘 다일 때만, 그 밖의 호는 허용만 본다(ue_sdk.md §4.5).
     *  이미 그 상태면 아무것도 안 한다. 송출 시작은 첫 프레임을 키프레임으로 만들고, 정지는 카메라를 닫는다(재협상 없음). */
    void applyVideoTx(PjCall* call);
    /** 영상 keep-alive 틱(ue-ctl, PJMEDIA_STREAM_KA_INTERVAL 주기) — 송출하지 않는 MCVideo 영상 스트림의 NAT 매핑 유지.
     *  CMP 는 멤버 영상 포트를 그 멤버가 보낸 패킷으로 latch 한다(cmp.md) — 보내지 않는 수신자도 보내야 받는다. */
    void videoTick();
    /** 틱 예약 — 첫 예약은 1 s 뒤(스트림 개시 keep-alive 가 서버 JOIN 보다 먼저 닿아 버려질 수 있다), 이후 KA 주기. */
    void armVideoTick(unsigned delayMs = PJMEDIA_STREAM_KA_INTERVAL * 1000);
    /** 기억한 장치 단 음량을 slot 0 에 다시 건다 — 게이트 전환·재오픈·미디어 결선 뒤(재오픈은 slot 0 레벨을 초기화한다). */
    void applyDeviceLevels();
    /** media plane SDS 입출력 스레드(분리 실행) — 결과는 onRequestResult(MSRP)·onSds 로, 끝나면 호를 정리한다. */
    void startMsrpSend(int callId, int accountId, const MsrpLeg& leg);
    void startMsrpRecv(int callId, int accountId, const MsrpLeg& leg);
    void runMsrpThread(std::shared_ptr<std::atomic<bool>> cancel, std::function<void()> body);
    /** 큰 그룹 SDS — MSRP 발신 INVITE(ue-ctl). */
    bool startMsrpInvite(int accountId, const std::string& groupId, int64_t token, const std::string& sigTlv, const std::string& payTlv);
    /** 캡처 카메라 목록(합성 장치 제외)과 전면 카메라. */
    std::vector<int> cameras();
    int frontCamera();
    /** 셀프뷰를 송출 상태에 맞춘다(ue-ctl 로 넘긴다) — 원하고(previewWanted) 영상을 보내는 호가 있으면 송출 카메라(camDev)에 미리보기 창을
     *  연다(카메라는 송출이 이미 열어 두었다 — 같은 캡처 포트를 함께 쓴다), 아니면 닫는다. */
    void requestPreviewSync();
    void syncPreview();
};

static void setCallMedia(pj::CallSetting& opt, bool video);
static void setMcVideoMedia(pj::CallSetting& opt);

namespace {

/** MCPTT 세션(그룹콜/사설콜) 부속 상태 — PjCall 소유. */
struct McpttSession {
    std::string groupId;                 // bare id (그룹) 또는 상대 번호(사설콜)
    bool isPrivate = false;
    bool fullDuplex = false;             // mc_no_floor_ctrl — floor 없이 마이크 상시
    bool listenOnly = false;
    bool emergency = false, imminentPeril = false;   // 세션 조건 현재값 — 개시 옵션·착신 mcptt-info 로 시작(CallInfo.condition 의 원본)
    bool condMine = false;               // 이 단말이 올린 조건
    bool condPending = false;            // 상향·하향 re-INVITE 응답 대기 — 끝나면 prev* 로 되돌리거나(Denied) 확정(Confirmed)
    void* condTsx = nullptr;             // 그 re-INVITE 의 pjsip 트랜잭션 — 보낼 때(CALLING) 붙잡는다
    bool prevEmergency = false, prevImminent = false, prevMine = false;
    int condLastCode = 0;
    bool broadcast = false;              // 일제 통화 개시(<broadcast-ind>) — 이 단말이 개시자
    bool micOpen = false;                // floor Granted 로 열림
    bool implicitAwaitAnswer = false;    // 개시 INVITE 가 암묵적 발언 요청 — 200 OK answer 의 fmtp 로 판정(TS 24.380 §14.3.4·§14.3.5)
    std::string pendingAppSdp;           // 송신 SDP 에 주입할 m=application 섹션
    std::unique_ptr<floor::Participant> floor;
    bool remoteLearned = false;
};

/** MCVideo 그룹 호(TS 24.281 §9.2.1 prearranged · §9.2.2 chat) 부속 상태 — PjCall 소유. MCPTT 세션과 독립 다이얼로그다(§7.1). */
struct McVideoSession {
    std::string groupId;                 // bare id
    bool prearranged = false;
    bool incoming = false;               // 제어 기능의 멤버 초대(§9.2.1.3·§6.3.3.1) — 착신
    std::string sessionUri;              // 제어 기능 Contact 의 MCVideo 세션 식별자(재합류 R-URI — §9.2.1.2.4)
    bool implicitAwaitAnswer = false;    // 개시 INVITE 가 암묵적 송출 요청 — 200 OK answer 의 fmtp 로 판정(TS 24.581 §14.3.4·§14.3.5)
    std::string pendingAppSdp;           // 송신 SDP 에 주입할 제어 채널 섹션(`m=application <port> udp MCVideo`)
    std::unique_ptr<mcvideo::Participant> tc;
    bool remoteLearned = false;
    bool contactSet = false;             // 다이얼로그 Contact 에 MCVideo 특성 태그를 실었다(setDialogContactParams)
    bool established = false;            // 전송 제어 호 성립을 처리했다(establishMcVideo)
    bool sendOn = false;                 // 'U: has permission to transmit' — 마이크·카메라 송출(Participant onSend)
};

/** media plane SDS(MSRP, TS 24.282 §9.2.3) 호 — 앱 호 목록에 나오지 않는다(CallInfo 없음). 발신 = 큰 그룹 SDS, 수신 = 서버발 배포. */
struct MsrpLeg {
    bool outgoing = true;
    std::string sessionId = msrp::newSessionId();
    std::string localPath;                 // 첫 SDP 를 만들 때 c= 주소로 정한다 — 주입 섹션·MSRP From-Path 가 같은 값
    std::string serverPath;                // cmdp a=path — 발신 = 200 OK answer, 수신 = offer
    bool started = false;                  // 입출력 스레드를 띄웠다
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    // 발신
    int64_t token = -1;
    std::string signallingTlv, payloadTlv;
    // 수신
    std::string fromUri, groupUri;
};

/** Endpoint — transport 상태 콜백으로 TLS 서버 인증서 만료를 관측한다(sip_tls_signaling.md §8.6.2). */
class PjEndpoint : public pj::Endpoint {
public:
    explicit PjEndpoint(Engine::Impl* o) : o_(o) {}
    void onTransportState(const pj::OnTransportStateParam& prm) override {
        if (prm.state != PJSIP_TP_STATE_CONNECTED || prm.tlsInfo.isEmpty()) return;
        const pj::SslCertInfo& rc = prm.tlsInfo.remoteCertInfo;
        if (rc.isEmpty() || rc.validityEnd.sec <= 0) return;
        TlsPeerExpiry e;
        e.valid = true;
        e.notAfterEpoch = (int64_t)rc.validityEnd.sec;
        e.observedEpoch = (int64_t)std::time(nullptr);
        e.subject = rc.subjectInfo.empty() ? rc.subjectCn : rc.subjectInfo;
        e.remote = prm.tlsInfo.remoteAddr;
        std::lock_guard<std::mutex> lk(o_->snapM);
        o_->tlsPeer = e;
    }
    /** util timer — 코어가 쓰는 것은 영상 keep-alive 틱 하나다. pjsua 작업 스레드에서 오므로 ue-ctl 로 넘긴다. */
    void onTimer(const pj::OnTimerParam&) override {
        o_->ctl.post([o = o_] { o->videoTick(); });
    }
private:
    Engine::Impl* o_;
};

class PjLog : public pj::LogWriter {
public:
    explicit PjLog(Engine::Impl* o) : o_(o) {}
    void write(const pj::LogEntry& e) override {
        std::string m = e.msg;
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
        o_->log(e.level, m);
    }
private:
    Engine::Impl* o_;
};

class PjCall : public pj::Call {
public:
    PjCall(Engine::Impl* o, pj::Account& acc, int accountId, int callId = PJSUA_INVALID_ID)
        : pj::Call(acc, callId), o_(o), accountId_(accountId) {}

    std::unique_ptr<McpttSession> mcptt;
    std::unique_ptr<McVideoSession> mcvideo;   // MCVideo 그룹 호 — mcptt 와 함께 서지 않는다(독립 다이얼로그)
    std::unique_ptr<MsrpLeg> msrp;       // media plane SDS 호 — 앱에 나오지 않는다
    bool recvOnly = false;               // 감청 Join 등 청취 전용 평문 leg (a=recvonly, 마이크 없음)
    bool videoSend = true;               // 내 영상 송출 허용(Engine::setVideoSend) — CallInfo.videoSend 의 원본

    /**
     * 이 호가 **낡은 스냅샷을 아직 비우지 않았다**.
     *
     * pjsua 는 call id 를 순환 재사용하므로(`alloc_call_id`) 새 호는 같은 id 의 옛 항목을 물려받는다.
     * `PjCall` 은 호마다 새로 만들어지므로, 그 객체가 처음 스냅샷을 건드릴 때 한 번 비우면 된다.
     * 발신은 `makeCall` 이 **동기적으로** `onCallState(CALLING)` 을 부르므로 그 콜백이 첫 지점이고,
     * 착신은 `onIncomingCall` 이 첫 지점이다.
     */
    bool needsReset_ = true;
    void claimFresh(int id) { if (needsReset_) { needsReset_ = false; o_->resetCall(id); } }
    int accountId() const { return accountId_; }

    /** MCPTT 세션 신원을 CallInfo 에 투영. 발신은 makeCall 이 동기적으로 onCallState(CALLING) 를 부르므로
     *  첫 스냅샷부터 isMcptt/groupId 가 실려야 앱이 호 종류를 잠시라도 VoLTE 로 읽지 않는다 — startMcptt 의
     *  사후 기록과 onCallState 가 같은 값을 쓴다. 착신은 INVITE 의 mcptt-info(mi) 가 이미 채웠으므로 건드리지 않는다. */
    void projectMcptt(CallInfo& c) const {
        if (!mcptt || c.isMcptt) return;
        c.isMcptt = true; c.groupId = mcptt->groupId;
        c.mcptt.present = true; c.mcptt.sessionType = mcptt->isPrivate ? "private" : "prearranged";
        c.mcptt.privateCall = mcptt->isPrivate; c.mcptt.noFloorCtrl = mcptt->fullDuplex;
        c.mcptt.emergency = mcptt->emergency; c.mcptt.imminentPeril = mcptt->imminentPeril;
        c.mcptt.broadcast = mcptt->broadcast;
        c.halfDuplex = !mcptt->fullDuplex; c.listenOnly = mcptt->listenOnly;
        c.condition.emergency = mcptt->emergency; c.condition.imminentPeril = mcptt->imminentPeril;
        c.condition.mine = mcptt->condMine;
    }

    /** MCVideo 호 신원을 CallInfo 에 투영 — 발신 첫 스냅샷(makeCall 이 동기로 부르는 onCallState(CALLING))부터 서비스가 실려야
     *  앱이 호 종류를 VoLTE 로 읽지 않는다(projectMcptt 와 같은 이유). */
    void projectMcVideo(CallInfo& c) const {
        if (!mcvideo) return;
        c.service = McService::McVideo;
        c.groupId = mcvideo->groupId;
        if (!mcvideo->sessionUri.empty()) c.sessionUri = mcvideo->sessionUri;
    }

    /** 전송 제어 participant 생성·바인드 + 콜백 배선(openFloor 와 같은 규칙 — callId 는 sealCallId 로 확정). 송출 게이트는 ue-ctl 로
     *  넘겨 마이크·카메라를 연다/닫는다. localSsrc = offer·answer `mc_transmission_ssrc`(서버가 이 단말에 보내는 RTCP 헤더 SSRC). */
    bool openTc(const std::string& userId) {
        mcvideo::Participant::Callbacks cb;
        Engine::Impl* o = o_;
        auto idRef = floorCallId_;
        cb.onTransmission = [o, idRef](const TransmissionEvent& e) {
            TransmissionEvent ev = e;
            ev.callId = *idRef;
            o->emit([o, ev] { o->listener->onTransmission(ev); });
        };
        cb.onReception = [o, idRef](const ReceptionEvent& e) {
            ReceptionEvent ev = e;
            ev.callId = *idRef;
            o->emit([o, ev] { o->listener->onReception(ev); });
        };
        cb.onSend = [o, idRef](bool on, uint32_t audioSsrc, uint32_t videoSsrc) {
            int id = *idRef;
            // 송출 SSRC — 규격은 Granted 의 값을 쓰게 하지만(§6.2.4.4.6 2) pjmedia 는 호 중 스트림 SSRC 를 못 바꾼다. CMP 가 송출자를
            //   멤버 전용 포트로 가려 할당 SSRC 를 찍으므로 분배는 맞다(ue_sdk.md §4.6 편차, cmp_media_api.md §7.9).
            o->log(3, "mcvideo call " + std::to_string(id) + " transmit " + (on ? "on" : "off") +
                          (on ? " (server ssrc audio=" + std::to_string(audioSsrc) + " video=" + std::to_string(videoSsrc) + ")" : ""));
            o->ctl.post([o, id, on] {
                PjCall* c = o->findCall(id);
                if (!c || !c->mcvideo) return;
                c->mcvideo->sendOn = on;
                try { o->wireMedia(c, id); } catch (pj::Error& e) { o->log(2, std::string("mcvideo mic: ") + e.info(false)); }
                o->applyVideoTx(c);
            });
        };
        cb.onReceive = [o, idRef](const VideoTransmitter& t, bool on) {
            // 수신 = 호의 수신 창이 받는 영상을 그린다(CMP 가 수신 허가된 송출만 보낸다 — 1차 수신 상한 1). 송출별 렌더는 다중 수신(V8)
            o->log(3, "mcvideo call " + std::to_string(*idRef) + " receive " + t.userId + (on ? " on" : " off"));
        };
        cb.log = [o](int level, const std::string& m) { o->log(level, m); };
        mcvideo->tc.reset(new mcvideo::Participant(-1, mcvideoTcSsrc(), userId, cb));
        if (!mcvideo->tc->open(0)) { mcvideo->tc.reset(); return false; }
        return true;
    }

    /** 상대 SDP 의 제어 채널 — 목적지·헤더 SSRC(`mc_transmission_ssrc`, TS 24.581 §14.3.9). 착신 = 제어 기능 offer, 발신 = 200 OK answer.
     *  목적지는 처음 한 번 배운다. fmtp 는 늘 돌려준다(answer 판정용). 제어 채널이 없으면 false. */
    bool learnTcRemote(const std::string& sdp, mcvideo::TcFmtp* fmtpOut = nullptr) {
        if (!mcvideo || !mcvideo->tc) return false;
        std::string ip; int port = 0;
        mcvideo::TcFmtp f;
        if (!mcvideo::parseControl(sdp, ip, port, f)) return false;
        if (fmtpOut) *fmtpOut = f;
        if (!mcvideo->remoteLearned) {
            mcvideo->remoteLearned = true;
            mcvideo->tc->setRemote(ip, port, f.hasTcSsrc ? f.tcSsrc : 0);
        }
        return true;
    }

    /**
     * MCVideo 호 성립(TS 24.581 §6.2.4.2 · §6.2.5.2) — 한 번. 개시 = 200 OK 수신: 협상된 answer 의 제어 채널 목적지·헤더 SSRC, 제어 기능
     * Contact 의 세션 식별자(isfocus — TS 24.281 §9.2.2.4.1.1 19)), 암묵적 송출 요청의 결과(§14.3.4 mc_granted · §14.3.5
     * mc_implicit_request · §14.4 mc_audio_ssrc/mc_video_ssrc). 착신 = 200 OK 송신(목적지·세션 식별자는 초대로 이미 배웠다).
     * pjsua 공개 API 에 협상 SDP·다이얼로그 조회가 없어 pjsua 내부 표를 읽는다(onCallState 안 — 다이얼로그 락 아래).
     */
    void establishMcVideo(pjsip_role_e role) {
        if (!mcvideo || mcvideo->established) return;
        mcvideo->established = true;
        if (!mcvideo->tc) return;
        if (role == PJSIP_ROLE_UAS) { mcvideo->tc->onEstablished(); return; }
        std::string sdp, contact;
        const int id = getId();
        if (id >= 0 && id < (int)PJSUA_MAX_CALLS) {
            pjsip_inv_session* inv = pjsua_var.calls[id].inv;
            const pjmedia_sdp_session* rem = nullptr;
            if (inv && inv->neg && pjmedia_sdp_neg_get_state(inv->neg) == PJMEDIA_SDP_NEG_STATE_DONE &&
                pjmedia_sdp_neg_get_active_remote(inv->neg, &rem) == PJ_SUCCESS && rem) {
                std::vector<char> buf(PJSIP_MAX_PKT_LEN);
                int n = pjmedia_sdp_print(rem, buf.data(), buf.size());
                if (n > 0) sdp.assign(buf.data(), (size_t)n);
            }
            if (inv && inv->dlg && inv->dlg->remote.contact) {
                char buf[PJSIP_MAX_URL_SIZE + 256];
                int n = pjsip_hdr_print_on(inv->dlg->remote.contact, buf, sizeof(buf) - 1);
                if (n > 0) contact.assign(buf, (size_t)n);
            }
        }
        mcvideo::TcFmtp f;
        if (!learnTcRemote(sdp, &f)) o_->log(2, "mcvideo call " + std::to_string(id) + ": answer has no transmission control channel");
        const size_t a = contact.find('<'), b = contact.find('>');
        if (a != std::string::npos && b != std::string::npos && contact.find("isfocus") != std::string::npos)
            mcvideo->sessionUri = contact.substr(a + 1, b - a - 1);
        const bool implicitAccepted = mcvideo->implicitAwaitAnswer && f.implicitRequest;
        mcvideo->implicitAwaitAnswer = false;
        mcvideo->tc->onEstablished(implicitAccepted, implicitAccepted && f.granted, f.hasAudioSsrc ? f.audioSsrc : 0,
                                   f.hasVideoSsrc ? f.videoSsrc : 0);
    }

    /** 이 단말이 고른 `mc_transmission_ssrc` — 호마다 새 값(§14.2.7 «unique» — 0 은 쓰지 않는다). */
    static uint32_t mcvideoTcSsrc() {
        static std::mt19937 rng{std::random_device{}()};
        static std::mutex m;
        std::lock_guard<std::mutex> lk(m);
        uint32_t v = 0;
        while (!v) v = (uint32_t)rng();
        return v;
    }

    /** 세션 조건을 스냅샷에 옮기고 onMcpttCondition 을 낸다. */
    void publishCondition(ConditionCause cause) {
        if (!mcptt) return;
        CallInfo snap;
        o_->updateCall(getId(), [&](CallInfo& c) {
            c.condition.emergency = mcptt->emergency; c.condition.imminentPeril = mcptt->imminentPeril;
            c.condition.mine = mcptt->condMine; c.condition.pending = mcptt->condPending;
            c.condition.lastCode = mcptt->condLastCode;
        }, &snap);
        o_->emit([o = o_, snap, cause] { o->listener->onMcpttCondition(snap, cause); });
    }

    /** 서버 재광고(TS 24.379 §10.1.1.2.1.6) — emergency-ind true 는 임박을 내린다(1)d)), false 는 긴급만, imminentperil-ind 는 임박만.
     *  둘 다 내려가면 이 단말이 올린 조건도 끝이다. 바뀌었으면 true. */
    bool applyAdvertised(int e, int i) {
        if (!mcptt || (!e && !i)) return false;
        bool ne = mcptt->emergency, ni = mcptt->imminentPeril;
        if (e > 0) { ne = true; ni = false; }
        else if (e < 0) ne = false;
        if (i > 0 && e <= 0) ni = true;
        else if (i < 0) ni = false;
        if (ne == mcptt->emergency && ni == mcptt->imminentPeril) return false;
        mcptt->emergency = ne; mcptt->imminentPeril = ni;
        if (!ne && !ni) mcptt->condMine = false;
        return true;
    }

    /** 청취 전용 leg — 로컬 SDP 의 audio 방향을 recvonly 로 (서버가 PTT_JOIN recv_only / tap 으로 해석). */
    static std::string forceRecvOnly(const std::string& w) {
        size_t a = w.find("a=sendrecv");
        if (a != std::string::npos) return w.substr(0, a) + "a=recvonly" + w.substr(a + 10);
        size_t ma = w.find("m=audio ");
        if (ma == std::string::npos) return w;
        size_t eol = w.find("\r\n", ma);
        return eol == std::string::npos ? w : w.substr(0, eol + 2) + "a=recvonly\r\n" + w.substr(eol + 2);
    }

    /** floor participant 생성·바인드 + 콜백 배선. 이벤트 콜백 안의 callId 는 나중에(makeCall 뒤) 정해질 수
     *  있어 참조로 들고 있다가 sealCallId 로 확정한다. 마이크 게이트는 ue-ctl 로 넘겨 pjsua 를 만진다. */
    bool openFloor(const std::string& userId) {
        floor::Participant::Callbacks cb;
        Engine::Impl* o = o_;
        auto idRef = floorCallId_;
        cb.onEvent = [o, idRef](FloorEvent ev) {
            ev.callId = *idRef;
            o->emit([o, ev] { o->listener->onFloor(ev); });
        };
        cb.onMic = [o, idRef](bool on) {
            int id = *idRef;
            o->ctl.post([o, id, on] {
                PjCall* c = o->findCall(id);
                if (!c || !c->mcptt) return;
                c->mcptt->micOpen = on;
                try { o->wireMedia(c, id); } catch (pj::Error& e) { o->log(2, std::string("floor mic: ") + e.info(false)); }
            });
        };
        cb.log = [o](int level, const std::string& m) { o->log(level, m); };
        // 일제 통화 개시자: 발언을 놓은 뒤 B-bit Floor Idle → 호 해제(BYE) — TS 24.380 §6.2.4.6.4, TS 24.379 §4.12.
        //   세션은 서버가 T4 로도 거두지만, 개시 단말이 먼저 나가는 것이 규격 절차다.
        cb.onBroadcastEnd = [o, idRef] {
            int id = *idRef;
            o->ctl.post([o, id] {
                PjCall* c = o->findCall(id);
                if (!c) return;
                o->log(3, "broadcast call " + std::to_string(id) + ": floor idle after release → release call");
                try { pj::CallOpParam prm; c->hangup(prm); } catch (pj::Error& e) { o->log(2, std::string("broadcast release: ") + e.info(false)); }
            });
        };
        mcptt->floor.reset(new floor::Participant(-1, ssrcOf(userId), userId, cb));
        if (!mcptt->floor->open(0)) { mcptt->floor.reset(); return false; }
        mcptt->floor->setMicOpenDelay(o_->cfg.grantMicDelayMs);
        if (mcptt->listenOnly) mcptt->floor->setListenOnly(true);
        return true;
    }
    void sealCallId(int id) { *floorCallId_ = id; }

    void learnFloorRemote(const std::string& sdp) {
        if (!mcptt || !mcptt->floor || mcptt->remoteLearned) return;
        std::string ip; int port = 0;
        if (parseApplication(sdp, ip, port)) {
            mcptt->remoteLearned = true;
            mcptt->floor->setRemote(ip, port);
        }
    }

    void onCallSdpCreated(pj::OnCallSdpCreatedParam& prm) override {
        if (msrp) {
            // m=message 섹션 — 발신 offer 는 pjsua 의 m=text 슬롯 자리에, 수신 answer 는 pjsua 가 포트 0 으로 만든 섹션을 교체한다
            //   (media_count 불변). 수신의 더미 오디오는 inactive(서버 계약 — 포트 9 inactive 와 짝).
            try {
                std::string whole = prm.sdp.wholeSdp;
                if (whole.empty()) { o_->log(1, "msrp: empty wholeSdp — skip inject"); return; }
                if (msrp->localPath.empty()) {
                    std::string ip = msrp::connAddrOf(whole);
                    msrp->localPath = msrp::localPath(ip.empty() ? "127.0.0.1" : ip, msrp->sessionId);
                }
                // 미디어 수 불변(pjsua med_prov_cnt ≥ SDP media_count) — floor 주입과 같이 m=text 슬롯을 쓴다. 덧붙이면 assert.
                const char* slot = whole.find("m=message") != std::string::npos ? "m=message"
                                 : whole.find("m=text") != std::string::npos ? "m=text" : "\x01";
                whole = replaceMediaSection(whole, slot, msrp->outgoing ? msrp::sdpSection(msrp->localPath, "actpass", "sendonly")
                                                                        : msrp::sdpSection(msrp->localPath, "active", "recvonly"));
                if (!msrp->outgoing) whole = msrp::audioInactive(whole);
                prm.sdp.wholeSdp = whole;
            } catch (...) {}
            return;
        }
        if (mcvideo) {
            // 제어 채널 섹션 — 발신 offer 는 pjsua 의 m=text 슬롯(audio → video → text 순서라 K4 m 순서가 된다), 수신 answer 는 pjsua 가
            //   포트 0 으로 만든 m=application 을 교체한다(media_count 불변 — floor 주입과 같다). i= 성분 표시는 송신 직전 모듈이 넣는다.
            try {
                std::string whole = prm.sdp.wholeSdp;
                if (whole.empty()) {
                    o_->log(1, "onCallSdpCreated: empty wholeSdp (SDP print buffer overflow) — skip mcvideo inject");
                } else if (!mcvideo->pendingAppSdp.empty()) {
                    // 영상 없는 빌드 — offer 에 m=video 가 없으면 첫 text 슬롯이 port 0 영상 자리다(setMcVideoMedia)
                    if (prm.remSdp.wholeSdp.empty() && whole.find("m=video") == std::string::npos && whole.find("m=text") != std::string::npos)
                        whole = replaceMediaSection(whole, "m=text", mcvideo::kVideoPlaceholderSdp);
                    const char* slot = whole.find("m=application") != std::string::npos ? "m=application"
                                     : whole.find("m=text") != std::string::npos ? "m=text" : "\x01";
                    prm.sdp.wholeSdp = replaceMediaSection(whole, slot, mcvideo->pendingAppSdp);
                }
                if (!prm.remSdp.wholeSdp.empty()) learnTcRemote(prm.remSdp.wholeSdp);         // UAS: 제어 기능 offer
                // 개시 INVITE Contact = MCVideo 특성 태그(§9.2.1.2.1.1) — pjsua 는 이 콜백 뒤에 INVITE 를 만든다
                if (!mcvideo->contactSet) {
                    mcvideo->contactSet = setDialogContactParams(getId(), mcvideo::contactFeatureParams());
                    if (!mcvideo->contactSet) o_->log(2, "mcvideo call " + std::to_string(getId()) + ": Contact feature tags not set");
                }
            } catch (...) {}
            return;
        }
        if (!mcptt && !recvOnly) return;
        try {
            if (recvOnly && !prm.sdp.wholeSdp.empty()) prm.sdp.wholeSdp = forceRecvOnly(prm.sdp.wholeSdp);
            if (!mcptt) return;
            if (!mcptt->pendingAppSdp.empty()) {
                std::string whole = prm.sdp.wholeSdp;
                if (whole.empty()) {
                    o_->log(1, "onCallSdpCreated: empty wholeSdp (SDP print buffer overflow) — skip floor inject");
                } else if (whole.find("m=application") != std::string::npos) {
                    prm.sdp.wholeSdp = replaceMediaSection(whole, "m=application", mcptt->pendingAppSdp);
                } else if (whole.find("m=text") != std::string::npos) {
                    prm.sdp.wholeSdp = replaceMediaSection(whole, "m=text", mcptt->pendingAppSdp);
                } else {
                    prm.sdp.wholeSdp = replaceMediaSection(whole, "\x01", mcptt->pendingAppSdp);   // append
                }
            }
            // 청취 전용 합류(a=recvonly) — 관제 PTT 청취(dispatch_center.md §5.6): 서버가 PTT_JOIN recv_only 로 변환.
            if (mcptt->listenOnly && !prm.sdp.wholeSdp.empty()) prm.sdp.wholeSdp = forceRecvOnly(prm.sdp.wholeSdp);
            if (!prm.remSdp.wholeSdp.empty()) learnFloorRemote(prm.remSdp.wholeSdp);          // UAS: 상대 offer
        } catch (...) {}
    }

    void onCallTsxState(pj::OnCallTsxStateParam& prm) override {
        try {
            if (prm.e.type != PJSIP_EVENT_TSX_STATE) return;
            const pj::SipTransaction& tsx = prm.e.body.tsxState.tsx;
            // 상향·하향 re-INVITE 의 최종 응답(§10.1.1.2.1.3~5) — 수신 응답·타이머(408) 모두. 2xx = 확정, 그 밖 = 이전 값(§6.2.8.1.5).
            //   그 re-INVITE 의 트랜잭션만 본다 — 첫 INVITE 트랜잭션의 늦은 상태 이벤트(TERMINATED·DESTROYED, 200)가 re-INVITE 를
            //   보낸 뒤에 올 수 있어 "대기 중의 UAC INVITE 최종 응답" 만으로는 섞인다. 보낼 때(CALLING) 붙잡고, 401/407 이면 스택이
            //   인증을 실어 새 트랜잭션으로 다시 보내므로 그것을 다시 붙잡는다.
            if (mcptt && mcptt->condPending && tsx.role == PJSIP_ROLE_UAC && tsx.method == "INVITE") {
                if (!mcptt->condTsx && tsx.state == PJSIP_TSX_STATE_CALLING) mcptt->condTsx = tsx.pjTransaction;
                if (tsx.pjTransaction == mcptt->condTsx && tsx.statusCode >= 200) {
                    mcptt->condTsx = nullptr;
                    if (tsx.statusCode != 401 && tsx.statusCode != 407) {
                        mcptt->condPending = false;
                        mcptt->condLastCode = tsx.statusCode;
                        const bool ok = tsx.statusCode / 100 == 2;
                        if (!ok) { mcptt->emergency = mcptt->prevEmergency; mcptt->imminentPeril = mcptt->prevImminent; mcptt->condMine = mcptt->prevMine; }
                        o_->log(3, "call " + std::to_string(getId()) + " condition re-INVITE → " + std::to_string(tsx.statusCode));
                        publishCondition(ok ? ConditionCause::Confirmed : ConditionCause::Denied);
                    }
                }
            }
            if (prm.e.body.tsxState.type != PJSIP_EVENT_RX_MSG) return;
            const std::string& msg = prm.e.body.tsxState.src.rdata.wholeMsg;
            if (msg.empty()) return;
            // in-dialog INFO — Info Package(RFC 6086). 아는 패키지 g.3gpp.mcptt-info 는 200 + 해석(TS 24.379 §6.3.3.3 미응답 멤버),
            //   모르는 패키지는 469 Bad Info Package(§4.2.2). 패키지 없는 옛 INFO 는 스택 기본 처리에 맡긴다.
            if (tsx.role == PJSIP_ROLE_UAS && msg.rfind("INFO ", 0) == 0 && tsx.state == PJSIP_TSX_STATE_TRYING) {
                const std::string pkg = detail::headerValue(msg, "Info-Package");
                if (!pkg.empty()) {
                    const bool known = pkg.find("g.3gpp.mcptt-info") != std::string::npos;
                    auto* rd = static_cast<pjsip_rx_data*>(prm.e.body.tsxState.src.rdata.pjRxData);
                    auto* t = static_cast<pjsip_transaction*>(tsx.pjTransaction);
                    pjsip_dialog* dlg = t ? pjsip_tsx_get_dlg(t) : nullptr;
                    if (rd && dlg) pjsip_dlg_respond(dlg, rd, known ? 200 : 469, nullptr, nullptr, nullptr);
                    if (known) {
                        std::vector<std::string> nonAck = mcptt::nonAcknowledgedUsers(sipBody(msg));
                        if (!nonAck.empty()) {
                            CallInfo snap;
                            o_->updateCall(getId(), [&](CallInfo& c) { c.nonAcknowledgedUsers = nonAck; }, &snap);
                            o_->emit([o = o_, snap] { o->listener->onNonAcknowledgedUsers(snap); });
                        }
                    }
                    return;
                }
            }
            // 개시 200 OK 의 P-Answer-State(RFC 4964 — TS 24.379 §10.1.1.2.1.1 2A) 사용자에게 알릴 수 있게 기록한다
            if (tsx.role == PJSIP_ROLE_UAC && tsx.method == "INVITE" && msg.rfind("SIP/2.0 200", 0) == 0) {
                const std::string st = detail::headerValue(msg, "P-Answer-State");
                if (!st.empty()) o_->updateCall(getId(), [&](CallInfo& c) { c.answerState = st; });
            }
            if (msrp) {
                // 발신 200 OK answer 의 cmdp a=path → 입출력 스레드(TS 24.282 §9.2.3)
                if (msrp->outgoing && !msrp->started && tsx.role == PJSIP_ROLE_UAC && tsx.method == "INVITE" && msg.rfind("SIP/2.0 2", 0) == 0) {
                    msrp->serverPath = msrp::pathOfSdp(sipBody(msg));
                    msrp->started = true;
                    o_->startMsrpSend(getId(), accountId_, *msrp);
                }
                return;
            }
            if (mcptt && msg.rfind("SIP/2.0 2", 0) == 0 && msg.find("m=application") != std::string::npos) {
                const std::string body = sipBody(msg);
                learnFloorRemote(body);                                                       // UAC: 200 OK answer
                // 암묵적 발언 요청의 결과(§14.3.4 mc_granted = 승인 · §14.3.5 mc_implicit_request = 받아들임) — 목적지를 안 뒤에
                if (mcptt->implicitAwaitAnswer && mcptt->floor && prm.e.body.tsxState.tsx.method == "INVITE") {
                    mcptt->implicitAwaitAnswer = false;
                    const mcptt::FloorFmtp f = mcptt::parseFloorFmtp(body);
                    mcptt->floor->onInitialAnswer(f.granted, f.implicitRequest);
                }
            }
            if (msg.rfind("SIP/2.0 2", 0) == 0 && msg.find("a=ssrc:") != std::string::npos) {
                // 감청 leg 200 OK — a=ssrc label:caller/callee (RFC 5576) → 소스 귀속(U10 디먹스 라벨)
                std::vector<MediaSource> src = mcptt::sdpSsrcLabels(sipBody(msg));
                if (!src.empty()) {
                    CallInfo snap;
                    o_->updateCall(getId(), [&](CallInfo& c) { c.sources = src; }, &snap);
                    o_->emit([o = o_, snap] { o->listener->onCallMedia(snap); });
                }
            }
            // 세션 조건 재광고(TS 24.379 §6.3.3.1.6 긴급·§6.3.3.1.10 긴급 취소·§6.3.3.1.15 임박 위험) — 서버가 멤버 leg 에 보내는 re-INVITE, 조인 200 OK 동봉.
            //   rdata 는 수신 원문만이므로 "INVITE " = 수신 (re-)INVITE, "SIP/2.0 200" = 내 INVITE 의 응답. 바뀐 경우만 이벤트.
            if (mcptt && (msg.rfind("INVITE ", 0) == 0 || (msg.rfind("SIP/2.0 200", 0) == 0 && tsx.method == "INVITE")) &&
                msg.find("mcpttinfo") != std::string::npos) {
                if (applyAdvertised(mcptt::indicator(msg, "emergency-ind"), mcptt::indicator(msg, "imminentperil-ind")))
                    publishCondition(ConditionCause::Advertised);
            }
            if (msg.rfind("NOTIFY ", 0) == 0 && msg.find("conference-info") != std::string::npos) {
                std::vector<RosterEntry> users; bool full = false;
                if (mcptt::parseConferenceInfo(sipBody(msg), users, full)) {
                    std::string gid = mcptt ? mcptt->groupId : std::string();
                    int acc = accountId_;
                    o_->emit([o = o_, acc, gid, users, full] { o->listener->onRoster(acc, gid, users, full); });
                }
            }
        } catch (...) {}
    }

    void onCallState(pj::OnCallStateParam&) override {
        pj::CallInfo ci = getInfo();
        const int id = getId();
        if (msrp) {                                                       // 앱 호 목록 밖 — 끝나면 정리만
            if (ci.state != PJSIP_INV_STATE_DISCONNECTED) return;
            msrp->cancel->store(true);
            if (msrp->outgoing && !msrp->started) {                        // INVITE 가 거절됐다(403 게이트·488 등) — 발신 결과로 알린다
                RequestResult r;
                r.accountId = accountId_; r.token = msrp->token; r.method = "MSRP";
                r.code = ci.lastStatusCode ? ci.lastStatusCode : 500; r.reason = ci.lastReason;
                o_->emit([o = o_, r] { o->listener->onRequestResult(r); });
            }
            o_->ctl.post([o = o_, id] { o->calls.erase(id); });
            return;
        }
        claimFresh(id);                  // 재사용된 call id 의 낡은 상태를 물려받지 않는다
        CallInfo snap;
        bool changed = false;
        // MCVideo 호 성립 — 상태를 알리기 전에(앱이 Active 를 보자마자 [영상 보내기] 할 수 있게) 전송 제어를 성립시킨다. 개시 호는
        //   2xx 에서 CONNECTING 이 SDP 협상보다 먼저라(sip_inv.c) 협상이 끝난 CONFIRMED(ACK 송신)에서, 착신은 200 OK 송신(CONNECTING)에서.
        const bool mcvUacPending = mcvideo && ci.role == PJSIP_ROLE_UAC && ci.state == PJSIP_INV_STATE_CONNECTING;
        if (mcvideo && !mcvUacPending && (ci.state == PJSIP_INV_STATE_CONNECTING || ci.state == PJSIP_INV_STATE_CONFIRMED))
            establishMcVideo(ci.role);
        o_->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId_;
            projectMcptt(c);
            projectMcVideo(c);
            c.remoteUri = ci.remoteUri;
            c.lastCode = ci.lastStatusCode;
            c.lastReason = ci.lastReason;
            CallState ns = c.state;
            switch (ci.state) {
                case PJSIP_INV_STATE_CALLING:
                case PJSIP_INV_STATE_EARLY:
                    if (ci.role == PJSIP_ROLE_UAC) ns = CallState::Outgoing;
                    break;
                case PJSIP_INV_STATE_CONNECTING:
                case PJSIP_INV_STATE_CONFIRMED:
                    if (mcvUacPending) break;                              // MCVideo 개시 — 전송 제어 성립(CONFIRMED) 뒤 Active
                    if (c.state != CallState::Held) ns = CallState::Active;
                    break;
                case PJSIP_INV_STATE_DISCONNECTED:
                    ns = CallState::Disconnected;
                    c.mediaActive = false;
                    break;
                default: break;
            }
            changed = ns != c.state;
            c.state = ns;
        }, &snap);
        if (changed) o_->emit([o = o_, snap] { o->listener->onCallState(snap); });
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            o_->ctl.post([o = o_, id] {                    // 콜백 안에서 자기 객체를 지우지 않는다
                o->calls.erase(id);                        // ~PjCall → floor participant close
                o->syncPreview();                          // 송출하던 호가 끝났으면 셀프뷰도 닫는다(카메라를 놓는다)
                std::lock_guard<std::mutex> lk(o->snapM);
                o->pruneFinished();
            });
        }
    }

    /** 오디오 스트림 생성(pjsua_aud.c — 스트림 시작 뒤·브리지 결선 전). MCVideo 는 송출 허가 전이면 인코더를 멈춘 채로 브리지에
     *  붙인다 — 브리지가 프레임을 넣기 전이라 무음 프레임 한 개도 나가지 않는다(setAudioTx 와 같은 이유, 이후 전환은 wireMedia). */
    void onStreamCreated(pj::OnStreamCreatedParam& prm) override {
        if (mcvideo && !mcvideo->sendOn && prm.stream)
            pjmedia_stream_pause(static_cast<pjmedia_stream*>(prm.stream), PJMEDIA_DIR_ENCODING);
    }

    void onStreamDestroyed(pj::OnStreamDestroyedParam& prm) override {
        if (msrp) return;
        try {
            StreamStats s = Engine::Impl::fromPj(getStreamStat(prm.streamIdx));
            CallQuality q;
            bool audio = false;
            try {
                pj::CallInfo ci = getInfo();
                audio = prm.streamIdx < ci.media.size() && ci.media[prm.streamIdx].type == PJMEDIA_TYPE_AUDIO;
                if (audio) q = Engine::Impl::measure(this, prm.streamIdx);
            } catch (...) { audio = false; }
            std::lock_guard<std::mutex> lk(o_->snapM);
            o_->finalStats[getId()] = s;
            if (audio && q.valid) o_->finalQuality[getId()] = quality::merge(o_->finalQuality[getId()], q);
        } catch (...) {}
    }

    void onCallMediaState(pj::OnCallMediaStateParam&) override {
        if (msrp) return;                                                 // 더미 오디오 — 결선하지 않는다
        const int id = getId();
        pj::CallInfo ci = getInfo();
        bool held = false, active = false, videoActive = false;
        const bool rxOnly = recvOnly || (mcptt && mcptt->listenOnly);
        for (auto& m : ci.media) {
            if (m.type == PJMEDIA_TYPE_VIDEO && m.status == PJSUA_CALL_MEDIA_ACTIVE) videoActive = true;
            if (m.type != PJMEDIA_TYPE_AUDIO) continue;
            if (m.status == PJSUA_CALL_MEDIA_ACTIVE) active = true;
            else if (m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD && rxOnly) active = true;     // 서버 sendonly ↔ 우리 recvonly
            else if (m.status == PJSUA_CALL_MEDIA_LOCAL_HOLD || m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD) held = true;
        }
        try { if (active) o_->wireMedia(this, id); } catch (pj::Error& e) { o_->log(2, std::string("wireMedia: ") + e.info(false)); }
        o_->attachVideo(this, accountId_);
        CallInfo snap;
        bool stateChanged = false;
        o_->updateCall(id, [&](CallInfo& c) {
            c.mediaActive = active;
            c.video = videoActive;                                        // 협상 결과 — offer·발신 옵션이 아니라
            CallState ns = c.state;
            if (held) ns = CallState::Held;
            else if (active && c.state == CallState::Held) ns = CallState::Active;
            stateChanged = ns != c.state;
            c.state = ns;
        }, &snap);
        o_->emit([o = o_, snap, stateChanged] {
            o->listener->onCallMedia(snap);
            if (stateChanged) o->listener->onCallState(snap);
        });
    }

private:
    Engine::Impl* o_;
    int accountId_;
    std::shared_ptr<int> floorCallId_ = std::make_shared<int>(-1);
};

class PjAccount : public pj::Account {
public:
    PjAccount(Engine::Impl* o, int accountId) : o_(o), accountId_(accountId) {}

    void onRegState(pj::OnRegStateParam& prm) override {
        bool active = false;
        int expires = 0;
        try { pj::AccountInfo ai = getInfo(); active = ai.regIsActive; expires = ai.regExpiresSec; } catch (...) {}
        RegInfo ri;
        ri.accountId = accountId_;
        ri.code = prm.code;
        ri.reason = prm.reason;
        ri.expiresSec = expires;
        if (active && prm.code / 100 == 2) ri.state = RegState::Registered;
        else if (!active && prm.code / 100 == 2) ri.state = RegState::Unregistered;
        else ri.state = RegState::Failed;
        { std::lock_guard<std::mutex> lk(o_->snapM); o_->regInfos[accountId_] = ri; }
        o_->emit([o = o_, ri] { o->listener->onRegState(ri); });
        // 앞 등록이 끝났다 — 망 변경으로 미뤄 둔 재등록이 있으면 지금 보낸다(Engine::handleNetworkChange).
        o_->ctl.post([o = o_, id = accountId_] { if (o->running && o->regRecovery.settled(id)) o->reRegister(id); });
    }

    void onIncomingCall(pj::OnIncomingCallParam& prm) override {
        auto* call = new PjCall(o_, *this, accountId_, prm.callId);
        call->sealCallId(prm.callId);
        call->claimFresh(prm.callId);                   // 재사용된 call id 의 낡은 상태를 물려받지 않는다
        std::string whole;
        try { whole = prm.rdata.wholeMsg; } catch (...) {}
        std::string remote;
        try { remote = call->getInfo().remoteUri; } catch (...) {}
        const AccountConfig& cfg = o_->accountCfgs[accountId_];
        // MCData media plane 배포 INVITE(TS 24.282 §9.2.3 — m=message TCP/MSRP + a=path) — 통화가 아니다: 앱에 알리지 않고 받아
        //   cmdp 에 붙어 본문을 받는다(onSds, mediaPlane). 발신자·그룹은 mcdata-info(1:1 이면 request-uri 가 나 자신).
        if (whole.find("TCP/MSRP") != std::string::npos && whole.find("a=path:") != std::string::npos) {
            { std::lock_guard<std::mutex> lk(o_->snapM); o_->callInfos.erase(prm.callId); }   // claimFresh 가 만든 빈 항목 — 앱 호가 아니다
            call->msrp.reset(new MsrpLeg);
            call->msrp->outgoing = false;
            call->msrp->serverPath = msrp::pathOfSdp(whole);
            call->msrp->fromUri = msrp::mcdataInfoUri(whole, "mcdata-calling-user-id");
            if (call->msrp->fromUri.empty()) call->msrp->fromUri = remote;
            std::string req = msrp::mcdataInfoUri(whole, "mcdata-request-uri");
            if (mcptt::bareId(req) != mcptt::bareId(cfg.effectiveMcpttId()) && mcptt::bareId(req) != cfg.msisdn) call->msrp->groupUri = req;
            o_->ctl.post([o = o_, call, id = prm.callId] {
                o->calls[id].reset(call);
                try {
                    pj::CallOpParam p(true);
                    p.statusCode = PJSIP_SC_OK;
                    p.opt.audioCount = 1;
                    p.opt.videoCount = 0;
                    call->answer(p);                                     // answer SDP 는 여기서 만든다(onCallSdpCreated 주입)
                } catch (pj::Error& e) { o->log(1, std::string("msrp answer: ") + e.info(false)); return; }
                call->msrp->started = true;
                o->startMsrpRecv(id, call->accountId(), *call->msrp);
            });
            return;
        }
        // MCVideo 그룹 호 초대(TS 24.281 §9.2.1.3 prearranged 멤버 초대 — 제어 기능이 보낸다, 골든 07) — mcvideo-info 로 가른다.
        const mcvideo::InfoRx vi = mcvideo::parseInfo(whole);
        if (vi.present && mcvideo::isMcVideoSdp(whole)) {
            onIncomingMcVideo(prm, call, whole, remote, vi);
            return;
        }
        McpttInfo mi = mcptt::parseMcpttInfo(whole);
        bool autoAnswer = false;
        if (mi.present) {
            // MCPTT 착신 — floor 소켓은 **180 전에** 바인드해야 한다(pjsua 는 여기서 응답 SDP 를 한 번 만들고
            // 200 에 재사용하므로, 늦으면 m=application 0 이 나가 CSP 가 착신 leg 의 floor 포트를 모른다).
            call->mcptt.reset(new McpttSession);
            call->mcptt->isPrivate = mi.privateCall;
            call->mcptt->fullDuplex = mi.noFloorCtrl;
            call->mcptt->groupId = mi.privateCall ? mcptt::bareId(mi.callingUserId) : mcptt::bareId(remote);
            call->mcptt->emergency = mi.emergency;
            call->mcptt->imminentPeril = mi.imminentPeril && !mi.emergency;
            if (!mi.noFloorCtrl) {
                if (call->openFloor(cfg.effectiveMcpttId()))
                    call->mcptt->pendingAppSdp = mcptt::floorSdp(call->mcptt->floor->localPort(), false);
            } else {
                call->mcptt->micOpen = true;                                                 // 전이중 — 마이크 상시
            }
            autoAnswer = cfg.autoAnswerMcptt;
        }
        CallInfo snap;
        o_->updateCall(prm.callId, [&](CallInfo& c) {
            c.accountId = accountId_;
            c.dir = CallDir::Incoming;
            c.state = CallState::Incoming;
            c.remoteUri = remote;
            c.video = whole.find("m=video") != std::string::npos;
            c.calledParty = detail::uriUser(detail::headerValue(whole, "P-Called-Party-ID"));
            if (mi.present) {
                c.isMcptt = true; c.mcptt = mi; c.groupId = call->mcptt->groupId;
                c.halfDuplex = !mi.noFloorCtrl;
                c.condition.emergency = call->mcptt->emergency; c.condition.imminentPeril = call->mcptt->imminentPeril;
            }
        }, &snap);
        o_->ctl.post([o = o_, call, id = prm.callId] { o->calls[id].reset(call); });
        try {
            pj::CallOpParam p;
            p.statusCode = PJSIP_SC_RINGING;
            call->answer(p);
        } catch (pj::Error& e) { o_->log(2, std::string("180 failed: ") + e.info(false)); }
        o_->emit([o = o_, snap] { o->listener->onIncomingCall(snap); });
        if (autoAnswer) {
            // MCPTT 호는 음성만 — 서버가 m=video 를 실어도 port 0 으로 거절한다(RFC 3264 §6, 그룹 영상 = MCVideo 호 — mcvideo.md §8)
            o_->ctl.post([o = o_, id = prm.callId] {
                PjCall* c = o->findCall(id);
                if (!c) return;
                try {
                    pj::CallOpParam p(true);
                    p.statusCode = PJSIP_SC_OK;
                    p.opt.audioCount = 1;
                    p.opt.videoCount = 0;
                    c->answer(p);
                } catch (pj::Error& e) { o->log(1, std::string("mcptt auto-answer: ") + e.info(false)); }
            });
        }
    }

    /**
     * MCVideo 그룹 호 초대(TS 24.281 §9.2.1.3 — 제어 기능의 prearranged 멤버 초대, 골든 07).
     *
     * 제어 채널 participant 를 **180 전에** 바인드한다 — pjsua 는 180 에서 answer SDP 를 한 번 만들어 200 에 재사용한다(MCPTT floor 와
     * 같은 이유). answer fmtp(TS 24.581 §14.3) = offer 의 `mc_priority` 를 되돌리고(§14.3.3 끝 문단) `mc_queueing` 은 offer 에 있을 때만
     * (§14.3.2 — 참여자는 대기열을 지원한다), `mc_transmission_ssrc` = 이 단말이 고른 값(§14.3.9). Contact = MCVideo 특성 태그(§6.2.3.1.1
     * 3)·4)·§6.2.3.2.1 3)·4)). 자동 수락(AccountConfig.autoAnswerMcvideo)이면 곧바로 200(§6.2.3.1.2), 아니면 앱이 answer/reject.
     */
    void onIncomingMcVideo(pj::OnIncomingCallParam& prm, PjCall* call, const std::string& whole, const std::string& remote,
                           const mcvideo::InfoRx& vi) {
        const AccountConfig& cfg = o_->accountCfgs[accountId_];
        call->mcvideo.reset(new McVideoSession);
        McVideoSession& mv = *call->mcvideo;
        mv.incoming = true;
        mv.prearranged = vi.sessionType == "prearranged";
        mv.groupId = mcptt::bareId(vi.callingGroupId.empty() ? remote : vi.callingGroupId);
        const std::string fc = detail::headerValue(whole, "Contact");               // 제어 기능 Contact = 세션 식별자(§6.3.3.1.2 1))
        const size_t a = fc.find('<'), b = fc.find('>');
        if (a != std::string::npos && b != std::string::npos && fc.find("isfocus") != std::string::npos) mv.sessionUri = fc.substr(a + 1, b - a - 1);
        if (call->openTc(cfg.effectiveMcpttId())) {
            mcvideo::TcFmtp offer;
            call->learnTcRemote(whole, &offer);
            mcvideo::TcFmtp ans;
            ans.queueing = offer.queueing;
            ans.priority = offer.priority;
            ans.hasTcSsrc = true;
            ans.tcSsrc = mv.tc->localSsrc();
            mv.pendingAppSdp = mcvideo::controlSdp(mv.tc->localPort(), ans);
        } else {
            o_->log(1, "mcvideo tc socket bind failed — answer without transmission control");
        }
        mv.contactSet = setDialogContactParams(prm.callId, mcvideo::contactFeatureParams());
        CallInfo snap;
        o_->updateCall(prm.callId, [&](CallInfo& c) {
            c.accountId = accountId_;
            c.dir = CallDir::Incoming;
            c.state = CallState::Incoming;
            c.remoteUri = remote;
            c.video = true;                                                   // 초대 offer 에 m=video 가 있다
            call->projectMcVideo(c);
        }, &snap);
        o_->ctl.post([o = o_, call, id = prm.callId] { o->calls[id].reset(call); });
        try {
            pj::CallOpParam p;
            p.statusCode = PJSIP_SC_RINGING;
            pj::SipHeader req; req.hName = "Require"; req.hValue = "timer";   // 수동 개시 180(TS 24.281 §6.2.3.2.1 2)) — 자동 개시도 같은 180
            p.txOption.headers.push_back(req);
            call->answer(p);
        } catch (pj::Error& e) { o_->log(2, std::string("mcvideo 180 failed: ") + e.info(false)); }
        o_->emit([o = o_, snap] { o->listener->onIncomingCall(snap); });
        o_->log(3, "mcvideo invitation " + mv.groupId + " (" + vi.sessionType + ") → call " + std::to_string(prm.callId));
        if (!cfg.autoAnswerMcvideo) return;
        o_->ctl.post([o = o_, id = prm.callId] {
            PjCall* c = o->findCall(id);
            if (!c) return;
            try {
                pj::CallOpParam p(true);
                p.statusCode = PJSIP_SC_OK;
                setMcVideoMedia(p.opt);                                               // MCVideo 호 = audio + video(§6.2.2 1))
                c->answer(p);
            } catch (pj::Error& e) { o->log(1, std::string("mcvideo auto-answer: ") + e.info(false)); }
        });
    }

    /** sendRequest 트랜잭션 최종 응답(≥200) — 같은 tsx 가 COMPLETED/TERMINATED 로 두 번 올 수 있다. */
    void onSendRequest(pj::OnSendRequestParam& prm) override {
        try {
            if (prm.e.type != PJSIP_EVENT_TSX_STATE) return;
            auto& ts = prm.e.body.tsxState;
            if (ts.tsx.statusCode < 200) return;
            RequestResult r;
            r.accountId = accountId_;
            r.token = (int64_t)(intptr_t)prm.userData;
            r.method = ts.tsx.method;
            r.code = ts.tsx.statusCode;
            r.reason = ts.tsx.statusText;
            if (ts.type == PJSIP_EVENT_RX_MSG) r.etag = detail::headerValue(ts.src.rdata.wholeMsg, "SIP-ETag");
            Engine::Impl::PendingPublish retry;
            int64_t retryToken = -1;
            {
                std::lock_guard<std::mutex> lk(o_->snapM);
                auto it = o_->publishPending.find(r.token);
                if (it != o_->publishPending.end()) {
                    const Engine::Impl::PendingPublish p = it->second;
                    const std::string key = Engine::Impl::publishKey(p.accountId, p.groupId, p.service);
                    r.token = p.appToken;                  // 앱은 affiliate() 의 token 으로 상관한다
                    if (r.code == PJSIP_SC_CONDITIONAL_REQUEST_FAILED) {
                        // RFC 3903 §5 — 412 를 낸 entity-tag 는 버리고(MUST) 같은 요청을 다시 보내지 않는다(MUST NOT).
                        // 상태는 SIP-If-Match 없는 초기 PUBLISH 로 다시 알린다(SHOULD, §4.2) — 한 번만, 결과는 그 응답으로.
                        o_->publishEtag.erase(key);
                        if (p.conditional) { retry = p; retryToken = o_->nextToken++; }
                    } else if (!r.etag.empty() && r.code / 100 == 2) {
                        o_->publishEtag[key] = r.etag;
                    }
                    o_->publishPending.erase(it);
                }
            }
            if (retryToken >= 0) {
                o_->ctl.post([o = o_, retry, retryToken, r] {
                    const int64_t t = retry.service == McService::McVideo
                                          ? o->sendMcVideoAffiliation(retry.accountId, retryToken, retry.appToken, false)
                                          : o->sendAffiliation(retry.accountId, retry.groupId, retry.on, retryToken, retry.appToken, false);
                    if (t < 0) o->emit([o, r] { o->listener->onRequestResult(r); });   // 재발행을 못 만들면 412 를 그대로
                });
                return;
            }
            o_->emit([o = o_, r] { o->listener->onRequestResult(r); });
        } catch (...) {}
    }

    /** MESSAGE/NOTIFY 본문 — MCData SDS → onSds, conference-info → onRoster, 그 외 onMessage.
     *  multipart 는 pjsua2 msgBody 가 비거나 boundary 가 빠지므로 원문에서 Content-Type·본문을 직접 뽑는다. */
    void onInstantMessage(pj::OnInstantMessageParam& prm) override {
        std::string ct = prm.contentType, body = prm.msgBody, from = prm.fromUri;
        bool multipart = ct.rfind("multipart/", 0) == 0;
        if (body.empty() || (multipart && ct.find("boundary") == std::string::npos)) {
            std::string whole;
            try { whole = prm.rdata.wholeMsg; } catch (...) {}
            if (!whole.empty()) {
                std::string h = detail::headerValue(whole, "Content-Type");
                if (!h.empty()) ct = h;
                body = sipBody(whole);
            }
        }
        int acc = accountId_;
        if (body.find("mcdata-signalling") == std::string::npos && body.find("mcpttinfo") != std::string::npos) {
            // 긴급 경보·취소·그룹 긴급 통지(TS 24.379 §12.1.1.3) — 200 OK 는 pjsua 가 이미 보냈다(7)·8)).
            EmergencyAlert a; a.accountId = acc;
            if (mcptt::parseEmergencyAlert(body, a)) {
                if (a.userId.empty()) a.userId = mcptt::bareId(from);
                auto ic = o_->accountCfgs.find(acc);
                a.self = ic != o_->accountCfgs.end() && a.userId == mcptt::bareId(ic->second.effectiveMcpttId());
                o_->emit([o = o_, a] { o->listener->onEmergencyAlert(a); });
                return;
            }
        }
        if (body.find("mcdata-signalling") != std::string::npos) {
            SdsMessage m;
            if (mcdata::parse(ct, body, m)) {
                m.accountId = acc;
                if (m.fromUri.empty()) m.fromUri = from;          // mcdata-calling-user-id 가 없는 발신(옛 단말·1:1 직행)
                o_->emit([o = o_, m] { o->listener->onSds(m); });
                return;
            }
        }
        if (ct.find("dialog-info") != std::string::npos) {
            std::vector<DialogInfo> dl;
            if (mcptt::parseDialogInfo(body, dl)) {
                if (dl.empty()) {                                   // 초기 full 스냅샷에 dialog 없음 — 구독 성립 신호(callId 빈 값)
                    DialogInfo none; none.accountId = acc; none.watched = mcptt::bareId(from); none.full = true;
                    o_->emit([o = o_, none] { o->listener->onDialogInfo(none); });
                }
                for (auto& d : dl) { d.accountId = acc; o_->emit([o = o_, d] { o->listener->onDialogInfo(d); }); }
                return;
            }
        }
        if (ct.find("conference-info") != std::string::npos) {
            std::vector<RosterEntry> users; bool full = false;
            if (mcptt::parseConferenceInfo(body, users, full)) {
                std::string gid = mcptt::bareId(from);
                o_->emit([o = o_, acc, gid, users, full] { o->listener->onRoster(acc, gid, users, full); });
                return;
            }
        }
        o_->emit([o = o_, acc, from, ct, body] { o->listener->onMessage(acc, from, ct, body); });
    }

private:
    Engine::Impl* o_;
    int accountId_;
};

}  // namespace

// ── Impl 헬퍼 ──

/**
 * ue-ctl. 한 계정을 다시 등록한다. 앞 등록 트랜잭션이 걸려 있으면(PJSIP_EBUSY) 그것이 끝난 뒤(onRegState) 다시 —
 * 겹쳐 보내지 않는다(RFC 3261 §10.2, detail::RegRecovery).
 */
void Engine::Impl::reRegister(int id) {
    auto it = accounts.find(id);
    if (it == accounts.end()) return;
    try {
        it->second->setRegistration(true);
        std::lock_guard<std::mutex> lk(snapM);
        regInfos[id].state = RegState::Registering;
    } catch (pj::Error& e) {
        if (e.status == PJSIP_EBUSY) {
            regRecovery.busy(id);
            log(3, "network change: acc " + std::to_string(id) + " 앞 등록이 끝나면 다시 등록");
        } else {
            log(2, "network change: acc " + std::to_string(id) + " 재등록 실패 " + e.info(false));
        }
    }
}

bool Engine::Impl::rxOnlyLeg(PjCall* call) { return call->recvOnly || (call->mcptt && call->mcptt->listenOnly); }

pj::AudioMedia* Engine::Impl::activeAudio(PjCall* call, unsigned* idxOut) {
    pj::CallInfo ci = call->getInfo();
    for (auto& m : ci.media) {
        // 청취 전용 leg(a=recvonly)는 서버가 sendonly 로 답하므로 pjsua 가 REMOTE_HOLD 로 분류한다 — 미디어는 흐른다.
        bool ok = m.status == PJSUA_CALL_MEDIA_ACTIVE || (m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD && rxOnlyLeg(call));
        if (m.type == PJMEDIA_TYPE_AUDIO && ok) {
            pj::Media* med = call->getMedia(m.index);          // 비활성(hold 등)이면 NULL 일 수 있다
            if (!med) continue;
            if (idxOut) *idxOut = m.index;
            return pj::AudioMedia::typecastFromMedia(med);
        }
    }
    return nullptr;
}

PjCall* Engine::Impl::findCall(int callId) {
    auto it = calls.find(callId);
    return it == calls.end() ? nullptr : static_cast<PjCall*>(it->second.get());
}

void Engine::Impl::applyCodecPolicy() {
    // 음성: AMR-WB 최우선 + fmtp octet-align=1; mode-set=0,1,2 (enc/dec). G.711 은 안전망으로 낮은 우선순위,
    // 그 외는 0(협상 표면 축소). codecId 는 실제 열람 결과에서 부분일치로 찾는다(백엔드 표기 차이 흡수).
    std::string amrwb;
    std::vector<std::string> ids;
    for (auto& c : ep->codecEnum2()) {
        ids.push_back(c.codecId);
        if (amrwb.empty() && c.codecId.find("AMR-WB") != std::string::npos) amrwb = c.codecId;
    }
    for (auto& id : ids) {
        unsigned char prio = 0;
        if (id == amrwb) prio = 254;
        else if (id.rfind("PCMU", 0) == 0 || id.rfind("PCMA", 0) == 0) prio = 100;
        try { ep->codecSetPriority(id, prio); } catch (...) {}
    }
    if (!amrwb.empty()) {
        try {
            pj::CodecParam cp = ep->codecGetParam(amrwb);
            pj::CodecFmtpVector f;
            pj::CodecFmtp oa; oa.name = "octet-align"; oa.val = "1";
            pj::CodecFmtp ms; ms.name = "mode-set"; ms.val = "0,1,2";
            f.push_back(oa); f.push_back(ms);
            cp.setting.encFmtp = f;
            cp.setting.decFmtp = f;
            ep->codecSetParam(amrwb, cp);
        } catch (pj::Error& e) { log(2, std::string("AMR-WB fmtp: ") + e.info(false)); }
    } else {
        log(2, "AMR-WB codec not found — 음성 협상은 G.711 안전망만 가능");
    }
    std::string all;
    for (auto& id : ids) all += id + " ";
    log(3, "codecs: " + all + (amrwb.empty() ? "" : "(AMR-WB first)"));
#if PJSUA_HAS_VIDEO
    // 영상: H.264 최우선 + 인코딩 15 fps·평균 400 / 최대 500 kbit/s — 기존 VoLTE 앱(CodecConfig.kt)과 같은 값. 크기 = 카메라 방향:
    //   단말(세로로 든 휴대폰) 480x640, Windows 웹캠은 가로라 640x480 — 캡처를 인코더 크기로 늘이면(vid_port 변환) 비율이 깨진다.
#if defined(_WIN32)
    const unsigned encW = 640, encH = 480;
#else
    const unsigned encW = 480, encH = 640;
#endif
    try {
        for (auto& c : ep->videoCodecEnum2()) {
            if (c.codecId.find("H264") == std::string::npos) continue;
            ep->videoCodecSetPriority(c.codecId, 254);
            pj::VidCodecParam vp = ep->getVideoCodecParam(c.codecId);
            vp.encFmt.width = encW; vp.encFmt.height = encH;
            vp.encFmt.fpsNum = 15; vp.encFmt.fpsDenum = 1;
            vp.encFmt.avgBps = 400000; vp.encFmt.maxBps = 500000;
            ep->setVideoCodecParam(c.codecId, vp);
            log(3, "video codec " + c.codecId + " first, enc " + std::to_string(encW) + "x" + std::to_string(encH) + " 15fps 400k/500k");
            break;
        }
    } catch (pj::Error& e) { log(2, std::string("video codec: ") + e.info(false)); }
#endif
}

// ── 영상 ──
#if defined(__ANDROID__)
static void windowAcquire(void* w) { if (w) ANativeWindow_acquire(static_cast<ANativeWindow*>(w)); }
static void windowRelease(void* w) { if (w) ANativeWindow_release(static_cast<ANativeWindow*>(w)); }
#else
static void windowAcquire(void*) {}                     // 참조 수를 세지 않는 창(HWND 등)
static void windowRelease(void*) {}
#endif

#if CIMSUE_FRAME_SINK
/** 프레임 렌더 장치의 창 토큰 — 수신 창 = callId+1(NULL 은 장치가 버리므로 0 을 피한다), 셀프뷰 = -1. */
static void* frameToken(int callId) { return reinterpret_cast<void*>(static_cast<intptr_t>(callId) + 1); }
static void* const kPreviewToken = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

/** 렌더 장치 put_frame(영상 회의 브리지 클럭 스레드) → Listener::onVideoFrame. 프레임은 이 호출 동안만 유효하다. */
static void onSinkFrame(void* user, void* token, const pjmedia_cims_frame* f) {
    auto* o = static_cast<Engine::Impl*>(user);
    if (!o || !o->running || !f || !f->data) return;
    VideoFrame vf;
    vf.callId = token == kPreviewToken ? -1 : static_cast<int>(reinterpret_cast<intptr_t>(token) - 1);
    vf.width = static_cast<int>(f->width);
    vf.height = static_cast<int>(f->height);
    vf.stride = static_cast<int>(f->stride);
    vf.data = static_cast<const uint8_t*>(f->data);
    vf.size = f->size;
    o->fanout.onVideoFrame(vf);
}
#endif

std::vector<int> Engine::Impl::cameras() {
    std::vector<int> v;
#if PJSUA_HAS_VIDEO
    try {
        pj::VideoDevInfoVector2 devs = ep->vidDevManager().enumDev2();
        for (auto& d : devs) {
            if (!(d.dir & PJMEDIA_DIR_CAPTURE)) continue;
            if (d.driver == "Colorbar") continue;           // 합성 장치(시험용 색 막대)
            v.push_back(d.id);
        }
    } catch (pj::Error& e) { log(2, std::string("camera enum: ") + e.info(false)); }
#endif
    return v;
}

int Engine::Impl::frontCamera() {
#if PJSUA_HAS_VIDEO
    int first = -1;
    try {
        for (auto& d : ep->vidDevManager().enumDev2()) {
            if (!(d.dir & PJMEDIA_DIR_CAPTURE) || d.driver == "Colorbar") continue;
            if (first < 0) first = d.id;
            std::string n = d.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            if (n.find("front") != std::string::npos) return d.id;
        }
    } catch (...) {}
    return first >= 0 ? first : (int)PJMEDIA_VID_DEFAULT_CAPTURE_DEV;
#else
    return -1;
#endif
}

void Engine::Impl::attachVideo(PjCall* call, int accountId) {
#if PJSUA_HAS_VIDEO
    bool autoTx = false;
    { auto it = accountCfgs.find(accountId); if (it != accountCfgs.end()) autoTx = it->second.videoAutoTransmit; }
    pj::CallInfo ci;
    try { ci = call->getInfo(); } catch (...) { return; }
    for (auto& m : ci.media) {
        if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
        // 수신 창(렌더러)은 디코딩 스트림이 만든다 — 아직 없으면(win_in 무효) 붙일 곳이 없다. pjsua_vid_win_* 는 무효 id 를
        // PJ_ASSERT_RETURN 으로 막아 디버그 빌드에서 프로세스가 abort 하므로 부르기 전에 거른다.
        if (!(m.dir & PJMEDIA_DIR_DECODING)) {
            // 송신 전용 — 그릴 것이 없다
        } else if (m.videoIncomingWindowId == PJSUA_INVALID_ID) {
            log(4, "video window: call " + std::to_string(call->getId()) + " has no renderer yet");
        } else {
            std::lock_guard<std::mutex> lk(videoM);
#if CIMSUE_FRAME_SINK
            void* win = frameToken(call->getId());              // 프레임 렌더 — 창 = 이 호를 가리키는 토큰(프레임이 onVideoFrame 으로 간다)
#else
            void* win = videoWindow;
#endif
            if (!win) {
                log(4, "video window: call " + std::to_string(call->getId()) + " renderer waits for a window");
            } else {
                pj::VideoWindow vw = m.videoWindow;
                try {
                    if (vw.getInfo().winHandle.handle.window != win) {
                        pj::VideoWindowHandle h;
#if defined(__ANDROID__)
                        h.type = PJMEDIA_VID_DEV_HWND_TYPE_ANDROID;
#endif
                        h.handle.window = win;
#if !CIMSUE_FRAME_SINK
                        windowAcquire(win);                     // 렌더러가 이 참조를 가진다(교체·스트림 소멸 때 푼다)
#endif
                        vw.setWindow(h);
                        log(3, "video window attached: call " + std::to_string(call->getId()) + " wid " +
                               std::to_string(m.videoIncomingWindowId));
                    }
                } catch (pj::Error& e) { log(2, std::string("video window: ") + e.info(false)); }
                // 표시 전환은 렌더러가 지원할 때만 — Android OpenGL 렌더러는 창을 받으면 그리고 SHOW 능력이 없다(INVCAP).
                try { vw.Show(true); } catch (pj::Error&) {}
            }
        }
        if (autoTx && !call->mcptt && !call->mcvideo && call->videoSend) {
            // 계정 autoTransmitOutgoing 만으로는 협상 방향에 따라 캡처가 열리지 않을 수 있다 — 송신 방향이 없으면 sendrecv 로, 있으면 송신 개시.
            try {
                pj::CallVidSetStreamParam p;
                p.medIdx = (int)m.index;
                pjsua_call_vid_strm_op op = PJSUA_CALL_VID_STRM_START_TRANSMIT;
                if (!(m.dir & PJMEDIA_DIR_ENCODING)) { op = PJSUA_CALL_VID_STRM_CHANGE_DIR; p.dir = PJMEDIA_DIR_ENCODING_DECODING; }
                call->vidSetStream(op, p);
            } catch (pj::Error& e) { log(3, std::string("video transmit: ") + e.info(false)); }
        }
    }
    // MCVideo — 송출은 계정 autoTransmit 이 아니라 송출 허가를 따른다. 보내지 않는 동안은 keep-alive 로 NAT 를 연다.
    if (call->mcvideo) {
        applyVideoTx(call);
        armVideoTick(1000);
    }
#else
    (void)call; (void)accountId;
#endif
}

void Engine::Impl::applyVideoTx(PjCall* call) {
#if PJSUA_HAS_VIDEO
    bool want = call->videoSend && !call->recvOnly;
    if (call->mcvideo) want = want && call->mcvideo->sendOn;                // 송출 허가에서만(TS 24.581 §6.2.4.4.6)
    pj::CallInfo ci;
    try { ci = call->getInfo(); } catch (...) { return; }
    for (auto& m : ci.media) {
        if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
        bool running = false;
        try { running = call->vidStreamIsRunning((int)m.index, PJMEDIA_DIR_ENCODING); } catch (...) {}
        if (want == running) continue;
        pj::CallVidSetStreamParam p;
        p.medIdx = (int)m.index;
        pjsua_call_vid_strm_op op = want ? PJSUA_CALL_VID_STRM_START_TRANSMIT : PJSUA_CALL_VID_STRM_STOP_TRANSMIT;
        if (want && !(m.dir & PJMEDIA_DIR_ENCODING)) {
            if (call->mcvideo) continue;                                  // 그룹 세션 방향은 서버가 정한다 — 재협상하지 않는다
            op = PJSUA_CALL_VID_STRM_CHANGE_DIR; p.dir = PJMEDIA_DIR_ENCODING_DECODING;
        }
        if (want && camDev >= 0) {
            // 송출 카메라 = 지금 고른 카메라(setVideoCaptureDevice) — 호의 캡처 장치는 계정 기본값으로 시작하므로 열기 전에 맞춘다
            //   (송출 전이면 장치 번호만 바뀐다 — pjsua call_change_cap_dev).
            try { pj::CallVidSetStreamParam cp; cp.medIdx = (int)m.index; cp.capDev = (pjmedia_vid_dev_index)camDev;
                  call->vidSetStream(PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV, cp); }
            catch (pj::Error& e) { log(3, std::string("video capture device: ") + e.info(false)); }
        }
        try {
            call->vidSetStream(op, p);
            log(3, std::string("video transmit ") + (want ? "start" : "stop") + ": call " + std::to_string(call->getId()));
        } catch (pj::Error& e) { log(2, std::string("video transmit: ") + e.info(false)); }
        requestPreviewSync();
    }
#else
    (void)call;
#endif
}

void Engine::Impl::requestPreviewSync() {
#if CIMSUE_FRAME_SINK
    if (running) ctl.post([this] { syncPreview(); });
#endif
}

void Engine::Impl::syncPreview() {
#if CIMSUE_FRAME_SINK
    if (!running || !ep) return;
    // 영상을 보내는 호가 있는가 — pjsua 호 목록에서 직접 본다(호 소멸 직후에도 맞게)
    bool anyTx = false;
    pjsua_call_id ids[PJSUA_MAX_CALLS];
    unsigned n = PJ_ARRAY_SIZE(ids);
    if (pjsua_enum_calls(ids, &n) == PJ_SUCCESS) {
        for (unsigned i = 0; i < n && !anyTx; ++i) {
            pjsua_call_info ci;
            if (pjsua_call_get_info(ids[i], &ci) != PJ_SUCCESS) continue;
            for (unsigned mi = 0; mi < ci.media_cnt && !anyTx; ++mi)
                if (ci.media[mi].type == PJMEDIA_TYPE_VIDEO && pjsua_call_vid_stream_is_running(ids[i], (int)mi, PJMEDIA_DIR_ENCODING))
                    anyTx = true;
        }
    }
    const int want = previewWanted && anyTx ? camDev : -2;
    if (want == previewDev) return;
    if (previewDev != -2) {
        try { pj::VideoPreview(previewDev).stop(); } catch (pj::Error& e) { log(3, std::string("self view stop: ") + e.info(false)); }
        log(3, "self view off (cam " + std::to_string(previewDev) + ")");
        previewDev = -2;
    }
    if (want == -2) return;
    try {
        pj::VideoPreviewOpParam p;
        p.show = true;
        p.window.handle.window = kPreviewToken;                         // 프레임 렌더 — 셀프뷰 = callId -1
        pj::VideoPreview(want).start(p);
        previewDev = want;
        log(3, "self view on (cam " + std::to_string(want) + ")");
    } catch (pj::Error& e) { log(2, std::string("self view: ") + e.info(false)); }
#endif
}

void Engine::Impl::videoTick() {
    videoTickArmed = false;
#if PJSUA_HAS_VIDEO
    if (!running || !ep) return;
    bool any = false;
    for (auto& kv : calls) {
        PjCall* c = static_cast<PjCall*>(kv.second.get());
        if (!c || !c->mcvideo) continue;
        pj::CallInfo ci;
        try { ci = c->getInfo(); } catch (...) { continue; }
        for (auto& m : ci.media) {
            if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
            any = true;
            bool txOn = false;
            try { txOn = c->vidStreamIsRunning((int)m.index, PJMEDIA_DIR_ENCODING); } catch (...) {}
            if (txOn) continue;                                          // 송출 중 = 스트림이 영상을 보낸다
            try {
                pj::CallVidSetStreamParam p;
                p.medIdx = (int)m.index;
                c->vidSetStream(PJSUA_CALL_VID_STRM_SEND_KEEPALIVE, p);
            } catch (pj::Error& e) { log(4, std::string("video keep-alive: ") + e.info(false)); }
        }
    }
    if (any) armVideoTick();
#endif
}

void Engine::Impl::armVideoTick(unsigned delayMs) {
#if PJSUA_HAS_VIDEO
    if (!ep || videoTickArmed.exchange(true)) return;
    try { ep->utilTimerSchedule(delayMs, nullptr); }
    catch (pj::Error& e) { videoTickArmed = false; log(2, std::string("video tick: ") + e.info(false)); }
#else
    (void)delayMs;
#endif
}

void Engine::Impl::wireMedia(PjCall* call, int callId) {
    // conference bridge 결선 — 호 → 스피커(listen), 마이크 → 호. MCPTT 반이중은 floor Granted(micOpen)에서만
    // 마이크를 결선한다. 장치 미디어는 Endpoint 소유라 보관하지 않고 매번 재취득.
    pj::AudioMedia* aud = activeAudio(call);
    if (!aud) return;
    CallInfo snap = snapshotCall(callId);
    pj::AudDevManager& adm = ep->audDevManager();
    pj::AudioMedia& spk = adm.getPlaybackDevMedia();
    pj::AudioMedia& mic = adm.getCaptureDevMedia();
    // 송출 원천 — 마이크 또는 기준 음원 재생기(setTxSource, ue_voice_quality.md §4.2). 재생기를 쓰는 동안 마이크는 호에서 뗀다.
    pj::AudioMedia& src = txPlayer ? static_cast<pj::AudioMedia&>(*txPlayer) : mic;
    if (txPlayer) mic.stopTransmit(*aud);
    // 재생 sink — 라우트 0 = 기본 재생 장치, 그 외 = 추가 재생 라우트. 선택되지 않은 sink 와의 결선은 끊는다
    // (미결선 쌍의 disconnect 는 no-op). 라우트가 사라졌으면 기본 장치로 폴백.
    pj::AudioMedia* sink = &spk;
    auto rt = routes.find(snap.playbackRoute);
    if (snap.playbackRoute != 0 && rt != routes.end()) sink = rt->second.get();
    if (sink != &spk) aud->stopTransmit(spk);
    for (auto& kv : routes) if (kv.second.get() != sink) aud->stopTransmit(*kv.second);
    if (snap.listen) aud->startTransmit(*sink); else aud->stopTransmit(*sink);
    // 듣는 크기 = 통화 포트→bridge 유입. pjsua2 AudioMedia 방향은 미디어 관점이라 유입은 adjustTxLevel 이다 — adjustRxLevel 은
    //   bridge→통화(= 상대에게 보내는 내 음성)를 바꾼다(ue_audio_level.md §2). 재협상으로 포트가 새로 생기면 1 로 돌아가므로 매 결선.
    aud->adjustTxLevel(snap.rxLevel);
    bool micOn;
    // 반이중 = floor 가 게이트(Granted), 전이중(mc_no_floor_ctrl) = 앱의 음소거(setMuted — PTT 로컬 게이트, 원천 앱 동작)
    if (call->mcptt) micOn = !call->mcptt->listenOnly && (call->mcptt->fullDuplex ? !snap.muted : call->mcptt->micOpen);
    else if (call->mcvideo) micOn = call->mcvideo->sendOn && !snap.muted;   // MCVideo = 송출 허가('U: has permission')에서만
    else micOn = !snap.muted && !call->recvOnly;
    if (micOn) src.startTransmit(*aud); else src.stopTransmit(*aud);
    // 허가 밖에서는 무음 프레임도 내지 않는다. 허가 중 음소거(setMuted) = 음성 송신만 멈춘다 — 영상은 계속(mcvideo.md §7 D12)
    if (call->mcvideo) setAudioTx(callId, call->mcvideo->sendOn && !snap.muted);
    applyDeviceLevels();                                   // 결선으로 장치가 막 열렸을 수 있다 — 장치 단 음량 재적용
}

void Engine::Impl::applyDeviceLevels() {
    if (!levelsSet || cfg.nullAudioDevice) return;
    pj::AudDevManager& adm = ep->audDevManager();
    // ue_audio_level.md §2 — slot 0 은 캡처·재생 미디어가 같은 객체이고 pjsua2 방향은 미디어 관점이다:
    // adjustRxLevel = bridge → 장치 = 스피커, adjustTxLevel = 장치 → bridge = 마이크. 두 축은 따로 건다(한 축 실패가 다른 축을 지우지 않게).
    try { adm.getPlaybackDevMedia().adjustRxLevel(spkLevel); }
    catch (pj::Error& e) { log(4, std::string("speaker level: ") + e.info(false)); }
    try {
        adm.getCaptureDevMedia().adjustTxLevel(1.f);
        adm.setCaptureAgc(true, (float)micTarget);
    } catch (pj::Error& e) { log(4, std::string("mic agc: ") + e.info(false)); }   // 장치 지연 개방 중 — 다음 결선에서 다시
}

int64_t Engine::Impl::doSendRequest(int accountId, const std::string& method, const std::string& targetUri,
                                 const std::string& contentType, const std::string& body,
                                 const std::map<std::string, std::string>& headers, int64_t token) {
    auto it = accounts.find(accountId);
    if (it == accounts.end()) return -1;
    try {
        pj::SipTxOption tx;
        tx.targetUri = targetUri;
        if (!contentType.empty()) tx.contentType = contentType;
        if (!body.empty()) tx.msgBody = body;
        for (auto& kv : headers) { pj::SipHeader h; h.hName = kv.first; h.hValue = kv.second; tx.headers.push_back(h); }
        pj::SendRequestParam prm;
        prm.method = method;
        prm.txOption = tx;
        prm.userData = (pj::Token)(intptr_t)token;
        it->second->sendRequest(prm);
        return token;
    } catch (pj::Error& e) {
        log(1, method + " " + targetUri + ": " + e.info(false));
        return -1;
    }
}

// ── Engine 공개 API ──

Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() { stop(); }

TlsPeerExpiry Engine::tlsPeerExpiry() const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    return impl_->tlsPeer;
}

std::string Engine::version() { return std::string(CIMSUE_VERSION) + " (pjproject " + pj_get_version() + ")"; }

bool Engine::running() const { return impl_->running; }

Result Engine::start(const EngineConfig& cfg, Listener* listener) {
    if (impl_->running) return Result::fail(-1, "already running");
    impl_->cfg = cfg;
    impl_->fanout.primary = listener;
    impl_->listener = &impl_->fanout;
    impl_->evt.start();
    impl_->ctl.start();
    Result r = impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        try {
            pj_log_set_level(o->cfg.logLevel);               // libInit 전(writer 미설정) pjlib 기본 sink 는 stdout
            o->ep.reset(new PjEndpoint(o));
            o->ep->libCreate();
#if CIMSUE_FRAME_SINK
            pjmedia_cims_frame_dev_set_callback(&onSinkFrame, o);    // 렌더 스트림이 생기기 전에(장치 계약)
            o->previewWanted = false;
            o->previewDev = -2;
#endif
            pj::EpConfig epc;
            epc.uaConfig.userAgent = o->cfg.userAgent;
            epc.logConfig.level = o->cfg.logLevel;
            epc.logConfig.consoleLevel = o->cfg.logLevel;    // pjsua 는 앱 writer 호출도 console_level 로 게이트한다
            std::unique_ptr<pj::LogWriter> writer(new PjLog(o));
            epc.logConfig.writer = writer.get();
            epc.medConfig.noVad = o->cfg.noVad;
            epc.medConfig.clockRate = o->cfg.clockRate;
            // UDP→TCP 승격 스위치 — pjsip 전역, 송신 시점에 읽히므로 libInit 전 설정으로 충분하다.
            pjsip_cfg()->endpt.disable_tcp_switch = o->cfg.udpNoTcpSwitch ? PJ_TRUE : PJ_FALSE;
            o->ep->libInit(epc);
            o->logWriter = writer.release();                 // 이제 pjsua2 소유
            // 송신 직전 SDP 보정(MCVideo 미디어 i=·서비스 호 2xx Require·이어지는 offer fmtp — mcTxFix). 모듈은 endpoint 가 없어질 때
            //   (libDestroy) 함께 풀린다.
            if (pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &g_txFixModule) != PJ_SUCCESS)
                o->log(2, "tx fix module registration failed — MCVideo SDP i= lines will be missing");
            // 착신 MCPTT·MCVideo 초대의 세션 갱신 주체 = uas(mcRxFix)
            if (pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &g_rxFixModule) != PJ_SUCCESS)
                o->log(2, "rx fix module registration failed — MCPTT/MCVideo 200 OK refresher will be uac");
            {
                pj::TransportConfig tc; tc.port = o->cfg.udpPort;
                o->ep->transportCreate(PJSIP_TRANSPORT_UDP, tc);
            }
            try {
                pj::TransportConfig tc; tc.port = o->cfg.tcpPort;
                o->ep->transportCreate(PJSIP_TRANSPORT_TCP, tc);
            } catch (pj::Error& e) { o->log(2, std::string("TCP transport: ") + e.info(false)); }
            try {
                pj::TransportConfig tc; tc.port = o->cfg.tlsPort;
                tc.tlsConfig.CaBuf = o->cfg.tlsCaPem;
                tc.tlsConfig.verifyServer = o->cfg.tlsVerifyServer;
                o->ep->transportCreate(PJSIP_TRANSPORT_TLS, tc);
            } catch (pj::Error& e) { o->log(2, std::string("TLS transport: ") + e.info(false)); }
            if (o->cfg.nullAudioDevice) o->ep->audDevManager().setNullDev();
            o->ep->libStart();
            o->applyCodecPolicy();
            o->captureOn = true;
            o->regRecovery.clear();
            o->running = true;
            o->log(3, std::string("libcimsue ") + version() + " started");
            return Result::success();
        } catch (pj::Error& e) {
            try { if (o->ep) o->ep->libDestroy(); } catch (...) {}
            o->logWriter = nullptr;
            o->ep.reset();
            return fromError(e);
        }
    });
    if (!r.ok) { impl_->ctl.stop(); impl_->evt.stop(); }
    return r;
}

void Engine::stop() {
    if (!impl_->running) return;
    {
        std::lock_guard<std::mutex> lk(impl_->msrpM);                   // media plane 입출력 취소(소켓 대기는 ≤200 ms 안에 본다)
        for (auto& w : impl_->msrpCancels) if (auto c = w.lock()) c->store(true);
    }
    impl_->ctl.runSync([this] {
        Impl* o = impl_.get();
        o->calls.clear();                        // ~Call → hangup, floor participant close
        o->txPlayer.reset();                     // ~AudioMediaPlayer → bridge 포트 해제 (libDestroy 전)
        o->routes.clear();                       // ~ExtraAudioDevice → close (libDestroy 전)
        o->accounts.clear();                     // ~Account → shutdown
        o->mcvideoAffiliations.clear();
        try { o->ep->libDestroy(); } catch (...) {}               // LogWriter 도 여기서 pjsua2 가 delete
#if CIMSUE_FRAME_SINK
        pjmedia_cims_frame_dev_set_callback(nullptr, nullptr);       // 렌더 스트림이 모두 사라진 뒤
        o->previewDev = -2;
#endif
        o->logWriter = nullptr;
        o->ep.reset();
        o->running = false;
        return 0;
    });
    {
        std::unique_lock<std::mutex> lk(impl_->msrpM);                  // 분리 실행한 입출력 스레드가 Impl 을 더 쓰지 않게
        impl_->msrpCv.wait_for(lk, std::chrono::seconds(10), [&] { return impl_->msrpActive == 0; });
        impl_->msrpCancels.clear();
    }
    impl_->emit([o = impl_.get()] { o->listener->onEngineStopped(); });
    impl_->ctl.stop();
    impl_->evt.stop();
    std::lock_guard<std::mutex> lk(impl_->snapM);
    impl_->regInfos.clear();
    impl_->callInfos.clear();
    impl_->finalStats.clear();
    impl_->finalQuality.clear();
    impl_->publishPending.clear();
    impl_->publishEtag.clear();
}

int Engine::addAccount(const AccountConfig& cfg) {
    if (!impl_->running) return -1;
    if (!cfg.isComplete()) { impl_->log(1, "addAccount: config incomplete (host/domain/msisdn/IMPI/cred)"); return -1; }
    return impl_->ctl.runSync([this, cfg]() -> int {
        Impl* o = impl_.get();
        const int id = o->nextAccountId++;
        try {
            std::string note;
            pj::AccountConfig ac = detail::buildPjAccountConfig(cfg, &note);
#if PJSUA_HAS_VIDEO
            if (o->camDev < 0) o->camDev = o->frontCamera();
            ac.videoConfig.defaultCaptureDevice = (pjmedia_vid_dev_index)o->camDev;   // 셀프뷰 = 전면 카메라
#endif
            auto acc = std::make_unique<PjAccount>(o, id);
            o->accountCfgs[id] = cfg;                        // onIncomingCall 이 읽으므로 create 전에
            acc->create(ac, o->accounts.empty());
            o->accounts[id] = std::move(acc);
            { std::lock_guard<std::mutex> lk(o->snapM); o->regInfos[id] = RegInfo{id, RegState::Unregistered, 0, "", 0}; }
            o->log(3, "account " + std::to_string(id) + " " + cfg.aor() + " via " + cfg.serverHost + ":" +
                          std::to_string(cfg.serverPort) + "/" + toString(cfg.transport) + " user=" + cfg.digestUsername() +
                          " mcptt=" + cfg.effectiveMcpttId() + " " + note);
            return id;
        } catch (pj::Error& e) {
            o->accountCfgs.erase(id);
            o->log(1, std::string("addAccount: ") + e.info(false));
            return -1;
        }
    });
}

static Result withAccount(Engine::Impl* o, int id, const std::function<void(pj::Account&)>& f) {
    return o->ctl.runSync([o, id, f]() -> Result {
        auto it = o->accounts.find(id);
        if (it == o->accounts.end()) return Result::fail(-2, "no such account");
        try { f(*it->second); return Result::success(); } catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::registerAccount(int id) {
    if (!impl_->running) return Result::fail(-1, "not running");
    Impl* o = impl_.get();
    Result r = withAccount(o, id, [o, id](pj::Account& a) { a.setRegistration(true); o->regRecovery.want(id); });
    if (r.ok) {
        std::lock_guard<std::mutex> lk(impl_->snapM);
        impl_->regInfos[id].state = RegState::Registering;
    }
    return r;
}
Result Engine::unregisterAccount(int id) {
    if (!impl_->running) return Result::fail(-1, "not running");
    Impl* o = impl_.get();
    return withAccount(o, id, [o, id](pj::Account& a) { o->regRecovery.unwant(id); a.setRegistration(false); });
}
Result Engine::refreshRegistration(int id) { return registerAccount(id); }

Result Engine::handleNetworkChange() {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        // ① TCP/TLS 연결을 닫는다 — 옛 망의 연결을 재사용하지 않게(새 REGISTER 가 새 연결을 연다). UDP 는 0.0.0.0 에 묶여
        //    있어 그대로 쓰고, 낡은 Via/Contact 는 rport·Contact 재작성이 고친다(account_map.cpp natConfig).
        //    수신 소켓 재시작(pjsua IP 변경 처리)은 하지 않는다 — 재시작 실패 경로가 계정 처리로 이어지며 전송이 빈 regc 를
        //    역참조한다(pjsua_core.c handle_ip_change_on_acc).
        pjsip_tpmgr_shutdown_param sd;
        pjsip_tpmgr_shutdown_param_default(&sd);
        sd.include_udp = PJ_FALSE;
        pj_status_t st = pjsip_tpmgr_shutdown_all(pjsip_endpt_get_tpmgr(pjsua_get_pjsip_endpt()), &sd);
        if (st != PJ_SUCCESS) o->log(2, "network change: transport shutdown " + std::to_string(st));
        // ② 등록을 켠 계정마다 다시 등록 — 걸려 있으면 끝난 뒤 한 번 더(RegRecovery). 일반 등록 경로라 실패하면
        //    pjsua 자동 재시도(regConfig.retryIntervalSec)가 그대로 산다.
        for (int id : o->regRecovery.targets()) o->reRegister(id);
        return Result::success();
    });
}

Result Engine::removeAccount(int id) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, id]() -> Result {
        Impl* o = impl_.get();
        if (!o->accounts.erase(id)) return Result::fail(-2, "no such account");
        o->accountCfgs.erase(id);
        o->mcvideoAffiliations.erase(id);
        o->regRecovery.unwant(id);
        std::lock_guard<std::mutex> lk(o->snapM);
        o->regInfos.erase(id);
        return Result::success();
    });
}

RegInfo Engine::regInfo(int id) const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    auto it = impl_->regInfos.find(id);
    return it == impl_->regInfos.end() ? RegInfo{} : it->second;
}

std::vector<int> Engine::accounts() const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    std::vector<int> v;
    for (auto& kv : impl_->regInfos) v.push_back(kv.first);
    return v;
}

/**
 * 호 미디어 설정 — 음성 1 + 영상 0/1. 영상 키프레임 요청은 **RTCP PLI 만** 쓴다(TS 26.114 §7.3 — MTSI 영상 코덱
 * 제어 = RTCP AVPF PLI/FIR, RFC 4585·5104). pjsua 기본값은 SIP INFO(RFC 5168 `media_control+xml`)도 보내는데, 서버는
 * INFO 를 Allow 에 두지 않아 501 로 끝난다(RFC 3261 §8.2.1) — 요청이 상대에게 가지 않고 신호만 늘린다.
 */
static void setCallMedia(pj::CallSetting& opt, bool video) {
    opt.audioCount = 1;
    opt.videoCount = video ? 1 : 0;
    opt.reqKeyframeMethod = PJSUA_VID_REQ_KEYFRAME_RTCP_PLI;
}

/**
 * MCVideo 호 미디어 — audio + video(TS 24.281 §6.2.1 2)·3)) + 제어 채널 자리(pjsua text 슬롯 — 제어 채널 섹션으로 바뀐다).
 * 영상 없는 빌드(Linux 헤드리스·Windows 1차 — config_site PJMEDIA_HAS_VIDEO 0)는 pjsua 영상 슬롯이 없어 text 슬롯 둘을 두고 첫째를
 * port 0 영상 자리(mcvideo::kVideoPlaceholderSdp)로 바꾼다 — m-line 수·순서(K4)는 같고 음성·전송 제어는 그대로 협상된다(ue_sdk.md §4.6).
 */
static void setMcVideoMedia(pj::CallSetting& opt) {
#if PJSUA_HAS_VIDEO
    setCallMedia(opt, true);
#else
    setCallMedia(opt, false);
    opt.textCount = 2;
#endif
}

int Engine::dial(int accountId, const std::string& target, const CallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, target, opts]() -> int {
        Impl* o = impl_.get();
        auto it = o->accounts.find(accountId);
        if (it == o->accounts.end()) { o->log(1, "dial: no such account"); return -1; }
        const std::string dst = detail::normalizeTarget(target, o->accountCfgs[accountId].domain);
        auto call = std::make_unique<PjCall>(o, *it->second, accountId);
        try {
            pj::CallOpParam prm(true);
            setCallMedia(prm.opt, opts.video);
            call->makeCall(dst, prm);
        } catch (pj::Error& e) {
            o->log(1, std::string("dial ") + dst + ": " + e.info(false));
            return -1;
        }
        const int id = call->getId();
        call->sealCallId(id);
        o->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
            c.remoteUri = dst; c.video = opts.video;
        });
        o->calls[id] = std::move(call);
        o->log(3, "dial " + dst + " → call " + std::to_string(id));
        return id;
    });
}

static Result withCall(Engine::Impl* o, int callId, const std::function<void(PjCall&)>& f) {
    if (!o->running) return Result::fail(-1, "not running");
    return o->ctl.runSync([o, callId, f]() -> Result {
        PjCall* c = o->findCall(callId);
        if (!c) return Result::fail(-2, "no such call");
        try { f(*c); return Result::success(); } catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::answer(int callId, const CallOptions& opts) {
    return withCall(impl_.get(), callId, [&](PjCall& c) {
        pj::CallOpParam prm(true);
        prm.statusCode = PJSIP_SC_OK;
        if (c.mcvideo) setMcVideoMedia(prm.opt);                           // MCVideo 호 = audio + video(TS 24.281 §6.2.2 1)) — 수동 수락도 같다
        else setCallMedia(prm.opt, opts.video);
        c.answer(prm);
    });
}
Result Engine::reject(int callId, int statusCode) {
    return withCall(impl_.get(), callId, [&](pj::Call& c) {
        pj::CallOpParam prm;
        prm.statusCode = (pjsip_status_code)statusCode;
        c.hangup(prm);
    });
}
Result Engine::hangup(int callId) {
    return withCall(impl_.get(), callId, [](pj::Call& c) { pj::CallOpParam prm; c.hangup(prm); });
}
Result Engine::hold(int callId) {
    return withCall(impl_.get(), callId, [](pj::Call& c) { pj::CallOpParam prm; c.setHold(prm); });
}
Result Engine::resume(int callId) {
    return withCall(impl_.get(), callId, [](pj::Call& c) {
        pj::CallOpParam prm(true);
        prm.opt.flag |= PJSUA_CALL_UNHOLD;
        prm.opt.reqKeyframeMethod = PJSUA_VID_REQ_KEYFRAME_RTCP_PLI;   // 재초대의 설정이 호 설정을 대신한다(setCallMedia)
        c.reinvite(prm);
    });
}
/**
 * 명령이 바꾼 스냅샷을 **앱에 알린다**.
 *
 * `setMuted`·`setListen`·`setRxLevel`·`setCallRoute` 는 `CallInfo` 를 바꾸지만 SIP 상태가 바뀌지 않아
 * `onCallState` 가 뒤따르지 않는다. 알리지 않으면 앱은 명령 전 스냅샷을 그대로 들고 있어 **토글이
 * 화면에 반영되지 않고**, 다음 누름이 같은 값을 다시 보내 해제되지 않는다(관제 앱 음소거 증상).
 * 미디어 이벤트 축으로 낸다 — 상태 전이가 아니라 미디어 배치의 변화이기 때문이다.
 */
static void emitMediaSnapshot(Engine::Impl* o, int callId) {
    CallInfo snap = o->snapshotCall(callId);
    o->emit([o, snap] { o->listener->onCallMedia(snap); });
}

Result Engine::setMuted(int callId, bool muted) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, muted](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.muted = muted; });
        o->wireMedia(&c, callId);
        emitMediaSnapshot(o, callId);
    });
}
Result Engine::setListen(int callId, bool listen) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, listen](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.listen = listen; });
        o->wireMedia(&c, callId);
        emitMediaSnapshot(o, callId);
    });
}
Result Engine::setRxLevel(int callId, float level) {
    if (level < 0) return Result::fail(-2, "negative level");
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, level](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.rxLevel = level; });
        // 오디오가 있으면 곧바로, 없으면(성립 전·보류) 다음 결선(wireMedia)에서 건다.
        if (pj::AudioMedia* aud = o->activeAudio(&c)) aud->adjustTxLevel(level);
        emitMediaSnapshot(o, callId);
    });
}
Result Engine::sendDtmf(int callId, const std::string& digits) {
    return withCall(impl_.get(), callId, [&](pj::Call& c) { c.dialDtmf(digits); });
}

CallInfo Engine::callInfo(int callId) const { return impl_->snapshotCall(callId); }

std::vector<int> Engine::calls() const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    std::vector<int> v;
    for (auto& kv : impl_->callInfos) v.push_back(kv.first);
    return v;
}

StreamStats Engine::streamStats(int callId) const {
    StreamStats s;
    if (!impl_->running) return s;
    auto finalOf = [this, callId]() {
        std::lock_guard<std::mutex> lk(impl_->snapM);
        auto it = impl_->finalStats.find(callId);
        return it == impl_->finalStats.end() ? StreamStats{} : it->second;
    };
    return impl_->ctl.runSync([this, callId, s, finalOf]() mutable -> StreamStats {
        Impl* o = impl_.get();
        PjCall* c = o->findCall(callId);
        if (!c) return finalOf();
        try {
            unsigned idx = 0;
            if (!o->activeAudio(c, &idx)) return finalOf();
            return Impl::fromPj(c->getStreamStat(idx));
        } catch (...) { return finalOf(); }
    });
}

void Engine::addObserver(Listener* l) { impl_->fanout.add(l); }
void Engine::removeObserver(Listener* l) { impl_->fanout.remove(l); }

Result Engine::setTxSource(const std::string& wavPath) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, wavPath]() -> Result {
        Impl* o = impl_.get();
        std::unique_ptr<pj::AudioMediaPlayer> next;
        if (!wavPath.empty()) {
            try {
                next.reset(new pj::AudioMediaPlayer());
                next->createPlayer(wavPath, 0);              // 0 = 끝나면 처음부터 반복
            } catch (pj::Error& e) { return Result::fail(-1, "tx source: " + e.info(false)); }
        }
        // 지금 원천을 모든 호에서 떼고 바꾼 뒤 다시 결선한다(마이크 ↔ 재생기).
        pj::AudioMedia& mic = o->ep->audDevManager().getCaptureDevMedia();
        for (auto& kv : o->calls) {
            PjCall* c = static_cast<PjCall*>(kv.second.get());
            pj::AudioMedia* aud = nullptr;
            try { aud = o->activeAudio(c); } catch (...) {}
            if (!aud) continue;
            try { if (o->txPlayer) o->txPlayer->stopTransmit(*aud); else mic.stopTransmit(*aud); } catch (pj::Error&) {}
        }
        o->txPlayer = std::move(next);
        for (auto& kv : o->calls) {
            try { o->wireMedia(static_cast<PjCall*>(kv.second.get()), kv.first); } catch (pj::Error&) {}
        }
        o->log(3, wavPath.empty() ? "tx source: microphone" : "tx source: " + wavPath);
        return Result::success();
    });
}

CallQuality Engine::callQuality(int callId) const {
    if (!impl_->running) return CallQuality{};
    auto finalOf = [this, callId]() {
        std::lock_guard<std::mutex> lk(impl_->snapM);
        auto it = impl_->finalQuality.find(callId);
        return it == impl_->finalQuality.end() ? CallQuality{} : it->second;
    };
    return impl_->ctl.runSync([this, callId, finalOf]() -> CallQuality {
        Impl* o = impl_.get();
        PjCall* c = o->findCall(callId);
        if (!c) return finalOf();
        try {
            unsigned idx = 0;
            if (!o->activeAudio(c, &idx)) return finalOf();
            return quality::merge(finalOf(), Impl::measure(c, idx));   // 소멸한 앞 스트림 + 현재 스트림
        } catch (...) { return finalOf(); }
    });
}

// ── MCPTT ──

static int startMcptt(Engine::Impl* o, int accountId, const std::string& id, bool isPrivate, const GroupCallOptions& opts) {
    auto it = o->accounts.find(accountId);
    if (it == o->accounts.end()) { o->log(1, "mcptt: no such account"); return -1; }
    const AccountConfig& cfg = o->accountCfgs[accountId];
    for (auto& kv : o->calls) {                                          // 같은 세션 중복 방지
        PjCall* c = static_cast<PjCall*>(kv.second.get());
        if (c->mcptt && c->mcptt->groupId == id && c->mcptt->isPrivate == isPrivate) return kv.first;
    }
    auto call = std::make_unique<PjCall>(o, *it->second, accountId);
    call->mcptt.reset(new McpttSession);
    call->mcptt->groupId = id;
    call->mcptt->isPrivate = isPrivate;
    call->mcptt->fullDuplex = isPrivate && opts.fullDuplex;
    call->mcptt->listenOnly = opts.listenOnly;
    call->mcptt->emergency = opts.emergency;
    call->mcptt->imminentPeril = opts.imminentPeril && !opts.emergency;       // 긴급이 임박을 대체
    call->mcptt->condMine = opts.emergency || opts.imminentPeril;
    call->mcptt->broadcast = !isPrivate && opts.broadcast;               // 일제 통화는 그룹 호 속성(TS 24.379 §4.12)
    const std::string mcpttId = cfg.effectiveMcpttId();
    // floor 소켓은 makeCall 전에 — makeCall 이 동기적으로 onCallSdpCreated 를 부르며 로컬 offer 에 포트를 광고한다.
    if (!call->mcptt->fullDuplex) {
        if (!call->openFloor(mcpttId)) { o->log(1, "floor socket bind failed"); return -1; }
        if (call->mcptt->broadcast) call->mcptt->floor->setBroadcastInitiator(true);
        // 암묵적 발언 요청(TS 24.380 §14.2.5) — 개시 INVITE 가 요청을 싣고 floor 는 'U: pending Request'(§6.2.4.2.2 4.)
        const bool implicitReq = opts.implicitFloorRequest && !opts.listenOnly;
        if (implicitReq) { call->mcptt->floor->armImplicitRequest(opts.emergency); call->mcptt->implicitAwaitAnswer = true; }
        call->mcptt->pendingAppSdp = mcptt::floorSdp(call->mcptt->floor->localPort(), false, implicitReq);
    } else {
        call->mcptt->micOpen = true;
    }
    try {
        pj::CallOpParam prm(true);
        prm.opt.audioCount = 1;
        prm.opt.videoCount = 0;                                          // MCPTT 호는 음성만 — 그룹 영상은 MCVideo 호(mcvideo.md §8)
        prm.txOption.multipartContentType.type = "multipart";
        prm.txOption.multipartContentType.subType = "mixed";
        pj::SipMultipartPart p1;
        p1.contentType.type = "application"; p1.contentType.subType = "vnd.3gpp.mcptt-info+xml";
        // 지시자 조합(TS 24.379 §6.3.3.1.17) — 긴급 개시는 alert-ind 를 함께 싣고(경보를 요청하지 않았으면 false, §6.2.8.1.1 4)),
        //   임박은 긴급·경보 지시자 없이(§6.2.8.1.9). 둘 다 요청하면 긴급이 임박을 대체한다(위 mcptt->imminentPeril).
        p1.body = mcptt::mcpttInfo(isPrivate ? "private" : "prearranged", "tel:" + id, mcpttId, "tel:" + id,
                                   call->mcptt->emergency ? 1 : 0, call->mcptt->imminentPeril ? 1 : 0, call->mcptt->broadcast,
                                   call->mcptt->emergency ? -1 : 0);
        prm.txOption.multipartParts.push_back(p1);
        // 우선 그룹콜의 Resource-Priority(TS 24.379 §6.2.8.1.2·§6.2.8.1.12) — 값 = service-config(§6.2.8.1.15, AccountConfig.rp*)
        if (call->mcptt->emergency || call->mcptt->imminentPeril) {
            pj::SipHeader rp; rp.hName = "Resource-Priority";
            rp.hValue = call->mcptt->emergency ? cfg.rpEmergency : cfg.rpImminentPeril;
            if (!rp.hValue.empty()) prm.txOption.headers.push_back(rp);
        }
        if (!opts.members.empty()) {
            pj::SipMultipartPart p2;
            p2.contentType.type = "application"; p2.contentType.subType = "resource-lists+xml";
            p2.body = mcptt::resourceLists(opts.members);
            prm.txOption.multipartParts.push_back(p2);
        }
        call->makeCall("sip:" + id + "@" + cfg.domain, prm);
        // makeCall 이 개시 offer 를 동기적으로 만들었다 — 이어지는 offer(re-INVITE)·answer 는 암묵 요청·mc_granted 없이(§14.5)
        if (call->mcptt->floor) call->mcptt->pendingAppSdp = mcptt::floorSdp(call->mcptt->floor->localPort(), false);
    } catch (pj::Error& e) {
        o->log(1, std::string("mcptt invite ") + id + ": " + e.info(false));
        return -1;
    }
    const int callId = call->getId();
    call->sealCallId(callId);
    o->updateCall(callId, [&](CallInfo& c) {
        c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
        c.remoteUri = "sip:" + id + "@" + cfg.domain;
        call->projectMcptt(c);                                            // onCallState(CALLING) 가 먼저 투영했으면 no-op
    });
    const bool broadcast = call->mcptt->broadcast;
    o->calls[callId] = std::move(call);
    o->log(3, std::string(isPrivate ? "private call " : broadcast ? "broadcast group call " : "group call ") + id + " → call " +
                  std::to_string(callId));
    return callId;
}

int Engine::joinGroupCall(int accountId, const std::string& groupId, const GroupCallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, groupId, opts] { return startMcptt(impl_.get(), accountId, groupId, false, opts); });
}
int Engine::startPrivateCall(int accountId, const std::string& peer, const GroupCallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, peer, opts] { return startMcptt(impl_.get(), accountId, mcptt::bareId(peer), true, opts); });
}

Result Engine::floorRequest(int callId, int priority) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, priority](PjCall& c) {
        if (!c.mcptt) throw pj::Error(PJ_EINVALIDOP, "floorRequest", "not an MCPTT session", __FILE__, __LINE__);
        if (c.mcptt->fullDuplex) return;                                     // 전이중 — 마이크 상시, floor 없음
        if (!c.mcptt->floor) throw pj::Error(PJ_EINVALIDOP, "floorRequest", "no floor participant", __FILE__, __LINE__);
        c.mcptt->floor->request(priority, c.mcptt->emergency);             // 세션 조건 현재값(상향·재광고 반영)
    });
}
Result Engine::floorRelease(int callId) {
    return withCall(impl_.get(), callId, [](PjCall& c) {
        if (c.mcptt && c.mcptt->floor) c.mcptt->floor->release();
    });
}
Result Engine::floorQueueCancel(int callId) {
    return withCall(impl_.get(), callId, [](PjCall& c) {
        if (c.mcptt && c.mcptt->floor) c.mcptt->floor->cancelQueued();
    });
}
FloorInfo Engine::floorInfo(int callId) const {
    FloorInfo fi;
    if (!impl_->running) return fi;
    return impl_->ctl.runSync([this, callId, fi]() mutable {
        PjCall* c = impl_->findCall(callId);
        if (c && c->mcptt && c->mcptt->floor) fi = c->mcptt->floor->info();
        return fi;
    });
}

// ── MCVideo 그룹 호 (TS 24.281 §9.2.1 prearranged · §9.2.2 chat, 전송 제어 TS 24.581 — mcvideo.md §5.4) ──

static int startMcVideo(Engine::Impl* o, int accountId, const std::string& groupId, const VideoGroupCallOptions& opts) {
    auto it = o->accounts.find(accountId);
    if (it == o->accounts.end()) { o->log(1, "mcvideo: no such account"); return -1; }
    const AccountConfig& cfg = o->accountCfgs[accountId];
    const bool rejoin = !opts.sessionUri.empty();
    if (!rejoin && cfg.mcvideoServerUri.empty()) { o->log(1, "mcvideo: no MCVideo server URI (ue-init-config)"); return -1; }
    const std::string gid = mcptt::bareId(groupId);
    for (auto& kv : o->calls) {                                          // 같은 그룹의 MCVideo 호 중복 방지
        PjCall* c = static_cast<PjCall*>(kv.second.get());
        if (c->mcvideo && c->mcvideo->groupId == gid) return kv.first;
    }
    auto call = std::make_unique<PjCall>(o, *it->second, accountId);
    call->mcvideo.reset(new McVideoSession);
    McVideoSession& mv = *call->mcvideo;
    mv.groupId = gid;
    mv.prearranged = opts.prearranged || rejoin;
    // 제어 채널 소켓은 makeCall 전에 — makeCall 이 동기적으로 onCallSdpCreated 를 부르며 offer 에 포트를 광고한다.
    if (!call->openTc(cfg.effectiveMcpttId())) { o->log(1, "mcvideo tc socket bind failed"); return -1; }
    const bool implicitReq = opts.implicitTransmissionRequest && !rejoin;
    mcvideo::TcFmtp f;                                                   // TS 24.581 §14.2
    f.queueing = opts.queueing;
    f.priority = opts.maxPriority;
    f.receptionPriority = opts.maxReceptionPriority;
    f.granted = implicitReq;                                             // §14.2.4 — 200 OK 허가 표시를 받을 수 있다
    f.implicitRequest = implicitReq;                                     // §14.2.5
    f.hasTcSsrc = true;                                                  // §14.2.7 · TS 24.281 §6.2.1 4)b)
    f.tcSsrc = mv.tc->localSsrc();
    if (implicitReq) { mv.tc->armImplicitRequest(); mv.implicitAwaitAnswer = true; }
    mv.pendingAppSdp = mcvideo::controlSdp(mv.tc->localPort(), f);
    const std::string target = rejoin ? opts.sessionUri : cfg.mcvideoServerUri;
    try {
        pj::CallOpParam prm(true);
        setMcVideoMedia(prm.opt);                                        // m=audio + m=video(§6.2.1 2)·3)) + 제어 채널 자리
        auto hdr = [&](const char* n, const std::string& v) { pj::SipHeader h; h.hName = n; h.hValue = v; prm.txOption.headers.push_back(h); };
        hdr("Accept-Contact", mcvideo::acceptContactFeature());         // §9.2.1.2.1.1 · §9.2.2.2.1.1 — require;explicit 둘
        hdr("Accept-Contact", mcvideo::acceptContactIcsi());
        hdr("P-Preferred-Service", mcvideo::kIcsi);
        prm.txOption.multipartContentType.type = "multipart";
        prm.txOption.multipartContentType.subType = "mixed";
        pj::SipMultipartPart p1;
        p1.contentType.type = "application"; p1.contentType.subType = "vnd.3gpp.mcvideo-info+xml";
        mcvideo::InfoParams ip;
        ip.sessionType = mv.prearranged ? "prearranged" : "chat";        // Annex F.1.3 — 그룹 문서 invite-members 와 맞아야 한다(§6.3.5.2)
        ip.requestUri = "tel:" + gid;
        ip.clientId = cfg.effectiveMcpttClientId();                       // 단일 MC 서비스 신원(mcvideo.md §7 D1)
        p1.body = mcvideo::info(ip);
        prm.txOption.multipartParts.push_back(p1);
        // name-addr 로 넘긴다 — addr-spec 이면 pjsip 이 To 를 꺾쇠 없이 찍어 세션 식별자의 `;gr=` 가 To 헤더 파라미터로 읽힌다
        //   (RFC 3261 §20 — URI 에 `;`·`,`·`?` 가 있으면 `<>` 로 감싼다). R-URI 는 그대로 URI 다.
        call->makeCall(target.front() == '<' ? target : "<" + target + ">", prm);
        // makeCall 이 개시 offer 를 동기적으로 만들었다 — 이어지는 offer(re-INVITE)는 mc_granted·mc_implicit_request 없이(§14.5)
        f.granted = false;
        f.implicitRequest = false;
        mv.pendingAppSdp = mcvideo::controlSdp(mv.tc->localPort(), f);
    } catch (pj::Error& e) {
        o->log(1, std::string("mcvideo invite ") + gid + ": " + e.info(false));
        return -1;
    }
    const int callId = call->getId();
    call->sealCallId(callId);
    o->updateCall(callId, [&](CallInfo& c) {
        c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
        c.remoteUri = target;
        c.video = PJSUA_HAS_VIDEO != 0;                                  // 영상 없는 빌드는 port 0 자리만
        call->projectMcVideo(c);
    });
    o->calls[callId] = std::move(call);
    o->log(3, std::string("mcvideo ") + (rejoin ? "rejoin " : mv.prearranged ? "prearranged group call " : "chat group call ") + gid +
                  " → call " + std::to_string(callId));
    return callId;
}

int Engine::joinVideoGroupCall(int accountId, const std::string& groupId, const VideoGroupCallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, groupId, opts] { return startMcVideo(impl_.get(), accountId, groupId, opts); });
}

/** MCVideo 호의 participant 에 명령 — ue-ctl 에서 호를 찾고 결과를 그대로 돌려준다. */
static Result withTc(Engine::Impl* o, int callId, const std::function<Result(mcvideo::Participant&)>& f) {
    if (!o->running) return Result::fail(-1, "not running");
    return o->ctl.runSync([o, callId, f]() -> Result {
        PjCall* c = o->findCall(callId);
        if (!c) return Result::fail(-2, "no such call");
        if (!c->mcvideo || !c->mcvideo->tc) return Result::fail(-2, "not an MCVideo call");
        return f(*c->mcvideo->tc);
    });
}

Result Engine::requestTransmission(int callId, int priority) {
    return withTc(impl_.get(), callId, [priority](mcvideo::Participant& p) { return p.requestTransmission(priority); });
}
Result Engine::releaseTransmission(int callId) {
    return withTc(impl_.get(), callId, [](mcvideo::Participant& p) { return p.releaseTransmission(); });
}
Result Engine::acceptReception(int callId, const std::string& transmitterId, int priority) {
    return withTc(impl_.get(), callId, [transmitterId, priority](mcvideo::Participant& p) { return p.acceptReception(transmitterId, priority); });
}
Result Engine::endReception(int callId, const std::string& transmitterId) {
    return withTc(impl_.get(), callId, [transmitterId](mcvideo::Participant& p) { return p.endReception(transmitterId); });
}
TransmissionInfo Engine::transmissionInfo(int callId) const {
    if (!impl_->running) return TransmissionInfo();
    return impl_->ctl.runSync([this, callId]() -> TransmissionInfo {
        PjCall* c = impl_->findCall(callId);
        return c && c->mcvideo && c->mcvideo->tc ? c->mcvideo->tc->info() : TransmissionInfo();
    });
}

Result Engine::setCallCondition(int callId, bool emergency, bool imminentPeril) {
    if (emergency && imminentPeril) return Result::fail(-2, "emergency and imminent peril are exclusive");
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, emergency, imminentPeril](PjCall& c) {
        if (!c.mcptt || c.mcptt->isPrivate)
            throw pj::Error(PJ_EINVALIDOP, "setCallCondition", "not a group call", __FILE__, __LINE__);
        McpttSession& m = *c.mcptt;
        if (m.condPending) throw pj::Error(PJ_EBUSY, "setCallCondition", "condition change pending", __FILE__, __LINE__);
        if (o->snapshotCall(callId).state != CallState::Active)
            throw pj::Error(PJ_EINVALIDOP, "setCallCondition", "call not active", __FILE__, __LINE__);
        if (m.emergency == emergency && m.imminentPeril == imminentPeril) return;           // 바뀐 것 없음 — 보내지 않는다
        // 긴급 → 임박은 한 요청으로 못 한다 — 임박 상향은 긴급이 없을 때만(§6.2.8.1.9 1)), 지시자 조합도 성립하지 않는다(§6.3.3.1.17)
        if (m.emergency && imminentPeril)
            throw pj::Error(PJ_EINVALIDOP, "setCallCondition", "cancel emergency before imminent peril", __FILE__, __LINE__);
        // 바뀐 지시자만 true/false 로 명시(§6.2.8.1.1·§6.2.8.1.3·§6.2.8.1.9·§6.2.8.1.11). 임박 → 긴급 상향은 emergency-ind true 만
        //   싣는다 — 임박은 제어 기능이 내린다(§6.3.3.1.6 3)d)), imminentperil-ind 를 함께 싣으면 §6.3.3.1.17 위반(403 150).
        //   긴급 상향은 alert-ind 를 동반한다 — 경보를 요청하지 않으므로 false(§6.2.8.1.1 4)).
        const int e = m.emergency == emergency ? 0 : (emergency ? 1 : -1);
        const int i = emergency || m.imminentPeril == imminentPeril ? 0 : (imminentPeril ? 1 : -1);
        const int a = e > 0 ? -1 : 0;
        const AccountConfig& cfg = o->accountCfgs[c.accountId()];
        m.prevEmergency = m.emergency; m.prevImminent = m.imminentPeril; m.prevMine = m.condMine;
        m.emergency = emergency; m.imminentPeril = imminentPeril;
        m.condMine = emergency || imminentPeril;
        m.condPending = true;
        m.condTsx = nullptr;
        m.condLastCode = 0;
        c.publishCondition(ConditionCause::Local);
        try {
            pj::CallOpParam prm(true);
            prm.opt.audioCount = 1;
            prm.opt.videoCount = 0;
            prm.txOption.multipartContentType.type = "multipart";
            prm.txOption.multipartContentType.subType = "mixed";
            pj::SipMultipartPart p1;
            p1.contentType.type = "application"; p1.contentType.subType = "vnd.3gpp.mcptt-info+xml";
            p1.body = mcptt::mcpttInfo("prearranged", "tel:" + m.groupId, cfg.effectiveMcpttId(), "tel:" + m.groupId, e, i, false, a);
            prm.txOption.multipartParts.push_back(p1);
            pj::SipHeader rp; rp.hName = "Resource-Priority";                           // §6.2.8.1.2 — 하향은 normal 값(§6.2.8.1.15)
            rp.hValue = emergency ? cfg.rpEmergency : imminentPeril ? cfg.rpImminentPeril : cfg.rpNormal;
            if (!rp.hValue.empty()) prm.txOption.headers.push_back(rp);
            c.reinvite(prm);                                                             // SDP = 협상 그대로 + floor 섹션 재주입
        } catch (pj::Error&) {
            m.emergency = m.prevEmergency; m.imminentPeril = m.prevImminent; m.condMine = m.prevMine;
            m.condPending = false;
            c.publishCondition(ConditionCause::Denied);
            throw;
        }
        o->log(3, "call " + std::to_string(callId) + " condition → emergency=" + (emergency ? "1" : "0") +
                      " imminent=" + (imminentPeril ? "1" : "0"));
    });
}

int64_t Engine::sendEmergencyAlert(int accountId, const std::string& groupId, bool activate,
                                   const std::string& originatedBy, bool cancelGroupEmergency) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> int64_t {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return -1;
        const AccountConfig& cfg = ic->second;
        const std::string gid = mcptt::bareId(groupId);
        // ICSI mcptt(§12.1.1.1 1)·2)). Request-URI = 참여 기능 PSI(§12.1.1.1 8)), 대상 그룹 = 본문 mcptt-request-uri.
        //   PSI 를 모르면(mcpttServerUri 비어 있음) 그룹 URI — CSP 0.2.166 전 서버는 Request-URI 그룹으로만 받는다.
        std::map<std::string, std::string> h;
        h["P-Preferred-Service"] = mcptt::kIcsiMcptt;
        h["Accept-Contact"] = std::string("*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit");
        std::string ob = originatedBy.empty() ? std::string()
                       : (originatedBy.find(':') == std::string::npos ? "tel:" + originatedBy : originatedBy);
        std::string body = mcptt::alertInfo("tel:" + gid, cfg.effectiveMcpttId(), cfg.effectiveMcpttClientId(), activate, ob,
                                            (!activate && cancelGroupEmergency) ? -1 : 0);
        const std::string target = cfg.mcpttServerUri.empty() ? "sip:" + gid + "@" + cfg.domain : cfg.mcpttServerUri;
        return o->doSendRequest(accountId, "MESSAGE", target, mcptt::kCtMcpttInfo, body, h, token);
    });
}

int64_t Engine::sendRequest(int accountId, const std::string& method, const std::string& targetUri,
                            const std::string& contentType, const std::string& body,
                            const std::map<std::string, std::string>& headers) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=] { return impl_->doSendRequest(accountId, method, targetUri, contentType, body, headers, token); });
}

int64_t Engine::Impl::sendAffiliation(int accountId, const std::string& groupId, bool on, int64_t token, int64_t appToken,
                                      bool allowConditional) {
    auto ic = accountCfgs.find(accountId);
    if (ic == accountCfgs.end()) return -1;
    std::map<std::string, std::string> h;
    h["Event"] = "mcptt";                                              // TS 24.379 §9 — 없으면 CSP 489
    h["Expires"] = on ? "3600" : "0";
    {
        std::lock_guard<std::mutex> lk(snapM);
        PendingPublish p;
        p.accountId = accountId; p.groupId = groupId; p.on = on; p.appToken = appToken;
        auto et = publishEtag.find(publishKey(accountId, groupId, McService::Mcptt));
        if (allowConditional && et != publishEtag.end()) { h["SIP-If-Match"] = et->second; p.conditional = true; }
        publishPending[token] = p;
    }
    int64_t r = doSendRequest(accountId, "PUBLISH", "sip:" + groupId + "@" + ic->second.domain, mcptt::kCtAffiliation,
                              mcptt::affiliationCommand("tel:" + groupId, on), h, token);
    if (r < 0) { std::lock_guard<std::mutex> lk(snapM); publishPending.erase(token); }
    return r < 0 ? -1 : appToken;
}

int64_t Engine::Impl::sendMcVideoAffiliation(int accountId, int64_t token, int64_t appToken, bool allowConditional) {
    auto ic = accountCfgs.find(accountId);
    if (ic == accountCfgs.end()) return -1;
    const AccountConfig& cfg = ic->second;
    const std::string clientId = cfg.effectiveMcpttClientId();
    if (cfg.mcvideoServerUri.empty() || clientId.empty()) {
        log(1, std::string("mcvideo affiliation: ") + (cfg.mcvideoServerUri.empty() ? "no MCVideo server URI" : "no MC client ID"));
        return -1;
    }
    const std::set<std::string>& groups = mcvideoAffiliations[accountId];
    std::vector<std::string> uris;
    for (const auto& g : groups) uris.push_back("tel:" + g);
    // TS 24.281 §8.2.1.2 — R-URI = 참여 MCVideo 기능 PSI(1), mcvideo-info request-uri = 자기 MCVideo ID(2), ICSI(3),
    //   Expires = 관심 그룹이 있으면 2^32-1 · 없으면 0(4·5), pidf = 관심 그룹 전부 · client ID · 유일 p-id(6), Event presence(RFC 3856).
    std::map<std::string, std::string> h;
    h["P-Preferred-Service"] = mcvideo::kIcsi;
    h["Event"] = "presence";
    h["Expires"] = groups.empty() ? "0" : mcvideo::kAffiliationExpires;
    {
        std::lock_guard<std::mutex> lk(snapM);
        PendingPublish p;
        p.accountId = accountId; p.on = !groups.empty(); p.appToken = appToken; p.service = McService::McVideo;
        auto et = publishEtag.find(publishKey(accountId, std::string(), McService::McVideo));
        if (allowConditional && et != publishEtag.end()) { h["SIP-If-Match"] = et->second; p.conditional = true; }
        publishPending[token] = p;
    }
    mcvideo::InfoParams ip;
    ip.requestUri = cfg.effectiveMcpttId();
    const std::string boundary = "mcv-pub-" + mcdata::newMessageId().substr(0, 12);
    const std::string body = "--" + boundary + "\r\nContent-Type: " + mcvideo::kCtInfo + "\r\n\r\n" + mcvideo::info(ip) + "\r\n" +
                             "--" + boundary + "\r\nContent-Type: " + mcvideo::kCtPidf + "\r\n\r\n" +
                             mcvideo::affiliationPidf(cfg.effectiveMcpttId(), clientId, uris, mcdata::newMessageId()) + "\r\n" +
                             "--" + boundary + "--\r\n";
    int64_t r = doSendRequest(accountId, "PUBLISH", cfg.mcvideoServerUri, "multipart/mixed;boundary=" + boundary, body, h, token);
    if (r < 0) { std::lock_guard<std::mutex> lk(snapM); publishPending.erase(token); }
    return r < 0 ? -1 : appToken;
}

int64_t Engine::affiliate(int accountId, const std::string& groupId, bool on, McService service) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    if (service == McService::McVideo) {
        return impl_->ctl.runSync([=]() -> int64_t {
            Impl* o = impl_.get();
            if (!o->accountCfgs.count(accountId)) return -1;
            std::set<std::string>& groups = o->mcvideoAffiliations[accountId];
            const std::string gid = mcptt::bareId(groupId);
            if (on) groups.insert(gid); else groups.erase(gid);
            return o->sendMcVideoAffiliation(accountId, token, token, true);
        });
    }
    return impl_->ctl.runSync([=]() -> int64_t { return impl_->sendAffiliation(accountId, groupId, on, token, token, true); });
}

Result Engine::subscribeConference(int accountId, const std::string& groupId, bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        std::map<std::string, std::string> h{{"Event", "conference"}, {"Expires", on ? "3600" : "0"}};
        int64_t r = o->doSendRequest(accountId, "SUBSCRIBE", "sip:" + groupId + "@" + ic->second.domain, "", "", h, token);
        return r < 0 ? Result::fail(-3, "subscribe failed") : Result::success();
    });
}

Result Engine::subscribeXcapDiff(int accountId, const std::string& psiUri, bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        std::map<std::string, std::string> h{{"Event", "xcap-diff"}, {"Expires", on ? "3600" : "0"}};
        int64_t r = impl_->doSendRequest(accountId, "SUBSCRIBE", psiUri, "", "", h, token);
        return r < 0 ? Result::fail(-3, "subscribe failed") : Result::success();
    });
}

// ── 관제 ──

Result Engine::dialogWatch(int accountId, const std::string& targetAor, bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        std::map<std::string, std::string> h{{"Event", "dialog"}, {"Expires", on ? "3600" : "0"}};
        int64_t r = o->doSendRequest(accountId, "SUBSCRIBE", detail::normalizeTarget(targetAor, ic->second.domain), "", "", h, token);
        return r < 0 ? Result::fail(-3, "subscribe failed") : Result::success();
    });
}

int Engine::join(int accountId, const std::string& targetUri, const DialogInfo& dlg) {
    if (!impl_->running || dlg.callId.empty()) return -1;
    return impl_->ctl.runSync([this, accountId, targetUri, dlg]() -> int {
        Impl* o = impl_.get();
        auto it = o->accounts.find(accountId);
        if (it == o->accounts.end()) return -1;
        const std::string dst = detail::normalizeTarget(targetUri, o->accountCfgs[accountId].domain);
        auto call = std::make_unique<PjCall>(o, *it->second, accountId);
        call->recvOnly = true;
        try {
            pj::CallOpParam prm(true);
            prm.opt.audioCount = 1;
            prm.opt.videoCount = 0;
            pj::SipHeader hj; hj.hName = "Join"; hj.hValue = dlg.joinHeader();
            pj::SipHeader hs; hs.hName = "Supported"; hs.hValue = "join";
            prm.txOption.headers.push_back(hj);
            prm.txOption.headers.push_back(hs);
            call->makeCall(dst, prm);
        } catch (pj::Error& e) {
            o->log(1, std::string("join ") + dst + ": " + e.info(false));
            return -1;
        }
        const int id = call->getId();
        call->sealCallId(id);
        o->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
            c.remoteUri = dst; c.listenOnly = true; c.joinedDialog = dlg.callId;
        });
        o->calls[id] = std::move(call);
        o->log(3, "join " + dst + " (Join: " + dlg.joinHeader() + ") → call " + std::to_string(id));
        return id;
    });
}

int Engine::pickup(int accountId, const std::string& featureCode, const std::string& number) {
    if (featureCode.empty()) return -1;
    return dial(accountId, featureCode + number);
}

Result Engine::transfer(int callId, const std::string& target) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, target](PjCall& c) {
        int acc = o->snapshotCall(callId).accountId;
        std::string dst = detail::normalizeTarget(target, o->accountCfgs[acc].domain);
        pj::CallOpParam prm;
        c.xfer(dst, prm);
    });
}

Result Engine::transferAttended(int callId, int consultCallId) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, consultCallId](PjCall& c) {
        PjCall* d = o->findCall(consultCallId);
        if (!d) throw pj::Error(PJ_ENOTFOUND, "transferAttended", "no consult call", __FILE__, __LINE__);
        pj::CallOpParam prm;
        c.xferReplaces(*d, prm);
    });
}

// ── MCData media plane SDS (MSRP, TS 24.282 §9.2.3 — mcdata/msrp.h) ──

void Engine::Impl::runMsrpThread(std::shared_ptr<std::atomic<bool>> cancel, std::function<void()> body) {
    {
        std::lock_guard<std::mutex> lk(msrpM);
        msrpActive++;
        msrpCancels.erase(std::remove_if(msrpCancels.begin(), msrpCancels.end(),
                                         [](const std::weak_ptr<std::atomic<bool>>& w) { return w.expired(); }), msrpCancels.end());
        msrpCancels.push_back(cancel);
    }
    std::thread([this, body] {
        try { body(); } catch (...) {}
        std::lock_guard<std::mutex> lk(msrpM);
        msrpActive--;
        msrpCv.notify_all();
    }).detach();
}

/** 입출력이 끝난 MSRP 호 — 서버가 저장·전달 뒤 BYE 한다(mcdata_messaging.md §4.7). 5 s 안에 끊기지 않으면 우리가 끊는다. */
static void finishMsrpCall(Engine::Impl* o, int callId, const std::atomic<bool>& cancel) {
    for (int i = 0; i < 25 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (cancel) return;
    o->ctl.post([o, callId] {
        PjCall* c = o->findCall(callId);
        if (!c) return;
        try { pj::CallOpParam p; c->hangup(p); } catch (pj::Error&) {}
    });
}

void Engine::Impl::startMsrpSend(int callId, int accountId, const MsrpLeg& leg) {
    auto cancel = leg.cancel;
    const std::string sp = leg.serverPath, lp = leg.localPath, sig = leg.signallingTlv, pay = leg.payloadTlv;
    const int64_t token = leg.token;
    runMsrpThread(cancel, [this, callId, accountId, cancel, sp, lp, sig, pay, token] {
        std::string err;
        const int code = sp.empty() ? 488 : msrp::sendSds(sp, lp, sig, pay, 10, *cancel, nullptr, err);
        log(3, "msrp send call " + std::to_string(callId) + " " + sp + " → " + std::to_string(code) + (err.empty() ? "" : " (" + err + ")"));
        RequestResult r;
        r.accountId = accountId; r.token = token; r.method = "MSRP"; r.code = code;
        r.reason = !err.empty() ? err : code == 200 ? "OK" : sp.empty() ? "no a=path in answer" : "";
        emit([this, r] { listener->onRequestResult(r); });
        finishMsrpCall(this, callId, *cancel);
    });
}

void Engine::Impl::startMsrpRecv(int callId, int accountId, const MsrpLeg& leg) {
    auto cancel = leg.cancel;
    const std::string sp = leg.serverPath, lp = leg.localPath, from = leg.fromUri, group = leg.groupUri;
    runMsrpThread(cancel, [this, callId, accountId, cancel, sp, lp, from, group] {
        std::string ct, body, err;
        if (!sp.empty() && msrp::receiveSds(sp, lp, 15, *cancel, ct, body, err)) {
            SdsMessage m;
            if (mcdata::parse(ct, body, m)) {
                m.accountId = accountId;
                if (m.fromUri.empty()) m.fromUri = from;          // 본문에 mcdata-info 가 없다 — 배포 INVITE 의 것
                if (m.groupUri.empty()) m.groupUri = group;
                m.mediaPlane = true;
                log(3, "msrp recv call " + std::to_string(callId) + " msg=" + m.msgId + " bytes=" + std::to_string(m.text.size()));
                emit([this, m] { listener->onSds(m); });
            } else {
                log(2, "msrp recv call " + std::to_string(callId) + ": body is not MCData SDS (" + ct + ")");
            }
        } else {
            log(2, "msrp recv call " + std::to_string(callId) + " " + sp + ": " + (sp.empty() ? "no a=path" : err));
        }
        finishMsrpCall(this, callId, *cancel);
    });
}

bool Engine::Impl::startMsrpInvite(int accountId, const std::string& groupId, int64_t token, const std::string& sigTlv,
                                   const std::string& payTlv) {
    auto it = accounts.find(accountId);
    auto ic = accountCfgs.find(accountId);
    if (it == accounts.end() || ic == accountCfgs.end()) return false;
    const AccountConfig& cfg = ic->second;
    auto call = std::make_unique<PjCall>(this, *it->second, accountId);
    call->msrp.reset(new MsrpLeg);
    call->msrp->outgoing = true;
    call->msrp->token = token;
    call->msrp->signallingTlv = sigTlv;
    call->msrp->payloadTlv = payTlv;
    try {
        pj::CallOpParam prm(true);
        prm.opt.audioCount = 1;                                          // 더미 오디오(서버는 포트 9 inactive 로 답한다)
        prm.opt.videoCount = 0;
        auto hdr = [&](const char* n, const std::string& v) { pj::SipHeader h; h.hName = n; h.hValue = v; prm.txOption.headers.push_back(h); };
        hdr("Accept-Contact", std::string("*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\";require;explicit"));
        hdr("P-Preferred-Service", msrp::kIcsiMcDataSds);
        hdr("P-Preferred-Identity", "<" + cfg.aor() + ">");
        call->makeCall("sip:" + groupId + "@" + cfg.domain, prm);        // offer = pjsua audio + m=message(onCallSdpCreated)
    } catch (pj::Error& e) {
        log(1, "msrp invite " + groupId + ": " + e.info(false));
        return false;
    }
    const int callId = call->getId();
    log(3, "msrp sds " + groupId + " (" + std::to_string(payTlv.size()) + " bytes) → call " + std::to_string(callId));
    calls[callId] = std::move(call);
    return true;
}

/** 호출자가 준 message ID — 비면 새로. hex32(UUID 16 옥텟)가 아니면 빈 문자열(실패). */
static std::string sdsMessageId(const std::string& given) {
    if (given.empty()) return mcdata::newMessageId();
    if (given.size() != 32) return std::string();
    std::string v = given;
    for (auto& ch : v) {
        if (!std::isxdigit((unsigned char)ch)) return std::string();
        ch = (char)std::tolower((unsigned char)ch);
    }
    return v;
}

SdsSend Engine::sendGroupSds(int accountId, const std::string& groupId, const std::string& text, bool requestDelivery,
                             const std::string& givenMsgId) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (text.empty())    { out.code = -2; out.reason = "empty text";  return out; }
    std::string msgId = sdsMessageId(givenMsgId);
    if (msgId.empty())   { out.code = -2; out.reason = "bad message id"; return out; }
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        const int64_t now = (int64_t)std::time(nullptr);
        // 시그널링 평면 상한을 넘으면 media plane(MSRP) — 서버는 초과 MESSAGE 를 403 Warning 203 으로 거절한다(TS 24.282 §9.2.2 8)).
        const int cap = ic->second.maxSdsCplaneBytes;
        if (cap > 0 && (int)text.size() > cap)
            return o->startMsrpInvite(accountId, groupId, token,
                                      mcdata::sdsSignallingTlv(mcdata::conversationIdOf(groupId), msgId, requestDelivery, now),
                                      mcdata::sdsPayloadTlv(text));
        mcdata::Body b = mcdata::buildGroupSds("tel:" + groupId, text, mcdata::conversationIdOf(groupId), msgId, requestDelivery, now);
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + groupId + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendSds(int accountId, const std::string& peer, const std::string& text, bool requestDelivery,
                        const std::string& givenMsgId) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (text.empty())    { out.code = -2; out.reason = "empty text";  return out; }
    std::string to = mcptt::bareId(peer);
    if (to.empty())      { out.code = -2; out.reason = "empty peer";  return out; }
    std::string msgId = sdsMessageId(givenMsgId);
    if (msgId.empty())   { out.code = -2; out.reason = "bad message id"; return out; }
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        // conversation ID 는 **나와 상대의 쌍**으로 짓는다 — 상대가 답장할 때 같은 값이 나와야 한 대화다.
        std::string me = mcptt::bareId(ic->second.effectiveMcpttId());
        mcdata::Body b = mcdata::buildOneToOneSds("tel:" + to, text, mcdata::conversationIdOneToOne(me, to),
                                                  msgId, requestDelivery, (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + to + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendGroupFd(int accountId, const std::string& groupId, const FdFile& file) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (file.url.empty()) { out.code = -2; out.reason = "empty file url"; return out; }
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        // 그룹 SDS 와 같은 대화(conversation ID) — 파일도 그 그룹 스레드에 놓인다.
        mcdata::Body b = mcdata::buildGroupFd("tel:" + groupId, file, mcdata::conversationIdOf(groupId), msgId,
                                              (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + groupId + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendFd(int accountId, const std::string& peer, const FdFile& file) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (file.url.empty()) { out.code = -2; out.reason = "empty file url"; return out; }
    std::string to = mcptt::bareId(peer);
    if (to.empty())       { out.code = -2; out.reason = "empty peer";     return out; }
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        std::string me = mcptt::bareId(ic->second.effectiveMcpttId());
        mcdata::Body b = mcdata::buildOneToOneFd("tel:" + to, file, mcdata::conversationIdOneToOne(me, to), msgId,
                                                 (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + to + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendSdsNotification(int accountId, const std::string& peer, const std::string& convId,
                                    const std::string& msgId, int notifType, const std::string& groupId) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (mcptt::bareId(peer).empty()) { out.code = -2; out.reason = "empty peer"; return out; }
    int64_t token = impl_->nextToken++;
    out.token = token;
    Result r = impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        const AccountConfig& cfg = ic->second;
        const std::string to = mcptt::bareId(peer);
        const int64_t now = (int64_t)std::time(nullptr);
        int64_t rc;
        if (!cfg.mcdataServerUri.empty()) {
            // TS 24.282 §12.2.1.1 — Request-URI = 참여 MCData 기능 PSI(§6.2.4.1 4)), 대상 = resource-lists, 그룹 통지면
            //   <mcdata-calling-group-id>. ICSI mcdata.sds(§6.2.4.1 1)a)~c)).
            auto tel = [](const std::string& id) { return id.find(':') == std::string::npos ? "tel:" + id : id; };
            const std::string gid = mcptt::bareId(groupId);
            mcdata::Body b = mcdata::buildNotification(convId, msgId, notifType, now, tel(to), gid.empty() ? std::string() : tel(gid));
            std::map<std::string, std::string> h;
            h["Accept-Contact"] = "*;+g.3gpp.mcdata.sds;require;explicit, "
                                  "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\";require;explicit";
            h["P-Preferred-Service"] = msrp::kIcsiMcDataSds;
            rc = o->doSendRequest(accountId, "MESSAGE", cfg.mcdataServerUri, b.contentType, b.body, h, token);
        } else {
            mcdata::Body b = mcdata::buildNotification(convId, msgId, notifType, now);
            rc = o->doSendRequest(accountId, "MESSAGE", "sip:" + to + "@" + cfg.domain, b.contentType, b.body, {}, token);
        }
        return rc < 0 ? Result::fail(-3, "send failed") : Result::success();
    });
    out.ok = r.ok; out.code = r.code; out.reason = r.reason;
    return out;
}

std::vector<AudioDeviceInfo> Engine::audioDevices() const {
    std::vector<AudioDeviceInfo> v;
    if (!impl_->running) return v;
    return impl_->ctl.runSync([this, v]() mutable {
        try {
            for (auto& d : impl_->ep->audDevManager().enumDev2()) {
                AudioDeviceInfo i; i.id = d.id; i.name = d.name; i.driver = d.driver;
                i.inputCount = d.inputCount; i.outputCount = d.outputCount;
                v.push_back(i);
            }
        } catch (...) {}
        return v;
    });
}

Result Engine::refreshAudioDevices() {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this]() -> Result {
        try { impl_->ep->audDevManager().refreshDevs(); return Result::success(); }
        catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::setAudioDevices(int captureDev, int playbackDev) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, captureDev, playbackDev]() -> Result {
        try {
            impl_->ep->audDevManager().setCaptureDev(captureDev);
            impl_->ep->audDevManager().setPlaybackDev(playbackDev);
            return Result::success();
        } catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::setCaptureEnabled(bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, on]() -> Result {
        Impl* o = impl_.get();
        if (o->captureOn == on) return Result::success();
        // null 장치는 모드를 받지 않고 다시 만들어질 뿐이다(pjsua_set_snd_dev2) — 상태만 둔다.
        if (!o->cfg.nullAudioDevice) {
            // 모드는 장치 선택을 넘어 유지된다(setCaptureDev·setPlaybackDev 가 pjsua_get_snd_dev2 로 현재 모드를 이어받는다).
            // NO_IMMEDIATE_OPEN — 장치가 닫혀 있으면 열지 않고 모드만, 열려 있으면 곧바로 다시 연다(브리지 결선은 유지).
            const unsigned mode = (on ? 0u : (unsigned)PJSUA_SND_DEV_SPEAKER_ONLY) | (unsigned)PJSUA_SND_DEV_NO_IMMEDIATE_OPEN;
            try { o->ep->audDevManager().setSndDevMode(mode); }
            catch (pj::Error& e) { return fromError(e); }
        }
        o->captureOn = on;
        o->applyDeviceLevels();                              // 재오픈은 slot 0 레벨을 초기화한다
        o->log(4, std::string("capture ") + (on ? "enabled (full duplex)" : "disabled (speaker only)"));
        return Result::success();
    });
}

Result Engine::setDeviceAudioLevels(float speaker, double micTargetDbov) {
    if (!impl_->running) return Result::fail(-1, "not running");
    if (!(speaker >= 0.f)) return Result::fail(-1, "speaker level");
    return impl_->ctl.runSync([this, speaker, micTargetDbov]() -> Result {
        Impl* o = impl_.get();
        o->spkLevel = speaker;
        o->micTarget = std::min(-10.0, std::max(-40.0, micTargetDbov));   // 엔진 목표 범위(§4)
        o->levelsSet = true;
        o->applyDeviceLevels();
        o->log(4, "device levels speaker=" + std::to_string(speaker) + " mic_target=" + std::to_string(o->micTarget) + " dBov");
        return Result::success();
    });
}

static pjmedia_aud_dev_route pjRoute(AudioRoute r) {
    switch (r) {
        case AudioRoute::Earpiece: return PJMEDIA_AUD_DEV_ROUTE_EARPIECE;
        case AudioRoute::Loudspeaker: return PJMEDIA_AUD_DEV_ROUTE_LOUDSPEAKER;
        default: return PJMEDIA_AUD_DEV_ROUTE_DEFAULT;
    }
}

Result Engine::setAudioRoute(AudioRoute output, AudioRoute input) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, output, input]() -> Result {
        Impl* o = impl_.get();
        if (o->cfg.nullAudioDevice) return Result::success();   // null 장치는 라우트 능력이 없다
        pj::AudDevManager& adm = o->ep->audDevManager();
        try { adm.setOutputRoute(pjRoute(output), true); }
        catch (pj::Error& e) { return fromError(e); }
        // 입력 라우트를 모르는 백엔드도 있다 — 출력은 이미 걸렸으므로 실패는 기록만(keep = 발언마다 장치가 다시 열려도 유지)
        try { adm.setInputRoute(pjRoute(input), true); }
        catch (pj::Error& e) { o->log(3, std::string("input route: ") + e.info(false)); }
        return Result::success();
    });
}

Result Engine::reopenAudioDevice() {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        if (o->cfg.nullAudioDevice) return Result::success();
        pj::AudDevManager& adm = o->ep->audDevManager();
        if (!adm.sndIsActive()) return Result::success();       // 닫혀 있다 — 다음 개방이 새 트랙이다
        // 같은 장치·같은 모드면 pjsua_set_snd_dev2 는 "No changes" 로 돌아간다(열려 있거나 NO_IMMEDIATE_OPEN 이면). 모드에서
        // NO_IMMEDIATE_OPEN 만 빼 값이 달라지게 하면 장치를 닫고 곧바로 다시 연다 — 게이트(SPEAKER_ONLY)는 그대로 둔다.
        const unsigned mode = o->captureOn ? 0u : (unsigned)PJSUA_SND_DEV_SPEAKER_ONLY;
        try { adm.setSndDevMode(mode); }
        catch (pj::Error& e) { return fromError(e); }
        o->applyDeviceLevels();
        o->log(3, "sound device reopened");
        return Result::success();
    });
}

bool Engine::captureEnabled() const { return impl_->captureOn; }

Result Engine::setVideoWindow(void* nativeWindow) {
#if CIMSUE_FRAME_SINK
    (void)nativeWindow;
    return Result::fail(-3, "frame sink build — frames come through onVideoFrame");
#elif PJSUA_HAS_VIDEO
    if (!impl_->running) { windowRelease(nativeWindow); return Result::fail(-1, "not running"); }
    return impl_->ctl.runSync([this, nativeWindow]() -> Result {
        Impl* o = impl_.get();
        void* old = nullptr;
        {
            std::lock_guard<std::mutex> lk(o->videoM);
            old = o->videoWindow;
            o->videoWindow = nativeWindow;
        }
        o->log(4, std::string("video window ") + (nativeWindow ? "set" : "cleared") + " (" + std::to_string(o->calls.size()) + " calls)");
        // 활성 영상 호에 곧바로 — 해제(nullptr)면 렌더러에서 창을 뗀다(렌더러가 자기 참조를 푼다).
        for (auto& kv : o->calls) {
            auto* call = static_cast<PjCall*>(kv.second.get());
            if (nativeWindow) { o->attachVideo(call, -1); continue; }
            try {
                for (auto& m : call->getInfo().media) {
                    if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
                    if (m.videoIncomingWindowId == PJSUA_INVALID_ID) continue;   // 렌더러 없음 — 뗄 창도 없다(attachVideo 와 같은 이유)
                    pj::VideoWindow vw = m.videoWindow;
                    pj::VideoWindowHandle h;
                    h.handle.window = nullptr;
                    vw.setWindow(h);
                }
            } catch (...) {}
        }
        if (old && old != nativeWindow) windowRelease(old);
        return Result::success();
    });
#else
    windowRelease(nativeWindow);                         // 넘겨받은 참조 — 쓰지 않으니 바로 돌려준다
    return Result::fail(-3, "video not built");
#endif
}

Result Engine::setVideoPreview(bool on) {
#if CIMSUE_FRAME_SINK
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, on]() -> Result {
        Impl* o = impl_.get();
        o->previewWanted = on;
        if (on && o->camDev < 0) o->camDev = o->frontCamera();
        o->syncPreview();
        return Result::success();
    });
#else
    (void)on;
    return Result::fail(-3, "no frame sink");
#endif
}

Result Engine::setVideoCaptureDevice(int deviceId) {
#if PJSUA_HAS_VIDEO
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, deviceId]() -> Result {
        Impl* o = impl_.get();
        int dev = deviceId;
        if (dev < 0) dev = o->frontCamera();
        else {
            try {
                pj::VideoDevInfo info = o->ep->vidDevManager().getDevInfo(dev);
                if (!(info.dir & PJMEDIA_DIR_CAPTURE)) return Result::fail(-2, "not a capture device");
            } catch (pj::Error& e) { return fromError(e); }
        }
        if (dev == o->camDev) return Result::success();
        o->camDev = dev;
        o->log(3, "camera -> " + std::to_string(dev));
        // 지금 보내는 호는 곧바로 바꾼다(pjsua 가 캡처 포트를 새 장치로 갈아 끼운다)
        for (auto& kv : o->calls) {
            auto* call = static_cast<PjCall*>(kv.second.get());
            try {
                for (auto& m : call->getInfo().media) {
                    if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
                    if (!call->vidStreamIsRunning((int)m.index, PJMEDIA_DIR_ENCODING)) continue;
                    pj::CallVidSetStreamParam p;
                    p.medIdx = (int)m.index;
                    p.capDev = (pjmedia_vid_dev_index)dev;
                    call->vidSetStream(PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV, p);
                }
            } catch (pj::Error& e) { o->log(2, std::string("camera switch: ") + e.info(false)); }
        }
        o->syncPreview();
        return Result::success();
    });
#else
    (void)deviceId;
    return Result::fail(-3, "video not built");
#endif
}

Result Engine::switchCamera(int callId) {
#if PJSUA_HAS_VIDEO
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, callId]() -> Result {
        Impl* o = impl_.get();
        PjCall* call = o->findCall(callId);
        if (!call) return Result::fail(-2, "no such call");
        std::vector<int> cams = o->cameras();
        if (cams.size() < 2) return Result::fail(-3, "single camera");
        if (o->camDev < 0) o->camDev = o->frontCamera();
        int next = cams[0];
        for (size_t i = 0; i < cams.size(); ++i)
            if (cams[i] == o->camDev) { next = cams[(i + 1) % cams.size()]; break; }
        bool done = false;
        try {
            for (auto& m : call->getInfo().media) {
                if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
                pj::CallVidSetStreamParam p;
                p.medIdx = (int)m.index;
                p.capDev = (pjmedia_vid_dev_index)next;
                call->vidSetStream(PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV, p);
                done = true;
            }
        } catch (pj::Error& e) { return fromError(e); }
        if (!done) return Result::fail(-4, "no active video");
        o->camDev = next;
        o->log(3, "camera -> " + std::to_string(next));
        o->syncPreview();
        return Result::success();
    });
#else
    (void)callId;
    return Result::fail(-3, "video not built");
#endif
}

Result Engine::setVideoSend(int callId, bool on) {
#if PJSUA_HAS_VIDEO
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, callId, on]() -> Result {
        Impl* o = impl_.get();
        PjCall* call = o->findCall(callId);
        if (!call) return Result::fail(-2, "no such call");
        call->videoSend = on;
        CallInfo snap;
        o->updateCall(callId, [&](CallInfo& c) { c.videoSend = on; }, &snap);
        o->applyVideoTx(call);
        o->emit([o, snap] { o->listener->onCallMedia(snap); });
        return Result::success();
    });
#else
    (void)callId; (void)on;
    return Result::fail(-3, "video not built");
#endif
}

std::vector<VideoDeviceInfo> Engine::videoDevices() const {
    std::vector<VideoDeviceInfo> v;
#if PJSUA_HAS_VIDEO
    if (!impl_->running) return v;
    return impl_->ctl.runSync([this]() {
        std::vector<VideoDeviceInfo> out;
        try {
            for (auto& d : impl_->ep->vidDevManager().enumDev2()) {
                VideoDeviceInfo i;
                i.id = d.id; i.name = d.name; i.driver = d.driver;
                i.capture = (d.dir & PJMEDIA_DIR_CAPTURE) != 0;
                i.render = (d.dir & PJMEDIA_DIR_RENDER) != 0;
                out.push_back(i);
            }
        } catch (...) {}
        return out;
    });
#else
    return v;
#endif
}

int Engine::addPlaybackRoute(int playbackDev) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, playbackDev]() -> int {
        Impl* o = impl_.get();
        try {
            // recDev = PJMEDIA_AUD_INVALID_DEV → 재생 전용(엔진 패치, ExtraAudioDevice::open). 브리지 포맷(16k mono) 으로 연다.
            std::unique_ptr<pj::ExtraAudioDevice> dev(new pj::ExtraAudioDevice(playbackDev, PJMEDIA_AUD_INVALID_DEV));
            dev->open();
            int id = o->nextRouteId++;
            o->routes[id] = std::move(dev);
            o->log(3, "playback route " + std::to_string(id) + " ← dev " + std::to_string(playbackDev));
            return id;
        } catch (pj::Error& e) {
            o->log(1, "addPlaybackRoute dev " + std::to_string(playbackDev) + ": " + e.info(false));
            return -1;
        }
    });
}

Result Engine::removePlaybackRoute(int routeId) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, routeId]() -> Result {
        Impl* o = impl_.get();
        auto it = o->routes.find(routeId);
        if (it == o->routes.end()) return Result::fail(-1, "no such route");
        // 이 라우트에 붙은 호는 기본 장치로 되돌리고 재결선 — 결선을 먼저 끊은 뒤 장치를 닫는다
        for (auto& kv : o->calls) {
            int callId = kv.first;
            if (o->snapshotCall(callId).playbackRoute != routeId) continue;
            o->updateCall(callId, [](CallInfo& c) { c.playbackRoute = 0; });
            try { o->wireMedia(static_cast<PjCall*>(kv.second.get()), callId); } catch (pj::Error&) {}
        }
        o->routes.erase(it);                     // ~ExtraAudioDevice → close
        return Result::success();
    });
}

Result Engine::setCallRoute(int callId, int routeId) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, callId, routeId]() -> Result {
        Impl* o = impl_.get();
        if (routeId != 0 && o->routes.find(routeId) == o->routes.end()) return Result::fail(-1, "no such route");
        PjCall* c = o->findCall(callId);
        if (!c) return Result::fail(-1, "no such call");
        CallInfo snap;
        o->updateCall(callId, [routeId](CallInfo& ci) { ci.playbackRoute = routeId; }, &snap);
        if (snap.mediaActive) {
            try { o->wireMedia(c, callId); } catch (pj::Error& e) { return fromError(e); }
        }
        emitMediaSnapshot(o, callId);
        return Result::success();
    });
}

}  // namespace cimsue
