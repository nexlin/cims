#!/usr/bin/env python3
"""oam-deploy — OAM API 로 패키지 등록·배포 업그레이드·설정 컬렉션 주입을 콘솔 없이 한 번에 한다.

절차 정본: docs/dev/oam_api_deploy_runbook.md. 콘솔의 [패키지 업로드] → [패키지 전환+upgrade job] → [패키지 제어 start] 와
같은 API 를 같은 순서로 부른다(다른 경로가 아니다).

  export OAM_URL=https://127.0.0.1:4419 OAM_LOGIN=admin OAM_PASSWORD=…      # 또는 OAM_TOKEN
  scripts/oam-deploy.py status                                               # 배포 목록(id·모듈·버전·live)
  scripts/oam-deploy.py packages build/dist/packages/csp-0.2.137.tar.gz …     # 등록(패키지 store 로 복사 뒤 POST /packages)
  scripts/oam-deploy.py upgrade 34=61 31=62                                   # dep=pkg … 순서대로 stop → upgrade → start(up 대기)
  scripts/oam-deploy.py upgrade --latest csp csc                              # 모듈의 최신 등록 패키지로
  scripts/oam-deploy.py collection 34 access_services --set country_code=82 --signal   # 레코드 전부에 필드 주입 + SIGUSR1
  scripts/oam-deploy.py collection 34 access_services --file recs.json --signal        # 레코드 통째 교체
  scripts/oam-deploy.py install csp cmp --agent media01 --config csp=csp.json           # 새 배포(생성→install→overlay→start)
  scripts/oam-deploy.py config 31 --file csc.json --restart                             # overlay 저장(+재기동)
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request

_CTX = ssl._create_unverified_context()
_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


class Oam:
    def __init__(self, url: str, token: str):
        self.url = url.rstrip('/') + '/api/v1'
        self.token = token

    def call(self, method: str, path: str, body=None, timeout: int = 60):
        req = urllib.request.Request(self.url + path, data=(json.dumps(body).encode() if body is not None else None),
                                     method=method, headers={'Authorization': 'Bearer ' + self.token,
                                                             'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, context=_CTX, timeout=timeout) as r:
                raw = r.read()
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as e:
            raise SystemExit(f'{method} {path} → HTTP {e.code}: {e.read().decode()[:400]}')

    @staticmethod
    def login(url: str, login_id: str, password: str) -> str:
        req = urllib.request.Request(url.rstrip('/') + '/api/v1/auth/login', method='POST',
                                     data=json.dumps({'login_id': login_id, 'password': password}).encode(),
                                     headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, context=_CTX, timeout=30) as r:
            return json.load(r)['token']

    # ── 조회 ──
    def deployments(self) -> list:
        d = self.call('GET', '/deployments')
        return d if isinstance(d, list) else d.get('items') or d.get('deployments') or []

    def deployment(self, did: int) -> dict:
        return self.call('GET', f'/deployments/{did}')

    def packages(self) -> list:
        d = self.call('GET', '/packages')
        return d if isinstance(d, list) else d.get('items') or d.get('packages') or []


def _token(args) -> Oam:
    url = args.url or os.environ.get('OAM_URL', 'https://127.0.0.1:4419')
    tok = args.token or os.environ.get('OAM_TOKEN', '')
    if not tok:
        login, pw = os.environ.get('OAM_LOGIN', 'admin'), os.environ.get('OAM_PASSWORD', '')
        if not pw:
            raise SystemExit('OAM_TOKEN 또는 OAM_LOGIN/OAM_PASSWORD 가 필요하다')
        tok = Oam.login(url, login, pw)
    return Oam(url, tok)


def _module_of(row: dict) -> str:
    pkg = row.get('package') if isinstance(row.get('package'), dict) else {}
    return row.get('package_name') or row.get('module') or pkg.get('module') or pkg.get('name') or row.get('name') or '?'


def cmd_status(oam: Oam, args) -> int:
    rows = oam.deployments()
    print(f"{'dep':>4} {'module':18} {'version':10} {'status':9} {'live':6} install_path")
    for r in rows:
        if args.all or r.get('live_state'):
            print(f"{r['id']:>4} {_module_of(r):18} {str(r.get('package_version') or ''):10} {str(r.get('status') or ''):9} "
                  f"{str(r.get('live_state') or ''):6} {r.get('install_path') or ''}")
    return 0


def cmd_packages(oam: Oam, args) -> int:
    """tarball 을 OAM 패키지 store(기본 build/dist/oam/packages_tb — 4419 개발 TB 의 store)로 복사한 뒤 POST /packages 로 등록한다.
    파일 경로는 OAM 프로세스가 읽을 수 있는 절대 경로여야 한다(같은 호스트 전제)."""
    store = args.store or os.environ.get('OAM_PACKAGE_STORE', os.path.join(_ROOT, 'build', 'dist', 'oam', 'packages_tb'))
    os.makedirs(store, exist_ok=True)
    for src in args.files:
        if not os.path.isfile(src):
            raise SystemExit(f'없음: {src}')
        dst = os.path.join(store, os.path.basename(src))
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copy2(src, dst)
        body = {'file_path': os.path.abspath(dst)}
        if args.force:
            body['force'] = True
        r = oam.call('POST', '/packages', body)
        print(f"{os.path.basename(src)} → package id={r.get('id')} module={r.get('module') or r.get('name')} version={r.get('version')}")
    return 0


def _wait(oam: Oam, did: int, pred, timeout: int, what: str) -> dict:
    t0 = time.time()
    d = oam.deployment(did)
    while time.time() - t0 < timeout:
        d = oam.deployment(did)
        if pred(d):
            return d
        time.sleep(3)
    print(f"  ! {what} 대기 시한 초과 — live={d.get('live_state')} status={d.get('status')} ver={d.get('package_version')}")
    return d


def _latest_package(oam: Oam, module: str) -> dict:
    cands = [p for p in oam.packages() if (p.get('module') or p.get('name')) == module]
    if not cands:
        raise SystemExit(f'모듈 {module} 의 등록 패키지가 없다')
    return max(cands, key=lambda p: int(p['id']))


def cmd_upgrade(oam: Oam, args) -> int:
    """dep=pkg 쌍(또는 --latest 모듈명)을 순서대로: stop job → live down 대기 → POST /upgrade {package_id} → 버전 반영 대기 →
    up 이 아니면 start job → up 대기. 콘솔이 하는 것과 같은 job 들이다. 같은 호스트에 dev OAM 이 있어 live_state 가 늘 up 인
    base oam(dep29) 은 stop 이 409 로 막힌다 — 그 모듈은 콘솔·별도 절차."""
    plan = []
    rows = {int(r['id']): r for r in oam.deployments()}
    if args.latest:
        for mod in args.targets:
            deps = [r for r in rows.values() if _module_of(r) == mod and r.get('live_state')]
            if not deps:
                raise SystemExit(f'모듈 {mod} 의 live 배포가 없다 — dep=pkg 로 지정한다')
            pkg = _latest_package(oam, mod)
            for r in deps:
                plan.append((int(r['id']), int(pkg['id']), f"{mod} {pkg.get('version')}"))
    else:
        for t in args.targets:
            if '=' not in t:
                raise SystemExit(f'형식은 dep=pkg 다: {t}')
            did, pid = (int(x) for x in t.split('=', 1))
            pkg = next((p for p in oam.packages() if int(p['id']) == pid), {})
            plan.append((did, pid, f"{pkg.get('module') or pkg.get('name') or '?'} {pkg.get('version') or '?'}"))
    rc = 0
    for did, pid, label in plan:
        cur = rows.get(did) or oam.deployment(did)
        print(f"## dep{did} {_module_of(cur)} {cur.get('package_version')} → pkg{pid} ({label})")
        if str(cur.get('package_id')) == str(pid) and not args.force:
            print('  이미 그 패키지 — 건너뜀(--force 로 재적용)')
            continue
        r = oam.call('POST', f'/deployments/{did}/job', {'job_type': 'stop'})
        print(f"  stop job {r.get('job_id')}")
        _wait(oam, did, lambda d: d.get('live_state') == 'down', args.timeout, 'down')
        r = oam.call('POST', f'/deployments/{did}/upgrade', {'package_id': pid})
        print(f"  upgrade job {r.get('job_id')}")
        # upgrade job 은 설치·current 전환까지만 하고 프로세스를 띄우지 않는다(실측 — live 는 down 으로 남는다). 패키지 전환만 기다리고 바로 start
        d = _wait(oam, did, lambda d: str(d.get('package_id')) == str(pid), args.timeout, 'upgrade(package 전환)')
        if str(d.get('package_id')) != str(pid):
            print('  ! 패키지 전환이 반영되지 않았다 — job 결과 확인(GET /agents/{aid}/jobs/{jid})')
            rc = 1
            continue
        time.sleep(3)   # 설치 마무리(current 심볼릭·overlay 머지) 여유
        d = oam.deployment(did)
        if d.get('live_state') != 'up':
            r = oam.call('POST', f'/deployments/{did}/job', {'job_type': 'start'})
            print(f"  start job {r.get('job_id')}")
            d = _wait(oam, did, lambda d: d.get('live_state') == 'up', args.timeout, 'up')
        ok = d.get('live_state') == 'up' and str(d.get('package_id')) == str(pid)
        rc = rc if ok else 1
        print(f"  → {'OK' if ok else 'FAIL'} ver={d.get('package_version')} status={d.get('status')} live={d.get('live_state')} path={d.get('install_path')}")
    return rc


def _agent_id(oam: Oam, ref: str) -> int:
    d = oam.call('GET', '/agents')
    rows = d if isinstance(d, list) else d.get('items') or d.get('agents') or []
    for a in rows:
        if str(a.get('id')) == str(ref) or a.get('name') == ref or a.get('hostname') == ref:
            return int(a['id'])
    raise SystemExit(f'agent {ref!r} 없음 — 목록: ' + ', '.join(f"{a.get('id')}={a.get('name')}" for a in rows))


def cmd_install(oam: Oam, args) -> int:
    """새 배포 = POST /deployments {agent_id, package_id, process_name} → install job(설치·current 전환, 프로세스는 안 띄움)
    → (있으면) PUT config overlay → start job → up 대기. 콘솔 [패키지 설치]→[패키지 설정]→[패키지 제어] 와 같은 API·순서.
    targets = 패키지 id 또는 모듈 이름(그 모듈의 최신 등록 패키지). --config 는 모듈=파일 로 overlay 를 준다(둘 이상 반복)."""
    aid = _agent_id(oam, args.agent)
    cfgs = {}
    for kv in args.config:
        if '=' not in kv:
            raise SystemExit(f'--config 형식은 모듈=파일 이다: {kv}')
        m, f = kv.split('=', 1)
        cfgs[m] = json.load(open(f, encoding='utf-8'))
    pkgs = oam.packages()
    rc = 0
    for t in args.targets:
        pkg = next((p for p in pkgs if str(p['id']) == t), None) if t.isdigit() else _latest_package(oam, t)
        if not pkg:
            raise SystemExit(f'패키지 {t} 없음')
        mod = pkg.get('module') or pkg.get('name') or t
        pid = int(pkg['id'])
        body = {'agent_id': aid, 'package_id': pid, 'process_name': args.process_name or mod, 'service_functions': []}
        if mod in cfgs:
            body['config'] = cfgs[mod]
        r = oam.call('POST', '/deployments', body)
        did = int(r.get('id') or (r.get('deployment') or {}).get('id') or 0)
        if not did:
            raise SystemExit(f'배포 생성 응답에 id 없음: {r}')
        pruned = r.get('pruned_keys') or []
        print(f"## {mod} {pkg.get('version')} → dep{did} (agent {aid}){' — 템플릿 밖 키 제외: ' + ','.join(pruned) if pruned else ''}")
        j = oam.call('POST', f'/deployments/{did}/job', {'job_type': 'install'})
        print(f"  install job {j.get('job_id')}")
        d = _wait(oam, did, lambda d: bool(d.get('install_path')) and d.get('status') in ('stopped', 'running', 'failed'), args.timeout, 'install')
        if d.get('status') == 'failed' or not d.get('install_path'):
            print(f"  ! 설치 실패 status={d.get('status')} — GET /agents/{aid}/jobs/{j.get('job_id')}")
            rc = 1
            continue
        if not args.no_start and d.get('live_state') != 'up':
            j = oam.call('POST', f'/deployments/{did}/job', {'job_type': 'start'})
            print(f"  start job {j.get('job_id')}")
            d = _wait(oam, did, lambda d: d.get('live_state') == 'up', args.timeout, 'up')
        ok = d.get('live_state') == 'up' or args.no_start
        rc = rc if ok else 1
        print(f"  → {'OK' if ok else 'FAIL'} dep{did} ver={d.get('package_version')} status={d.get('status')} live={d.get('live_state')} path={d.get('install_path')}")
    return rc


def cmd_config(oam: Oam, args) -> int:
    """배포 overlay 저장 — PUT /deployments/{id}/config {config} (변경분 병합). --file 의 JSON 을 그대로 보낸다. --restart 면 restart job."""
    cfg = json.load(open(args.file, encoding='utf-8'))
    r = oam.call('PUT', f'/deployments/{args.dep}/config', {'config': cfg, 'queue_update': False})
    pruned = r.get('pruned_keys') or []
    print(f"dep{args.dep} config 저장 — 키 {len(cfg)}개{' (템플릿 밖 제외: ' + ','.join(pruned) + ')' if pruned else ''}")
    if args.restart:
        j = oam.call('POST', f'/deployments/{args.dep}/job', {'job_type': 'restart'})
        print(f"  restart job {j.get('job_id')}")
        d = _wait(oam, args.dep, lambda d: d.get('live_state') == 'up', args.timeout, 'up')
        print(f"  → live={d.get('live_state')}")
    return 0


def cmd_collection(oam: Oam, args) -> int:
    """배포 모듈 컬렉션(csp local_nodes/access_services/routes… — config_template.json collections) 을 PUT 한다.
    --set k=v … 는 현재 레코드 전부에 필드를 주입(값은 JSON 이면 파싱, 아니면 문자열), --file 은 레코드 배열 통째 교체.
    --signal 이면 CSP SIGUSR1 리로드까지(재기동 없음)."""
    if args.file:
        records = json.load(open(args.file, encoding='utf-8'))
        if isinstance(records, dict):
            records = records.get('records') or []
    else:
        records = oam.call('GET', f'/deployments/{args.dep}/collection/{args.name}').get('records') or []
        if not args.set:
            print(json.dumps(records, ensure_ascii=False, indent=1))
            return 0
        for kv in args.set:
            k, v = kv.split('=', 1)
            try:
                v = json.loads(v)
            except ValueError:
                pass
            for rec in records:
                rec[k] = v
    r = oam.call('PUT', f'/deployments/{args.dep}/collection/{args.name}', {'records': records, 'signal': bool(args.signal)})
    print(f"PUT {args.name}: ok={r.get('ok')} count={r.get('count')} signaled={r.get('signaled')}")
    return 0 if r.get('ok', True) else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--url', help='OAM (기본 $OAM_URL 또는 https://127.0.0.1:4419)')
    ap.add_argument('--token', help='로그인 토큰 (기본 $OAM_TOKEN, 없으면 $OAM_LOGIN/$OAM_PASSWORD 로 로그인)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('status', help='배포 목록'); s.add_argument('--all', action='store_true', help='live 없는 옛 배포까지')
    p = sub.add_parser('packages', help='tarball 등록'); p.add_argument('files', nargs='+'); p.add_argument('--store'); p.add_argument('--force', action='store_true')
    u = sub.add_parser('upgrade', help='dep=pkg … 또는 --latest 모듈 …')
    u.add_argument('targets', nargs='+'); u.add_argument('--latest', action='store_true'); u.add_argument('--force', action='store_true')
    u.add_argument('--timeout', type=int, default=120, help='단계별 대기 상한(초)')
    c = sub.add_parser('collection', help='컬렉션 조회/주입'); c.add_argument('dep', type=int); c.add_argument('name')
    c.add_argument('--set', action='append', default=[], metavar='k=v'); c.add_argument('--file'); c.add_argument('--signal', action='store_true')
    i = sub.add_parser('install', help='새 배포 — 패키지 id 또는 모듈 이름 … (생성 → install → overlay → start)')
    i.add_argument('targets', nargs='+'); i.add_argument('--agent', required=True, help='agent id 또는 이름')
    i.add_argument('--config', action='append', default=[], metavar='모듈=overlay.json'); i.add_argument('--process-name')
    i.add_argument('--no-start', action='store_true'); i.add_argument('--timeout', type=int, default=180)
    g = sub.add_parser('config', help='배포 overlay 저장 (--file JSON) [--restart]'); g.add_argument('dep', type=int); g.add_argument('--file', required=True)
    g.add_argument('--restart', action='store_true'); g.add_argument('--timeout', type=int, default=120)
    args = ap.parse_args(argv)
    oam = _token(args)
    return {'status': cmd_status, 'packages': cmd_packages, 'upgrade': cmd_upgrade, 'collection': cmd_collection,
            'install': cmd_install, 'config': cmd_config}[args.cmd](oam, args)


if __name__ == '__main__':
    sys.exit(main())
