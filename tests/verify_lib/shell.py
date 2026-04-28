"""cims.sh sub-function 호출 helper.

점진 마이그레이션 단계에서 verify_lib 항목 함수가 cims.sh 의 cmd_xxx 함수를
직접 호출할 수 있도록 subprocess wrapper 제공. sanitized env 적용.
"""
from __future__ import annotations

import os
import shlex
import subprocess
from typing import Optional

from .context import sanitized_env


def run(cmd: list, cwd: Optional[str] = None, timeout: int = 600,
        capture: bool = True) -> tuple:
    """일반 shell 명령 실행. (returncode, stdout, stderr) 반환.

    capture=False 면 stdout/stderr 는 빈 문자열, 출력은 그대로 흘러감.
    """
    if capture:
        proc = subprocess.run(
            cmd, cwd=cwd, env=sanitized_env(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, text=True,
        )
        return (proc.returncode, proc.stdout, proc.stderr)
    else:
        proc = subprocess.run(cmd, cwd=cwd, env=sanitized_env(), timeout=timeout)
        return (proc.returncode, "", "")


def run_cims_sh(repo_root: str, *args: str, capture: bool = True,
                timeout: int = 600) -> tuple:
    """cims.sh 호출. (rc, stdout, stderr) 반환."""
    cims_sh = os.path.join(repo_root, "cims.sh")
    cmd = ["/bin/bash", cims_sh] + list(args)
    return run(cmd, cwd=repo_root, capture=capture, timeout=timeout)


def cmd_sim(repo_root: str, *sim_args: str, timeout: int = 120) -> tuple:
    """cims.sh cmd_sim 래퍼."""
    return run_cims_sh(repo_root, "sim", *sim_args, capture=True, timeout=timeout)


def port_listening(port: int, proto: str = "tcp") -> bool:
    """ss -tln/-uln 으로 LISTEN 확인."""
    flag = "-uln" if proto == "udp" else "-tln"
    try:
        out = subprocess.check_output(
            ["ss", flag], stderr=subprocess.DEVNULL, timeout=2,
        ).decode("utf-8", errors="replace")
    except Exception:
        return False
    needle = f":{port}"
    for line in out.splitlines():
        cols = line.split()
        if len(cols) < 4: continue
        addr = cols[3]
        # endswith 으로 :PORT 매칭 (앞이 IP)
        if addr.endswith(needle):
            return True
    return False
