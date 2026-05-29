# OAM / CSC systemd 영구화 가이드

OAM 분리 Phase 4 후속 작업. 현재 management host 의 oam_app.py / csc_app.py 는 nohup 으로 띄워져 있어 host 재기동 시 소실. systemd unit 등록으로 자동 부활 보장.

## 현 상태 (2026-05-29)

management host (10.0.2.45 = ctrl01):
- `python3 oam_app.py` (port 4419) — nohup, agent heartbeat 수신
- `python3 -u csc_app.py` (port 4421 admin + 4430 mcptt) — nohup, 가입자 CRUD + UE IdMS

## systemd 등록 (사용자 sudo 필요)

### 1. cims-oam.service

`/etc/systemd/system/cims-oam.service`:

```ini
[Unit]
Description=CIMS OAM (Operation & Management)
After=network-online.target mariadb.service
Wants=network-online.target

[Service]
Type=simple
User=cims
Group=cims
WorkingDirectory=/home/cims/work/cims/build/dist/oam/src
ExecStart=/usr/bin/python3 -u oam_app.py
Environment=CIMS_OAM_CONFIG=/home/cims/work/cims/build/dist/oam/config/oam-tb.json
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 2. cims-csc.service

`/etc/systemd/system/cims-csc.service`:

```ini
[Unit]
Description=CIMS CSC (Service Controller — 가입자/MCPTT)
After=network-online.target mariadb.service cims-oam.service
Wants=network-online.target

[Service]
Type=simple
User=cims
Group=cims
WorkingDirectory=/home/cims/work/cims/build/dist/csc/src
ExecStart=/usr/bin/python3 -u csc_app.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 3. 활성화 (사용자 sudo)

```bash
# 기존 nohup 정리
pkill -f "build/dist/oam/src/oam_app.py" || true
pkill -f "build/dist/csc/src/csc_app.py" || true

# systemd 등록 + 시작
sudo systemctl daemon-reload
sudo systemctl enable --now cims-oam.service
sudo systemctl enable --now cims-csc.service

# 확인
systemctl status cims-oam cims-csc --no-pager
ss -tlnp | grep -E ':4419|:4421|:4430'
```

## prod 배포 시 (별도 cycle)

management host 의 dev tree (`build/dist/`) 가 아닌 정식 install path (예: `/opt/cims-oam`, `/opt/cims-csc`) 로 배포 후 systemd 등록 권장. 현재는 dev tree 동작 → host 재기동 시 nohup 소실.

prod 절체 시:
1. oam tarball 별도 install (예: `/opt/cims-oam/`)
2. csc tarball 별도 install (예: `/opt/cims-csc/`)
3. ExecStart 경로 그대로 변경
4. systemd 등록 + 활성화

## 후속 cycle 작업

- agent 의 `cims-svc start csc/oam` 가 single-module install 지원하도록 lifecycle.sh 수정
  - 현재: `cmd_start <svc>` 가 default 'all' fallback → cmp/csp 도 찾아 fail
  - 해결: process_name="csc" or "oam" 명시 시 해당 모듈만 시작
- prod 망 분리 시 agent install_command URL 의 host IP 변경 (현재 10.0.2.45:4419 = TB-OAM)
