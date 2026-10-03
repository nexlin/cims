#!/usr/bin/env python3
"""MCPTT·MCData 요청 형식 계약 — 규격형 SIP 요청 골든 조립 (docs/dev/conformance_gap_plan.md §7 «요청 형식 계약», WP S17).

아래 build() 가 정본이고 같은 디렉터리의 *.txt 는 이 스크립트가 낸 **전송 바이트 그대로**(CRLF · 정확한 Content-Length)다.
본문을 바꾸려면 여기서 고치고 다시 낸다 — .txt 를 손으로 고치면 Content-Length 가 어긋난다.

  python3 tests/fixtures/mcptt/sip/build_goldens.py            # *.txt 생성
  python3 tests/fixtures/mcptt/sip/build_goldens.py --check    # *.txt 가 최신인지(S1-MCX-REQUEST-CONTRACT 가 부른다)

시나리오 = README.md(같은 디렉터리). 규격 = TS 24.379 V20.0.0(§11.1.1.2.1.1 개별 호 개시 · §11.1.2.2 floor 없는 개별 호 ·
§11.1.1.3.1.1 8)·9) 145 · §6.2.1 SDP · §4.4 Warning · Annex F.1 mcptt-info) · TS 24.380 V20.0.0(표 4.3.3.1-1 · §14 fmtp) ·
TS 24.282 V19.8.0(§6.2.4.1 · §9.2.2.2.1 SDS · §10.2.4.2.1 FD · §9.2.3.2.1·§9.2.3.2.3 미디어 평면 SDS · §9.2.2.4.2 5) 204 ·
§15 메시지 · Annex D mcdata-info).
"""
from __future__ import annotations

import base64
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

DOMAIN = "ptt.cims.example.kr"
CSP = "csp.ptt.cims.example.kr"
MCPTT_PSI = f"sip:mcptt_psi@{DOMAIN}"
MCDATA_PSI = f"sip:mcdata_psi@{DOMAIN}"
ICSI_MCPTT = "urn:urn-7:3gpp-service.ims.icsi.mcptt"
ICSI_MCPTT_ENC = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt"
ICSI_SDS = "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds"
ICSI_SDS_ENC = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds"
ICSI_FD = "urn:urn-7:3gpp-service.ims.icsi.mcdata.fd"
ICSI_FD_ENC = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.fd"
UE_A, UE_B = "+82510002001", "+82510002002"
IP_A = "10.10.1.21"
CLIENT_A = "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f"
GROUP = "tel:g101"
FD_URL = f"https://csc.{DOMAIN}:4430/mcdata/fd/0123456789abcdef0123456789abcdef"

MCPTT_TAGS = f'+g.3gpp.mcptt;+g.3gpp.icsi-ref="{ICSI_MCPTT_ENC}"'
ACCEPT_MCPTT = ["Accept-Contact: *;+g.3gpp.mcptt;require;explicit",
                f'Accept-Contact: *;+g.3gpp.icsi-ref="{ICSI_MCPTT_ENC}";require;explicit']
ACCEPT_SDS = ["Accept-Contact: *;+g.3gpp.mcdata.sds;require;explicit",
              f'Accept-Contact: *;+g.3gpp.icsi-ref="{ICSI_SDS_ENC}";require;explicit']
ACCEPT_FD = ["Accept-Contact: *;+g.3gpp.mcdata.fd;require;explicit",
             f'Accept-Contact: *;+g.3gpp.icsi-ref="{ICSI_FD_ENC}";require;explicit']

# MCData 메시지 고정값 — Date and time(§15.2.8, 5 octet 초) · Conversation ID · Message ID(§15.2.9·§15.2.10, UUID 16 octet)
SENT_SEC = 1790775600
CONV_GROUP = "3f2a1b0c4d5e4f60a1b2c3d4e5f60718"
CONV_1TO1 = "7c6b5a49384736a5b4c3d2e1f0091827"
MSG_IDS = {"sds-g": "a1000000000040008000000000000001", "sds-1": "a1000000000040008000000000000002",
           "fd-g": "a1000000000040008000000000000003", "fd-1": "a1000000000040008000000000000004",
           "sds-old": "a1000000000040008000000000000005"}


def multipart(boundary, parts):
    """parts = (Content-Type, 본문[, Content-Transfer-Encoding])"""
    out = ""
    for part in parts:
        ctype, body = part[0], part[1]
        cte = f"Content-Transfer-Encoding: {part[2]}\n" if len(part) > 2 else ""
        out += f"--{boundary}\nContent-Type: {ctype}\n{cte}\n{body}"
    return out + f"--{boundary}--\n"


def message(start, headers, ctype=None, body=""):
    """본문 LF → CRLF, Content-Length = CRLF 바이트 수."""
    body_crlf = body.replace("\n", "\r\n")
    hs = list(headers)
    if ctype:
        hs.append(f"Content-Type: {ctype}")
    hs.append(f"Content-Length: {len(body_crlf.encode('utf-8'))}")
    return (start + "\r\n" + "\r\n".join(hs) + "\r\n\r\n" + body_crlf).encode("utf-8")


def via(branch):
    return f"Via: SIP/2.0/TLS {IP_A}:50601;branch=z9hG4bK{branch};rport"


def via_resp(branch):
    return via(branch).replace(";rport", f";rport=50601;received={IP_A}")


def ue_contact(tags):
    return f"Contact: <sip:{UE_A}@{IP_A}:50601;transport=tls>;{tags}"


def mcptt_info(*params):
    """Annex F.1 — 루트 <mcpttinfo>, contentType 자식은 mcpttURI/mcpttString + type="Normal"."""
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<mcpttinfo xmlns="urn:3gpp:ns:mcpttInfo:1.0">\n  <mcptt-Params>\n'
            + "".join(f"    {p}\n" for p in params) + "  </mcptt-Params>\n</mcpttinfo>\n")


def mcdata_info(*params):
    """Annex D.2 — 루트 <mcdatainfo>, contentType 자식은 mcdataURI/mcdataString + type="Normal"."""
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0">\n  <mcdata-Params>\n'
            + "".join(f"    {p}\n" for p in params) + "  </mcdata-Params>\n</mcdatainfo>\n")


def mcdata_uri(tag, v):
    return f'<{tag} type="Normal"><mcdataURI>{v}</mcdataURI></{tag}>'


def mcdata_string(tag, v):
    return f'<{tag} type="Normal"><mcdataString>{v}</mcdataString></{tag}>'


def resource_lists(*uris):
    """RFC 4826 · RFC 5366 — 대상 하나마다 <entry uri>."""
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<resource-lists xmlns="urn:ietf:params:xml:ns:resource-lists">\n'
            "  <list>\n" + "".join(f'    <entry uri="{u}"/>\n' for u in uris) + "  </list>\n</resource-lists>\n")


def mcptt_sdp(floor):
    """TS 24.379 §6.2.1 — 음성 m-line(i=speech) + 발언권 제어 채널 `m=application <port> udp MCPTT`(TS 24.380 표 4.3.3.1-1,
    §14.2 fmtp). floor=False 면 floor 없는 개별 호 — 발언권 제어 채널을 싣지 않는다(TS 24.379 §11.1.2.2 1))."""
    lines = ["v=0", f"o=- 3900001001 3900001001 IN IP4 {IP_A}", "s=-", f"c=IN IP4 {IP_A}", "t=0 0",
             "m=audio 40100 RTP/AVP 96 101", "i=speech", "a=rtpmap:96 AMR-WB/16000", "a=fmtp:96 octet-align=1",
             "a=rtpmap:101 telephone-event/16000", "a=fmtp:101 0-15", "a=sendrecv"]
    if floor:
        lines += ["m=application 40102 udp MCPTT", "a=fmtp:MCPTT mc_queueing;mc_priority=5"]
    return "\n".join(lines) + "\n"


def _datetime(sec):
    return sec.to_bytes(5, "big")


def sds_signalling(conv, msg, delivery=True):
    """§15.1.2 SDS SIGNALLING PAYLOAD — type 0x01 · Date and time · Conversation ID · Message ID · SDS disposition request type
    (TV 1, IEI 8-: delivery = 1)."""
    b = bytes([0x01]) + _datetime(SENT_SEC) + bytes.fromhex(conv) + bytes.fromhex(msg)
    if delivery:
        b += bytes([0x81])
    return b


def data_payload(text):
    """§15.1.4 DATA PAYLOAD — type 0x03 · Number of payloads 1 · Payload(TLV-E 0x78: content type TEXT 0x01 + UTF-8)."""
    t = text.encode("utf-8")
    n = 1 + len(t)
    return bytes([0x03, 0x01, 0x78, (n >> 8) & 0xFF, n & 0xFF, 0x01]) + t


def fd_signalling(conv, msg):
    """§15.1.3 FD SIGNALLING PAYLOAD — type 0x02 · Date and time · Conversation ID · Message ID · Payload(TLV-E 0x78: FILEURL 0x04 +
    URL) · Metadata(TLV-E 0x79: file-selector RFC 5547)."""
    url = FD_URL.encode()
    meta = b'name:"site-map.png" size:48213 type:image/png'
    b = bytes([0x02]) + _datetime(SENT_SEC) + bytes.fromhex(conv) + bytes.fromhex(msg)
    b += bytes([0x78, ((1 + len(url)) >> 8) & 0xFF, (1 + len(url)) & 0xFF, 0x04]) + url
    b += bytes([0x79, (len(meta) >> 8) & 0xFF, len(meta) & 0xFF]) + meta
    return b


def b64(b):
    """CIMS 와이어 형식 — 이진 파트를 base64 전송 인코딩으로 싣는다(mcdata_messaging.md «편차» — PJSIP 문자열 본문)."""
    return base64.b64encode(b).decode() + "\n"


def mcdata_message(name_branch, call_id, accept, icsi, boundary, parts):
    return message(
        f"MESSAGE {MCDATA_PSI} SIP/2.0",
        [via(name_branch), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag={call_id}-f", f"To: <{MCDATA_PSI}>",
         f"Call-ID: {call_id}@{IP_A}", "CSeq: 1 MESSAGE", *accept, f"P-Preferred-Service: {icsi}"],
        f"multipart/mixed;boundary={boundary}", multipart(boundary, parts))


def private_invite(branch, call_id, floor):
    return message(
        f"INVITE {MCPTT_PSI} SIP/2.0",
        [via(branch), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag={call_id}-f", f"To: <{MCPTT_PSI}>",
         f"Call-ID: {call_id}@{IP_A}", "CSeq: 1 INVITE", ue_contact(MCPTT_TAGS), *ACCEPT_MCPTT,
         f"P-Preferred-Service: {ICSI_MCPTT}", "Answer-Mode: Manual", "Supported: timer", "Session-Expires: 1800"],
        f"multipart/mixed;boundary={call_id}",
        multipart(call_id, [
            ("application/sdp", mcptt_sdp(floor)),
            ("application/vnd.3gpp.mcptt-info+xml", mcptt_info("<session-type>private</session-type>")),
            ("application/resource-lists+xml", resource_lists(f"tel:{UE_B}"))]))


def build():
    msgs = {}

    # 01 — 개별 호 개시(floor 있음, TS 24.379 §11.1.1.2.1.1): Request-URI = 참여 기능 PSI(1)), Contact 특성 태그(5)), Accept-Contact 둘(6)·8)),
    #   P-Preferred-Service(7)), 착신자 = resource-lists 의 entry 하나(9)), SDP offer 에 발언권 제어 채널(12)), Answer-Mode(14)b)ii)),
    #   mcptt-info session-type private(14)c)i)).
    msgs["01_private_invite.txt"] = private_invite("-prv-inv1", "prv-a1", True)

    # 02 — floor 없는 개별 호(§11.1.2.2 1)): 01 과 같고 SDP offer 에 발언권 제어 채널(m=application)이 없다 — 참여·제어 기능과 착신 단말은
    #   이것으로 floor 없는 호를 안다(§11.1.2.3.1·§11.1.2.2 끝 문단). fmtp mc_no_floor_ctrl 은 쓰지 않는다(pre-established 용, TS 24.380 §14.2.6).
    msgs["02_private_full_duplex_invite.txt"] = private_invite("-prv-inv2", "prv-a2", False)

    # 03 — 착신자를 정하지 못한 개별 호(§11.1.1.3.1.1 8)·9) — resource-lists 없음·entry 둘 이상): 403 + Warning 145(§4.4).
    msgs["03_private_reject_403_145.txt"] = message(
        "SIP/2.0 403 Forbidden",
        [via_resp("-prv-inv3"), f"From: <sip:{UE_A}@{DOMAIN}>;tag=prv-a3-f", f"To: <{MCPTT_PSI}>;tag=csp-e145",
         f"Call-ID: prv-a3@{IP_A}", "CSeq: 1 INVITE",
         f'Warning: 399 {DOMAIN} "145 unable to determine called party"'])

    # 04 — 그룹 SDS(TS 24.282 §9.2.2.2.1 3)): Request-URI = 참여 MCData 기능 PSI(§6.2.4.1 4)), Accept-Contact 둘·P-Preferred-Service
    #   (§6.2.4.1 1)), mcdata-info request-type group-sds · <mcdata-request-uri> = 그룹 · <mcdata-client-id>, signalling·payload.
    msgs["04_sds_group_message.txt"] = mcdata_message(
        "-sds-g1", "sds-g1", ACCEPT_SDS, ICSI_SDS, "sds-g1", [
            ("application/vnd.3gpp.mcdata-info+xml",
             mcdata_info("<request-type>group-sds</request-type>", mcdata_uri("mcdata-request-uri", GROUP),
                         mcdata_string("mcdata-client-id", CLIENT_A))),
            ("application/vnd.3gpp.mcdata-signalling", b64(sds_signalling(CONV_GROUP, MSG_IDS["sds-g"])), "base64"),
            ("application/vnd.3gpp.mcdata-payload", b64(data_payload("현장 도착")), "base64")])

    # 05 — 1:1 SDS(§9.2.2.2.1 2)): 대상 = resource-lists 의 entry 하나(2)a)), mcdata-info request-type one-to-one-sds(2)b)i)).
    msgs["05_sds_one_to_one_message.txt"] = mcdata_message(
        "-sds-o1", "sds-o1", ACCEPT_SDS, ICSI_SDS, "sds-o1", [
            ("application/vnd.3gpp.mcdata-info+xml", mcdata_info("<request-type>one-to-one-sds</request-type>")),
            ("application/resource-lists+xml", resource_lists(f"tel:{UE_B}")),
            ("application/vnd.3gpp.mcdata-signalling", b64(sds_signalling(CONV_1TO1, MSG_IDS["sds-1"])), "base64"),
            ("application/vnd.3gpp.mcdata-payload", b64(data_payload("확인 바람")), "base64")])

    # 06 — 1:1 SDS 인데 resource-lists 가 없다(대상을 Request-URI·<mcdata-request-uri> 에 싣는 옛 형식): 403 + Warning 204(§9.2.2.4.2 5)b)i)).
    msgs["06_sds_reject_403_204.txt"] = message(
        "SIP/2.0 403 Forbidden",
        [via_resp("-sds-o2"), f"From: <sip:{UE_A}@{DOMAIN}>;tag=sds-o2-f", f"To: <{MCDATA_PSI}>;tag=csp-e204",
         f"Call-ID: sds-o2@{IP_A}", "CSeq: 1 MESSAGE",
         f'Warning: 399 {DOMAIN} "204 unable to determine targeted user for one-to-one SDS"'])

    # 07 — 그룹 FD(신호 평면, TS 24.282 §10.2.4.2.1): FD 특성 태그·ICSI(§6.2.4.1 2)), request-type group-fd, FD SIGNALLING(FILEURL + Metadata).
    msgs["07_fd_group_message.txt"] = mcdata_message(
        "-fd-g1", "fd-g1", ACCEPT_FD, ICSI_FD, "fd-g1", [
            ("application/vnd.3gpp.mcdata-info+xml",
             mcdata_info("<request-type>group-fd</request-type>", mcdata_uri("mcdata-request-uri", GROUP),
                         mcdata_string("mcdata-client-id", CLIENT_A))),
            ("application/vnd.3gpp.mcdata-signalling", b64(fd_signalling(CONV_GROUP, MSG_IDS["fd-g"])), "base64")])

    # 08 — 1:1 FD: request-type one-to-one-fd + resource-lists(§10.2.4.4.2 10)) — 없으면 403 + 205.
    msgs["08_fd_one_to_one_message.txt"] = mcdata_message(
        "-fd-o1", "fd-o1", ACCEPT_FD, ICSI_FD, "fd-o1", [
            ("application/vnd.3gpp.mcdata-info+xml", mcdata_info("<request-type>one-to-one-fd</request-type>")),
            ("application/resource-lists+xml", resource_lists(f"tel:{UE_B}")),
            ("application/vnd.3gpp.mcdata-signalling", b64(fd_signalling(CONV_1TO1, MSG_IDS["fd-1"])), "base64")])

    # 09 — 미디어 평면 그룹 SDS(§9.2.3.2.3): Request-URI = PSI, Contact 특성 태그(1)), Accept-Contact 둘(2)·3)), P-Preferred-Service(4)),
    #   timer(5)·6)), mcdata-info group-sds·그룹·client ID, SDP = m=message TCP/MSRP(§9.2.3.2.1 — sendonly · path · accept-types · actpass).
    msrp_sdp = "\n".join([
        "v=0", f"o=- 3900001009 3900001009 IN IP4 {IP_A}", "s=-", f"c=IN IP4 {IP_A}", "t=0 0",
        "m=message 40200 TCP/MSRP *", "a=sendonly", f"a=path:msrp://{IP_A}:40200/s1a2b3c4;tcp",
        "a=accept-types:application/vnd.3gpp.mcdata-signalling application/vnd.3gpp.mcdata-payload",
        "a=setup:actpass"]) + "\n"
    msgs["09_sds_media_group_invite.txt"] = message(
        f"INVITE {MCDATA_PSI} SIP/2.0",
        [via("-sdsm-g1"), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag=sdsm-g1-f", f"To: <{MCDATA_PSI}>",
         f"Call-ID: sdsm-g1@{IP_A}", "CSeq: 1 INVITE",
         ue_contact(f'+g.3gpp.mcdata.sds;+g.3gpp.icsi-ref="{ICSI_SDS_ENC}"'), *ACCEPT_SDS,
         f"P-Preferred-Service: {ICSI_SDS}", "Supported: timer", "Session-Expires: 1800"],
        "multipart/mixed;boundary=sdsm-g1",
        multipart("sdsm-g1", [
            ("application/sdp", msrp_sdp),
            ("application/vnd.3gpp.mcdata-info+xml",
             mcdata_info("<request-type>group-sds</request-type>", mcdata_uri("mcdata-request-uri", GROUP),
                         mcdata_string("mcdata-client-id", CLIENT_A)))]))
    return msgs


def main(argv):
    msgs = build()
    if "--check" in argv:
        bad = []
        for name, data in msgs.items():
            path = os.path.join(HERE, name)
            if not os.path.exists(path) or open(path, "rb").read() != data:
                bad.append(name)
        stale = sorted(f for f in os.listdir(HERE) if f.endswith(".txt") and f not in msgs)
        for n in bad:
            print(f"FAIL: {n} 가 build_goldens.py 와 다르다 — 다시 낸다")
        for n in stale:
            print(f"FAIL: {n} 는 build_goldens.py 에 없다")
        if not bad and not stale:
            print(f"sip goldens OK — {len(msgs)}개 최신")
        return 1 if (bad or stale) else 0
    for name, data in msgs.items():
        with open(os.path.join(HERE, name), "wb") as f:
            f.write(data)
        print("wrote", name)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
