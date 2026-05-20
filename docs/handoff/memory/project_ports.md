---
name: project_ports
description: CIMS server port assignments (2026-04-24 d90a08c 블록 A 완료 반영). 4421 은 TCP/UDP 공용 — 서로 다른 서비스에 할당되어 있으므로 혼동 주의. 8080 = Test-Console 단독, 8443 = Test-CWRTC.
type: project
originSessionId: b9397035-535c-4b7a-985c-20e0ae5efe36
---
| Port | Proto | Service | Notes |
|------|-------|---------|-------|
| 3000 | TCP | **TB-Console** (Vite dev) | 상시 기동. TB-CSC UI. |
| 3001 | TCP | **Dev-Console** (Phase 1, 소스 vite dev) | `cims-console/` 트리에서 기동. Test-CSC 4421 로 `/api` proxy. |
| 3002 | TCP | cims-phone (Vite dev) | MCPTT soft-phone UE UI (2026-04-24 3000→3002) |
| 4419 | TCP | **TB-CSC** | 상시 기동. 검증용 패키지/에이전트/배포 관리. |
| 4420 | TCP | **배포 대상 CSC** (Phase 2 csc-server/csc/) | 운영 포트. Phase 2 tarball 배포본. |
| 4421 | TCP | **Test-CSC** (Phase 1, build/dist/csc/) | 구 4420. 기능 검증용 직접 기동본. |
| 4421 | UDP | **CSP CscInterface** | `csp/CspServer.cpp:259`. 사용자/그룹 change NOTIFY 수신. **같은 번호지만 proto 다름 — TCP/UDP 가 서로 간섭 없음.** |
| 4430 | TCP | Test-CSC MCPTT Server | IdMS/GMS/CMS/KMS. 배포 console(4431 I4) 와 별개. |
| 4431 | TCP | TB-CSC MCPTT Server (부수 리슨, I4) | overlay 로 비활성화 요망. |
| 4445 | TCP | Phase 2 csc start job overlay | verify phase2 v2 가 Server.Port=4445 로 overlay 하여 기동 (Phase 1 Test-CSC 4421/ 배포본 4420 과 모두 분리). |
| 5060 | UDP | CSP SIP | 본 SIP signaling. |
| 5061 | TCP/TLS | CSP SIP TLS | 보안 SIP. |
| 8080 | TCP/HTTPS | **Test-Console** (dist HTTPS serve) | Phase 1 dist 정적 서빙 단독. cwrtc 가 8443 으로 이전(d90a08c)되어 단독 사용 가능. |
| 8443 | TCP/WSS | **Test-CWRTC** (이전 8080) | WSS WebRTC 게이트웨이. d90a08c (2026-04-24 블록 A) 에서 8080 → 8443 이전 완료. |
| 9000 | UDP | CMP control listen | CSP → CMP JSON commands. |
| 9001 | UDP | CSP → CMP client (local) | CSP 측 send socket. |
| 9902 | TCP | **TB-agent sync** | TB-CSC ↔ TB-agent. 상시. |
| 9903 | TCP | Test-agent (verify phase2) sync | `csc-server-local` Test-agent 가 사용. |

## Console 3분화 (2026-04-24 e0c44a7)
같은 `cims-console/` 코드베이스가 기동 모드로 3개 인스턴스 형태:
- **Dev-Console** (3001) — `SRC_CONSOLE` 존재 시 vite dev. 개발 + Phase 1 기능 검증 UI.
- **Test-Console** (8080) — dist 트리만 있을 때 `serve dist` (HTTPS). Phase 1 dist 검증용. 블록 A 전엔 cwrtc 충돌.
- **배포본 console** (80) — Phase 2/3 csc-server/console/. 운영 포트 (cap_net_bind 또는 reverse proxy 설계 미정 — Phase 3 와 함께 재논의).

`cims.sh start console` 한 명령으로 SRC_CONSOLE 존재 여부에 따라 Dev/Test 자동 분기.

## 포트 설계 원칙
- **Phase 1 (Test-\*, dev/debug)**: Test-CSC 4421 / Dev-Console 3001 / Test-Console 8080 / Test-CWRTC 8443 (d90a08c). 운영 포트(4420/80) 와 분리되어 동일 호스트에서 Phase 2 배포본과 공존.
- **Phase 2 (csc-server 배포본, 운영)**: csc 4420 / console 80 (운영, cap_net_bind 설계 후 추가).
- **TB 3종 (상시)**: TB-CSC 4419 / TB-Console 3000 / TB-agent sync 9902. Phase 진행 중 절대 내리지 않음.
- **verify phase2 overlay**: csc Start job 은 Server.Port=4445 overlay 로 Phase 1/배포본 모두와 공존 검증.

## 주의
- TCP 4421 과 UDP 4421 은 완전히 다른 서비스 (Test-CSC admin / CSP CscInterface). 번호 재배치 후보지만 현재 기술적 충돌 없음.
- Admin 과 MCPTT 는 같은 host 에서 서로 다른 port 로 분리 (ex: Test-CSC 4421 admin / 4430 mcptt).
- 같은 호스트에서 여러 agent: `CIMS_AGENT_SYNC_PORT` env 로 주입 (TB-agent 9902, Test-agent 9903 등).
- **cwrtc.json LocalIp stale 함정**: configure 가 실제 ens160 IP 가 아닌 옛 IP 로 호출되면 cwrtc 가 SIP UA UDP bind 실패 ("UdpListen(5062) error" → "StartServer failed"). preflight 의 ens160 IP 와 cwrtc.json `Setup.LocalIp` 가 일치하는지 항상 확인.
