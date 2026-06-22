---
name: project-session-2026-05-13-preset
description: 2026-05-13 추가 세션 — 백로그 2 preset (config_template.presets + ModuleConfigModal PresetBar) 인프라+컨텐츠 동시 완료. dev 가능한 5번째 작업 종료.
metadata: 
  node_type: memory
  type: project
  originSessionId: 5d4e1a79-f2d6-406f-8268-2c82caf7e771
---

# 2026-05-13 — 백로그 2 preset 완료

## 진입 컨텍스트

전 세션 (`project_session_2026_05_13_phase2_more.md`) 종료 후 사용자 "이어서 진행". 백로그 2 "잔여 — preset (패키지 config_template 컨텐츠 authoring 필요)" 가 dev 가능한 마지막 작업이었음 (백로그 3 은 wishlist 결정 대기, 1 LIVE / 2 preset 만 실행 가능). preset 인프라 자체가 없었음 — 처음부터 빌드.

## 변경 파일

```
ems/core/console/src/api/deployment.ts                          — ConfigTemplatePreset 타입 신설
ems/core/console/src/components/module/ModuleConfigModal.tsx    — applyPreset() + PresetBar UI
csp/config/config_template.json                              — 3 preset
cmp/config/config_template.json                              — 3 preset
csc/config/config_template.json                              — 3 preset
build/dist/{csp,cmp,csc}/config/config_template.json         — sync
build/dist/console/dist/                                     — sync (vite build 산출물)
```

## preset 스키마

```ts
interface ConfigTemplatePreset {
  name: string          // kebab-case key
  label: string         // UI 표시명 (한국어 가능)
  description?: string
  values: Record<string, string | number | boolean | null>
}
```

`ConfigTemplate.presets?: ConfigTemplatePreset[]`. 모달 헤더에 PresetBar (select + 적용 버튼) — 선택 → 검토 → 저장. ChangeSummaryPanel 이 자동 diff. 모르는 키 (template field 에 없음) 는 SKIP + toast 경고.

**중요**: preset 적용은 즉시 저장하지 않음. `values` state 만 set → 사용자가 검토 후 직접 저장 클릭. 안전 모델.

## 작성한 preset (3 × 3 = 9개)

**CSP** (`csp/config/config_template.json`):
1. `dev-single-node` — "개발 (단일 노드)" — UdpThreadCount=2, Roles all but IBCF, MediaServer.Host=127.0.0.1, Debug=true
2. `high-capacity` — "고용량 운영" — UdpThreadCount=8, OPTIONS keepalive 120s, UserTimeout 1800s, Debug=false
3. `ibcf-peering` — "IBCF 피어링 활성" — Roles.IBCF=true, PTT_AS=false

**CMP** (`cmp/config/config_template.json`):
1. `dev-single-node` — "개발 (소규모)" — RtpPoolSize=20, PttRtpPoolSize=10, Worker=2
2. `high-capacity` — "고용량 운영" — RtpPoolSize=200, PttRtpPoolSize=50, Worker=8, SessionTimeout=300
3. `ptt-only` — "PTT 전용 (PMP)" — VoIP 풀 4 / PTT 풀 100 / Worker 6

**CSC** (`csc/config/config_template.json`):
1. `integrated` — "통합 배포 (CSP=PSP 동일 호스트)" — Notify 127.0.0.1 단일, mTLS off
2. `split-ptt-mtls` — "PTT 분리 + mTLS" — mTLS=true (Notify IP 는 사용자 입력)
3. `long-session` — "장기 세션" — AccessTokenTtl 24h, RefreshTokenTtl 30d

## 검증

- `python3 -c json.load` × 3 — 모든 preset key 가 sections.fields 의 key 와 매칭 (오타 0)
- `npx tsc --noEmit` — pass
- `npx eslint` — pass
- `npm run build` — pass (vite build 246ms, 513KB → 143KB gzip)
- dist sync — `cp` × 3 templates + `cp -r ems/core/console/dist/. build/dist/console/dist/`

## 백엔드 (CSC) 무변경

`csc/src/handlers/modules.py:_load_dist_module()` 는 `json.load(f)` 로 template 전체를 통째로 파싱하여 응답. preset 필드도 자동 통과. `_template_field_map()` 은 sections.fields 만 walk → preset 적용 시도 키가 template 소유 키와 일치 (preset 검증은 console 측 `applyPreset` 에서). **CSC restart 불필요** — 핸들러가 매번 파일에서 읽음 (캐싱 없음).

## 다음 세션 진입 후보

| 옵션 | 메모 | 상태 |
|---|---|---|
| 백로그 3 — Console 관리 wishlist | DashboardPage/StatsPage 이미 충실. 추가 wishlist (실시간 차트 / alert 이력 / bulk import 강화) 사용자 우선순위 필요 | 🟡 사용자 결정 |
| 백로그 1 LIVE | 2-node 환경, Redis 셋업 (`apt install libhiredis-dev`), CMP All-Active 다중 인스턴스 | ⚪ 환경 의존 |
| 차후 트랙 | B1 상용 환경 / WebRTC / CERT-ROTATE | ⚪ 메인 안정화 후 |

**메인 백로그 5개 중 dev 가능 작업 전부 완료**. 남은 모든 항목은 사용자 결정 (백로그 3) 또는 환경 셋업 (백로그 1 LIVE) 의존.

## 관련 메모리

- [[project_backlog_main_track]] — 5 백로그 가이드 (§2.4 preset 진입 지점 → 본 세션으로 완료)
- [[project_session_2026_05_13_phase2_more]] — 직전 세션 (Phase 2 + 1.E-2 + 1.D-2 + 백로그 2/4)
- [[feedback_csc_dist_sync]] — dist sync 규칙 (이 세션에서도 적용)
