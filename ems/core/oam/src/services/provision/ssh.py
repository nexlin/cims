"""SSH 러너 — agent 자동 설치용 원격 실행 (auto_deployment.md §5).

`sshpass` 의존 없이 stock OpenSSH 로 동작한다:
  - 비밀번호 인증 : SSH_ASKPASS + SSH_ASKPASS_REQUIRE=force (OpenSSH 8.4+)
  - sudo 비밀번호 : `sudo -S -p ''` 의 **stdin** 으로 주입

**비밀번호 인증 전용**이다 — 운영 환경이 SSH 키를 쓰지 않으므로 키/ssh-agent 경로는 두지
않는다. agent 가 이미 설치된 노드(OAM 부트스트랩 노드)는 애초에 SSH 대상이 아니다
(inventory 의 agent_preinstalled).

비밀값 취급 불변식:
  I1. 비밀번호는 argv 에 절대 싣지 않는다 (`/proc/<pid>/cmdline` 은 동일 uid 에 노출).
  I2. 비밀번호는 0600 파일(0700 디렉토리) 또는 파이프로만 전달하고, 사용 직후 삭제한다.
  I3. 러너를 통과하는 모든 문자열(stdout/stderr/예외 메시지)은 반환 전에 마스킹한다.

원격 명령의 stdin 은 sudo 비밀번호 전용이다. 따라서 스크립트는 `bash -s` 로 흘려넣지 않고
**scp 로 올린 뒤 실행**한다 — stdin 이 두 용도로 경합하면 sudo 가 스크립트 본문을 비밀번호로
읽어버린다.
"""

from __future__ import annotations

import os
import shlex
import shutil
import stat
import subprocess
import tempfile


class SshError(Exception):
    """접속·인증 실패 등 명령 실행 이전 단계의 오류. message 는 마스킹 완료 상태."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code            # ssh_auth_failed | host_key_changed | ssh_unreachable | ...
        self.message = message


class Result:
    __slots__ = ('rc', 'stdout', 'stderr')

    def __init__(self, rc: int, stdout: str, stderr: str):
        self.rc = rc
        self.stdout = stdout
        self.stderr = stderr

    @property
    def ok(self) -> bool:
        return self.rc == 0

    def __repr__(self):
        return f'<Result rc={self.rc} out={len(self.stdout)}B err={len(self.stderr)}B>'


def mask(text: str, secrets) -> str:
    """알려진 비밀값을 치환. 러너 밖으로 나가는 모든 문자열이 이 함수를 통과한다."""
    if not text:
        return text
    for s in secrets or ():
        if s and len(s) >= 3:       # 너무 짧은 값은 오히려 오탐 치환을 만든다
            text = text.replace(s, '••••')
    return text


# ── 진단 가능한 실패로의 매핑 ─────────────────────────────────────
# ssh 는 인증 실패도 255 로 뭉뚱그리므로 stderr 문구로 분류한다.
_ERR_PATTERNS = (
    ('host_key_changed', ('REMOTE HOST IDENTIFICATION HAS CHANGED',
                          'Host key verification failed')),
    ('ssh_auth_failed',  ('Permission denied', 'Too many authentication failures',
                          'No supported authentication methods')),
    ('ssh_unreachable',  ('Connection refused', 'No route to host', 'Network is unreachable',
                          'Connection timed out', 'Name or service not known',
                          'Operation timed out')),
)


def _classify(stderr: str) -> str | None:
    for code, needles in _ERR_PATTERNS:
        for n in needles:
            if n in stderr:
                return code
    return None


class SshTarget:
    """서버 1대에 대한 SSH 실행기. `with` 로 감싸 임시 자격증명 파일을 확실히 회수한다."""

    def __init__(self, server, *, connect_timeout: int = 15, command_timeout: int = 900,
                 strict_host_key: str = 'accept-new', known_hosts: str | None = None):
        self.s = server                       # schema.Server
        self.connect_timeout = connect_timeout
        self.command_timeout = command_timeout
        self.strict_host_key = strict_host_key
        self.known_hosts = known_hosts
        self._tmpdir: str | None = None
        self._askpass: str | None = None
        self._secrets = server.secrets()

    # ── 생명주기 ────────────────────────────────────────────────
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self):
        if self._tmpdir and os.path.isdir(self._tmpdir):
            shutil.rmtree(self._tmpdir, ignore_errors=True)
        self._tmpdir = None
        self._askpass = None

    # ── 자격증명 전달 매체 ──────────────────────────────────────
    def _ensure_tmpdir(self) -> str:
        if not self._tmpdir:
            self._tmpdir = tempfile.mkdtemp(prefix='cims-prov-')
            os.chmod(self._tmpdir, stat.S_IRWXU)          # 0700
        return self._tmpdir

    def _ensure_askpass(self) -> str:
        """SSH_ASKPASS 헬퍼 생성. 비밀번호는 헬퍼 **본문이 아니라** 0600 파일에 두고
        헬퍼가 읽어 출력한다 — 셸 따옴표 이스케이프 사고를 원천 차단."""
        if self._askpass:
            return self._askpass
        d = self._ensure_tmpdir()
        pw_file = os.path.join(d, 'ssh.pw')
        fd = os.open(pw_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(self.s.ssh_password or '')            # 개행 없이 — ssh 는 첫 줄을 쓴다
        helper = os.path.join(d, 'askpass.sh')
        fd = os.open(helper, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o700)
        with os.fdopen(fd, 'w') as f:
            f.write('#!/bin/sh\nexec cat "$CIMS_PROV_PW_FILE"\n')
        self._askpass = helper
        self._pw_file = pw_file
        return helper

    # ── 명령 조립 ───────────────────────────────────────────────
    def _base_opts(self) -> list:
        opts = [
            '-o', f'ConnectTimeout={self.connect_timeout}',
            '-o', f'StrictHostKeyChecking={self.strict_host_key}',
            '-o', 'LogLevel=ERROR',
        ]
        if self.known_hosts:
            opts += ['-o', f'UserKnownHostsFile={self.known_hosts}']
        # 비밀번호 인증 전용 (운영 환경에 SSH 키 미사용).
        # BatchMode=yes 는 askpass 를 무력화하므로 명시적으로 끈다. PubkeyAuthentication=no 는
        # 로컬에 우연히 있는 키가 먼저 시도되어 'Too many authentication failures' 로
        # 끊기는 것을 막는다.
        opts += ['-o', 'BatchMode=no',
                 '-o', 'NumberOfPasswordPrompts=1',
                 '-o', 'PubkeyAuthentication=no',
                 '-o', 'PreferredAuthentications=password,keyboard-interactive']
        return opts

    def _env(self) -> dict:
        env = dict(os.environ)
        helper = self._ensure_askpass()
        env['SSH_ASKPASS'] = helper
        env['SSH_ASKPASS_REQUIRE'] = 'force'          # OpenSSH 8.4+ — TTY 없이 동작
        env['CIMS_PROV_PW_FILE'] = self._pw_file
        env.setdefault('DISPLAY', ':0')               # 구버전 ssh 의 askpass 전제 충족
        env.pop('SSH_AUTH_SOCK', None)                # ssh-agent 키가 먼저 시도되지 않게
        return env

    def _spawn(self, argv: list, stdin_data: bytes | None, timeout: int) -> Result:
        try:
            p = subprocess.run(
                argv, env=self._env(), input=stdin_data,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise SshError('ssh_timeout',
                           f'{self.s.name}: 원격 명령이 {timeout}초 내에 끝나지 않음') from None
        except FileNotFoundError:
            raise SshError('ssh_missing', 'ssh 클라이언트를 찾을 수 없음') from None

        out = mask(p.stdout.decode('utf-8', 'replace'), self._secrets)
        err = mask(p.stderr.decode('utf-8', 'replace'), self._secrets)
        if p.returncode == 255:
            code = _classify(err)
            if code:
                raise SshError(code, f'{self.s.name}: {err.strip() or code}')
        return Result(p.returncode, out, err)

    # ── 공개 동작 ───────────────────────────────────────────────
    def run(self, command: str, *, sudo: bool = False, timeout: int | None = None) -> Result:
        """원격 셸 명령 실행. sudo=True 면 root 로 승격해 실행한다.

        sudo 비밀번호는 stdin 으로만 흐른다 — 원격 argv 에도, 로컬 argv 에도 남지 않는다.
        """
        argv = ['ssh', '-T'] + self._base_opts() + \
               ['-p', str(self.s.ssh_port), f'{self.s.ssh_user}@{self.s.host}']

        stdin_data = None
        if sudo:
            if self.s.sudo_method == 'nopasswd':
                remote = f'sudo -n {command}'
            else:
                remote = f'sudo -S -p "" {command}'
                stdin_data = ((self.s.sudo_password or '') + '\n').encode()
        else:
            remote = command
        argv.append(remote)

        r = self._spawn(argv, stdin_data, timeout or self.command_timeout)
        if sudo and r.rc != 0 and (
                'incorrect password' in r.stderr or 'Sorry, try again' in r.stderr
                or 'a password is required' in r.stderr
                or 'not in the sudoers file' in r.stderr):
            raise SshError('sudo_failed', f'{self.s.name}: sudo 실패 — {r.stderr.strip()}')
        return r

    def put(self, local_path: str, remote_path: str, *, mode: str | None = None) -> Result:
        """scp 로 파일 전송. 전송 대상은 sudo 없이 접근 가능한 경로여야 한다(보통 /tmp)."""
        argv = ['scp'] + self._base_opts() + \
               ['-P', str(self.s.ssh_port), local_path,
                f'{self.s.ssh_user}@{self.s.host}:{remote_path}']
        r = self._spawn(argv, None, self.command_timeout)
        if r.rc != 0:
            raise SshError('scp_failed',
                           f'{self.s.name}: 파일 전송 실패 ({remote_path}) — {r.stderr.strip()}')
        if mode:
            self.run(f'chmod {mode} {shlex.quote(remote_path)}')
        return r

    def preflight(self) -> dict:
        """접속·sudo·OS 를 바꾸지 않고 확인만 한다. dry-run 과 콘솔 [접속 확인] 이 쓴다."""
        info: dict = {'server': self.s.name, 'host': self.s.host,
                      'auth_mode': self.s.auth_mode, 'ok': False}
        r = self.run('echo __cims_ok__; uname -s; id -un', timeout=self.connect_timeout + 10)
        if r.rc != 0 or '__cims_ok__' not in r.stdout:
            info['error'] = r.stderr.strip() or f'rc={r.rc}'
            return info
        lines = [l for l in r.stdout.splitlines() if l and l != '__cims_ok__']
        info['os'] = lines[0] if lines else ''
        info['login_user'] = lines[1] if len(lines) > 1 else ''

        s = self.run('id -un', sudo=True, timeout=self.connect_timeout + 10)
        info['sudo_ok'] = (s.rc == 0 and 'root' in s.stdout)
        if not info['sudo_ok']:
            info['error'] = s.stderr.strip() or 'sudo 승격 실패'
            return info
        info['ok'] = True
        return info


def preflight_all(servers, *, max_parallel: int = 8, **kw) -> list:
    """여러 서버를 동시에 점검. 실패해도 전체를 끝까지 돌려 문제 서버를 한 번에 보여준다.

    agent_preinstalled 노드는 SSH 대상이 아니므로 접속을 시도하지 않고 그 사실만 보고한다.
    """
    from concurrent.futures import ThreadPoolExecutor

    def one(srv):
        if getattr(srv, 'agent_preinstalled', False):
            return {'server': srv.name, 'host': srv.host, 'auth_mode': 'preinstalled',
                    'ok': True, 'os': '-', 'login_user': '-', 'sudo_ok': True,
                    'note': 'agent 기설치 — SSH 미사용'}
        try:
            with SshTarget(srv, **kw) as t:
                return t.preflight()
        except SshError as e:
            return {'server': srv.name, 'host': srv.host, 'auth_mode': srv.auth_mode,
                    'ok': False, 'error_code': e.code, 'error': e.message}

    with ThreadPoolExecutor(max_workers=max(1, min(max_parallel, len(servers) or 1))) as ex:
        return list(ex.map(one, servers))
