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
        "{urn:3gpp:mcvideo:mcvideoUEConfig:1.0}mcvideo-UE-configuration": mk(os.path.join(XSD, "mcvideo-ue-config.xsd")),
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


ICSI = "urn:urn-7:3gpp-service.ims.icsi.mcvideo"
ICSI_ENC = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcvideo"


def parse_sip(data: bytes):
    """전송 바이트 → (시작 줄, [(이름, 값)], [(Content-Type, 본문)]). Content-Length 가 본문 바이트와 맞는지도 본다."""
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("utf-8").split("\r\n")
    hdrs = [tuple(x.strip() for x in ln.split(":", 1)) for ln in lines[1:]]
    get = lambda n: [v for k, v in hdrs if k.lower() == n.lower()]
    errs = []
    cl = get("Content-Length")
    if not cl or int(cl[0]) != len(body):
        errs.append(f"Content-Length {cl} ≠ 본문 {len(body)} 바이트")
    parts = []
    ctype = (get("Content-Type") or [""])[0]
    if ctype.lower().startswith("multipart/"):
        m = re.search(r'boundary="?([^";]+)"?', ctype)
        if not m:
            errs.append("multipart 에 boundary 없음")
        else:
            b = ("--" + m.group(1)).encode()
            for chunk in body.split(b)[1:]:
                if chunk.startswith(b"--"):
                    break
                ph, _, pb = chunk.lstrip(b"\r\n").partition(b"\r\n\r\n")
                pct = re.search(rb"Content-Type:\s*([^\r\n;]+)", ph, flags=re.I)
                parts.append(((pct.group(1).decode().strip().lower() if pct else ""), pb.rstrip(b"\r\n").decode("utf-8")))
    elif ctype:
        parts.append((ctype.split(";")[0].strip().lower(), body.decode("utf-8")))
    return lines[0], hdrs, parts, errs


def _sdp_media(sdp: str):
    """SDP → [(media, port, proto, fmt, [속성])]."""
    out = []
    for ln in sdp.splitlines():
        if ln.startswith("m="):
            f = ln[2:].split()
            out.append([f[0], int(f[1]), f[2], f[3:], []])
        elif out:
            out[-1][4].append(ln)
    return out


def _fmtp_params(media) -> list:
    for a in media[4]:
        if a.startswith("a=fmtp:MCVideo "):
            return [x.split("=", 1)[0] for x in a.split(" ", 1)[1].split(";") if x]
    return []


def sip_rules(name, start, hdrs, parts) -> list:
    """K3·K4 규칙 — mcvideo.md §1.4 «CIMS SDP 프로파일» · TS 24.281 §9.2.1.2.1.1·§9.2.2.2.1.1·§6.3.3.1.2."""
    errs = []
    get = lambda n: [v for k, v in hdrs if k.lower() == n.lower()]
    method = start.split()[0]
    is_req = not start.startswith("SIP/2.0")
    sdps = [b for c, b in parts if c == "application/sdp"]
    if is_req and method == "INVITE":
        from_ue = not start.startswith(f"INVITE sip:+")
        if from_ue:
            if get("P-Preferred-Service") != [ICSI]:
                errs.append("P-Preferred-Service ≠ MCVideo ICSI (§9.2.2.2.1.1 5))")
        else:
            if get("P-Asserted-Service") != [ICSI]:
                errs.append("P-Asserted-Service ≠ MCVideo ICSI (§6.3.3.1.2 3))")
            if "isfocus" not in (get("Contact") or [""])[0]:
                errs.append("제어 기능 Contact 에 isfocus 없음 (§6.3.3.1.2 1))")
        ac = " ".join(get("Accept-Contact"))
        if "+g.3gpp.mcvideo;require;explicit" not in ac or f'+g.3gpp.icsi-ref="{ICSI_ENC}";require;explicit' not in ac:
            errs.append("Accept-Contact 두 가지(g.3gpp.mcvideo · icsi-ref mcvideo, require;explicit)가 아님")
        if "+g.3gpp.mcvideo" not in (get("Contact") or [""])[0]:
            errs.append("Contact 에 +g.3gpp.mcvideo 없음")
        if not any(c == "application/vnd.3gpp.mcvideo-info+xml" for c, _ in parts):
            errs.append("mcvideo-info 본문 없음")
    if method == "REGISTER":
        c = (get("Contact") or [""])[0]
        if "+g.3gpp.mcvideo" not in c or ICSI_ENC not in c:
            errs.append("REGISTER Contact 에 MCVideo 특성 태그·icsi-ref 없음 (§7.2.1)")
    if method == "PUBLISH":
        if get("P-Preferred-Service") != [ICSI] or get("Event") != ["presence"]:
            errs.append("affiliation PUBLISH 헤더(P-Preferred-Service·Event presence)가 아님 (§8.2.1.2)")
    for body in sdps:
        media = _sdp_media(body)
        if [m[0] for m in media] != ["audio", "video", "application"]:
            errs.append(f"m-line 순서 {[m[0] for m in media]} ≠ audio,video,application (K4)")
            continue
        app = media[2]
        if app[2] != "udp" or app[3] != ["MCVideo"]:
            errs.append(f"제어 채널 m-line 이 'udp MCVideo' 가 아님 ({app[2]} {app[3]})")
        if "i=audio component of MCVideo" not in media[0][4] or "i=video component of MCVideo" not in media[1][4]:
            errs.append("i= 성분 표시 없음 (§6.2.1 2)c)·3)d))")
        fl = [a for a in app[4] if a.startswith("a=fmtp:MCVideo ")]
        if not fl or ":" in fl[0].split(" ", 1)[1] or "mc_transmission_ssrc" not in _fmtp_params(app):
            errs.append("fmtp:MCVideo 가 ';' 구분·mc_transmission_ssrc 포함이 아님 (K4)")
    return errs


def _check_offer_answer(files) -> list:
    """answer 는 offer 에 없던 fmtp 파라미터를 더하지 않는다(TS 24.581 §14.3.1) · m 수·순서 같음(RFC 3264 §6)."""
    errs = []
    for offer_n, answer_n in (("03_chat_join_invite.txt", "04_chat_join_200.txt"),
                              ("05_prearranged_initiate_invite.txt", "06_prearranged_initiate_200.txt")):
        po, pa = files.get(offer_n), files.get(answer_n)
        if not po or not pa:
            continue
        so = [b for c, b in po[2] if c == "application/sdp"][0]
        sa = [b for c, b in pa[2] if c == "application/sdp"][0]
        mo, ma = _sdp_media(so), _sdp_media(sa)
        if [m[0] for m in mo] != [m[0] for m in ma]:
            errs.append(f"{answer_n}: m-line 수·순서가 offer 와 다르다")
            continue
        # mc_audio_ssrc·mc_video_ssrc 는 answer 전용 값이다 — offer 에 없어도 암묵 요청을 받아들인 answer 가 싣고 offerer 가 쓴다
        #   (§12.1.2.2·§14.3.7·§14.3.8·§14.4 — §14.3.1 일반 규칙의 예외, mcvideo.md §9).
        extra = set(_fmtp_params(ma[2])) - set(_fmtp_params(mo[2])) - {"mc_audio_ssrc", "mc_video_ssrc"}
        if extra:
            errs.append(f"{answer_n}: offer 에 없던 fmtp 파라미터 {sorted(extra)} (§14.3.1)")
        if "mc_implicit_request" in _fmtp_params(ma[2]) and not {"mc_audio_ssrc", "mc_video_ssrc"} <= set(_fmtp_params(ma[2])):
            errs.append(f"{answer_n}: 암묵 요청 수락 answer 에 mc_audio_ssrc·mc_video_ssrc 없음 (§14.3.7·§14.3.8)")
    return errs


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
    sip_parsed = {}
    for path in files:
        label = os.path.relpath(path, HERE)
        errs = []
        if path.endswith(".txt"):
            start, hdrs, parts, errs = parse_sip(open(path, "rb").read())
            sip_parsed[os.path.basename(path)] = (start, hdrs, parts)
            errs += [f"{label}: {e}" for e in sip_rules(os.path.basename(path), start, hdrs, parts)]
            for ctype, body in parts:
                if ctype == "application/vnd.3gpp.mcvideo-info+xml":
                    errs += check_xml_text(schemas, body, f"{label} [{ctype}]")
                elif ctype == "application/pidf+xml":
                    errs += _check_pidf(schemas, body, f"{label} [pidf]")
        else:
            errs = check_xml_text(schemas, open(path, encoding="utf-8").read(), label)
        print(("FAIL " if errs else "PASS ") + label)
        for e in errs:
            print("   - " + e)
        fails += bool(errs)
    oa = _check_offer_answer(sip_parsed)
    for e in oa:
        print("FAIL offer/answer — " + e)
    build = os.path.join(FIX, "sip", "build_goldens.py")
    if not argv[1:] and os.path.isfile(build):
        import subprocess
        r = subprocess.run([sys.executable, build, "--check"], capture_output=True, text=True)
        print(("PASS " if r.returncode == 0 else "FAIL ") + r.stdout.strip())
        fails += r.returncode != 0
    fails += bool(oa)
    print(f"{len(files) - fails}/{len(files)} PASS" if not fails else f"FAIL {fails}")
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
