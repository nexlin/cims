"""run 실행 엔진 — phase/step 오케스트레이션 + 체크포인트 (auto_deployment.md §4).

phase 는 순서대로, phase 안의 step 은 병렬(직렬 지정 phase 는 예외)로 돈다. **step 하나가
끝날 때마다 run 레코드를 디스크에 기록**하므로, 프로세스가 죽거나 OAM 이 재기동돼도
`resume` 이 마지막 성공 지점부터 이어간다.

phase 모듈 계약:
    KEY      : str            — 'AGENT' 등
    TITLE    : str
    SERIAL   : bool           — True 면 step 을 순차 실행 (기동 순서가 중요한 phase)
    plan(ctx)          -> [ {'target': str, ...} ]     실행 전 계획 (dry-run 이 이것만 쓴다)
    execute(ctx, step) -> {'status': 'done'|'skipped', 'detail': str}
        예외를 던지면 엔진이 'failed' 로 기록한다.

step 은 **멱등**이어야 한다. resume 은 done/skipped 를 건너뛰지만, 같은 step 이 두 번
실행되는 경우(중단 직전 완료했으나 기록 전 사망)를 phase 가 스스로 견뎌야 한다.
"""

from __future__ import annotations

import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime


def _now() -> str:
    return datetime.now().isoformat(timespec='seconds')


class Aborted(Exception):
    """운영자 중단 요청."""


class Context:
    """phase 가 받는 실행 컨텍스트."""

    def __init__(self, *, blueprint, inventory, oam, config: dict, run: dict, log):
        self.blueprint = blueprint
        self.inventory = inventory
        self.oam = oam
        self.config = config
        self.run = run
        self.log = log

    # 자주 쓰는 설정 접근자 — phase 코드에서 dict 파기를 반복하지 않게
    @property
    def run_cfg(self) -> dict:
        return self.config.get('Run') or {}

    @property
    def ssh_cfg(self) -> dict:
        return self.config.get('Ssh') or {}

    @property
    def max_parallel(self) -> int:
        return int(self.run_cfg.get('MaxParallel', 8) or 8)

    def record_created(self, kind: str, ident, label: str = ''):
        """이 run 이 **새로 만든** 엔티티를 기록한다 — 롤백은 이 목록만 되돌린다.

        run 이전부터 있던 그룹·배포를 재사용한 경우는 기록하지 않으므로, 롤백이 남의
        구성을 지우지 않는다(§4 '롤백').
        """
        created = self.run.setdefault('created', [])
        entry = {'kind': kind, 'id': ident, 'label': label}
        if entry not in created:
            created.append(entry)

    def ssh_kwargs(self, runtime_dir: str | None = None) -> dict:
        import os
        kw = {
            'connect_timeout': int(self.ssh_cfg.get('ConnectTimeout', 15) or 15),
            'command_timeout': int(self.ssh_cfg.get('CommandTimeout', 900) or 900),
            'strict_host_key': self.ssh_cfg.get('StrictHostKeyChecking') or 'accept-new',
        }
        if runtime_dir:
            kw['known_hosts'] = os.path.join(runtime_dir, 'known_hosts')
        return kw


class Engine:
    def __init__(self, store, ctx: Context, phases: list, *, on_error: str = 'stop'):
        self.store = store
        self.ctx = ctx
        self.phases = phases                 # phase 모듈 리스트 (실행 순서)
        self.on_error = on_error             # 'stop' (fail-fast) | 'continue'
        self._abort = False

    def abort(self):
        self._abort = True

    # ── 계획 ────────────────────────────────────────────────────
    def plan(self) -> list:
        """dry-run — 아무것도 바꾸지 않고 phase×step 계획만 만든다."""
        out = []
        for mod in self.phases:
            try:
                steps = mod.plan(self.ctx)
            except Exception as e:
                out.append({'key': mod.KEY, 'title': mod.TITLE, 'error': str(e), 'steps': []})
                continue
            out.append({'key': mod.KEY, 'title': mod.TITLE,
                        'serial': getattr(mod, 'SERIAL', False), 'steps': steps})
        return out

    # ── 실행 ────────────────────────────────────────────────────
    def execute(self) -> dict:
        run = self.ctx.run
        run['status'] = 'running'
        run.setdefault('started_at', _now())
        run['updated_at'] = _now()
        self._save()

        try:
            for mod in self.phases:
                if self._abort:
                    raise Aborted()
                self._run_phase(mod)
                ph = self._phase_rec(mod.KEY)
                if ph['status'] == 'failed' and self.on_error == 'stop':
                    run['status'] = 'failed'
                    run['error'] = f"{mod.KEY} phase 실패 — 이후 단계를 진행하지 않음"
                    break
            else:
                run['status'] = 'succeeded'
        except Aborted:
            run['status'] = 'aborted'
            run['error'] = '운영자 중단'
        except Exception as e:                                  # 엔진 자체 오류
            run['status'] = 'failed'
            run['error'] = f'엔진 오류: {e}'
            run['traceback'] = traceback.format_exc()

        run['finished_at'] = _now()
        run['updated_at'] = _now()
        self._save()
        return run

    # ── 내부 ────────────────────────────────────────────────────
    def _save(self):
        self.ctx.run['updated_at'] = _now()
        self.store.save_run(self.ctx.run)

    def _phase_rec(self, key: str) -> dict:
        for ph in self.ctx.run.setdefault('phases', []):
            if ph['key'] == key:
                return ph
        ph = {'key': key, 'title': '', 'status': 'pending', 'steps': []}
        self.ctx.run['phases'].append(ph)
        return ph

    @staticmethod
    def _step_rec(ph: dict, target: str) -> dict | None:
        for st in ph['steps']:
            if st['target'] == target:
                return st
        return None

    def _run_phase(self, mod):
        ph = self._phase_rec(mod.KEY)
        ph['title'] = mod.TITLE
        ph['status'] = 'running'
        ph['started_at'] = ph.get('started_at') or _now()
        self._save()

        try:
            planned = mod.plan(self.ctx)
        except Exception as e:
            ph['status'] = 'failed'
            ph['error'] = f'계획 수립 실패: {e}'
            self._save()
            return

        # 기존 기록과 병합 — resume 시 done/skipped 는 재실행하지 않는다.
        todo = []
        for spec in planned:
            existing = self._step_rec(ph, spec['target'])
            if existing and existing.get('status') in ('done', 'skipped'):
                continue
            if not existing:
                existing = {'target': spec['target'], 'status': 'pending', 'detail': ''}
                ph['steps'].append(existing)
            existing['status'] = 'pending'
            existing['error'] = None
            todo.append((spec, existing))
        self._save()

        if todo:
            if getattr(mod, 'SERIAL', False):
                for spec, rec in todo:
                    if self._abort:
                        break
                    self._exec_step(mod, spec, rec)
                    if rec['status'] == 'failed' and self.on_error == 'stop':
                        break
            else:
                workers = max(1, min(self.ctx.max_parallel, len(todo)))
                with ThreadPoolExecutor(max_workers=workers) as ex:
                    list(ex.map(lambda t: self._exec_step(mod, t[0], t[1]), todo))

        states = [s.get('status') for s in ph['steps']]
        if 'failed' in states:
            ph['status'] = 'failed'
        elif all(s in ('done', 'skipped') for s in states) and states:
            ph['status'] = 'done'
        else:
            ph['status'] = 'aborted' if self._abort else 'partial'
        ph['finished_at'] = _now()
        self._save()

    def _exec_step(self, mod, spec: dict, rec: dict):
        if self._abort:
            rec['status'] = 'aborted'
            return
        rec['status'] = 'running'
        rec['started_at'] = _now()
        self._save()
        t0 = time.time()
        try:
            out = mod.execute(self.ctx, spec) or {}
            rec['status'] = out.get('status') or 'done'
            rec['detail'] = out.get('detail') or ''
            for k in ('agent_id', 'deployment_id', 'job_id', 'version'):
                if k in out:
                    rec[k] = out[k]
        except Exception as e:
            rec['status'] = 'failed'
            rec['detail'] = ''
            rec['error'] = str(e)
            rec['error_code'] = getattr(e, 'code', None) or type(e).__name__
            self.ctx.log(f"[{mod.KEY}] {spec['target']} 실패: {e}")
        finally:
            rec['elapsed_sec'] = round(time.time() - t0, 1)
            rec['finished_at'] = _now()
            self._save()


def new_run(store, *, blueprint_id, inventory_id, blueprint_name: str,
            actor: str = '', on_error: str = 'stop') -> dict:
    return {
        'id': store.new_run_id(),
        'status': 'pending',
        'blueprint_id': blueprint_id,
        'inventory_id': inventory_id,
        'blueprint': blueprint_name,
        'actor': actor,
        'on_error': on_error,
        'created_at': _now(),
        'phases': [],
        'log': [],
    }
