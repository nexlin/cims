# OAM — Operation & Management

CIMS O&M 모듈. Agent/HA/배포/검증/통계/알림 책임.

설계: [docs/design/oam_csc_split.md](../docs/design/oam_csc_split.md)

## Phase 별 진행 상태

- **Phase 1 (코드 구조 분리)** — 진행 중. top-level `oam/` 디렉토리 신설, handler 12개 (`agents/agent_api/ha_groups/modules/build/service_control/verification/alerts/stats/recording/auth/users`) 가 `csc/src/handlers/` 에서 이동. **같은 binary 유지** (`cims-csc` 프로세스가 oam 코드도 import). PEP 420 namespace package 로 `handlers.*` 임포트 호환.
- **Phase 2 (패키지 분리)** — 미진행. `oam/pkg.json` 신설 + `cims.sh pkg oam` 으로 별도 tarball.
- **Phase 3 (프로세스 분리)** — 미진행. `cims-oam` systemd unit + 4419 포트.
- **Phase 4 (호스트 분리)** — 미진행. 운영망/서비스망 분리.

## 디렉토리

```
oam/
├─ src/
│  ├─ handlers/   # OAM 책임 REST handler (agents, ha_groups, build, ...)
│  └─ services/   # OAM 전용 service (Phase 2 이후 file_store 이관)
├─ docs/          # (Phase 2 이후)
└─ README.md
```

Phase 1 동안 OAM handlers 는 csc/src/services/ 의 공유 service (file_store, flow_logger, logger 등) 와 csc/src/util/ 을 그대로 import 한다. csc/src 와 oam/src 가 모두 sys.path 에 있어 `from services.X` / `from handlers.X` / `from util.X` 가 양쪽에서 해석된다.
