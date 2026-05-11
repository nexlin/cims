"""S6-SCN-IBCF-TRUNK — IBCF 트렁크 라우팅 e2e (ISP → mock 외부 peer).

**현재 상태 (2026-05-11)**: 인프라 (seed/cspsim/시나리오) 완성. 다만 CSP 의
외부 peer inbound auth-skip 흐름 (ModuleDispatcher.cpp L383 G10 주석 — "외부
peer 인바운드는 AclPolicy (remote_nodes 기반) 에서 평가되어야 함 — 추후 확장")
이 미구현이라 caller 의 INVITE 가 ISP 의 가입자 맵 매칭 실패 → 무조건 401
challenge. 따라서 routing_policies 가 정상 시드/sync 되어도 routing path 까지
도달하지 못함. 본 시나리오는 **SKIP** 으로 둔다.

다음 라운드 작업:
  1. AclPolicy + remote_nodes 기반의 외부 peer 식별 흐름 구현
  2. routing_policies 매칭 시 user map 인증 check skip (PendingRouteMap.Has
     로 분기) — 또는
  3. cspsim 에 pre-emptive Digest auth 옵션 추가 (덜 깨끗한 우회)

흐름 설계 (참고용):
  caller cspsim ── INVITE 9000@trunk.peer.test ──▶ ISP (127.0.0.5:5060)
                                                   │
                                  routing_policies │ rule: req_uri_user
                                  매칭 (S6-SEED 시드) │ contains "trunk.peer.test"
                                                   ▼
                                  route_set → route → mock peer (127.0.0.1:6800)
                                                   │
                                                   ▼
                                  mock peer cspsim ── 200 OK ──▶ ISP ──▶ caller
"""
from __future__ import annotations

import os
import subprocess
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext, sanitized_env
from ...common.ibcf_routing import IBCF_PEER_DOMAIN
from .seed import IBCF_MOCK_PEER_IP, IBCF_MOCK_PEER_PORT
from ._helpers import target_ip


def _start_mock_peer(ctx: VerifyContext) -> subprocess.Popen | None:
    """mock 외부 SIP peer 를 cspsim -no_register 모드로 백그라운드 기동.

    stdin=PIPE 로 열어두어 cspsim 의 fgets 가 EOF 로 즉시 break 하지 않도록.
    시나리오 종료 시 "q\\n" 송신으로 정상 종료.
    """
    cspsim_bin = os.path.join(ctx.dist_dir, "cspsim", "bin", "cspsim")
    if not os.path.isfile(cspsim_bin):
        return None
    cwd = os.path.join(ctx.dist_dir, "cspsim")
    args = [
        cspsim_bin,
        "-no_register",
        "-mode", "volte",
        "-server_ip", "127.0.0.1",  # REGISTER 안하므로 dummy
        "-local_ip", IBCF_MOCK_PEER_IP, "-local_port", str(IBCF_MOCK_PEER_PORT),
        "-count", "1",
        "-user", "9000",
        "-domain", IBCF_PEER_DOMAIN,
        "-no_video",
    ]
    try:
        return subprocess.Popen(
            args, cwd=cwd, env=sanitized_env(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
    except Exception:
        return None


def _stop_mock_peer(proc: subprocess.Popen) -> str:
    """mock peer 정상 종료 시도. stdout tail 반환."""
    try:
        if proc.stdin and not proc.stdin.closed:
            proc.stdin.write("q\n")
            proc.stdin.flush()
            proc.stdin.close()
        proc.wait(timeout=5)
    except Exception:
        try: proc.kill()
        except Exception: pass
    out = ""
    try:
        if proc.stdout:
            out = proc.stdout.read() or ""
    except Exception:
        pass
    # 마지막 20 줄만 남김
    return "\n".join(out.splitlines()[-20:])


@verify_item(
    id="S6-SCN-IBCF-TRUNK", stage=6, category="시나리오",
    name="IBCF 트렁크 라우팅 (ISP → mock 외부 peer)",
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["sim-call"], timeout_s=90,
    execution_order=50,
)
def scn_ibcf_trunk(ctx: VerifyContext) -> ItemResult:
    """현재 SKIP. CSP G10 (외부 peer inbound auth-skip) 미구현 → 다음 라운드.

    routing_policies 시드 자체는 S6-SEED 가 정상 수행. ISP 의
    RoutingPolicyEngine sync 도 1 policy 로 동작 확인 (csp log).
    여기서는 cspsim caller 가 401 challenge 를 받기 때문에 시나리오 자체는
    routing path 도달 불가 → 인증 흐름 fix 까지 SKIP.
    """
    item_id = "S6-SCN-IBCF-TRUNK"
    title = "IBCF 트렁크 라우팅"

    isp_ip = target_ip("isp", "127.0.0.5")
    # ISP 의 routing_policies seed 가 실제 적용됐는지 정합 확인.
    isp_cfg = os.path.join(ctx.dist_dir, "ibcf-sip-server", "config", "routing_policies.jsonl")
    seed_ok = os.path.isfile(isp_cfg) and os.path.getsize(isp_cfg) > 0

    skip_reason = (
        "CSP G10 (외부 peer inbound auth-skip) 미구현 — ModuleDispatcher.cpp "
        "L383 주석 참조. routing seed 자체는 적용됨"
        + (" (jsonl OK)" if seed_ok else " (jsonl 미발견)")
        + ". 다음 라운드에서 AclPolicy + remote_nodes 기반 inbound 식별 추가 후 PASS 검증."
    )
    ctx.w(f"### {item_id} — {title}")
    ctx.w(f"- ISP: {isp_ip}:5060  /  mock peer: {IBCF_MOCK_PEER_IP}:{IBCF_MOCK_PEER_PORT}")
    ctx.w(f"- routing seed: {'OK' if seed_ok else 'MISSING'} ({isp_cfg})")
    ctx.w(f"- [SKIP] {skip_reason}")
    ctx.w()
    return ItemResult(
        id=item_id, name=title, status=ItemStatus.SKIP,
        detail=skip_reason, stage=6,
    )


def _scn_ibcf_trunk_full(ctx: VerifyContext) -> ItemResult:
    """본격 LIVE 검증 (G10 구현 후 활성화)."""
    item_id = "S6-SCN-IBCF-TRUNK"
    title = "IBCF 트렁크 라우팅"

    isp_ip = target_ip("isp", "127.0.0.5")

    # 1) mock peer 기동
    peer_proc = _start_mock_peer(ctx)
    if peer_proc is None:
        ctx.w(f"### {item_id} — {title}")
        ctx.w("- [SKIP] cspsim 바이너리 없음")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.SKIP,
            detail="cspsim 바이너리 없음", stage=6,
        )

    time.sleep(2)  # mock peer ready

    # 2) caller — ISP 로 직접 INVITE. bin/cspsim 직접 호출 (cims.sh sim 의
    # 후처리 ls 출력이 stdout tail 을 점거하는 것 회피).
    cspsim_bin = os.path.join(ctx.dist_dir, "cspsim", "bin", "cspsim")
    cspsim_cwd = os.path.join(ctx.dist_dir, "cspsim")
    callee_target = f"9000@{IBCF_PEER_DOMAIN}"  # sip: prefix 없이 — cspsim 이 자동 처리
    caller_args = [
        cspsim_bin,
        "-server_ip", isp_ip, "-server_port", "5060",
        "-mode", "volte", "-scenario", "call",
        "-count", "1", "-call_duration", "3",
        "-user", "8000", "-domain", "csp",
        "-password", "1234", "-no_video",
        "-no_register",
        "-callee_override", callee_target,
    ]
    try:
        caller_proc = subprocess.run(
            caller_args, cwd=cspsim_cwd, env=sanitized_env(),
            capture_output=True, text=True, timeout=30,
        )
        caller_tail = caller_proc.stdout + caller_proc.stderr
        rc = caller_proc.returncode
    except subprocess.TimeoutExpired as e:
        caller_tail = (e.stdout or "") + (e.stderr or "")
        rc = -1

    # 3) mock peer 종료
    peer_tail = _stop_mock_peer(peer_proc)

    # 4) 판정 — routing 동작 검증의 핵심은 mock peer 가 INVITE 를 받았느냐.
    # caller 측 "CALL STARTED" 는 200 OK 수신까지 가야 하는데 1차에서는
    # routing → peer 도착까지만 검증한다 (peer 가 200 OK 자동응답 → caller
    # 도 받을 것).
    caller_invite_sent = "INVITE → " in caller_tail or "INVITE sip:" in caller_tail
    caller_call_started = "CALL STARTED" in caller_tail
    peer_received = "INVITE from=" in peer_tail

    # 본 항목 PASS = routing 정책 매칭으로 peer 까지 INVITE 도달.
    # caller 의 CALL STARTED 는 부수 검증 (있으면 좋고 없어도 routing 검증은 PASS).
    ok = caller_invite_sent and peer_received

    ctx.w(f"### {item_id} — {title}")
    ctx.w(f"- ISP: {isp_ip}:5060  /  mock peer: {IBCF_MOCK_PEER_IP}:{IBCF_MOCK_PEER_PORT}")
    ctx.w(f"- callee: `{callee_target}` (req_uri_user contains \"{IBCF_PEER_DOMAIN}\" → route_set)")
    ctx.w("```")
    ctx.w("[caller tail]")
    for line in caller_tail.splitlines()[-25:]:
        ctx.w(line)
    ctx.w("[peer tail]")
    for line in peer_tail.splitlines()[-15:]:
        ctx.w(line)
    ctx.w("```")
    mark = "[PASS]" if ok else "[FAIL]"
    ctx.w(f"- {mark} caller rc={rc} / "
          f"INVITE 송신={'OK' if caller_invite_sent else 'NO'} / "
          f"200 OK 수신={'OK' if caller_call_started else 'NO'} / "
          f"peer INVITE 수신={'OK' if peer_received else 'NO'}")
    ctx.w()
    return ItemResult(
        id=item_id, name=title,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"rc={rc}, invite_sent={caller_invite_sent}, "
               f"call_started={caller_call_started}, peer_received={peer_received}",
        stage=6,
    )
