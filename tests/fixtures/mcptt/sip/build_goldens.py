#!/usr/bin/env python3
"""MCPTT·MCData 요청 형식 계약 — 규격형 SIP 요청 골든 조립 (docs/dev/conformance_gap_plan.md §7 «요청 형식 계약», WP S17).

아래 build() 가 정본이고 같은 디렉터리의 *.txt 는 이 스크립트가 낸 **전송 바이트 그대로**(CRLF · 정확한 Content-Length)다.
본문을 바꾸려면 여기서 고치고 다시 낸다 — .txt 를 손으로 고치면 Content-Length 가 어긋난다.

  python3 tests/fixtures/mcptt/sip/build_goldens.py            # *.txt 생성
  python3 tests/fixtures/mcptt/sip/build_goldens.py --check    # *.txt 가 최신인지(S1-MCX-REQUEST-CONTRACT 가 부른다)

시나리오 = README.md(같은 디렉터리). 규격 = TS 24.379 V20.0.0(§11.1.1.2.1.1 개별 호 개시 · §11.1.2.2 floor 없는 개별 호 ·
§11.1.1.3.1.1 8)·9) 145 · §10.1.3 conference 구독 · §10.1.1.2.4.1·§10.1.1.4.5.1 재합류 · §7.2·§7.3 서비스 인가·설정(poc-settings) · §6.2.1 SDP · §4.4 Warning · Annex F.1 mcptt-info) · TS 24.380 V20.0.0(표 4.3.3.1-1 · §14 fmtp) ·
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
# IdMS 접근 토큰(TS 33.180 B.2.2 — JWS compact, 값은 자리표시. 서버는 IdMS 에 introspection 으로 검증한다 — 형식을 읽지 않는다)
ACCESS_TOKEN = "eyJhbGciOiJSUzI1NiIsImtpZCI6ImNpbXMifQ.eyJtY3B0dF9pZCI6IiI4MjUxMDAwMjAwMSJ9.c2lnbmF0dXJl"
# MCPTT 세션 식별자(TS 24.379 §4.5 — 개시 200 OK·멤버 INVITE Contact). To 에는 port·transport 를 두지 않는다(RFC 3261 §19.1.1 표 1)
SESSION_ID = f"sip:g101@{CSP}:5061;transport=tls;gr=1790775600123456-7"
SESSION_ID_TO = f"sip:g101@{CSP};gr=1790775600123456-7"

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
    # 10 — conference 구독(TS 24.379 §10.1.3.2): Request-URI = 진행 중 세션 식별자(개시 200 OK Contact — 그룹 AoR + gr, 2)),
    #   P-Preferred-Service(3)), Accept-Contact icsi-ref(4)), Expires 4294967295(5)), Accept conference-info(7)), mcptt-info
    #   <mcptt-request-uri> = 그룹(8)). 구독자는 그 세션의 참가자(§10.1.3.4.1 1)a)i)). 200 OK·NOTIFY Contact = 같은 세션 식별자.
    msgs["10_conference_subscribe.txt"] = message(
        f"SUBSCRIBE {SESSION_ID} SIP/2.0",
        [via("-conf-s1"), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag=conf-a1-f", f"To: <{SESSION_ID_TO}>",
         f"Call-ID: conf-a1@{IP_A}", "CSeq: 1 SUBSCRIBE", ue_contact(MCPTT_TAGS),
         f'Accept-Contact: *;+g.3gpp.icsi-ref="{ICSI_MCPTT_ENC}";require;explicit',
         f"P-Preferred-Service: {ICSI_MCPTT}", "Event: conference", "Expires: 4294967295",
         "Accept: application/conference-info+xml"],
        "application/vnd.3gpp.mcptt-info+xml",
        mcptt_info(f'<mcptt-request-uri type="Normal"><mcpttURI>{GROUP}</mcpttURI></mcptt-request-uri>'))

    # 11 — 진행 중 세션으로 풀리지 않는 conference 구독(그룹 URI 만·끝난 세션의 gr): 404 + Warning 137(§10.1.3.3 2)).
    msgs["11_conference_reject_404_137.txt"] = message(
        "SIP/2.0 404 Not Found",
        [via_resp("-conf-s2"), f"From: <sip:{UE_A}@{DOMAIN}>;tag=conf-a2-f", f"To: <sip:g101@{DOMAIN}>;tag=csp-e137",
         f"Call-ID: conf-a2@{IP_A}", "CSeq: 1 SUBSCRIBE",
         f'Warning: 399 {DOMAIN} "137 the indicated group call does not exist"'])

    # 12 — 재합류(TS 24.379 §10.1.1.2.4.1): 편성 그룹 호 개시(§10.1.1.2.1.1)와 같고 Request-URI 만 진행 중 세션 식별자(10)의
    #   clarification — 개시 200 OK·멤버 INVITE Contact 의 URI 그대로)다. Contact 특성 태그(4)), Accept-Contact 둘(5)·7)),
    #   P-Preferred-Service(6)), timer(8)·9)), mcptt-info session-type prearranged · <mcptt-request-uri> = 그룹 · client ID(14)),
    #   SDP offer(15)). 제어 기능은 세션 식별자에 연결된 그룹으로 처리한다(§10.1.1.4.5.1).
    msgs["12_rejoin_invite.txt"] = message(
        f"INVITE {SESSION_ID} SIP/2.0",
        [via("-rejoin-i1"), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag=rejoin-a1-f", f"To: <{SESSION_ID_TO}>",
         f"Call-ID: rejoin-a1@{IP_A}", "CSeq: 1 INVITE", ue_contact(MCPTT_TAGS), *ACCEPT_MCPTT,
         f"P-Preferred-Service: {ICSI_MCPTT}", "Supported: timer", "Session-Expires: 1800"],
        "multipart/mixed;boundary=rejoin-a1",
        multipart("rejoin-a1", [
            ("application/sdp", mcptt_sdp(True)),
            ("application/vnd.3gpp.mcptt-info+xml",
             mcptt_info("<session-type>prearranged</session-type>",
                        f'<mcptt-request-uri type="Normal"><mcpttURI>{GROUP}</mcpttURI></mcptt-request-uri>',
                        f'<mcptt-client-id type="Normal"><mcpttString>{CLIENT_A}</mcpttString></mcptt-client-id>'))]))

    # 13 — 세션 식별자가 가리키는 그룹 호가 없는 재합류(끝난 세션의 gr · <mcptt-request-uri> 가 다른 그룹): 404, Warning 없음
    #   (§10.1.1.4.5.1 2)). 새 세션을 열지 않는다 — 단말은 그룹 호 개시(§10.1.1.2.1.1)로 다시 건다.
    msgs["13_rejoin_reject_404.txt"] = message(
        "SIP/2.0 404 Not Found",
        [via_resp("-rejoin-i2"), f"From: <sip:{UE_A}@{DOMAIN}>;tag=rejoin-a2-f", f"To: <{SESSION_ID_TO}>;tag=csp-r404",
         f"Call-ID: rejoin-a2@{IP_A}", "CSeq: 1 INVITE"])

    # 14~19 — 서비스 인가·서비스 설정 (TS 24.379 §7.2·§7.3, RFC 3903 · RFC 4354 · §7.4.1.2.2). 공통(§7.2.1A) = Request-URI 참여 기능 PSI ·
    #   P-Preferred-Service MCPTT ICSI · Event poc-settings · Expires 4294967295(설정 제거·로그오프 = 0). poc-settings 의 entity id = 클라이언트의
    #   Instance ID URN(NOTE 2), Answer-Mode = <am-settings><answer-mode>automatic|manual · 확장 요소는 mcs10Set 이름공간(표 7.4.1.2.2-2).
    pub_hdr = lambda branch, cid, cseq, expires, extra=(): (
        [via(branch), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag={cid}-f", f"To: <sip:{UE_A}@{DOMAIN}>",
         f"Call-ID: {cid}@{IP_A}", f"CSeq: {cseq} PUBLISH", f"P-Preferred-Service: {ICSI_MCPTT}", "Event: poc-settings",
         f"Expires: {expires}", *extra])
    poc = ('<?xml version="1.0" encoding="UTF-8"?>\n<poc-settings xmlns="urn:oma:params:xml:ns:poc:poc-settings" '
           'xmlns:mcs10Set="urn:3gpp:mcsSettings:1.0">\n'
           f'  <entity id="{CLIENT_A}">\n'
           '    <am-settings><answer-mode>manual</answer-mode></am-settings>\n'
           '    <mcs10Set:selected-user-profile-index><mcs10Set:user-profile-index>1</mcs10Set:user-profile-index>'
           '</mcs10Set:selected-user-profile-index>\n'
           '    <mcs10Set:multiplex-support>false</mcs10Set:multiplex-support>\n'
           '  </entity>\n</poc-settings>\n')
    client_id = f'<mcptt-client-id type="Normal"><mcpttString>{CLIENT_A}</mcpttString></mcptt-client-id>'
    # 14 — 서비스 인가 + 서비스 설정(§7.2.2): mcptt-info <mcptt-access-token>(인증에서 받은 접근 토큰) · <mcptt-client-id> + poc-settings.
    #   서버(§7.3.3) = 토큰 검증 → MCPTT ID 를 IMPU 에 결박(실패 403 101) → 설정 캐시 → 200(SIP-ETag, 바인딩 둘 이상이면 multiple-devices-ind).
    msgs["14_poc_settings_publish_auth.txt"] = message(
        f"PUBLISH {MCPTT_PSI} SIP/2.0", pub_hdr("-poc-p1", "poc-a1", 1, 4294967295),
        "multipart/mixed;boundary=poc-a1",
        multipart("poc-a1", [
            ("application/vnd.3gpp.mcptt-info+xml",
             mcptt_info(f'<mcptt-access-token type="Normal"><mcpttString>{ACCESS_TOKEN}</mcpttString></mcptt-access-token>',
                        client_id)),
            ("application/poc-settings+xml", poc)]))
    # 15 — 서비스 설정만(§7.2.3): mcptt-info <mcptt-request-uri> = 자기 MCPTT ID · <mcptt-client-id> + poc-settings. 서버(§7.3.4) = 그 IMPU 와
    #   MCPTT ID 의 바인딩이 있어야 한다(없으면 404 141). SIP-If-Match = 14 의 200 OK 가 준 SIP-ETag(RFC 3903 §4.4 갱신).
    msgs["15_poc_settings_publish_settings.txt"] = message(
        f"PUBLISH {MCPTT_PSI} SIP/2.0", pub_hdr("-poc-p2", "poc-a1", 2, 4294967295, ["SIP-If-Match: poc-etag-1"]),
        "multipart/mixed;boundary=poc-a2",
        multipart("poc-a2", [
            ("application/vnd.3gpp.mcptt-info+xml",
             mcptt_info(f'<mcptt-request-uri type="Normal"><mcpttURI>tel:{UE_A}</mcpttURI></mcptt-request-uri>', client_id)),
            ("application/poc-settings+xml", poc)]))
    # 16 — 설정 제거 = MCPTT 로그오프(§7.2.1A 4) NOTE 3 · §7.3.5): Expires 0 + SIP-If-Match, 본문 없음(RFC 3903 §4.5). 서버 = 설정·제휴·바인딩 제거.
    msgs["16_poc_settings_publish_remove.txt"] = message(
        f"PUBLISH {MCPTT_PSI} SIP/2.0", pub_hdr("-poc-p3", "poc-a1", 3, 0, ["SIP-If-Match: poc-etag-1"]))
    # 17 — 서비스 설정 구독(§7.2.4 · §7.3.6): Request-URI = 참여 기능 PSI · mcptt-info <mcptt-request-uri> = 자기 MCPTT ID · Accept poc-settings ·
    #   Expires 4294967295(0 = fetch). 서버 = 구독자 MCPTT ID 가 대상과 같아야 한다(아니면 403) → NOTIFY 본문 = 그 사용자 클라이언트들의 entity.
    msgs["17_poc_settings_subscribe.txt"] = message(
        f"SUBSCRIBE {MCPTT_PSI} SIP/2.0",
        [via("-poc-s1"), "Max-Forwards: 70", f"From: <sip:{UE_A}@{DOMAIN}>;tag=poc-s1-f", f"To: <{MCPTT_PSI}>",
         f"Call-ID: poc-s1@{IP_A}", "CSeq: 1 SUBSCRIBE", ue_contact(MCPTT_TAGS), f"P-Preferred-Service: {ICSI_MCPTT}",
         "Event: poc-settings", "Expires: 4294967295", "Accept: application/poc-settings+xml"],
        "application/vnd.3gpp.mcptt-info+xml",
        mcptt_info(f'<mcptt-request-uri type="Normal"><mcpttURI>tel:{UE_A}</mcpttURI></mcptt-request-uri>'))
    # 18 — 서비스 인가 실패(§7.3.3 6)): 403 + 101 (토큰이 무효·만료·scope 3gpp:mc:ptt_service 없음·토큰의 MCPTT ID ≠ 요청 IMPU).
    msgs["18_poc_settings_reject_403_101.txt"] = message(
        "SIP/2.0 403 Forbidden",
        [via_resp("-poc-p1"), f"From: <sip:{UE_A}@{DOMAIN}>;tag=poc-a1-f", f"To: <sip:{UE_A}@{DOMAIN}>;tag=csp-e101",
         f"Call-ID: poc-a1@{IP_A}", "CSeq: 1 PUBLISH", f'Warning: 399 {DOMAIN} "101 service authorisation failed"'])
    # 19 — 바인딩 없는 설정만 PUBLISH(§7.3.4 6)): 404 + 141.
    msgs["19_poc_settings_reject_404_141.txt"] = message(
        "SIP/2.0 404 Not Found",
        [via_resp("-poc-p2"), f"From: <sip:{UE_A}@{DOMAIN}>;tag=poc-a1-f", f"To: <sip:{UE_A}@{DOMAIN}>;tag=csp-e141",
         f"Call-ID: poc-a1@{IP_A}", "CSeq: 2 PUBLISH", f'Warning: 399 {DOMAIN} "141 user unknown to the participating function"'])
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
