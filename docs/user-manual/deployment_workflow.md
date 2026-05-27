# Console 배포 작업 순서

Console 에서 새 서버에 모듈을 배포하고 서비스 시작까지의 표준 절차.

## 0. 전제

- CSC/Console 이미 구동 중
- 관리자 계정으로 Console 로그인 (`https://<CSC>:8081/` 또는 dev `http://<ens160>:3000/`)

> 좌측 메뉴는 두 갈래:
> - **패키징** (`/release/...`) — 검증 / 검증 이력 / 빌드·패키징·다운로드
> - **배포** (`/deploy/...`) — 패키지 / 서버+HA (primary) / 서버 Inspector (advanced)

## 1. 패키지 업로드 (배포 메뉴)

**메뉴**: 좌측 `배포 > 패키지` (`/deploy/packages`)

1. `＋ 패키지 업로드` 클릭
2. 빌드된 tarball (`<module>-<version>.tar.gz`) 을 선택 (여러 개 동시 가능)
3. "업로드" → 완료될 때까지 대기
4. 좌측 모듈 목록에서 업로드된 모듈이 보임 (버전 수 뱃지 표시)
5. 버전 카드 펼치면 `config_template.json` 유무 · 크기 · SHA256 확인 가능

**재업로드**: 동일 (모듈명, 버전) 업로드 시 자동으로 덮어쓰기.

> CIMS 자체에서 빌드한 tarball 은 `패키징 > 패키징` (`/release/package`)
> 카드의 ⤓ 다운로드 버튼으로 받아서 그대로 위 화면에 업로드한다. 자세한
> 워크플로우는 `design/features/build_and_packaging.md` 참고.

## 2. 서비스 + 서버 등록 (primary 흐름)

**메뉴**: 좌측 `배포 > 서버 + HA` (`/deploy/services`)

서비스(=HA 그룹 또는 standalone) 단위로 서버를 묶어 inline 편집. 팝업 없음.

1. `＋ 시스템 추가` 클릭
2. 이름 입력 + 유형 선택:
   - **A/S** — master + backup 2개 자동 발급 (예: SIP 서버 이중화)
   - **AA** — 시작 1개 발급, `＋ 서버 추가` 로 N개 확장 (예: 미디어 서버 분산)
   - **Standalone** — agent 1개 (예: 단일 노드)
3. `생성` — agent 자동 발급 + (A/S·AA 만) ha_groups 자동 생성
4. 각 서버 row 의 `📋 복사` 로 install command 복사 → 대상 호스트에서 실행:
   ```bash
   mkdir /opt/cims-agent && cd /opt/cims-agent
   curl -k https://<CSC>:4420/install-agent.sh | bash -s -- \
     --csc-url https://<CSC>:4420 \
     --enrollment-token <TOKEN> \
     --name <이름>
   ```
5. agent enroll 완료 → `pending → online` 자동 전환 + `interfaces` 자동 보고
6. 서버 row 의 `📡 인터페이스 N개` 펼침 → IP / 용도 입력 → 자동 저장
7. (A/S·AA) 서비스 row 의 `📡 VIP` 펼침 → 용도 선택 → VIP IP 입력 → 멤버별 iface 자동 매핑

**패키지 추가**: 서비스 행 펼침 → `＋ 패키지 추가` → 체크박스 → 서비스의 모든 멤버에 일괄 deployment 생성.

> 검증/시험 환경에서는 `volte-sip-server` / `volte-media-server` /
> `ptt-sip-server` / `ptt-media-server` / `mgmt-server` 5개 이름을 그대로
> 따라 등록하면 verify pipeline 의 `_INSTANCES` 매핑과 일치한다.

## 3. 모듈 세부 설정 (Advanced — Server Inspector)

**메뉴**: 좌측 `배포 > 서버 Inspector` (`/deploy/servers`)

서비스 단위 일괄 배포(2.)로 부족할 때 — process_name 커스터마이즈,
service_functions 체크박스(volte/ptt/ibcf), per-deployment 메모.

**위치**: 서버 선택 → `모듈` 탭 → `＋ 모듈 추가`

1. **Module** 선택 (예: `csp`, `psp`, `cmp`, `pmp`, ...)
   - 같은 base 바이너리의 변종은 별도 패키지로 노출 (csp/psp/isp / cmp/pmp/imp)
2. **Version** 선택 (예: `0.0.10` / 최신)
3. **Process** 선택 — `CSP` (통합), `PSP` (PTT 전용), `ISP` (IBCF 전용),
   `CMP`, `PMP`, `IMP` 중. 보통 모듈명과 일치
4. **Functions** 선택 — process 별로 체크박스 (`volte`, `ptt`, `ibcf`)
5. 메모 (선택) → `추가`

→ Deployment 가 `pending` 상태로 생성됨. 아직 파일 없음.

## 4. 설정 입력

**위치**: 해당 모듈 row → `⚙ 설정` 버튼

### 4.1 scalar 설정 (탭: "설정")

- `config_template.json` 의 sections 가 폼으로 렌더링
- 🔁 재기동 필요 / ⚡ 즉시 적용 표시
- 변경한 필드는 ● 표시
- 저장 시 `update_config` job 큐잉

### 4.2 collection 설정 (탭: "리스너" 등)

- deployment 가 **install 되어 있어야** 활성화 (pending 상태에선 에러)
- `＋ 추가` → 행 편집 → `저장`
- 저장 시 Agent 의 jsonl 에 즉시 반영 + SIGUSR1 시그널 (CSP 가 실행 중이면 즉시 rebind)

## 5. 설치 (Install)

**위치**: 모듈 row → `설치` 버튼 (pending 상태에서만)

- 내부적으로 `install` job 큐잉 → Agent 가 heartbeat(30s) 시 pickup
- 완료까지 최대 ~1분. Status 가 `stopped` 로 전환

## 6. 시작 (Start)

**위치**: 모듈 row → `▶ Start`

- `start` job → Agent 가 `install_path/cims.sh start <process>` 실행
- 성공 시 `running`

## 7. 이후 운영

| 작업 | 메뉴 / 버튼 |
|---|---|
| 설정 변경 | ⚙ 설정 (탭 선택) |
| 재기동 | ↻ (Restart) |
| 중지 | ■ (Stop) |
| 재설치 (같은 or 상위 버전) | 설치 |
| 모듈 제거 | ✕ (Delete deployment) |
| 서버 세션 폐기 | 서버 헤더의 "폐기" |
| Agent 바이너리 업그레이드 | "↑ 업그레이드" |

## 8. 문제 해결

| 증상 | 확인 |
|---|---|
| Agent status 가 `approved` 에서 `online` 으로 안 바뀜 | 대상 호스트에서 `systemctl --user status cims-agent` active 인지, `journalctl --user -u cims-agent` 로그, CSC URL 유효한지 |
| "＋ 모듈 추가" 에서 모듈 목록이 비어있음 | 1. 패키지 업로드 먼저 |
| 설정 저장 시 `not_installed` | 설치 버튼 먼저 눌러 deployment.install_path 생성 |
| Collection 탭에서 `agent_proxy_failed` | Agent 가 heartbeat 보내 `sync_port` 가 DB 에 기록됐는지, 방화벽 9900 포트 |
| `signaled:[]` 로 반환 (빈 배열) | `install_path/run/*.pid` 파일이 없음. CSP 가 pid 를 쓰도록 실행 중이어야 함 |
