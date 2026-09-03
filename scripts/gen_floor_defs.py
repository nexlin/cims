#!/usr/bin/env python3
"""MCPTT floor 정의 테이블(docs/design/features/mcptt_floor_defs.yaml) → 생성·대조.

  gen_floor_defs.py            sdk/core/src/floor/floor_defs.h 생성(정본 테이블에서)
  gen_floor_defs.py --check    생성물이 최신인지 + cmp/PMcpttGroup.h · android FloorControl.kt ·
                               scripts/mcptt_floor_policy_probe.py 의 상수가 테이블과 같은지 대조 (S1 게이트)

PyYAML 없이 동작한다(테이블은 단순 매핑만 쓰므로 자체 파서). 의도적으로 외부 의존이 없다.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAML = os.path.join(ROOT, "docs/design/features/mcptt_floor_defs.yaml")
OUT_H = os.path.join(ROOT, "sdk/core/src/floor/floor_defs.h")
CMP_H = os.path.join(ROOT, "cmp/PMcpttGroup.h")
KT = os.path.join(ROOT, "android/ptt-client/src/main/java/com/cims/ue/ptt/floor/FloorControl.kt")
PROBE = os.path.join(ROOT, "scripts/mcptt_floor_policy_probe.py")


def _val(s):
    s = s.strip()
    if s.startswith('"'):
        return s.strip('"')
    if re.fullmatch(r"0x[0-9a-fA-F]+", s):
        return int(s, 16)
    if re.fullmatch(r"-?\d+", s):
        return int(s)
    if s in ("true", "false"):
        return s == "true"
    return s


def load_table(path):
    """2단계 매핑(section → key → value|{k: v}) 전용 미니 YAML 파서."""
    table = {}
    section = None
    for raw in open(path, encoding="utf-8"):
        line = raw.split("#", 1)[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            continue
        if not line.startswith(" "):
            section = line.rstrip(":")
            table[section] = {}
            continue
        m = re.match(r"\s+([^:]+):\s*(.*)$", line)
        if not m:
            continue
        key, rest = m.group(1).strip(), m.group(2).strip()
        if rest.startswith("{"):
            inner = {}
            for kv in re.findall(r"([a-z_]+):\s*(\"[^\"]*\"|[^,}]+)", rest[1:-1]):
                inner[kv[0]] = _val(kv[1])
            table[section][key] = inner
        else:
            table[section][key] = _val(rest)
    return table


def gen_header(t):
    L = []
    L.append("// 생성 파일 — 손으로 고치지 않는다. 정본: docs/design/features/mcptt_floor_defs.yaml,")
    L.append("// 생성기: scripts/gen_floor_defs.py (--check 가 cmp/android/probe 상수와 대조).")
    L.append("// 3GPP TS 24.380 §8 — MCPTT floor control 메시지 정의.")
    L.append("#pragma once")
    L.append("#include <cstdint>")
    L.append("")
    L.append("namespace cimsue {")
    L.append("namespace floor {")
    L.append("")
    r = t["rtcp"]
    L.append(f"constexpr uint8_t kRtcpPtApp = {r['pt_app']};")
    L.append(f'constexpr char kRtcpName[4] = {{\'{r["name"][0]}\', \'{r["name"][1]}\', \'{r["name"][2]}\', \'{r["name"][3]}\'}};')
    L.append(f"constexpr uint8_t kAckRequiredBit = 0x{r['ack_required_bit']:02X};")
    L.append("")
    L.append("/** Table 8.2.2-1 — RTCP APP subtype(메시지 타입). */")
    L.append("enum class Op : uint8_t {")
    for k, v in t["opcodes"].items():
        L.append(f"    {k} = 0x{v['value']:02X},  // {v['name']} ({v['dir']})")
    L.append("};")
    L.append("inline const char* opName(uint8_t op) {")
    L.append("    switch (op & 0x0F) {")
    for k, v in t["opcodes"].items():
        L.append(f"        case 0x{v['value']:02X}: return \"{k}\";")
    L.append("        default: return \"UNKNOWN\";")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("/** §8.2.3 — floor control specific field ID. */")
    L.append("enum class Field : uint8_t {")
    for k, v in t["fields"].items():
        L.append(f"    {k} = {v['value']},")
    L.append("};")
    strs = [k for k, v in t["fields"].items() if v.get("string")]
    L.append("/** 가변 길이(문자열) 필드 — 4옥텟 정렬 패딩 대상. */")
    L.append("inline bool isStringField(uint8_t id) {")
    L.append("    switch (id) {")
    for k in strs:
        L.append(f"        case {t['fields'][k]['value']}:")
    L.append("            return true;")
    L.append("        default: return false;")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("/** §8.2.3.13 Floor Indicator 비트. */")
    L.append("namespace indicator {")
    for k, v in t["indicator"].items():
        L.append(f"constexpr uint16_t {k} = 0x{v:04X};")
    L.append("}  // namespace indicator")
    L.append("")
    for sec, name in (("source", "Source"), ("permission", "Permission"), ("queued_purpose", "QueuedPurpose")):
        L.append(f"enum class {name} : uint16_t {{")
        for k, v in t[sec].items():
            L.append(f"    {k} = {v},")
        L.append("};")
        L.append("")
    for sec, fn in (("reject_cause", "rejectCauseText"), ("revoke_cause", "revokeCauseText"),
                    ("queued_result", "queuedResultText")):
        L.append(f"inline const char* {fn}(int v) {{")
        L.append("    switch (v) {")
        for k, v in t[sec].items():
            L.append(f"        case {k}: return \"{v}\";")
        L.append("        default: return nullptr;")
        L.append("    }")
        L.append("}")
        L.append("")
    L.append(f"constexpr uint8_t kMediaFlowResumeBit = 0x{t['media_flow']['RESUME_BIT']:02X};")
    L.append("")
    L.append("}  // namespace floor")
    L.append("}  // namespace cimsue")
    return "\n".join(L) + "\n"


def _grep_consts(text, pattern):
    """pattern 의 그룹1=이름, 그룹2=값 을 {name: int}."""
    out = {}
    for m in re.finditer(pattern, text):
        try:
            out[m.group(1)] = int(m.group(2), 0)
        except ValueError:
            pass
    return out


def check(t):
    fails = []

    def cmp(label, expect, actual, mapping):
        for k, ak in mapping.items():
            if ak not in actual:
                fails.append(f"{label}: {ak} 없음 (테이블 {k}={expect[k]})")
            elif actual[ak] != expect[k]:
                fails.append(f"{label}: {ak}={actual[ak]} ≠ 테이블 {k}={expect[k]}")

    ops = {k: v["value"] for k, v in t["opcodes"].items()}
    flds = {k: v["value"] for k, v in t["fields"].items()}

    # 1) 생성물 최신성
    if not os.path.exists(OUT_H) or open(OUT_H, encoding="utf-8").read() != gen_header(t):
        fails.append(f"{os.path.relpath(OUT_H, ROOT)} 가 테이블과 다르다 — gen_floor_defs.py 재실행")

    # 2) CMP 서버 헤더
    if os.path.exists(CMP_H):
        txt = open(CMP_H, encoding="utf-8").read()
        c_ops = _grep_consts(txt, r"\b(FLOOR_[A-Z_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
        c_ff = _grep_consts(txt, r"\b(FF_[A-Z_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
        cmp("cmp/PMcpttGroup.h opcodes", ops, c_ops, {
            "REQUEST": "FLOOR_REQUEST", "GRANTED": "FLOOR_GRANT", "TAKEN": "FLOOR_TAKEN", "DENY": "FLOOR_REJECT",
            "RELEASE": "FLOOR_RELEASE", "IDLE": "FLOOR_IDLE", "REVOKE": "FLOOR_REVOKE",
            "QUEUE_POS_REQ": "FLOOR_QUEUE_POS_REQ", "QUEUE_POS_INFO": "FLOOR_QUEUE_POS_INFO", "ACK": "FLOOR_ACK",
            "MEDIA_FLOW": "FLOOR_MEDIA_FLOW", "QUEUED_CANCEL": "FLOOR_QUEUED_CANCEL", "RELEASE_MULTI": "FLOOR_RELEASE_MULTI"})
        cmp("cmp/PMcpttGroup.h fields", flds, c_ff, {k: "FF_" + k for k in flds})
        m = re.search(r"#define\s+FLOOR_ACK_REQ_BIT\s+(0x[0-9A-Fa-f]+)", txt)
        if not m or int(m.group(1), 16) != t["rtcp"]["ack_required_bit"]:
            fails.append("cmp/PMcpttGroup.h FLOOR_ACK_REQ_BIT ≠ 테이블")

    # 3) Android Kotlin
    if os.path.exists(KT):
        txt = open(KT, encoding="utf-8").read()
        k_all = {}
        blk = re.search(r"object FloorMsgType \{(.*?)\n\}", txt, re.S)     # opcode 는 이 블록 안에서만(필드 MEDIA_FLOW 와 이름 충돌)
        if blk:
            k_all = _grep_consts(blk.group(1), r"const val ([A-Z_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
        cmp("FloorControl.kt opcodes", ops, k_all, {
            "REQUEST": "REQUEST", "GRANTED": "GRANTED", "TAKEN": "TAKEN", "DENY": "DENY", "RELEASE": "RELEASE",
            "IDLE": "IDLE", "REVOKE": "REVOKE", "QUEUE_POS_REQ": "QUEUE_POS_REQUEST", "QUEUE_POS_INFO": "QUEUE_POS_INFO",
            "ACK": "ACK", "MEDIA_FLOW": "MEDIA_FLOW", "QUEUED_CANCEL": "QUEUED_CANCEL", "RELEASE_MULTI": "RELEASE_MULTI"})
        # 필드 상수는 object FloorFieldId 블록 안에서만 (MEDIA_FLOW 이름이 opcode 와 겹친다)
        blk = re.search(r"object FloorFieldId \{(.*?)\n\}", txt, re.S)
        if blk:
            k_ff = _grep_consts(blk.group(1), r"const val ([A-Z_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
            cmp("FloorControl.kt FloorFieldId", flds, k_ff, {k: k for k in flds})
        blk = re.search(r"object FloorIndicator \{(.*?)\n\}", txt, re.S)
        if blk:
            k_fi = _grep_consts(blk.group(1), r"const val ([A-Z_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
            cmp("FloorControl.kt FloorIndicator", t["indicator"], k_fi, {k: k for k in t["indicator"]})
        if k_all.get("ACK_REQUIRED_BIT") != t["rtcp"]["ack_required_bit"]:
            fails.append("FloorControl.kt ACK_REQUIRED_BIT ≠ 테이블")

    # 4) Python probe
    if os.path.exists(PROBE):
        txt = "\n".join(l.split("#", 1)[0].rstrip() for l in open(PROBE, encoding="utf-8"))   # 주석 제거
        p = {}
        for m in re.finditer(r"^([A-Z_][A-Z_0-9, ]*?)\s*=\s*([0-9xXa-fA-F, ]+)$", txt, re.M):
            names = [n.strip() for n in m.group(1).split(",")]
            vals = [v.strip() for v in m.group(2).split(",")]
            if len(names) == len(vals):
                for n, v in zip(names, vals):
                    try:
                        p[n] = int(v, 0)
                    except ValueError:
                        pass
        cmp("probe opcodes", ops, p, {
            "REQUEST": "REQUEST", "GRANTED": "GRANT", "TAKEN": "TAKEN", "DENY": "DENY", "RELEASE": "RELEASE",
            "IDLE": "IDLE", "REVOKE": "REVOKE", "QUEUE_POS_REQ": "QUEUE_POS_REQ", "QUEUE_POS_INFO": "QUEUE_POS_INFO",
            "ACK": "ACK", "MEDIA_FLOW": "FLOOR_MEDIA_FLOW", "QUEUED_CANCEL": "QUEUED_CANCEL", "RELEASE_MULTI": "RELEASE_MULTI"})
        cmp("probe fields", flds, p, {
            "PRIORITY": "FF_PRIORITY", "DURATION": "FF_DURATION", "REJECT_CAUSE": "FF_CAUSE", "QUEUE_INFO": "FF_QUEUE_INFO",
            "GRANTED_PARTY": "FF_GRANTED_PARTY", "PERMISSION": "FF_PERMISSION", "USER_ID": "FF_USER_ID",
            "MSG_SEQ": "FF_MSG_SEQ", "SOURCE": "FF_SOURCE", "MSG_TYPE": "FF_MSG_TYPE", "FLOOR_INDICATOR": "FF_INDICATOR",
            "SSRC": "FF_SSRC", "GRANTED_USERS": "FF_GRANTED_USERS", "SSRC_LIST": "FF_SSRC_LIST",
            "QUEUED_PURPOSE": "FF_QUEUED_PURPOSE", "QUEUED_USERS": "FF_QUEUED_USERS", "QUEUED_RESULT": "FF_QUEUED_RESULT",
            "MEDIA_FLOW": "FF_MEDIA_FLOW"})
        cmp("probe indicator", t["indicator"], p, {"EMERGENCY": "FI_EMERGENCY", "DUAL_FLOOR": "FI_DUAL", "MULTI_TALKER": "FI_MULTI"})
        if p.get("ACK_REQ_BIT") != t["rtcp"]["ack_required_bit"]:
            fails.append("probe ACK_REQ_BIT ≠ 테이블")
        if p.get("RTCP_PT_APP") != t["rtcp"]["pt_app"]:
            fails.append("probe RTCP_PT_APP ≠ 테이블")

    for f in fails:
        print("FAIL:", f)
    if not fails:
        print("floor defs OK — 생성물 최신, cmp/android/probe 상수 일치")
    return 0 if not fails else 1


def main():
    t = load_table(YAML)
    if "--check" in sys.argv:
        return check(t)
    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    open(OUT_H, "w", encoding="utf-8").write(gen_header(t))
    print("generated", os.path.relpath(OUT_H, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
