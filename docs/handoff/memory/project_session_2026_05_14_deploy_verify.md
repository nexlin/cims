---
name: session-2026-05-14-deploy-verify
description: "2026-05-14 야간 — NetNS 4-node 에 csp+cmp 패키지 배포 + 모듈 기동 + SIP 통신 layer 검증 (자율 진행). REGISTER 까지 ✓, 호 시도는 service_binding 부재로 skip. 발견 이슈 3건 + 다음 회차 가이드."
metadata: 
  node_type: memory
  type: project
  originSessionId: 1dc43bf8-57c0-434a-a796-ac65292b07a5
---

# 자율 진행 결과 — 패키지 배포 + 검증 (2026-05-14)

## TL;DR

- **목표**: NetNS 4-node 환경에 csp/cmp 배포 + 모듈 기동 + 1콜 VoLTE
- **결과**: 통신/인증 layer ✓ — SIP REGISTER 가 csp 에 도달 + Digest 인증 성공. 단 **호 시도는 service_binding (volte/ptt 구독 정보) 부재로 거부** (외부 DB 위임 결정에 따라 NetNS 에서 DB 없음).
- **발견 이슈 3건**: csp 가 `LocalIp=0.0.0.0` 무시하고 mgmt IP 로 bind / fail-over 시 VIP 경유 의미 제한 / 모듈 default config IP 가 외부 환경 박힘.

## 진행 단계 + 사용자 수동 재현 명령

### A. 사전 점검 (모두 정상)

```bash
# 환경 살아있는지
ss -tlnp 2>/dev/null | grep -E ':4419|:3000'   # TB-CSC + Vite
sudo ./verify/scripts/ha-netns-status.sh        # NetNS ping/multicast
tail -50 /tmp/tb-csc.log | grep heartbeat | tail -4  # 4 agent

# file_store 데이터
ls /home/nex/work/cims/build/dist/ext_mnt/runtime/{agents,ha_groups,packages,deployments}/
```

### B. 패키지 배포

**Control-Server 에 csp / Media-Server 에 cmp 배포** (Media-Server 의 CMP/PMP/IMP 3종 pending deployment 19~24 는 옛 회차 잔재 — 이번엔 19, 20 (CMP) 만 활용).

```bash
# 1) Control-Server 용 CSP deployment 2개 생성 (agent 1, 2)
for aid in 1 2; do
  curl -sk -X POST -H "Content-Type: application/json" \
    -d "{\"agent_id\":$aid,\"package_id\":5,\"process_name\":\"CSP\"}" \
    https://127.0.0.1:4419/api/v1/deployments
done

# 2) install job 트리거 (Control CSP=27,28 + Media CMP=19,20)
for dep in 27 28 19 20; do
  curl -sk -X POST -H "Content-Type: application/json" \
    -d '{"job_type":"install"}' \
    https://127.0.0.1:4419/api/v1/deployments/$dep/job
done

# 3) 결과 확인 (rc=0 이어야)
ls -t /home/nex/work/cims/build/dist/ext_mnt/runtime/jobs/*.json | head -4
```

**install_path** (cwd fallback): `~/build/dist/netns-agents/<ns>/install/modules/<pkg>/0.0.1/<PROC>`

### C. 모듈 기동 — config IP fix 필수

⚡ **이번 회차의 핵심 발견**: 패키지 default config 의 IP 가 외부 환경 (`192.168.199.129`) 으로 박혀있어 NetNS 에서 bind 실패. start job 전에 IP fix 필요.

```bash
# config patch (Python 3 in-place 수정)
python3 <<'EOF'
import json
configs = {
    'ctrl-a': {
        'path': '/home/nex/work/cims/build/dist/netns-agents/ctrl-a/install/modules/csp/0.0.1/CSP/csp/config/csp.json',
        'patches': {'Setup.Sip.LocalIp': '0.0.0.0',
                    'Setup.MediaServer.Host': '10.0.1.21',
                    'Setup.MediaServer.LocalIp': '10.0.1.11'},
    },
    'ctrl-b': {
        'path': '/home/nex/work/cims/build/dist/netns-agents/ctrl-b/install/modules/csp/0.0.1/CSP/csp/config/csp.json',
        'patches': {'Setup.Sip.LocalIp': '0.0.0.0',
                    'Setup.MediaServer.Host': '10.0.1.21',
                    'Setup.MediaServer.LocalIp': '10.0.1.12'},
    },
    'media-a': {
        'path': '/home/nex/work/cims/build/dist/netns-agents/media-a/install/modules/cmp/0.0.1/CMP/cmp/config/cmp.json',
        'patches': {'ServerIp': '10.0.1.21', 'RtpIp': '10.0.1.21', 'CspIp': '10.0.1.13'},
    },
    'media-b': {
        'path': '/home/nex/work/cims/build/dist/netns-agents/media-b/install/modules/cmp/0.0.1/CMP/cmp/config/cmp.json',
        'patches': {'ServerIp': '10.0.1.22', 'RtpIp': '10.0.1.22', 'CspIp': '10.0.1.13'},
    },
}
def set_nested(d, k, v):
    keys = k.split('.'); cur = d
    for x in keys[:-1]: cur = cur.setdefault(x, {})
    cur[keys[-1]] = v
for ns, info in configs.items():
    with open(info['path']) as f: d = json.load(f)
    for k, v in info['patches'].items(): set_nested(d, k, v)
    with open(info['path'], 'w') as f: json.dump(d, f, indent=4, ensure_ascii=False)
    print(f"[{ns}] patched")
EOF

# restart job
for dep in 27 28 19 20; do
  curl -sk -X POST -H "Content-Type: application/json" \
    -d '{"job_type":"restart"}' \
    https://127.0.0.1:4419/api/v1/deployments/$dep/job
done

# LISTEN 검증
for ns in ctrl-a ctrl-b media-a media-b; do
  echo "[$ns]"
  sudo ip netns exec $ns ss -tulnp | grep -E "csp|cmp" | head -10
done
```

**결과**:
- ctrl-a/b: csp UDP 5060 + TCP 25061/5061/16000 + CscInterface 4421 + CMP heartbeat 정상
- media-a/b: cmp UDP 9000 (control) + RTP 50000~ / PTT 52000~ / Floor 54000~ / Video 56000~

⚠ **이슈**: csp 가 `Setup.Sip.LocalIp=0.0.0.0` 무시하고 mgmt IP (10.0.0.11/12) 로 bind. svc 망 (10.0.1.x) / VIP (10.0.1.13) 로 SIP 수신 못함.

### D. SIP 통신 검증 — cspsim REGISTER

NetNS 안에서 cspsim 띄움 (host 의 cspsim 은 svc bridge 도달 못함).

```bash
sudo ip netns exec ctrl-a sudo -u nex bash -c '
cd /home/nex/work/cims/build/dist/cspsim
timeout 10 ./bin/cspsim \
  -server_ip 10.0.0.11 \
  -count 1 \
  -user +82571900001 \
  -domain ptt.mnc033.mcc450.3gppnetwork.org \
  -password 123456 \
  -mode ptt \
  -scenario register'
```

**결과**: SIP INVITE/REGISTER 정상 도달 + Digest 인증 성공 (`auth_id=4503382571900001` 매칭). 응답: **`403 Forbidden — Auth reject: user=+82571900001 has no service binding`**.

즉:
- ✅ 통신 layer (UDP 5060) ✓
- ✅ SIP parsing ✓
- ✅ Digest 인증 ✓
- ✅ CSP → CMP heartbeat (`CmpClient TX → 10.0.1.21:9000 OK`)
- ❌ **service_binding 데이터 부재** — volte_subscriptions / ptt_subscriptions 가 DB 에 있어야 함. NetNS 에 DB 미연결 + 외부 DB 위임 결정 ([[project_db_external]]).

### E. fail-over LIVE 검증 — skip 결정

skip 사유: csp 가 mgmt IP 로 bind → VIP 10.0.1.13 통한 SIP 트래픽 수신 불가 → fail-over 시 cspsim 이 ctrl-b 의 mgmt IP (10.0.0.12) 로 재호 필요 (VIP 경유 fail-over 의미 없음). 인프라 layer 의 keepalived fail-over 자체는 이전 세션 검증 완료 (10.0.1.13 VIP 인수).

## 발견 이슈 (우선순위)

| # | 우선순위 | 이슈 | 근본 원인 추정 | 권장 fix |
|---|---|---|---|---|
| 1 | 🔴 High | csp 가 `Setup.Sip.LocalIp=0.0.0.0` 무시하고 mgmt IP 로 bind | csp 의 bind 로직이 0.0.0.0 → "default route 의 outgoing IP" 로 변환. 또는 local_nodes JSONL 의 endpoint 비어있어서 _infra fallback 후 사회적 default 적용 | local_nodes.jsonl 에 explicit endpoint 추가 (10.0.1.13:5060) 또는 csp 코드의 0.0.0.0 처리 명확화 |
| 2 | 🟡 Mid | 모듈 default config IP 가 외부 환경 (`192.168.199.129`) 박힘 | 패키지 빌드 시 default config 가 운영 환경 가정 | config_template 에 placeholder (`@AGENT_SVC_IP@` 등) + agent 가 install 시 자동 치환 (또는 deployment.config 의 ConfigPutResult 로 명시) |
| 3 | 🟢 Low | service_binding 데이터 부재 — DB 미연결 NetNS 에서 자체 검증 어려움 | 외부 DB 위임 결정 ([[project_db_external]]) | 옵션 A: NetNS 안에 sqlite/mysql 띄우기 — Test only / 옵션 B: cspsim 의 `-no_register` 모드로 우회 검증 / 옵션 C: 가입자 도메인도 file-store migrate 검토 (옛 결정과 충돌) |

## fix 권장 순서

1. **이슈 1 (csp bind)** — local_nodes 정의 시도. csp 의 `Setup.Sip.LocalIp` 가 의도대로 동작하는지 코드 fix.
2. **이슈 2 (default config IP)** — agent install job 의 params 또는 deployment.config 로 placeholder 자동 치환.
3. **이슈 3 (service_binding)** — 1, 2 fix 후 cspsim `-no_register` 또는 sqlite 환경으로 1콜 LIVE.

## 환경 정리 (이번 세션 끝 상태)

```
file_store/deployments:
  19, 20  CMP@media-a/b      status=running   /modules/cmp/0.0.1/CMP
  21, 22  PMP@media-a/b      status=pending   (이번 시나리오 외)
  23, 24  IMP@media-a/b      status=pending   (이번 시나리오 외)
  27, 28  CSP@ctrl-a/b       status=running   /modules/csp/0.0.1/CSP

LISTEN:
  ctrl-a: 10.0.0.11:5060 (CSP UDP) + 25061/5061 (TCP) + 4421 (CscInterface) + 16000 / 9001
  ctrl-b: 10.0.0.12:5060 / 25061 / 5061 / 4421 / 16000 / 9001
  media-a: 10.0.1.21:9000 (CMP control) + 50000~/52000~/54000~/56000~
  media-b: 10.0.1.22:9000 (CMP control) + 50000~/52000~/54000~/56000~

agent: 4 (PID 48521~) heartbeat 정상
keepalived: ctrl-a MASTER (VIP 10.0.1.13), ctrl-b BACKUP
TB-CSC: 4419 ✓
Vite: 3000 ✓
```

## 명령 cheat sheet — 내일 수동 재현 시

```bash
# (0) sudo 비번: <REDACTED_SUDO_PW>

# (1) 환경 살아있는지
ss -tlnp | grep -E ':4419|:3000'
sudo ./verify/scripts/ha-netns-status.sh

# (2) deployment 상태
ls /home/nex/work/cims/build/dist/ext_mnt/runtime/deployments/*.json

# (3) LISTEN 검증
sudo ip netns exec ctrl-a ss -tulnp | grep csp
sudo ip netns exec media-a ss -ulnp | grep cmp

# (4) cspsim REGISTER 재현
sudo ip netns exec ctrl-a sudo -u nex bash -c '
cd /home/nex/work/cims/build/dist/cspsim
./bin/cspsim -server_ip 10.0.0.11 -count 1 \
  -user +82571900001 -domain ptt.mnc033.mcc450.3gppnetwork.org \
  -password 123456 -mode ptt -scenario register
'

# (5) csp 로그 (REGISTER 처리 흔적)
tail -30 /home/nex/work/cims/build/dist/netns-agents/ctrl-a/install/modules/csp/0.0.1/CSP/csp/log/csp_*.log

# (6) 모듈 stop/start (deployment 상태 변경)
curl -sk -X POST -H "Content-Type: application/json" \
  -d '{"job_type":"stop"}' \
  https://127.0.0.1:4419/api/v1/deployments/27/job
```

## 관련 메모리

- [[user_credentials]] — sudo 비번 `<REDACTED_SUDO_PW>`
- [[project_db_external]] — 외부 DB 위임 결정 (가입자 도메인)
- [[project_session_2026_05_14_phase1_refactor.md]] — A/S 정상 설치 + split brain fix
