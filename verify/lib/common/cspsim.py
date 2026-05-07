"""cspsim 호출 helper — cims.sh sim wrapper 경유.

stdout 의 마지막 N 줄 + 종료 코드 반환. tail_lines 는 default 100 — cims.sh
sim 이 시나리오 종료 후 "검증 결과" 블럭을 추가 출력하므로, 30 줄로는 cspsim
본체의 시나리오 완료 마커 ("[Scenario] Subscriptions complete" 등) 가 잘릴
수 있어 100 으로 확장.
"""
from __future__ import annotations

import os
import subprocess

from ..context import sanitized_env


def run_cspsim(repo_root: str, sim_args: list, timeout: int = 120,
               tail_lines: int = 100) -> tuple:
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
        return (proc.returncode, "\n".join(out.splitlines()[-tail_lines:]))
    except subprocess.TimeoutExpired:
        return (-1, "(timeout)")
    except Exception as e:
        return (-2, f"({type(e).__name__}: {e})")
