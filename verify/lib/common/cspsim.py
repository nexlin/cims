"""cspsim 호출 helper — cims.sh sim wrapper 경유.

stdout 의 마지막 N 줄 + 종료 코드 반환. tail_lines 는 default 100 — cims.sh
sim 이 시나리오 종료 후 "검증 결과" 블럭을 추가 출력하므로, 30 줄로는 cspsim
본체의 시나리오 완료 마커 ("[Scenario] Subscriptions complete" 등) 가 잘릴
수 있어 100 으로 확장.

cspsim 은 30~60 초 단위 시나리오를 진행하면서 REGISTER/SUBSCRIBE/INVITE/PTT
floor rotation 등 다수의 stdout 라인을 출력한다. 본 helper 는 진행 중에 부모
stdout 으로 라인을 즉시 echo 하여:
  1. backend verify job log (`/tmp/cims_verify_jobs/stage*.log`) 가 진행
     중에도 누적되어 console UI 의 polling 응답에 반영된다 (LIVE 진행 표시).
  2. CLI 에서 verify 회차 진행 시에도 cspsim 진행 상황이 즉시 보인다.

종료 후 마지막 tail_lines 줄을 컴팩트한 detail 로 반환하여 시나리오 항목의
detail 본문에 그대로 사용한다.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from collections import deque

from ..context import sanitized_env


def run_cspsim(repo_root: str, sim_args: list, timeout: int = 120,
               tail_lines: int = 100, on_line=None) -> tuple:
    """`cims.sh sim ARGS...` 실행. (returncode, stdout_tail) 반환.

    Popen 으로 라인 단위 stream:
      · 부모 stdout 으로 즉시 echo (verify backend job log 누적)
      · 마지막 tail_lines 줄을 ring-buffer 로 보관 → 종료 시 반환
      · on_line(line) 이 있으면 각 줄마다 호출 — cspsim 이 살아있는 동안(예: `-hold` 등록 유지 창)
        외부 프로브를 걸어야 하는 항목이 마커 줄에 반응한다. 예외는 삼키고 계속 읽는다.
    timeout 도달 시 SIGKILL + (-1, '(timeout)') 반환.
    """
    cims_sh = os.path.join(repo_root, "cims.sh")
    cmd = ["/bin/bash", cims_sh, "sim"] + sim_args
    try:
        proc = subprocess.Popen(
            cmd, cwd=repo_root, env=sanitized_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,  # line-buffered
        )
    except Exception as e:
        return (-2, f"({type(e).__name__}: {e})")

    tail: deque = deque(maxlen=max(tail_lines, 1))
    deadline = time.time() + timeout
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            # 즉시 부모 stdout 으로 echo — backend job log 가 progressive 갱신
            try:
                sys.stdout.write(line)
                sys.stdout.flush()
            except Exception:
                pass
            tail.append(line.rstrip("\n"))
            if on_line is not None:
                try:
                    on_line(line.rstrip("\n"))
                except Exception as e:  # 프로브 실패는 항목이 결과로 판정한다
                    tail.append(f"[VERIFY-CSPSIM] on_line error: {type(e).__name__}: {e}")
            if time.time() >= deadline:
                try: proc.kill()
                except Exception: pass
                tail.append(f"[VERIFY-CSPSIM] timeout after {timeout}s — killed")
                return (-1, "\n".join(tail))
        proc.wait(timeout=max(deadline - time.time(), 1.0))
        return (proc.returncode, "\n".join(tail))
    except subprocess.TimeoutExpired:
        try: proc.kill()
        except Exception: pass
        return (-1, "\n".join(tail) if tail else "(timeout)")
    except Exception as e:
        try: proc.kill()
        except Exception: pass
        return (-2, "\n".join(tail) + f"\n({type(e).__name__}: {e})")
