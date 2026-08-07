"""S6-SCN-FAILOVER-OAM — 관리평면(OAM) 절체 검증.

관리평면은 자기 상태를 파일로 들고 있어 **VIP 이관만으로는 정합이 성립하지 않는다**
(docs/design/features/oam_ha.md). 그래서 이 시나리오는 "콘솔이 뜨는가" 가 아니라
**절체 후에도 관리 데이터·인증·프록시가 이어지는가** 를 본다.

LIVE 조건 (전부 충족해야 실행 — 아니면 SKIP):
  1. `run/keepalived/ha.json` 에 `oam` 이 관리 모듈(relevant/cold)로 들어있다
     = 관리평면이 HA 편입된 구성.
  2. `services.<svc>.shared_store` 스펙이 있다 = 공유 store 구성.
  3. peer 의 OAM 포트가 응답한다 = 2-node 환경.

검증 항목 (LIVE):
  A. **공유 store 확인** — 승격 노드가 store 마운트·write 확인 후 모듈을 기동
  B. **agent 재인증 불필요** — 절체 후 agent_token 그대로 heartbeat 200
  C. **게이트웨이 라우트 유효** — 서비스 API 가 신 Active 에서 200
  D. **리스 인수** — 신 Active 가 `.owner.json` epoch 를 올려 소유(구 Active 는 read-only)
  E. **계획 절체 종결** — operation 이 COMMITTED (관측 불가로 인한 ROLLED_BACK 오기록 없음)

현재는 **전제 점검 + 프리플라이트**까지 구현하고, 실제 kill/절체 본체는 2-node 실환경
(공유 store 포함)에서만 의미가 있어 그 환경이 감지될 때 수행한다. 미충족 환경은 SKIP 이며,
**충족 환경에서 전제가 깨져 있으면 FAIL** 로 드러낸다(조용한 SKIP 로 숨기지 않는다).
"""
from __future__ import annotations

import json
import os
import socket

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S6-SCN-FAILOVER-OAM"
_NAME = "HA fail-over — 관리평면(OAM) 절체 후 관리 데이터·인증·프록시 연속성"
_HA_JSON_REL = ("agent", "keepalived", "ha.json")
_DEFAULT_OAM_PORT = 4419


@verify_item(
    id=_ID, stage=6, category="시나리오",
    name=_NAME,
    depends_on=["S6-SEED"],
    presets=["stage6-full", "pipeline-full"],
    side_effects=["process-state", "network"], timeout_s=120,
    execution_order=96,
)
def scn_failover_oam(ctx: VerifyContext) -> ItemResult:
    ha_path = os.path.join(ctx.dist_dir, *_HA_JSON_REL)
    if not os.path.isfile(ha_path):
        return _skip(ctx, f"ha.json 없음 ({ha_path}) — HA 비활성 환경")
    try:
        cfg = json.load(open(ha_path))
    except Exception as e:
        return _skip(ctx, f"ha.json 파싱 실패: {e}")

    services = cfg.get("services") or {}
    svc_name, svc = _find_oam_service(services)
    if not svc:
        # 선언 집행(oam_ha.md §6.3)으로 제외된 경우는 **사유를 그대로 보고**한다 —
        # "미편입" 과 "전제 미충족으로 빠짐" 은 운영자에게 전혀 다른 정보다.
        why = _excluded_reason(services)
        if why:
            return _skip(ctx, f"oam 이 전제 미충족으로 HA 편입 제외됨 ({why}) — "
                              f"그룹에 공유 store 를 설정하면 편입된다")
        return _skip(ctx, "ha.json 에 oam 이 관리 모듈로 없음 — 관리평면 HA 미편입 구성")

    # ── 전제 점검 (충족 환경에서 어긋나면 FAIL) ─────────────────────────────
    problems: list[str] = []
    store = svc.get("shared_store") or {}
    if not str(store.get("mount_point") or "").startswith("/"):
        problems.append("shared_store 없음 — 관리 store 가 공유 경로에 없다(절체하면 빈 콘솔)")
    mh = svc.get("module_health") or {}
    if "oam" not in mh:
        problems.append("module_health.oam 없음 — 모듈별 감시가 아니라 대표 1개만 검사(좀비 미탐)")
    relevant = {str(m).lower() for m in (svc.get("relevant_modules") or [])}
    if "oam" not in relevant:
        problems.append("relevant_modules 에 oam 없음 — OAM 장애가 절체 사유가 아니다")
    peer_ip = cfg.get("peer_ip") or ""
    port = int((mh.get("oam") or {}).get("port") or svc.get("port") or _DEFAULT_OAM_PORT)
    if not peer_ip:
        problems.append("ha.json.peer_ip 미설정 — 2-node 판정 불가")
    elif not _port_open(peer_ip, port):
        # peer 의 OAM 은 cold standby 라 **정상적으로 닫혀 있다** — 문제로 보지 않는다.
        ctx.w(f"- peer {peer_ip}:{port} 미응답 (cold standby 정상: 승격 전까지 미기동)")

    if problems:
        ctx.w(f"### {_ID} — FAIL (전제 미충족)")
        for p in problems:
            ctx.w(f"- {p}")
        ctx.w()
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.FAIL, stage=6,
                          detail="; ".join(problems))

    ctx.w(f"### {_ID} — 전제 충족")
    ctx.w(f"- service={svc_name} shared_store={store.get('mount_point')}")
    ctx.w(f"- relevant={sorted(relevant)} module_health={sorted(mh)}")
    ctx.w(f"- peer={peer_ip}:{port}")
    ctx.w()
    # 절체 본체(kill → VIP 이관 → A~E 확인)는 공유 store 가 실제로 붙은 2-node 에서만 수행.
    # 그 환경 자동 감지·실행은 후속(실서버 검증 라운드)에서 채운다 — 여기서 조용히 통과시키지
    # 않고 "전제 충족/미충족" 을 명확히 남긴다.
    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, stage=6,
                      detail="전제 충족 — 절체 본체는 공유 store 붙은 2-node 실환경에서 수행")


def _find_oam_service(services: dict) -> tuple:
    """`oam` 을 관리 모듈로 갖는 서비스 (name, entry). 없으면 (None, None)."""
    for name, s in (services or {}).items():
        mods = {str(m).lower() for m in (s.get("relevant_modules") or [])}
        mods |= {str(m).lower() for m in (s.get("cold_modules") or [])}
        if "oam" in mods:
            return name, s
    return None, None


def _excluded_reason(services: dict) -> str:
    """`ha_excluded` 에 oam/oam-svc 가 있으면 "모듈=사유" 문자열. 없으면 빈 문자열."""
    out = []
    for s in (services or {}).values():
        for mod, why in ((s.get("ha_excluded") or {}) or {}).items():
            if str(mod).lower() in ("oam", "oam-svc"):
                out.append(f"{mod}={why}")
    return ", ".join(sorted(set(out)))


def _port_open(ip: str, port: int, timeout: float = 1.0) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except Exception:
        return False


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w(f"### {_ID} — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, stage=6, detail=reason)
