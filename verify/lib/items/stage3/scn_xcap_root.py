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
  X4 CSP 실소비      — CSP 로그의 `[topology] MCPTT xcap-root =` + 실제 송출된 xcap-diff 본문
  X5 설정 폐기 확인  — csp.json 에 Setup.Xcap 부재 (재도입 회귀 방지)
  X6 정본 추종       — PublicUrl 을 프로브값으로 바꾸면 내부 API·ue-init-config·CSP 가 모두
                       그 값을 따르는가 (원복 포함). 단일 노드에서는 설정 유도값과 정본이
                       **우연히 같아** X2~X4 만으로는 정본 경로가 실제로 동작하는지 구분되지
                       않는다 — 이 검사가 그 우연을 깬다.
"""
from __future__ import annotations

import glob
import json
import os
import re
import time

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


def _csp_emitted_root(dist_dir: str) -> str:
    """CSP 로그의 xcap-diff 본문에서 실제 단말에 나간 xcap-root (마지막 값)."""
    logs = sorted(glob.glob(os.path.join(dist_dir, "csp", "log", "csp_*.log")), key=os.path.getmtime)
    found = ""
    for path in logs[-3:]:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.search(r'<xcap-diff[^>]*xcap-root="([^"]+)"', line)
                    if m:
                        found = m.group(1)
        except Exception:
            continue
    return found


def _csc_cfg_path(dist_dir: str) -> str:
    """실효 csc 설정 파일 — overlay(config.json)가 있으면 그것이 primary, 없으면 base."""
    overlay = os.path.join(dist_dir, "csc", "config.json")
    if os.path.isfile(overlay):
        return overlay
    base = os.path.join(dist_dir, "csc", "config", "csc.json")
    return base if os.path.isfile(base) else ""


def _read_public_url(path: str):
    """(현재 PublicUrl, 평면키 여부). 키가 없으면 (None, 평면키 여부)."""
    d = _json_file(path)
    if "McpttServer.PublicUrl" in d:                      # overlay = flat dot-path
        return d["McpttServer.PublicUrl"], True
    flat = any(k.startswith("McpttServer.") for k in d)
    ms = d.get("McpttServer")
    if isinstance(ms, dict) and "PublicUrl" in ms:
        return ms["PublicUrl"], False
    return None, flat


def _write_public_url(path: str, value, flat: bool) -> bool:
    """PublicUrl 설정/복원. value=None 이면 키 삭제(원래 없던 상태로)."""
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        if flat:
            if value is None:
                d.pop("McpttServer.PublicUrl", None)
            else:
                d["McpttServer.PublicUrl"] = value
        else:
            ms = d.setdefault("McpttServer", {})
            if value is None:
                ms.pop("PublicUrl", None)
            else:
                ms["PublicUrl"] = value
        with open(path, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2, ensure_ascii=False)
        return True
    except Exception:
        return False


def _pid_of(pid_file: str, want: str) -> int:
    """pid 파일의 PID — 살아있고 cmdline 에 `want` 가 있어야 유효(stale/재사용 방지). 아니면 0."""
    try:
        with open(pid_file) as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            cmd = f.read().decode("utf-8", "replace")
        return pid if want in cmd else 0
    except Exception:
        return 0


def _sigusr1(pid_file: str, want: str, wait_sec: float = 1.5) -> bool:
    pid = _pid_of(pid_file, want)
    if not pid:
        return False
    try:
        os.kill(pid, 10)
        time.sleep(wait_sec)
        return True
    except Exception:
        return False


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-HEALTH"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["config-write"], timeout_s=120,
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

    # X4 — CSP 가 실제로 그 값을 소비했는가 (기동 로그 + 실제 송출된 NOTIFY 본문)
    csp_root = _csp_topology_root(ctx.dist_dir)
    if not csp_root:
        lines.append("- X4a CSP 로그의 `[topology] MCPTT xcap-root` 없음 → 생략 "
                     "(재기동 이후 로그가 로테이션됐거나 CSC 미기동 상태로 기동 — X6 가 대체 증명)")
    else:
        chk("X4a CSP 취득 값", norm(csp_root) == ep_root, csp_root, ep_root)
    emitted = _csp_emitted_root(ctx.dist_dir)
    if not emitted:
        lines.append("- X4b 송출된 xcap-diff 본문 없음 → 생략 (구독/NOTIFY 이력 없음)")
    else:
        chk("X4b 단말에 송출된 xcap-root", norm(emitted) == ep_root, emitted, ep_root)

    # X5 — 폐기 설정 재도입 방지
    csp_cfg = _json_file(os.path.join(ctx.dist_dir, "csp", "config", "csp.json"))
    has_xcap = "Xcap" in (csp_cfg.get("Setup") or {})
    chk("X5 csp.json Setup.Xcap 부재", not has_xcap, "있음" if has_xcap else "없음", "없음")

    # X6 — 정본 추종. 단일 노드에서는 설정 유도값(Csc.Host:4430)과 CSC 정본이 우연히 같아
    #   X2~X4 가 전부 PASS 여도 "정본 경로가 실제로 동작하는지" 는 증명되지 않는다.
    #   PublicUrl 을 프로브값으로 바꿔 세 소비처가 모두 따라오는지 보고 원복한다.
    probe = "https://xcap-sot-probe.example:9443"
    cfg_path = _csc_cfg_path(ctx.dist_dir)
    csc_pid = os.path.join(ctx.dist_dir, "run", "csc.pid")
    csp_pid = os.path.join(ctx.dist_dir, "run", "csp.pid")
    if not cfg_path or not _pid_of(csc_pid, "csc_app.py"):
        lines.append(f"- X6 생략 — csc 설정({cfg_path or '없음'}) 또는 유효 pid"
                     f"({csc_pid}) 부재 (dev CSC 미기동/stale pid)")
    else:
        orig, flat = _read_public_url(cfg_path)
        lines.append(f"- X6 대상: {os.path.relpath(cfg_path, ctx.dist_dir)} "
                     f"(원 PublicUrl={orig if orig is not None else '(키 없음)'}) → 프로브 {probe}")
        applied = False
        try:
            applied = _write_public_url(cfg_path, probe, flat) and _sigusr1(csc_pid, "csc_app.py")
            if not applied:
                lines.append("  - [생략] 프로브 적용 실패 (설정 쓰기 또는 SIGUSR1 실패)")
            else:
                st6, b6 = csc_http.request_status("GET", ep_url, token=token, timeout=5)
                got6 = str((b6 or {}).get("xcap_root") or "") if isinstance(b6, dict) else ""
                cfgd6 = bool((b6 or {}).get("public_url_configured")) if isinstance(b6, dict) else False
                # public_url_configured=False 면 CSC 가 PublicUrl 을 읽지 못한 것 — SIGUSR1 리로드
                #   미전달(환경)과 설정 파싱 회귀(코드)가 모두 여기로 온다. 유효 pid 를 이미
                #   확인했으므로 이 경우는 코드 회귀로 보고 FAIL 로 둔다.
                chk("X6a 내부 API 가 PublicUrl 추종", got6 == probe + "/",
                    f"{got6 or st6}{'' if cfgd6 else ' (public_url_configured=false — 리로드/파싱 확인)'}",
                    probe + "/")
                st7, xml7 = csc_http.request_status("GET", ue_url, token=None, timeout=5)
                m7 = re.search(r"<CMS-XCAP-root-URI>([^<]*)</CMS-XCAP-root-URI>", xml7) \
                    if isinstance(xml7, str) else None
                got7 = m7.group(1).strip() if m7 else ""
                chk("X6b ue-init-config 가 PublicUrl 추종", norm(got7) == probe + "/",
                    got7 or str(st7), probe)
                if _sigusr1(csp_pid, "csp", wait_sec=2.5):
                    got8 = _csp_topology_root(ctx.dist_dir)
                    chk("X6c CSP 가 재조회로 추종", norm(got8) == probe + "/", got8 or "(로그 없음)",
                        probe + "/")
                else:
                    lines.append("  - X6c 생략 — csp pid 부재/SIGUSR1 실패")
        finally:
            if applied or _read_public_url(cfg_path)[0] == probe:
                restored = _write_public_url(cfg_path, orig, flat) and _sigusr1(csc_pid, "csc_app.py")
                _sigusr1(csp_pid, "csp", wait_sec=2.5)
                lines.append(f"  - 원복: {'OK' if restored else 'FAIL'}")
        # X6d — 원복이 실제로 됐는지 (검사 실패 시에도 dev 스택이 오염된 채 남지 않게)
        st9, b9 = csc_http.request_status("GET", ep_url, token=token, timeout=5)
        got9 = str((b9 or {}).get("xcap_root") or "") if isinstance(b9, dict) else ""
        chk("X6d 원복 확인", got9 == ep_root, got9 or str(st9), ep_root)

    for ln in lines:
        ctx.w(ln)
    if all(checks):
        return done(ItemStatus.PASS, f"xcap-root={ep_root} (CSC 정본, 추종 검증 포함)")
    return done(ItemStatus.FAIL, f"{sum(1 for c in checks if not c)}/{len(checks)} 검사 실패")
