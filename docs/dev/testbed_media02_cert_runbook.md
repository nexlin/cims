# 테스트베드 121.161.164.49 (hostname `cims-rocky10.2`, 구 media02) 단말 대면 인증서 — 현장 runbook

> 개발·시험 환경 운영 메모. 패키지 설치는 팀원이, **인증서와 서비스 동작 확인은 인증서 담당자**가 한다.
> 이 문서는 인증서 담당자가 현장에서 보는 절차다. 절차 정본은
> [sip_tls_signaling.md §8.3](../design/features/sip_tls_signaling.md#83-발급배치-절차--새-노드는-scriptsservice-certsh-로),
> 설치 문맥은 [initial_install.md §4.5](../user-manual/initial_install.md#45-단말-대면-tls-인증서--없으면-단말이-로그인하지-못한다).

## 왜 필요한가

사내 단말 APK 는 **CIMS Service CA** 한 장만 신뢰한다. 패키지를 설치하면 CSC 4430 은 agent 가 그룹
CA(`CIMS-OAM-CA`)로 자동 발급한 인증서, CSP TLS 는 동봉 자가서명(`cert/csp.pem`)이 된다. 둘 다 단말이
거절하는 발급자라 **로그인 단계에서 TLS 가 끊기고 CSC 로그에는 요청이 남지 않는다.** .46(ctrl02)·.48(media01)
에서 실측한 현상이다. 아래 조치가 없으면 서비스 확인을 시작조차 못 한다.

## 0. 준비물 — .45 에 이미 있다

| 항목 | 값 |
|---|---|
| 묶음 | `/home/cims/certs/cert-init-cims-rocky10.2.tgz` (600, 키 포함 — scp/USB 로만 반입) |
| 발급 | 2026-09-11, leaf 만료 2028-09-10, 발급자 `CN=CIMS Service CA` |
| SAN | `DNS:cims-rocky10.2, IP:127.0.0.1, IP:121.161.164.49, IP:10.0.2.49` (+ `DNS:csc.cims.local` / `DNS:csp.cims.local`) — 설치 후 agent 가 계산한 요구 목록(`DNS:cims-rocky10.2, 127.0.0.1, 121.161.164.49`)의 상위집합 |
| 설치 후 실측 (09-11) | OS 재설치로 hostname 이 `media02` → **`cims-rocky10.2`**. 4430 = `CIMS-OAM-CA` 발급 leaf(단말 거절), 4419 = 자가서명, **CSP TLS = 5061**(자가서명 `CN=csp`), 15061 닫힘. verify 기준선 FAIL 확인 |
| 안에 든 것 | `csc/server.{crt,key}` · `csp/csp-chain.pem,csp.key` · `cims-service-ca.crt` · `service-cert.sh` · `README.txt`(0~6 단계 현장 절차 전체) · `RUNBOOK-media02.md`(이 문서 사본) — **현장에서는 tgz 만 있으면 된다** |

묶음 디렉터리 `/home/cims/certs/cert-init-cims-rocky10.2/` 의 `service-cert.sh verify` 는 .45 에서도 원격 검증에 쓸 수 있다.
재발급이 필요하면 .45 에서 `scripts/service-cert.sh issue --ip 121.161.164.49 --runbook docs/dev/testbed_media02_cert_runbook.md` 한 줄이다(같은 자리에 덮어쓴다).
`.45` 의 ssh 키가 이 노드에서 거부되면 `--host cims-rocky10.2 --san "DNS:cims-rocky10.2,IP:127.0.0.1,IP:121.161.164.49,IP:10.0.2.49"` 를 함께 준다.
VIP 를 쓰게 되면 `--vip <VIP 주소>` 를 붙인다.

## 1. 팀원(설치 담당)에게 미리 확인·요청할 것

| 항목 | 이유 |
|---|---|
| OAM 접속 주소(`AgentOamUrl`)를 `121.161.164.49` 또는 `10.0.2.49` 로 | 둘은 SAN 에 있다. 다른 이름·VIP 를 쓰거나 노드에 IP 가 더 생기면 SAN 부족으로 `install` 이 거절한다 → .45 에서 `issue` 재발급 1회 |
| HA/VIP 미사용 확인 | VIP 는 SAN 에 없다. 쓰면 `issue --ip 121.161.164.49 --vip <VIP>` 로 재발급 |
| CSP `local_nodes` TLS 행의 `bind_port` — 실측 **5061** | `verify --csp-port 5061`. CSC 자동 프로비저닝 `TLS 포트`도 5061 이어야 단말에 TLS 가 광고된다. UDP 포트(5060/15060)도 CSC 프로비저닝과 일치 확인 |
| 사내 단말 가입자·로그인 계정 프로비저닝 | CSC DB 가 별개라 .45 계정이 없다 |
| **CSC 가 기동된 시점**을 알려달라 | 그때 `install` 을 돌린다(CSC 가 이미 떠 있어야 핫리로드 확인이 된다) |
| 콘솔 admin 비밀번호 (또는 편집 권한) | §3 자동 저장(`csp-node`)에 admin 로그인이 필요. 없으면 팀원에게 콘솔 저장 두 줄을 넘긴다 |
| `10.0.2.49` 가 ctrl02(.46)의 보조 주소로도 잡혀 있다 | 재설치 후 이 노드에는 `10.0.2.49` 가 잡혀 있지 않은 것으로 보인다(agent 인증서 SAN 기준). 다시 붙이면 ctrl02 와 충돌 — 그때 ctrl02 쪽 보조 주소 정리 |

## 2. 인증서 배치 — CSC 설치·기동이 끝난 직후, media02 에서 `cims` 계정으로

```bash
tar xzf cert-init-cims-rocky10.2.tgz
bash cert-init-cims-rocky10.2/service-cert.sh install
```

출력 판독:

| 출력 | 뜻 |
|---|---|
| `[OK] SAN 대조 통과` | 이 노드가 요구하는 SAN 을 다 담고 있다. 재기동에도 덮이지 않는다 |
| `[INFO] 기존 CSC 인증서 백업: server.{crt,key}.bak.<시각>` | 원복 지점 |
| `[OK] CSC 가 새 인증서를 서빙한다` | 핫리로드 완료(재시작 불필요) |
| `[WARN] CSC 핫리로드 미반영` | 콘솔에서 CSC 재시작 요청 → 뒤에 verify |
| `[ERROR] … SAN 에 이 노드가 요구하는 항목이 없다` (exit 2) | 배치하지 않았다. .45 에서 `issue --ip 121.161.164.49` 로 재발급 → 새 tgz 반입 → 다시 install |

CSP 파일은 `/opt/cims-agent/modules/csp/runtime/cert/` 에 놓인다. §3 이 있어야 CSP 가 쓴다.

## 3. CSP 에 경로 알리기 — 자동(권장) 또는 콘솔

CSP 는 파일을 놓는 것만으로는 새 인증서를 쓰지 않는다. `local_nodes` TLS 행이 그 파일을 가리켜야 한다.

**자동** — 같은 노드에서, 콘솔 admin 비밀번호를 넣는다. 콘솔 저장 버튼이 부르는 OAM API 를 그대로 호출해
SIGUSR1 로 무중단 반영되고, 끝나면 그 포트로 verify 까지 돌린다. 먼저 `--dry-run` 으로 바뀔 내용만 볼 수 있다.

```bash
bash cert-init-cims-rocky10.2/service-cert.sh csp-node --dry-run     # 바뀔 행·경로 미리보기 (저장 없음)
bash cert-init-cims-rocky10.2/service-cert.sh csp-node               # 저장 + SIGUSR1 + CSP TLS verify (TLS 행 5061 이 이미 있다)
```

TLS 행이 아직 없으면 `--port <TLS 포트>` 를 붙이면 `access-tls` 행을 만든다. OAM 이 다른 주소면 `--oam https://<IP>:4419`.

> ⚠️ **콘솔에서 local_nodes 행을 지우고 다시 만들거나 포트를 옮기면 인증서 경로가 되돌아간다.** 테스트베드 자동
> 설치(`deployment/tb/steps/45-config.sh`)는 TLS 행에 자가서명 `…/oam/runtime/cert/csp-site.pem` 을 넣고, 새로 만든 행은
> 그 값이나 빈 값으로 시작한다(09-11 실측: 포트 정리 후 15061 이 다시 `CN=csp` 자가서명을 냄). 행을 손댄 뒤에는
> **`csp-node` 를 다시 돌리거나** `tls_cert_path`/`tls_key_path` 를 다시 적고, `verify` 로 발급자를 확인한다.

**콘솔(수동)** — `[패키지 설정] > csp > local_nodes` 의 `protocol=TLS` 행에 절대경로 저장:

```
tls_cert_path = /opt/cims-agent/modules/csp/runtime/cert/csp-chain.pem
tls_key_path  = /opt/cims-agent/modules/csp/runtime/cert/csp.key
```

## 4. 검증 — 단말 전에 서버측만으로 확정

```bash
bash cert-init-cims-rocky10.2/service-cert.sh verify --csp-port 5061     # --ip 121.161.164.49 는 묶음에서 읽는다
```

전부 PASS 여야 한다. "틀린 이름 거절" 항목은 서버가 신원 검사를 집행하는지 보는 음성 대조군이다.
**CSC 를 한 번 재시작한 뒤 다시 돌려** 발급자가 `CIMS Service CA` 로 유지되는지 본다 — 그룹 CA 로
되돌아가 있으면 SAN 부족이다. .45 에서 원격으로 돌려도 같다.

## 5. 단말

- 로그인 화면 서버 주소 `121.161.164.49`. APK 는 08-19 이후 빌드(그 전 빌드는 검증을 안 해 문제를 못 본다).
- 단말 시계 자동 동기(틀리면 유효기간 검사 실패).
- Windows 관제조작반: "서버 인증서 검증" 켬 + CA PEM 경로 = 묶음의 `cims-service-ca.crt`.

증상 판독:

| 증상 | 원인 축 |
|---|---|
| 로그인 즉시 실패 + CSC 로그에 `/idms/authreq` 없음 | **인증서** — §4 verify 로 확인 |
| 401/403 | 계정·비밀번호·Digest(인증서 아님) |
| 로그인·프로비저닝은 되는데 REGISTER 실패 | `local_nodes` 포트 ↔ CSC 프로비저닝 포트 불일치([initial_install.md §4.4](../user-manual/initial_install.md)) |
| TLS 로 바꾸면 등록 503 `PJSIP_TLS_ECERTVERIF` | CSP 인증서 미교체(§3 미저장) |

## 6. 원복

CSC: `/opt/cims-agent/modules/csc/runtime/cert/server.{crt,key}.bak.<시각>` 을 되돌리면 30초 안에
핫리로드된다. CSP: `local_nodes` 경로를 원래 값으로 되돌린다.
