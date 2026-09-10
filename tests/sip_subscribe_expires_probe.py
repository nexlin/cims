"""SUBSCRIBE Expires 부여 검사 프로브 — RFC 3261 §20.19 32bit delta-seconds + RFC 6665 §4.2.1.1 초기 NOTIFY.

외부 MCX SDK 실측(09-07): `Expires: 4294967295` 로 xcap-diff SUBSCRIBE → 서버가 int 오버플로로 해지(Expires 0)로
오판해 200 OK `Expires: 0` 만 주고 초기 NOTIFY 를 보내지 않았다. 이 프로브는 그 요청을 그대로 재현해
(1) 200 OK 의 Expires 가 상한(3600)으로 부여됐는지 (2) 초기 NOTIFY(Subscription-State: active, xcap-diff 본문)가
오는지 (3) Expires:0 해지에 200 + terminated NOTIFY 가 오는지 본다. 등록 없이 UDP 로 직접 보낸다(구독은 등록을 요구하지 않음).

  python3 tests/sip_subscribe_expires_probe.py --host 10.0.2.45 --port 15060 --user +82500000004 \
      --target sip:cms_psi@ptt.cims.example.kr --expires 4294967295
"""
import argparse, hashlib, os, random, re, socket, sys, time, uuid

def digest_response(username, realm, password, method, uri, nonce, qop, cnonce, nc):
    md5 = lambda x: hashlib.md5(x.encode()).hexdigest()
    ha1 = md5(f"{username}:{realm}:{password}"); ha2 = md5(f"{method}:{uri}")
    return md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}") if qop else md5(f"{ha1}:{nonce}:{ha2}")

def auth_header(challenge, username, password, method, uri):
    """WWW-Authenticate 챌린지 → Authorization 헤더 값 (RFC 2617 Digest, qop=auth 우선)."""
    kv = dict(re.findall(r'(\w+)=(?:"([^"]*)"|([^,\s]+))', challenge) and
              [(m[0], m[1] or m[2]) for m in re.findall(r'(\w+)=(?:"([^"]*)"|([^,\s]+))', challenge)])
    realm, nonce = kv.get('realm', ''), kv.get('nonce', '')
    qop = 'auth' if 'auth' in (kv.get('qop') or '') else ''
    cnonce, nc = uuid.uuid4().hex[:16], '00000001'
    resp = digest_response(username, realm, password, method, uri, nonce, qop, cnonce, nc)
    h = f'Digest username="{username}",realm="{realm}",nonce="{nonce}",uri="{uri}",response="{resp}",algorithm=MD5'
    if qop: h += f',cnonce="{cnonce}",nc={nc},qop=auth'
    return h

def sip_txn(s, build, wait, ctx):
    """요청 1회 전송 → 최종 응답(및 도중 수신 NOTIFY 들) 회수. build(auth) 로 401 재시도 1회."""
    s.send(build(None)); resp=None; notifies=[]; deadline=time.time()+wait
    while time.time() < deadline:
        try: data,_ = s.recvfrom(65535)
        except socket.timeout: break
        msg = data.decode(errors='replace')
        if msg.startswith('SIP/2.0'):
            resp = msg
            if resp.split(' ')[1] == '401':
                s.send(build(auth_header(hdr(resp,'WWW-Authenticate') or '', ctx['impi'], ctx['pw'], ctx['method'], ctx['uri']))); resp=None; continue
            if resp.split(' ')[1] != '100': break
        elif msg.startswith('NOTIFY'):
            notifies.append(msg); via = hdr(msg,'Via')
            s.send((f"SIP/2.0 200 OK\r\nVia: {via}\r\nFrom: {hdr(msg,'From')}\r\nTo: {hdr(msg,'To')}\r\n"
                    f"Call-ID: {hdr(msg,'Call-ID')}\r\nCSeq: {hdr(msg,'CSeq')}\r\nContent-Length: 0\r\n\r\n").encode())
    # 응답 뒤에 오는 초기 NOTIFY 를 조금 더 기다린다
    s.settimeout(1.0); t2=time.time()+2.0
    while time.time() < t2:
        try: data,_ = s.recvfrom(65535)
        except socket.timeout: break
        msg = data.decode(errors='replace')
        if msg.startswith('NOTIFY'):
            notifies.append(msg); via = hdr(msg,'Via')
            s.send((f"SIP/2.0 200 OK\r\nVia: {via}\r\nFrom: {hdr(msg,'From')}\r\nTo: {hdr(msg,'To')}\r\n"
                    f"Call-ID: {hdr(msg,'Call-ID')}\r\nCSeq: {hdr(msg,'CSeq')}\r\nContent-Length: 0\r\n\r\n").encode())
    s.settimeout(wait)
    return resp, notifies

def build_register(local_ip, local_port, args, call_id, from_tag, cseq, expires, auth):
    branch = 'z9hG4bK' + uuid.uuid4().hex[:16]
    a = f"Authorization: {auth}\r\n" if auth else ''
    return (f"REGISTER sip:{args.domain} SIP/2.0\r\nVia: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport\r\n"
            f"Max-Forwards: 70\r\nFrom: <sip:{args.user}@{args.domain}>;tag={from_tag}\r\nTo: <sip:{args.user}@{args.domain}>\r\n"
            f"Call-ID: {call_id}\r\nCSeq: {cseq} REGISTER\r\nContact: <sip:{args.user}@{local_ip}:{local_port}>\r\n"
            f"Expires: {expires}\r\n{a}User-Agent: cims-subscribe-probe\r\nContent-Length: 0\r\n\r\n").encode()

def build_subscribe(local_ip, local_port, args, call_id, from_tag, cseq, expires, to_tag=None, auth=None):
    branch = 'z9hG4bK' + uuid.uuid4().hex[:16]
    to = f"<{args.target}>" + (f";tag={to_tag}" if to_tag else '')
    a = f"Authorization: {auth}\r\n" if auth else ''
    return (
        f"SUBSCRIBE {args.target} SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport\r\n"
        f"Max-Forwards: 70\r\n"
        f"From: <sip:{args.user}@{args.domain}>;tag={from_tag}\r\n"
        f"To: {to}\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: {cseq} SUBSCRIBE\r\n"
        f"Contact: <sip:{args.user}@{local_ip}:{local_port}>\r\n"
        f"Event: xcap-diff\r\n"
        f"Accept: application/xcap-diff+xml\r\n"
        f"Expires: {expires}\r\n"
        f"{a}User-Agent: cims-subscribe-probe\r\n"
        f"Content-Length: 0\r\n\r\n").encode()

def hdr(msg, name):
    m = re.search(rf"^{name}: *(.+?)\r?$", msg, re.M | re.I)
    return m.group(1).strip() if m else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='121.161.164.45'); ap.add_argument('--port', type=int, default=15060)
    ap.add_argument('--user', default='+82500000011'); ap.add_argument('--domain', default='ptt.cims.example.kr')
    ap.add_argument('--impi', default=''); ap.add_argument('--password', default='1234')
    ap.add_argument('--target', default='sip:cms_psi@ptt.cims.example.kr')
    ap.add_argument('--expires', default='4294967295'); ap.add_argument('--wait', type=float, default=3.0)
    ap.add_argument('--no-unregister', action='store_true', help='끝에 REGISTER Expires:0 을 보내지 않음(실단말 계정이면 필수 — CSP 해지는 바인딩 전체 삭제)')
    args = ap.parse_args()
    impi = args.impi or f"45033{args.user.lstrip('+')}@{args.domain}"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(args.wait)
    s.connect((args.host, args.port)); local_ip, local_port = s.getsockname()
    ctx = {'impi': impi, 'pw': args.password, 'method': '', 'uri': ''}
    fails = 0
    def check(cond, msg):
        nonlocal fails
        print(("  PASS  " if cond else "  FAIL  ") + msg); fails += (0 if cond else 1)

    # 0) 등록 (Digest) — SUBSCRIBE 는 등록 신원에만 열린다
    reg_cid, reg_tag = uuid.uuid4().hex, uuid.uuid4().hex[:10]
    ctx.update(method='REGISTER', uri=f"sip:{args.domain}")
    r, _ = sip_txn(s, lambda auth: build_register(local_ip, local_port, args, reg_cid, reg_tag, 1 if auth is None else 2, 120, auth), args.wait, ctx)
    st = r.split('\r\n')[0] if r else None
    print(f"REGISTER {args.user} (IMPI {impi}) → {st}")
    check(r is not None and ' 200 ' in st, f"등록 200 (got {st})")
    if not (r and ' 200 ' in st):
        print("결과: FAIL (등록 실패 — 계정/비밀번호 확인)"); sys.exit(1)

    # 1) SUBSCRIBE 큰 Expires
    call_id = uuid.uuid4().hex; from_tag = uuid.uuid4().hex[:10]
    ctx.update(method='SUBSCRIBE', uri=args.target)
    resp, notifies = sip_txn(s, lambda auth: build_subscribe(local_ip, local_port, args, call_id, from_tag, 1 if auth is None else 2, args.expires, auth=auth), args.wait, ctx)
    print(f"SUBSCRIBE Expires={args.expires} → {args.target}")
    st = resp.split('\r\n')[0] if resp else None
    to_tag = None
    if resp:
        m = re.search(r'tag=([^;\s>]+)', hdr(resp, 'To') or ''); to_tag = m.group(1) if m else None
    check(resp is not None and ' 200 ' in st, f"200 OK (got {st})")
    gexp = hdr(resp, 'Expires') if resp else None
    check(gexp is not None and gexp.isdigit() and 0 < int(gexp) <= 3600, f"부여 Expires = 1..3600 (got {gexp})")
    check(len(notifies) >= 1, f"초기 NOTIFY 수신 (got {len(notifies)})")
    if notifies:
        n = notifies[0]
        check((hdr(n, 'Subscription-State') or '').startswith('active'), f"Subscription-State active (got {hdr(n,'Subscription-State')})")
        check('xcap-diff' in (hdr(n, 'Content-Type') or ''), f"Content-Type xcap-diff+xml (got {hdr(n,'Content-Type')})")
        print(f"  info  xcap-diff documents: {re.findall(r'<document [^>]*sel=\"([^\"]+)\"', n)}")
    # 2) 해지
    if resp and to_tag and gexp not in (None, '0'):
        r2, term = sip_txn(s, lambda auth: build_subscribe(local_ip, local_port, args, call_id, from_tag, 3 if auth is None else 4, 0, to_tag, auth), args.wait, ctx)
        check(r2 is not None and ' 200 ' in r2.split('\r\n')[0] and hdr(r2, 'Expires') == '0', f"해지 200 + Expires 0 (got {r2.split(chr(13))[0] if r2 else None})")
        check(any((hdr(t, 'Subscription-State') or '').startswith('terminated') for t in term), f"terminated NOTIFY (got {len(term)})")
    # 3) 등록 해제 (휴면 계정 전제)
    if not args.no_unregister:
        ctx.update(method='REGISTER', uri=f"sip:{args.domain}")
        r3, _ = sip_txn(s, lambda auth: build_register(local_ip, local_port, args, reg_cid, reg_tag, 5 if auth is None else 6, 0, auth), args.wait, ctx)
        print(f"  info  unregister → {r3.split(chr(13))[0] if r3 else None}")
    print(f"결과: FAIL {fails}")
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
