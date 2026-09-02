"""S3 미디어 SRTP 회귀 — SDES 협상·종단·게이트 (media_security.md §9).

UE↔CMP 구간 SRTP(RFC 3711 + SDES RFC 4568)의 dev 스택 회귀. 접속서비스 정책
(`media_srtp`)을 dist access_services.jsonl 에서 플립하고 SIGUSR1 로 reload 한 뒤
cspsim `-srtp` 군/대조군을 돌린다 (자기복원 — 종료 시 S3-SEED 원본으로 복구).

검사:
  R1 required 협상+종단 — `-srtp required` 그룹콜: cspsim "[RTP] SRTP enabled"(SAVP
     a=crypto 왕복) + CMP "member media SRTP audio enabled"(media_crypto 수신) +
     신규 녹취/이벤트 ≥1 (unprotect 후 평문 녹취 — 탭 이동 검증)
  R2 required 488 게이트 — 평문 offer(`-srtp off`) → CSP "SRTP negotiation failed →488"
  R3 optional 관대 수용 — `-srtp optional`(AVP+a=crypto best-effort) 도 SRTP 로 성립
  R4 off 대조군 — 정책·단말 모두 off 로 원복 후 평문 그룹콜 그린 (기존 동작 유지)
  R5 VoLTE relay leg 종단 — volte 서비스 required 플립 + `-mode volte -scenario call
     -srtp required`(영상 동반 — cims.sh sim 기본 미디어): 양 단말 오디오·비디오 SRTP 성립 +
     CMP relay leg crypto audio("SRTP audio peer[")·video("SRTP video peer[") 각 2건
     (leg·m-line 별 독립 키 — crypto 투과가 아니라 CSP 재작성·종단, media_security.md §5.2)

와이어 캡처 실측(§9-2)·혼용 그룹(§9-5)·mediasec 능력 기반 offer(§9-6)는 실기기/
패킷캡처 전제 — 라이브 정지 창 절차로 별도 수행한다.
"""
from __future__ import annotations

import glob
import json
import os
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.subscribers import MCPTT_DOMAIN, VOLTE_DOMAIN, cred_args
from ...common.access_services import signal_csp_reload
from ...common.cspsim import run_cspsim
from ...common.recordings import count_recordings, count_ptt_events

_RID = "S3-SCN-SRTP"
_RNAME = "미디어 SRTP (SDES 협상·CMP 종단·488 게이트·off 대조군)"


def _jsonl_path(ctx: VerifyContext) -> str:
    return os.path.join(ctx.dist_dir, "config", "access_services.jsonl")


def _set_media_srtp(path: str, kind: str, value: str) -> bool:
    """kind 레코드의 media_srtp 를 value 로 (빈 값 = 필드 제거). 성공 시 True."""
    try:
        rows = []
        hit = False
        with open(path, encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                r = json.loads(line)
                if r.get("kind") == kind:
                    if value:
                        r["media_srtp"] = value
                    else:
                        r.pop("media_srtp", None)
                    hit = True
                rows.append(r)
        if not hit:
            return False
        with open(path, "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        return True
    except Exception:
        return False


def _grep_logs(pattern: str, dirs: list, since: float) -> int:
    """since 이후 수정된 로그 파일에서 pattern 등장 횟수 합산 (간이 — 파일 단위 mtime 필터)."""
    n = 0
    for d in dirs:
        for path in glob.glob(os.path.join(d, "*.log")):
            try:
                if os.path.getmtime(path) < since:
                    continue
                with open(path, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        if pattern in line:
                            n += 1
            except Exception:
                continue
    return n


@verify_item(
    id=_RID,
    stage=3, category="시나리오",
    name=_RNAME,
    depends_on=["S3-SEED"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write", "service-signal", "sim-call"], timeout_s=600,
    execution_order=62,
)
def srtp(ctx: VerifyContext) -> ItemResult:
    s = ctx.state
    ctx.w(f"### {_RID} — {_RNAME}")

    def done(status: ItemStatus, detail: str) -> ItemResult:
        ctx.w()
        return ItemResult(id=_RID, name=_RNAME, status=status, detail=detail, stage=3)

    missing = [k for k in ("PTT_USER", "PTT_GROUP") if not s.get(k)]
    if missing:
        ctx.w(f"- [SKIP] 가입자 정보 부족: {','.join(missing)}")
        return done(ItemStatus.SKIP, f"가입자/그룹 미준비: {','.join(missing)}")
    jsonl = _jsonl_path(ctx)
    if not os.path.isfile(jsonl):
        ctx.w(f"- [SKIP] {jsonl} 없음 (S3-SEED 선행)")
        return done(ItemStatus.SKIP, "access_services.jsonl 없음")

    pid_file = os.path.join(ctx.dist_dir, "run", "csp.pid")
    log_dirs = [os.path.join(ctx.dist_dir, "cmp", "log"), os.path.join(ctx.dist_dir, "csp", "log")]
    media_dir = os.path.join(ctx.repo_root, "tests", "media")

    def flip(value: str, kind: str = "ptt") -> bool:
        if not _set_media_srtp(jsonl, kind, value):
            return False
        return signal_csp_reload(pid_file)

    # 마커는 tail 이 아니라 on_line 전 스트림에서 찾는다 — 5인 그룹콜 출력이
    # tail 창(100줄)을 넘겨 초반 "[RTP] SRTP enabled" 가 잘린다.
    def srtp_watch() -> tuple:
        seen = {"srtp": False, "video": 0}

        def on_line(line: str):
            if "[RTP] SRTP enabled" in line:
                seen["srtp"] = True
            elif "[RTP] SRTP video enabled" in line:
                seen["video"] += 1
        return seen, on_line

    def group_call(srtp_arg: str) -> tuple:
        """cspsim 그룹콜 1회 — (rc, tail, 신규 녹취+이벤트, cspsim SRTP enabled 여부)"""
        t0 = time.time()
        args = [
            "-mode", "ptt", "-scenario", "group_call",
            "-count", "5", "-duration", "8", "-ip", ctx.sim_ip,
            "-user", s.get("PTT_USER", ""),
            "-domain", s.get("PTT_DOM", MCPTT_DOMAIN),
            *cred_args(s, "PTT", 5),
            "-group", s.get("PTT_GROUP", ""),
            "-media_dir", media_dir,
            "-srtp", srtp_arg,
        ]
        seen, on_line = srtp_watch()
        rc, tail = run_cspsim(ctx.repo_root, args, timeout=120, on_line=on_line)
        delta = count_recordings(ctx.dist_dir, since=t0) + count_ptt_events(ctx.dist_dir, since=t0)
        return rc, tail, delta, seen["srtp"], t0

    checks = []  # (이름, ok, 상세)
    try:
        # ── R1: required — SAVP 협상 + CMP 종단 + 평문 녹취 ──
        if not flip("required"):
            ctx.w("- [FAIL] 정책 플립(required)/reload 실패")
            return done(ItemStatus.FAIL, "정책 플립 실패")
        rc, tail, delta, sim_srtp, t0 = group_call("required")
        cmp_srtp = _grep_logs("member media SRTP audio enabled", log_dirs[:1], t0 - 5)
        ok1 = sim_srtp and cmp_srtp >= 1 and delta >= 1
        checks.append(("R1 required 협상+종단",
                       ok1, f"sim_srtp={sim_srtp} cmp_media_crypto={cmp_srtp} 녹취/이벤트 +{delta} rc={rc}"))

        # ── R2: required — 평문 offer 488 ──
        t2 = time.time()
        rc2, tail2, delta2, _, _ = group_call("off")
        gate = _grep_logs("SRTP negotiation failed", [log_dirs[1]], t2 - 5)
        # 게이트 로그가 찍히고, 그 통화가 SRTP 종단 없이 굴러가지 않았어야 한다
        ok2 = gate >= 1
        checks.append(("R2 required 평문 offer 488", ok2, f"csp_488_log={gate} rc={rc2}"))

        # ── R3: optional — AVP+a=crypto best-effort 수용 ──
        if not flip("optional"):
            checks.append(("R3 optional 관대 수용", False, "정책 플립(optional) 실패"))
        else:
            rc3, tail3, delta3, sim3, t3 = group_call("optional")
            cmp3 = _grep_logs("member media SRTP audio enabled", log_dirs[:1], t3 - 5)
            ok3 = sim3 and cmp3 >= 1 and delta3 >= 1
            checks.append(("R3 optional 관대 수용", ok3,
                           f"sim_srtp={sim3} cmp_media_crypto={cmp3} 녹취/이벤트 +{delta3} rc={rc3}"))

        # ── R5: VoLTE relay leg 종단 (B2BUA — leg 별 독립 키, media_security.md §5.2) ──
        if not s.get("VOIP_USER"):
            ctx.w("- [INFO] R5 생략 — VOIP_USER 미준비")
        elif not flip("required", "volte"):
            checks.append(("R5 volte relay 종단", False, "정책 플립(volte required) 실패"))
        else:
            t5 = time.time()
            args5 = [
                "-no-db", "-mode", "volte", "-scenario", "call",
                "-count", "2", "-duration", "6", "-ip", ctx.sim_ip,
                "-user", s.get("VOIP_USER", ""),
                "-domain", s.get("VOIP_DOM", VOLTE_DOMAIN),
                *cred_args(s, "VOIP", 2),
                "-srtp", "required",
                # 영상 동반(cims.sh sim 기본 미디어) — 오디오·비디오 m-line 각각 SAVP+a=crypto 로 offer 하고
                #   CSP 가 leg 별로 재작성, CMP 가 m-line 별 독립 키로 종단하는 경로까지 실측한다.
            ]
            seen5, on_line5 = srtp_watch()
            rc5, tail5 = run_cspsim(ctx.repo_root, args5, timeout=120, on_line=on_line5)
            sim5 = seen5["srtp"]
            sim5v = seen5["video"]
            # PRtpRelay setLegCrypto — 양 leg 각 1건 이상, audio/video 각각
            relay5 = _grep_logs("SRTP audio peer[", log_dirs[:1], t5 - 5)
            relay5v = _grep_logs("SRTP video peer[", log_dirs[:1], t5 - 5)
            ok5 = sim5 and sim5v >= 2 and relay5 >= 2 and relay5v >= 2
            checks.append(("R5 volte relay 종단(audio+video)", ok5,
                           f"sim_srtp={sim5} sim_video_srtp={sim5v} relay_leg_crypto audio={relay5} video={relay5v} rc={rc5}"))
    finally:
        # ── 자기복원 + R4: off 대조군 (기존 평문 동작 유지) ──
        restored = flip("") and flip("", "volte")
        rc4, tail4, delta4, sim4, _ = group_call("off")
        ok4 = restored and delta4 >= 1 and not sim4
        checks.append(("R4 off 대조군(원복)", ok4, f"restored={restored} 녹취/이벤트 +{delta4} rc={rc4}"))

    all_ok = all(ok for _, ok, _ in checks)
    for name, ok, detail in checks:
        ctx.w(f"- [{'PASS' if ok else 'FAIL'}] {name} — {detail}")
    return done(ItemStatus.PASS if all_ok else ItemStatus.FAIL,
                "\n".join(f"{'PASS' if ok else 'FAIL'} {name}: {d}" for name, ok, d in checks))
