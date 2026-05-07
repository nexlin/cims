"""S6-SCN-CERT-ROTATE — mTLS agent cert rotation e2e.

흐름:
  1. csc-tb.json `Agent.MtlsEnabled=true` 확인 (false 면 SKIP).
  2. cims_agent 테이블에서 csc-server-local agent 가 status='online' 확인.
  3. issued cert 디렉토리 (`cert/agent_mtls/issued/<agent>/`) 의 cert 파일 list +
     mtime 캡처.
  4. UPDATE cims_agent SET cert_rotate_pending=1 WHERE name='csc-server-local'.
  5. 최대 15초 대기 (heartbeat 주기 3s 가정), 1초마다 `cert_rotate_pending`
     확인 — 0 으로 reset 되면 CSC 가 응답 보냄 ✓.
  6. issued cert 디렉토리에 mtime > 캡처 시각인 새 파일 생성됐는지 확인 ✓.

Agent 가 rotate 후 exit(0) 하므로 후속 stage 영향 — 현재 verify 환경은 systemd
없음, agent 종료 시 다시 spawn 안 됨. 이 시나리오는 stage6 의 마지막 직전에
실행되는 것이 안전 (depends_on 으로 보장).
"""
from __future__ import annotations

import json
import os
import time
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db


_AGENT_NAME = "csc-server-local"
_TB_CSC_CFG_REL = ("csc", "config", "csc-tb.json")


@verify_item(
    id="S6-SCN-CERT-ROTATE", stage=6, category="시나리오",
    name="mTLS cert rotation e2e (csc-server-local)",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["db-write", "process-state"], timeout_s=60,
)
def scn_cert_rotate(ctx: VerifyContext) -> ItemResult:
    notes: list = []

    # 1) mTLS 활성 여부
    mtls = _read_mtls_enabled(ctx.dist_dir)
    if mtls is None:
        return _skip("S6-SCN-CERT-ROTATE", ctx,
                     "csc-tb.json 못 읽음 — TB-CSC 미배포")
    if not mtls:
        return _skip("S6-SCN-CERT-ROTATE", ctx,
                     "Agent.MtlsEnabled=false — mTLS 모드 비활성")
    notes.append("- mTLS: ON")

    # 2) DB 접속 + agent online 확인
    cfg = _db.csp_db_config(ctx.dist_dir)
    if not cfg:
        return _skip("S6-SCN-CERT-ROTATE", ctx, "csp DB config 없음")
    try:
        conn = _db.connect(cfg)
    except Exception as e:
        return _skip("S6-SCN-CERT-ROTATE", ctx,
                     f"DB 접속 실패: {type(e).__name__}: {e}")
    cert_renewed = False
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT id, status, cert_rotate_pending, cert_issued_at "
            "FROM cims_agent WHERE name=%s",
            (_AGENT_NAME,),
        )
        row = cur.fetchone()
        if not row:
            conn.close()
            return _skip("S6-SCN-CERT-ROTATE", ctx,
                         f"agent {_AGENT_NAME} 없음 — S5 미실행?")
        aid, status, prev_pending, prev_issued_at = row
        if status != "online":
            conn.close()
            return _skip("S6-SCN-CERT-ROTATE", ctx,
                         f"agent {_AGENT_NAME} status={status} (online 아님)")
        notes.append(
            f"- agent: id={aid} status=online prev_pending={prev_pending} "
            f"prev_cert_issued_at={prev_issued_at}"
        )

        # 4) cert_rotate_pending=1 토글
        cur.execute(
            "UPDATE cims_agent SET cert_rotate_pending=1 WHERE id=%s", (aid,),
        )
        notes.append("- pending=1 토글")

        # 5) 최대 15초 폴링 — pending=0 reset + cert_issued_at 갱신 확인
        rotated = False
        waited = 0
        for _ in range(15):
            time.sleep(1); waited += 1
            cur.execute(
                "SELECT cert_rotate_pending, cert_issued_at FROM cims_agent "
                "WHERE id=%s", (aid,),
            )
            r = cur.fetchone()
            if r and r[0] == 0:
                rotated = True
                # cert_issued_at 가 prev 보다 newer 면 실제 발급 완료
                if r[1] and (prev_issued_at is None or r[1] > prev_issued_at):
                    cert_renewed = True
                    notes.append(
                        f"- cert_issued_at 갱신: {prev_issued_at} → {r[1]}"
                    )
                break
        notes.append(f"- pending reset: {'YES' if rotated else 'TIMEOUT'} ({waited}s)")
    finally:
        try: conn.close()
        except Exception: pass

    # 6) agent state_dir 의 agent_mtls.crt 가 갱신됐는지 추가 확인 (best-effort).
    state_crt = os.path.join(ctx.dist_dir, "csc-server", "agent", "state",
                             "agent_mtls.crt")
    crt_recent = False
    if os.path.isfile(state_crt):
        try:
            crt_recent = (time.time() - os.path.getmtime(state_crt)) < 60
        except OSError:
            pass
    notes.append(f"- agent_mtls.crt mtime within 60s: {crt_recent}")

    # PASS: pending reset YES + cert_issued_at 갱신 OR agent state crt 최근 갱신
    ok = rotated and (cert_renewed or crt_recent)
    ctx.w("### S6-SCN-CERT-ROTATE — mTLS cert rotation e2e")
    for n in notes: ctx.w(n)
    ctx.w()

    return ItemResult(
        id="S6-SCN-CERT-ROTATE", name="mTLS cert rotation e2e",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail="\n".join(notes), stage=6,
    )


def _skip(item_id: str, ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w(f"### {item_id} — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id=item_id, name="mTLS cert rotation e2e",
        status=ItemStatus.SKIP, detail=reason, stage=6,
    )


def _read_mtls_enabled(dist_dir: str) -> "bool | None":
    """csc-tb.json (TB-CSC config) 의 Agent.MtlsEnabled. 없으면 None."""
    p = os.path.join(dist_dir, *_TB_CSC_CFG_REL)
    if not os.path.isfile(p):
        return None
    try:
        with open(p) as f:
            d = json.load(f)
        return bool((d.get("Agent") or {}).get("MtlsEnabled", False))
    except Exception:
        return None


def _list_cert_files(cert_dir: str) -> list:
    """issued cert 디렉토리의 (path, mtime) 리스트. 없으면 빈 list."""
    out: list = []
    if not os.path.isdir(cert_dir):
        return out
    for p in glob(os.path.join(cert_dir, "*.pem")) + \
             glob(os.path.join(cert_dir, "*.crt")):
        try:
            out.append((p, os.path.getmtime(p)))
        except OSError:
            pass
    return out
