# 테스트베드 media02(121.161.164.49) 단말 대면 인증서 — 현장 runbook

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
| 묶음 | `/home/cims/certs/service-cert-media02.tgz` (600, 키 포함 — scp/USB 로만 반입) |
| 발급 | 2026-09-10, leaf 만료 2028-09-09, 발급자 `CN=CIMS Service CA` |
| SAN | `DNS:media02, IP:127.0.0.1, IP:121.161.164.49, IP:10.0.2.49` (+ `DNS:csc.cims.local` / `DNS:csp.cims.local`) |
| 안에 든 것 | `csc/server.{crt,key}` · `csp/csp-chain.pem,csp.key` · `cims-service-ca.crt` · `service-cert.sh` · `README.txt` |

묶음 디렉터리 `/home/cims/certs/service-cert-media02/` 의 `service-cert.sh verify` 는 .45 에서도 원격 검증에 쓸 수 있다.

## 1. 팀원(설치 담당)에게 미리 확인·요청할 것

| 항목 | 이유 |
|---|---|
| OAM 접속 주소(`AgentOamUrl`)를 `121.161.164.49` 또는 `10.0.2.49` 로 | 둘은 SAN 에 있다. 다른 이름·VIP 를 쓰면 SAN 부족으로 `install` 이 거절한다 → .45 에서 재발급 1회 |
| HA/VIP 미사용 확인 | VIP 는 SAN 에 없다. 쓰면 재발급 |
| CSP `local_nodes` TLS 행의 `bind_port` (관례 5061 또는 15061) | `verify --csp-port` 값. CSC 자동 프로비저닝 `TLS 포트`도 같은 값이어야 단말에 TLS 가 광고된다 |
| 사내 단말 가입자·로그인 계정 프로비저닝 | CSC DB 가 별개라 .45 계정이 없다 |
| **CSC 가 기동된 시점**을 알려달라 | 그때 `install` 을 돌린다(CSC 가 이미 떠 있어야 핫리로드 확인이 된다) |
| 콘솔 `local_nodes` 편집 권한 | §3 값 저장을 누가 할지 |
| `10.0.2.49` 가 ctrl02(.46)의 보조 주소로도 잡혀 있다 | 같은 `10.0.2.0/24` 중복. media02 가 그 주소를 쓰면 충돌 가능 — 설치 전에 정리 |

## 2. 인증서 배치 — CSC 설치·기동이 끝난 직후, media02 에서 `cims` 계정으로

```bash
tar xzf service-cert-media02.tgz
bash service-cert-media02/service-cert.sh install
```

출력 판독:

| 출력 | 뜻 |
|---|---|
| `[OK] SAN 대조 통과` | 이 노드가 요구하는 SAN 을 다 담고 있다. 재기동에도 덮이지 않는다 |
| `[INFO] 기존 CSC 인증서 백업: server.{crt,key}.bak.<시각>` | 원복 지점 |
| `[OK] CSC 가 새 인증서를 서빙한다` | 핫리로드 완료(재시작 불필요) |
| `[WARN] CSC 핫리로드 미반영` | 콘솔에서 CSC 재시작 요청 → 뒤에 verify |
| `[ERROR] … SAN 에 이 노드가 요구하는 항목이 없다` (exit 2) | 배치하지 않았다. 출력된 `issue … --force` 명령을 .45 에서 그대로 실행 → 새 tgz 반입 → 다시 install |

CSP 파일은 `/opt/cims-agent/modules/csp/runtime/cert/` 에 놓인다. §3 이 있어야 CSP 가 쓴다.

## 3. 콘솔 `local_nodes` TLS 행 — 절대경로 저장 (저장 시 SIGUSR1 무중단 반영)

```
tls_cert_path = /opt/cims-agent/modules/csp/runtime/cert/csp-chain.pem
tls_key_path  = /opt/cims-agent/modules/csp/runtime/cert/csp.key
```

## 4. 검증 — 단말 전에 서버측만으로 확정

```bash
bash service-cert-media02/service-cert.sh verify --ip 121.161.164.49 --csc-port 4430 --csp-port <bind_port>
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
