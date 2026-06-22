# 개발 서버 이전 — Claude Code 이어가기 가이드

새 개발 서버로 옮긴 뒤 이 저장소를 받아 Claude Code 로 작업을 이어가기 위한 절차.

작성 시점 마지막 커밋: `8fb6f55` (2026-05-20).
`docs/handoff/memory/` 에 Claude Code 의 auto-memory 스냅샷이 동봉되어 있다.

## 0. 새 서버 사전 준비

```bash
# 빌드/런타임 의존성
sudo apt-get update
sudo apt-get install -y cmake build-essential libssl-dev clang-format \
                        git python3 python3-pip nodejs npm \
                        net-tools iproute2 sudo openssl

# Python 추가 (csc 의존성)
pip3 install pymysql aiohttp

# (선택) DB 가 외부 위임이면 skip. 로컬 DB 띄울 거면 mariadb-server
```

Node 버전은 `npm --version` 이 9 이상 (vite 호환). 안 되면 nvm 으로 v20+.

## 1. 저장소 받기

```bash
mkdir -p ~/work && cd ~/work
git clone https://github.com/nexlin/cims.git
cd cims
git log --oneline -3   # 8fb6f55 가 최신인지 확인
```

CLAUDE.md 가 루트에 있다 — Claude Code 가 자동으로 읽음.

## 2. Claude Code 설치 + 메모리 복원

```bash
# Claude Code 설치 — 공식 안내 따르기 (npm 또는 native installer).
# 예: npm 으로
npm install -g @anthropic-ai/claude-code

# 메모리 디렉터리 복원 (이 저장소 안 사본 → ~/.claude 영역)
MEM_SRC="$PWD/docs/handoff/memory"
MEM_DST="$HOME/.claude/projects/-home-nex-work-cims/memory"
mkdir -p "$MEM_DST"
cp -p "$MEM_SRC"/*.md "$MEM_DST"/
ls "$MEM_DST" | wc -l    # 50+ 개여야 함
```

이후 `cd ~/work/cims && claude` 하면 이전 세션 메모리를 그대로 읽고 작업을 이어간다.

**중요**: 새 서버의 작업 경로가 `/home/<user>/work/cims` 가 아니면 `~/.claude/projects/-home-<user>-work-cims/memory` 처럼 경로 슬러그가 달라진다. 위 cp 대상 경로의 `-home-nex-work-cims` 부분을 본인 경로로 바꿔야 함.

**민감 데이터 안내**: 옛 메모리에는 sudo/DB 비밀번호 평문이 있어 git 에 올릴 때 마스킹(`<REDACTED_SUDO_PW>`, `<REDACTED_DB_PW>`)으로 치환했고 `user_credentials.md` 자체는 동봉하지 않았다. 새 서버에서 sudo 자동화가 필요하면 본인이 다음 파일을 직접 만든다 (git untracked 영역):

```bash
cat > "$MEM_DST/user_credentials.md" <<'EOF'
---
name: user-credentials
description: 사용자의 dev 머신 자격증명. 절대 git/script/docs/코드에 평문 기록 금지.
metadata:
  type: user
---

## sudo 비밀번호
- 값: `<본인 비번>`
EOF
chmod 600 "$MEM_DST/user_credentials.md"
```

Claude Code 가 이 파일을 자동으로 읽어 본 머신용 비밀번호로 사용한다. 옛 메모리의 `<REDACTED_SUDO_PW>` 자리는 본인이 따로 인지하면 됨.

## 3. 빌드 + 시험환경 설정

```bash
# C++ + Web UI 일괄 빌드 (첫 빌드 5~10분, 외부 의존성 다운로드 포함)
./cims.sh build

# 로컬 IP / DB 비번 / 도메인 반영
./cims.sh configure --local-ip <SERVER_IP> --db-password <PASSWORD>
# 또는 비대화형:
./cims.sh configure --local-ip $(ip route get 8.8.8.8 | awk '{print $7; exit}')
```

`cims.sh init` 으로 `.cims/server.local.json` 만들어두면 다음부터 인자 생략 가능.

## 4. TB-CSC + TB-Console 기동 (개발 워크플로 4419 / 3000)

```bash
./cims.sh tb start         # csc + console 둘 다
./cims.sh tb status        # 4419 / 3000 점유 확인

# 헬스 체크 (csc 는 HTTPS only, /api/v1/health 없음 — 실제 endpoint 로 확인)
curl -sk https://127.0.0.1:4419/api/v1/csp/services | head -c 200
```

Console 접속: `http://<SERVER_IP>:3000` (브라우저).

## 5. (선택) netns 다중 노드 환경 재구축

지금까지 commit 들은 ctrl-a/b + media-a/b 4-node netns 환경 기준으로 LIVE 검증되었다. 새 서버에 같은 토폴로지를 만들려면 `deployment/` 가이드를 따른다. 빠른 길:

```bash
# 환경 정의 + 시나리오 render → bundle
./cims.sh sync   # python/스크립트 만 dist 로 (build 직후라면 skip 가능)
ls deployment/   # tb-netns-4-node / volte-ptt 등 yaml 셋트
# 단일-host 빠른 검증 (netns 안 만들고)
deployment/bin/apply.py --env dev-single-host --scenario volte-only --backup --restart auto --verify
```

자세한 절차는 `deployment/README.md` + `docs/handoff/memory/project_session_2026_05_15_deployment_scaffold.md` 참조.

## 6. 진행 중인 트랙 — 어디서 이어갈지

메모리 인덱스(`docs/handoff/memory/MEMORY.md`) 상단을 보면 다음과 같이 적혀있다.

> **다음 세션 진입 — M1 사용자 브라우저 클릭 검증 + M1.2 agent 재기동 후 LIVE 확인**

핵심 미완 작업:
1. **M1.2 LIVE 확인** — ServiceIp [적용] 동기 호출 (commit `8fb6f55`) 의 진짜 ip addr add 까지 LIVE round-trip 확인. 새 서버에서 netns 다시 셋업하면 agent 도 새 코드로 처음부터 도니 그냥 UI [적용] 누르면 됨.
2. **M1 브라우저 검증** — HaServicesPage 의 그룹 카드 → 모듈 설정 모달 → drift 배너 / A/S system scope 통합 편집을 사용자 눈으로 확인.

세부 컨텍스트는 메모리 인덱스에서 가장 위 두 줄의 `project_session_2026_05_20_m1_ui_verify.md` 파일 참조.

## 7. Claude Code 진입 cheat sheet (새 서버에서)

```bash
cd ~/work/cims
claude                    # 대화형 진입
# 또는 plan/loop 등 슬래시 명령 — /help 참조
```

첫 발화 권장: "메모리 인덱스 보고 다음 진입 후보 정리해줘" — 이전 세션의 작업 트랙을 보여줌.

## 부록 — 알려진 환경 의존성

- `cims.sh tb start` 가 csc 띄울 때 `build/dist/csc/config/csc-tb.json` 필요 → `./cims.sh build` 후 자동 생성.
- TB-Console 의 dev 모드는 `ems/core/console/node_modules/` 필요 → `ems/core/console/` 에서 `npm install` 한 번. configure 가 자동 처리하지 않을 수 있음.
- netns 셋업은 `sudo` 필요 — 새 서버의 user 가 sudoers 에 있어야 함.
- DB 는 외부 위임 결정 (`docs/handoff/memory/project_db_external.md`). 로컬 DB 안 띄워도 됨, 단 가입자 데이터 (volte_subscriptions / ptt_groups 등) 가 필요한 시나리오 (호 시험) 는 DB 연결 필요.
- agent 의 enrollment_token 은 새 서버에서 재발급. 옛 token (메모리 안 노출 있을 수 있음) 은 무효.

## 부록 — 메모리 파일 한 줄 인벤토리

`docs/handoff/memory/MEMORY.md` 가 인덱스. 그 외 핵심 참조:

- `project_cims_overview.md` — 프로젝트 큰 그림
- `project_ports.md` — 포트 인벤토리
- `project_db_external.md` — DB 외부 위임 결정
- `project_backlog_main_track.md` — 메인 백로그 5개 트랙 진척
- `project_session_2026_05_20_m1_ui_verify.md` — **마지막 세션 (이어가는 출발점)**
