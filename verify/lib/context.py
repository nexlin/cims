"""항목 실행 컨텍스트.

VerifyContext: 항목 함수가 받는 실행 인자.
- 작업 디렉토리, 옵션, 환경, 타임스탬프
- 리포트 작성 헬퍼
- ens160 IP / git sha / DIST_DIR 등 자주 쓰이는 값 캐시
"""
from __future__ import annotations

import os
import re
import socket
import subprocess
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import IO, Optional


def _detect_ens160_ip() -> str:
    """ip -4 -o addr show ens160 → IPv4 추출. 실패 시 빈 문자열."""
    try:
        out = subprocess.check_output(
            ["ip", "-4", "-o", "addr", "show", "ens160"],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode("utf-8", errors="replace")
        m = re.search(r"inet\s+(\d+\.\d+\.\d+\.\d+)/", out)
        return m.group(1) if m else ""
    except Exception:
        return ""


def _detect_git(repo_root: str) -> tuple:
    try:
        sha = subprocess.check_output(
            ["git", "-C", repo_root, "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode().strip()
    except Exception:
        sha = "?"
    try:
        branch = subprocess.check_output(
            ["git", "-C", repo_root, "rev-parse", "--abbrev-ref", "HEAD"],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode().strip()
    except Exception:
        branch = "?"
    return (branch, sha)


# subprocess 호출 시 차단할 TB 전용 env (csc-tb.json 누수 방지)
_BLOCKED_ENV = {"CIMS_CSC_CONFIG", "CIMS_AGENT_SYNC_PORT"}


def sanitized_env() -> dict:
    return {k: v for k, v in os.environ.items() if k not in _BLOCKED_ENV}


@dataclass
class VerifyContext:
    repo_root: str                       # /home/nex/work/cims
    dist_dir: str                        # build/dist 절대 경로
    report_path: str                     # verify_reports/<ts>_stageN.md (또는 multi)
    stage: int                           # 1~6 (S1~S6) — 0=multi-stage 실행
    ts: str                              # YYYYMMDD_HHMMSS
    opts: dict = field(default_factory=dict)
    ens_ip: str = ""
    git_branch: str = "?"
    git_sha: str = "?"
    state: dict = field(default_factory=dict)        # 항목 간 데이터 공유 (예: 가입자 정보)

    # 내부
    _report_fp: Optional[IO] = None

    @classmethod
    def create(cls, repo_root: str, stage: int, opts: Optional[dict] = None,
               report_dir: Optional[str] = None) -> "VerifyContext":
        repo_root = os.path.abspath(repo_root)
        dist_dir = os.path.join(repo_root, "build", "dist")
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        rdir = report_dir or os.path.join(repo_root, "verify_reports")
        os.makedirs(rdir, exist_ok=True)
        # stage=0 (multi-stage) 의 경우 stageMulti 사용
        suffix = f"stage{stage}" if stage else "multi"
        report_path = os.path.join(rdir, f"{ts}_{suffix}.md")
        ens_ip = _detect_ens160_ip()
        branch, sha = _detect_git(repo_root)
        return cls(
            repo_root=repo_root, dist_dir=dist_dir, report_path=report_path,
            stage=stage, ts=ts, opts=opts or {},
            ens_ip=ens_ip, git_branch=branch, git_sha=sha,
        )

    # ── 리포트 작성 헬퍼 ────────────────────────────────────────
    def report_open(self) -> None:
        if self._report_fp is None:
            self._report_fp = open(self.report_path, "w", encoding="utf-8")

    def report_close(self) -> None:
        if self._report_fp is not None:
            self._report_fp.close()
            self._report_fp = None

    def w(self, line: str = "") -> None:
        """리포트 한 줄 추가."""
        if self._report_fp is None:
            self.report_open()
        self._report_fp.write(line + "\n")
        self._report_fp.flush()

    # ── 옵션 단축 ───────────────────────────────────────────────
    @property
    def skip_build(self) -> bool: return bool(self.opts.get("skip_build", False))
    @property
    def skip_pkg(self) -> bool: return bool(self.opts.get("skip_pkg", False))
    @property
    def skip_reset(self) -> bool: return bool(self.opts.get("skip_reset", False))
    @property
    def keep_agent(self) -> bool: return bool(self.opts.get("keep_agent", False))
    @property
    def stop_after(self) -> bool: return bool(self.opts.get("stop_after", False))
    @property
    def sim_ip(self) -> str: return self.ens_ip or "127.0.0.1"

    def only_children_for(self, item_id: str) -> Optional[set]:
        """주어진 부모 항목(MODULE-CSC 등) 하위 자식 ID 필터.

        opts["only_children"] 형식: {"MODULE-CSC": ["CSC-AUTH-01", "CSC-USER-01"]}
        값이 없거나 빈 리스트면 None (전체 실행).
        """
        m = self.opts.get("only_children") or {}
        ids = m.get(item_id) if isinstance(m, dict) else None
        return set(ids) if ids else None
