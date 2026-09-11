"""단말이 붙는 포트를 **OAM 에서** 읽어 방화벽 spec 으로 낸다.

**정본은 `local_nodes` 다.** `tb-site.conf`/`pkg_setting.cfg` 의 `SIP_UDP/TCP/TLS` 는
45 단계가 local_nodes 를 처음 쓸 때의 입력일 뿐이다 — 그 뒤 콘솔에서 포트를 바꾸면
둘이 갈라지고, 설정 파일만 보면 **쓰지도 않는 포트를 열게 된다**(2026-09-11 실측:
사이트는 15060/15061 인데 cfg 기본값 5060·25061·5061 을 열었다).

CSC 단말 대면 포트도 같은 이유로 csc 배포의 `McpttServer.Port` 에서 읽는다.

출력 한 줄 = `<포트>/<proto>|<설명>`. 실패하면 아무것도 내지 않고 1 로 끝난다 —
호출부가 설정 파일 값으로 물러나고 그 사실을 경고한다(설치 전이라 OAM 이 없을 수 있다).
"""
import os, sys
sys.path.insert(0, os.environ['TB_LIB'])
import tb_oam

try:
    o = tb_oam.Oam(os.environ['TB_OAM_URL'], os.environ.get('TB_ADMIN_PASS', ''))
    deps = o._items(o.req('GET', '/api/v1/deployments'), 'deployments', 'items')
except SystemExit:
    sys.exit(1)

out = []
csp = next((d for d in deps if d.get('package_name') == 'csp'), None)
if csp:
    r = o.req('GET', f"/api/v1/deployments/{csp['id']}/collection/local_nodes")
    rows = r if isinstance(r, list) else (r.get('records') or [])
    for n in rows:
        if not n.get('enabled'):
            continue
        p, proto = n.get('bind_port'), str(n.get('protocol') or '').upper()
        nm = n.get('name') or n.get('id')
        if not p:
            continue
        if proto == 'UDP':
            out.append((f"{p}/udp", f"SIP UDP ({nm})"))
        elif proto in ('TCP', 'TLS'):
            out.append((f"{p}/tcp", f"SIP {proto} ({nm})"))
        elif proto == 'IPSEC':
            # 보호 포트쌍: bind_port 는 UDP+TCP(단말 요청), client_port 는 UDP(서버 발신)
            out.append((f"{p}/udp", f"SIP IPsec 서버포트 ({nm})"))
            out.append((f"{p}/tcp", f"SIP IPsec 서버포트 ({nm})"))
            cp = n.get('client_port')
            if cp:
                out.append((f"{cp}/udp", f"SIP IPsec 단말포트 ({nm})"))

csc = next((d for d in deps if d.get('package_name') == 'csc'), None)
if csc:
    eff = (o.req('GET', f"/api/v1/deployments/{csc['id']}/config") or {}).get('effective') or {}
    e = eff.get('McpttServer.Port') or {}
    port = e.get('v') or 4430
    out.append((f"{port}/tcp", "CSC 단말 대면 HTTPS (4-8 인증서와 짝)"))

if not out:
    sys.exit(1)
seen = set()
for spec, note in out:
    if spec in seen:
        continue
    seen.add(spec)
    print(f"{spec}|{note}")
