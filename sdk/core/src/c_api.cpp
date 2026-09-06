// libcimsue — C API 구현 (cimsue_c.h · ue_sdk.md §6.4)
//
// 공개 C++ 표면의 얇은 평탄화 층이다. 여기에는 프로토콜 로직이 없다 — 타입 변환과 수명 규약만 둔다.
//   - 입력 POD → C++ 객체: NULL 문자열 필드는 C++ 기본값을 그대로 둔다(설정 구조체의 default 규약).
//   - C++ 객체 → 산출 POD: 문자열은 소유자 객체를 가리킨다. 소유자는 콜백 인자(스택) 또는
//     스레드별 스냅샷(getter)·핸들 스냅샷(CSC) 이며, 그 수명이 곧 헤더가 약속한 유효 구간이다.
#include "cimsue/cimsue_c.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "cimsue/csc.h"
#include "cimsue/engine.h"
#include "cimsue/listener.h"

using namespace cimsue;

namespace {

// ── 공통 변환 ──

std::string S(const char* p) { return p ? std::string(p) : std::string(); }
/** NULL 이 아닐 때만 덮어쓴다 — NULL 은 "C++ 기본값 유지". */
void assignIf(std::string& dst, const char* p) { if (p) dst = p; }
const char* C(const std::string& s) { return s.c_str(); }
int32_t B(bool b) { return b ? 1 : 0; }

std::vector<std::string> strList(const char* const* v, int32_t n) {
    std::vector<std::string> out;
    for (int32_t i = 0; v && i < n; ++i) out.push_back(S(v[i]));
    return out;
}

/** 문자열 산출 헬퍼 공통 규약 — NUL 종료로 최대 cap 바이트 기록, NUL 제외 실제 길이 반환. */
int32_t copyOut(const std::string& s, char* out, int32_t cap) {
    if (out && cap > 0) {
        size_t n = std::min(s.size(), (size_t)(cap - 1));
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return (int32_t)s.size();
}

thread_local std::string g_lastError;

cimsue_status_t ret(const Result& r) {
    if (r.ok) return CIMSUE_OK;
    g_lastError = r.reason;
    return r.code != 0 ? (cimsue_status_t)r.code : -1;
}
/** id 를 돌려주는 명령의 실패(-1) 사유 기록. */
int32_t retId(int id, const char* what) {
    if (id < 0) g_lastError = what;
    return (int32_t)id;
}

// ── 입력 POD → C++ ──

EngineConfig toCxx(const cimsue_engine_config_t* c) {
    EngineConfig e;
    if (!c) return e;
    assignIf(e.userAgent, c->user_agent);
    e.logLevel = c->log_level;
    assignIf(e.tlsCaPem, c->tls_ca_pem);
    e.tlsVerifyServer = c->tls_verify_server != 0;
    e.nullAudioDevice = c->null_audio_device != 0;
    e.noVad = c->no_vad != 0;
    e.udpPort = c->udp_port; e.tcpPort = c->tcp_port; e.tlsPort = c->tls_port;
    e.clockRate = c->clock_rate;
    return e;
}

AccountConfig toCxx(const cimsue_account_config_t* c) {
    AccountConfig a;
    if (!c) return a;
    assignIf(a.serverHost, c->server_host);
    a.serverPort = c->server_port;
    a.transport = (Transport)c->transport;
    assignIf(a.domain, c->domain);
    assignIf(a.msisdn, c->msisdn);
    assignIf(a.imsi, c->imsi);
    assignIf(a.authId, c->auth_id);
    assignIf(a.displayName, c->display_name);
    assignIf(a.ha1, c->ha1);
    assignIf(a.password, c->password);
    a.authScheme = (AuthScheme)c->auth_scheme;
    assignIf(a.akaK, c->aka_k); assignIf(a.akaOpc, c->aka_opc); assignIf(a.akaAmf, c->aka_amf);
    if (c->sec_mechanisms) a.secMechanisms = strList(c->sec_mechanisms, c->sec_mechanism_count);
    a.mediaSecurity = (MediaSecurity)c->media_security;
    a.expiresSec = c->expires_sec;
    assignIf(a.contactParams, c->contact_params);
    a.videoAutoTransmit = c->video_auto_transmit != 0;
    assignIf(a.mcpttId, c->mcptt_id);
    a.autoAnswerMcptt = c->auto_answer_mcptt != 0;
    return a;
}

CallOptions toCxx(const cimsue_call_options_t* c) {
    CallOptions o;
    if (!c) return o;
    o.video = c->video != 0;
    o.emergency = c->emergency != 0;
    return o;
}

GroupCallOptions toCxx(const cimsue_group_call_options_t* c) {
    GroupCallOptions o;
    if (!c) return o;
    o.emergency = c->emergency != 0;
    o.imminentPeril = c->imminent_peril != 0;
    o.listenOnly = c->listen_only != 0;
    o.fullDuplex = c->full_duplex != 0;
    o.members = strList(c->members, c->member_count);
    return o;
}

DialogInfo toCxx(const cimsue_dialog_info_t* c) {
    DialogInfo d;
    if (!c) return d;
    d.accountId = c->account_id;
    d.watched = S(c->watched); d.id = S(c->id); d.callId = S(c->call_id);
    d.localTag = S(c->local_tag); d.remoteTag = S(c->remote_tag);
    d.direction = S(c->direction); d.state = S(c->state); d.remoteIdentity = S(c->remote_identity);
    d.full = c->full != 0;
    return d;
}

ServiceProfile toCxx(const cimsue_service_profile_t* c) {
    ServiceProfile s;
    if (!c) return s;
    s.kind = S(c->kind);
    s.sipHost = S(c->sip_host); s.sipPort = c->sip_port; s.transport = (Transport)c->transport;
    for (int32_t i = 0; c->transports && i < c->transport_count; ++i)
        s.transports.push_back({(Transport)c->transports[i].transport, c->transports[i].port});
    s.enforced = c->enforced != 0;
    s.mediaSecurity = (MediaSecurity)c->media_security;
    s.domain = S(c->domain); s.msisdn = S(c->msisdn); s.imsi = S(c->imsi);
    s.authId = S(c->auth_id); s.sipHa1 = S(c->sip_ha1); s.mcpttId = S(c->mcptt_id);
    s.authScheme = (AuthScheme)c->auth_scheme;
    assignIf(s.akaK, c->aka_k); assignIf(s.akaOpc, c->aka_opc); assignIf(s.akaAmf, c->aka_amf);
    s.secMechanisms = strList(c->sec_mechanisms, c->sec_mechanism_count);
    s.maxPayloadSdsCplaneBytes = c->max_payload_sds_cplane_bytes;
    return s;
}

// ── C++ → 산출 POD (문자열은 인자 객체를 가리킨다 — 소유자 수명이 곧 유효 구간) ──

void fill(cimsue_reg_info_t& o, const RegInfo& r) {
    o.account_id = r.accountId;
    o.state = (cimsue_reg_state_t)r.state;
    o.code = r.code;
    o.reason = C(r.reason);
    o.expires_sec = r.expiresSec;
}

void fill(cimsue_mcptt_info_t& o, const McpttInfo& m) {
    o.present = B(m.present);
    o.session_type = C(m.sessionType);
    o.request_uri = C(m.requestUri);
    o.calling_user_id = C(m.callingUserId);
    o.calling_group_id = C(m.callingGroupId);
    o.emergency = B(m.emergency);
    o.imminent_peril = B(m.imminentPeril);
    o.private_call = B(m.privateCall);
    o.no_floor_ctrl = B(m.noFloorCtrl);
}

/** sources 배열은 호출자가 준 벡터에 담는다(그 벡터가 소유자). */
void fill(cimsue_call_info_t& o, const CallInfo& c, std::vector<cimsue_media_source_t>& srcBuf) {
    o.call_id = c.callId; o.account_id = c.accountId;
    o.dir = (cimsue_call_dir_t)c.dir;
    o.state = (cimsue_call_state_t)c.state;
    o.remote_uri = C(c.remoteUri);
    o.called_party = C(c.calledParty);
    o.video = B(c.video); o.media_active = B(c.mediaActive); o.muted = B(c.muted); o.listen = B(c.listen);
    o.playback_route = c.playbackRoute;
    o.last_code = c.lastCode;
    o.last_reason = C(c.lastReason);
    srcBuf.clear();
    for (const auto& s : c.sources) srcBuf.push_back({s.ssrc, C(s.label), B(s.active), s.level});
    o.sources = srcBuf.empty() ? nullptr : srcBuf.data();
    o.source_count = (int32_t)srcBuf.size();
    o.is_mcptt = B(c.isMcptt);
    o.group_id = C(c.groupId);
    fill(o.mcptt, c.mcptt);
    o.half_duplex = B(c.halfDuplex);
    o.listen_only = B(c.listenOnly);
    o.joined_dialog = C(c.joinedDialog);
}

void fillTalkers(std::vector<cimsue_talker_t>& buf, const std::vector<Talker>& t) {
    buf.clear();
    for (const auto& x : t) buf.push_back({C(x.id), x.ssrc, B(x.self)});
}

void fill(cimsue_floor_event_t& o, const FloorEvent& e, std::vector<cimsue_talker_t>& buf) {
    o.kind = (cimsue_floor_kind_t)e.kind;
    o.call_id = e.callId;
    o.state = (cimsue_floor_state_t)e.state;
    o.duration_sec = e.durationSec;
    o.cause = e.cause;
    o.cause_text = C(e.causeText);
    o.indicator = e.indicator;
    o.permission = e.permission;
    o.queue_position = e.queuePosition;
    o.me_speaking = B(e.meSpeaking);
    fillTalkers(buf, e.talkers);
    o.talkers = buf.empty() ? nullptr : buf.data();
    o.talker_count = (int32_t)buf.size();
    o.raw_type = e.rawType;
}

void fill(cimsue_floor_info_t& o, const FloorInfo& f, std::vector<cimsue_talker_t>& buf) {
    o.state = (cimsue_floor_state_t)f.state;
    fillTalkers(buf, f.talkers);
    o.talkers = buf.empty() ? nullptr : buf.data();
    o.talker_count = (int32_t)buf.size();
    o.can_request = B(f.canRequest);
    o.indicator = f.indicator;
    o.queue_position = f.queuePosition;
    o.local_port = f.localPort;
    o.remote_ip = C(f.remoteIp);
    o.remote_port = f.remotePort;
    o.granted_count = f.grantedCount; o.taken_count = f.takenCount; o.deny_count = f.denyCount;
}

void fill(cimsue_request_result_t& o, const RequestResult& r) {
    o.account_id = r.accountId;
    o.token = r.token;
    o.method = C(r.method);
    o.code = r.code;
    o.reason = C(r.reason);
    o.etag = C(r.etag);
}

void fill(cimsue_dialog_info_t& o, const DialogInfo& d) {
    o.account_id = d.accountId;
    o.watched = C(d.watched);
    o.id = C(d.id); o.call_id = C(d.callId);
    o.local_tag = C(d.localTag); o.remote_tag = C(d.remoteTag);
    o.direction = C(d.direction); o.state = C(d.state);
    o.remote_identity = C(d.remoteIdentity);
    o.full = B(d.full);
}

void fill(cimsue_sds_message_t& o, const SdsMessage& m) {
    o.account_id = m.accountId;
    o.from_uri = C(m.fromUri);
    o.group_uri = C(m.groupUri);
    o.conv_id = C(m.convId); o.msg_id = C(m.msgId);
    o.time_sec = m.timeSec;
    o.disposition_req = m.dispositionReq;
    o.text = C(m.text);
    o.notification = B(m.notification);
    o.notif_type = m.notifType;
    o.fd = B(m.fd);
    o.file_url = C(m.fileUrl); o.file_name = C(m.fileName); o.file_type = C(m.fileType);
    o.file_size = m.fileSize;
}

void fill(cimsue_stream_stats_t& o, const StreamStats& s) {
    o.rx_packets = s.rxPackets; o.rx_bytes = s.rxBytes; o.rx_loss = s.rxLoss; o.rx_discard = s.rxDiscard;
    o.tx_packets = s.txPackets; o.tx_bytes = s.txBytes;
    o.valid = B(s.valid);
}

/** AccountConfig 산출(to_account) — sec_mechanisms 포인터 배열은 함께 넘긴 버퍼가 소유한다. */
void fill(cimsue_account_config_t& o, const AccountConfig& a, std::vector<const char*>& secBuf) {
    o.server_host = C(a.serverHost);
    o.server_port = a.serverPort;
    o.transport = (cimsue_transport_t)a.transport;
    o.domain = C(a.domain); o.msisdn = C(a.msisdn); o.imsi = C(a.imsi); o.auth_id = C(a.authId);
    o.display_name = C(a.displayName);
    o.ha1 = C(a.ha1); o.password = C(a.password);
    o.auth_scheme = (cimsue_auth_scheme_t)a.authScheme;
    o.aka_k = C(a.akaK); o.aka_opc = C(a.akaOpc); o.aka_amf = C(a.akaAmf);
    secBuf.clear();
    for (const auto& s : a.secMechanisms) secBuf.push_back(C(s));
    o.sec_mechanisms = secBuf.empty() ? nullptr : secBuf.data();
    o.sec_mechanism_count = (int32_t)secBuf.size();
    o.media_security = (cimsue_media_security_t)a.mediaSecurity;
    o.expires_sec = a.expiresSec;
    o.contact_params = C(a.contactParams);
    o.video_auto_transmit = B(a.videoAutoTransmit);
    o.mcptt_id = C(a.mcpttId);
    o.auto_answer_mcptt = B(a.autoAnswerMcptt);
}

/** Profile 한 벌의 소유자 — C++ 객체와 그것을 가리키는 POD 배열을 함께 들고 있는다. */
/** GroupDoc 의 C 스냅샷 — 핸들(getter 산출)과 스레드 스크래치(parse) 양쪽이 쓴다. */
struct GroupDocHolder {
    GroupDoc cxx;
    std::vector<cimsue_group_member_t> mem;
    cimsue_group_doc_t out{};

    void build() {
        mem.clear();
        for (const auto& m : cxx.members) mem.push_back({C(m.uri), C(m.name), C(m.role), m.priority});
        out = cimsue_group_doc_t{};
        out.uri = C(cxx.uri); out.display_name = C(cxx.displayName); out.etag = C(cxx.etag);
        out.members = mem.empty() ? nullptr : mem.data();
        out.member_count = (int32_t)mem.size();
        out.session_type = C(cxx.sessionType);
        out.video_enabled = B(cxx.videoEnabled); out.encryption = B(cxx.encryption);
        out.emergency_call = B(cxx.emergencyCall); out.emergency_alert = B(cxx.emergencyAlert);
        out.allow_sds = B(cxx.allowSds); out.allow_fd = B(cxx.allowFd); out.require_affiliation = B(cxx.requireAffiliation);
        out.priority = cxx.priority; out.max_participants = cxx.maxParticipants;
        out.org_code = C(cxx.orgCode); out.authorized_user = C(cxx.authorizedUser);
    }
};

GroupDoc toCxx(const cimsue_group_doc_t* d) {
    GroupDoc g;
    if (!d) return g;
    g.uri = S(d->uri); g.displayName = S(d->display_name); g.etag = S(d->etag);
    for (int32_t i = 0; d->members && i < d->member_count; ++i) {
        GroupMember m;
        m.uri = S(d->members[i].uri); m.name = S(d->members[i].display_name);
        if (d->members[i].role && *d->members[i].role) m.role = d->members[i].role;
        m.priority = d->members[i].priority;
        g.members.push_back(m);
    }
    if (d->session_type && *d->session_type) g.sessionType = d->session_type;
    g.videoEnabled = d->video_enabled != 0; g.encryption = d->encryption != 0;
    g.emergencyCall = d->emergency_call != 0; g.emergencyAlert = d->emergency_alert != 0;
    g.allowSds = d->allow_sds != 0; g.allowFd = d->allow_fd != 0; g.requireAffiliation = d->require_affiliation != 0;
    g.priority = d->priority; g.maxParticipants = d->max_participants;
    g.orgCode = S(d->org_code); g.authorizedUser = S(d->authorized_user);
    return g;
}

struct ProfileHolder {
    Profile cxx;
    std::vector<cimsue_service_profile_t>              svc;
    std::vector<std::vector<cimsue_service_endpoint_t>> eps;
    std::vector<std::vector<const char*>>              sec;
    std::vector<cimsue_dispatch_member_t>              members;
    std::vector<cimsue_dispatch_target_t>              targets;
    cimsue_profile_t out{};

    void build() {
        const size_t n = cxx.services.size();
        svc.assign(n, cimsue_service_profile_t{});
        eps.assign(n, {});
        sec.assign(n, {});
        for (size_t i = 0; i < n; ++i) {
            const ServiceProfile& s = cxx.services[i];
            cimsue_service_profile_t& o = svc[i];
            o.kind = C(s.kind);
            o.sip_host = C(s.sipHost); o.sip_port = s.sipPort; o.transport = (cimsue_transport_t)s.transport;
            for (const auto& e : s.transports)
                eps[i].push_back({(cimsue_transport_t)e.transport, e.port});
            o.transports = eps[i].empty() ? nullptr : eps[i].data();
            o.transport_count = (int32_t)eps[i].size();
            o.enforced = B(s.enforced);
            o.media_security = (cimsue_media_security_t)s.mediaSecurity;
            o.domain = C(s.domain); o.msisdn = C(s.msisdn); o.imsi = C(s.imsi);
            o.auth_id = C(s.authId); o.sip_ha1 = C(s.sipHa1); o.mcptt_id = C(s.mcpttId);
            o.auth_scheme = (cimsue_auth_scheme_t)s.authScheme;
            o.aka_k = C(s.akaK); o.aka_opc = C(s.akaOpc); o.aka_amf = C(s.akaAmf);
            for (const auto& m : s.secMechanisms) sec[i].push_back(C(m));
            o.sec_mechanisms = sec[i].empty() ? nullptr : sec[i].data();
            o.sec_mechanism_count = (int32_t)sec[i].size();
            o.max_payload_sds_cplane_bytes = s.maxPayloadSdsCplaneBytes;
        }
        out.display_name = C(cxx.displayName);
        out.login_id = C(cxx.loginId);
        out.country_code = C(cxx.countryCode);
        out.csc_host = C(cxx.cscHost);
        out.csc_port = cxx.cscPort;
        out.services = svc.empty() ? nullptr : svc.data();
        out.service_count = (int32_t)svc.size();
        const DispatchProfile& d = cxx.dispatch;
        out.dispatch.present = B(d.present);
        out.dispatch.group_id = C(d.groupId);
        out.dispatch.group_name = C(d.groupName);
        out.dispatch.pilot_id = C(d.pilotId);
        out.dispatch.monitor_scope = C(d.monitorScope);
        out.dispatch.ptt_listen = C(d.pttListen);
        out.dispatch.listen_visibility = C(d.listenVisibility);
        members.clear(); targets.clear();
        for (const auto& m : d.members) members.push_back({C(m.userId), C(m.name), C(m.volteAor), C(m.pttId), C(m.extension)});
        for (const auto& t : d.pttTargets) targets.push_back({C(t.id), C(t.uri), C(t.name)});
        out.dispatch.members = members.empty() ? nullptr : members.data();
        out.dispatch.member_count = (int32_t)members.size();
        out.dispatch.ptt_targets = targets.empty() ? nullptr : targets.data();
        out.dispatch.ptt_target_count = (int32_t)targets.size();
        out.allow_group_creation = B(cxx.allowGroupCreation);
    }
};

/** getter 산출의 스레드별 스냅샷 — "같은 스레드가 다음 조회를 부를 때까지" 의 실체. */
struct Scratch {
    RegInfo                                 reg;
    cimsue_reg_info_t                       regC{};
    CallInfo                                call;
    cimsue_call_info_t                      callC{};
    std::vector<cimsue_media_source_t>      callSrc;
    FloorInfo                               floor;
    cimsue_floor_info_t                     floorC{};
    std::vector<cimsue_talker_t>            floorTalkers;
    std::vector<int32_t>                    ids;
    std::vector<AudioDeviceInfo>            devs;
    std::vector<cimsue_audio_device_info_t> devsC;
    AccountConfig                           acc;
    std::vector<const char*>                accSec;
    ProfileHolder                           profile;
    GroupDocHolder                          groupDoc;
};
thread_local Scratch g_s;

// ── Listener 어댑터 ──

class CListener : public Listener {
public:
    cimsue_listener_t cb{};

    void onLog(int level, const std::string& msg) override {
        if (cb.on_log) cb.on_log(cb.user, level, C(msg));
    }
    void onRegState(const RegInfo& info) override {
        if (!cb.on_reg_state) return;
        cimsue_reg_info_t o{}; fill(o, info);
        cb.on_reg_state(cb.user, &o);
    }
    void onIncomingCall(const CallInfo& info) override { call(cb.on_incoming_call, info); }
    void onCallState(const CallInfo& info) override { call(cb.on_call_state, info); }
    void onCallMedia(const CallInfo& info) override { call(cb.on_call_media, info); }
    void onFloor(const FloorEvent& ev) override {
        if (!cb.on_floor) return;
        cimsue_floor_event_t o{}; std::vector<cimsue_talker_t> t; fill(o, ev, t);
        cb.on_floor(cb.user, &o);
    }
    void onRoster(int accountId, const std::string& groupId, const std::vector<RosterEntry>& users,
                  bool full) override {
        if (!cb.on_roster) return;
        std::vector<cimsue_roster_entry_t> u;
        for (const auto& e : users) u.push_back({C(e.uri), C(e.status)});
        cb.on_roster(cb.user, accountId, C(groupId), u.empty() ? nullptr : u.data(), (int32_t)u.size(), B(full));
    }
    void onDialogInfo(const DialogInfo& d) override {
        if (!cb.on_dialog_info) return;
        cimsue_dialog_info_t o{}; fill(o, d);
        cb.on_dialog_info(cb.user, &o);
    }
    void onSds(const SdsMessage& msg) override {
        if (!cb.on_sds) return;
        cimsue_sds_message_t o{}; fill(o, msg);
        cb.on_sds(cb.user, &o);
    }
    void onRequestResult(const RequestResult& r) override {
        if (!cb.on_request_result) return;
        cimsue_request_result_t o{}; fill(o, r);
        cb.on_request_result(cb.user, &o);
    }
    void onMessage(int accountId, const std::string& fromUri, const std::string& contentType,
                   const std::string& body) override {
        if (cb.on_message) cb.on_message(cb.user, accountId, C(fromUri), C(contentType), C(body));
    }
    void onEngineStopped() override {
        if (cb.on_engine_stopped) cb.on_engine_stopped(cb.user);
    }

private:
    using CallCb = void(CIMSUE_CALL*)(void*, const cimsue_call_info_t*);
    void call(CallCb fn, const CallInfo& info) {
        if (!fn) return;
        cimsue_call_info_t o{}; std::vector<cimsue_media_source_t> src; fill(o, info, src);
        fn(cb.user, &o);
    }
};

}  // namespace

// ── 핸들 ──

struct cimsue_engine {
    Engine    eng;
    CListener listener;
};

struct cimsue_csc {
    std::unique_ptr<CscClient> cli;
    TokenSet                   token;
    cimsue_token_set_t         tokenC{};
    ProfileHolder              profile;
    std::vector<GroupSummary>  groups;
    std::vector<cimsue_group_summary_t> groupsC;
    XcapDoc                    doc;
    cimsue_xcap_doc_t          docC{};
    GroupDocHolder             group;
};

namespace {

void fillToken(cimsue_csc_t* c) {
    c->tokenC.access_token = C(c->token.accessToken);
    c->tokenC.token_type = C(c->token.tokenType);
    c->tokenC.refresh_token = C(c->token.refreshToken);
    c->tokenC.id_token = C(c->token.idToken);
    c->tokenC.scope = C(c->token.scope);
    c->tokenC.expires_in_sec = c->token.expiresInSec;
}

void fillDoc(cimsue_csc_t* c) {
    c->docC.body = C(c->doc.body);
    c->docC.etag = C(c->doc.etag);
    c->docC.not_modified = B(c->doc.notModified);
}

}  // namespace

extern "C" {

// ── 엔진 ──

cimsue_engine_t* CIMSUE_CALL cimsue_engine_create(void) { return new (std::nothrow) cimsue_engine(); }

void CIMSUE_CALL cimsue_engine_destroy(cimsue_engine_t* e) {
    if (!e) return;
    e->eng.stop();
    delete e;
}

void CIMSUE_CALL cimsue_engine_config_default(cimsue_engine_config_t* cfg) {
    if (!cfg) return;
    const EngineConfig d;
    *cfg = cimsue_engine_config_t{};              // 문자열은 NULL = C++ 기본값 유지
    cfg->log_level = d.logLevel;
    cfg->tls_verify_server = B(d.tlsVerifyServer);
    cfg->null_audio_device = B(d.nullAudioDevice);
    cfg->no_vad = B(d.noVad);
    cfg->udp_port = d.udpPort; cfg->tcp_port = d.tcpPort; cfg->tls_port = d.tlsPort;
    cfg->clock_rate = d.clockRate;
}

cimsue_status_t CIMSUE_CALL cimsue_engine_start(cimsue_engine_t* e, const cimsue_engine_config_t* cfg,
                                                const cimsue_listener_t* listener) {
    if (!e) return -1;
    // 기동 중이면 리스너를 건드리지 않는다 — 이벤트 스레드가 cb 를 읽는 중에 덮어쓰면 경쟁이고, 살아 있는 콜백이 사라진다.
    if (e->eng.running()) { g_lastError = "already running"; return -1; }
    e->listener.cb = listener ? *listener : cimsue_listener_t{};   // start() 중에도 onLog 가 올 수 있어 먼저 채운다
    return ret(e->eng.start(toCxx(cfg), &e->listener));
}

void CIMSUE_CALL cimsue_engine_stop(cimsue_engine_t* e) { if (e) e->eng.stop(); }

int32_t CIMSUE_CALL cimsue_engine_running(const cimsue_engine_t* e) { return e ? B(e->eng.running()) : 0; }

// 계정

void CIMSUE_CALL cimsue_account_config_default(cimsue_account_config_t* cfg) {
    if (!cfg) return;
    const AccountConfig d;
    *cfg = cimsue_account_config_t{};
    cfg->server_port = d.serverPort;
    cfg->transport = (cimsue_transport_t)d.transport;
    cfg->auth_scheme = (cimsue_auth_scheme_t)d.authScheme;
    cfg->media_security = (cimsue_media_security_t)d.mediaSecurity;
    cfg->expires_sec = d.expiresSec;
    cfg->video_auto_transmit = B(d.videoAutoTransmit);
    cfg->auto_answer_mcptt = B(d.autoAnswerMcptt);
}

int32_t CIMSUE_CALL cimsue_engine_add_account(cimsue_engine_t* e, const cimsue_account_config_t* cfg) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.addAccount(toCxx(cfg)), "addAccount failed");
}

cimsue_status_t CIMSUE_CALL cimsue_engine_register_account(cimsue_engine_t* e, int32_t account_id) {
    return e ? ret(e->eng.registerAccount(account_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_unregister_account(cimsue_engine_t* e, int32_t account_id) {
    return e ? ret(e->eng.unregisterAccount(account_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_refresh_registration(cimsue_engine_t* e, int32_t account_id) {
    return e ? ret(e->eng.refreshRegistration(account_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_remove_account(cimsue_engine_t* e, int32_t account_id) {
    return e ? ret(e->eng.removeAccount(account_id)) : -1;
}

void CIMSUE_CALL cimsue_engine_reg_info(const cimsue_engine_t* e, int32_t account_id, cimsue_reg_info_t* out) {
    if (!out) return;
    g_s.reg = e ? e->eng.regInfo(account_id) : RegInfo();
    fill(g_s.regC, g_s.reg);
    *out = g_s.regC;
}

int32_t CIMSUE_CALL cimsue_engine_accounts(const cimsue_engine_t* e, const int32_t** out) {
    g_s.ids.clear();
    if (e) for (int id : e->eng.accounts()) g_s.ids.push_back(id);
    if (out) *out = g_s.ids.empty() ? nullptr : g_s.ids.data();
    return (int32_t)g_s.ids.size();
}

// 호

void CIMSUE_CALL cimsue_call_options_default(cimsue_call_options_t* opts) {
    if (!opts) return;
    const CallOptions d;
    opts->video = B(d.video);
    opts->emergency = B(d.emergency);
}

int32_t CIMSUE_CALL cimsue_engine_dial(cimsue_engine_t* e, int32_t account_id, const char* target,
                                       const cimsue_call_options_t* opts) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.dial(account_id, S(target), toCxx(opts)), "dial failed");
}
cimsue_status_t CIMSUE_CALL cimsue_engine_answer(cimsue_engine_t* e, int32_t call_id,
                                                 const cimsue_call_options_t* opts) {
    return e ? ret(e->eng.answer(call_id, toCxx(opts))) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_reject(cimsue_engine_t* e, int32_t call_id, int32_t status_code) {
    return e ? ret(e->eng.reject(call_id, status_code)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_hangup(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.hangup(call_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_hold(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.hold(call_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_resume(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.resume(call_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_muted(cimsue_engine_t* e, int32_t call_id, int32_t muted) {
    return e ? ret(e->eng.setMuted(call_id, muted != 0)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_listen(cimsue_engine_t* e, int32_t call_id, int32_t listen) {
    return e ? ret(e->eng.setListen(call_id, listen != 0)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_rx_level(cimsue_engine_t* e, int32_t call_id, float level) {
    return e ? ret(e->eng.setRxLevel(call_id, level)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_send_dtmf(cimsue_engine_t* e, int32_t call_id, const char* digits) {
    return e ? ret(e->eng.sendDtmf(call_id, S(digits))) : -1;
}

void CIMSUE_CALL cimsue_engine_call_info(const cimsue_engine_t* e, int32_t call_id, cimsue_call_info_t* out) {
    if (!out) return;
    g_s.call = e ? e->eng.callInfo(call_id) : CallInfo();
    fill(g_s.callC, g_s.call, g_s.callSrc);
    *out = g_s.callC;
}

int32_t CIMSUE_CALL cimsue_engine_calls(const cimsue_engine_t* e, const int32_t** out) {
    g_s.ids.clear();
    if (e) for (int id : e->eng.calls()) g_s.ids.push_back(id);
    if (out) *out = g_s.ids.empty() ? nullptr : g_s.ids.data();
    return (int32_t)g_s.ids.size();
}

void CIMSUE_CALL cimsue_engine_stream_stats(const cimsue_engine_t* e, int32_t call_id, cimsue_stream_stats_t* out) {
    if (!out) return;
    fill(*out, e ? e->eng.streamStats(call_id) : StreamStats());
}

// MCPTT

void CIMSUE_CALL cimsue_group_call_options_default(cimsue_group_call_options_t* opts) {
    if (!opts) return;
    const GroupCallOptions d;
    *opts = cimsue_group_call_options_t{};
    opts->emergency = B(d.emergency);
    opts->imminent_peril = B(d.imminentPeril);
    opts->listen_only = B(d.listenOnly);
    opts->full_duplex = B(d.fullDuplex);
}

int32_t CIMSUE_CALL cimsue_engine_join_group_call(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                  const cimsue_group_call_options_t* opts) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.joinGroupCall(account_id, S(group_id), toCxx(opts)), "joinGroupCall failed");
}
int32_t CIMSUE_CALL cimsue_engine_start_private_call(cimsue_engine_t* e, int32_t account_id, const char* peer,
                                                     const cimsue_group_call_options_t* opts) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.startPrivateCall(account_id, S(peer), toCxx(opts)), "startPrivateCall failed");
}
cimsue_status_t CIMSUE_CALL cimsue_engine_leave_group_call(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.leaveGroupCall(call_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_floor_request(cimsue_engine_t* e, int32_t call_id, int32_t priority) {
    return e ? ret(e->eng.floorRequest(call_id, priority)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_floor_release(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.floorRelease(call_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_floor_queue_cancel(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.floorQueueCancel(call_id)) : -1;
}

void CIMSUE_CALL cimsue_engine_floor_info(const cimsue_engine_t* e, int32_t call_id, cimsue_floor_info_t* out) {
    if (!out) return;
    g_s.floor = e ? e->eng.floorInfo(call_id) : FloorInfo();
    fill(g_s.floorC, g_s.floor, g_s.floorTalkers);
    *out = g_s.floorC;
}

int64_t CIMSUE_CALL cimsue_engine_affiliate(cimsue_engine_t* e, int32_t account_id, const char* group_id, int32_t on) {
    if (!e) { g_lastError = "no engine"; return -1; }
    int64_t t = e->eng.affiliate(account_id, S(group_id), on != 0);
    if (t < 0) g_lastError = "affiliate failed";
    return t;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_subscribe_conference(cimsue_engine_t* e, int32_t account_id,
                                                               const char* group_id, int32_t on) {
    return e ? ret(e->eng.subscribeConference(account_id, S(group_id), on != 0)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_subscribe_xcap_diff(cimsue_engine_t* e, int32_t account_id,
                                                              const char* psi_uri, int32_t on) {
    return e ? ret(e->eng.subscribeXcapDiff(account_id, S(psi_uri), on != 0)) : -1;
}

int64_t CIMSUE_CALL cimsue_engine_send_request(cimsue_engine_t* e, int32_t account_id, const char* method,
                                               const char* target_uri, const char* content_type, const char* body,
                                               const cimsue_header_t* headers, int32_t header_count) {
    if (!e) { g_lastError = "no engine"; return -1; }
    std::map<std::string, std::string> h;
    for (int32_t i = 0; headers && i < header_count; ++i) h[S(headers[i].name)] = S(headers[i].value);
    int64_t t = e->eng.sendRequest(account_id, S(method), S(target_uri), S(content_type), S(body), h);
    if (t < 0) g_lastError = "sendRequest failed";
    return t;
}

// 관제

cimsue_status_t CIMSUE_CALL cimsue_engine_dialog_watch(cimsue_engine_t* e, int32_t account_id, const char* target_aor,
                                                       int32_t on) {
    return e ? ret(e->eng.dialogWatch(account_id, S(target_aor), on != 0)) : -1;
}

int32_t CIMSUE_CALL cimsue_engine_join(cimsue_engine_t* e, int32_t account_id, const char* target_uri,
                                       const cimsue_dialog_info_t* dlg) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.join(account_id, S(target_uri), toCxx(dlg)), "join failed");
}

int32_t CIMSUE_CALL cimsue_engine_pickup(cimsue_engine_t* e, int32_t account_id, const char* feature_code,
                                         const char* number) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.pickup(account_id, S(feature_code), S(number)), "pickup failed");
}

cimsue_status_t CIMSUE_CALL cimsue_engine_transfer(cimsue_engine_t* e, int32_t call_id, const char* target) {
    return e ? ret(e->eng.transfer(call_id, S(target))) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_transfer_attended(cimsue_engine_t* e, int32_t call_id,
                                                            int32_t consult_call_id) {
    return e ? ret(e->eng.transferAttended(call_id, consult_call_id)) : -1;
}

// MCData SDS

cimsue_status_t CIMSUE_CALL cimsue_engine_send_group_sds(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                         const char* text, int32_t request_delivery, char* msg_id_out,
                                                         int32_t msg_id_cap) {
    if (!e) { g_lastError = "no engine"; return -1; }
    std::string id = e->eng.sendGroupSds(account_id, S(group_id), S(text), request_delivery != 0);
    if (id.empty()) { g_lastError = "sendGroupSds failed"; return -1; }
    copyOut(id, msg_id_out, msg_id_cap);
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_engine_send_sds_notification(cimsue_engine_t* e, int32_t account_id,
                                                                const char* peer, const char* conv_id,
                                                                const char* msg_id, int32_t notif_type) {
    return e ? ret(e->eng.sendSdsNotification(account_id, S(peer), S(conv_id), S(msg_id), notif_type)) : -1;
}

// 장치

int32_t CIMSUE_CALL cimsue_engine_audio_devices(const cimsue_engine_t* e, const cimsue_audio_device_info_t** out) {
    g_s.devs = e ? e->eng.audioDevices() : std::vector<AudioDeviceInfo>();
    g_s.devsC.clear();
    for (const auto& d : g_s.devs)
        g_s.devsC.push_back({d.id, C(d.name), C(d.driver), d.inputCount, d.outputCount});
    if (out) *out = g_s.devsC.empty() ? nullptr : g_s.devsC.data();
    return (int32_t)g_s.devsC.size();
}

cimsue_status_t CIMSUE_CALL cimsue_engine_refresh_audio_devices(cimsue_engine_t* e) {
    return e ? ret(e->eng.refreshAudioDevices()) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_audio_devices(cimsue_engine_t* e, int32_t capture_dev,
                                                            int32_t playback_dev) {
    return e ? ret(e->eng.setAudioDevices(capture_dev, playback_dev)) : -1;
}
int32_t CIMSUE_CALL cimsue_engine_add_playback_route(cimsue_engine_t* e, int32_t playback_dev) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.addPlaybackRoute(playback_dev), "addPlaybackRoute failed");
}
cimsue_status_t CIMSUE_CALL cimsue_engine_remove_playback_route(cimsue_engine_t* e, int32_t route_id) {
    return e ? ret(e->eng.removePlaybackRoute(route_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_call_route(cimsue_engine_t* e, int32_t call_id, int32_t route_id) {
    return e ? ret(e->eng.setCallRoute(call_id, route_id)) : -1;
}

const char* CIMSUE_CALL cimsue_version(void) {
    static const std::string v = Engine::version();
    return v.c_str();
}

const char* CIMSUE_CALL cimsue_last_error(void) { return g_lastError.c_str(); }

const char* CIMSUE_CALL cimsue_reg_state_str(cimsue_reg_state_t s) { return toString((RegState)s); }
const char* CIMSUE_CALL cimsue_call_state_str(cimsue_call_state_t s) { return toString((CallState)s); }
const char* CIMSUE_CALL cimsue_transport_str(cimsue_transport_t t) { return toString((Transport)t); }
const char* CIMSUE_CALL cimsue_floor_state_str(cimsue_floor_state_t s) { return toString((FloorState)s); }
const char* CIMSUE_CALL cimsue_floor_kind_str(cimsue_floor_kind_t k) { return toString((FloorEvent::Kind)k); }

// 문자열 산출 헬퍼

int32_t CIMSUE_CALL cimsue_account_config_aor(const cimsue_account_config_t* cfg, char* out, int32_t cap) {
    return copyOut(toCxx(cfg).aor(), out, cap);
}
int32_t CIMSUE_CALL cimsue_account_config_mcptt_id(const cimsue_account_config_t* cfg, char* out, int32_t cap) {
    return copyOut(toCxx(cfg).effectiveMcpttId(), out, cap);
}
int32_t CIMSUE_CALL cimsue_account_config_digest_username(const cimsue_account_config_t* cfg, char* out, int32_t cap) {
    return copyOut(toCxx(cfg).digestUsername(), out, cap);
}
int32_t CIMSUE_CALL cimsue_account_config_is_complete(const cimsue_account_config_t* cfg) {
    return B(toCxx(cfg).isComplete());
}
int32_t CIMSUE_CALL cimsue_dialog_info_join_header(const cimsue_dialog_info_t* d, char* out, int32_t cap) {
    return copyOut(toCxx(d).joinHeader(), out, cap);
}

// ── CSC ──

void CIMSUE_CALL cimsue_csc_endpoint_default(cimsue_csc_endpoint_t* ep) {
    if (!ep) return;
    const CscEndpoint d;
    *ep = cimsue_csc_endpoint_t{};
    ep->port = d.port;
    ep->verify_server = B(d.verifyServer);
}

cimsue_csc_t* CIMSUE_CALL cimsue_csc_create(const cimsue_csc_endpoint_t* ep) {
    CscEndpoint e;
    if (ep) {
        assignIf(e.host, ep->host);
        e.port = ep->port;
        assignIf(e.clientId, ep->client_id);
        assignIf(e.redirectUri, ep->redirect_uri);
        assignIf(e.scope, ep->scope);
        assignIf(e.caPem, ep->ca_pem);
        e.verifyServer = ep->verify_server != 0;
    }
    auto* c = new (std::nothrow) cimsue_csc();
    if (!c) return nullptr;
    c->cli.reset(new CscClient(e));
    return c;
}

void CIMSUE_CALL cimsue_csc_destroy(cimsue_csc_t* c) { delete c; }

cimsue_status_t CIMSUE_CALL cimsue_csc_login(cimsue_csc_t* c, const char* user_name, const char* password,
                                             cimsue_token_set_t* out) {
    if (!c) return -1;
    c->token = TokenSet();
    cimsue_status_t st = ret(c->cli->login(S(user_name), S(password), c->token));
    fillToken(c);
    if (out) *out = c->tokenC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_refresh(cimsue_csc_t* c, const char* refresh_token, cimsue_token_set_t* out) {
    if (!c) return -1;
    c->token = TokenSet();
    cimsue_status_t st = ret(c->cli->refresh(S(refresh_token), c->token));
    fillToken(c);
    if (out) *out = c->tokenC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_profile(cimsue_csc_t* c, const char* access_token,
                                                     cimsue_profile_t* out) {
    if (!c) return -1;
    c->profile.cxx = Profile();
    cimsue_status_t st = ret(c->cli->fetchProfile(S(access_token), c->profile.cxx));
    c->profile.build();
    if (out) *out = c->profile.out;
    return st;
}

int32_t CIMSUE_CALL cimsue_csc_list_groups(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                           const cimsue_group_summary_t** out) {
    if (!c) return -1;
    c->groups.clear();
    Result r = c->cli->listGroups(S(access_token), S(user_uri), c->groups);
    c->groupsC.clear();
    for (const auto& g : c->groups)
        c->groupsC.push_back({C(g.uri), C(g.displayName), C(g.etag), g.memberCount, B(g.isOwner)});
    if (out) *out = c->groupsC.empty() ? nullptr : c->groupsC.data();
    if (!r.ok) { ret(r); return -1; }
    return (int32_t)c->groupsC.size();
}

cimsue_status_t CIMSUE_CALL cimsue_csc_get_group(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                 const char* group_uri, cimsue_group_doc_t* out) {
    if (!c) return -1;
    c->group.cxx = GroupDoc();
    cimsue_status_t st = ret(c->cli->getGroup(S(access_token), S(user_uri), S(group_uri), c->group.cxx));
    c->group.build();
    if (out) *out = c->group.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_put_group(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                 const cimsue_group_doc_t* doc, const char* if_match, cimsue_group_doc_t* out) {
    if (!c || !doc) return -1;
    GroupDoc in = toCxx(doc);
    c->group.cxx = GroupDoc();
    cimsue_status_t st = ret(c->cli->putGroup(S(access_token), S(user_uri), in, S(if_match), c->group.cxx));
    c->group.build();
    if (out) *out = c->group.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_delete_group(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                    const char* group_uri) {
    if (!c) return -1;
    return ret(c->cli->deleteGroup(S(access_token), S(user_uri), S(group_uri)));
}

int32_t CIMSUE_CALL cimsue_group_doc_to_xml(const cimsue_group_doc_t* doc, char* out, int32_t cap) {
    return copyOut(toCxx(doc).toXml(), out, cap);
}

cimsue_status_t CIMSUE_CALL cimsue_group_doc_parse(const char* xml, cimsue_group_doc_t* out) {
    g_s.groupDoc.cxx = GroupDoc();
    std::string err;
    bool ok = GroupDoc::parse(S(xml), g_s.groupDoc.cxx, &err);
    g_s.groupDoc.build();
    if (out) *out = g_s.groupDoc.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_xcap_get(cimsue_csc_t* c, const char* access_token, const char* path,
                                                const char* accept, const char* if_none_match,
                                                cimsue_xcap_doc_t* out) {
    if (!c) return -1;
    c->doc = XcapDoc();
    cimsue_status_t st = ret(c->cli->xcapGet(S(access_token), S(path), S(accept), S(if_none_match), c->doc));
    fillDoc(c);
    if (out) *out = c->docC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_get_user_profile(cimsue_csc_t* c, const char* access_token,
                                                        const char* user_uri, const char* etag,
                                                        cimsue_xcap_doc_t* out) {
    if (!c) return -1;
    c->doc = XcapDoc();
    cimsue_status_t st = ret(c->cli->getUserProfile(S(access_token), S(user_uri), S(etag), c->doc));
    fillDoc(c);
    if (out) *out = c->docC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_get_service_config(cimsue_csc_t* c, const char* access_token,
                                                          const char* user_uri, const char* etag,
                                                          cimsue_xcap_doc_t* out) {
    if (!c) return -1;
    c->doc = XcapDoc();
    cimsue_status_t st = ret(c->cli->getServiceConfig(S(access_token), S(user_uri), S(etag), c->doc));
    fillDoc(c);
    if (out) *out = c->docC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_parse_profile(const char* json, cimsue_profile_t* out) {
    g_s.profile.cxx = Profile();
    std::string err;
    bool ok = CscClient::parseProfile(S(json), g_s.profile.cxx, &err);
    g_s.profile.build();
    if (out) *out = g_s.profile.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

int32_t CIMSUE_CALL cimsue_csc_enc(const char* s, char* out, int32_t cap) {
    return copyOut(CscClient::enc(S(s)), out, cap);
}

const cimsue_service_profile_t* CIMSUE_CALL cimsue_profile_service(const cimsue_profile_t* profile, const char* kind) {
    if (!profile || !profile->services) return nullptr;
    const std::string k = S(kind);
    for (int32_t i = 0; i < profile->service_count; ++i)
        if (profile->services[i].kind && k == profile->services[i].kind) return &profile->services[i];
    return nullptr;
}

void CIMSUE_CALL cimsue_service_profile_to_account(const cimsue_service_profile_t* sp, const char* login_pw,
                                                   cimsue_account_config_t* out) {
    if (!out) return;
    g_s.acc = toCxx(sp).toAccount(S(login_pw));
    cimsue_account_config_t c{};
    fill(c, g_s.acc, g_s.accSec);
    *out = c;
}

// ── ABI 자기검사 ──

int32_t CIMSUE_CALL cimsue_struct_size(cimsue_struct_id_t id) {
    switch (id) {
    case CIMSUE_STRUCT_ENGINE_CONFIG:     return (int32_t)sizeof(cimsue_engine_config_t);
    case CIMSUE_STRUCT_ACCOUNT_CONFIG:    return (int32_t)sizeof(cimsue_account_config_t);
    case CIMSUE_STRUCT_CALL_OPTIONS:      return (int32_t)sizeof(cimsue_call_options_t);
    case CIMSUE_STRUCT_GROUP_CALL_OPTIONS: return (int32_t)sizeof(cimsue_group_call_options_t);
    case CIMSUE_STRUCT_HEADER:            return (int32_t)sizeof(cimsue_header_t);
    case CIMSUE_STRUCT_REG_INFO:          return (int32_t)sizeof(cimsue_reg_info_t);
    case CIMSUE_STRUCT_MCPTT_INFO:        return (int32_t)sizeof(cimsue_mcptt_info_t);
    case CIMSUE_STRUCT_MEDIA_SOURCE:      return (int32_t)sizeof(cimsue_media_source_t);
    case CIMSUE_STRUCT_CALL_INFO:         return (int32_t)sizeof(cimsue_call_info_t);
    case CIMSUE_STRUCT_TALKER:            return (int32_t)sizeof(cimsue_talker_t);
    case CIMSUE_STRUCT_FLOOR_EVENT:       return (int32_t)sizeof(cimsue_floor_event_t);
    case CIMSUE_STRUCT_FLOOR_INFO:        return (int32_t)sizeof(cimsue_floor_info_t);
    case CIMSUE_STRUCT_REQUEST_RESULT:    return (int32_t)sizeof(cimsue_request_result_t);
    case CIMSUE_STRUCT_DIALOG_INFO:       return (int32_t)sizeof(cimsue_dialog_info_t);
    case CIMSUE_STRUCT_ROSTER_ENTRY:      return (int32_t)sizeof(cimsue_roster_entry_t);
    case CIMSUE_STRUCT_SDS_MESSAGE:       return (int32_t)sizeof(cimsue_sds_message_t);
    case CIMSUE_STRUCT_STREAM_STATS:      return (int32_t)sizeof(cimsue_stream_stats_t);
    case CIMSUE_STRUCT_AUDIO_DEVICE_INFO: return (int32_t)sizeof(cimsue_audio_device_info_t);
    case CIMSUE_STRUCT_LISTENER:          return (int32_t)sizeof(cimsue_listener_t);
    case CIMSUE_STRUCT_CSC_ENDPOINT:      return (int32_t)sizeof(cimsue_csc_endpoint_t);
    case CIMSUE_STRUCT_TOKEN_SET:         return (int32_t)sizeof(cimsue_token_set_t);
    case CIMSUE_STRUCT_SERVICE_ENDPOINT:  return (int32_t)sizeof(cimsue_service_endpoint_t);
    case CIMSUE_STRUCT_SERVICE_PROFILE:   return (int32_t)sizeof(cimsue_service_profile_t);
    case CIMSUE_STRUCT_DISPATCH_PROFILE:  return (int32_t)sizeof(cimsue_dispatch_profile_t);
    case CIMSUE_STRUCT_PROFILE:           return (int32_t)sizeof(cimsue_profile_t);
    case CIMSUE_STRUCT_GROUP_SUMMARY:     return (int32_t)sizeof(cimsue_group_summary_t);
    case CIMSUE_STRUCT_XCAP_DOC:          return (int32_t)sizeof(cimsue_xcap_doc_t);
    case CIMSUE_STRUCT_DISPATCH_MEMBER:   return (int32_t)sizeof(cimsue_dispatch_member_t);
    case CIMSUE_STRUCT_DISPATCH_TARGET:   return (int32_t)sizeof(cimsue_dispatch_target_t);
    case CIMSUE_STRUCT_GROUP_MEMBER:      return (int32_t)sizeof(cimsue_group_member_t);
    case CIMSUE_STRUCT_GROUP_DOC:         return (int32_t)sizeof(cimsue_group_doc_t);
    default:                              return -1;
    }
}

}  // extern "C"
