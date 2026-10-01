// libcimsue — C API 구현 (cimsue_c.h · ue_sdk.md §6.4)
//
// 공개 C++ 표면의 얇은 평탄화 층이다. 여기에는 프로토콜 로직이 없다 — 타입 변환과 수명 규약만 둔다.
//   - 입력 POD → C++ 객체: NULL 문자열 필드는 C++ 기본값을 그대로 둔다(설정 구조체의 default 규약).
//   - C++ 객체 → 산출 POD: 문자열은 소유자 객체를 가리킨다. 소유자는 콜백 인자(스택) 또는
//     스레드별 스냅샷(getter)·핸들 스냅샷(CSC) 이며, 그 수명이 곧 헤더가 약속한 유효 구간이다.
#include "cimsue/cimsue_c.h"

#include <algorithm>
#include <cstring>
#include <ctime>
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
    e.udpNoTcpSwitch = c->udp_no_tcp_switch != 0;
    e.grantMicDelayMs = c->grant_mic_delay_ms;
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
    assignIf(a.instanceId, c->instance_id);
    assignIf(a.mcpttClientId, c->mcptt_client_id);
    assignIf(a.rpEmergency, c->rp_emergency);
    assignIf(a.rpImminentPeril, c->rp_imminent_peril);
    assignIf(a.rpNormal, c->rp_normal);
    a.maxSdsCplaneBytes = c->max_sds_cplane_bytes;
    a.mcdataMsrp = c->mcdata_msrp != 0;
    assignIf(a.mcpttServerUri, c->mcptt_server_uri);
    assignIf(a.mcdataServerUri, c->mcdata_server_uri);
    a.mcvideoEnabled = c->mcvideo_enabled != 0;
    assignIf(a.mcvideoServerUri, c->mcvideo_server_uri);
    a.autoAnswerMcvideo = c->auto_answer_mcvideo != 0;
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
    o.broadcast = c->broadcast != 0;
    o.implicitFloorRequest = c->implicit_floor_request != 0;
    return o;
}

VideoGroupCallOptions toCxx(const cimsue_video_group_call_options_t* c) {
    VideoGroupCallOptions o;
    if (!c) return o;
    o.prearranged = c->prearranged != 0;
    o.queueing = c->queueing != 0;
    o.maxPriority = c->max_priority;
    o.maxReceptionPriority = c->max_reception_priority;
    o.implicitTransmissionRequest = c->implicit_transmission_request != 0;
    assignIf(o.sessionUri, c->session_uri);
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
    s.udpNoTcpSwitch = c->udp_no_tcp_switch != 0;
    s.smsGateway = c->sms_gateway != 0;
    return s;
}

/** CMS 문서 입력(C → C++) — capabilities_of 가 코어 규칙(Capabilities::of)을 그대로 쓰게. 판정에 쓰는 필드만 옮긴다. */
UserProfileDoc toCxx(const cimsue_user_profile_doc_t* c) {
    UserProfileDoc u;
    if (!c) return u;
    u.maxAffiliationsN2 = c->max_affiliations_n2;
    u.allowPrivateCall = c->allow_private_call != 0;
    u.allowEmergencyGroupCall = c->allow_emergency_group_call != 0;
    u.allowImminentPerilCall = c->allow_imminent_peril_call != 0;
    u.allowActivateEmergencyAlert = c->allow_activate_emergency_alert != 0;
    u.allowCancelEmergencyAlert = c->allow_cancel_emergency_alert != 0;
    u.allowEmergencyPrivateCall = c->allow_emergency_private_call != 0;
    u.allowAdhocGroupCall = c->allow_adhoc_group_call != 0;
    u.allowCancelGroupEmergency = c->allow_cancel_group_emergency != 0;
    u.allowCancelImminentPeril = c->allow_cancel_imminent_peril != 0;
    return u;
}

ServiceConfigDoc toCxx(const cimsue_service_config_doc_t* c) {
    ServiceConfigDoc s;
    if (!c) return s;
    s.domain = S(c->domain);
    s.numLevelsGroupHierarchy = c->num_levels_group_hierarchy;
    s.numLevelsUserHierarchy = c->num_levels_user_hierarchy;
    s.rpEmergency = S(c->rp_emergency); s.rpImminentPeril = S(c->rp_imminent_peril); s.rpNormal = S(c->rp_normal);
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
    o.broadcast = B(m.broadcast);
}

/** sources·non_ack_users 배열은 호출자가 준 벡터에 담는다(그 벡터가 소유자). */
void fill(cimsue_call_info_t& o, const CallInfo& c, std::vector<cimsue_media_source_t>& srcBuf, std::vector<const char*>& ackBuf) {
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
    o.rx_level = c.rxLevel;
    o.condition.emergency = B(c.condition.emergency);
    o.condition.imminent_peril = B(c.condition.imminentPeril);
    o.condition.mine = B(c.condition.mine);
    o.condition.pending = B(c.condition.pending);
    o.condition.last_code = c.condition.lastCode;
    o.answer_state = C(c.answerState);
    ackBuf.clear();
    for (const auto& u : c.nonAcknowledgedUsers) ackBuf.push_back(C(u));
    o.non_ack_users = ackBuf.empty() ? nullptr : ackBuf.data();
    o.non_ack_user_count = (int32_t)ackBuf.size();
    o.service = (cimsue_mc_service_t)c.service;
    o.session_uri = C(c.sessionUri);
    o.video_send = B(c.videoSend);
}

void fill(cimsue_video_transmitter_t& o, const VideoTransmitter& t) {
    o.user_id = C(t.userId);
    o.audio_ssrc = t.audioSsrc; o.video_ssrc = t.videoSsrc;
    o.functional_alias = C(t.functionalAlias);
    o.automatic = B(t.automatic);
    o.state = (cimsue_reception_state_t)t.state;
}

void fill(cimsue_transmission_event_t& o, const TransmissionEvent& e) {
    o.kind = (cimsue_transmission_kind_t)e.kind;
    o.call_id = e.callId;
    o.state = (cimsue_transmission_state_t)e.state;
    o.cause = e.cause;
    o.cause_text = C(e.causeText);
    o.duration_sec = e.durationSec;
    o.priority = e.priority;
    o.queue_position = e.queuePosition;
    o.indicator = e.indicator;
    o.audio_ssrc = e.audioSsrc; o.video_ssrc = e.videoSsrc;
    o.receiver_id = C(e.receiverId);
    o.raw_type = e.rawType;
}

void fill(cimsue_reception_event_t& o, const ReceptionEvent& e) {
    o.kind = (cimsue_reception_kind_t)e.kind;
    o.call_id = e.callId;
    fill(o.transmitter, e.transmitter);
    o.cause = e.cause;
    o.cause_text = C(e.causeText);
    o.raw_type = e.rawType;
}

void fill(cimsue_transmission_info_t& o, const TransmissionInfo& t, std::vector<cimsue_video_transmitter_t>& buf) {
    o.state = (cimsue_transmission_state_t)t.state;
    buf.clear();
    for (const auto& x : t.transmitters) { cimsue_video_transmitter_t v{}; fill(v, x); buf.push_back(v); }
    o.transmitters = buf.empty() ? nullptr : buf.data();
    o.transmitter_count = (int32_t)buf.size();
    o.queue_position = t.queuePosition;
    o.local_port = t.localPort;
    o.remote_ip = C(t.remoteIp);
    o.remote_port = t.remotePort;
}

void fill(cimsue_emergency_alert_t& o, const EmergencyAlert& a) {
    o.account_id = a.accountId;
    o.group_id = C(a.groupId); o.user_id = C(a.userId); o.originated_by = C(a.originatedBy); o.mc_org = C(a.mcOrg);
    o.alert_ind = a.alertInd; o.emergency_ind = a.emergencyInd; o.imminent_peril_ind = a.imminentPerilInd;
    o.self = B(a.self);
}

void fill(cimsue_tls_peer_expiry_t& o, const TlsPeerExpiry& t) {
    o.valid = B(t.valid);
    o.not_after_epoch = t.notAfterEpoch;
    o.observed_epoch = t.observedEpoch;
    o.days_left = t.daysLeft((int64_t)std::time(nullptr));
    o.subject = C(t.subject);
    o.remote = C(t.remote);
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
    o.media_plane = B(m.mediaPlane);
}

void fill(cimsue_stream_stats_t& o, const StreamStats& s) {
    o.rx_packets = s.rxPackets; o.rx_bytes = s.rxBytes; o.rx_loss = s.rxLoss; o.rx_discard = s.rxDiscard;
    o.tx_packets = s.txPackets; o.tx_bytes = s.txBytes;
    o.valid = B(s.valid);
}

void fill(cimsue_quality_direction_t& o, const QualityDirection& d) {
    o.valid = B(d.valid);
    o.packets = d.packets; o.lost = d.lost; o.discarded = d.discarded;
    o.loss_pct = d.lossPct; o.discard_pct = d.discardPct; o.jitter_ms = d.jitterMs; o.jitter_max_ms = d.jitterMaxMs;
    o.burst_density_pct = d.burstDensityPct; o.gap_density_pct = d.gapDensityPct;
    o.burst_ms = d.burstMs; o.gap_ms = d.gapMs;
    o.signal_dbm = d.signalDbm; o.noise_dbm = d.noiseDbm;
}

void fill(cimsue_call_quality_t& o, const CallQuality& q) {
    o.valid = B(q.valid);
    o.codec = C(q.codec);
    o.clock_rate = q.clockRate;
    o.wideband = B(q.wideband);
    fill(o.rx, q.rx);
    fill(o.remote, q.remote);
    o.rtd_ms = q.rtdMs; o.esd_ms = q.esdMs; o.one_way_ms = q.oneWayMs;
    o.r_lq = q.rLq; o.r_cq = q.rCq; o.mos_lq = q.mosLq; o.mos_cq = q.mosCq;
    o.start_epoch_ms = q.startEpochMs; o.duration_ms = q.durationMs;
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
    o.instance_id = C(a.instanceId);
    o.mcptt_client_id = C(a.mcpttClientId);
    o.rp_emergency = C(a.rpEmergency); o.rp_imminent_peril = C(a.rpImminentPeril); o.rp_normal = C(a.rpNormal);
    o.max_sds_cplane_bytes = a.maxSdsCplaneBytes;
    o.mcdata_msrp = B(a.mcdataMsrp);
    o.mcptt_server_uri = C(a.mcpttServerUri);
    o.mcdata_server_uri = C(a.mcdataServerUri);
    o.mcvideo_enabled = B(a.mcvideoEnabled);
    o.mcvideo_server_uri = C(a.mcvideoServerUri);
    o.auto_answer_mcvideo = B(a.autoAnswerMcvideo);
}

/** CMS 문서의 C 스냅샷 — 핸들(fetch)과 스레드 스크래치(parse) 양쪽이 쓴다. */
struct UserProfileHolder {
    UserProfileDoc cxx;
    std::vector<const char*> groups, implicit;
    cimsue_user_profile_doc_t out{};

    void build() {
        auto entry = [](const CmsEntry& e) { return cimsue_cms_entry_t{C(e.uri), C(e.mode)}; };
        groups.clear(); implicit.clear();
        for (const auto& g : cxx.groups) groups.push_back(C(g));
        for (const auto& g : cxx.implicitAffiliations) implicit.push_back(C(g));
        out = cimsue_user_profile_doc_t{};
        out.etag = C(cxx.etag); out.not_modified = B(cxx.notModified); out.user_uri = C(cxx.userUri);
        out.emergency_group = entry(cxx.emergencyGroup);
        out.imminent_peril_group = entry(cxx.imminentPerilGroup);
        out.emergency_alert_group = entry(cxx.emergencyAlertGroup);
        out.emergency_private_recipient = entry(cxx.emergencyPrivateRecipient);
        out.groups = groups.empty() ? nullptr : groups.data();
        out.group_count = (int32_t)groups.size();
        out.implicit_affiliations = implicit.empty() ? nullptr : implicit.data();
        out.implicit_affiliation_count = (int32_t)implicit.size();
        out.max_affiliations_n2 = cxx.maxAffiliationsN2;
        out.allow_private_call = B(cxx.allowPrivateCall);
        out.allow_emergency_group_call = B(cxx.allowEmergencyGroupCall);
        out.allow_imminent_peril_call = B(cxx.allowImminentPerilCall);
        out.allow_activate_emergency_alert = B(cxx.allowActivateEmergencyAlert);
        out.allow_cancel_emergency_alert = B(cxx.allowCancelEmergencyAlert);
        out.allow_emergency_private_call = B(cxx.allowEmergencyPrivateCall);
        out.allow_adhoc_group_call = B(cxx.allowAdhocGroupCall);
        out.allow_cancel_group_emergency = B(cxx.allowCancelGroupEmergency);
        out.allow_cancel_imminent_peril = B(cxx.allowCancelImminentPeril);
    }
};

struct McVideoUserProfileHolder {
    McVideoUserProfileDoc cxx;
    std::vector<const char*> groups, implicit;
    cimsue_mcvideo_user_profile_doc_t out{};

    void build() {
        auto entry = [](const CmsEntry& e) { return cimsue_cms_entry_t{C(e.uri), C(e.mode)}; };
        groups.clear(); implicit.clear();
        for (const auto& g : cxx.groups) groups.push_back(C(g));
        for (const auto& g : cxx.implicitAffiliations) implicit.push_back(C(g));
        out = cimsue_mcvideo_user_profile_doc_t{};
        out.etag = C(cxx.etag); out.not_modified = B(cxx.notModified); out.user_uri = C(cxx.userUri); out.mcvideo_id = C(cxx.mcvideoId);
        out.groups = groups.empty() ? nullptr : groups.data();
        out.group_count = (int32_t)groups.size();
        out.implicit_affiliations = implicit.empty() ? nullptr : implicit.data();
        out.implicit_affiliation_count = (int32_t)implicit.size();
        out.max_affiliations_n2 = cxx.maxAffiliationsN2;
        out.max_simultaneous_video_streams = cxx.maxSimultaneousVideoStreams;
        out.max_simultaneous_calls_n6 = cxx.maxSimultaneousCallsN6;
        out.emergency_group = entry(cxx.emergencyGroup);
        out.imminent_peril_group = entry(cxx.imminentPerilGroup);
        out.emergency_alert_group = entry(cxx.emergencyAlertGroup);
        out.allow_private_call = B(cxx.allowPrivateCall);
        out.allow_emergency_group_call = B(cxx.allowEmergencyGroupCall);
        out.allow_emergency_private_call = B(cxx.allowEmergencyPrivateCall);
        out.allow_imminent_peril_call = B(cxx.allowImminentPerilCall);
        out.allow_activate_emergency_alert = B(cxx.allowActivateEmergencyAlert);
        out.allow_revoke_transmit = B(cxx.allowRevokeTransmit);
        out.allow_remote_ambient_viewing = B(cxx.allowRemoteAmbientViewing);
        out.allow_local_ambient_viewing = B(cxx.allowLocalAmbientViewing);
        out.allow_adhoc_group_call = B(cxx.allowAdhocGroupCall);
    }
};
struct McVideoServiceConfigHolder {
    McVideoServiceConfigDoc cxx;
    cimsue_mcvideo_service_config_doc_t out{};

    void build() {
        out = cimsue_mcvideo_service_config_doc_t{};
        out.etag = C(cxx.etag); out.not_modified = B(cxx.notModified); out.domain = C(cxx.domain);
        out.rp_emergency = C(cxx.rpEmergency); out.rp_imminent_peril = C(cxx.rpImminentPeril); out.rp_normal = C(cxx.rpNormal);
        out.confidentiality_protection = B(cxx.confidentialityProtection);
        out.integrity_protection = B(cxx.integrityProtection);
        out.t100_sec = cxx.t100Sec; out.t101_sec = cxx.t101Sec; out.t102_sec = cxx.t102Sec; out.t103_sec = cxx.t103Sec; out.t104_sec = cxx.t104Sec;
    }
};
struct ServiceConfigHolder {
    ServiceConfigDoc cxx;
    cimsue_service_config_doc_t out{};

    void build() {
        out = cimsue_service_config_doc_t{};
        out.etag = C(cxx.etag); out.not_modified = B(cxx.notModified); out.domain = C(cxx.domain);
        out.num_levels_group_hierarchy = cxx.numLevelsGroupHierarchy;
        out.num_levels_user_hierarchy = cxx.numLevelsUserHierarchy;
        out.rp_emergency = C(cxx.rpEmergency); out.rp_imminent_peril = C(cxx.rpImminentPeril); out.rp_normal = C(cxx.rpNormal);
    }
};

struct UeInitConfigHolder {
    UeInitConfigDoc cxx;
    cimsue_ue_init_config_doc_t out{};

    void build() {
        out = cimsue_ue_init_config_doc_t{};
        out.etag = C(cxx.etag); out.not_modified = B(cxx.notModified); out.domain = C(cxx.domain);
        out.mcptt_server_uri = C(cxx.mcpttServerUri); out.mcdata_server_uri = C(cxx.mcdataServerUri);
        out.mcvideo_server_uri = C(cxx.mcvideoServerUri);
    }
};

/** Profile 한 벌의 소유자 — C++ 객체와 그것을 가리키는 POD 배열을 함께 들고 있는다. */
/** GroupDoc 의 C 스냅샷 — 핸들(getter 산출)과 스레드 스크래치(parse) 양쪽이 쓴다. */
/** MCVideo 몫 기본값(C++ McVideoGroupAttrs 와 같다). */
void fillDefault(cimsue_mcvideo_group_attrs_t& o) {
    const McVideoGroupAttrs d;
    o = cimsue_mcvideo_group_attrs_t{};
    o.present = B(d.present);
    o.invite_members = B(d.inviteMembers);
    o.max_duration_sec = d.maxDurationSec;
    o.protect_media = B(d.protectMedia);
    o.protect_transmission_control = B(d.protectTransmissionControl);
    o.urgent_real_time_video_mode = d.urgentRealTimeVideoMode;
    o.non_urgent_real_time_video_mode = d.nonUrgentRealTimeVideoMode;
    o.non_real_time_video_mode = d.nonRealTimeVideoMode;
    o.max_transmitters = d.maxTransmitters;
    o.min_number_to_start = d.minNumberToStart;
    o.group_priority = d.groupPriority;
    o.reception_hang_timer_sec = d.receptionHangTimerSec;
    o.allow_conference_state = d.allowConferenceState;
    o.allow_emergency_call = d.allowEmergencyCall;
    o.allow_emergency_alert = d.allowEmergencyAlert;
    o.allow_imminent_peril_call = d.allowImminentPerilCall;
}

McVideoGroupAttrs toCxx(const cimsue_mcvideo_group_attrs_t& c) {
    McVideoGroupAttrs a;
    a.present = c.present != 0;
    if (!a.present) return a;                         // 전환기 — MCVideo 를 싣지 않는다(나머지는 보지 않는다)
    a.inviteMembers = c.invite_members != 0;
    a.maxDurationSec = c.max_duration_sec;
    a.protectMedia = c.protect_media != 0;
    a.protectTransmissionControl = c.protect_transmission_control != 0;
    a.audioEncodings = strList(c.audio_encodings, c.audio_encoding_count);
    a.videoEncodings = strList(c.video_encodings, c.video_encoding_count);
    a.videoResolutions = S(c.video_resolutions);
    a.videoFrameRate = S(c.video_frame_rate);
    a.urgentRealTimeVideoMode = c.urgent_real_time_video_mode;
    a.nonUrgentRealTimeVideoMode = c.non_urgent_real_time_video_mode;
    a.nonRealTimeVideoMode = c.non_real_time_video_mode;
    a.activeRealTimeVideoMode = S(c.active_real_time_video_mode);
    a.maxTransmitters = c.max_transmitters;
    a.minNumberToStart = c.min_number_to_start;
    a.groupPriority = c.group_priority;
    a.receptionHangTimerSec = c.reception_hang_timer_sec;
    a.allowConferenceState = c.allow_conference_state;
    a.allowEmergencyCall = c.allow_emergency_call;
    a.allowEmergencyAlert = c.allow_emergency_alert;
    a.allowImminentPerilCall = c.allow_imminent_peril_call;
    return a;
}

struct GroupDocHolder {
    GroupDoc cxx;
    std::vector<cimsue_group_member_t> mem;
    std::vector<const char*> audioEnc, videoEnc;
    cimsue_group_doc_t out{};

    void build() {
        mem.clear();
        for (const auto& m : cxx.members)
            mem.push_back({C(m.uri), C(m.name), C(m.role), m.priority, B(m.required), C(m.title), C(m.mcvideoId)});
        out = cimsue_group_doc_t{};
        out.uri = C(cxx.uri); out.display_name = C(cxx.displayName); out.etag = C(cxx.etag);
        out.members = mem.empty() ? nullptr : mem.data();
        out.member_count = (int32_t)mem.size();
        out.session_type = C(cxx.sessionType);
        out.encryption = B(cxx.encryption);
        out.emergency_call = B(cxx.emergencyCall); out.emergency_alert = B(cxx.emergencyAlert);
        out.allow_sds = B(cxx.allowSds); out.allow_fd = B(cxx.allowFd); out.require_affiliation = B(cxx.requireAffiliation);
        out.priority = cxx.priority; out.max_participants = cxx.maxParticipants;
        out.org_code = C(cxx.orgCode); out.authorized_user = C(cxx.authorizedUser);
        // C++ 의 kUnset(-1) ↔ has_* = 0. 값이 있으면 has_* = 1.
        out.has_hang_timer = cxx.hangTimerSec >= 0;             out.hang_timer_sec = cxx.hangTimerSec >= 0 ? cxx.hangTimerSec : 0;
        out.has_max_duration = cxx.maxDurationSec >= 0;         out.max_duration_sec = cxx.maxDurationSec >= 0 ? cxx.maxDurationSec : 0;
        out.has_conference_state = cxx.allowConferenceState >= 0;
        out.allow_conference_state = cxx.allowConferenceState > 0;
        out.has_max_sds_size = cxx.maxSdsSize >= 0;             out.max_sds_size = cxx.maxSdsSize >= 0 ? cxx.maxSdsSize : 0;
        out.has_max_auto_recv = cxx.maxAutoRecv >= 0;           out.max_auto_recv = cxx.maxAutoRecv >= 0 ? cxx.maxAutoRecv : 0;
        out.has_min_number_to_start = cxx.minNumberToStart >= 0;
        out.min_number_to_start = cxx.minNumberToStart >= 0 ? cxx.minNumberToStart : 0;
        out.has_ack_timeout = cxx.ackTimeoutSec >= 0;           out.ack_timeout_sec = cxx.ackTimeoutSec >= 0 ? cxx.ackTimeoutSec : 0;
        out.ack_action = cxx.ackAction.empty() ? nullptr : C(cxx.ackAction);
        const McVideoGroupAttrs& v = cxx.mcvideo;
        cimsue_mcvideo_group_attrs_t& o = out.mcvideo;
        fillDefault(o);
        o.present = B(v.present);
        o.invite_members = B(v.inviteMembers);
        o.max_duration_sec = v.maxDurationSec;
        o.protect_media = B(v.protectMedia);
        o.protect_transmission_control = B(v.protectTransmissionControl);
        audioEnc.clear(); videoEnc.clear();
        for (const auto& e : v.audioEncodings) audioEnc.push_back(C(e));
        for (const auto& e : v.videoEncodings) videoEnc.push_back(C(e));
        o.audio_encodings = audioEnc.empty() ? nullptr : audioEnc.data();
        o.audio_encoding_count = (int32_t)audioEnc.size();
        o.video_encodings = videoEnc.empty() ? nullptr : videoEnc.data();
        o.video_encoding_count = (int32_t)videoEnc.size();
        o.video_resolutions = C(v.videoResolutions);
        o.video_frame_rate = C(v.videoFrameRate);
        o.urgent_real_time_video_mode = v.urgentRealTimeVideoMode;
        o.non_urgent_real_time_video_mode = v.nonUrgentRealTimeVideoMode;
        o.non_real_time_video_mode = v.nonRealTimeVideoMode;
        o.active_real_time_video_mode = C(v.activeRealTimeVideoMode);
        o.max_transmitters = v.maxTransmitters;
        o.min_number_to_start = v.minNumberToStart;
        o.group_priority = v.groupPriority;
        o.reception_hang_timer_sec = v.receptionHangTimerSec;
        o.allow_conference_state = v.allowConferenceState;
        o.allow_emergency_call = v.allowEmergencyCall;
        o.allow_emergency_alert = v.allowEmergencyAlert;
        o.allow_imminent_peril_call = v.allowImminentPerilCall;
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
        m.required = d->members[i].required != 0;
        m.mcvideoId = S(d->members[i].mcvideo_id);
        g.members.push_back(m);
    }
    if (d->session_type && *d->session_type) g.sessionType = d->session_type;
    g.encryption = d->encryption != 0;
    g.emergencyCall = d->emergency_call != 0; g.emergencyAlert = d->emergency_alert != 0;
    g.allowSds = d->allow_sds != 0; g.allowFd = d->allow_fd != 0; g.requireAffiliation = d->require_affiliation != 0;
    g.priority = d->priority; g.maxParticipants = d->max_participants;
    g.orgCode = S(d->org_code); g.authorizedUser = S(d->authorized_user);
    // has_* = 0 이면 kUnset 으로 둔다 — 값이 0 이어도 «미기재» 다(0 초기화된 입력이 서버 값을 덮지 않게).
    //   음수 값은 받지 않는다(서버도 범위 밖은 400) — 미기재로 떨어뜨린다.
    auto opt = [](int32_t has, int32_t v) { return has && v >= 0 ? (int)v : GroupDoc::kUnset; };
    g.hangTimerSec = opt(d->has_hang_timer, d->hang_timer_sec);
    g.maxDurationSec = opt(d->has_max_duration, d->max_duration_sec);
    g.allowConferenceState = d->has_conference_state ? (d->allow_conference_state != 0 ? 1 : 0) : GroupDoc::kUnset;
    g.maxSdsSize = opt(d->has_max_sds_size, d->max_sds_size);
    g.maxAutoRecv = opt(d->has_max_auto_recv, d->max_auto_recv);
    g.minNumberToStart = opt(d->has_min_number_to_start, d->min_number_to_start);
    g.ackTimeoutSec = opt(d->has_ack_timeout, d->ack_timeout_sec);
    if (d->ack_action && *d->ack_action) g.ackAction = std::string(d->ack_action) == "proceed" ? "proceed" : "abandon";
    g.mcvideo = toCxx(d->mcvideo);
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
            o.udp_no_tcp_switch = B(s.udpNoTcpSwitch);
            o.sms_gateway = B(s.smsGateway);
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
        out.dispatch.directory_admin = C(d.directoryAdmin);
        out.dispatch.org_code = C(d.orgCode);
        members.clear(); targets.clear();
        for (const auto& m : d.members) members.push_back({C(m.userId), C(m.name), C(m.volteAor), C(m.pttId), C(m.extension), C(m.groupId)});
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
    std::vector<const char*>                callAck;
    FloorInfo                               floor;
    cimsue_floor_info_t                     floorC{};
    std::vector<cimsue_talker_t>            floorTalkers;
    std::vector<int32_t>                    ids;
    std::vector<AudioDeviceInfo>            devs;
    std::vector<cimsue_audio_device_info_t> devsC;
    std::vector<VideoDeviceInfo>            vdevs;
    std::vector<cimsue_video_device_info_t> vdevsC;
    UserProfileHolder                       userProfile;
    ServiceConfigHolder                     serviceConfig;
    UeInitConfigHolder                      ueInitConfig;
    McVideoUserProfileHolder                mcvideoUserProfile;
    McVideoServiceConfigHolder              mcvideoServiceConfig;
    AccountConfig                           acc;
    std::vector<const char*>                accSec;
    ProfileHolder                           profile;
    GroupDocHolder                          groupDoc;
    TlsPeerExpiry                           tlsPeer;
    cimsue_tls_peer_expiry_t                tlsPeerC{};
    CallQuality                             quality;
    TransmissionInfo                        tx;
    cimsue_transmission_info_t              txC{};
    std::vector<cimsue_video_transmitter_t> txTransmitters;
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
    void onMcpttCondition(const CallInfo& info, ConditionCause cause) override {
        if (!cb.on_mcptt_condition) return;
        cimsue_call_info_t o{}; std::vector<cimsue_media_source_t> src; std::vector<const char*> ack; fill(o, info, src, ack);
        cb.on_mcptt_condition(cb.user, &o, (cimsue_condition_cause_t)cause);
    }
    void onNonAcknowledgedUsers(const CallInfo& info) override { call(cb.on_non_acknowledged_users, info); }
    void onTransmission(const TransmissionEvent& ev) override {
        if (!cb.on_transmission) return;
        cimsue_transmission_event_t o{}; fill(o, ev);
        cb.on_transmission(cb.user, &o);
    }
    void onReception(const ReceptionEvent& ev) override {
        if (!cb.on_reception) return;
        cimsue_reception_event_t o{}; fill(o, ev);
        cb.on_reception(cb.user, &o);
    }
    void onVideoFrame(const VideoFrame& f) override {             // 영상 스레드 — 복사 없이 넘긴다(콜백 동안만 유효)
        if (!cb.on_video_frame) return;
        cimsue_video_frame_t o{f.callId, f.width, f.height, f.stride, f.data, (int64_t)f.size};
        cb.on_video_frame(cb.user, &o);
    }
    void onEmergencyAlert(const EmergencyAlert& alert) override {
        if (!cb.on_emergency_alert) return;
        cimsue_emergency_alert_t o{}; fill(o, alert);
        cb.on_emergency_alert(cb.user, &o);
    }

private:
    using CallCb = void(CIMSUE_CALL*)(void*, const cimsue_call_info_t*);
    void call(CallCb fn, const CallInfo& info) {
        if (!fn) return;
        cimsue_call_info_t o{}; std::vector<cimsue_media_source_t> src; std::vector<const char*> ack; fill(o, info, src, ack);
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
    HttpResult                 http;
    cimsue_http_result_t       httpC{};
    TlsPeerExpiry              tlsPeer;
    cimsue_tls_peer_expiry_t   tlsPeerC{};
    FdUpload                   fd;
    cimsue_fd_upload_t         fdC{};
    UserProfileHolder          userProfile;
    ServiceConfigHolder        serviceConfig;
    UeInitConfigHolder         ueInitConfig;
    McVideoUserProfileHolder   mcvideoUserProfile;
    McVideoServiceConfigHolder mcvideoServiceConfig;
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

void fillHttp(cimsue_csc_t* c) {
    c->httpC.status = c->http.status;
    c->httpC.content_type = C(c->http.contentType);
    c->httpC.etag = C(c->http.etag);
    c->httpC.body = reinterpret_cast<const uint8_t*>(c->http.body.data());
    c->httpC.body_len = (int32_t)c->http.body.size();
}

FdFile fdFileOf(const cimsue_fd_file_t* f) {
    FdFile o;
    if (f) { o.url = S(f->url); o.name = S(f->name); o.type = S(f->type); o.size = f->size; }
    return o;
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
    cfg->udp_no_tcp_switch = B(d.udpNoTcpSwitch);
    cfg->grant_mic_delay_ms = d.grantMicDelayMs;
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
    cfg->max_sds_cplane_bytes = d.maxSdsCplaneBytes;
    cfg->mcdata_msrp = B(d.mcdataMsrp);
    cfg->mcvideo_enabled = B(d.mcvideoEnabled);
    cfg->auto_answer_mcvideo = B(d.autoAnswerMcvideo);
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
cimsue_status_t CIMSUE_CALL cimsue_engine_handle_network_change(cimsue_engine_t* e) {
    return e ? ret(e->eng.handleNetworkChange()) : -1;
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
    fill(g_s.callC, g_s.call, g_s.callSrc, g_s.callAck);
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

void CIMSUE_CALL cimsue_engine_call_quality(const cimsue_engine_t* e, int32_t call_id, cimsue_call_quality_t* out) {
    if (!out) return;
    g_s.quality = e ? e->eng.callQuality(call_id) : CallQuality();
    fill(*out, g_s.quality);
}

void CIMSUE_CALL cimsue_engine_tls_peer_expiry(const cimsue_engine_t* e, cimsue_tls_peer_expiry_t* out) {
    if (!out) return;
    g_s.tlsPeer = e ? e->eng.tlsPeerExpiry() : TlsPeerExpiry();
    fill(g_s.tlsPeerC, g_s.tlsPeer);
    *out = g_s.tlsPeerC;
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
    opts->broadcast = B(d.broadcast);
    opts->implicit_floor_request = B(d.implicitFloorRequest);
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

cimsue_status_t CIMSUE_CALL cimsue_engine_set_call_condition(cimsue_engine_t* e, int32_t call_id, int32_t emergency,
                                                             int32_t imminent_peril) {
    return e ? ret(e->eng.setCallCondition(call_id, emergency != 0, imminent_peril != 0)) : -1;
}

int64_t CIMSUE_CALL cimsue_engine_send_emergency_alert(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                       int32_t activate, const char* originated_by,
                                                       int32_t cancel_group_emergency) {
    if (!e) { g_lastError = "no engine"; return -1; }
    int64_t t = e->eng.sendEmergencyAlert(account_id, S(group_id), activate != 0, S(originated_by), cancel_group_emergency != 0);
    if (t < 0) g_lastError = "sendEmergencyAlert failed";
    return t;
}

int64_t CIMSUE_CALL cimsue_engine_affiliate(cimsue_engine_t* e, int32_t account_id, const char* group_id, int32_t on) {
    return cimsue_engine_affiliate_service(e, account_id, group_id, on, CIMSUE_MC_SERVICE_MCPTT);
}
int64_t CIMSUE_CALL cimsue_engine_affiliate_service(cimsue_engine_t* e, int32_t account_id, const char* group_id, int32_t on,
                                                    cimsue_mc_service_t service) {
    if (!e) { g_lastError = "no engine"; return -1; }
    int64_t t = e->eng.affiliate(account_id, S(group_id), on != 0, (McService)service);
    if (t < 0) g_lastError = "affiliate failed";
    return t;
}

// MCVideo

void CIMSUE_CALL cimsue_video_group_call_options_default(cimsue_video_group_call_options_t* opts) {
    if (!opts) return;
    const VideoGroupCallOptions d;
    *opts = cimsue_video_group_call_options_t{};
    opts->prearranged = B(d.prearranged);
    opts->queueing = B(d.queueing);
    opts->max_priority = d.maxPriority;
    opts->max_reception_priority = d.maxReceptionPriority;
    opts->implicit_transmission_request = B(d.implicitTransmissionRequest);
}
int32_t CIMSUE_CALL cimsue_engine_join_video_group_call(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                        const cimsue_video_group_call_options_t* opts) {
    if (!e) return retId(-1, "no engine");
    return retId(e->eng.joinVideoGroupCall(account_id, S(group_id), toCxx(opts)), "joinVideoGroupCall failed");
}
cimsue_status_t CIMSUE_CALL cimsue_engine_request_transmission(cimsue_engine_t* e, int32_t call_id, int32_t priority) {
    return e ? ret(e->eng.requestTransmission(call_id, priority)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_release_transmission(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.releaseTransmission(call_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_accept_reception(cimsue_engine_t* e, int32_t call_id, const char* transmitter_id,
                                                           int32_t priority) {
    return e ? ret(e->eng.acceptReception(call_id, S(transmitter_id), priority)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_end_reception(cimsue_engine_t* e, int32_t call_id, const char* transmitter_id) {
    return e ? ret(e->eng.endReception(call_id, S(transmitter_id))) : -1;
}
void CIMSUE_CALL cimsue_engine_transmission_info(const cimsue_engine_t* e, int32_t call_id, cimsue_transmission_info_t* out) {
    if (!out) return;
    g_s.tx = e ? e->eng.transmissionInfo(call_id) : TransmissionInfo();
    fill(g_s.txC, g_s.tx, g_s.txTransmitters);
    *out = g_s.txC;
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
                                                         const char* text, int32_t request_delivery, const char* msg_id,
                                                         char* msg_id_out, int32_t msg_id_cap, int64_t* token_out) {
    if (!e) { g_lastError = "no engine"; return -1; }
    SdsSend r = e->eng.sendGroupSds(account_id, S(group_id), S(text), request_delivery != 0, S(msg_id));
    if (token_out) *token_out = r.token;
    if (!r.ok) { g_lastError = r.reason.empty() ? "sendGroupSds failed" : r.reason; return -1; }
    copyOut(r.msgId, msg_id_out, msg_id_cap);
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_engine_send_sds(cimsue_engine_t* e, int32_t account_id, const char* peer,
                                                    const char* text, int32_t request_delivery, const char* msg_id,
                                                    char* msg_id_out, int32_t msg_id_cap, int64_t* token_out) {
    if (!e) { g_lastError = "no engine"; return -1; }
    SdsSend r = e->eng.sendSds(account_id, S(peer), S(text), request_delivery != 0, S(msg_id));
    if (token_out) *token_out = r.token;
    if (!r.ok) { g_lastError = r.reason.empty() ? "sendSds failed" : r.reason; return -1; }
    copyOut(r.msgId, msg_id_out, msg_id_cap);
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_engine_send_group_fd(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                        const cimsue_fd_file_t* file, char* msg_id_out,
                                                        int32_t msg_id_cap, int64_t* token_out) {
    if (!e) { g_lastError = "no engine"; return -1; }
    SdsSend r = e->eng.sendGroupFd(account_id, S(group_id), fdFileOf(file));
    if (token_out) *token_out = r.token;
    if (!r.ok) { g_lastError = r.reason.empty() ? "sendGroupFd failed" : r.reason; return -1; }
    copyOut(r.msgId, msg_id_out, msg_id_cap);
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_engine_send_fd(cimsue_engine_t* e, int32_t account_id, const char* peer,
                                                  const cimsue_fd_file_t* file, char* msg_id_out,
                                                  int32_t msg_id_cap, int64_t* token_out) {
    if (!e) { g_lastError = "no engine"; return -1; }
    SdsSend r = e->eng.sendFd(account_id, S(peer), fdFileOf(file));
    if (token_out) *token_out = r.token;
    if (!r.ok) { g_lastError = r.reason.empty() ? "sendFd failed" : r.reason; return -1; }
    copyOut(r.msgId, msg_id_out, msg_id_cap);
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_engine_send_sds_notification(cimsue_engine_t* e, int32_t account_id,
                                                                const char* peer, const char* conv_id,
                                                                const char* msg_id, const char* group_id, int32_t notif_type,
                                                                int64_t* token_out) {
    if (!e) return -1;
    SdsSend r = e->eng.sendSdsNotification(account_id, S(peer), S(conv_id), S(msg_id), notif_type, S(group_id));
    if (token_out) *token_out = r.token;
    return ret(Result{r.ok, r.code, r.reason});
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
cimsue_status_t CIMSUE_CALL cimsue_engine_set_capture_enabled(cimsue_engine_t* e, int32_t on) {
    return e ? ret(e->eng.setCaptureEnabled(on != 0)) : -1;
}
int32_t CIMSUE_CALL cimsue_engine_capture_enabled(const cimsue_engine_t* e) { return e ? B(e->eng.captureEnabled()) : 0; }
cimsue_status_t CIMSUE_CALL cimsue_engine_set_device_audio_levels(cimsue_engine_t* e, float speaker, double mic_target_dbov) {
    return e ? ret(e->eng.setDeviceAudioLevels(speaker, mic_target_dbov)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_audio_route(cimsue_engine_t* e, cimsue_audio_route_t output,
                                                          cimsue_audio_route_t input) {
    return e ? ret(e->eng.setAudioRoute((AudioRoute)output, (AudioRoute)input)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_reopen_audio_device(cimsue_engine_t* e) {
    return e ? ret(e->eng.reopenAudioDevice()) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_window(cimsue_engine_t* e, void* native_window) {
    return e ? ret(e->eng.setVideoWindow(native_window)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_preview(cimsue_engine_t* e, int32_t on) {
    return e ? ret(e->eng.setVideoPreview(on != 0)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_capture_device(cimsue_engine_t* e, int32_t device_id) {
    return e ? ret(e->eng.setVideoCaptureDevice(device_id)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_send(cimsue_engine_t* e, int32_t call_id, int32_t on) {
    return e ? ret(e->eng.setVideoSend(call_id, on != 0)) : -1;
}
cimsue_status_t CIMSUE_CALL cimsue_engine_switch_camera(cimsue_engine_t* e, int32_t call_id) {
    return e ? ret(e->eng.switchCamera(call_id)) : -1;
}
int32_t CIMSUE_CALL cimsue_engine_video_devices(const cimsue_engine_t* e, const cimsue_video_device_info_t** out) {
    g_s.vdevs = e ? e->eng.videoDevices() : std::vector<VideoDeviceInfo>();
    g_s.vdevsC.clear();
    for (const auto& d : g_s.vdevs) g_s.vdevsC.push_back({d.id, C(d.name), C(d.driver), B(d.capture), B(d.render)});
    if (out) *out = g_s.vdevsC.empty() ? nullptr : g_s.vdevsC.data();
    return (int32_t)g_s.vdevsC.size();
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
const char* CIMSUE_CALL cimsue_condition_cause_str(cimsue_condition_cause_t c) { return toString((ConditionCause)c); }
const char* CIMSUE_CALL cimsue_mc_service_str(cimsue_mc_service_t s) { return toString((McService)s); }
const char* CIMSUE_CALL cimsue_transmission_state_str(cimsue_transmission_state_t s) { return toString((TransmissionState)s); }
const char* CIMSUE_CALL cimsue_reception_state_str(cimsue_reception_state_t s) { return toString((ReceptionState)s); }
const char* CIMSUE_CALL cimsue_transmission_kind_str(cimsue_transmission_kind_t k) { return toString((TransmissionEvent::Kind)k); }
const char* CIMSUE_CALL cimsue_reception_kind_str(cimsue_reception_kind_t k) { return toString((ReceptionEvent::Kind)k); }

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
int32_t CIMSUE_CALL cimsue_user_agent_of(const char* product, const char* version, const char* os, const char* model,
                                         char* out, int32_t cap) {
    return copyOut(userAgentOf(S(product), S(version), S(os), S(model)), out, cap);
}
int32_t CIMSUE_CALL cimsue_imei_urn(const char* imei, char* out, int32_t cap) {
    return copyOut(imeiUrn(S(imei)), out, cap);
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

cimsue_status_t CIMSUE_CALL cimsue_csc_request(cimsue_csc_t* c, const char* access_token, const char* method,
                                               const char* path, const char* content_type, const uint8_t* body,
                                               int32_t body_len, const char* accept, const char* if_match,
                                               const char* if_none_match, cimsue_http_result_t* out) {
    if (!c) return -1;
    c->http = HttpResult();
    std::string b = (body && body_len > 0) ? std::string(reinterpret_cast<const char*>(body), (size_t)body_len) : std::string();
    cimsue_status_t st = ret(c->cli->request(S(access_token), S(method), S(path), S(content_type), b, S(accept),
                                             S(if_match), S(if_none_match), c->http));
    fillHttp(c);
    if (out) *out = c->httpC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_upload_fd(cimsue_csc_t* c, const char* access_token, const uint8_t* data,
                                                 int32_t data_len, const char* name, const char* mime,
                                                 const char* group_id, cimsue_fd_upload_t* out) {
    if (!c) return -1;
    c->fd = FdUpload();
    std::string d = (data && data_len > 0) ? std::string(reinterpret_cast<const char*>(data), (size_t)data_len) : std::string();
    cimsue_status_t st = ret(c->cli->uploadFd(S(access_token), d, S(name), S(mime), S(group_id), c->fd));
    c->fdC.id = C(c->fd.id); c->fdC.url = C(c->fd.url); c->fdC.name = C(c->fd.name); c->fdC.size = c->fd.size;
    if (out) *out = c->fdC;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_download_fd(cimsue_csc_t* c, const char* access_token, const char* url,
                                                   cimsue_http_result_t* out) {
    if (!c) return -1;
    c->http = HttpResult();
    cimsue_status_t st = ret(c->cli->downloadFd(S(access_token), S(url), c->http));
    fillHttp(c);
    if (out) *out = c->httpC;
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

cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_user_profile(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                          const char* etag, cimsue_user_profile_doc_t* out) {
    if (!c) return -1;
    c->userProfile.cxx = UserProfileDoc();
    cimsue_status_t st = ret(c->cli->fetchUserProfile(S(access_token), S(user_uri), S(etag), c->userProfile.cxx));
    c->userProfile.build();
    if (out) *out = c->userProfile.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_service_config(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                            const char* etag, cimsue_service_config_doc_t* out) {
    if (!c) return -1;
    c->serviceConfig.cxx = ServiceConfigDoc();
    cimsue_status_t st = ret(c->cli->fetchServiceConfig(S(access_token), S(user_uri), S(etag), c->serviceConfig.cxx));
    c->serviceConfig.build();
    if (out) *out = c->serviceConfig.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_ue_init_config(cimsue_csc_t* c, const char* mcs_ue_id, const char* etag,
                                                            cimsue_ue_init_config_doc_t* out) {
    if (!c) return -1;
    c->ueInitConfig.cxx = UeInitConfigDoc();
    cimsue_status_t st = ret(c->cli->fetchUeInitConfig(S(mcs_ue_id), S(etag), c->ueInitConfig.cxx));
    c->ueInitConfig.build();
    if (out) *out = c->ueInitConfig.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_user_profile_parse(const char* xml, cimsue_user_profile_doc_t* out) {
    g_s.userProfile.cxx = UserProfileDoc();
    std::string err;
    bool ok = UserProfileDoc::parse(S(xml), g_s.userProfile.cxx, &err);
    g_s.userProfile.build();
    if (out) *out = g_s.userProfile.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_service_config_parse(const char* xml, cimsue_service_config_doc_t* out) {
    g_s.serviceConfig.cxx = ServiceConfigDoc();
    std::string err;
    bool ok = ServiceConfigDoc::parse(S(xml), g_s.serviceConfig.cxx, &err);
    g_s.serviceConfig.build();
    if (out) *out = g_s.serviceConfig.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_ue_init_config_parse(const char* xml, cimsue_ue_init_config_doc_t* out) {
    g_s.ueInitConfig.cxx = UeInitConfigDoc();
    std::string err;
    bool ok = UeInitConfigDoc::parse(S(xml), g_s.ueInitConfig.cxx, &err);
    g_s.ueInitConfig.build();
    if (out) *out = g_s.ueInitConfig.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_mcvideo_user_profile(cimsue_csc_t* c, const char* access_token, const char* mcvideo_id,
                                                                  const char* etag, cimsue_mcvideo_user_profile_doc_t* out) {
    if (!c) return -1;
    c->mcvideoUserProfile.cxx = McVideoUserProfileDoc();
    cimsue_status_t st = ret(c->cli->fetchMcVideoUserProfile(S(access_token), S(mcvideo_id), S(etag), c->mcvideoUserProfile.cxx));
    c->mcvideoUserProfile.build();
    if (out) *out = c->mcvideoUserProfile.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_mcvideo_service_config(cimsue_csc_t* c, const char* access_token, const char* etag,
                                                                    cimsue_mcvideo_service_config_doc_t* out) {
    if (!c) return -1;
    c->mcvideoServiceConfig.cxx = McVideoServiceConfigDoc();
    cimsue_status_t st = ret(c->cli->fetchMcVideoServiceConfig(S(access_token), S(etag), c->mcvideoServiceConfig.cxx));
    c->mcvideoServiceConfig.build();
    if (out) *out = c->mcvideoServiceConfig.out;
    return st;
}

cimsue_status_t CIMSUE_CALL cimsue_mcvideo_user_profile_parse(const char* xml, cimsue_mcvideo_user_profile_doc_t* out) {
    g_s.mcvideoUserProfile.cxx = McVideoUserProfileDoc();
    std::string err;
    bool ok = McVideoUserProfileDoc::parse(S(xml), g_s.mcvideoUserProfile.cxx, &err);
    g_s.mcvideoUserProfile.build();
    if (out) *out = g_s.mcvideoUserProfile.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

cimsue_status_t CIMSUE_CALL cimsue_mcvideo_service_config_parse(const char* xml, cimsue_mcvideo_service_config_doc_t* out) {
    g_s.mcvideoServiceConfig.cxx = McVideoServiceConfigDoc();
    std::string err;
    bool ok = McVideoServiceConfigDoc::parse(S(xml), g_s.mcvideoServiceConfig.cxx, &err);
    g_s.mcvideoServiceConfig.build();
    if (out) *out = g_s.mcvideoServiceConfig.out;
    if (!ok) { g_lastError = err; return -1; }
    return CIMSUE_OK;
}

void CIMSUE_CALL cimsue_mcvideo_group_attrs_default(cimsue_mcvideo_group_attrs_t* out) {
    if (out) fillDefault(*out);
}

void CIMSUE_CALL cimsue_capabilities_of(const cimsue_user_profile_doc_t* user_profile,
                                        const cimsue_service_config_doc_t* service_config, cimsue_capabilities_t* out) {
    if (!out) return;
    UserProfileDoc up = toCxx(user_profile);
    ServiceConfigDoc sc = toCxx(service_config);
    Capabilities k = Capabilities::of(user_profile ? &up : nullptr, service_config ? &sc : nullptr);
    *out = cimsue_capabilities_t{};
    out->user_profile_known = B(k.userProfileKnown); out->service_config_known = B(k.serviceConfigKnown);
    out->private_call = B(k.privateCall); out->emergency_group_call = B(k.emergencyGroupCall);
    out->imminent_peril_call = B(k.imminentPerilCall); out->emergency_private_call = B(k.emergencyPrivateCall);
    out->emergency_alert = B(k.emergencyAlert); out->cancel_emergency_alert = B(k.cancelEmergencyAlert);
    out->adhoc_group_call = B(k.adhocGroupCall); out->max_affiliations_n2 = k.maxAffiliationsN2;
    out->cancel_group_emergency = B(k.cancelGroupEmergency); out->cancel_imminent_peril = B(k.cancelImminentPeril);
}

void CIMSUE_CALL cimsue_csc_tls_peer_expiry(cimsue_csc_t* c, cimsue_tls_peer_expiry_t* out) {
    if (!out) return;
    if (!c) { *out = cimsue_tls_peer_expiry_t{}; return; }
    c->tlsPeer = c->cli->tlsPeerExpiry();
    fill(c->tlsPeerC, c->tlsPeer);
    *out = c->tlsPeerC;
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

const cimsue_service_profile_t* CIMSUE_CALL cimsue_profile_phone_service(const cimsue_profile_t* profile) {
    if (const cimsue_service_profile_t* s = cimsue_profile_service(profile, "voip")) return s;
    return cimsue_profile_service(profile, "volte");
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
    case CIMSUE_STRUCT_HTTP_RESULT:       return (int32_t)sizeof(cimsue_http_result_t);
    case CIMSUE_STRUCT_TLS_PEER_EXPIRY:   return (int32_t)sizeof(cimsue_tls_peer_expiry_t);
    case CIMSUE_STRUCT_DISPATCH_MEMBER:   return (int32_t)sizeof(cimsue_dispatch_member_t);
    case CIMSUE_STRUCT_DISPATCH_TARGET:   return (int32_t)sizeof(cimsue_dispatch_target_t);
    case CIMSUE_STRUCT_GROUP_MEMBER:      return (int32_t)sizeof(cimsue_group_member_t);
    case CIMSUE_STRUCT_GROUP_DOC:         return (int32_t)sizeof(cimsue_group_doc_t);
    case CIMSUE_STRUCT_FD_FILE:           return (int32_t)sizeof(cimsue_fd_file_t);
    case CIMSUE_STRUCT_FD_UPLOAD:         return (int32_t)sizeof(cimsue_fd_upload_t);
    case CIMSUE_STRUCT_QUALITY_DIRECTION: return (int32_t)sizeof(cimsue_quality_direction_t);
    case CIMSUE_STRUCT_CALL_QUALITY:      return (int32_t)sizeof(cimsue_call_quality_t);
    case CIMSUE_STRUCT_MCPTT_CONDITION:   return (int32_t)sizeof(cimsue_mcptt_condition_t);
    case CIMSUE_STRUCT_EMERGENCY_ALERT:   return (int32_t)sizeof(cimsue_emergency_alert_t);
    case CIMSUE_STRUCT_VIDEO_DEVICE_INFO: return (int32_t)sizeof(cimsue_video_device_info_t);
    case CIMSUE_STRUCT_CMS_ENTRY:         return (int32_t)sizeof(cimsue_cms_entry_t);
    case CIMSUE_STRUCT_USER_PROFILE_DOC:  return (int32_t)sizeof(cimsue_user_profile_doc_t);
    case CIMSUE_STRUCT_SERVICE_CONFIG_DOC: return (int32_t)sizeof(cimsue_service_config_doc_t);
    case CIMSUE_STRUCT_CAPABILITIES:      return (int32_t)sizeof(cimsue_capabilities_t);
    case CIMSUE_STRUCT_UE_INIT_CONFIG_DOC: return (int32_t)sizeof(cimsue_ue_init_config_doc_t);
    case CIMSUE_STRUCT_VIDEO_GROUP_CALL_OPTIONS: return (int32_t)sizeof(cimsue_video_group_call_options_t);
    case CIMSUE_STRUCT_VIDEO_TRANSMITTER:  return (int32_t)sizeof(cimsue_video_transmitter_t);
    case CIMSUE_STRUCT_TRANSMISSION_EVENT: return (int32_t)sizeof(cimsue_transmission_event_t);
    case CIMSUE_STRUCT_RECEPTION_EVENT:    return (int32_t)sizeof(cimsue_reception_event_t);
    case CIMSUE_STRUCT_TRANSMISSION_INFO:  return (int32_t)sizeof(cimsue_transmission_info_t);
    case CIMSUE_STRUCT_MCVIDEO_GROUP_ATTRS: return (int32_t)sizeof(cimsue_mcvideo_group_attrs_t);
    case CIMSUE_STRUCT_MCVIDEO_USER_PROFILE_DOC: return (int32_t)sizeof(cimsue_mcvideo_user_profile_doc_t);
    case CIMSUE_STRUCT_MCVIDEO_SERVICE_CONFIG_DOC: return (int32_t)sizeof(cimsue_mcvideo_service_config_doc_t);
    case CIMSUE_STRUCT_VIDEO_FRAME: return (int32_t)sizeof(cimsue_video_frame_t);
    default:                              return -1;
    }
}

}  // extern "C"
