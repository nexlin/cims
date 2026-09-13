"""S3-HEALTH — Health check (csp/cmp/csc 로그 ERROR/FATAL 누적 + 단말 대면 인증서 잔여).

로그 포맷별 정밀 매칭:
- csp/cmp (C++ psip 포맷): `[YYYY-MM-DD ...] [E|F] [Module] ...` — level letter 정확 매칭
- csc (Python uvicorn): 행 시작 `ERROR:` / `CRITICAL:` — Python logging 포맷

전체 파일이 아닌 tail (last N lines) 만 검사 — 장기 가동 시 메모리 폭증 회피.

단말 대면 인증서(CSC HTTPS 4430 · CSP TLS 접속점 = local_nodes.jsonl 의 enabled TLS 행)는 서빙 중인
체인의 **가장 이른 만료**(leaf·사이트 CA)까지 잔여가 갱신 임계(60일)를 넘어야 PASS — 임계는
sip_tls_signaling.md §8.6.2 단일 정의(cert.sh CERT_RENEW_DAYS·service-cert.sh RENEW_DAYS 와 같은 값).
그 안이면 lifecycle 엔진의 자동 갱신이 돌지 않았다는 뜻이라 FAIL. 접속되지 않는 접속점은 건너뛴다
(dev 스택에 TLS 행이 없을 수 있다 — 개설 실패 축은 별개 항목).
"""
from __future__ import annotations

import json
import os
import re
import socket
import subprocess
from collections import deque
from datetime import datetime, timezone

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


_NATIVE_RE = re.compile(rb"^\[[\d\-: .]+\]\s+\[(E|F)\]")
_PY_RE = re.compile(rb"^(ERROR|CRITICAL):")
_TAIL_LINES = 2000


def _tail_bytes(path: str, n_lines: int) -> list:
    try:
        with open(path, "rb") as f:
            return list(deque(f, maxlen=n_lines))
    except Exception:
        return []


def _scan_native(path: str) -> tuple:
    """C++ psip 로그 — `[E]` / `[F]` 행만 카운트."""
    err = 0
    samples: list = []
    for line in _tail_bytes(path, _TAIL_LINES):
        if _NATIVE_RE.match(line):
            err += 1
            if len(samples) < 10:
                samples.append(
                    f"{os.path.basename(path)}: {line.decode(errors='replace').rstrip()}"
                )
    return err, samples


def _scan_python(path: str) -> tuple:
    """Python uvicorn 로그 — 행 시작 `ERROR:` / `CRITICAL:` 만 카운트."""
    err = 0
    samples: list = []
    for line in _tail_bytes(path, _TAIL_LINES):
        if _PY_RE.match(line):
            err += 1
            if len(samples) < 10:
                samples.append(
                    f"{os.path.basename(path)}: {line.decode(errors='replace').rstrip()}"
                )
    return err, samples


# ── 단말 대면 인증서 잔여 ──
CERT_RENEW_DAYS = 60          # sip_tls_signaling.md §8.6.2 — 갱신 임계 (경고 30 / 위험 7 은 알람 축)
_CSC_HTTPS_PORT = 4430


def _served_chain(ip: str, port: int, timeout: float = 3.0) -> list:
    """접속점이 핸드셰이크에서 보낸 인증서 체인(PEM 목록) — openssl s_client -showcerts (service-cert.sh verify 와
    같은 관측 경로라 판정이 어긋나지 않는다). 접속 실패/TLS 아님이면 []."""
    try:
        with socket.create_connection((ip, port), timeout=1.0):
            pass
    except OSError:
        return []
    try:
        out = subprocess.run(["openssl", "s_client", "-connect", f"{ip}:{port}", "-showcerts"], input=b"",
                             capture_output=True, timeout=timeout + 5).stdout.decode(errors="replace")
    except Exception:
        return []
    pems = []
    for part in out.split("-----BEGIN CERTIFICATE-----")[1:]:
        pems.append("-----BEGIN CERTIFICATE-----" + part.split("-----END CERTIFICATE-----")[0] + "-----END CERTIFICATE-----\n")
    return pems


def _pem_days_left(pem: str) -> "int | None":
    """PEM 인증서의 notAfter 까지 남은 일수 (openssl — verify 도구·엔진과 같은 판정 경로)."""
    try:
        out = subprocess.run(["openssl", "x509", "-noout", "-enddate"], input=pem.encode(),
                             capture_output=True, timeout=5).stdout.decode(errors="replace").strip()
    except Exception:
        return None
    if not out.startswith("notAfter="):
        return None
    txt = out.split("=", 1)[1].strip()
    for fmt in ("%b %d %H:%M:%S %Y %Z", "%b  %d %H:%M:%S %Y %Z"):
        try:
            dt = datetime.strptime(txt, fmt).replace(tzinfo=timezone.utc)
            return (dt - datetime.now(timezone.utc)).days
        except ValueError:
            continue
    return None


def _tls_endpoints(dist_dir: str) -> list:
    """(label, ip, port) — CSC HTTPS + local_nodes.jsonl 의 enabled TLS 행."""
    eps = [("CSC HTTPS", "127.0.0.1", _CSC_HTTPS_PORT)]
    path = os.path.join(dist_dir, "config", "local_nodes.jsonl")
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                if str(r.get("protocol", "")).upper() != "TLS" or not r.get("enabled", True):
                    continue
                ip = str(r.get("bind_ip") or "127.0.0.1")
                if ip in ("0.0.0.0", ""):
                    ip = "127.0.0.1"
                try:
                    port = int(r.get("bind_port") or 0)
                except Exception:
                    continue
                if port > 0:
                    eps.append((f"CSP TLS {r.get('name') or r.get('id') or ''}".strip(), ip, port))
    except Exception:
        pass
    return eps


def check_cert_residual(dist_dir: str) -> tuple:
    """(fail_count, lines) — 접속점별 잔여 판정. 접속 불가는 SKIP 행(실패 아님)."""
    fails = 0
    lines = []
    for label, ip, port in _tls_endpoints(dist_dir):
        chain = _served_chain(ip, port)
        if not chain:
            lines.append(f"- [SKIP] {label} {ip}:{port} — 접속 불가/TLS 아님")
            continue
        days = [d for d in (_pem_days_left(c) for c in chain) if d is not None]
        if not days:
            fails += 1
            lines.append(f"- [FAIL] {label} {ip}:{port} — 인증서 만료 해석 불가")
            continue
        earliest = min(days)
        which = "leaf" if days.index(earliest) == 0 else "사이트 CA"
        if earliest > CERT_RENEW_DAYS:
            lines.append(f"- [PASS] {label} {ip}:{port} — 잔여 {earliest}일 ({which}, 체인 {len(chain)}장)")
        else:
            fails += 1
            lines.append(f"- [FAIL] {label} {ip}:{port} — 잔여 {earliest}일 ({which}) ≤ 갱신 임계 {CERT_RENEW_DAYS}일 "
                         f"— lifecycle 엔진 자동 갱신 미동작(A-PRC-009 cert/<module>/renew 확인)")
    return fails, lines


@verify_item(
    id="S3-HEALTH",
    stage=3, category="검증",
    name="Health check (csp/cmp ERROR/FATAL + csc ERROR/CRITICAL, last 2000 lines · 단말 대면 인증서 잔여 > 60일)",
    depends_on=["S3-START"],
    presets=["stage3-full", "stage3-quick", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=10,
    execution_order=70,
)
def health(ctx: VerifyContext) -> ItemResult:
    log_dir = os.path.join(ctx.dist_dir, "log")
    err_total = 0
    samples: list = []
    for fname in ("csp.log", "cmp.log"):
        path = os.path.join(log_dir, fname)
        if not os.path.isfile(path):
            continue
        n, s = _scan_native(path)
        err_total += n
        samples.extend(s)
    for fname in ("csc.log",):
        path = os.path.join(log_dir, fname)
        if not os.path.isfile(path):
            continue
        n, s = _scan_python(path)
        err_total += n
        samples.extend(s)
    samples = samples[:10]
    cert_fails, cert_lines = check_cert_residual(ctx.dist_dir)

    ctx.w("## S3-HEALTH — Health check (last 2000 lines per file)")
    ctx.w(f"- ERROR/FATAL/CRITICAL 누적: {err_total}건")
    if samples:
        ctx.w("```")
        for s in samples:
            ctx.w(s[:400])
        ctx.w("```")
    ctx.w(f"### 단말 대면 인증서 잔여 (갱신 임계 {CERT_RENEW_DAYS}일)")
    for ln in cert_lines:
        ctx.w(ln)
    ctx.w()

    detail = (f"ERROR/FATAL/CRITICAL: {err_total}\n" + "\n".join(samples)
              + f"\n인증서 잔여 FAIL: {cert_fails}\n" + "\n".join(cert_lines))
    return ItemResult(
        id="S3-HEALTH", name="Health check",
        status=ItemStatus.PASS if (err_total == 0 and cert_fails == 0) else ItemStatus.FAIL,
        detail=detail, stage=3,
    )
