#!/usr/bin/env python3
"""MCVideo 계약 K3·K4 — SIP 골든 메시지 조립 (docs/dev/mcvideo_dev_plan.md §3).

아래 MESSAGES 가 정본이고 같은 디렉터리의 *.txt 는 이 스크립트가 낸 **전송 바이트 그대로**(CRLF · 정확한 Content-Length)다.
본문을 바꾸려면 여기서 고치고 다시 낸다 — .txt 를 손으로 고치면 Content-Length 가 어긋난다.

  python3 tests/fixtures/mcvideo/sip/build_goldens.py            # *.txt 생성
  python3 tests/fixtures/mcvideo/sip/build_goldens.py --check    # *.txt 가 최신인지(S1-MCVIDEO-CONTRACT 가 부른다)

시나리오 = README.md(같은 디렉터리). 규격 = TS 24.281 V18.14.0(§7.2.1 REGISTER · §8.2.1.2 affiliation · §9.2.1 prearranged · §9.2.2 chat ·
§6.2.1/§6.3.3.1.1/§6.3.3.2.1 SDP · §4.4 Warning · Annex F.1 mcvideo-info) · TS 24.581 V18.8.0(§4.3.3.1·§12.1.2·§14 제어 채널 fmtp) ·
mcvideo.md §1.4 «CIMS SDP 프로파일».
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

DOMAIN = "ptt.cims.example.kr"
CSP = "csp.ptt.cims.example.kr"
PSI = f"sip:mcvideo_psi@{DOMAIN}"
ICSI = "urn:urn-7:3gpp-service.ims.icsi.mcvideo"
ICSI_ENC = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcvideo"
ICSI_MCPTT_ENC = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt"
UE_A, UE_B = "+82510002001", "+82510002002"
IP_A, IP_B, IP_CMP = "10.10.1.21", "10.10.1.22", "10.10.0.20"
CLIENT_A = "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f"
TOKEN_A = "eyJhbGciOiJIUzI1NiJ9.eyJtY3ZpZGVvX2lkIjoidGVsOis4MjUxMDAwMjAwMSJ9.c2lnbmF0dXJl"
MCVIDEO_TAGS = f'+g.3gpp.mcvideo;+g.3gpp.icsi-ref="{ICSI_ENC}"'
FOCUS = f"{MCVIDEO_TAGS};isfocus"


def mcvideo_info(*params: str) -> str:
    """Annex F.1 — 루트 <mcvideoinfo>(스키마), contentType 자식은 mcvideoURI/mcvideoString/mcvideoBoolean + type="Normal"."""
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<mcvideoinfo xmlns="urn:3gpp:ns:mcvideoInfo:1.0">\n  <mcvideo-Params>\n'
            + "".join(f"    {p}\n" for p in params) + "  </mcvideo-Params>\n</mcvideoinfo>\n")


def uri(tag, v):
    return f'<{tag} type="Normal"><mcvideoURI>{v}</mcvideoURI></{tag}>'


def string(tag, v):
    return f'<{tag} type="Normal"><mcvideoString>{v}</mcvideoString></{tag}>'


def sdp(origin_ip, o_line, audio, video, control, fmtp, video_extra=True):
    """mcvideo.md §1.4 — m 순서 audio → video → application, i= 로 MCVideo 성분 표시(TS 24.281 §6.2.1 2)c)·3)d))."""
    lines = ["v=0", o_line, "s=-", f"c=IN IP4 {origin_ip}", "t=0 0",
             f"m=audio {audio} RTP/AVP 96 101", "i=audio component of MCVideo",
             "a=rtpmap:96 AMR-WB/16000", "a=fmtp:96 octet-align=1",
             "a=rtpmap:101 telephone-event/16000", "a=fmtp:101 0-15", "a=sendrecv",
             f"m=video {video} RTP/AVP 97", "i=video component of MCVideo",
             "a=rtpmap:97 H264/90000", "a=fmtp:97 profile-level-id=42e01f;packetization-mode=1"]
    if video_extra:
        lines += ["a=rtcp-fb:97 nack pli", "a=rtcp-fb:97 ccm fir"]
    lines += ["a=sendrecv", f"m=application {control} udp MCVideo", f"a=fmtp:MCVideo {fmtp}"]
    return "\n".join(lines) + "\n"


def multipart(boundary, parts):
    out = ""
    for ctype, body in parts:
        out += f"--{boundary}\nContent-Type: {ctype}\n\n{body}"
    return out + f"--{boundary}--\n"


def message(start, headers, ctype=None, body=""):
    """본문 LF → CRLF, Content-Length = CRLF 바이트 수."""
    body_crlf = body.replace("\n", "\r\n")
    hs = list(headers)
    if ctype:
        hs.append(f"Content-Type: {ctype}")
    hs.append(f"Content-Length: {len(body_crlf.encode('utf-8'))}")
    return (start + "\r\n" + "\r\n".join(hs) + "\r\n\r\n" + body_crlf).encode("utf-8")


def via(ip, port, branch):
    return f"Via: SIP/2.0/TLS {ip}:{port};branch=z9hG4bK{branch};rport"


def ue_contact(msisdn, ip, port, tags=MCVIDEO_TAGS):
    return f"Contact: <sip:{msisdn}@{ip}:{port};transport=tls>;{tags}"


ACCEPT = [f"Accept-Contact: *;+g.3gpp.mcvideo;require;explicit",
          f'Accept-Contact: *;+g.3gpp.icsi-ref="{ICSI_ENC}";require;explicit']
SESSION_ID = f"sip:g101@{CSP}:5061;transport=tls;gr=1790775600123456-3"      # chat g101 세션 식별자(§4.5)
SESSION_ID_103 = f"sip:g103@{CSP}:5061;transport=tls;gr=1790775900654321-1"  # prearranged g103
# To 에는 port·transport-param 을 두지 않는다(RFC 3261 §19.1.1 표 1) — 세션 식별자의 gr(other-param)은 둔다.
SESSION_ID_103_TO = f"sip:g103@{CSP};gr=1790775900654321-1"

A_SDP = sdp(IP_A, f"o=- 3900000001 3900000001 IN IP4 {IP_A}", 40000, 40002, 40004,
            "mc_queueing;mc_priority=5;mc_transmission_ssrc=305419896")
A_ANSWER = sdp(IP_CMP, f"o=CSS 4 1 IN IP4 {IP_CMP}", 52000, 56000, 58000,
               "mc_queueing;mc_priority=5;mc_transmission_ssrc=2863311530")


def build():
    msgs = {}

    # 01 — REGISTER (TS 24.281 §7.2.1): 한 REGISTER 에 MCPTT·MCVideo 클라이언트 둘(§7.1) — 특성 태그 둘 + icsi-ref 목록,
    #   서비스마다 info 본문(multipart). 토큰은 규격대로 싣는다(CIMS SIP 평면은 Digest — 토큰 검증은 향후, mcx_identity_scope.md §10).
    reg_body = multipart("mcv-reg-1", [
        ("application/vnd.3gpp.mcptt-info+xml",
         '<?xml version="1.0" encoding="UTF-8"?>\n<mcpttinfo xmlns="urn:3gpp:ns:mcpttInfo:1.0">\n  <mcptt-Params>\n'
         f'    <mcptt-access-token type="Normal"><mcpttString>{TOKEN_A}</mcpttString></mcptt-access-token>\n'
         f'    <mcptt-client-id type="Normal"><mcpttString>{CLIENT_A}</mcpttString></mcptt-client-id>\n'
         "  </mcptt-Params>\n</mcpttinfo>\n"),
        ("application/vnd.3gpp.mcvideo-info+xml",
         mcvideo_info(string("mcvideo-access-token", TOKEN_A), string("mcvideo-client-id", CLIENT_A))),
    ])
    msgs["01_register.txt"] = message(
        f"REGISTER sip:{DOMAIN} SIP/2.0",
        [via(IP_A, 50601, "-mcv-reg1"), "Max-Forwards: 70",
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=reg-a1", f"To: <sip:{UE_A}@{DOMAIN}>",
         f"Call-ID: mcv-reg-a1@{IP_A}", "CSeq: 2 REGISTER",
         f'Contact: <sip:{UE_A}@{IP_A}:50601;transport=tls>;+sip.instance="<{CLIENT_A}>";+g.3gpp.mcptt;+g.3gpp.mcvideo;'
         f'+g.3gpp.icsi-ref="{ICSI_MCPTT_ENC},{ICSI_ENC}";expires=3600',
         f'Authorization: Digest username="450081000002001@{DOMAIN}",realm="{DOMAIN}",nonce="7f3a9c21",'
         f'uri="sip:{DOMAIN}",response="0d1e2f3a4b5c6d7e8f90a1b2c3d4e5f6",algorithm=MD5',
         "Supported: path, gruu", "User-Agent: cimsue/1.0"],
        "multipart/mixed;boundary=mcv-reg-1", reg_body)

    # 02 — affiliation PUBLISH (TS 24.281 §8.2.1.2·§8.3.1): Request-URI = 참여 기능 PSI, P-Preferred-Service = MCVideo ICSI,
    #   Event presence, Expires 2^32-1, mcvideo-info(<mcvideo-request-uri> = 자기 MCVideo ID) + pidf(tuple id = client ID, mcvideoPresInfo).
    pidf = ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<presence xmlns="urn:ietf:params:xml:ns:pidf" xmlns:mcvideoPI10="urn:3gpp:ns:mcvideoPresInfo:1.0"\n'
            f'  entity="tel:{UE_A}">\n'
            f'  <tuple id="{CLIENT_A}">\n    <status>\n      <mcvideoPI10:affiliation group="tel:g101"/>\n'
            '      <mcvideoPI10:affiliation group="tel:g103"/>\n    </status>\n  </tuple>\n'
            '  <mcvideoPI10:p-id>a1-mcv-aff-0001</mcvideoPI10:p-id>\n</presence>\n')
    msgs["02_publish_affiliation.txt"] = message(
        f"PUBLISH {PSI} SIP/2.0",
        [via(IP_A, 50601, "-mcv-pub1"), "Max-Forwards: 70",
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=pub-a1", f"To: <sip:{UE_A}@{DOMAIN}>",
         f"Call-ID: mcv-pub-a1@{IP_A}", "CSeq: 1 PUBLISH", f"P-Preferred-Service: {ICSI}",
         "Event: presence", "Expires: 4294967295"],
        "multipart/mixed;boundary=mcv-pub-1",
        multipart("mcv-pub-1", [("application/vnd.3gpp.mcvideo-info+xml", mcvideo_info(uri("mcvideo-request-uri", f"tel:{UE_A}"))),
                                ("application/pidf+xml", pidf)]))

    # 03 — chat 합류 INVITE (TS 24.281 §9.2.2.2.1.1): Request-URI = PSI, Contact·Accept-Contact·P-Preferred-Service = MCVideo,
    #   mcvideo-info session-type chat + mcvideo-request-uri 그룹 + client id, SDP 세 m-line(K4). 첫 합류가 곧 세션 개시(chat).
    msgs["03_chat_join_invite.txt"] = message(
        f"INVITE {PSI} SIP/2.0",
        [via(IP_A, 50601, "-mcv-inv1"), "Max-Forwards: 70",
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a1", f"To: <{PSI}>",
         f"Call-ID: mcv-join-a1@{IP_A}", "CSeq: 1 INVITE", ue_contact(UE_A, IP_A, 50601), *ACCEPT,
         f"P-Preferred-Service: {ICSI}", "Supported: timer", "Session-Expires: 1800",
         "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, UPDATE, INFO, MESSAGE, NOTIFY"],
        "multipart/mixed;boundary=mcv-inv-1",
        multipart("mcv-inv-1", [
            ("application/sdp", A_SDP),
            ("application/vnd.3gpp.mcvideo-info+xml",
             mcvideo_info("<session-type>chat</session-type>", uri("mcvideo-request-uri", "tel:g101"),
                          string("mcvideo-client-id", CLIENT_A)))]))

    # 04 — 그 200 OK (TS 24.281 §9.2.2.4.1.1 15)~20)·§9.2.2.3.1.1 5)): Contact = 세션 식별자 + MCVideo 태그 + isfocus, Require timer,
    #   Supported tdialog, PAI = 참여 기능 PSI, SDP answer = CMP 멤버 포트(cmp_media_api.md §7.9)·fmtp = offer 에 있던 것만(mc_queueing 은
    #   CMP 가 송출 큐를 쓰므로 되돌린다 — TS 24.581 §14.3.2) + mc_transmission_ssrc = CMP tc_ssrc(§6.3.3.2.1 2)b)).
    #   Session-Expires refresher=uac — 단말이 갱신한다(§6.3.3.2.3.2 2)).
    msgs["04_chat_join_200.txt"] = message(
        "SIP/2.0 200 OK",
        [via(IP_A, 50601, "-mcv-inv1").replace(";rport", ";rport=50601;received=" + IP_A),
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a1", f"To: <{PSI}>;tag=csp-7f3a",
         f"Call-ID: mcv-join-a1@{IP_A}", "CSeq: 1 INVITE", f"Contact: <{SESSION_ID}>;{FOCUS}",
         "Require: timer", "Supported: tdialog", "Session-Expires: 1800;refresher=uac",
         f"P-Asserted-Identity: <{PSI}>"],
        "application/sdp", A_ANSWER)

    # 05 — prearranged 개시 + 암묵적 송출 요청 (TS 24.281 §9.2.1.2.1.1 · TS 24.581 §14.2.4·§14.2.5·§14.3.5): 새 세션 개시라 서버가 받는다.
    b_offer = sdp(IP_A, f"o=- 3900000002 3900000002 IN IP4 {IP_A}", 40010, 40012, 40014,
                  "mc_priority=5;mc_granted;mc_implicit_request;mc_transmission_ssrc=305419897")
    msgs["05_prearranged_initiate_invite.txt"] = message(
        f"INVITE {PSI} SIP/2.0",
        [via(IP_A, 50601, "-mcv-inv2"), "Max-Forwards: 70",
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a2", f"To: <{PSI}>",
         f"Call-ID: mcv-pre-a1@{IP_A}", "CSeq: 1 INVITE", ue_contact(UE_A, IP_A, 50601), *ACCEPT,
         f"P-Preferred-Service: {ICSI}", "Supported: timer", "Session-Expires: 1800"],
        "multipart/mixed;boundary=mcv-inv-2",
        multipart("mcv-inv-2", [
            ("application/sdp", b_offer),
            ("application/vnd.3gpp.mcvideo-info+xml",
             mcvideo_info("<session-type>prearranged</session-type>", uri("mcvideo-request-uri", "tel:g103"),
                          string("mcvideo-client-id", CLIENT_A)))]))

    # 06 — 그 200 OK: 암묵 요청을 받아들였다(mc_implicit_request) + 허가(mc_granted — offer 에 있었다) + 송출 SSRC 쌍(CMP 할당 — K6 R2).
    msgs["06_prearranged_initiate_200.txt"] = message(
        "SIP/2.0 200 OK",
        [via(IP_A, 50601, "-mcv-inv2").replace(";rport", ";rport=50601;received=" + IP_A),
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a2", f"To: <{PSI}>;tag=csp-8b21",
         f"Call-ID: mcv-pre-a1@{IP_A}", "CSeq: 1 INVITE", f"Contact: <{SESSION_ID_103}>;{FOCUS}",
         "Require: timer", "Supported: tdialog", "Session-Expires: 1800;refresher=uac", f"P-Asserted-Identity: <{PSI}>"],
        "application/sdp",
        sdp(IP_CMP, f"o=CSS 4 1 IN IP4 {IP_CMP}", 52010, 56010, 58010,
            "mc_priority=5;mc_granted;mc_implicit_request;mc_audio_ssrc=1111638594;mc_video_ssrc=1111638595;"
            "mc_transmission_ssrc=2863311531"))

    # 07 — prearranged 멤버 초대 (TS 24.281 §6.3.3.1.2·§9.2.1.4.1.1): Contact = 세션 식별자 + isfocus, Accept-Contact 둘,
    #   P-Asserted-Service = MCVideo ICSI(RFC 6050 헤더 이름 — 본문의 «P-Asserted-Service-Id» 는 오기, mcvideo.md §9),
    #   mcvideo-info = request-uri(초대받는 MCVideo ID)·calling-user-id·calling-group-id, SDP offer = CMP 가 이 멤버에게 준 포트 +
    #   fmtp mc_priority=<user-priority>(TS 24.581 §14.2.3)·mc_transmission_ssrc(§6.3.3.1.1 4)). Session-Expires 는 refresher 를
    #   싣지 않는다(§6.3.3.1.2 6)) — 단말이 200 OK 에서 refresher=uas 로 정한다(§6.2.3.1.1 5)).
    msgs["07_prearranged_member_invite.txt"] = message(
        f"INVITE sip:{UE_B}@{IP_B}:50602;transport=tls SIP/2.0",
        [f"Via: SIP/2.0/TLS {CSP}:5061;branch=z9hG4bK-mcv-fan1", "Max-Forwards: 70",
         f"From: <sip:g103@{DOMAIN}>;tag=csp-fan1", f"To: <sip:{UE_B}@{DOMAIN}>",
         "Call-ID: csp-mcv-fan-b1@csp", "CSeq: 1 INVITE", f"Contact: <{SESSION_ID_103}>;{FOCUS}", *ACCEPT,
         f"P-Asserted-Service: {ICSI}", "Supported: timer", "Session-Expires: 1800"],
        "multipart/mixed;boundary=mcv-fan-1",
        multipart("mcv-fan-1", [
            ("application/sdp", sdp(IP_CMP, f"o=CSS 4 1 IN IP4 {IP_CMP}", 52012, 56012, 58012,
                                    "mc_priority=5;mc_transmission_ssrc=2863311532")),
            ("application/vnd.3gpp.mcvideo-info+xml",
             mcvideo_info("<session-type>prearranged</session-type>", uri("mcvideo-request-uri", f"tel:{UE_B}"),
                          uri("mcvideo-calling-user-id", f"tel:{UE_A}"), uri("mcvideo-calling-group-id", "tel:g103")))]))

    # 08 — prearranged 재합류 (TS 24.281 §9.2.1.2.4.1): Request-URI = 세션 식별자(06 의 Contact), 나머지는 05 와 같다(암묵 요청 없음 —
    #   진행 중 세션 합류는 받지 않는다, TS 24.581 §14.3.5).
    msgs["08_prearranged_rejoin_invite.txt"] = message(
        f"INVITE {SESSION_ID_103} SIP/2.0",
        [via(IP_A, 50601, "-mcv-inv3"), "Max-Forwards: 70",
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a3", f"To: <{SESSION_ID_103_TO}>",
         f"Call-ID: mcv-rejoin-a1@{IP_A}", "CSeq: 1 INVITE", ue_contact(UE_A, IP_A, 50601), *ACCEPT,
         f"P-Preferred-Service: {ICSI}", "Supported: timer", "Session-Expires: 1800"],
        "multipart/mixed;boundary=mcv-inv-3",
        multipart("mcv-inv-3", [
            ("application/sdp", sdp(IP_A, f"o=- 3900000003 3900000003 IN IP4 {IP_A}", 40020, 40022, 40024,
                                    "mc_priority=5;mc_transmission_ssrc=305419898")),
            ("application/vnd.3gpp.mcvideo-info+xml",
             mcvideo_info("<session-type>prearranged</session-type>", uri("mcvideo-request-uri", "tel:g103"),
                          string("mcvideo-client-id", CLIENT_A)))]))

    # 09·10 — 그룹 종류 검사 (TS 24.281 §6.3.5.2 5)c)·d)): 404 + Warning 399 <PTT 도메인> "117 …" / "118 …" (§4.4).
    msgs["09_reject_404_117.txt"] = message(
        "SIP/2.0 404 Not Found",
        [via(IP_A, 50601, "-mcv-inv4").replace(";rport", ";rport=50601;received=" + IP_A),
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a4", f"To: <{PSI}>;tag=csp-e117",
         f"Call-ID: mcv-wrong-a1@{IP_A}", "CSeq: 1 INVITE",
         f'Warning: 399 {DOMAIN} "117 the group identity indicated in the request is a prearranged group"'])
    msgs["10_reject_404_118.txt"] = message(
        "SIP/2.0 404 Not Found",
        [via(IP_A, 50601, "-mcv-inv5").replace(";rport", ";rport=50601;received=" + IP_A),
         f"From: <sip:{UE_A}@{DOMAIN}>;tag=inv-a5", f"To: <{PSI}>;tag=csp-e118",
         f"Call-ID: mcv-wrong-a2@{IP_A}", "CSeq: 1 INVITE",
         f'Warning: 399 {DOMAIN} "118 the group identity indicated in the request is a chat group"'])
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
