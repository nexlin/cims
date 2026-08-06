"""배포 phase 모듈 (auto_deployment.md §4).

실행 순서: AGENT → TOPOLOGY → INSTALL → CONFIG → START → VERIFY.
AGENT 만 SSH 를 쓰고 나머지는 OAM REST 로 동작한다.

각 모듈은 engine.py 상단의 phase 계약(KEY/TITLE/SERIAL/plan/execute)을 따른다.
phase 간에는 배리어가 있다 — CONFIG 의 collection PUT 은 INSTALL 이 끝나야 받아지고
(미설치 배포는 409), START 는 설정이 반영된 뒤여야 한다.
"""

from . import agent, topology, install, config, start, verify

ORDER = [agent, topology, install, config, start, verify]

BY_KEY = {m.KEY: m for m in ORDER}

__all__ = ['agent', 'topology', 'install', 'config', 'start', 'verify', 'ORDER', 'BY_KEY']
