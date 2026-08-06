"""base OAM REST 클라이언트 (auto_deployment.md §2).

provisioner 는 OAM 의 file_store 를 직접 건드리지 않고 **공개 REST 만** 호출한다 —
게이트웨이 라우트 재등록·JwtSecret 주입·install_history 기록 같은 OAM 소유 부수효과를
우회하지 않기 위함이다.

인증: 운영자가 콘솔에서 배포를 실행할 때 쓴 admin JWT 를 그대로 전달한다. provisioner 는
토큰을 발급하지 않는다(자격증명 발급 주체를 늘리지 않는다). 대신 긴 run 도중 토큰이 만료되면
`OamError.code == 'auth_expired'` 로 실패하며, 콘솔에서 재로그인 후 [재개]하면 그 지점부터
이어진다.

전송은 stdlib `urllib.request` 를 쓴다 — SSH 실행이 블로킹 스레드에서 도는 구조라 async
HTTP 를 섞을 이유가 없고, vendor 의존도 늘지 않는다.
"""

from __future__ import annotations

import json
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request


class OamError(Exception):
    def __init__(self, code: str, message: str, status: int = 0, body=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.body = body

    def as_dict(self) -> dict:
        return {'code': self.code, 'message': self.message, 'status': self.status}


class OamClient:
    def __init__(self, base_url: str, token: str, *, timeout: int = 30, verify_tls: bool = False):
        self.base = base_url.rstrip('/')
        self.token = token
        self.timeout = timeout
        # base OAM 은 기본 self-signed(설치 시 생성)이고 호출은 통상 loopback 이다.
        # 상용 인증서를 넣은 사이트는 verify_tls=True 로 켠다.
        if verify_tls:
            self._ctx = ssl.create_default_context()
        else:
            self._ctx = ssl.create_default_context()
            self._ctx.check_hostname = False
            self._ctx.verify_mode = ssl.CERT_NONE

    # ── 저수준 ──────────────────────────────────────────────────
    def request(self, method: str, path: str, body=None, *, params: dict | None = None):
        url = self.base + path
        if params:
            url += '?' + urllib.parse.urlencode(params)
        data = None
        headers = {'Accept': 'application/json'}
        if self.token:
            headers['Authorization'] = f'Bearer {self.token}'
        if body is not None:
            data = json.dumps(body).encode('utf-8')
            headers['Content-Type'] = 'application/json'

        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=self._ctx) as resp:
                raw = resp.read()
                if not raw:
                    return None
                try:
                    return json.loads(raw.decode('utf-8'))
                except ValueError:
                    return raw.decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                parsed = json.loads(raw.decode('utf-8'))
            except Exception:
                parsed = raw.decode('utf-8', 'replace')
            if e.code in (401, 403):
                raise OamError('auth_expired',
                               'OAM 인증 실패 — 콘솔에서 재로그인 후 [재개]하십시오',
                               e.code, parsed) from None
            detail = parsed.get('detail') or parsed.get('error') if isinstance(parsed, dict) else parsed
            raise OamError('oam_http_error', f'{method} {path} → {e.code}: {detail}',
                           e.code, parsed) from None
        except urllib.error.URLError as e:
            raise OamError('oam_unreachable', f'OAM 에 연결할 수 없음 ({self.base}): {e.reason}') from None

    def get(self, path, **kw):    return self.request('GET', path, **kw)
    def post(self, path, body=None, **kw):  return self.request('POST', path, body, **kw)
    def put(self, path, body=None, **kw):   return self.request('PUT', path, body, **kw)
    def delete(self, path, **kw): return self.request('DELETE', path, **kw)

    # ── agents ──────────────────────────────────────────────────
    def list_agents(self) -> list:
        r = self.get('/api/v1/agents')
        if isinstance(r, dict):
            return r.get('agents') or r.get('items') or []
        return r or []

    def find_agent(self, name: str):
        for a in self.list_agents():
            if a.get('name') == name:
                return a
        return None

    def create_agent(self, name: str, note: str | None = None) -> dict:
        """새 agent 레코드 + enrollment_token 발급.

        enrollment_token 은 **생성 응답에서만** 돌려받을 수 있고 TTL 이 짧다(기본 10분).
        그래서 호출부는 서버 하나를 설치하기 직전에 발급받는다 — run 시작 시점에 전부
        미리 발급하면 뒤쪽 서버에서 만료된다.
        """
        return self.post('/api/v1/agents', {'name': name, 'note': note or 'provisioner'})

    def regenerate_enrollment_token(self, agent_id: int) -> dict:
        """토큰 재발급. 기존 토큰이 아직 유효하면 OAM 이 409 로 거절한다."""
        return self.post(f'/api/v1/agents/{agent_id}/regenerate-token')

    def ensure_enrollment_token(self, name: str) -> tuple:
        """(agent_id, enrollment_token, reused) — 멱등 진입점.

        - agent 없음        → 새로 만들고 토큰 반환
        - 있고 online       → (id, None, True). 호출부가 설치를 skip 한다
        - 있고 미enroll     → 토큰 재발급 시도. 아직 유효해 409 면 재사용 불가이므로
                              만료를 기다리는 대신 오류로 올린다(운영자가 판단).
        """
        existing = self.find_agent(name)
        if not existing:
            created = self.create_agent(name)
            return created.get('id'), created.get('enrollment_token'), False
        if existing.get('status') == 'online':
            return existing.get('id'), None, True
        try:
            r = self.regenerate_enrollment_token(existing['id'])
            return existing['id'], r.get('enrollment_token'), False
        except OamError as e:
            if e.status == 409:
                raise OamError(
                    'enrollment_token_unavailable',
                    f"'{name}' 의 기존 enrollment 토큰이 아직 유효해 재발급이 거절됐다 "
                    f'— 직전 설치 시도가 진행 중이거나, 토큰 만료(기본 10분) 후 재개 필요',
                    e.status, e.body) from None
            raise

    def wait_agent_online(self, name: str, timeout_sec: int, poll_sec: int = 3) -> dict:
        """enroll → heartbeat 로 online 전이할 때까지 대기."""
        deadline = time.time() + timeout_sec
        last = None
        while time.time() < deadline:
            a = self.find_agent(name)
            if a:
                last = a.get('status')
                if last == 'online':
                    return a
                if last == 'revoked':
                    raise OamError('agent_revoked', f"'{name}' 이 revoked 상태다")
            time.sleep(poll_sec)
        raise OamError('enroll_timeout',
                       f"'{name}' 이 {timeout_sec}초 내에 online 이 되지 않음 "
                       f'(현재 {last or "레코드 없음"}) — 대상 노드에서 '
                       f'`systemctl --user status cims-agent` 확인')

    # ── packages ────────────────────────────────────────────────
    def list_packages(self) -> list:
        r = self.get('/api/v1/packages')
        if isinstance(r, dict):
            return r.get('packages') or r.get('items') or []
        return r or []

    def resolve_package(self, name: str, version: str) -> dict:
        """(name, version) → 패키지 레코드. 'latest' 는 저장소 최신으로 확정(pin).

        deployment 생성이 package_id 를 요구하므로 버전 문자열이 아니라 레코드를 돌려준다.
        """
        cands = [p for p in self.list_packages() if p.get('name') == name and p.get('version')]
        if not cands:
            raise OamError('package_missing',
                           f"패키지 '{name}' 이 OAM 저장소에 없음 "
                           f'— [관리 > 시스템 > 패키지] 에 업로드 후 재개')
        if version != 'latest':
            for p in cands:
                if str(p['version']) == str(version):
                    return p
            raise OamError('package_missing',
                           f"패키지 '{name}' 의 버전 {version} 없음 "
                           f"(보유: {', '.join(sorted(str(c['version']) for c in cands))})")
        return sorted(cands, key=lambda p: _version_key(p['version']))[-1]

    # ── ha-groups ───────────────────────────────────────────────
    def list_ha_groups(self) -> list:
        r = self.get('/api/v1/ha-groups')
        if isinstance(r, dict):
            return r.get('groups') or r.get('ha_groups') or r.get('items') or []
        return r or []

    def find_ha_group(self, name: str):
        for g in self.list_ha_groups():
            if g.get('name') == name:
                return g
        return None

    def create_ha_group(self, body: dict) -> dict:
        return self.post('/api/v1/ha-groups', body)

    def update_ha_group(self, gid: int, body: dict) -> dict:
        return self.put(f'/api/v1/ha-groups/{gid}', body)

    # ── deployments ─────────────────────────────────────────────
    def list_deployments(self) -> list:
        r = self.get('/api/v1/deployments')
        if isinstance(r, dict):
            return r.get('deployments') or r.get('items') or []
        return r or []

    def find_deployment(self, agent_id: int, package_name: str):
        """(agent, package) 로 기존 배포를 찾는다 — 멱등 판정의 기준.
        removed 는 없는 것으로 본다(재배포 가능)."""
        for d in self.list_deployments():
            if d.get('agent_id') != agent_id:
                continue
            if d.get('status') == 'removed':
                continue
            if (d.get('package_name') or d.get('package') or '') == package_name:
                return d
        return None

    def create_deployment(self, *, agent_id: int, package_id: int, process_name: str,
                          config: dict | None = None, note: str = 'provisioner') -> dict:
        return self.post('/api/v1/deployments', {
            'agent_id': agent_id, 'package_id': package_id,
            'process_name': process_name, 'config': config or {}, 'note': note})

    def update_deployment(self, did: int, body: dict) -> dict:
        return self.put(f'/api/v1/deployments/{did}', body)

    def get_deployment(self, did: int) -> dict:
        return self.get(f'/api/v1/deployments/{did}')

    def put_collection(self, did: int, name: str, records: list, *, signal: bool = True):
        return self.put(f'/api/v1/deployments/{did}/collection/{name}',
                        {'records': records, 'signal': signal})

    # ── jobs ────────────────────────────────────────────────────
    def queue_job(self, did: int, job_type: str, extra: dict | None = None) -> int:
        body = {'job_type': job_type}
        if extra:
            body['extra'] = extra
        r = self.post(f'/api/v1/deployments/{did}/job', body)
        jid = (r or {}).get('job_id')
        if not jid:
            raise OamError('job_queue_failed', f'{job_type} job 큐잉 응답에 job_id 없음: {r}')
        return jid

    def wait_job(self, agent_id: int, job_id: int, timeout_sec: int,
                 poll_sec: int = 2) -> dict:
        """job 이 종료 상태(succeeded/failed/cancelled)가 될 때까지 대기.

        OAM job 은 fire-and-forget 이라 완료 게이팅이 없다 — provisioner 가 여기서
        폴링해 순서를 만든다(§1 '지금과 뭐가 다른가').
        """
        deadline = time.time() + timeout_sec
        last = None
        while time.time() < deadline:
            j = self.get(f'/api/v1/agents/{agent_id}/jobs/{job_id}')
            if isinstance(j, dict):
                last = j.get('status')
                if last in ('succeeded', 'failed', 'cancelled'):
                    return j
            time.sleep(poll_sec)
        raise OamError('job_timeout',
                       f'job#{job_id} 이 {timeout_sec}초 내에 끝나지 않음 (현재 {last})')

    def run_job(self, *, deployment_id: int, agent_id: int, job_type: str,
                timeout_sec: int, poll_sec: int = 2, extra: dict | None = None) -> dict:
        """큐잉 + 완료 대기 + 실패 시 stderr 를 그대로 올린다."""
        jid = self.queue_job(deployment_id, job_type, extra)
        j = self.wait_job(agent_id, jid, timeout_sec, poll_sec)
        if j.get('status') != 'succeeded':
            err = (j.get('result_stderr') or j.get('result_stdout') or '').strip()
            tail = ' / '.join(err.splitlines()[-3:]) if err else '출력 없음'
            raise OamError(f'{job_type}_job_failed',
                           f"{job_type} job#{jid} {j.get('status')} "
                           f"(rc={j.get('result_code')}) — {tail}")
        j['job_id'] = jid
        return j


def _version_key(v: str):
    out = []
    for part in str(v).split('.'):
        try:
            out.append(int(part))
        except ValueError:
            out.append(-1)
    return out
