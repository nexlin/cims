"""S3 XCAP root 정본 회귀 — 단말이 듣는 두 주소가 같은가.

단말은 XCAP 문서(그룹/user-profile/service-config)를 받을 주소를 두 경로로 듣는다.
  ① CSC ue-init-config 의 `GMS/CMS-XCAP-root-URI` (로그인 전 부트스트랩)
  ② CSP xcap-diff NOTIFY 의 `xcap-root` (문서 변경 통지)
이 둘이 어긋나면 단말은 통지를 받고도 엉뚱한 주소로 문서를 조회한다. 정본은 CSC
(`McpttServer.PublicUrl`) 한 곳이고 CSP 는 내부 API 로 취득한다 — CSP 에는 이 주소를
적는 설정이 없다(구 `Setup.Xcap.*` 폐기).

검사:
  X1 내부 API 게이트 — 토큰 없음 401 / 잘못된 토큰 401
  X2 내부 API 응답   — 200 + xcap_root (후행 '/' 포함)
  X3 정본 일치       — ue-init-config 의 GMS/CMS-XCAP-root-URI == 내부 API xcap_root
  X4 CSP 실소비      — CSP 로그의 `[topology] MCPTT xcap-root =` 가 같은 값
  X5 설정 폐기 확인  — csp.json 에 Setup.Xcap 부재 (재도입 회귀 방지)
"""
from __future__ import annotations

import glob
import json
import os
import re

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import csc_http

_RID = "S3-SCN-XCAP-ROOT"
_RNAME = "XCAP root 정본 일치 (ue-init-config == NOTIFY xcap-root)"


def _json_file(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _csp_topology_root(dist_dir: str) -> str:
    """가장 최근 CSP 로그의 마지막 `[topology] MCPTT xcap-root = <url>` 값."""
    logs = sorted(glob.glob(os.path.join(dist_dir, "csp", "log", "csp_*.log")), key=os.path.getmtime)
    found = ""
    for path in logs[-3:]:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.search(r"\[topology\] MCPTT xcap-root = (\S+)", line)
                    if m:
                        found = m.group(1)
        except Exception:
            continue
    return found


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-HEALTH"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=[], timeout_s=60,
    execution_order=65,
)
def xcap_root_sot(ctx: VerifyContext) -> ItemResult:
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    def skip(reason: str) -> ItemResult:
        ctx.w(f"- [SKIP] {reason}")
        return done(ItemStatus.SKIP, reason)

    csc = _json_file(os.path.join(ctx.dist_dir, "csc", "config", "csc.json"))
    if not csc:
        return skip("csc.json 없음 (S3-CONFIGURE 선행)")
    token = str((csc.get("InternalApi") or {}).get("Token") or "")
    if not token:
        return skip("csc.json InternalApi.Token 미설정 (configure 재실행)")
    admin_port = int((csc.get("Server") or {}).get("Port") or 4421)
    mcptt_port = int((csc.get("McpttServer") or {}).get("Port") or 4430)
    public_url = str((csc.get("McpttServer") or {}).get("PublicUrl") or "")

    ip = ctx.sim_ip
    ep_url = f"https://{ip}:{admin_port}/internal/mcptt/endpoint"
    ue_url = (f"https://{ip}:{mcptt_port}/org.3gpp.mcptt.ue-init-config"
              f"/users/verify-probe/ue-init-config")
    lines = [f"- CSC: admin {ip}:{admin_port} · mcptt {ip}:{mcptt_port} · "
             f"PublicUrl={public_url or '(미설정 — 요청 Host 유도)'}"]
    checks: list = []

    def chk(label: str, ok: bool, got: str, expect: str) -> None:
        checks.append(ok)
        lines.append(f"- {label} → {got} ({'PASS' if ok else 'FAIL'} — 기대 {expect})")

    # X1 — 토큰 게이트
    try:
        st, _ = csc_http.request_status("GET", ep_url, token=None, timeout=5)
        chk("X1a 토큰 없음", st == 401, str(st), "401")
        st, _ = csc_http.request_status("GET", ep_url, token="wrong-token", timeout=5)
        chk("X1b 잘못된 토큰", st == 401, str(st), "401")
    except Exception as e:
        return skip(f"내부 API 도달 실패 ({e}) — CSC 기동 확인")

    # X2 — 정상 응답
    try:
        st, body = csc_http.request_status("GET", ep_url, token=token, timeout=5)
    except Exception as e:
        return skip(f"내부 API 호출 실패 ({e})")
    ep_root = ""
    if isinstance(body, dict):
        ep_root = str(body.get("xcap_root") or "")
    ok2 = st == 200 and ep_root.startswith("http") and ep_root.endswith("/")
    chk("X2 내부 API xcap_root", ok2, f"{st} {ep_root or '(없음)'}", "200 + https://…/")

    # X3 — ue-init-config 문서와 대조
    try:
        st, xml = csc_http.request_status("GET", ue_url, token=None, timeout=5)
    except Exception as e:
        st, xml = 0, f"error {e}"
    roots = {}
    if st == 200 and isinstance(xml, str):
        for tag in ("GMS-XCAP-root-URI", "CMS-XCAP-root-URI"):
            m = re.search(rf"<{tag}>([^<]*)</{tag}>", xml)
            roots[tag] = (m.group(1).strip() if m else "")
    norm = lambda u: (u.rstrip("/") + "/") if u else ""          # noqa: E731
    ok3 = bool(roots) and ep_root and all(norm(v) == ep_root for v in roots.values())
    chk("X3 ue-init-config == 내부 API",
        ok3,
        f"{st} GMS={roots.get('GMS-XCAP-root-URI', '?')} CMS={roots.get('CMS-XCAP-root-URI', '?')}",
        f"둘 다 {ep_root or '(내부 API 값)'}")

    # X4 — CSP 가 실제로 그 값을 소비했는가
    csp_root = _csp_topology_root(ctx.dist_dir)
    if not csp_root:
        lines.append("- X4 CSP 로그의 `[topology] MCPTT xcap-root` 없음 → SKIP "
                     "(CSC 미기동 상태로 CSP 가 기동했을 수 있음 — 재기동 후 재검)")
    else:
        chk("X4 CSP 실소비 값", norm(csp_root) == ep_root, csp_root, ep_root)

    # X5 — 폐기 설정 재도입 방지
    csp_cfg = _json_file(os.path.join(ctx.dist_dir, "csp", "config", "csp.json"))
    has_xcap = "Xcap" in (csp_cfg.get("Setup") or {})
    chk("X5 csp.json Setup.Xcap 부재", not has_xcap, "있음" if has_xcap else "없음", "없음")

    for ln in lines:
        ctx.w(ln)
    if all(checks):
        return done(ItemStatus.PASS, f"xcap-root={ep_root} (CSC 정본)")
    return done(ItemStatus.FAIL, f"{sum(1 for c in checks if not c)}/{len(checks)} 검사 실패")
