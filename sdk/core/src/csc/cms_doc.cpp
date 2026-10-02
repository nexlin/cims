// libcimsue — CMS 문서(TS 24.484) 해석 · 정책 게이트 스냅샷 (csc.h UserProfileDoc·ServiceConfigDoc·Capabilities ·
// MCVideo McVideoUserProfileDoc·McVideoServiceConfigDoc — 서버 산출 = csc/src/services/mcvideo.py, 골든 = tests/fixtures/mcvideo/).
// 원천 = android/ptt-client PttController.parseUserProfile/parseServiceConfig/svcAllows. 서버 산출 = csc/src/services/mcptt.py
// get_user_profile_xml·get_service_config_xml. 스캔 도구 = xml_scan.h(접두사 무관 로컬 이름).
#include <cstdlib>

#include "cimsue/csc.h"
#include "csc/xml_scan.h"

namespace cimsue {

namespace {

using namespace xmlscan;

/** ruleset 불리언 — 요소가 없으면 허용(ue_sdk.md §4.2). */
bool allowFlag(const std::string& s, const char* local) {
    bool f = false;
    std::string v = elemText(s, local, &f);
    return f ? isTrue(v) : true;
}

int intElem(const std::string& s, const char* local) {
    bool f = false;
    std::string v = elemText(s, local, &f);
    return f && !v.empty() ? std::atoi(v.c_str()) : -1;
}

/** EntryType(§8.3.2.7) — <tag entry-info="…"><uri-entry>…</uri-entry></tag>. */
CmsEntry entryOf(const std::string& s, const char* local) {
    CmsEntry e;
    Elem x = elem(s, local);
    if (!x.found) return e;
    e.mode = attrOf(x.openTag, "entry-info");
    e.uri = elemText(x.inner, "uri-entry");
    return e;
}

/** 목록 요소(MCPTTGroupInfo·ImplicitAffiliations) 안 <entry> 마다 uri-entry. */
std::vector<std::string> entryUris(const std::string& s, const char* listLocal) {
    std::vector<std::string> out;
    Elem list = elem(s, listLocal);
    if (!list.found) return out;
    size_t from = 0;
    for (;;) {
        Elem e = elem(list.inner, "entry", from);
        if (!e.found) break;
        std::string u = elemText(e.inner, "uri-entry");
        if (!u.empty()) out.push_back(u);
        from = e.end;
    }
    return out;
}

}  // namespace

bool UserProfileDoc::parse(const std::string& xml, UserProfileDoc& out, std::string* err) {
    Elem root = elem(xml, "mcptt-user-profile");
    if (!root.found) { if (err) *err = "no mcptt-user-profile"; return false; }
    UserProfileDoc d;
    d.etag = out.etag;                                     // 호출자가 헤더에서 채운 값 유지
    d.userUri = attrOf(root.openTag, "XUI-URI");
    const std::string& s = root.inner;

    // 긴급 대상 — <Common> 안에 <PrivateCall><EmergencyCall> 과 <MCPTT-group-call><EmergencyCall> 이 둘 다 있으므로
    //   부모 요소로 먼저 좁힌다(첫 <EmergencyCall> 은 사설콜 쪽이다).
    Elem gc = elem(s, "MCPTT-group-call");
    if (gc.found) {
        Elem ec = elem(gc.inner, "EmergencyCall");
        if (ec.found) d.emergencyGroup = entryOf(elem(ec.inner, "MCPTTGroupInitiation").inner, "entry");
        Elem ip = elem(gc.inner, "ImminentPerilCall");
        if (ip.found) d.imminentPerilGroup = entryOf(elem(ip.inner, "MCPTTGroupInitiation").inner, "entry");
        d.emergencyAlertGroup = entryOf(elem(gc.inner, "EmergencyAlert").inner, "entry");
    }
    Elem pc = elem(s, "PrivateCall");
    if (pc.found) {
        Elem ec = elem(pc.inner, "EmergencyCall");
        if (ec.found) d.emergencyPrivateRecipient = entryOf(elem(ec.inner, "MCPTTPrivateRecipient").inner, "entry");
    }

    Elem on = elem(s, "OnNetwork");
    if (on.found) {
        d.groups = entryUris(on.inner, "MCPTTGroupInfo");
        d.implicitAffiliations = entryUris(on.inner, "ImplicitAffiliations");
        d.maxAffiliationsN2 = intElem(on.inner, "MaxAffiliationsN2");
    }

    // 인가 — <cp:ruleset> 의 actions. ad hoc 은 규격 <anyExt><allow-adhoc-group-call>(Rel-18) 과 옛 서버 확장
    //   <cims:allow-adhoc-group-call> 이 로컬 이름이 같아 한 번에 읽힌다.
    Elem rs = elem(s, "ruleset");
    const std::string& r = rs.found ? rs.inner : std::string();
    d.allowPrivateCall = allowFlag(r, "allow-private-call");
    d.allowEmergencyGroupCall = allowFlag(r, "allow-emergency-group-call");
    d.allowCancelGroupEmergency = allowFlag(r, "allow-cancel-group-emergency");
    d.allowImminentPerilCall = allowFlag(r, "allow-imminent-peril-call");
    d.allowCancelImminentPeril = allowFlag(r, "allow-cancel-imminent-peril");
    d.allowActivateEmergencyAlert = allowFlag(r, "allow-activate-emergency-alert");
    d.allowCancelEmergencyAlert = allowFlag(r, "allow-cancel-emergency-alert");
    d.allowEmergencyPrivateCall = allowFlag(r, "allow-emergency-private-call");
    d.allowAdhocGroupCall = allowFlag(r, "allow-adhoc-group-call");
    out = d;
    return true;
}

bool ServiceConfigDoc::parse(const std::string& xml, ServiceConfigDoc& out, std::string* err) {
    Elem root = elem(xml, "service-configuration-info");
    if (!root.found) { if (err) *err = "no service-configuration-info"; return false; }
    ServiceConfigDoc d;
    d.etag = out.etag;
    Elem params = elem(root.inner, "service-configuration-params");
    if (!params.found) { out = d; return true; }           // §8.4.2.3 — params 없으면 설정 없음
    d.domain = attrOf(params.openTag, "domain");
    const std::string& s = params.inner;
    Elem common = elem(s, "common");
    if (common.found) {
        Elem bg = elem(common.inner, "broadcast-group");
        if (bg.found) {
            d.numLevelsGroupHierarchy = intElem(bg.inner, "num-levels-group-hierarchy");
            d.numLevelsUserHierarchy = intElem(bg.inner, "num-levels-user-hierarchy");
        }
    }
    Elem on = elem(s, "on-network");
    if (on.found) {
        auto rp = [&](const char* local) {
            Elem e = elem(on.inner, local);
            if (!e.found) return std::string();
            std::string ns = elemText(e.inner, "resource-priority-namespace");
            std::string pr = elemText(e.inner, "resource-priority-priority");
            return ns.empty() || pr.empty() ? std::string() : ns + "." + pr;
        };
        d.rpEmergency = rp("emergency-resource-priority");
        d.rpImminentPeril = rp("imminent-peril-resource-priority");
        d.rpNormal = rp("normal-resource-priority");
    }
    out = d;
    return true;
}

bool UeInitConfigDoc::parse(const std::string& xml, UeInitConfigDoc& out, std::string* err) {
    Elem root = elem(xml, "mcptt-UE-initial-configuration");
    if (!root.found) { if (err) *err = "no mcptt-UE-initial-configuration"; return false; }
    UeInitConfigDoc d;
    d.etag = out.etag;
    d.domain = attrOf(root.openTag, "domain");
    // *-Service-Details 는 <on-network><anyExt> 안(§7.2.2.1 9)~14)) — 요소 이름이 서비스별로 달라 on-network 안에서 바로 찾는다.
    Elem on = elem(root.inner, "on-network");
    if (on.found) {
        Elem mcptt = elem(on.inner, "MCPTT-Service-Details");
        if (mcptt.found) d.mcpttServerUri = elemText(mcptt.inner, "Server-URI");
        Elem mcdata = elem(on.inner, "MCData-Service-Details");
        if (mcdata.found) d.mcdataServerUri = elemText(mcdata.inner, "Server-URI");
        Elem mcvideo = elem(on.inner, "MCVideo-Service-Details");
        if (mcvideo.found) d.mcvideoServerUri = elemText(mcvideo.inner, "Server-URI");
        // 발언권 참여자 타이머(초, unsignedByte 0~255 — §7.2.2.7, TS 24.380 표 11.1.1-1). 0·없음 = 기본값.
        Elem t = elem(on.inner, "Timers");
        if (t.found) {
            auto ms = [&](const char* local) { int v = intElem(t.inner, local); return v > 0 && v <= 255 ? v * 1000 : 0; };
            d.floorTimers.t100Ms = ms("T100");
            d.floorTimers.t101Ms = ms("T101");
            d.floorTimers.t103Ms = ms("T103");
            d.floorTimers.t104Ms = ms("T104");
            d.floorTimers.t132Ms = ms("T132");
        }
    }
    out = d;
    return true;
}

bool McVideoUserProfileDoc::parse(const std::string& xml, McVideoUserProfileDoc& out, std::string* err) {
    Elem root = elem(xml, "mcvideo-user-profile");
    if (!root.found) { if (err) *err = "no mcvideo-user-profile"; return false; }
    McVideoUserProfileDoc d;
    d.etag = out.etag;
    d.userUri = attrOf(root.openTag, "XUI-URI");
    const std::string& s = root.inner;
    Elem common = elem(s, "Common");
    if (common.found) {
        d.mcvideoId = elemText(elem(common.inner, "MCVideoUserID").inner, "uri-entry");
        // 긴급 대상 — <PrivateCall><EmergencyCall> 과 <MCVideo-group-call><EmergencyCall> 이 둘 다 있어 부모로 먼저 좁힌다
        Elem gc = elem(common.inner, "MCVideo-group-call");
        if (gc.found) {
            d.maxSimultaneousCallsN6 = intElem(gc.inner, "MaxSimultaneousCallsN6");
            Elem ec = elem(gc.inner, "EmergencyCall");
            if (ec.found) d.emergencyGroup = entryOf(elem(ec.inner, "MCVideoGroupInitiation").inner, "entry");
            Elem ip = elem(gc.inner, "ImminentPerilCall");
            if (ip.found) d.imminentPerilGroup = entryOf(elem(ip.inner, "MCVideoGroupInitiation").inner, "entry");
            d.emergencyAlertGroup = entryOf(elem(gc.inner, "EmergencyAlert").inner, "entry");
        }
    }
    Elem on = elem(s, "OnNetwork");
    if (on.found) {
        // MCVideoGroupInfo 는 그룹마다 하나씩 되풀이된다(§9.3.2.3 OnNetworkType choice) — 각각의 MCVideo-Group-ID
        size_t from = 0;
        for (;;) {
            Elem gi = elem(on.inner, "MCVideoGroupInfo", from);
            if (!gi.found) break;
            std::string u = elemText(elem(gi.inner, "MCVideo-Group-ID").inner, "uri-entry");
            if (!u.empty()) d.groups.push_back(u);
            from = gi.end;
        }
        d.implicitAffiliations = entryUris(on.inner, "ImplicitAffiliations");
        d.maxAffiliationsN2 = intElem(on.inner, "MaxAffiliationsN2");
        d.maxSimultaneousVideoStreams = intElem(on.inner, "MaxSimultaneousVideoStreams");
    }
    Elem rs = elem(s, "ruleset");
    const std::string& r = rs.found ? rs.inner : std::string();
    d.allowPrivateCall = allowFlag(r, "allow-private-call");
    d.allowEmergencyGroupCall = allowFlag(r, "allow-emergency-group-call");
    d.allowEmergencyPrivateCall = allowFlag(r, "allow-emergency-private-call");
    d.allowImminentPerilCall = allowFlag(r, "allow-imminent-peril-call");
    d.allowActivateEmergencyAlert = allowFlag(r, "allow-activate-emergency-alert");
    d.allowRevokeTransmit = allowFlag(r, "allow-revoke-transmit");
    d.allowRemoteAmbientViewing = allowFlag(r, "allow-request-remote-initiated-ambient-viewing");
    d.allowLocalAmbientViewing = allowFlag(r, "allow-request-locally-initiated-ambient-viewing");
    d.allowAdhocGroupCall = allowFlag(r, "allow-adhoc-group-call");
    out = d;
    return true;
}

bool McVideoServiceConfigDoc::parse(const std::string& xml, McVideoServiceConfigDoc& out, std::string* err) {
    Elem root = elem(xml, "service-configuration-info");
    if (!root.found) { if (err) *err = "no service-configuration-info"; return false; }
    McVideoServiceConfigDoc d;
    d.etag = out.etag;
    Elem params = elem(root.inner, "service-configuration-params");
    if (!params.found) { out = d; return true; }
    d.domain = attrOf(params.openTag, "domain");
    Elem on = elem(params.inner, "on-network");
    if (on.found) {
        auto rp = [&](const char* local) {
            Elem e = elem(on.inner, local);
            if (!e.found) return std::string();
            std::string ns = elemText(e.inner, "resource-priority-namespace");
            std::string pr = elemText(e.inner, "resource-priority-priority");
            return ns.empty() || pr.empty() ? std::string() : ns + "." + pr;
        };
        d.rpEmergency = rp("emergency-resource-priority");
        d.rpImminentPeril = rp("imminent-peril-resource-priority");
        d.rpNormal = rp("normal-resource-priority");
        Elem sp = elem(on.inner, "signalling-protection");
        if (sp.found) {                                    // 요소가 없으면 켜진 것으로 읽는다(TS 24.281 §6.6.2.1·§6.6.3.1)
            d.confidentialityProtection = allowFlag(sp.inner, "confidentiality-protection");
            d.integrityProtection = allowFlag(sp.inner, "integrity-protection");
        }
        Elem tc = elem(on.inner, "tc-timers-counters-R14");   // <anyExt> 안(§9.4.2.1 d)) — 참여자 T100~T104 는 초(unsignedByte)
        if (tc.found) {
            d.t100Sec = intElem(tc.inner, "T100-transmission-request");
            d.t101Sec = intElem(tc.inner, "T101-transmission-end-request");
            d.t102Sec = intElem(tc.inner, "T102-queue-position-request");
            d.t103Sec = intElem(tc.inner, "T103-receive-media-request");
            d.t104Sec = intElem(tc.inner, "T104-receive-media-release");
        }
    }
    out = d;
    return true;
}

Capabilities Capabilities::of(const UserProfileDoc* up, const ServiceConfigDoc* sc) {
    Capabilities c;
    c.userProfileKnown = up != nullptr;
    c.serviceConfigKnown = sc != nullptr;
    UserProfileDoc u;                                      // 기본값 = 전부 허용(미수신)
    if (up) u = *up;
    c.privateCall = u.allowPrivateCall;
    c.emergencyGroupCall = u.allowEmergencyGroupCall;
    c.cancelGroupEmergency = u.allowCancelGroupEmergency;
    c.imminentPerilCall = u.allowImminentPerilCall;
    c.cancelImminentPeril = u.allowCancelImminentPeril;
    c.emergencyPrivateCall = u.allowPrivateCall && u.allowEmergencyPrivateCall;
    c.emergencyAlert = u.allowActivateEmergencyAlert;
    c.cancelEmergencyAlert = u.allowCancelEmergencyAlert;
    c.adhocGroupCall = u.allowAdhocGroupCall;
    c.maxAffiliationsN2 = u.maxAffiliationsN2 > 0 ? u.maxAffiliationsN2 : 0;
    return c;
}

}  // namespace cimsue
