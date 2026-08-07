"""S6-SCN-DB-SYNC — admin CRUD → notify_csp UDP → CSP 캐시 갱신 검증.

흐름:
  1. 배포본 csc(4445) admin login.
  2. 임시 PTT 그룹 (group_id="verify-test-<ts>") 추가 → POST /api/v1/ptt/groups.
     CSC 가 notify_csp("GROUP_CHANGED", uri, "POST") UDP 발송.
  3. 잠시 대기 (1~2s) — UDP 전파 + CSP 처리.
  4. 배포본 csp 로그 (`build/dist/<agent_name>/<dir>/csp/log/csp_*.log` —
     volte-sip-server/csp + ptt-sip-server/psp 등) 의 마지막 N 라인에
     GROUP_CHANGED / group_change 라인 grep.
  5. cleanup: DELETE 임시 그룹.

검증 시 사용자 데이터 영향 X (임시 그룹은 verify 식별자 prefix). 실패해도
cleanup 시도. PASS 조건: HTTP 200/201 + csp 로그에 변경 알림 라인 발견.
"""
from __future__ import annotations

import os
import time
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import csc_http


def _deployed_admin_base(ctx: VerifyContext) -> str:
    """admin CRUD 진입점 — 배포본 OAM(게이트웨이). auth/login 은 OAM 소유이고
    가입자/그룹 라우트는 csc 모듈이 install 시 self-register 한 프록시로 도달."""
    return csc_http.deployed_mgmt_base((ctx.opts or {}).get("target") or "verify")


@verify_item(
    id="S6-SCN-DB-SYNC", stage=6, category="시나리오",
    name="DB 가입자/그룹 sync (admin CRUD → notify_csp → CSP 캐시)",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["db-write", "network"], timeout_s=30,
    execution_order=80,
)
def scn_db_sync(ctx: VerifyContext) -> ItemResult:
    notes: list = []

    # 1) 배포본 OAM admin login (target 의 csc 포트)
    base = _deployed_admin_base(ctx)
    login_id = os.environ.get("CIMS_TB_ADMIN_ID", "admin")
    pw = os.environ.get("CIMS_TB_ADMIN_PASSWORD", "1234")
    try:
        tok = csc_http.admin_login(base, login_id, pw, timeout=5)
    except Exception as e:
        return _skip(ctx, f"배포본 OAM login 예외: {type(e).__name__}: {e}")
    if not tok:
        return _skip(ctx, f"배포본 csc({base}) login 실패 — S5 미실행?")
    notes.append(f"- login: {base} OK")

    # 2) 임시 그룹 추가 — verify-test-<ms>
    # admin API 가 'id' 필드를 요구 (id is required) — name 은 옵션 (default = id).
    gid = f"verify-test-{int(time.time() * 1000)}"
    create_payload = {
        "id":       gid,
        "name":     f"verify-test-{gid[-6:]}",
        "members":  [],
    }
    csp_log_paths = _csp_log_paths(ctx.dist_dir)
    log_offsets_before = _log_offsets(csp_log_paths)

    try:
        st, body = csc_http.post_json(
            f"{base}/api/v1/ptt/groups", create_payload, token=tok, timeout=10,
        )
    except Exception as e:
        return _skip(ctx, f"POST /ptt/groups 예외: {type(e).__name__}: {e}")
    if st not in (200, 201):
        return _fail(ctx, f"POST /ptt/groups status={st} body={str(body)[:200]}")
    notes.append(f"- 임시 그룹 추가: gid={gid} status={st}")

    try:
        # 3) 전파 대기
        time.sleep(2)

        # 4) csp 로그 grep — 새 라인 중에서 group/user change 패턴 검색
        new_lines = _read_log_tail(csp_log_paths, log_offsets_before)
        notify_lines = [
            ln for ln in new_lines
            if any(k in ln for k in (
                "GROUP_CHANGED", "group_change", "GroupChange",
                gid, "USER_CHANGED",
            ))
        ]
        notes.append(f"- csp 로그 새 라인: {len(new_lines)}, notify 라인: {len(notify_lines)}")
        for ln in notify_lines[:3]:
            notes.append(f"  - {ln[:160]}")

        ok = len(notify_lines) >= 1
    finally:
        # 5) cleanup
        try:
            csc_http.delete(f"{base}/api/v1/ptt/groups/{gid}", token=tok)
            notes.append(f"- 임시 그룹 삭제: gid={gid}")
        except Exception as e:
            notes.append(f"- [WARN] cleanup 실패: {type(e).__name__}: {e}")

    ctx.w("### S6-SCN-DB-SYNC — admin CRUD → notify_csp → CSP 캐시")
    for n in notes: ctx.w(n)
    ctx.w()

    return ItemResult(
        id="S6-SCN-DB-SYNC", name="DB 가입자/그룹 sync",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail="\n".join(notes), stage=6,
    )


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-SCN-DB-SYNC — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-SCN-DB-SYNC", name="DB 가입자/그룹 sync",
        status=ItemStatus.SKIP, detail=reason, stage=6,
    )


def _fail(ctx: VerifyContext, msg: str) -> ItemResult:
    ctx.w("### S6-SCN-DB-SYNC — FAIL")
    ctx.w(f"- {msg}")
    ctx.w()
    return ItemResult(
        id="S6-SCN-DB-SYNC", name="DB 가입자/그룹 sync",
        status=ItemStatus.FAIL, detail=msg, stage=6,
    )


def _csp_log_paths(dist_dir: str) -> list:
    """배포본 csp 변종 (CSP/PSP/ISP) 모든 인스턴스의 log 파일 list.
    GROUP_CHANGED notify 는 PSP 로, USER_CHANGED 는 CSP+PSP broadcast (mcptt.py
    의 _notify_targets 라우팅) — 양쪽 로그를 모두 grep 해야 검출 가능."""
    from ..stage5._native_steps import _INSTANCES as _NATIVE_INSTANCES
    paths: list = []
    for inst in _NATIVE_INSTANCES:
        if inst.get("tarball") not in ("csp", "psp", "isp"):
            continue
        # 버전형 설치(current 통로): <agent>/modules/<dir>/current/<dir>/log/csp_*.log.
        # csp ELF 의 SystemId 기반 prefix 라 변종 (psp/isp) 도 csp_*.log 그대로.
        # 구(평탄) 설치 fallback: <agent>/<dir>/log/csp_*.log.
        log_glob = os.path.join(
            dist_dir, inst["agent_name"], "modules", inst["dir"], "current",
            inst["dir"], "log", "csp_*.log",
        )
        hits = glob(log_glob)
        if not hits:
            hits = glob(os.path.join(
                dist_dir, inst["agent_name"], inst["dir"], "log", "csp_*.log"))
        paths.extend(hits)
    return sorted(paths)


def _log_offsets(paths: list) -> dict:
    """파일별 현재 size — 이후 새로 추가된 라인만 읽기 위한 anchor."""
    out: dict = {}
    for p in paths:
        try:
            out[p] = os.path.getsize(p)
        except OSError:
            out[p] = 0
    return out


def _read_log_tail(paths: list, offsets_before: dict) -> list:
    """offsets_before 이후로 추가된 새 라인. 파일별 합산."""
    all_lines: list = []
    for p in paths:
        start = offsets_before.get(p, 0)
        try:
            with open(p, "rb") as f:
                f.seek(start)
                data = f.read()
        except OSError:
            continue
        for raw in data.decode("utf-8", errors="replace").splitlines():
            all_lines.append(raw)
    return all_lines
