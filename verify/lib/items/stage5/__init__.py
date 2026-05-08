"""Stage 5 — 로컬 배포 (TB-CSC → Test-agent → mgmt-server → service-server 배포 체인).

7 부모 (RESET, CSC-DEPLOY, CSC-VERIFY, CSC-RUN, MODULES-DEPLOY, MODULES-RUN,
FINALIZE) + 13 자식. 본체는 모두 `_native_steps.step_NN_*` (Python). 옛
`_verify_phase2` (bash, cims.sh) + `_legacy.py` 어댑터는 22 step 모두 native
포팅 후 제거됨.
"""
