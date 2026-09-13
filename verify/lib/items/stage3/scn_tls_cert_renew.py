"""S3-SCN-TLS-CERT-RENEW — 단말 대면 leaf 자동 갱신 (sip_tls_signaling.md §8.6.1, §9 #5).

lifecycle 엔진(`agent/lib/cert.sh`, `cims-svc cert`)과 CSP 의 같은 경로 내용 교체 재적재를 dev 스택에서
끝까지 돈다. 시험용 루트를 즉석에서 만들어 dev 그룹 CA(`build/runtime/_secrets/ca/ca.crt`)를 교차 서명한다
(`service-cert.sh site-ca sign --cross`) — 실제 루트 키는 쓰지 않는다. 자기복원: 종료 시 교차 인증서·CSP 체인·
local_nodes.jsonl·갱신 상태 파일을 시작 전 상태로 되돌리고 SIGUSR1 로 CSP 를 원복한다.

검사:
  R1 체인 전환(갈래 ⑤) — 교차 인증서 배치 뒤 `cims-svc cert csp` → csp-chain.pem 2장(2번째 = 교차 인증서),
     leaf 가 시험 루트 앵커로 검증(openssl verify -CAfile 루트 -untrusted 교차), 상태 파일 ok=true
  R2 CSP 서빙 + 내용 교체 재적재 — 임시 TLS 행(csp-chain.pem/csp.key)을 SIGUSR1 로 올리고 서빙 체인 2장 확인.
     같은 경로에 잔여 10일 leaf 를 써넣고 SIGUSR1 → 서빙 leaf 지문이 바뀐다(경로 불변 — 지문 재적재)
  R3 자동 갱신(갈래 ④) — `cims-svc cert csp` → 잔여 ≥ 700일로 재발급 + 엔진의 SIGUSR1 로 서빙 지문 재변경,
     상태 파일 ok=true reason=renewed
  R4 강등 금지(E5) — 잔여 10일 leaf + cert 디렉터리 쓰기 불가 → 재발급 실패 시 기존 유지, 상태 파일 ok=false
     reason=renew_failed (A-PRC-009 cert/csp/renew 입력)
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import ssl
import stat
import subprocess
import tempfile
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.access_services import signal_csp_reload

_RID = "S3-SCN-TLS-CERT-RENEW"
_RNAME = "단말 대면 leaf 자동 갱신 (사이트 CA 체인 전환·CSP 지문 재적재·잔여 60일 갱신·강등 금지)"
_TLS_PORT = 5062
_TLS_NODE_ID = "verify-cert-renew"


def _sh(args: list, env: dict | None = None, timeout: int = 120, inp: bytes | None = None) -> tuple:
    try:
        r = subprocess.run(args, capture_output=True, timeout=timeout, env=env, input=inp)
        return r.returncode, (r.stdout + r.stderr).decode(errors="replace")
    except Exception as e:
        return 99, str(e)


def _paths(ctx: VerifyContext) -> dict:
    runtime = os.path.join(os.path.dirname(ctx.dist_dir), "runtime")       # build/runtime (cert.sh _node_cert_dir 규칙)
    return {
        "ca_dir": os.path.join(runtime, "_secrets", "ca"),
        "cross": os.path.join(runtime, "_secrets", "ca", "ca-cross.crt"),
        "cert_dir": os.path.join(runtime, "cert"),
        "chain": os.path.join(runtime, "cert", "csp-chain.pem"),
        "key": os.path.join(runtime, "cert", "csp.key"),
        "state": os.path.join(ctx.dist_dir, "run", "cert", "csp.json"),
        "nodes": os.path.join(ctx.dist_dir, "config", "local_nodes.jsonl"),
        "pid": os.path.join(ctx.dist_dir, "run", "csp.pid"),
        "cims_svc": os.path.join(ctx.repo_root, "agent", "bin", "cims-svc"),
        "svc_cert": os.path.join(ctx.repo_root, "scripts", "service-cert.sh"),
    }


def _cims_svc_cert(ctx: VerifyContext, p: dict) -> tuple:
    env = dict(os.environ)
    env["CIMS_DIST_DIR"] = ctx.dist_dir
    return _sh([p["cims_svc"], "cert", "csp"], env=env, timeout=120)


def _pem_count(path: str) -> int:
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().count("-----BEGIN CERTIFICATE-----")
    except Exception:
        return 0


def _pem_nth(path: str, n: int) -> str:
    try:
        with open(path, encoding="utf-8") as f:
            txt = f.read()
    except Exception:
        return ""
    parts = txt.split("-----BEGIN CERTIFICATE-----")
    if len(parts) <= n:
        return ""
    body = parts[n].split("-----END CERTIFICATE-----")[0]
    return "-----BEGIN CERTIFICATE-----" + body + "-----END CERTIFICATE-----\n"


def _fp(pem: str) -> str:
    rc, out = _sh(["openssl", "x509", "-noout", "-fingerprint", "-sha256"], inp=pem.encode(), timeout=10)
    return out.strip() if rc == 0 else ""


def _days_left(pem: str) -> int:
    rc, out = _sh(["openssl", "x509", "-noout", "-enddate"], inp=pem.encode(), timeout=10)
    if rc != 0 or "=" not in out:
        return -9999
    rc2, epoch = _sh(["date", "-d", out.strip().split("=", 1)[1], "+%s"], timeout=10)
    try:
        return (int(epoch.strip()) - int(time.time())) // 86400
    except Exception:
        return -9999


def _served(ip: str, port: int, timeout: float = 3.0) -> list:
    """서빙 체인(PEM 목록). openssl s_client — 접속점이 보낸 전 장을 그대로 본다."""
    rc, out = _sh(["openssl", "s_client", "-connect", f"{ip}:{port}", "-showcerts"], inp=b"", timeout=int(timeout) + 5)
    if "-----BEGIN CERTIFICATE-----" not in out:
        return []
    pems = []
    for part in out.split("-----BEGIN CERTIFICATE-----")[1:]:
        pems.append("-----BEGIN CERTIFICATE-----" + part.split("-----END CERTIFICATE-----")[0] + "-----END CERTIFICATE-----\n")
    return pems


def _reachable(ip: str, port: int) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=1.0):
            return True
    except OSError:
        return False


def _write_short_leaf(p: dict, tmp: str, days: int, host: str) -> bool:
    """그룹 CA 로 잔여 `days` 일 leaf 를 발급해 csp-chain.pem(+교차 인증서)/csp.key 에 원자 교체 — 만료 임박 모사."""
    san = f"subjectAltName=DNS:{host},IP:127.0.0.1\nbasicConstraints=CA:FALSE\n"
    ext = os.path.join(tmp, "short.ext")
    with open(ext, "w") as f:
        f.write(san)
    key, csr, crt = (os.path.join(tmp, "short." + x) for x in ("key", "csr", "crt"))
    if _sh(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256", "-subj", f"/CN={host}/O=CIMS",
            "-keyout", key, "-out", csr], timeout=60)[0] != 0:
        return False
    if _sh(["openssl", "x509", "-req", "-in", csr, "-CA", os.path.join(p["ca_dir"], "ca.crt"),
            "-CAkey", os.path.join(p["ca_dir"], "ca.key"), "-CAcreateserial", "-days", str(days), "-sha256",
            "-extfile", ext, "-out", crt], timeout=60)[0] != 0:
        return False
    with open(crt) as f:
        chain = f.read()
    if os.path.isfile(p["cross"]):
        with open(p["cross"]) as f:
            chain += f.read()
    nk, nc = p["key"] + ".new", p["chain"] + ".new"
    shutil.copy(key, nk); os.chmod(nk, 0o600)
    with open(nc, "w") as f:
        f.write(chain)
    os.replace(nk, p["key"]); os.replace(nc, p["chain"])
    return True


class _Snapshot:
    """시작 전 상태 — 종료 시 그대로 되돌린다."""
    def __init__(self, p: dict):
        self.p = p
        self.files = {}
        for k in ("cross", "chain", "key", "state", "nodes"):
            path = p[k]
            self.files[k] = open(path, "rb").read() if os.path.isfile(path) else None
        self.cert_dir_mode = stat.S_IMODE(os.stat(p["cert_dir"]).st_mode) if os.path.isdir(p["cert_dir"]) else None

    def restore(self) -> None:
        p = self.p
        if self.cert_dir_mode is not None and os.path.isdir(p["cert_dir"]):
            try:
                os.chmod(p["cert_dir"], self.cert_dir_mode)
            except Exception:
                pass
        for k, data in self.files.items():
            path = p[k]
            try:
                if data is None:
                    if os.path.exists(path):
                        os.remove(path)
                else:
                    with open(path, "wb") as f:
                        f.write(data)
                    if k == "key":
                        os.chmod(path, 0o600)
            except Exception:
                pass
        signal_csp_reload(p["pid"], wait_sec=1.5)


@verify_item(
    id=_RID, stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-START"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["config-write", "csp-reload", "self-restoring"], timeout_s=240,
    execution_order=76,
)
def scn_tls_cert_renew(ctx: VerifyContext) -> ItemResult:
    p = _paths(ctx)
    ctx.w(f"### {_RID} — {_RNAME}")
    if shutil.which("openssl") is None or not os.path.isfile(p["cims_svc"]) or not os.path.isfile(p["svc_cert"]):
        ctx.w("- [SKIP] openssl / agent/bin/cims-svc / scripts/service-cert.sh 필요")
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=ItemStatus.SKIP, detail="도구 부재", stage=3)
    if not os.path.isfile(p["pid"]):
        ctx.w("- [SKIP] CSP pid 파일 없음 (dev 스택 미기동)")
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=ItemStatus.SKIP, detail="CSP 미기동", stage=3)

    host = socket.getfqdn() or socket.gethostname()
    results: list = []
    def rec(ok: bool, label: str, detail: str = "") -> None:
        results.append((ok, label, detail))
        ctx.w(f"- [{'PASS' if ok else 'FAIL'}] {label}{' — ' + detail if detail else ''}")

    os.makedirs(p["cert_dir"], exist_ok=True)
    snap = _Snapshot(p)
    tmp = tempfile.mkdtemp(prefix="cert-renew-")
    try:
        # 준비 — 그룹 CA·기본 체인 보증(교차 인증서 없이 1회), 시험 루트 생성, 그룹 CA 교차 서명
        if os.path.isfile(p["cross"]):
            os.remove(p["cross"])
        _cims_svc_cert(ctx, p)
        root_crt = os.path.join(tmp, "testroot.crt"); root_key = os.path.join(tmp, "testroot.key")
        _sh(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "3650",
             "-subj", "/C=KR/O=CIMS/CN=VERIFY Test Root", "-addext", "basicConstraints=critical,CA:TRUE",
             "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-keyout", root_key, "-out", root_crt], timeout=60)
        rc, out = _sh(["bash", p["svc_cert"], "site-ca", "sign", "--cross", os.path.join(p["ca_dir"], "ca.crt"),
                       "--root-dir", tmp, "--root-name", "testroot", "--out", p["cross"]], timeout=60)
        rec(rc == 0 and os.path.isfile(p["cross"]), "준비: 시험 루트가 dev 그룹 CA 를 교차 서명", out.strip().splitlines()[-1][:160] if out.strip() else "")

        # R1 체인 전환
        rc, out = _cims_svc_cert(ctx, p)
        n = _pem_count(p["chain"])
        cross_fp = _fp(open(p["cross"]).read()) if os.path.isfile(p["cross"]) else ""
        second_fp = _fp(_pem_nth(p["chain"], 2)) if n >= 2 else ""
        leaf1 = _pem_nth(p["chain"], 1)
        with open(os.path.join(tmp, "leaf1.pem"), "w") as f:
            f.write(leaf1)
        vrc, vout = _sh(["openssl", "verify", "-CAfile", root_crt, "-untrusted", p["cross"], os.path.join(tmp, "leaf1.pem")], timeout=30)
        st = {}
        try:
            st = json.load(open(p["state"]))
        except Exception:
            pass
        rec(rc == 0 and n == 2 and cross_fp and second_fp == cross_fp, "R1 체인 전환(갈래 ⑤) — csp-chain.pem 2장, 2번째 = 교차 인증서", f"장수={n}")
        rec(vrc == 0, "R1 leaf → 교차 인증서 → 시험 루트 경로 검증", vout.strip()[:120])
        rec(bool(st.get("ok")) and st.get("reason") in ("renewed", "issued"), "R1 갱신 상태 파일 ok=true", json.dumps(st, ensure_ascii=False)[:160])

        # R2 CSP 서빙 + 같은 경로 내용 교체 재적재
        with open(p["nodes"], "a", encoding="utf-8") as f:
            f.write(json.dumps({"id": _TLS_NODE_ID, "name": _TLS_NODE_ID, "edge": "access", "bind_ip": ctx.sim_ip,
                                "bind_port": _TLS_PORT, "protocol": "TLS", "enabled": True, "is_primary": False,
                                "tls_cert_path": p["chain"], "tls_key_path": p["key"], "tls_verify_peer": False},
                               ensure_ascii=False) + "\n")
        signal_csp_reload(p["pid"], wait_sec=1.5)
        for _ in range(10):
            if _reachable(ctx.sim_ip, _TLS_PORT):
                break
            time.sleep(0.5)
        served = _served(ctx.sim_ip, _TLS_PORT)
        fp_a = _fp(served[0]) if served else ""
        rec(len(served) == 2 and fp_a == _fp(leaf1), "R2 CSP 임시 TLS 접속점이 체인 2장을 서빙", f"served={len(served)}장")
        okw = _write_short_leaf(p, tmp, 10, host)
        signal_csp_reload(p["pid"], wait_sec=2.0)
        served_b = _served(ctx.sim_ip, _TLS_PORT)
        fp_b = _fp(served_b[0]) if served_b else ""
        rec(okw and fp_b and fp_b != fp_a and _days_left(served_b[0]) <= 60,
            "R2 같은 경로 내용 교체 → SIGUSR1 → 서빙 leaf 지문 변경(지문 재적재, 경로 불변)",
            f"잔여 {_days_left(served_b[0]) if served_b else '?'}일")

        # R3 자동 갱신(갈래 ④) — 엔진이 재발급 + SIGUSR1
        rc, out = _cims_svc_cert(ctx, p)
        time.sleep(2.0)
        served_c = _served(ctx.sim_ip, _TLS_PORT)
        fp_c = _fp(served_c[0]) if served_c else ""
        days_c = _days_left(served_c[0]) if served_c else -9999
        try:
            st = json.load(open(p["state"]))
        except Exception:
            st = {}
        rec(rc == 0 and days_c >= 700 and _pem_count(p["chain"]) == 2, "R3 잔여 ≤60일 → 엔진 재발급(2년 leaf, 체인 2장)", f"잔여 {days_c}일 rc={rc}")
        rec(fp_c and fp_c != fp_b and len(served_c) == 2, "R3 엔진 SIGUSR1 로 서빙 leaf 재변경(무중단)", f"served={len(served_c)}장")
        rec(bool(st.get("ok")) and st.get("reason") == "renewed", "R3 갱신 상태 파일 ok=true reason=renewed", json.dumps(st, ensure_ascii=False)[:160])

        # R4 강등 금지 — 재발급 실패 시 기존 유지 + 실패 기록
        okw = _write_short_leaf(p, tmp, 10, host)
        before = open(p["chain"], "rb").read()
        os.chmod(p["cert_dir"], 0o500)
        rc, out = _cims_svc_cert(ctx, p)
        os.chmod(p["cert_dir"], snap.cert_dir_mode or 0o700)
        after = open(p["chain"], "rb").read()
        try:
            st = json.load(open(p["state"]))
        except Exception:
            st = {}
        rec(okw and before == after and st.get("ok") is False and st.get("reason") == "renew_failed",
            "R4 재발급 실패(디렉터리 쓰기 불가) → 기존 인증서 유지 + 상태 ok=false reason=renew_failed",
            json.dumps(st, ensure_ascii=False)[:160])
    finally:
        # 원복 — 임시 TLS 행 제거는 local_nodes 스냅샷 복원이 맡는다
        snap.restore()
        shutil.rmtree(tmp, ignore_errors=True)

    fails = [r for r in results if not r[0]]
    ctx.w(f"- 결과: {len(results) - len(fails)}/{len(results)} PASS")
    ctx.w()
    detail = "\n".join(f"[{'PASS' if ok else 'FAIL'}] {lab} {det}".strip() for ok, lab, det in results)
    return ItemResult(id=_RID, name=_RNAME, status=ItemStatus.PASS if not fails else ItemStatus.FAIL,
                      detail=detail, stage=3)
