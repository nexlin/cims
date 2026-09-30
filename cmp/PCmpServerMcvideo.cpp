// PCmpServer — MCVideo 그룹 호 명령 (cmp_media_api.md §7.9, TS 24.581).
//   명령은 MCPTT 와 같은 PTT_* 이고 hdr.service:"mcvideo" 가 이 경로로 보낸다(handlePacket). 그룹 자원 키 = (service, group_id) —
//   _mcvGroups 는 MCPTT _groups 와 따로 서고, 세션 상관 캐시(_sesidMap·_serviceMap·_groupSubId)는 McvKey(gid) 로 둔다.
#include "PCmpServer.h"
#include "PLog.h"
#include "SimpleJson.h"
#include <sstream>

namespace {

// 정수 필드 — 있으면 [lo, hi] 검사 후 out 에, 없으면 out 유지. 범위 밖이면 err.
bool McvIntField(const SimpleJson::JsonNode& o, const char* key, int lo, int hi, int& out, std::string& err) {
    if (!o.Has(key)) return true;
    long long v = o.GetInt(key, 0);
    if (v < lo || v > hi) {
        err = std::string(key) + " out of range (" + std::to_string(lo) + ".." + std::to_string(hi) + ")";
        return false;
    }
    out = (int)v;
    return true;
}

// tc_timers (TS 24.581 §11 — 미지정 = 현재 값). T1·T5 = 그룹 hang timer 라 0(미사용)을 받는다.
bool McvParseTimers(const SimpleJson::JsonNode& tt, McvTimers& t, std::string& err) {
    if (tt.type != SimpleJson::JSON_OBJECT) return true;
    return McvIntField(tt, "t1_ms", 0, 3600000, t.t1Ms, err) && McvIntField(tt, "t2_ms", 100, 60000, t.t2Ms, err) &&
           McvIntField(tt, "t3_ms", 100, 60000, t.t3Ms, err) && McvIntField(tt, "t4_ms", 100, 60000, t.t4Ms, err) &&
           McvIntField(tt, "t5_ms", 0, 3600000, t.t5Ms, err) && McvIntField(tt, "t6_ms", 100, 60000, t.t6Ms, err) &&
           McvIntField(tt, "t11_ms", 100, 600000, t.t11Ms, err) && McvIntField(tt, "c2", 1, 255, t.c2, err) &&
           McvIntField(tt, "c4", 1, 255, t.c4, err) && McvIntField(tt, "c6", 1, 255, t.c6, err) &&
           McvIntField(tt, "c7", 1, 255, t.c7, err) && McvIntField(tt, "c11", 1, 255, t.c11, err);
}

// tc_crypto (§7.8 floor_crypto 와 같은 형식 — key/salt = base64, mki = hex, TS 33.180). 없으면 have=false. 그룹·유닛을 잡기 전에
//   알고리즘·길이까지 검사한다(fail-fast — 평문으로 조용히 떨어뜨리지 않는다).
struct McvTcCrypto {
    bool have = false;
    std::string alg, key, salt, mki;
};
bool McvParseTcCrypto(const SimpleJson::JsonNode& payload, McvTcCrypto& out, std::string& err) {
    SimpleJson::JsonNode c = payload.Get("tc_crypto");
    if (c.type != SimpleJson::JSON_OBJECT) return true;
    out.have = true;
    out.alg = c.GetString("alg");
    if (!out.alg.empty() && !PMediaCrypto::IsSupportedAlg(out.alg))
        err = "tc_crypto.alg must be AES_CM_128_HMAC_SHA1_80|_32";
    else if (!PFloorCrypto::DecodeBase64(c.GetString("key"), out.key))
        err = "tc_crypto.key must be base64";
    else if (!PFloorCrypto::DecodeBase64(c.GetString("salt"), out.salt))
        err = "tc_crypto.salt must be base64";
    else if (!PFloorCrypto::DecodeHex(c.GetString("mki"), out.mki))
        err = "tc_crypto.mki must be hex";
    else if (out.key.size() != 16)
        err = "tc_crypto.key must decode to 16 bytes (AES-128)";
    else if (out.salt.size() != 14)
        err = "tc_crypto.salt must decode to 14 bytes";
    else if (out.mki.size() > (size_t)PFloorCrypto::kMaxMki)
        err = "tc_crypto.mki too long";
    return err.empty();
}

}  // namespace

// PTT_GROUP_ADD / PTT_GROUP_MODIFY (service:"mcvideo") — cmp_media_api.md §7.9.
void PCmpServer::processMcvAddGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string cmdName = payload.GetString("cmd");
    if (cmdName.empty()) cmdName = "PTT_GROUP_ADD";
    const std::string key = McvKey(groupId);
    const std::string svc = "mcvideo";

    PAutoLock lock(_mutex);
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) sesid = issueSesid("");
    std::string subid = payload.GetString("subid");
    std::string txIdStr = std::to_string(transId);
    logFlow(key, "csp", "cmp", "JSON", cmdName.c_str(), groupId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(),
            subid.c_str(), _lastRxSeq, "csp");

    auto reject = [&](const char* code, const std::string& why) {
        int txSeq = sendErr(ip, port, transId, cmdName, sesid, svc, code, why.c_str());
        logFlow(key, "cmp", "csp", "JSON", "ERROR", why.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "",
                txSeq, "csp");
        LOG_WARN("PCmpServer", "%s(mcvideo) group=%s rejected: %s %s", cmdName.c_str(), groupId.c_str(), code,
                 why.c_str());
    };

    if (groupId.empty()) return reject("BAD_REQUEST", "group_id required");
    if (_mcvMemberPool.empty()) return reject("NO_RESOURCE", "mcvideo not enabled (McVideoMemberPoolSize=0)");

    // MCVideo 에 floor 는 없다 — 송출·수신 제어는 그룹 종류가 가진다(§7.9). 조용히 무시하지 않고 거절(계약 위반 노출).
    static const char* kFloorFields[] = { "floor_control", "floor_policy", "max_talkers", "floor_timers",
                                          "floor_crypto",  "broadcast",    "initiator_id" };
    for (const char* f : kFloorFields)
        if (payload.Has(f)) return reject("BAD_REQUEST", std::string(f) + " not allowed for mcvideo (no floor)");
    McvTcCrypto tc;
    std::string tcErr;
    if (!McvParseTcCrypto(payload, tc, tcErr)) return reject("BAD_REQUEST", tcErr);

    auto it = _mcvGroups.find(groupId);
    std::shared_ptr<PMcvideoGroup> group = (it != _mcvGroups.end()) ? it->second : nullptr;
    // MODIFY 는 기존 그룹 전제 (§7.2) — 소실 그룹을 재생성하지 않는다(client 는 NOT_FOUND 에 ADD 로 재수립).
    if (!group && cmdName == "PTT_GROUP_MODIFY") return reject("NOT_FOUND", "group not found");

    // 그룹 종류·송출 상한·수신 모드·타이머 — 기존 그룹이면 싣지 않은 필드는 현재 값 유지.
    bool prearranged = group ? group->prearranged() : false;
    int maxTx = group ? group->maxTransmitters() : 0;
    bool autoRx = group ? group->receptionAutomatic() : false;
    McvCallType callType = group ? group->callType() : MCV_CALL_NORMAL;
    McvTimers timers = group ? group->timers() : McvTimers();
    std::string gt = payload.GetString("group_type");
    if (gt == "prearranged") prearranged = true;
    else if (gt == "chat") prearranged = false;
    else if (!gt.empty()) return reject("BAD_REQUEST", "group_type must be chat|prearranged");
    std::string rm = payload.GetString("reception_mode");
    if (rm == "automatic") autoRx = true;
    else if (rm == "manual") autoRx = false;
    else if (!rm.empty()) return reject("BAD_REQUEST", "reception_mode must be manual|automatic");
    // 호 종류 — 긴급·임박이면 전송 제어 메시지에 Transmission Indicator 를 싣고 수신은 automatic(TS 24.581 §6.3.6.3.3 1a)
    std::string ct = payload.GetString("call_type");
    if (ct == "normal") callType = MCV_CALL_NORMAL;
    else if (ct == "emergency") callType = MCV_CALL_EMERGENCY;
    else if (ct == "imminent") callType = MCV_CALL_IMMINENT;
    else if (!ct.empty()) return reject("BAD_REQUEST", "call_type must be normal|emergency|imminent");
    std::string err;
    if (!McvIntField(payload, "max_transmitters", 1, 16, maxTx, err)) return reject("BAD_REQUEST", err);
    if (maxTx <= 0) return reject("BAD_REQUEST", "max_transmitters (1..16) required");
    if (!McvParseTimers(payload.Get("tc_timers"), timers, err)) return reject("BAD_REQUEST", "tc_timers." + err);

    // members — "id:prio[:role]" 나열 (§7.1 과 같은 형식. prio = 그룹 문서 <user-priority> — 송출 우선순위 상한, §14.3.3)
    std::vector<std::string> memberIds;
    std::map<std::string, int> priorities;
    std::map<std::string, std::string> roles;
    {
        std::stringstream ss(payload.GetString("members"));
        std::string seg;
        while (std::getline(ss, seg, ',')) {
            size_t c1 = seg.find(':');
            if (c1 == std::string::npos || c1 == 0) continue;
            std::string sid = seg.substr(0, c1);
            size_t c2 = seg.find(':', c1 + 1);
            int prio = 0;
            try {
                prio = std::stoi(seg.substr(c1 + 1));
            } catch (...) {
            }
            std::string role = "participant";
            if (c2 != std::string::npos) {
                size_t c3 = seg.find(':', c2 + 1);
                role = seg.substr(c2 + 1, c3 == std::string::npos ? std::string::npos : c3 - (c2 + 1));
            }
            priorities[sid] = prio;
            roles[sid] = role;
            memberIds.push_back(sid);
        }
    }

    bool createdNow = false;
    if (!group) {
        group = std::make_shared<PMcvideoGroup>(groupId);
        bool logTc = _logFlowFloor;   // 전송 제어 메시지 flow 기록 = floor 와 같은 스위치(Logging.Flow.floor)
        std::string gkey = key;
        group->setLogCallback([this, logTc, gkey](const char* from, const char* to, const char* proto,
                                                  const char* label, const char* body, const std::string& gsesid,
                                                  const std::string& gsvc, const std::string& gsubid) {
            if (!logTc) return;
            logFlow(gkey, from, to, proto, label, body, "", gsvc.c_str(), gsesid.c_str(), gsubid.c_str());
        });
        // 송출자 집합 변경 → TRANSMITTERS, T1/T5 만료 → TRANSMISSION_INACTIVITY (cmp_media_api.md §8 — 해제는 CSP 정책)
        group->setTransmittersCallback([this](const std::string& gid, const std::vector<McvTransmitter>& tx,
                                              const std::string& gsesid, const std::string& gsvc) {
            SimpleJson::JsonNode arr;
            arr.type = SimpleJson::JSON_ARRAY;
            for (const auto& t : tx) {
                SimpleJson::JsonNode e;
                e.Set("user", t.memberId);
                e.Set("audio_ssrc", (long long)t.audioSsrc);
                e.Set("video_ssrc", (long long)t.videoSsrc);
                arr.Add(e);
            }
            SimpleJson::JsonNode p;
            p.Set("group_id", gid);
            p.Set("transmitters", arr);
            emitEvent("TRANSMITTERS", p, gsesid, gsvc.empty() ? "mcvideo" : gsvc);
        });
        group->setInactivityCallback([this](const std::string& gid, const char* timer, const std::string& gsesid,
                                            const std::string& gsvc) {
            SimpleJson::JsonNode p;
            p.Set("group_id", gid);
            p.Set("timer", timer ? timer : "");
            emitEvent("TRANSMISSION_INACTIVITY", p, gsesid, gsvc.empty() ? "mcvideo" : gsvc);
        });
        _mcvGroups[groupId] = group;
        createdNow = true;
    }
    group->setSessionMeta(sesid, svc, subid);
    group->setConfig(prearranged, maxTx, autoRx, callType, timers);
    // 전송 제어 SRTCP 그룹 키 — 멤버 CSK(JOIN tc_crypto)가 없는 멤버가 쓴다(TS 33.180 §9.4). 멤버를 들이기 전에 건다.
    if (tc.have && !group->setTcCrypto(tc.alg, tc.key, tc.salt, tc.mki, tcErr)) {
        if (createdNow) destroyMcvGroup(groupId);
        return reject("BAD_REQUEST", "tc_crypto: " + tcErr);
    }
    group->updateRoster(priorities, roles);
    std::string recordDir = payload.GetString("record_dir");
    if (!recordDir.empty()) group->setRecording(recordDir, payload.GetString("session_dir"));
    _sesidMap[key] = sesid;
    _serviceMap[key] = svc;
    if (!subid.empty()) _groupSubId[key] = subid;

    // 초기 로스터의 멤버별 전용 포트 (멱등 — 기존 유닛 재사용). client 는 각 멤버의 SDP 에 이 포트를 광고한다.
    SimpleJson::JsonNode memberPorts;
    memberPorts.type = SimpleJson::JSON_OBJECT;
    for (const auto& sid : memberIds) {
        PMcvMemberPort* mu = ensureMcvUnit(groupId, sid, group);
        if (!mu) {
            // 새 그룹이면 즉시 롤백 — 실패 응답 뒤 sweeper 까지 유닛이 점유되지 않게(PTT 경로와 대칭).
            //   기존 그룹의 선할당 유닛은 유지한다(멱등 재시도 때 재사용, LEAVE/그룹 해제로 회수).
            if (createdNow) destroyMcvGroup(groupId);
            return reject("NO_RESOURCE", "mcvideo member pool exhausted");
        }
        SimpleJson::JsonNode mp;
        mp.Set("port", (int)mu->getAudioPort());
        mp.Set("video_port", (int)mu->getVideoPort());
        mp.Set("control_port", (int)mu->getControlPort());
        memberPorts.Set(sid, mp);
    }
    if (createdNow) {
        logFlow(key, "cmp", "cmp", "INT", "GROUP_START", prearranged ? "mcvideo prearranged" : "mcvideo chat", "",
                svc.c_str(), sesid.c_str(), subid.c_str());
        LOG_INFO("PCmpServer", "ADD_GROUP(mcvideo) group=%s type=%s max_transmitters=%d reception=%s call=%s (new)",
                 groupId.c_str(), prearranged ? "prearranged" : "chat", maxTx, autoRx ? "automatic" : "manual",
                 callType == MCV_CALL_EMERGENCY ? "emergency" : callType == MCV_CALL_IMMINENT ? "imminent" : "normal");
    }

    SimpleJson::JsonNode respBody;
    respBody.Set("ip", _rtpIp);
    respBody.Set("member_ports", memberPorts);
    int txSeq = sendOk(ip, port, transId, cmdName, sesid, svc, &respBody);
    logFlow(key, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), subid.c_str(), txSeq,
            "csp");
}

// PTT_JOIN (service:"mcvideo") — 2단 멱등(§7.4): ① user_ip 없는 선할당 = 포트·tc_ssrc 만, ② SDP 교환 뒤 주소·협상 값 등록/갱신.
void PCmpServer::processMcvJoin(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");
    const std::string key = McvKey(groupId);
    const std::string svc = "mcvideo";

    PAutoLock lock(_mutex);
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto its = _sesidMap.find(key);
        if (its != _sesidMap.end()) sesid = its->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    std::string txIdStr = std::to_string(transId);
    logFlow(key, "csp", "cmp", "JSON", "PTT_JOIN", sessionId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "",
            _lastRxSeq, "csp");

    auto reject = [&](const char* code, const std::string& why) {
        int txSeq = sendErr(ip, port, transId, "PTT_JOIN", sesid, svc, code, why.c_str());
        logFlow(key, "cmp", "csp", "JSON", "ERROR", why.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "",
                txSeq, "csp");
        LOG_WARN("PCmpServer", "PTT_JOIN(mcvideo) group=%s session=%s rejected: %s %s", groupId.c_str(),
                 sessionId.c_str(), code, why.c_str());
    };

    if (groupId.empty() || sessionId.empty()) return reject("BAD_REQUEST", "group_id and session_id required");
    // floor 보호 키는 MCVideo 에 없다(전송 제어 = tc_crypto). 보호 키 형식 위반은 유닛을 잡기 전에 거절한다(fail-fast).
    if (payload.Has("floor_crypto")) return reject("BAD_REQUEST", "floor_crypto not allowed for mcvideo (use tc_crypto)");
    MediaCryptoParam mcAudio, mcVideo;
    McvTcCrypto tc;
    {
        std::string err;
        if (!ParseMediaCrypto(payload, "media_crypto", mcAudio, err) ||
            !ParseMediaCrypto(payload, "media_crypto_video", mcVideo, err) || !McvParseTcCrypto(payload, tc, err))
            return reject("BAD_REQUEST", err);
    }

    auto it = _mcvGroups.find(groupId);
    if (it == _mcvGroups.end()) return reject("NOT_FOUND", "group not found");
    std::shared_ptr<PMcvideoGroup> group = it->second;
    group->setSessionMeta(sesid, svc, "");
    _sesidMap[key] = sesid;

    // 선언 해석·검사를 유닛 할당보다 먼저 — 거절된 JOIN 이 유닛을 잡지 않게.
    std::string userIp = payload.GetString("user_ip");
    int userPort = (int)payload.GetInt("user_port", 0);
    bool addressed = !userIp.empty() && userPort > 0;
    bool implicitReq = payload.GetInt("implicit_request", 0) != 0;
    McvMemberDecl d;
    if (addressed) {
        std::string err;
        d.ip = userIp;
        d.port = userPort;
        d.videoPort = (int)payload.GetInt("user_video_port", 0);
        d.controlPort = (int)payload.GetInt("user_control_port", 0);
        d.nat = payload.GetInt("user_nat", 0) != 0;
        d.sigIp = payload.GetString("user_sig_ip");
        d.ptOut = (int)payload.GetInt("user_pt", 0);
        d.srcPt = (int)payload.GetInt("user_src_pt", 0);
        d.videoPtOut = (int)payload.GetInt("user_video_pt", 0);
        d.codec = payload.GetString("user_codec");
        d.role = payload.GetString("role");
        if (d.role.empty()) d.role = "participant";
        d.userUri = payload.GetString("user_uri");
        d.userTcSsrc = (unsigned int)payload.GetInt("user_tc_ssrc", 0);
        d.userAudioSsrc = (unsigned int)payload.GetInt("user_audio_ssrc", 0);
        d.userVideoSsrc = (unsigned int)payload.GetInt("user_video_ssrc", 0);
        d.queueing = payload.GetInt("queueing", 0) != 0;
        d.recvOnly = payload.GetInt("recv_only", 0) != 0;
        if (!McvIntField(payload, "max_priority", 0, 255, d.maxPriority, err) ||
            !McvIntField(payload, "max_reception_priority", 0, 255, d.maxRxPriority, err) ||
            !McvIntField(payload, "max_rx_streams", 1, 16, d.maxRxStreams, err))
            return reject("BAD_REQUEST", err);
    }
    // 암묵적 송출 요청은 새 prearranged 세션 개시에만 온다(TS 24.581 §14.3.5) — chat 합류에 오면 계약 위반.
    if (implicitReq && !group->prearranged())
        return reject("BAD_REQUEST", "implicit_request only for a new prearranged session");
    // 암묵적 요청은 참가 시점의 송출 요청이라(§6.3.5.2.2 1) 주소 등록(JOIN ②)과 함께만 온다.
    if (implicitReq && !addressed) return reject("BAD_REQUEST", "implicit_request requires user_ip/user_port");

    PMcvMemberPort* mu = ensureMcvUnit(groupId, sessionId, group);
    if (!mu) return reject("NO_RESOURCE", "mcvideo member pool exhausted");
    // 보호 키 — 참가 등록(Idle·Notification 송신) 전에 건다. 같은 구성 재선언은 세션 유지, 변경은 재생성(media_security.md §5.2).
    {
        std::string err;
        bool ok = (!tc.have || group->setMemberTcCrypto(sessionId, tc.alg, tc.key, tc.salt, tc.mki, err)) &&
                  (!mcAudio.have || group->setMemberMediaCrypto(sessionId, false, mcAudio.alg, mcAudio.rxKey,
                                                                mcAudio.rxSalt, mcAudio.txKey, mcAudio.txSalt, err)) &&
                  (!mcVideo.have || group->setMemberMediaCrypto(sessionId, true, mcVideo.alg, mcVideo.rxKey,
                                                                mcVideo.rxSalt, mcVideo.txKey, mcVideo.txSalt, err));
        if (!ok) return reject("BAD_REQUEST", err);
    }
    PMcvControl::ImplicitResult ires;
    if (addressed) group->addMember(sessionId, d, implicitReq, &ires);

    SimpleJson::JsonNode respBody;
    respBody.Set("ip", _rtpIp);
    respBody.Set("port", (int)mu->getAudioPort());
    respBody.Set("video_port", (int)mu->getVideoPort());
    respBody.Set("control_port", (int)mu->getControlPort());
    respBody.Set("tc_ssrc", (long long)group->tcSsrcOf(sessionId));
    // 암묵적 송출 요청 — 허가는 송출 제어가 정한다. SSRC 쌍은 허가 여부와 무관하게 예약·응답한다(§14.3.7·§14.3.8 «irrespective of
    //   mc_granted»). 허가 전(개시자 혼자)이면 granted 0 — 첫 초대 참가자가 등록될 때 CMP 가 Transmission Granted 를 보낸다(§6.3.2.2).
    if (implicitReq) {
        respBody.Set("granted", ires.granted ? 1 : 0);
        respBody.Set("audio_ssrc", (long long)ires.audioSsrc);
        respBody.Set("video_ssrc", (long long)ires.videoSsrc);
    }
    int txSeq = sendOk(ip, port, transId, "PTT_JOIN", sesid, svc, &respBody);
    logFlow(key, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
    LOG_INFO("PCmpServer", "PTT_JOIN(mcvideo) group=%s session=%s %s:%d local=%d control=%d tc_ssrc=%08x",
             groupId.c_str(), sessionId.c_str(), userIp.c_str(), userPort, mu->getAudioPort(), mu->getControlPort(),
             group->tcSsrcOf(sessionId));
}

// PTT_LEAVE (service:"mcvideo") — 자연 멱등: 없는 그룹·멤버도 OK.
void PCmpServer::processMcvLeave(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");
    const std::string key = McvKey(groupId);
    const std::string svc = "mcvideo";

    PAutoLock lock(_mutex);
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto its = _sesidMap.find(key);
        if (its != _sesidMap.end()) sesid = its->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    std::string txIdStr = std::to_string(transId);
    logFlow(key, "csp", "cmp", "JSON", "PTT_LEAVE", sessionId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "",
            _lastRxSeq, "csp");

    auto it = _mcvGroups.find(groupId);
    if (it != _mcvGroups.end()) {
        it->second->removeMember(sessionId);
        freeMcvUnit(groupId, sessionId);
        LOG_INFO("PCmpServer", "PTT_LEAVE(mcvideo) group=%s session=%s", groupId.c_str(), sessionId.c_str());
    } else {
        LOG_WARN("PCmpServer", "PTT_LEAVE(mcvideo) group=%s not found (idempotent OK)", groupId.c_str());
    }
    int txSeq = sendOk(ip, port, transId, "PTT_LEAVE", sesid, svc);
    logFlow(key, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
}

// PTT_GROUP_REMOVE (service:"mcvideo") — 자연 멱등.
void PCmpServer::processMcvRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port,
                                       int transId) {
    std::string groupId = payload.GetString("group_id");
    const std::string key = McvKey(groupId);
    const std::string svc = "mcvideo";

    PAutoLock lock(_mutex);
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto its = _sesidMap.find(key);
        if (its != _sesidMap.end()) sesid = its->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    std::string txIdStr = std::to_string(transId);
    logFlow(key, "csp", "cmp", "JSON", "PTT_GROUP_REMOVE", groupId.c_str(), txIdStr.c_str(), svc.c_str(),
            sesid.c_str(), "", _lastRxSeq, "csp");

    if (_mcvGroups.count(groupId)) {
        logFlow(key, "cmp", "cmp", "INT", "GROUP_END", "", "", svc.c_str(), sesid.c_str());
        destroyMcvGroup(groupId);
        LOG_INFO("PCmpServer", "PTT_GROUP_REMOVE(mcvideo) group=%s", groupId.c_str());
    } else {
        LOG_WARN("PCmpServer", "PTT_GROUP_REMOVE(mcvideo) group=%s not found (idempotent OK)", groupId.c_str());
    }
    int txSeq = sendOk(ip, port, transId, "PTT_GROUP_REMOVE", sesid, svc);
    logFlow(key, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
    _sesidMap.erase(key);
    _serviceMap.erase(key);
    _groupSubId.erase(key);
}

// ── 자원 ──────────────────────────────────────────────────────────────

void PCmpServer::initMcvMemberPool() {
    if (_mcvMemberPoolSize <= 0) {
        LOG_INFO("PCmpServer", "MCVideo member pool disabled (McVideoMemberPoolSize=0) — resource.mcvideo not advertised");
        return;
    }
    int basePort = _mcvStartPort;
    for (int i = 0; i < _mcvMemberPoolSize; ++i) {
        PMcvMemberPort* mu = new PMcvMemberPort(formatStr("McvMember_%d", i));
        if (mu->init(_rtpIp, basePort)) {
            int widx = i % _rtpWorkerCount;
            mu->setWorkerName(formatStr("RtpWorker_%d", widx));
            std::vector<int> fds;
            mu->collectFds(fds);
            epollAddHandler(widx, mu, fds);
            _mcvMemberPool.push_back(mu);
            _freeMcvMembers.push_back(mu);
        } else {
            LOG_ERROR("PCmpServer", "Failed to init MCVideo member unit base=%d", basePort);
            delete mu;
        }
        basePort += PMcvMemberPort::kStride;
    }
    LOG_INFO("PCmpServer", "MCVideo member pool: %lu units (%d-%d, %d ports/member)", _mcvMemberPool.size(),
             _mcvStartPort, basePort - 1, PMcvMemberPort::kStride);
}

PMcvMemberPort* PCmpServer::ensureMcvUnit(const std::string& groupId, const std::string& sessionId,
                                          const std::shared_ptr<PMcvideoGroup>& group) {
    std::string unitKey = groupId + "|" + sessionId;
    auto it = _mcvUnits.find(unitKey);
    if (it != _mcvUnits.end()) {
        group->reserveMember(sessionId, it->second);   // 멱등 — 같은 tc_ssrc
        return it->second;
    }
    if (_freeMcvMembers.empty()) {
        LOG_WARN("PCmpServer", "ensureMcvUnit: member pool exhausted (group=%s session=%s)", groupId.c_str(),
                 sessionId.c_str());
        return nullptr;
    }
    PMcvMemberPort* mu = _freeMcvMembers.back();
    _freeMcvMembers.pop_back();
    group->reserveMember(sessionId, mu);
    mu->bind(group, sessionId);
    _mcvUnits[unitKey] = mu;
    LOG_INFO("PCmpServer", "ensureMcvUnit: group=%s session=%s base=%d (remaining %lu)", groupId.c_str(),
             sessionId.c_str(), mu->getAudioPort(), _freeMcvMembers.size());
    return mu;
}

void PCmpServer::freeMcvUnit(const std::string& groupId, const std::string& sessionId) {
    auto it = _mcvUnits.find(groupId + "|" + sessionId);
    if (it == _mcvUnits.end()) return;
    it->second->reset();
    _freeMcvMembers.push_back(it->second);
    _mcvUnits.erase(it);
}

void PCmpServer::freeMcvGroupUnits(const std::string& groupId) {
    std::string prefix = groupId + "|";
    for (auto it = _mcvUnits.begin(); it != _mcvUnits.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it->second->reset();
            _freeMcvMembers.push_back(it->second);
            it = _mcvUnits.erase(it);
        } else {
            ++it;
        }
    }
}

void PCmpServer::destroyMcvGroup(const std::string& groupId) {
    auto it = _mcvGroups.find(groupId);
    if (it == _mcvGroups.end()) return;
    _srcDropTotal += it->second->getSrcDrop();   // 드롭 카운터 이월 (rtp_src_drop 단조 증가)
    it->second->close();                         // 멤버 먼저 비움 — 리액터가 쥔 참조로 늦게 온 패킷은 미등록 멤버로 버려진다
    freeMcvGroupUnits(groupId);
    _mcvGroups.erase(it);
    const std::string key = McvKey(groupId);
    _sesidMap.erase(key);
    _serviceMap.erase(key);
    _groupSubId.erase(key);
}
