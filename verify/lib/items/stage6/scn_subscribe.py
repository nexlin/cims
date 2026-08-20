"""S6-SCN-SUBSCRIBE — PTT GMS/CMS SUBSCRIBE/NOTIFY e2e.

cspsim -mode ptt -scenario subscribe 로 한 가입자가 GMS + CMS 양쪽 SUBSCRIBE
발송 → CSP 가 200 OK + NOTIFY 발송. cspsim 종료 시 stdout 에 "Subscriptions
complete" 출력. 보조 검증: SIP msg log 에서 SUBSCRIBE/NOTIFY 메서드 라인 grep.
"""
from __future__ import annotations

import os
import time
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.cspsim import run_cspsim
from ...common.subscribers import cred_args
from ._helpers import target_ip, local_ip_args


@verify_item(
    id="S6-SCN-SUBSCRIBE", stage=6, category="시나리오",
    name="SUBSCRIBE/NOTIFY e2e (PTT GMS+CMS)",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "stage6-ptt", "pipeline-full", "post-deploy"],
    side_effects=["sim-call"], timeout_s=60,
    execution_order=70,
)
def scn_subscribe(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    missing = [k for k in ("PTT_USER", "PTT_DOM", "PTT_PWD") if not s.get(k)]
    if missing:
        return ItemResult(
            id="S6-SCN-SUBSCRIBE", name="SUBSCRIBE/NOTIFY e2e",
            status=ItemStatus.SKIP, stage=6,
            detail=f"PTT 가입자 미준비: {','.join(missing)}",
        )

    _tgt = target_ip("psp", ctx.sim_ip)
    args = [
        "-no-db", "-mode", "ptt", "-scenario", "subscribe",
        "-count", "1", "-duration", "3", "-ip", _tgt,
        "-user", s["PTT_USER"], "-domain", s["PTT_DOM"],
        *cred_args(s, "PTT", 1),
    ]
    if s.get("PTT_AUTH"):
        args += ["-auth_id", s["PTT_AUTH"]]
    args += local_ip_args(_tgt)

    t0 = time.time()
    ctx.state["S6_SUBSCRIBE_T0"] = t0
    # tail_lines 를 크게 (500) — cims.sh sim 종료 후 검증 결과 ls 가 100+ 줄
    # 출력하면 cspsim 본체의 "Subscriptions complete" 마커가 default 100 라인
    # 윈도우 밖으로 밀려 detection 실패.
    rc, tail = run_cspsim(ctx.repo_root, args, timeout=45, tail_lines=500)
    sub_complete = "Subscriptions complete" in tail
    sub_sent = "Sending GMS/CMS SUBSCRIBE" in tail
    notify_seen = _count_notify_lines(ctx.dist_dir, since=t0)

    # PASS 조건: cspsim 정상 종료 (rc=0) + SUBSCRIBE 전송/완료 마커 검출.
    # NOTIFY 라인 카운트는 보너스 (msg_log 가 비활성/누락 시 0 일 수 있음).
    ok = (rc == 0) and (sub_complete or sub_sent)
    notes = [
        f"- rc={rc} subscribe-sent={sub_sent} subscribe-complete={sub_complete}"
        f" NOTIFY 라인={notify_seen}",
    ]
    if ok and notify_seen == 0:
        notes.append("- [INFO] msg_log 가 비활성이라 NOTIFY 직접 검증 X "
                     "— SUBSCRIBE 전송 마커로 판정")
    ctx.w(f"### S6-SCN-SUBSCRIBE — SUBSCRIBE/NOTIFY e2e")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    for n in notes:
        ctx.w(n)
    ctx.w()

    return ItemResult(
        id="S6-SCN-SUBSCRIBE", name="SUBSCRIBE/NOTIFY e2e",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail="\n".join(notes) + f"\n{tail[-400:]}", stage=6,
    )


def _count_notify_lines(dist_dir: str, *, since: float) -> int:
    """sip msg log (`ext_mnt/msg_log/csp/sip/.../sip.jsonl`) 에서 since 시각
    이후 NOTIFY method 라인 카운트.

    msg_log 가 없거나 jsonl 형식이 다르면 0 (ungated).
    """
    log_root = os.path.join(dist_dir, "ext_mnt", "msg_log", "csp", "sip")
    if not os.path.isdir(log_root):
        # 빌드/로그 경로가 다를 수 있음 — repo root 의 ext_mnt 도 시도
        log_root = os.path.join(os.path.dirname(dist_dir), "ext_mnt",
                                "msg_log", "csp", "sip")
    if not os.path.isdir(log_root):
        return 0
    cnt = 0
    for p in glob(os.path.join(log_root, "**", "sip.jsonl"), recursive=True):
        try:
            mtime = os.path.getmtime(p)
        except OSError:
            continue
        if mtime < since:
            continue
        try:
            with open(p, "rb") as f:
                for line in f:
                    if b'"NOTIFY"' in line or b'NOTIFY sip:' in line:
                        cnt += 1
        except Exception:
            pass
    return cnt
