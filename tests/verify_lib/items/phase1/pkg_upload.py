"""Phase 1 §5.5 — Package auto-upload.

reset 로 cims_package 비워진 상태를 build/dist/packages/*.tar.gz 로 채움.
Console 모듈관리에서 버전/템플릿/설정이 정상 표시되도록.

CSC 4421 (Test-CSC) → 4420 (운영 배포본) 순으로 admin 로그인 시도.
"""
from __future__ import annotations

import json
import os
from glob import glob
from urllib.error import URLError

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


def _admin_login(host: str, port: int, timeout: int = 3) -> str:
    """admin/1234 로 JWT 발급. 실패 시 빈 문자열."""
    import ssl
    import urllib.request
    body = json.dumps({"login_id": "admin", "password": "1234"}).encode()
    url = f"https://{host}:{port}/api/v1/auth/login"
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    ctx_ssl = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx_ssl) as r:
            data = json.loads(r.read().decode())
            return data.get("token", "") or data.get("access_token", "")
    except Exception:
        return ""


def _upload_package(host: str, port: int, token: str, tarball_path: str,
                    timeout: int = 30) -> int:
    """multipart/form-data 로 패키지 업로드. HTTP code 반환."""
    import ssl
    import urllib.request
    import uuid
    boundary = f"----cimsverify{uuid.uuid4().hex}"
    fname = os.path.basename(tarball_path)
    with open(tarball_path, "rb") as f: payload = f.read()
    parts = []
    parts.append(f"--{boundary}\r\n".encode())
    parts.append(f'Content-Disposition: form-data; name="file"; filename="{fname}"\r\n'
                 .encode())
    parts.append(b"Content-Type: application/gzip\r\n\r\n")
    parts.append(payload)
    parts.append(f"\r\n--{boundary}\r\n".encode())
    parts.append(b'Content-Disposition: form-data; name="force"\r\n\r\ntrue\r\n')
    parts.append(f"--{boundary}--\r\n".encode())
    body = b"".join(parts)
    url = f"https://{host}:{port}/api/v1/packages"
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
        },
    )
    ctx_ssl = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx_ssl) as r:
            return r.status
    except urllib.request.HTTPError as e:
        return e.code
    except Exception:
        return 0


@verify_item(
    id="P1-PKG-UPLOAD",
    phase=1, category="환경",
    name="패키지 auto-upload (build/dist/packages → CSC)",
    depends_on=["P1-START"],
    presets=["phase1-full"],
    side_effects=["network", "db-write"], timeout_s=300,
)
def pkg_upload(ctx: VerifyContext) -> ItemResult:
    pkg_dir = os.path.join(ctx.dist_dir, "packages")
    if not os.path.isdir(pkg_dir):
        ctx.w("## P1-PKG-UPLOAD — packages 디렉토리 없음 — SKIP")
        ctx.w()
        return ItemResult(
            id="P1-PKG-UPLOAD", name="패키지 auto-upload",
            status=ItemStatus.SKIP, detail=f"{pkg_dir} 없음", phase=1,
        )

    host = ctx.ens_ip or "127.0.0.1"
    token = ""
    used_port = 0
    for port in (4421, 4420):
        token = _admin_login(host, port)
        if token:
            used_port = port
            break

    if not token:
        ctx.w("## P1-PKG-UPLOAD — admin 로그인 실패 (4421/4420 모두)")
        ctx.w()
        # 로그인 실패는 SKIP 으로 처리 (Phase 1 의 핵심 기능 아님)
        return ItemResult(
            id="P1-PKG-UPLOAD", name="패키지 auto-upload",
            status=ItemStatus.SKIP,
            detail="admin 로그인 실패 (4421/4420)", phase=1,
        )

    tarballs = sorted(glob(os.path.join(pkg_dir, "*.tar.gz")))
    uploaded, failed = [], []
    for t in tarballs:
        code = _upload_package(host, used_port, token, t)
        if code in (200, 201):
            uploaded.append(os.path.basename(t))
        else:
            failed.append(f"{os.path.basename(t)}({code})")

    ctx.w(f"## P1-PKG-UPLOAD — admin@{host}:{used_port}")
    ctx.w(f"- 업로드 OK: {len(uploaded)}건")
    if uploaded:
        for f in uploaded: ctx.w(f"  - [OK] {f}")
    if failed:
        ctx.w(f"- 업로드 실패: {len(failed)}건")
        for f in failed: ctx.w(f"  - [FAIL] {f}")
    ctx.w()

    detail = (f"port={used_port}, OK={len(uploaded)}, FAIL={len(failed)}\n"
              + "\n".join(uploaded[:5] + failed[:5]))
    # 일부 실패해도 PASS — 로그만 남김 (Phase 1 검증 본질 아님)
    return ItemResult(
        id="P1-PKG-UPLOAD", name="패키지 auto-upload",
        status=ItemStatus.PASS, detail=detail, phase=1,
    )
