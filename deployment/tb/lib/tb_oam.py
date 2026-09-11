#!/usr/bin/env python3
"""tb_oam.py — OAM REST 클라이언트 (TB 설치 스크립트용, 표준 라이브러리만).

패키지 등록·모듈 설치·설정 주입·기동은 **모두 OAM 의 공개 REST 로만** 한다. 콘솔이
사람 손으로 하는 것과 같은 경로여서, OAM 이 소유한 부수효과(설치 이력, 게이트웨이 라우트
재등록, JwtSecret 주입)를 우회하지 않는다. 파일을 직접 만지지 않는 이유가 이것이다.

OAM job 은 fire-and-forget 이라 완료 게이팅이 없다 — 여기서 폴링해 순서를 만든다.

  tb_oam.py status
  tb_oam.py upload <tarball...> [--force]
  tb_oam.py install <패키지> <프로세스명>
  tb_oam.py config <패키지> --set K=V [--set ...]
  tb_oam.py collection <패키지> <컬렉션명> <records.json>
  tb_oam.py job <패키지> <job_type>
  tb_oam.py console-bundle

인증: tb-site.conf 의 TB_ADMIN_PASS(환경변수로 전달) → /api/v1/auth/login.
토큰은 state/oam.tok(0600)에 캐시하고 401 이면 한 번 재로그인한다.
"""

from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request

_HERE = os.path.dirname(os.path.abspath(__file__))
_STATE = os.environ.get('TB_STATE_DIR') or os.path.join(_HERE, '..', 'state')
_TOKFILE = os.path.join(_STATE, 'oam.tok')

# self-signed 인증서 — 관리평면은 부트스트랩이 만든 자체 서명 인증서를 쓴다.
_CTX = ssl.create_default_context()
_CTX.check_hostname = False
_CTX.verify_mode = ssl.CERT_NONE


def die(msg: str, *hints: str):
    print(f"ERROR: {msg}", file=sys.stderr)
    for h in hints:
        print(f"        {h}", file=sys.stderr)
    raise SystemExit(1)


class Oam:
    def __init__(self, base: str, admin_pass: str):
        self.base = base.rstrip('/')
        self.admin_pass = admin_pass
        self.token = self._cached_token()

    # ── 인증 ────────────────────────────────────────────────────
    def _cached_token(self):
        try:
            with open(_TOKFILE, encoding='utf-8') as f:
                return f.read().strip() or None
        except OSError:
            return None

    def login(self):
        body = json.dumps({'login_id': 'admin', 'password': self.admin_pass}).encode()
        r = self._raw('POST', '/api/v1/auth/login', body, 'application/json', auth=False)
        tok = (r or {}).get('token')
        if not tok:
            die("OAM 로그인 실패 — admin 비밀번호를 확인하세요",
                "tb-site.conf 의 TB_ADMIN_PASS (부트스트랩에서 지정한 값)")
        self.token = tok
        os.makedirs(_STATE, exist_ok=True)
        fd = os.open(_TOKFILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(tok)
        return tok

    # ── HTTP ────────────────────────────────────────────────────
    def _raw(self, method, path, data, ctype, auth=True, soft_conn=False):
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if ctype:
            req.add_header('Content-Type', ctype)
        if auth:
            req.add_header('Authorization', f'Bearer {self.token or ""}')
        try:
            with urllib.request.urlopen(req, context=_CTX, timeout=300) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            raw = e.read()
            if e.code == 401 and auth:
                raise _Unauthorized()
            try:
                detail = json.loads(raw.decode('utf-8', 'replace'))
            except Exception:
                detail = raw.decode('utf-8', 'replace')[:300]
            die(f"{method} {path} → HTTP {e.code}", f"응답: {detail}")
        except urllib.error.URLError as e:
            # 자기 재기동을 시킨 뒤 그 OAM 에게 진행을 묻는 경우처럼, 끊기는 것이 **예상된**
            # 자리가 있다. 거기서 ERROR 를 찍으면 정상 동작이 사고처럼 보인다 — 호출부가
            # soft_conn 으로 "여기선 끊겨도 된다" 를 밝히면 예외로 올려 판단을 넘긴다.
            if soft_conn:
                raise _ConnLost(str(e.reason))
            die(f"{method} {path} → 접속 실패 ({e.reason})",
                f"OAM 이 떠 있는지 확인: curl -sk -o /dev/null -w '%{{http_code}}' {self.base}/")
        if not raw:
            return {}
        try:
            return json.loads(raw.decode('utf-8', 'replace'))
        except Exception:
            return {'_raw': raw.decode('utf-8', 'replace')[:2000]}

    def req(self, method, path, body=None, *, raw_bytes=None, ctype=None, soft_conn=False):
        data = raw_bytes if raw_bytes is not None else (
            json.dumps(body).encode() if body is not None else None)
        ct = ctype or ('application/json' if data is not None and raw_bytes is None else None)
        for attempt in (1, 2):
            if not self.token:
                self.login()
            try:
                return self._raw(method, path, data, ct, soft_conn=soft_conn)
            except _Unauthorized:
                if attempt == 2:
                    die("인증이 계속 거부됩니다 (401)")
                self.token = None      # 만료 — 한 번 재로그인
        return {}

    # ── 조회 ────────────────────────────────────────────────────
    @staticmethod
    def _items(r, *keys):
        if isinstance(r, list):
            return r
        if isinstance(r, dict):
            for k in keys:
                if isinstance(r.get(k), list):
                    return r[k]
        return []

    def agents(self):
        return self._items(self.req('GET', '/api/v1/agents'), 'items', 'agents')

    def agent(self, name=None):
        ags = self.agents()
        if name:
            for a in ags:
                if a.get('name') == name:
                    return a
            die(f"agent '{name}' 를 찾을 수 없습니다",
                f"등록된 agent: {', '.join(str(a.get('name')) for a in ags) or '없음'}")
        if len(ags) == 1:
            return ags[0]
        die(f"agent 가 {len(ags)}개입니다 — TB_AGENT_NAME 으로 지정하세요",
            f"등록된 agent: {', '.join(str(a.get('name')) for a in ags) or '없음'}")

    def packages(self):
        return self._items(self.req('GET', '/api/v1/packages'), 'items', 'packages')

    def deployments(self):
        return self._items(self.req('GET', '/api/v1/deployments'), 'items', 'deployments')

    def deployment(self, pkg):
        for d in self.deployments():
            if d.get('package_name') == pkg:
                return d
        return None

    def need_deployment(self, pkg):
        d = self.deployment(pkg)
        if not d:
            die(f"'{pkg}' 배포가 없습니다 — 설치 단계(40)를 먼저 실행하세요")
        return d

    def latest_package(self, name):
        cands = [p for p in self.packages() if p.get('name') == name]
        if not cands:
            die(f"패키지 '{name}' 가 등록되지 않았습니다 — 등록 단계(30)를 먼저 실행하세요")

        def key(p):
            return [int(x) if x.isdigit() else 0 for x in str(p.get('version', '')).split('.')]
        return sorted(cands, key=key)[-1]

    # ── job ─────────────────────────────────────────────────────
    def run_job(self, did, agent_id, job_type, timeout=600, poll=3, expect_restart=False):
        r = self.req('POST', f'/api/v1/deployments/{did}/job', {'job_type': job_type})
        jid = (r or {}).get('job_id')
        if not jid:
            die(f"{job_type} job 큐잉 실패 — 응답에 job_id 없음: {r}")
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                j = self.req('GET', f'/api/v1/agents/{agent_id}/jobs/{jid}',
                             soft_conn=expect_restart)
            except _ConnLost as e:
                # oam 자신을 재기동시킨 경우 — job 은 이미 큐잉됐고 끊기는 것이 정상이다.
                # 완료 판정은 호출부(복귀 대기)가 한다.
                print(f"  {job_type} job#{jid} 큐잉됨 — OAM 재기동으로 응답이 끊겼습니다 ({e})")
                return {'status': 'restarting', 'job_id': jid}
            if isinstance(j, dict):
                last = j.get('status')
                if last in ('succeeded', 'failed', 'cancelled'):
                    if last != 'succeeded':
                        err = (j.get('result_stderr') or j.get('result_stdout') or '').strip()
                        tail = ' / '.join(err.splitlines()[-3:]) if err else '출력 없음'
                        die(f"{job_type} job#{jid} {last} (rc={j.get('result_code')})", tail)
                    return j
            time.sleep(poll)
        die(f"{job_type} job#{jid} 이 {timeout}초 내에 끝나지 않음 (현재 {last})")


class _Unauthorized(Exception):
    pass


class _ConnLost(Exception):
    """OAM 과의 연결이 끊겼다. 예상된 자리(자기 재기동)에서만 올라온다."""
    pass


# ── 값 파싱 ───────────────────────────────────────────────────────
def parse_val(s: str):
    """K=V 의 V 를 JSON 스칼라로 해석한다 — 4419 는 정수, false 는 불리언, [..]/{..} 는 JSON."""
    t = s.strip()
    low = t.lower()
    if low in ('true', 'false'):
        return low == 'true'
    if low in ('null', 'none'):
        return None
    if t and (t[0] in '[{'):
        return json.loads(t)
    try:
        return int(t)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        pass
    return t


# ── 서브커맨드 ────────────────────────────────────────────────────
def cmd_status(o, args):
    """배포 상태 표. packages 를 주면 **그 모듈만** up 을 요구한다.

    감시 대상이 아닌 배포까지 요구하면 판정이 항상 실패한다 — cspsim 은 상주 데몬이
    아니라 호시험(70)이 바이너리를 직접 실행하는 시뮬레이터여서 stopped 이 정상이다.
    그래서 요구 대상은 호출자가 정한다(60 단계는 TB_START_ORDER 를 넘긴다).
    생략하면 종전처럼 oam 을 뺀 전부를 요구한다.
    """
    a = o.agent(os.environ.get('TB_AGENT_NAME') or None)
    print(f"  agent  id={a.get('id')} name={a.get('name')!r} status={a.get('status')}")
    deps = o.deployments()
    if not deps:
        print("  배포 없음")
        return 0
    want = {m.strip() for m in (getattr(args, 'packages', None) or []) if m.strip()}
    bad = []
    for d in sorted(deps, key=lambda x: x.get('id') or 0):
        st, live = d.get('status'), d.get('live_state')
        name = d.get('package_name')
        up = (st == 'running' and live == 'up')
        required = (name in want) if want else (name != 'oam')
        if required and not up:
            bad.append(name)
        # ✓ 정상 / ✗ 요구 대상인데 안 떴다 / · 요구 대상이 아니다(정보성)
        mark = '✓' if up else ('✗' if required else '·')
        print(f"  {mark} #{d.get('id')} {str(name):8} "
              f"{str(d.get('package_version')):9} process={str(d.get('process_name')):8} "
              f"status={st} live={live}")
    if want:
        print(f"  요구 대상: {' '.join(sorted(want))}"
              + (f" — 미기동 {' '.join(bad)}" if bad else " — 전부 up"))
    return 2 if (args.require_up and bad) else 0


def cmd_upload(o, args):
    for path in args.files:
        if not os.path.isfile(path):
            die(f"파일 없음: {path}")
        with open(path, 'rb') as f:
            raw = f.read()
        q = '?force=true' if args.force else ''
        r = o.req('POST', f'/api/v1/packages{q}', raw_bytes=raw,
                  ctype='application/octet-stream')
        name = (r or {}).get('name') or (r or {}).get('package', {}).get('name')
        ver = (r or {}).get('version') or (r or {}).get('package', {}).get('version')
        print(f"  등록 {os.path.basename(path)} → {name or '?'} {ver or ''}"
              f" ({len(raw) // 1024}KB)")
    return 0


def cmd_packages(o, args):
    seen = {}
    for p in o.packages():
        seen.setdefault(p.get('name'), []).append(str(p.get('version')))
    for n in sorted(seen):
        print(f"   {n:10} {', '.join(sorted(seen[n]))}")
    need = [x for x in (args.require or '').split(',') if x]
    missing = [n for n in need if n not in seen]
    if missing:
        print(f"   ⚠ 필수 모듈 미등록: {', '.join(missing)}")
        return 1
    return 0


def cmd_install(o, args):
    a = o.agent(os.environ.get('TB_AGENT_NAME') or None)
    pkg = o.latest_package(args.package)
    dep = o.deployment(args.package)
    if dep and str(dep.get('package_version') or '') == str(pkg['version']) \
            and dep.get('install_path'):
        print(f"  {args.package} {pkg['version']} 이미 설치됨 (배포#{dep['id']}) — 건너뜀")
        return 0
    if dep:
        o.req('PUT', f"/api/v1/deployments/{dep['id']}", {'package_id': pkg['id']})
        did = dep['id']
        print(f"  {args.package} → {pkg['version']} 로 교체 (배포#{did})")
    else:
        r = o.req('POST', '/api/v1/deployments',
                  {'agent_id': a['id'], 'package_id': pkg['id'],
                   'process_name': args.process, 'config': {}, 'note': 'tb-install'})
        did = (r or {}).get('id') or (r or {}).get('deployment', {}).get('id')
        if not did:
            die(f"배포 레코드 생성 실패: {r}")
        print(f"  {args.package} {pkg['version']} 배포#{did} 생성")
    o.run_job(did, a['id'], 'install', timeout=args.timeout)
    print(f"  {args.package} 설치 완료")
    return 0


def cmd_config(o, args):
    a = o.agent(os.environ.get('TB_AGENT_NAME') or None)
    dep = o.need_deployment(args.package)
    overlay = {}
    for kv in args.set or []:
        if '=' not in kv:
            die(f"--set 은 KEY=VALUE 형식입니다: {kv}")
        k, v = kv.split('=', 1)
        overlay[k.strip()] = parse_val(v)
    if not overlay:
        print("  설정할 값이 없습니다"); return 0

    cur = dep.get('config') or {}
    changed = {k: v for k, v in overlay.items() if cur.get(k) != v}
    if changed:
        # 설정은 배포 레코드 본체가 아니라 **전용 경로**로 보낸다. 레코드 PUT 이 받는 필드는
        # process_name/install_path/note/package_id/service_functions 뿐이라 config 를 실어
        # 보내면 400 no_updatable_fields 로 거부된다 (OAM handlers/agents.py 의 라우팅 표).
        #
        # 이 경로는 **서버가 병합**한다(온 키만 반영, 값 null 이 명시 삭제). 그래서 읽어온
        # overlay 를 되쓰지 않고 우리가 정한 키만 보낸다 — 조회 응답의 password sentinel 이나
        # 다른 노드가 넣은 `_infra` 값을 되써서 날리는 사고를 원천 차단한다.
        #
        # queue_update=False — 아래에서 update_config 를 직접 돌리고 완료까지 기다린다.
        # 기본값(true)으로 두면 job 이 두 건 걸려 서로 경합한다.
        o.req('PUT', f"/api/v1/deployments/{dep['id']}/config",
              {'config': overlay, 'queue_update': False})
        for k in sorted(changed):
            shown = '********' if any(s in k.lower() for s in ('pass', 'secret', 'token')) \
                    else changed[k]
            print(f"  {args.package}: {k} = {shown}")
    else:
        print(f"  {args.package}: {len(overlay)}키 모두 이미 같은 값")
    # 값이 같아도 노드 파일에 렌더된 적이 없을 수 있어 update_config 는 항상 돌린다.
    o.run_job(dep['id'], a['id'], 'update_config', timeout=args.timeout)
    print(f"  {args.package}: 설정 파일 반영(update_config) 완료")
    return 0


def cmd_collection(o, args):
    dep = o.need_deployment(args.package)
    if not dep.get('install_path'):
        die(f"배포#{dep['id']} 가 설치되지 않아 컬렉션을 쓸 수 없습니다",
            "설치 단계(40)를 먼저 실행하세요")
    with open(args.file, encoding='utf-8') as f:
        records = json.load(f)
    if not isinstance(records, list):
        die(f"{args.file} 은 레코드 배열(JSON list)이어야 합니다")
    o.req('PUT', f"/api/v1/deployments/{dep['id']}/collection/{args.name}",
          {'records': records, 'signal': True})
    print(f"  {args.package}/{args.name}: {len(records)}행 저장")
    return 0


def cmd_ensure_running(o, args):
    """이미 live_state=up 이면 건너뛴다 — start job 은 기동 중이면 409 를 낸다."""
    a = o.agent(os.environ.get('TB_AGENT_NAME') or None)
    dep = o.need_deployment(args.package)
    if dep.get('status') == 'running' and dep.get('live_state') == 'up':
        print(f"  {args.package} 이미 기동됨 — 건너뜀")
        return 0
    o.run_job(dep['id'], a['id'], 'start', timeout=args.timeout)
    # live_state 는 heartbeat 관측이라 job 성공 직후엔 아직 down 일 수 있다 — 잠깐 확인한다.
    for _ in range(args.settle // 3 or 1):
        time.sleep(3)
        d = o.deployment(args.package) or {}
        if d.get('live_state') == 'up':
            print(f"  {args.package} 기동 확인 (live=up)")
            return 0
    print(f"  {args.package} start 는 성공했지만 live_state 가 아직 up 이 아닙니다 "
          f"— 상태 확인 단계에서 다시 봅니다")
    return 0


def cmd_job(o, args):
    a = o.agent(os.environ.get('TB_AGENT_NAME') or None)
    dep = o.need_deployment(args.package)
    r = o.run_job(dep['id'], a['id'], args.job_type, timeout=args.timeout,
                  expect_restart=getattr(args, 'expect_restart', False))
    if (r or {}).get('status') == 'restarting':
        return 0
    print(f"  {args.package}: {args.job_type} 완료")
    return 0


def cmd_console_bundle(o, args):
    """서빙 중인 콘솔 번들 파일명 — oam-svc 동봉본으로 승격됐는지 판정용."""
    import re
    r = o.req('GET', '/')
    html = r.get('_raw') if isinstance(r, dict) else ''
    m = re.findall(r'assets/index-[A-Za-z0-9_-]+\.js', html or '')
    print(m[0] if m else '(번들 표시 없음)')
    return 0


def main():
    ap = argparse.ArgumentParser(description="OAM REST 클라이언트 (TB 설치용)")
    ap.add_argument('--url', default=os.environ.get('TB_OAM_URL', 'https://127.0.0.1:4419'))
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('status'); p.add_argument('--require-up', action='store_true')
    p.add_argument('packages', nargs='*',
                   help='up 을 요구할 모듈 (생략하면 oam 을 뺀 전부)')
    p = sub.add_parser('packages'); p.add_argument('--require', default='')
    p = sub.add_parser('upload'); p.add_argument('files', nargs='+'); p.add_argument('--force', action='store_true')
    p = sub.add_parser('install'); p.add_argument('package'); p.add_argument('process')
    p.add_argument('--timeout', type=int, default=900)
    p = sub.add_parser('config'); p.add_argument('package'); p.add_argument('--set', action='append')
    p.add_argument('--timeout', type=int, default=600)
    p = sub.add_parser('collection'); p.add_argument('package'); p.add_argument('name'); p.add_argument('file')
    p = sub.add_parser('ensure-running'); p.add_argument('package')
    p.add_argument('--timeout', type=int, default=600); p.add_argument('--settle', type=int, default=30)
    p = sub.add_parser('job'); p.add_argument('package'); p.add_argument('job_type')
    p.add_argument('--timeout', type=int, default=600)
    # oam 자신을 재기동시키는 job — 진행을 물어볼 상대가 사라지는 것이 정상이다.
    p.add_argument('--expect-restart', action='store_true')
    sub.add_parser('console-bundle')

    args = ap.parse_args()
    admin_pass = os.environ.get('TB_ADMIN_PASS', '')
    if not admin_pass:
        die("TB_ADMIN_PASS 환경변수가 비어 있습니다 (tb-site.conf 의 admin 비밀번호)")

    o = Oam(args.url, admin_pass)
    fn = {'status': cmd_status, 'upload': cmd_upload, 'install': cmd_install,
          'packages': cmd_packages,
          'config': cmd_config, 'collection': cmd_collection, 'job': cmd_job,
          'ensure-running': cmd_ensure_running,
          'console-bundle': cmd_console_bundle}[args.cmd]
    raise SystemExit(fn(o, args) or 0)


if __name__ == '__main__':
    main()
