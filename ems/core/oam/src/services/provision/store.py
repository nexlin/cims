"""영속 스토어 — 블루프린트 / 인벤토리 / run 기록 (auto_deployment.md §4).

레이아웃 (CimsRuntimeDir 기본 = <컴포넌트루트>/runtime):

    runtime/
    ├── blueprints/<id>.json        구조 + 원문(raw) 동봉 — 원문은 다운로드용으로 보관
    ├── _secrets/
    │   └── inventories/<id>.json   0600. 비밀 포함 — API 응답은 항상 마스킹본만
    └── runs/<id>.json              run 체크포인트

**버전 디렉토리 밖**(모듈 루트 직하)에 두어야 업그레이드·롤백에 생존한다
(02_deployment.md §2 의 durability 제약과 동일 이유). 배포 config 의 CimsRuntimeDir 는
절대경로로 지정한다.

원자적 쓰기: 임시파일 → fsync → rename. run 진행 중 프로세스가 죽어도 체크포인트가
반쯤 쓰인 상태로 남지 않는다.
"""

from __future__ import annotations

import json
import os
import stat
import tempfile


class Store:
    def __init__(self, runtime_dir: str):
        self.root = os.path.abspath(runtime_dir)
        self.blueprints = os.path.join(self.root, 'blueprints')
        self.secrets = os.path.join(self.root, '_secrets')
        self.inventories = os.path.join(self.secrets, 'inventories')
        self.runs = os.path.join(self.root, 'runs')
        for d in (self.root, self.blueprints, self.runs):
            os.makedirs(d, exist_ok=True)
        os.makedirs(self.secrets, exist_ok=True)
        os.chmod(self.secrets, stat.S_IRWXU)              # 0700
        os.makedirs(self.inventories, exist_ok=True)
        os.chmod(self.inventories, stat.S_IRWXU)

    # ── 원자적 파일 IO ──────────────────────────────────────────
    @staticmethod
    def _write(path: str, obj, *, mode: int = 0o644):
        d = os.path.dirname(path)
        fd, tmp = tempfile.mkstemp(dir=d, prefix='.tmp-')
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                json.dump(obj, f, ensure_ascii=False, indent=2)
                f.flush()
                os.fsync(f.fileno())
            os.chmod(tmp, mode)
            os.replace(tmp, path)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise

    @staticmethod
    def _read(path: str):
        try:
            with open(path, encoding='utf-8') as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return None

    @staticmethod
    def _next_id(d: str) -> int:
        mx = 0
        for fn in os.listdir(d):
            if fn.endswith('.json') and not fn.startswith('.'):
                try:
                    mx = max(mx, int(fn[:-5]))
                except ValueError:
                    pass
        return mx + 1

    def _list(self, d: str) -> list:
        out = []
        for fn in sorted(os.listdir(d)):
            if fn.endswith('.json') and not fn.startswith('.'):
                o = self._read(os.path.join(d, fn))
                if o:
                    out.append(o)
        return out

    # ── blueprints ──────────────────────────────────────────────
    def save_blueprint(self, doc: dict, raw: str, *, bid: int | None = None,
                       name: str = '', description: str = '') -> dict:
        if bid is None:
            bid = self._next_id(self.blueprints)
        rec = {'id': bid, 'name': name or doc.get('name') or f'blueprint-{bid}',
               'description': description or doc.get('description') or '',
               'doc': doc, 'raw': raw}
        self._write(os.path.join(self.blueprints, f'{bid}.json'), rec)
        return rec

    def get_blueprint(self, bid: int):
        return self._read(os.path.join(self.blueprints, f'{bid}.json'))

    def list_blueprints(self) -> list:
        # 목록에는 원문/구조를 싣지 않는다 — 콘솔 목록이 무거워진다.
        return [{k: v for k, v in b.items() if k not in ('doc', 'raw')}
                for b in self._list(self.blueprints)]

    def delete_blueprint(self, bid: int) -> bool:
        p = os.path.join(self.blueprints, f'{bid}.json')
        if os.path.exists(p):
            os.unlink(p)
            return True
        return False

    # ── inventories (비밀 포함) ─────────────────────────────────
    def save_inventory(self, doc: dict, raw: str, *, iid: int | None = None,
                       name: str = '') -> dict:
        """원문까지 0600 으로 보관한다 — 인벤토리 원문에는 비밀번호가 들어 있으므로
        blueprint 와 달리 `/raw` 다운로드 경로를 제공하지 않는다(§8)."""
        if iid is None:
            iid = self._next_id(self.inventories)
        rec = {'id': iid, 'name': name or f'inventory-{iid}', 'doc': doc, 'raw': raw}
        self._write(os.path.join(self.inventories, f'{iid}.json'), rec, mode=0o600)
        return rec

    def get_inventory(self, iid: int):
        return self._read(os.path.join(self.inventories, f'{iid}.json'))

    def list_inventories(self) -> list:
        return [{'id': i['id'], 'name': i.get('name'),
                 'server_count': len((i.get('doc') or {}).get('servers') or [])}
                for i in self._list(self.inventories)]

    def delete_inventory(self, iid: int) -> bool:
        p = os.path.join(self.inventories, f'{iid}.json')
        if os.path.exists(p):
            os.unlink(p)
            return True
        return False

    # ── runs ────────────────────────────────────────────────────
    def new_run_id(self) -> int:
        return self._next_id(self.runs)

    def save_run(self, run: dict):
        self._write(os.path.join(self.runs, f"{run['id']}.json"), run)

    def get_run(self, rid: int):
        return self._read(os.path.join(self.runs, f'{rid}.json'))

    def list_runs(self, limit: int = 50) -> list:
        runs = self._list(self.runs)
        runs.sort(key=lambda r: r.get('id', 0), reverse=True)
        # 목록에서는 step 로그를 뺀다 — run 하나가 수백 step 일 수 있다.
        out = []
        for r in runs[:limit]:
            out.append({k: v for k, v in r.items() if k != 'phases'} |
                       {'progress': _progress(r)})
        return out


def _progress(run: dict) -> dict:
    total = done = failed = 0
    for ph in run.get('phases') or []:
        for st in ph.get('steps') or []:
            total += 1
            if st.get('status') in ('done', 'skipped'):
                done += 1
            elif st.get('status') == 'failed':
                failed += 1
    return {'total': total, 'done': done, 'failed': failed}
