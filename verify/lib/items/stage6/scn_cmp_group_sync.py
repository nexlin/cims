"""S6-CMP-GROUP-SYNC — 세션 중 admin 그룹 변경의 CMP 전파 검증.

규격 모델 (MCPTT on-demand — GroupCallService.SyncGroupsState 주석 참조):
CMP 의 그룹은 상시 디렉터리가 아니라 **미디어 세션 자원**(floor·멤버 포트)이다.
CSP 는 CMP 그룹 컨텍스트를 proactive 하게 만들지 않는다 — 수립은 발신
INVITE(on-demand) 또는 affiliation 합류가 담당하고, admin 변경은 **세션이
살아있는 동안** GROUP_CHANGED notify → SyncGroupsState(설정 해시 비교) →
PTT_GROUP_MODIFY 로 전파된다. 이 체인이 끊겨도 신규 세션은 정상이라 겉으로
드러나지 않는다(진행 중 세션만 낡은 사본으로 동작) — 그래서 게이트가 필요하다.

흐름:
  1. 배포본 OAM(4445) admin login + CMP(PMP) STATS reachability.
  2. cspsim PTT 그룹콜(PTT_GROUP, 5인)을 **배경 기동** — on-demand 로 CMP 그룹 수립.
  3. 폴링: STATS `group_details[]` 에 그룹 등장 (+ 현재 floor_policy 관측).
  4. 세션 중 admin PUT /ptt/groups/{gid} 로 floor_policy 변경(관측값과 다른 값,
     single↔multi 토글) → 폴링: STATS floor_policy 에 변경 반영.
  5. cleanup: floor_policy 원복 + sim 종료.

구(舊) 전제(그룹 생성만으로 CMP 에 상시 roster 가 생긴다)는 규격 정합에서
폐기된 동작이라 검증하지 않는다.
"""
from __future__ import annotations

import os
import subprocess
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext, sanitized_env
from ...common import csc_http
from ...common.cmp_client import cmp_stats
from ._helpers import target_ip, local_ip_args


def _deployed_admin_base(ctx: VerifyContext) -> str:
    """admin CRUD 진입점 — 배포본 OAM(게이트웨이). auth/login 은 OAM 소유이고
    가입자/그룹 라우트는 csc 모듈이 install 시 self-register 한 프록시로 도달."""
    return csc_http.deployed_mgmt_base((ctx.opts or {}).get("target") or "verify")


def _stats_group(stats: dict | None, gid: str) -> dict | None:
    """STATS group_details 에서 gid entry (없으면 None)."""
    if not isinstance(stats, dict):
        return None
    for g in stats.get("group_details") or []:
        if isinstance(g, dict) and g.get("group_id") == gid:
            return g
    return None


@verify_item(
    id="S6-CMP-GROUP-SYNC", stage=6, category="시나리오",
    name="CMP roster sync (세션 중 admin 변경 → PTT_GROUP_MODIFY → STATS)",
    depends_on=["S6-SCN-DB-SYNC"],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["db-write", "network", "sim-call"], timeout_s=180,
    execution_order=81,
)
def scn_cmp_group_sync(ctx: VerifyContext) -> ItemResult:
    notes: list = []
    s = ctx.state
    gid = s.get("PTT_GROUP") or ""
    if not gid or not s.get("PTT_DOM"):
        return _skip(ctx, "PTT 가입자/그룹 미준비 (S6-SEED 미실행?)")

    base = _deployed_admin_base(ctx)
    login_id = os.environ.get("CIMS_TB_ADMIN_ID", "admin")
    pw = os.environ.get("CIMS_TB_ADMIN_PASSWORD", "1234")
    # PTT 그룹 동기화는 PMP 미디어를 검증 (CSP 의 PTT_AS 가 PSP 로 분리된 P1 토폴로지).
    cmp_ip = s.get("CMP_IP") or target_ip("pmp", "127.0.0.1")
    cmp_port = int(s.get("CMP_PORT") or 9000)

    try:
        tok = csc_http.admin_login(base, login_id, pw, timeout=5)
    except Exception as e:
        return _skip(ctx, f"배포본 OAM({base}) login 예외: {type(e).__name__}: {e}")
    if not tok:
        return _skip(ctx, f"배포본 OAM({base}) login 실패 — S5 미실행?")
    notes.append(f"- login: {base} OK")

    pre = cmp_stats(ip=cmp_ip, port=cmp_port, timeout=1.0)
    if pre is None:
        return _skip(ctx, f"CMP {cmp_ip}:{cmp_port} STATS 응답 없음 — 미기동/방화벽?")
    notes.append(f"- CMP {cmp_ip}:{cmp_port} STATS 응답 OK "
                 f"(groups_before={pre.get('groups')})")

    # 원본 floor 설정 백업 (cleanup 원복용) — GET 실패 시 관측값으로 대신한다.
    orig_fp, orig_mt = None, None
    try:
        g = csc_http.get_json(f"{base}/api/v1/ptt/groups/{gid}", token=tok, timeout=10)
        if isinstance(g, dict):
            orig_fp = g.get("floor_policy")
            orig_mt = g.get("max_talkers")
    except Exception:
        pass

    # ── 세션 기동 (on-demand CMP 그룹 수립) ────────────────────────────────
    tgt = target_ip("psp", ctx.sim_ip)
    sim_args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "30", "-ip", tgt,
        "-domain", s["PTT_DOM"], "-group", gid, "-no_video",
    ] + local_ip_args(tgt)
    cims_sh = os.path.join(ctx.repo_root, "cims.sh")
    try:
        proc = subprocess.Popen(
            ["/bin/bash", cims_sh, "sim"] + sim_args,
            cwd=ctx.repo_root, env=sanitized_env(),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except Exception as e:
        return _skip(ctx, f"cspsim 기동 실패: {type(e).__name__}: {e}")
    notes.append(f"- cspsim 그룹콜 배경 기동 (group={gid}, 5인, duration=30s)")

    ok = False
    try:
        # ① 그룹 등장 폴링 — 등록(배포본에서 20s+ 소요 관측) + INVITE 까지 여유.
        entry = None
        for i in range(60):
            time.sleep(1)
            entry = _stats_group(cmp_stats(ip=cmp_ip, port=cmp_port, timeout=1.0), gid)
            if entry:
                notes.append(
                    f"- [OK] on-demand 수립: STATS 에 {gid} 등장 ({i + 1}s, "
                    f"members={entry.get('members')} "
                    f"floor_policy={entry.get('floor_policy')})")
                break
        if not entry:
            notes.append(f"- [FAIL] 60s 내 STATS 에 {gid} 미등장 — 그룹콜 수립 실패?")
            return _finish(ctx, notes, False)

        # ② 세션 중 admin floor 정책 변경 → PTT_GROUP_MODIFY 전파 확인.
        #    관측값과 다른 값으로 토글 — multi 는 정원(max_talkers) 필수.
        cur_fp = str(entry.get("floor_policy") or orig_fp or "single")
        if cur_fp == "multi":
            put_body: dict = {"floor_policy": "single"}
            new_fp = "single"
        else:
            put_body = {"floor_policy": "multi", "max_talkers": 3}
            new_fp = "multi"
        st, body = csc_http.put_json(
            f"{base}/api/v1/ptt/groups/{gid}", put_body, token=tok, timeout=10,
        )
        if st != 200:
            notes.append(f"- [FAIL] PUT floor_policy={new_fp} status={st} "
                         f"body={str(body)[:120]}")
            return _finish(ctx, notes, False)
        notes.append(f"- admin PUT: floor_policy {cur_fp} → {new_fp} (status={st})")

        for i in range(15):
            time.sleep(1)
            entry = _stats_group(cmp_stats(ip=cmp_ip, port=cmp_port, timeout=1.0), gid)
            if entry and str(entry.get("floor_policy")) == new_fp:
                notes.append(f"- [OK] 세션 중 전파: STATS floor_policy={new_fp} "
                             f"반영 ({i + 1}s)")
                ok = True
                break
        if not ok:
            cur = entry.get("floor_policy") if entry else "(그룹 소실)"
            notes.append(f"- [FAIL] 15s 내 STATS floor_policy 미반영 (현재={cur})")
    finally:
        # 원복 — 백업/기본값 기준. 실패해도 판정에는 영향 없음 (노트만).
        try:
            restore: dict = {"floor_policy": orig_fp or "single"}
            if orig_mt:
                restore["max_talkers"] = orig_mt
            rst, _ = csc_http.put_json(
                f"{base}/api/v1/ptt/groups/{gid}", restore, token=tok, timeout=10,
            )
            notes.append(f"- 원복: floor_policy={restore['floor_policy']} status={rst}")
        except Exception as e:
            notes.append(f"- [WARN] 원복 실패: {type(e).__name__}: {e}")
        try:
            proc.terminate()
            proc.wait(timeout=15)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        notes.append("- cspsim 종료")

    return _finish(ctx, notes, ok)


def _finish(ctx: VerifyContext, notes: list, ok: bool) -> ItemResult:
    ctx.w("### S6-CMP-GROUP-SYNC — CMP roster 동기화 (세션 중 변경 전파)")
    for n in notes:
        ctx.w(n)
    ctx.w()
    return ItemResult(
        id="S6-CMP-GROUP-SYNC", name="CMP roster sync",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        stage=6, detail="\n".join(notes),
    )


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-CMP-GROUP-SYNC — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-CMP-GROUP-SYNC", name="CMP roster sync",
        status=ItemStatus.SKIP, stage=6, detail=reason,
    )
