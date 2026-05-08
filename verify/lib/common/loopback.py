"""loopback IP alias helper — multi-instance topology 용.

PSP/PMP 같은 분리 인스턴스가 127.0.0.3 등 별도 IP 에 LISTEN 하려면 사전에
`ip addr add` 로 alias 등록 필요. 멱등 (이미 있으면 no-op). sudo 권한 없거나
실패 시 호출자가 검증/SKIP 결정.

`required_aliases(instances)` 가 _INSTANCES 의 local_ip 중 127.0.0.1 이 아닌
값들을 unique list 로 반환 — verify pre-step 이 이 list 를 받아 alias 검사.
"""
from __future__ import annotations

import re
import subprocess
from typing import Optional


def has_alias(ip: str) -> bool:
    """`ip -4 addr show dev lo` 에 inet <ip>/ 가 있으면 True."""
    try:
        out = subprocess.run(
            ["ip", "-4", "addr", "show", "dev", "lo"],
            capture_output=True, text=True, timeout=5,
        ).stdout
    except Exception:
        return False
    return bool(re.search(rf"\binet\s+{re.escape(ip)}/", out))


def ensure_alias(ip: str, prefix: int = 8) -> tuple:
    """idempotent. (status, msg).

    status 값:
      - "exists"  — 이미 alias 있음 (no-op)
      - "added"   — 새로 추가 성공
      - "no-sudo" — sudo -n 미허용 (사용자 1회 수동 필요)
      - "error"   — 그 외 실패
    """
    if has_alias(ip):
        return ("exists", f"loopback alias 이미 존재: {ip}/{prefix}")
    try:
        r = subprocess.run(
            ["sudo", "-n", "ip", "addr", "add", f"{ip}/{prefix}", "dev", "lo"],
            capture_output=True, text=True, timeout=10,
        )
    except Exception as e:
        return ("error", f"loopback alias add 예외 ({ip}): {type(e).__name__}: {e}")
    if r.returncode == 0:
        return ("added", f"loopback alias 추가됨: {ip}/{prefix}")
    err = (r.stderr or "").strip()
    # sudo: a password is required → sudo -n 미허용
    if "password is required" in err or "no tty present" in err:
        return ("no-sudo",
                f"sudo -n 권한 없음 — `sudo ip addr add {ip}/{prefix} dev lo` 1회 수동")
    return ("error",
            f"loopback alias add 실패 ({ip}): rc={r.returncode} {err[:120]}")


def required_aliases(instances: list) -> list:
    """instances 의 local_ip 중 127.0.0.1 이 아닌 unique IP list."""
    seen: set = set()
    out: list = []
    for inst in instances:
        ip = inst.get("local_ip", "127.0.0.1")
        if ip == "127.0.0.1" or ip in seen:
            continue
        seen.add(ip)
        out.append(ip)
    return out
