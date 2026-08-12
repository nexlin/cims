"""설정 리로드 — **기동 중 코드가 정한 값은 파일 리로드가 되돌리지 못한다.**

agent 의 `update_config` job 은 설정 파일을 쓴 뒤 모듈에 SIGUSR1 을 보낸다(재기동이
아니라 "다시 읽어라"). 모듈은 그 신호에 `config` 를 갱신하는데, 종전 구현은

    config.clear(); config.update(load_config())

였다. 이 방식은 **파일에 없는 값을 전부 지운다.** 모듈이 들고 있는 설정에는 두 종류가
섞여 있기 때문이다:

  1. 파일에서 읽은 값            — 리로드로 갱신되는 것이 맞다
  2. 기동 중 코드가 정한 값       — 파일에 없으므로 **되살아나지 않는다**

2번이 지워지면서 난 실측 사고:

  - `_ConsoleStaticDir`(콘솔 dist 실제 경로) 소실 → 설정을 저장하는 순간 콘솔이
    `console_not_bundled` 로 죽었다. 파일은 멀쩡히 그 자리에 있는데도.
  - `CimsRuntimeDir` 강등 취소 → 공유 마운트가 없어 **로컬 store 로 강등**해 뜬 노드가,
    설정 저장 한 번에 파일값(마운트 경로)으로 되돌아간다. 없는 경로를 관리 store 로
    쓰게 되므로 mount guard 가 막으려던 바로 그 상태(로컬 디스크에 두 번째 store)가 된다.

그래서 리로드 규칙을 이렇게 정한다:

  **파일 값으로 갈아끼우되, 기동 중 정해진 값(런타임 override)과 내부 파생 키(`_` 접두)는
  보존한다. 그 값들을 진짜로 바꾸려면 재기동해야 한다.**

경로·마운트처럼 기동 시 자원을 잡아 결정되는 값은 애초에 무중단 반영 대상이 아니다.
"""
from __future__ import annotations

_RT_KEY = '_runtime_overrides'      # {'set': {k: v}, 'unset': [k, ...]}


def _slot(config: dict) -> dict:
    rt = config.get(_RT_KEY)
    if not isinstance(rt, dict):
        rt = {'set': {}, 'unset': []}
        config[_RT_KEY] = rt
    rt.setdefault('set', {})
    rt.setdefault('unset', [])
    return rt


def runtime_set(config: dict, key: str, value) -> None:
    """기동 중 코드가 정한 값 — 리로드가 파일값으로 되돌리면 안 되는 것."""
    rt = _slot(config)
    rt['set'][key] = value
    if key in rt['unset']:
        rt['unset'].remove(key)
    config[key] = value


def runtime_unset(config: dict, key: str) -> None:
    """기동 중 코드가 **지운** 키 — 리로드가 파일에서 되살리면 안 되는 것."""
    rt = _slot(config)
    rt['set'].pop(key, None)
    if key not in rt['unset']:
        rt['unset'].append(key)
    config.pop(key, None)


def apply_reload(config: dict, newc: dict) -> int:
    """파일 값(newc)으로 교체하되 런타임 override·내부 파생 키를 보존한다.

    반환: 보존한 항목 수 (로그용). `config` 는 **in-place** 로 갱신한다 — sweeper 등
    이 dict 를 참조로 들고 있으므로 새 객체로 바꾸면 전파되지 않는다."""
    rt = _slot(config)
    keep_set = dict(rt.get('set') or {})
    keep_unset = list(rt.get('unset') or [])
    derived = {k: v for k, v in config.items() if k.startswith('_') and k != _RT_KEY}

    config.clear()
    config.update(newc or {})
    config.update(derived)          # 내부 파생 상태(_mgmt_net 등)
    config.update(keep_set)         # 기동 중 정한 값
    for k in keep_unset:
        config.pop(k, None)         # 기동 중 지운 키는 되살리지 않는다
    config[_RT_KEY] = {'set': keep_set, 'unset': keep_unset}
    return len(keep_set) + len(keep_unset) + len(derived)
