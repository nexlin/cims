"""Phase 3 의 4시나리오 — VoLTE 음성/영상, PTT 그룹 음성/영상.

각 시나리오는 cspsim 호출 + 녹취 파일 증분(delta>=1) 으로 PASS 판정.
ctx.state 에 P3-SEED 가 적재한 가입자 정보를 사용.
"""
from __future__ import annotations

import os
import subprocess
import time
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext, sanitized_env


def _count_recordings(dist_dir: str) -> int:
    pat = os.path.join(dist_dir, "ext_mnt", "service_log", "**", "seg_*.rtp")
    return len(glob(pat, recursive=True))


def _run_cspsim(ctx: VerifyContext, sim_args: list, timeout: int = 120) -> tuple:
    """cims.sh sim <args> 실행. (rc, stdout_tail) 반환."""
    cims_sh = os.path.join(ctx.repo_root, "cims.sh")
    cmd = ["/bin/bash", cims_sh, "sim"] + sim_args
    try:
        proc = subprocess.run(
            cmd, cwd=ctx.repo_root, env=sanitized_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, text=True,
        )
        out = proc.stdout or ""
        tail = "\n".join(out.splitlines()[-25:])
        return (proc.returncode, tail)
    except subprocess.TimeoutExpired:
        return (-1, "(timeout)")
    except Exception as e:
        return (-2, f"({type(e).__name__}: {e})")


def _scenario_run(ctx: VerifyContext, item_id: str, title: str,
                  sim_args: list, prereq_keys: list) -> ItemResult:
    """공통 시나리오 실행 + 녹취 delta 판정."""
    # 가입자 정보 부재 시 SKIP
    missing = [k for k in prereq_keys if not ctx.state.get(k)]
    if missing:
        ctx.w(f"### {item_id} — {title}")
        ctx.w(f"- [SKIP] 가입자 정보 부족: {','.join(missing)}")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.SKIP,
            detail=f"가입자/그룹 미준비: {','.join(missing)}", phase=3,
        )

    rec_before = _count_recordings(ctx.dist_dir)
    rc, stdout_tail = _run_cspsim(ctx, sim_args)
    rec_after = _count_recordings(ctx.dist_dir)
    delta = rec_after - rec_before
    ok = delta >= 1

    ctx.w(f"### {item_id} — {title}")
    ctx.w("```")
    for line in stdout_tail.splitlines(): ctx.w(line)
    ctx.w("```")
    mark = "[PASS]" if ok else "[FAIL]"
    ctx.w(f"- {mark} 녹취 파일 +{delta} (rc={rc})")
    ctx.w()

    return ItemResult(
        id=item_id, name=title,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"녹취 +{delta} (rc={rc})\n{stdout_tail[-500:]}",
        phase=3,
    )


@verify_item(
    id="P3-SCN-VOLTE-VOICE", phase=3, category="시나리오",
    name="VoLTE 음성 2자 통화",
    depends_on=["P3-SEED"], presets=["phase3-full"],
    side_effects=["sim-call"], timeout_s=60,
)
def scn_volte_voice(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", ctx.sim_ip,
        "-user", s["VOIP_USER"], "-domain", s["VOIP_DOM"],
        "-password", s["VOIP_PWD"], "-no_video",
    ]
    if s.get("VOIP_AUTH"): args += ["-auth_id", s["VOIP_AUTH"]]
    return _scenario_run(ctx, "P3-SCN-VOLTE-VOICE",
                         "VoLTE 음성 2자 통화", args, ["VOIP_USER"])


@verify_item(
    id="P3-SCN-VOLTE-VIDEO", phase=3, category="시나리오",
    name="VoLTE 영상 2자 통화",
    depends_on=["P3-SEED"], presets=["phase3-full"],
    side_effects=["sim-call"], timeout_s=60,
)
def scn_volte_video(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-no-db", "-mode", "volte", "-scenario", "call",
        "-count", "2", "-duration", "5", "-ip", ctx.sim_ip,
        "-user", s["VOIP_USER"], "-domain", s["VOIP_DOM"],
        "-password", s["VOIP_PWD"],
    ]
    if s.get("VOIP_AUTH"): args += ["-auth_id", s["VOIP_AUTH"]]
    return _scenario_run(ctx, "P3-SCN-VOLTE-VIDEO",
                         "VoLTE 영상 2자 통화", args, ["VOIP_USER"])


@verify_item(
    id="P3-SCN-PTT-VOICE", phase=3, category="시나리오",
    name="PTT 그룹 음성 통화 (5인)",
    depends_on=["P3-SEED"], presets=["phase3-full"],
    side_effects=["sim-call"], timeout_s=90,
)
def scn_ptt_voice(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", ctx.sim_ip,
        "-domain", s["PTT_DOM"], "-group", s["PTT_GROUP"], "-no_video",
    ]
    return _scenario_run(ctx, "P3-SCN-PTT-VOICE",
                         "PTT 그룹 음성 통화 (5인)", args,
                         ["PTT_USER", "PTT_GROUP"])


@verify_item(
    id="P3-SCN-PTT-VIDEO", phase=3, category="시나리오",
    name="PTT 그룹 영상 통화 (5인)",
    depends_on=["P3-SEED"], presets=["phase3-full"],
    side_effects=["sim-call"], timeout_s=90,
)
def scn_ptt_video(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    args = [
        "-mode", "ptt", "-scenario", "group_call",
        "-count", "5", "-duration", "10", "-ip", ctx.sim_ip,
        "-domain", s["PTT_DOM"], "-group", s["PTT_GROUP"],
    ]
    return _scenario_run(ctx, "P3-SCN-PTT-VIDEO",
                         "PTT 그룹 영상 통화 (5인)", args,
                         ["PTT_USER", "PTT_GROUP"])
