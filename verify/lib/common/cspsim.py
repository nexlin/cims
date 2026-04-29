"""cspsim 호출 helper — cims.sh sim wrapper 경유.

stdout 마지막 30줄 + 종료 코드 반환.
"""
from __future__ import annotations

import os
import subprocess

from ..context import sanitized_env


def run_cspsim(repo_root: str, sim_args: list, timeout: int = 120) -> tuple:
    """`cims.sh sim ARGS...` 실행. (returncode, stdout_tail) 반환."""
    cims_sh = os.path.join(repo_root, "cims.sh")
    cmd = ["/bin/bash", cims_sh, "sim"] + sim_args
    try:
        proc = subprocess.run(
            cmd, cwd=repo_root, env=sanitized_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, text=True,
        )
        out = proc.stdout or ""
        return (proc.returncode, "\n".join(out.splitlines()[-30:]))
    except subprocess.TimeoutExpired:
        return (-1, "(timeout)")
    except Exception as e:
        return (-2, f"({type(e).__name__}: {e})")
