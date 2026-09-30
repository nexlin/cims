#!/usr/bin/env python3
"""MCVideo 전송 제어 정의 테이블(docs/design/features/mcvideo_tc_defs.yaml) → 생성·대조 (계약 K5 — mcvideo_dev_plan.md §3).

  gen_mcvideo_tc_defs.py            sdk/core/src/mcvideo/tc_defs.h · cmp/PTransmissionDefs.h 생성(정본 테이블에서)
  gen_mcvideo_tc_defs.py --check    두 생성물이 테이블과 같은지 대조 (S1 게이트 S1-UE-MCVIDEO-TC-DEFS)

규격 = 3GPP TS 24.581 §9.2(메시지·필드·원인)·§11(타이머·카운터)·§12.1.2(fmtp). 단말 코어와 CMP 가 같은 테이블에서 난 상수를 쓰므로
두 끝이 다른 전송 제어를 말할 수 없다. 테이블 파서는 floor 정의 생성기(gen_floor_defs.py)의 것을 쓴다 — 외부 의존 없음.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_floor_defs import load_table  # noqa: E402 — 같은 모양(2단계 매핑)의 미니 YAML 파서

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAML = os.path.join(ROOT, "docs/design/features/mcvideo_tc_defs.yaml")
OUT_SDK = os.path.join(ROOT, "sdk/core/src/mcvideo/tc_defs.h")
OUT_CMP = os.path.join(ROOT, "cmp/PTransmissionDefs.h")

APPS = (("mcv0", "MCV0", "name_participant"), ("mcv1", "MCV1", "name_server"), ("mcv2", "MCV2", "name_both"))
KINDS = ("u8", "u16", "pair", "ssrc", "name", "uri", "cause", "track")
FIXED_LEN = {"u8": 2, "u16": 2, "pair": 2, "ssrc": 6, "name": 6}
CAUSES = (("reject_cause", "RejectCause", "rejectCauseText", "TC_REJECT_", "McvRejectCause", "McvRejectCauseText"),
          ("revoke_cause", "RevokeCause", "revokeCauseText", "TC_REVOKE_", "McvRevokeCause", "McvRevokeCauseText"),
          ("receive_reject_cause", "ReceiveRejectCause", "receiveRejectCauseText", "TC_RECV_REJECT_",
           "McvReceiveRejectCause", "McvReceiveRejectCauseText"))
ENUMS = (("source", "Source", "McvSource", "TC_SRC_"), ("permission", "Permission", "McvPermission", "TC_PERM_"),
         ("result", "ReceiveResult", "McvReceiveResult", "TC_RESULT_"),
         ("reception_mode", "ReceptionMode", "McvReceptionMode", "TC_RECEPTION_"))
HEAD = ("// 생성 파일 — 손으로 고치지 않는다. 정본: docs/design/features/mcvideo_tc_defs.yaml,",
        "// 생성기: scripts/gen_mcvideo_tc_defs.py (--check 가 sdk·cmp 두 생성물의 최신성을 대조 — S1-UE-MCVIDEO-TC-DEFS).")


def validate(t):
    """테이블 자체 정합 — 필드 이름·kind·메시지 필드 목록·값 중복."""
    errs = []
    names = set(t["fields"])
    ids = {}
    for k, v in t["fields"].items():
        if v.get("kind") not in KINDS:
            errs.append(f"fields.{k}: kind {v.get('kind')!r} 모름")
        if v["value"] in ids:
            errs.append(f"fields.{k}: ID {v['value']} 가 {ids[v['value']]} 와 겹친다")
        ids[v["value"]] = k
        if v["value"] >= 192:
            errs.append(f"fields.{k}: ID ≥ 192(2옥텟 Length)는 이 생성기가 다루지 않는다")
        if v["value"] >= 32:
            errs.append(f"fields.{k}: ID ≥ 32 는 allowedFields 비트마스크에 들어가지 않는다")
    for sec, _, _ in APPS:
        seen = {}
        for k, v in t[sec].items():
            if v["value"] in seen:
                errs.append(f"{sec}.{k}: subtype {v['value']} 가 {seen[v['value']]} 와 겹친다")
            seen[v["value"]] = k
            if v["value"] & ~t["rtcp"]["subtype_mask"]:
                errs.append(f"{sec}.{k}: subtype {v['value']} 가 4비트를 넘는다")
            for f in v.get("fields", "").split():
                if f not in names:
                    errs.append(f"{sec}.{k}: 필드 {f} 가 fields 에 없다")
    return errs


def _mask(t, msg):
    m = 0
    for f in msg.get("fields", "").split():
        m |= 1 << t["fields"][f]["value"]
    return m


def _kind_ident(k):
    return {"u8": "U8", "u16": "U16", "pair": "Pair", "ssrc": "Ssrc", "name": "Name", "uri": "Uri",
            "cause": "Cause", "track": "Track"}[k]


def _chars(s):
    return "{" + ", ".join(f"'{c}'" for c in s) + "}"


def gen_sdk(t):
    L = list(HEAD)
    L.append("// 3GPP TS 24.581 §9.2·§11·§12.1.2 — MCVideo 전송 제어 메시지·필드·원인·타이머·fmtp 정의(단말 코어).")
    L += ["#pragma once", "#include <cstdint>", "#include <cstring>", "", "namespace cimsue {", "namespace mcvideo {", ""]
    r = t["rtcp"]
    L.append(f"constexpr uint8_t kRtcpPtApp = {r['pt_app']};")
    L.append(f"constexpr uint8_t kAckRequiredBit = 0x{r['ack_required_bit']:02X};")
    L.append(f"constexpr uint8_t kSubtypeMask = 0x{r['subtype_mask']:02X};")
    L.append("")
    L.append("/** RTCP APP name(§9.1.2) — MCV0 참여자→서버 · MCV1 서버→참여자 · MCV2 양방향. */")
    L.append("enum class AppName : uint8_t { MCV0 = 0, MCV1 = 1, MCV2 = 2 };")
    for sec, ident, key in APPS:
        L.append(f"constexpr char kName{ident.capitalize()}[4] = {_chars(r[key])};")
    L.append("/** 헤더의 name 4옥텟 → AppName. 전송 제어(MCV0~2)가 아니면 false. */")
    L.append("inline bool appNameOf(const char* name4, AppName& out) {")
    for i, (sec, ident, key) in enumerate(APPS):
        L.append(f"    if (std::memcmp(name4, kName{ident.capitalize()}, 4) == 0) {{ out = AppName::{ident}; return true; }}")
    L.append("    return false;")
    L.append("}")
    L.append("inline const char* appNameText(AppName a) {")
    L.append("    switch (a) {")
    for sec, ident, key in APPS:
        L.append(f"        case AppName::{ident}: return \"{r[key]}\";")
    L.append("    }")
    L.append("    return \"\";")
    L.append("}")
    L.append("")
    for sec, ident, key in APPS:
        L.append(f"/** name {r[key]} 의 subtype(메시지 타입, 첫 비트 = ack 요구). */")
        L.append(f"enum class {ident.capitalize()} : uint8_t {{")
        for k, v in t[sec].items():
            L.append(f"    {k} = 0x{v['value']:02X},  // {v['name']} (§{v['ref']})")
        L.append("};")
    L.append("/** (name, subtype) → 메시지 식별자 이름(로그용). 모르면 \"UNKNOWN\". */")
    L.append("inline const char* msgName(AppName app, uint8_t subtype) {")
    L.append("    switch (app) {")
    for sec, ident, key in APPS:
        L.append(f"        case AppName::{ident}:")
        L.append("            switch (subtype & kSubtypeMask) {")
        for k, v in t[sec].items():
            L.append(f"                case 0x{v['value']:02X}: return \"{k}\";")
        L.append("                default: return \"UNKNOWN\";")
        L.append("            }")
    L.append("    }")
    L.append("    return \"UNKNOWN\";")
    L.append("}")
    L.append("/** 이 판본이 정한 메시지인가 — 모르는 subtype 이면 메시지 전체를 버린다(§9.1.4 1). */")
    L.append("inline bool knownMessage(AppName app, uint8_t subtype) {")
    L.append("    switch (app) {")
    for sec, ident, key in APPS:
        L.append(f"        case AppName::{ident}:")
        L.append("            switch (subtype & kSubtypeMask) {")
        L.append("                " + " ".join(f"case 0x{v['value']:02X}:" for v in t[sec].values()) + " return true;")
        L.append("                default: return false;")
        L.append("            }")
    L.append("    }")
    L.append("    return false;")
    L.append("}")
    L.append("/** 메시지가 실을 수 있는 필드 집합 — 비트 i = field ID i(§9.2.4~§9.2.31 의 표). 모르는 메시지 = 0. */")
    L.append("inline uint32_t allowedFields(AppName app, uint8_t subtype) {")
    L.append("    switch (app) {")
    for sec, ident, key in APPS:
        L.append(f"        case AppName::{ident}:")
        L.append("            switch (subtype & kSubtypeMask) {")
        for k, v in t[sec].items():
            L.append(f"                case 0x{v['value']:02X}: return 0x{_mask(t, v):08X}u;  // {k}")
        L.append("                default: return 0;")
        L.append("            }")
    L.append("    }")
    L.append("    return 0;")
    L.append("}")
    L.append("")
    L.append("/** Table 9.2.3.1-1 — 전송 제어 필드 ID. */")
    L.append("enum class Field : uint8_t {")
    for k, v in t["fields"].items():
        L.append(f"    {k} = {v['value']},  // §{v['ref']}")
    L.append("};")
    L.append("/** 필드 값의 모양 — 정본 테이블 kind(u8·u16·pair = Length 2, ssrc·name = Length 6, uri·cause·track = 가변·4옥텟 경계 패딩). */")
    L.append("enum class FieldKind : uint8_t { " + ", ".join(_kind_ident(k) for k in KINDS) + ", Unknown };")
    L.append("inline FieldKind fieldKind(uint8_t id) {")
    L.append("    switch (id) {")
    for k, v in t["fields"].items():
        L.append(f"        case {v['value']}: return FieldKind::{_kind_ident(v['kind'])};")
    L.append("        default: return FieldKind::Unknown;")
    L.append("    }")
    L.append("}")
    L.append("/** 고정 길이 kind 의 Length 값, 가변(uri·cause·track)·미지는 -1. */")
    L.append("inline int fixedLength(FieldKind k) {")
    L.append("    switch (k) {")
    for k in KINDS:
        if k in FIXED_LEN:
            L.append(f"        case FieldKind::{_kind_ident(k)}: return {FIXED_LEN[k]};")
    L.append("        default: return -1;")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("/** §9.2.3.11 Transmission Indicator 비트. */")
    L.append("namespace indicator {")
    for k, v in t["indicator"].items():
        L.append(f"constexpr uint16_t {k} = 0x{v:04X};")
    L.append("}  // namespace indicator")
    L.append("")
    for sec, name, _, _ in ENUMS:
        L.append(f"enum class {name} : uint16_t {{")
        for k, v in t[sec].items():
            L.append(f"    {k} = {v},")
        L.append("};")
    L.append("/** §9.2.3.5 Queue Position Info 특수값 · §9.2.3.13 Queueing Capability · 기본 우선순위(§9.2.3.2·§9.2.3.19). */")
    L.append("namespace queue {")
    for k, v in t["queue"].items():
        L.append(f"constexpr uint8_t {k} = {v};")
    L.append("}  // namespace queue")
    L.append("")
    for sec, ename, fn, _, _, _ in CAUSES:
        L.append(f"enum class {ename} : uint16_t {{")
        for k, v in t[sec].items():
            L.append(f"    {k} = {v['value']},")
        L.append("};")
        L.append(f"inline const char* {fn}(int v) {{")
        L.append("    switch (v) {")
        for k, v in t[sec].items():
            L.append(f"        case {v['value']}: return \"{v['text']}\";")
        L.append("        default: return nullptr;")
        L.append("    }")
        L.append("}")
    L.append("")
    L.append("/** §11 타이머(ms)·카운터 기본값 — 값의 정본은 service configuration `<tc-timers-counters-R14>`·그룹 문서, 없을 때 이 값. */")
    L.append("namespace timer {")
    for sec in ("participant_timers", "server_timers"):
        for k, v in t[sec].items():
            L.append(f"constexpr int {k}_MS = {v['default_ms']};  // {v['name']} ({v['origin']}, <{v['element']}>)")
    for sec in ("participant_counters", "server_counters"):
        for k, v in t[sec].items():
            L.append(f"constexpr int {k} = {v['default']};  // {v['name']}")
    L.append("}  // namespace timer")
    L.append("")
    s = t["sdp"]
    L.append("/** §4.3.3.1·§12.1.2 — 제어 채널 `m=application <RTCP 포트> udp MCVideo` 와 `a=fmtp:MCVideo` 파라미터. */")
    L.append(f"constexpr const char* kSdpProto = \"{s['proto']}\";")
    L.append(f"constexpr const char* kSdpFmt = \"{s['fmt']}\";")
    L.append(f"constexpr const char* kFmtpSeparator = \"{s['separator']}\";")
    L.append("namespace fmtp {")
    for k, v in t["fmtp"].items():
        L.append(f"constexpr const char* {k} = \"{v}\";")
    L.append("}  // namespace fmtp")
    L += ["", "}  // namespace mcvideo", "}  // namespace cimsue"]
    return "\n".join(L) + "\n"


def gen_cmp(t):
    L = list(HEAD)
    L.append("// 3GPP TS 24.581 §9.2·§11 — MCVideo 전송 제어 메시지·필드·원인·타이머 정의(CMP 서버 이름).")
    L += ["#ifndef __TRANSMISSION_DEFS_H__", "#define __TRANSMISSION_DEFS_H__", ""]
    r = t["rtcp"]
    L.append(f"#define MCV_RTCP_PT_APP {r['pt_app']}")
    L.append(f"#define MCV_ACK_REQ_BIT 0x{r['ack_required_bit']:02X}")
    L.append(f"#define MCV_SUBTYPE(subtype) ((subtype) & 0x{r['subtype_mask']:02X})")
    for sec, ident, key in APPS:
        L.append(f"#define MCV_NAME_{ident[-1]} \"{r[key]}\"")
    L.append("")
    L.append("// RTCP APP name (§9.1.2) — MCV0 참여자→서버 · MCV1 서버→참여자 · MCV2 양방향.")
    L.append("enum McvAppName { " + ", ".join(f"MCV_APP_{i[-1]} = {n}" for n, (_, i, _) in enumerate(APPS)) + " };")
    L.append("")
    for sec, ident, key in APPS:
        L.append(f"// name {r[key]} subtype")
        L.append(f"enum {ident.capitalize()}Subtype {{")
        items = list(t[sec].items())
        for n, (k, v) in enumerate(items):
            comma = "," if n < len(items) - 1 else ""
            L.append(f"    {ident}_{k} = 0x{v['value']:X}{comma}  // {v['name']} (§{v['ref']})")
        L.append("};")
    L.append("// (app, subtype) → 메시지 식별자 이름(로그용).")
    L.append("inline const char* McvMessageName(int app, int subtype) {")
    L.append("    switch (app) {")
    for sec, ident, key in APPS:
        L.append(f"    case MCV_APP_{ident[-1]}:")
        L.append("        switch (MCV_SUBTYPE(subtype)) {")
        for k, v in t[sec].items():
            L.append(f"        case {ident}_{k}: return \"{k}\";")
        L.append("        default: return \"UNKNOWN\";")
        L.append("        }")
    L.append("    }")
    L.append("    return \"UNKNOWN\";")
    L.append("}")
    L.append("// 이 판본이 정한 메시지인가 — 모르는 subtype 이면 메시지 전체를 버린다(§9.1.4 1).")
    L.append("inline bool McvKnownMessage(int app, int subtype) {")
    L.append("    switch (app) {")
    for sec, ident, key in APPS:
        L.append(f"    case MCV_APP_{ident[-1]}:")
        L.append("        switch (MCV_SUBTYPE(subtype)) {")
        L.append("        " + " ".join(f"case {ident}_{k}:" for k in t[sec]) + " return true;")
        L.append("        default: return false;")
        L.append("        }")
    L.append("    }")
    L.append("    return false;")
    L.append("}")
    L.append("// 메시지가 실을 수 있는 필드 집합 — 비트 i = field ID i. 모르는 메시지 = 0.")
    L.append("inline unsigned McvAllowedFields(int app, int subtype) {")
    L.append("    switch (app) {")
    for sec, ident, key in APPS:
        L.append(f"    case MCV_APP_{ident[-1]}:")
        L.append("        switch (MCV_SUBTYPE(subtype)) {")
        for k, v in t[sec].items():
            L.append(f"        case {ident}_{k}: return 0x{_mask(t, v):08X}u;")
        L.append("        default: return 0;")
        L.append("        }")
    L.append("    }")
    L.append("    return 0;")
    L.append("}")
    L.append("")
    L.append("// Table 9.2.3.1-1 — 전송 제어 필드 ID.")
    L.append("enum McvField {")
    items = list(t["fields"].items())
    for n, (k, v) in enumerate(items):
        comma = "," if n < len(items) - 1 else ""
        L.append(f"    TF_{k} = {v['value']}{comma}  // §{v['ref']}")
    L.append("};")
    L.append("// 필드 값의 모양 — u8·u16·pair = Length 2, ssrc·name = Length 6, uri·cause·track = 가변(4옥텟 경계 패딩).")
    L.append("enum McvFieldKind { " + ", ".join(f"TFK_{k.upper()}" for k in KINDS) + ", TFK_UNKNOWN };")
    L.append("inline McvFieldKind McvFieldKindOf(int id) {")
    L.append("    switch (id) {")
    for k, v in t["fields"].items():
        L.append(f"    case TF_{k}: return TFK_{v['kind'].upper()};")
    L.append("    default: return TFK_UNKNOWN;")
    L.append("    }")
    L.append("}")
    L.append("// 고정 길이 kind 의 Length 값, 가변·미지는 -1.")
    L.append("inline int McvFixedLength(McvFieldKind k) {")
    L.append("    switch (k) {")
    for k in KINDS:
        if k in FIXED_LEN:
            L.append(f"    case TFK_{k.upper()}: return {FIXED_LEN[k]};")
    L.append("    default: return -1;")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("// §9.2.3.11 Transmission Indicator 비트.")
    L.append("enum McvIndicatorBits {")
    items = list(t["indicator"].items())
    for n, (k, v) in enumerate(items):
        L.append(f"    TI_{k} = 0x{v:04X}" + ("," if n < len(items) - 1 else ""))
    L.append("};")
    for sec, _, cname, prefix in ENUMS:
        items = list(t[sec].items())
        L.append(f"enum {cname} {{ " + ", ".join(f"{prefix}{k} = {v}" for k, v in items) + " };")
    for k, v in t["queue"].items():
        L.append(f"#define TC_QUEUE_{k} {v}")
    L.append("")
    for sec, _, _, prefix, cname, fn in CAUSES:
        items = list(t[sec].items())
        L.append(f"enum {cname} {{")
        for n, (k, v) in enumerate(items):
            L.append(f"    {prefix}{k} = {v['value']}" + ("," if n < len(items) - 1 else ""))
        L.append("};")
        L.append(f"inline const char* {fn}(int v) {{")
        L.append("    switch (v) {")
        for k, v in items:
            L.append(f"    case {v['value']}: return \"{v['text']}\";")
        L.append("    default: return nullptr;")
        L.append("    }")
        L.append("}")
    L.append("")
    L.append("// §11 타이머(ms)·카운터 기본값 — 값의 정본은 service configuration `<tc-timers-counters-R14>`·그룹 문서.")
    for sec in ("participant_timers", "server_timers"):
        for k, v in t[sec].items():
            L.append(f"#define MCV_{k}_MS {v['default_ms']}  // {v['name']} ({v['origin']})")
    for sec in ("participant_counters", "server_counters"):
        for k, v in t[sec].items():
            L.append(f"#define MCV_{k} {v['default']}  // {v['name']}")
    L += ["", "#endif  // __TRANSMISSION_DEFS_H__"]
    return "\n".join(L) + "\n"


def check(t):
    fails = validate(t)
    for path, gen in ((OUT_SDK, gen_sdk), (OUT_CMP, gen_cmp)):
        if not os.path.exists(path) or open(path, encoding="utf-8").read() != gen(t):
            fails.append(f"{os.path.relpath(path, ROOT)} 가 테이블과 다르다 — gen_mcvideo_tc_defs.py 재실행")
    for f in fails:
        print("FAIL:", f)
    if not fails:
        print("mcvideo tc defs OK — 테이블 정합, sdk·cmp 생성물 최신")
    return 0 if not fails else 1


def main():
    t = load_table(YAML)
    if "--check" in sys.argv:
        return check(t)
    errs = validate(t)
    if errs:
        for e in errs:
            print("ERROR:", e)
        return 1
    for path, gen in ((OUT_SDK, gen_sdk), (OUT_CMP, gen_cmp)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8").write(gen(t))
        print("generated", os.path.relpath(path, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
