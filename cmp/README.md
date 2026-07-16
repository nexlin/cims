# CMP (Component Media Provider)

RTP relay + MCPTT floor control 미디어 서버. UDP JSON 제어(envelope v2)로 서비스 AS
(현재 CSP)가 사용한다.

- **제어 API 규격 (정본)**: [../docs/api/cmp_media_api.md](../docs/api/cmp_media_api.md)
- **모듈 상세 설계**: [../docs/design/modules/cmp.md](../docs/design/modules/cmp.md)
- **빌드/실행**: 레포 루트 [CLAUDE.md](../CLAUDE.md) 및
  [../docs/DEV_SERVER_SETUP.md](../docs/DEV_SERVER_SETUP.md) — `make cmp` 후
  `./bin/cmp ../cmp/cmp.json`
- **검증**: `cims-verify` S3~S6 게이트, `verify_rtp_bridge.py`(relay 브릿지 스모크),
  `scripts/mcptt_floor_preempt_check.py`(floor 선점 라이브 점검)
