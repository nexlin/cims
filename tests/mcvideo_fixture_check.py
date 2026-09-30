#!/usr/bin/env python3
"""MCVideo 계약 골든 문서 스키마 검사 (계약 K2·K3 — docs/dev/mcvideo_dev_plan.md §3).

tests/fixtures/mcvideo/ 의 XML(그룹 문서·MCVideo user profile·service config·ue-init-config)과 SIP 골든 메시지의 XML 본문
(mcvideo-info·pidf mcvideoPresInfo)을 규격 스키마로 **엄격** 검증한다. 스키마 = tests/fixtures/mcvideo/xsd/ — 3GPP 원문에서
그대로 옮긴 XSD(TS 24.481 V19.3.0 · TS 24.484 V20.0.0 · TS 24.281 V18.14.0) + 원문이 반입되지 않은 OMA/IETF 틀의 보조 스키마(aux-*).

알려진 비규격 요소는 아래 KNOWN_DEVIATIONS 에 사유와 함께 적힌 것만 검증 전에 떼어 낸다 — 목록에 없는 비규격 요소가 나오면 실패다.

xmlschema(+elementpath) 가 import 되지 않으면 SKIP(종료 0). 설치: pip install --target <dir> xmlschema 뒤 PYTHONPATH=<dir>.

  python3 tests/mcvideo_fixture_check.py            # 전부
  python3 tests/mcvideo_fixture_check.py FILE...    # 지정 파일(그룹 문서 등 CSC 산출물 점검용)
"""
from __future__ import annotations

import copy
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, "fixtures", "mcvideo")
XSD = os.path.join(FIX, "xsd")

GI = "urn:3gpp:ns:mcpttGroupInfo:1.0"
CIMS_GI = "urn:cims:groupinfo:1.0"

# CIMS 그룹 문서의 3GPP 이름공간 비규격 요소 — MCVideo 계약 밖의 기존 편차(검증 전에 뗀다).
KNOWN_DEVIATIONS = {
    f"{{{GI}}}mcptt-video": "현행 «PTT 영상» 전환기 요소 — V7 에서 제거 (mcvideo.md §6 V0 ②)",
    f"{{{GI}}}on-network-require-affiliation": "CIMS 기존 요소(TS 24.481 스키마에 없음) — MCPTT 편차",
    f"{{{GI}}}on-network-require-talker-id": "CIMS 기존 요소(TS 24.481 스키마에 없음) — MCPTT 편차",
    f"{{{GI}}}on-network-encryption": "CIMS 기존 요소(TS 24.481 스키마에 없음) — MCPTT 편차",
    f"{{{GI}}}org-code": "CIMS 기존 요소(TS 24.481 스키마에 없음) — MCPTT 편차",
    f"{{{GI}}}authorized-user": "CIMS 기존 요소(TS 24.481 스키마에 없음) — MCPTT 편차",
}


def _load_xmlschema():
    try:
        import xmlschema  # noqa: F401
        return xmlschema
    except Exception:
        extra = os.environ.get("CIMS_PYLIB")
        if extra and extra not in sys.path:
            sys.path.insert(0, extra)
            try:
                import xmlschema  # noqa: F401
                return xmlschema
            except Exception:
                pass
    return None


def _schemas(xs):
    loc = [
        ("urn:oma:xml:poc:list-service", os.path.join(XSD, "aux-oma-list-service.xsd")),
        ("urn:ietf:params:xml:ns:resource-lists", os.path.join(XSD, "aux-resource-lists.xsd")),
        ("urn:ietf:params:xml:ns:common-policy", os.path.join(XSD, "aux-common-policy.xsd")),
        ("urn:oma:xml:xdm:extensions", os.path.join(XSD, "aux-oma-xdm-extensions.xsd")),
        (GI, os.path.join(XSD, "mcpttGroupInfo.xsd")),
        ("urn:3gpp:mcptt:mcpttUEinitConfig:1.0", os.path.join(XSD, "ue-init-config.xsd")),
        ("http://www.w3.org/2001/04/xmlenc#", os.path.join(XSD, "aux-xmlenc.xsd")),
        ("urn:3gpp:ns:mcvideoGKTP:1.0", os.path.join(XSD, "aux-mcvideoGKTP.xsd")),
        ("urn:3gpp:ns:mcvideoPresInfo:1.0", os.path.join(XSD, "mcvideoPresInfo.xsd")),
    ]

    def mk(path):
        return xs.XMLSchema(path, locations=loc, allow="local", defuse="always")

    return {
        "{urn:oma:xml:poc:list-service}group": mk(os.path.join(XSD, "aux-oma-list-service.xsd")),
        "{urn:3gpp:ns:mcvideo:user-profile:1.0}mcvideo-user-profile": mk(os.path.join(XSD, "mcvideo-user-profile.xsd")),
        "{urn:3gpp:ns:mcvideoServiceConfig:1.0}service-configuration-info":
            mk(os.path.join(XSD, "mcvideo-service-config.xsd")),
        "{urn:3gpp:mcptt:mcpttUEinitConfig:1.0}mcptt-UE-initial-configuration": mk(os.path.join(XSD, "ue-init-config.xsd")),
        "{urn:3gpp:ns:mcvideoInfo:1.0}mcvideoinfo": mk(os.path.join(XSD, "mcvideoinfo.xsd")),
    }


def _strip_known(root) -> list:
    removed = []
    for parent in root.iter():
        for ch in list(parent):
            if ch.tag in KNOWN_DEVIATIONS or ch.tag.startswith(f"{{{CIMS_GI}}}"):
                parent.remove(ch)
                removed.append(ch.tag)
    return removed


def _group_rules(root) -> list:
    """TS 24.481 §7.2.2·§7.2.8 의 스키마 밖 규칙 — MCVideo 그룹 문서면 enabler·group-media·entry 신원이 있어야 한다."""
    errs = []
    oxe = "{urn:oma:xml:xdm:extensions}"
    ls = root.find("{urn:oma:xml:poc:list-service}list-service")
    services = {s.get("enabler"): s for s in ls.iter(f"{oxe}service")}
    if "example.mcptt" in services:
        errs.append("MCPTT enabler 가 자리표시 값(example.mcptt) — MCPTT ICSI 여야 한다 (§7.2.2)")
    mv = services.get("urn:urn-7:3gpp-service.ims.icsi.mcvideo")
    if mv is not None:
        if mv.find(f"{oxe}group-media/{{{GI}}}mcvideo-video-media") is None:
            errs.append("MCVideo <service> 에 <group-media><mcvideo-video-media> 없음 (§7.2.2)")
        for e in ls.iter("{urn:oma:xml:poc:list-service}entry"):
            vid = e.find(f"{{{GI}}}mcvideo-mcvideo-id")
            if vid is None or not vid.get("uri"):
                errs.append(f"entry {e.get('uri')} 에 <mcvideo-mcvideo-id uri> 없음 (§7.2.2 MCVideo entry b)")
        for tag in ("mcvideo-protect-media", "mcvideo-protect-transmission-control"):
            n = ls.find(f"{{{GI}}}{tag}")
            if n is None or (n.text or "").strip() != "false":
                errs.append(f"<{tag}> 가 명시 false 가 아님 — 없으면 true 로 읽힌다 (§7.2.8, mcvideo.md §7 D7)")
    return errs


def check_xml_text(schemas, text: str, label: str) -> list:
    root = ET.fromstring(text)
    work = copy.deepcopy(root)
    removed = _strip_known(work)
    sch = schemas.get(work.tag)
    if sch is None:
        return [f"{label}: 알 수 없는 루트 {work.tag}"]
    errs = [f"{label}: {str(e).splitlines()[0]} @ {getattr(e, 'path', '')}"
            for e in sch.iter_errors(ET.ElementTree(work))]
    if work.tag == "{urn:oma:xml:poc:list-service}group":
        errs += [f"{label}: {m}" for m in _group_rules(root)]
    if removed:
        print(f"  {label}: 알려진 편차 {len(removed)}개 제외 ({', '.join(sorted({t.split('}')[1] for t in removed}))})")
    return errs


def _xml_bodies_of_sip(text: str):
    """SIP 골든 메시지의 XML 본문(multipart 포함)을 (Content-Type, 본문) 로."""
    for m in re.finditer(r"Content-Type:\s*([^\r\n;]+)[^\r\n]*\r?\n(?:[^\r\n]+\r?\n)*\r?\n(<\?xml.*?)(?=\r?\n--|\Z)",
                         text, flags=re.S | re.I):
        ctype, body = m.group(1).strip().lower(), m.group(2).strip()
        if ctype.endswith("+xml"):
            yield ctype, body


def main(argv) -> int:
    xs = _load_xmlschema()
    if xs is None:
        print("SKIP: xmlschema 가 없다 — pip install --target <dir> xmlschema 뒤 CIMS_PYLIB=<dir>")
        return 0
    schemas = _schemas(xs)
    files = argv[1:] or sorted(
        [os.path.join(FIX, f) for f in os.listdir(FIX) if f.endswith(".xml")] +
        ([os.path.join(FIX, "sip", f) for f in os.listdir(os.path.join(FIX, "sip")) if f.endswith(".txt")]
         if os.path.isdir(os.path.join(FIX, "sip")) else []))
    fails = 0
    for path in files:
        label = os.path.relpath(path, HERE)
        text = open(path, encoding="utf-8").read()
        errs = []
        if path.endswith(".txt"):
            for ctype, body in _xml_bodies_of_sip(text):
                if ctype in ("application/vnd.3gpp.mcvideo-info+xml",):
                    errs += check_xml_text(schemas, body, f"{label} [{ctype}]")
                elif ctype == "application/pidf+xml":
                    errs += _check_pidf(schemas, body, f"{label} [pidf]")
        else:
            errs = check_xml_text(schemas, text, label)
        print(("FAIL " if errs else "PASS ") + label)
        for e in errs:
            print("   - " + e)
        fails += bool(errs)
    print(f"{len(files) - fails}/{len(files)} PASS")
    return 1 if fails else 0


def _check_pidf(schemas, body: str, label: str) -> list:
    """pidf 본문의 mcvideoPresInfo 요소만 검증(pidf 틀 = RFC 3863 은 반입하지 않았다) — TS 24.281 §8.3.1."""
    root = ET.fromstring(body)
    ns = "urn:3gpp:ns:mcvideoPresInfo:1.0"
    import xmlschema
    sch = xmlschema.XMLSchema(os.path.join(XSD, "mcvideoPresInfo.xsd"))
    errs = []
    found = False
    for el in root.iter():
        if el.tag.startswith(f"{{{ns}}}") and el.tag.split('}')[1] in ("affiliation", "p-id", "status"):
            found = True
            errs += [f"{label}: {str(e).splitlines()[0]}" for e in sch.iter_errors(ET.ElementTree(copy.deepcopy(el)))]
    if not found:
        errs.append(f"{label}: mcvideoPresInfo 요소 없음")
    return errs


if __name__ == "__main__":
    sys.exit(main(sys.argv))
