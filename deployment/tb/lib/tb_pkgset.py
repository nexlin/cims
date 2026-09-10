#!/usr/bin/env python3
"""tb_pkgset.py — pkg_setting.cfg 읽기 (45 단계 전용)

설정값을 스크립트에서 떼어내 파일 하나로 모으기 위한 도구다. 현장에서 값을 바꿀 때
스크립트를 고치게 하면 오타 한 번이 설치를 깨뜨리고, 무엇을 바꿨는지도 남지 않는다.

표준 라이브러리만 쓴다 (폐쇄망 — 반입본에 pip 의존을 늘리지 않는다).

줄 형식은 pkg_setting.cfg 머리말이 정본이다:
    <모듈> <키>=<값>              배포 overlay
    <모듈>/<컬렉션> <JSON 객체>   컬렉션 레코드 (한 줄 = 한 행, 적은 순서 유지)
    [vars]                        파일 안에서만 쓰는 값
    # …                           주석

치환은 ${이름} 하나뿐이고, **정의가 없거나 빈 값이면 중단한다** — 조용히 빈 문자열이
들어가면 등록은 되고 동작만 틀려서 원인을 찾는 데 훨씬 비싸다.
"""
import argparse
import json
import os
import re
import sys

VAR_RE = re.compile(r'\$\{([A-Za-z_][A-Za-z0-9_]*)\}')
SECRET_HINT = ('pass', 'secret', 'token')


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def load_vars(path):
    """NAME=VALUE 줄 목록 → dict. 45 단계가 사이트 값·파생값을 여기에 써서 넘긴다."""
    out = {}
    if not path:
        return out
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#') or '=' not in line:
                    continue
                k, v = line.split('=', 1)
                out[k.strip()] = v
    except OSError as e:
        die(f"변수 파일을 읽을 수 없습니다: {path} ({e})")
    return out


def subst(text, vars_, where):
    """${이름} 치환. **미정의면 중단**하고, 정의된 빈 값은 그대로 빈 문자열로 둔다.

    둘을 구분하는 이유: realm 처럼 "비우는 것이 유효한 설정"이 있다(비우면 CSP 가
    domain 을 realm 으로 쓴다). `PTT_REALM =` 처럼 [vars] 에 적어둔 빈 값은 의도이고,
    이름 자체가 없는 것은 오타이거나 site.conf 누락이다 — 후자만 막는다.
    """
    missing = []

    def one(m):
        name = m.group(1)
        if name not in vars_:
            missing.append(name)
            return ''
        return vars_[name]

    out = VAR_RE.sub(one, text)
    if missing:
        die(f"{where}: 값이 없는 변수 {sorted(set(missing))} — "
            f"tb-site.conf 에 넣거나 pkg_setting.cfg 의 [vars] 에 정의하세요")
    return out


def parse(path, vars_):
    """→ (overlay: {모듈: [(키, 값)]}, coll: {모듈/이름: [레코드]}, 모듈순서, 컬렉션순서)"""
    overlay, coll = {}, {}
    mod_order, coll_order = [], []
    local = dict(vars_)
    section = None
    try:
        lines = open(path, encoding='utf-8').read().splitlines()
    except OSError as e:
        die(f"설정 파일을 읽을 수 없습니다: {path} ({e})")

    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        where = f"{os.path.basename(path)}:{n}"

        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].strip().lower()
            if section != 'vars':
                die(f"{where}: 알 수 없는 절 [{section}] — [vars] 만 있습니다")
            continue

        if section == 'vars' and '=' in line and ' ' not in line.split('=', 1)[0].strip():
            k, v = line.split('=', 1)
            # 값이 비어도 등록한다 — "정의된 빈 값" 은 유효한 설정이다 (subst 주석 참조).
            local[k.strip()] = subst(v.strip(), local, where)
            continue

        parts = line.split(None, 1)
        if len(parts) != 2:
            die(f"{where}: '<모듈> <키>=<값>' 또는 '<모듈>/<컬렉션> <JSON>' 형식이어야 합니다 — {raw!r}")
        target, rest = parts[0], parts[1].strip()

        if '/' in target:                                  # 컬렉션 레코드
            rec_txt = subst(rest, local, where)
            try:
                rec = json.loads(rec_txt)
            except json.JSONDecodeError as e:
                die(f"{where}: JSON 이 아닙니다 ({e}) — 치환 결과: {rec_txt}")
            if not isinstance(rec, dict):
                die(f"{where}: 컬렉션 레코드는 JSON 객체여야 합니다")
            # 미사용 transport 처리 — bind_port 0 인 행은 넣지 않는다.
            if target.endswith('/local_nodes') and not rec.get('bind_port'):
                continue
            if target not in coll:
                coll[target] = []
                coll_order.append(target)
            coll[target].append(rec)
        else:                                              # overlay 키
            if '=' not in rest:
                die(f"{where}: overlay 는 '<키>=<값>' 이어야 합니다 — {raw!r}")
            k, v = rest.split('=', 1)
            k = k.strip()
            if not k:
                die(f"{where}: 키가 비었습니다")
            if target not in overlay:
                overlay[target] = []
                mod_order.append(target)
            overlay[target].append((k, subst(v, local, where)))

    return overlay, coll, mod_order, coll_order, local


def mask(key, val):
    return '********' if any(s in key.lower() for s in SECRET_HINT) else val


def main():
    ap = argparse.ArgumentParser(description='pkg_setting.cfg 읽기')
    ap.add_argument('--file', required=True)
    ap.add_argument('--vars', default='')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('modules')                                    # overlay 가 있는 모듈
    sub.add_parser('collections')                                # <모듈>/<이름>
    p = sub.add_parser('overlay'); p.add_argument('module')      # KEY=VALUE 줄
    p = sub.add_parser('collection'); p.add_argument('target')   # JSON 배열
    p = sub.add_parser('var'); p.add_argument('name')            # [vars] 해석값 하나
    sub.add_parser('check')                                      # 전체 검증 + 요약
    a = ap.parse_args()

    vars_ = load_vars(a.vars)
    overlay, coll, mod_order, coll_order, resolved = parse(a.file, vars_)

    if a.cmd == 'modules':
        print('\n'.join(mod_order))
    elif a.cmd == 'collections':
        print('\n'.join(coll_order))
    elif a.cmd == 'overlay':
        for k, v in overlay.get(a.module, []):
            print(f"{k}={v}")
    elif a.cmd == 'collection':
        if a.target not in coll:
            die(f"컬렉션이 없습니다: {a.target}")
        print(json.dumps(coll[a.target], ensure_ascii=False, indent=1))
    elif a.cmd == 'var':
        v = resolved.get(a.name)
        if v is None:
            die(f"[vars] 에 없습니다: {a.name}")
        print(v)
    elif a.cmd == 'check':
        for t in coll_order:
            print(f"  {t:28} {len(coll[t])}행")
        for m in mod_order:
            print(f"  {m:28} {len(overlay[m])}키")
        print(f"  합계: 컬렉션 {sum(len(v) for v in coll.values())}행 / "
              f"overlay {sum(len(v) for v in overlay.values())}키")
    return 0


if __name__ == '__main__':
    sys.exit(main())
