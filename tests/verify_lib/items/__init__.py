"""auto-import — 모든 phase 의 항목 모듈을 import 하여 registry 에 등록 트리거."""

# Step 1 — Phase 3
from . import phase3                                            # noqa: F401

# Step 2 — Phase 1
from . import phase1                                            # noqa: F401

# Step 3 — Phase 2 (1차: 단일 P2-RUN-ALL 항목, 후속에서 22단계 분해)
from . import phase2                                            # noqa: F401

# Step 4 — run_all.py 9개 모듈 → MODULE-* 항목 (Phase 1 의 디버깅 드릴다운)
from . import modules                                           # noqa: F401
