"""S3-SEED — 가입자/그룹 선택 + access_services.jsonl 시드 + csp reload."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import db as _db
from ...common.subscribers import (
    select_subscribers, VOLTE_DOMAIN, MCPTT_DOMAIN,
)
from ...common.access_services import seed_access_services, seed_tls_local_node, signal_csp_reload


def _warm_csc_https(host: str, port: int, tries: int = 6) -> str:
    """dev CSC 관리 서버(HTTPS)를 깨운다 — 200/401/404 등 어떤 HTTP 응답이든 오면 warm."""
    import ssl, time as _t, urllib.request, urllib.error
    ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    for i in range(tries):
        t0 = _t.monotonic()
        try:
            urllib.request.urlopen(f"https://{host}:{port}/internal/aka/av", timeout=5, context=ctx)
            return f"OK {int((_t.monotonic()-t0)*1000)}ms"
        except urllib.error.HTTPError as e:
            return f"OK(http {e.code}) {int((_t.monotonic()-t0)*1000)}ms"
        except Exception:
            _t.sleep(1.0)
    return "FAIL(unreachable)"


@verify_item(
    id="S3-SEED",
    stage=3, category="환경",
    name="가입자/그룹 선택 + access_services.jsonl 시드 + csp reload",
    depends_on=["S3-START"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write", "service-signal"], timeout_s=30,
    execution_order=40,
)
def seed(ctx: VerifyContext) -> ItemResult:
    """Stage 3 스모크용 가입자 정보를 ctx.state 에 적재 + access_services.jsonl 시드.

    대상: dev 환경 csp (build/dist/csp/, build/dist/config/).
    """
    cfg_dir  = os.path.join(ctx.dist_dir, "config")
    pid_file = os.path.join(ctx.dist_dir, "run", "csp.pid")

    # count 는 아래 시나리오의 cspsim -count 와 일치시킨다 — cspsim 이 비밀번호 하나로
    # 그 수만큼 단말을 만들므로, 연속·동일 비밀번호 구간의 첫 가입자를 골라야 한다.
    sub = select_subscribers(_db.csp_db_config(ctx.dist_dir), voip_count=2, ptt_count=5)
    seeded_n = seed_access_services(
        cfg_dir, sub["voip_ref"], sub["ptt_ref"],
        tag="verify-stage3-seed",
        note="auto-seeded by cims_verify S3-SEED",
        with_noxfer=True,   # S3-SCN-XFER 의 REFER 403 게이트용 transfer_allowed=false 변종
    )
    # TLS 접속점 — dev 기본 local_nodes 는 udp-primary 만이라 TLS 전제 시나리오(sec-agree·AKA over
    # TLS)가 5061 refused 로 죽는다(08-26 풀 S3 실측). 라이브 토폴로지대로 TLS 노드를 함께 시드.
    tls_seed = seed_tls_local_node(ctx.dist_dir, ctx.sim_ip, 5061)
    reloaded = signal_csp_reload(pid_file)
    # dev CSC HTTPS(4421) 워밍업 — 기동 직후 첫 TLS 요청이 CSP AV 클라이언트 타임아웃(2s)을 넘겨
    # AKA 제안이 504 로 떨어진 실측(V19a). 시나리오 전에 한 번 두드려 콜드스타트를 흡수한다.
    csc_warm = _warm_csc_https(ctx.sim_ip, 4421)

    voip_auth = f"{sub['voip_imsi']}@{VOLTE_DOMAIN}" if sub["voip_imsi"] else ""
    ptt_auth  = f"{sub['ptt_imsi']}@{MCPTT_DOMAIN}"  if sub["ptt_imsi"]  else ""
    ctx.state.update({
        "VOIP_USER": sub["voip_user"], "VOIP_HA1": sub.get("voip_ha1", ""),
        "VOIP_AUTH": voip_auth, "VOIP_DOM": VOLTE_DOMAIN,
        "VOIP_CREDS": sub.get("voip_creds", []),   # 창 전원의 단말별 자격 — cred_args(-creds) 입력
        "PTT_USER":  sub["ptt_user"],  "PTT_HA1": sub.get("ptt_ha1", ""),
        "PTT_AUTH":  ptt_auth,  "PTT_DOM":  MCPTT_DOMAIN,
        "PTT_CREDS": sub.get("ptt_creds", []),
        "PTT_GROUP": sub["ptt_group"],
    })

    # PASS gate — seed 0건 또는 reload 실패는 downstream (S3-SCN-*) 가
    # stale state 위에서 도는 false PASS 의 원인. 즉시 FAIL 로 차단.
    fail_reasons: list = []
    if seeded_n <= 0:
        fail_reasons.append(f"access_services seed 0건 (cfg_dir={cfg_dir}, voip/ptt 가입자 미존재 가능)")
    if not reloaded:
        fail_reasons.append(f"csp reload(SIGUSR1) 실패 (pid_file={pid_file})")

    lines = [
        f"- VoIP: user={sub['voip_user']!r} domain={VOLTE_DOMAIN} auth_id={voip_auth!r}",
        f"- PTT:  user={sub['ptt_user']!r}  domain={MCPTT_DOMAIN} group={sub['ptt_group']!r}",
        f"- jsonlDir: {cfg_dir}",
        f"- seeded: {seeded_n}건  / csp reload(SIGUSR1): {'OK' if reloaded else 'FAIL'}",
        f"- TLS 접속점(5061): {tls_seed}  / dev CSC 4421 warm-up: {csc_warm}",
    ]
    ctx.w("## S3-SEED — 시나리오 준비")
    for line in lines:
        ctx.w(line)
    if fail_reasons:
        ctx.w("- **FAIL 사유**:")
        for r in fail_reasons:
            ctx.w(f"  - {r}")
    ctx.w()
    return ItemResult(
        id="S3-SEED", name="가입자/그룹 선택 + access_services.jsonl 시드",
        status=ItemStatus.FAIL if fail_reasons else ItemStatus.PASS,
        detail="\n".join(lines + ([f"FAIL: {'; '.join(fail_reasons)}"] if fail_reasons else [])),
        stage=3,
    )
