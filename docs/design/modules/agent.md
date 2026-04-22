# Agent — 배포/프로세스 제어 데몬

> 버전: 1.0 (2026-04-21)

**단일 Python 바이너리** (`agent/cims_agent.py`) 로 구성된 배포 데몬. 각 호스트에서 실행되며 CSC 의 명령을 수행하고 설정 jsonl 을 관리합니다.

## 1. 역할

1. **Enroll**: 최초 기동 시 enrollment 토큰으로 CSC 에 등록 → session 토큰 수령
2. **Heartbeat**: 30초 주기로 CSC 에 상태 보고 + pending job 수령
3. **Job 실행**:
   - `install` / `upgrade`: 패키지 다운로드 + tar 풀기 + config 이관
   - `start` / `stop` / `restart`: `install_path/cims.sh` 호출
   - `update_config`: `config.json` 재기록
   - `uninstall`: install_path 제거
   - `upgrade_agent`: 자기 자신(cims_agent.py) 교체 후 재시작
   - `health_check`: 포트 probe
4. **Sync REST 서버** (HTTPS): CSC 가 collection(jsonl) 을 즉시 read/write 할 수 있게 REST 엔드포인트 노출

## 2. 파일 레이아웃

```
<agent 설치 디렉토리>/
├── cims_agent.py         바이너리
├── run.sh                런처 (systemd 없이 수동 기동)
├── state/
│   ├── state.json        {agent_id, session_token, name}
│   ├── agent.crt         self-signed 인증서 (REST 서버용)
│   └── agent.key
└── modules/              배포된 모듈들
    └── <module>/<version>/<process>/  ...
```

## 3. 상태 머신

| 상태 | 조건 |
|---|---|
| `pending` | CSC 가 agent 를 생성했으나 enroll 아직 안 됨 |
| `approved` | enroll 완료 (승인됨), heartbeat 시작 전/중단 |
| `online` | 최근 heartbeat 수신 |
| `offline` | 일정 시간 heartbeat 끊김 |
| `revoked` | 관리자가 세션 폐기 |

## 4. 설치 방법

```bash
# 설치 디렉토리에서 (현재 디렉토리에 설치됨)
mkdir /opt/cims-agent && cd /opt/cims-agent
curl -k https://<CSC>:4420/install-agent.sh | bash -s -- \
  --csc-url https://<CSC>:4420 \
  --enrollment-token <TOKEN> \
  --name <AGENT_NAME> \
  [--sync-port 9900] [--no-systemd]
```

- `--no-systemd`: systemd user service 설치 없이 수동 기동 (`./run.sh`)
- systemd 사용 시 `~/.config/systemd/user/cims-agent.service` 자동 생성. 부팅 시 자동 기동하려면:
  ```bash
  sudo loginctl enable-linger $USER
  ```

## 5. Sync REST 엔드포인트

TLS + `X-Agent-Token` 인증. 세부 명세는 `api/collection_api.md` 의 Agent 측 엔드포인트 참조.

| 메서드 | 경로 | 용도 |
|---|---|---|
| GET | `/health` | 헬스 + agent_id + 버전 |
| GET | `/collection?install_path=&name=` | jsonl 읽기 |
| PUT | `/collection?install_path=&name=` | jsonl 원자 쓰기 + 선택적 SIGUSR1 |
| POST | `/signal?install_path=&sig=usr1\|hup` | 프로세스에 시그널 |

## 6. 프로세스 시그널 룰

Agent 는 PUT /collection 의 `signal=true` 파라미터 수신 시 `install_path/run/*.pid` 파일을 찾아 해당 pid 에 SIGUSR1 전송. CSP 는 SIGUSR1 수신 시 jsonl 을 재로드합니다.

## 7. 보안

- 세션 토큰은 `state/state.json` (파일 모드 0600)
- Sync REST: 자체 서명 인증서 (`state/agent.crt/key`) + session 토큰 헤더
- 업그레이드: `cims_agent.py` 자체 교체 시 기존 바이너리 덮어쓰기 후 프로세스 종료 → systemd 가 재기동 (`Restart=always`)

## 8. 운영 시 주의

- `state/` 디렉토리 손실 = re-enroll 필요 (관리자가 새 enrollment 토큰 발급)
- Agent 업그레이드는 `upgrade_agent` job 으로만 수행 (파일 수동 교체 시 세션 상태 불일치)
- 동일 호스트에 Agent 여러 개 실행 금지 (port conflict: 9900)
