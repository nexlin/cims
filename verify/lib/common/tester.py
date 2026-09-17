"""계측기(oam-cims-tester) 호출 — 검증 시나리오 항목이 `cims.sh sim` 대신 `cims-tester run <scenario> --json` 을 부른다
(test_instrument.md §9 — S3/S6 시나리오 항목의 단계적 이전).

판정은 계측기 verdict 그대로다: 발생기 측 관측(RFC 6076 기대치·실패 인스턴스 0)이 1차, 시나리오의 `target_evidence`(녹취 등)가 2차.
cspsim 경로가 "대상에 녹취가 하나라도 늘었는가" 만 보던 것보다 좁고 정확하다.

설정은 환경변수 — 없으면 이전하지 않은 것으로 보고 호출자가 기존 cspsim 경로로 간다(`run_tester_scenario` 가 None):
  CIMS_TESTER_URL        계측기가 뒤에 붙은 OAM 게이트웨이(예: https://10.0.2.48:4419) 또는 컨트롤러 직접 주소
  CIMS_TESTER_TOPOLOGY   토폴로지 이름 또는 id
  CIMS_TESTER_TOKEN      로그인 토큰 — 없으면 CIMS_TESTER_LOGIN / CIMS_TESTER_PASSWORD 로 그 OAM 에 로그인한다
설정했는데 계측기에 닿지 못하면 조용히 cspsim 으로 돌아가지 않고 FAIL 이다(게이트가 어느 경로로 판정했는지 흐려지지 않게).
"""
from __future__ import annotations

import json
import os
import ssl
import subprocess
import urllib.request
from typing import Dict, List, Optional

from ..registry import ItemResult, ItemStatus
from ..context import VerifyContext

_CLI = os.path.join('ems', 'tester', 'oam', 'bin', 'cims-tester')


def tester_config() -> Optional[Dict[str, str]]:
    url = (os.environ.get('CIMS_TESTER_URL') or '').strip().rstrip('/')
    topo = (os.environ.get('CIMS_TESTER_TOPOLOGY') or '').strip()
    if not url or not topo:
        return None
    return {'url': url, 'topology': topo}


def _login(url: str) -> str:
    tok = (os.environ.get('CIMS_TESTER_TOKEN') or '').strip()
    if tok:
        return tok
    login = (os.environ.get('CIMS_TESTER_LOGIN') or '').strip()
    pw = os.environ.get('CIMS_TESTER_PASSWORD') or ''
    if not login:
        raise RuntimeError('CIMS_TESTER_TOKEN 또는 CIMS_TESTER_LOGIN/CIMS_TESTER_PASSWORD 가 필요하다')
    req = urllib.request.Request(url + '/api/v1/auth/login', method='POST',
                                 data=json.dumps({'login_id': login, 'password': pw}).encode(),
                                 headers={'Content-Type': 'application/json'})
    ctx = ssl._create_unverified_context() if url.startswith('https') else None
    with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
        d = json.loads(r.read().decode() or '{}')
    tok = d.get('token') or d.get('access_token') or (d.get('data') or {}).get('token')
    if not tok:
        raise RuntimeError('로그인 응답에 토큰이 없다')
    return str(tok)


def summarize(rec: dict) -> List[str]:
    """run 기록 → 보고서 줄(요약 + 어긋난 기대치·증거 + 참고)."""
    s = rec.get('summary') or {}
    out = [f"run {rec.get('id')} · {rec.get('scenario_id')} · verdict={rec.get('verdict')} · target={rec.get('target_build') or '-'}",
           '시도 {attempts} · 세션 {sessions} · 완료 {completed} · 실패 {failed} · 건너뜀 {skipped}'.format(
               **{k: s.get(k, 0) for k in ('attempts', 'sessions', 'completed', 'failed', 'skipped')})
           + f" · SER {s.get('ser_pct')} · RTP 손실 {s.get('rtp_loss_pct')} · SRD p95 {s.get('srd_ms_p95')}"]
    for e in rec.get('expect_results') or []:
        if not e.get('ok'):
            out.append(f"기대치 FAIL — #{e.get('step')} {e.get('kind')} {e.get('metric')}: 기대 {e.get('expect')} / 관측 {e.get('observed')}")
    for e in rec.get('evidence_results') or []:
        out.append(f"대상 증거 {e.get('kind')}: 관측 {e.get('observed')} → {'OK' if e.get('ok') else '판정 불가' if e.get('ok') is None else 'FAIL'}")
    for n in (rec.get('notes') or [])[:6]:
        out.append(f'참고 — {n}')
    return out


def run_tester_scenario(ctx: VerifyContext, item_id: str, title: str, scenario_id: str, stage: int,
                        instances: int = 1, ht: Optional[int] = None, binds: Optional[Dict[str, object]] = None,
                        timeout: int = 240) -> Optional[ItemResult]:
    """계측기로 시나리오 단발 실행 → ItemResult. 계측기 설정(환경변수)이 없으면 None — 호출자가 cspsim 경로로 간다."""
    cfg = tester_config()
    if cfg is None:
        return None
    ctx.w(f'### {item_id} — {title}')
    ctx.w(f"- 계측기 `{scenario_id}` @ {cfg['url']} · 토폴로지 {cfg['topology']} · 인스턴스 {instances}")
    try:
        token = _login(cfg['url'])
        cmd = ['python3', os.path.join(ctx.repo_root, _CLI), '--url', cfg['url'], '--token', token, '--json',
               'run', scenario_id, '--topology', cfg['topology'], '--instances', str(instances), '--timeout', str(timeout),
               '--label', item_id]
        if ht is not None:
            cmd += ['--ht', str(ht)]
        for k, v in (binds or {}).items():
            cmd += ['--bind', f'{k}={v}']
        proc = subprocess.run(cmd, cwd=ctx.repo_root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout + 30, text=True)
        rec = json.loads(proc.stdout) if proc.stdout.strip().startswith('{') else None
    except Exception as e:      # 설정했는데 못 닿음 — cspsim 으로 조용히 돌아가지 않는다
        ctx.w(f'- [FAIL] 계측기 호출 실패: {e}')
        ctx.w()
        return ItemResult(id=item_id, name=title, status=ItemStatus.FAIL, detail=f'계측기 호출 실패: {e}', stage=stage)
    if rec is None or 'verdict' not in rec:
        tail = (proc.stdout or proc.stderr or '').strip()[-600:]
        ctx.w(f'- [FAIL] 계측기가 run 기록을 돌려주지 않았다(rc={proc.returncode})')
        ctx.w('```')
        ctx.w(tail)
        ctx.w('```')
        ctx.w()
        return ItemResult(id=item_id, name=title, status=ItemStatus.FAIL, detail=f'계측기 응답 이상(rc={proc.returncode})\n{tail}', stage=stage)
    lines = summarize(rec)
    ok = rec.get('verdict') == 'pass'
    for ln in lines:
        ctx.w(f'- {ln}')
    ctx.w(f"- {'[PASS]' if ok else '[FAIL]'} 계측기 verdict={rec.get('verdict')} — 결과 화면 `/test/results?id={rec.get('id')}`")
    ctx.w()
    return ItemResult(id=item_id, name=title, status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail='\n'.join(lines), stage=stage)
