# 현장 테스트베드(192.168.50.100) — 인증서·단말 시험 점검표

> 개발·시험 환경 운영 메모. 09-11 사내 리허설(`.49`, hostname `cims-rocky10.2`)에서 설치 → 단말 등록 →
> PTT/VoLTE 호시험까지 완주하며 걸린 것을 현장 기준으로 정리한 것이다. 설치 절차 자체는 팀원의
> [deployment/tb/TB-INSTALL.txt](../../deployment/tb/TB-INSTALL.txt) 가 정본이고, 여기는 **인증서 담당자와
> 서비스 확인 담당자가 현장에서 보는 순서와 함정**만 담는다. 인증서 절차 정본은
> [sip_tls_signaling.md §8.3](../design/features/sip_tls_signaling.md#83-발급배치-절차--사이트-ca-는-개발사에서-1회-서버-leaf-는-현장에서).

## 0. 현장 조건

| 항목 | 값 | 비고 |
|---|---|---|
| 서버 주소 | **192.168.50.100** | 사설 LAN. 단말도 같은 LAN 이면 NAT 없음 |
| CA 보관 서버 | `.45` (`/home/cims/certs/`) | 현장에서 닿지 않는다고 가정 → **묶음은 출발 전에 발급** |
| 단말 | 현장 단말에 **우리 APK 3종**(cims·volte·ptt) 설치 | APK 에 Service CA 동봉 — 서버 인증서만 맞으면 붙는다 |
| hostname | 팀원이 설치 전에 확정 | 묶음 SAN 에 `DNS:<hostname>` 이 들어간다. 현장에서 바꾸면 재발급 |

## 1. 출발 전 (.45)

1. 팀원에게 확정받을 것: 서버 hostname, 서버가 가질 IPv4 전부(192.168.50.100 외 관리망 등), HA/VIP 여부,
   SIP 포트 계획(UDP/TCP/TLS), 콘솔 admin 비밀번호 보유자, 단말 계정·시험 그룹 시드(`data/*.csv`).
2. 묶음 발급 (현장에서 `.45` 로 ssh 가 안 되므로 수동 인자). 스크립트가 `DNS:<host>`·`IP:127.0.0.1` 은 빠지면 채운다.
   ```bash
   scripts/service-cert.sh issue --host <hostname> --san "IP:192.168.50.100[,IP:<기타 IP>]" \
       --runbook docs/dev/testbed_field_runbook.md
   # → /home/cims/certs/cert-init-<hostname>.tgz  (600, 키 포함 — USB 로 반입, 채팅·메일 금지)
   ```
   hostname 을 미리 알 수 없으면 현장에서 `collect` 결과를 받아 발급해야 하므로 한 번 왕복이 생긴다.
   `--site` 없이 발급하면 **루트 직서명(체인 1장, 임시)** 이고 스크립트가 경고를 낸다 — 임시 사이트 배포 단계의
   정식 형태다. 현장 노드를 자동 갱신 대상으로 옮기는 것은 설치 뒤 별도다: 노드의 그룹 CA 인증서
   (`<oam>/runtime/_secrets/ca/ca.crt`)를 가져와 `.45` 루트로 `site-ca sign --cross` → `ca-cross.crt` 를 되가져가
   배치(§8.6.4, 무중단) — 왕복 1회.
3. 반입물: `cert-init-<hostname>.tgz`, APK 3종(사내 세 단말과 같은 md5 = 서버 빌드), Windows 관제조작반을 쓰면
   `cims-service-ca.crt`(묶음 안에 있음).

## 2. 설치 담당(팀원)에게 전달할 리허설 교훈 — 설치 기본값이 `.45` 운영값과 다른 곳

`.49` 에서 순서대로 걸렸다. 전부 **설치 기본값** 문제라 현장에서도 그대로 재현된다. 설치 도구
(`deployment/tb/`)는 팀원 소유라 여기서는 고치지 않고 **전달 목록**으로만 둔다 — 도구에 반영할지,
설치 후 콘솔에서 고칠지는 팀원이 정한다. 아래 표는 그 전달 내용이다. 설치 도구 문서
(TB-INSTALL.txt 4-8)에는 `service-cert.sh` 의 옛 이름(`service-cert-<HOST>.tgz`)과 콘솔 수동 저장 단계가
남아 있는데, 현재 스크립트는 묶음 이름이 `cert-init-<host>` 이고 (4) 단계를 `csp-node` 로 대신할 수 있다 —
이것도 전달 항목이다.

| # | 증상 | 원인 | 조치 |
|---|---|---|---|
| 1 | 단말 로그인 즉시 실패, CSC 로그 무흔적 (`CertPathValidatorException`) | CSC 4430·CSP TLS 인증서가 Service CA 발급이 아님 | §3 인증서 절차 (TB-INSTALL 4-8) |
| 2 | 그룹콜 개시자가 0.1초 만에 튕김 (CSP `488`, `sdp parse error`) | TCP 가 UDP 와 **다른 포트**(25061) → pjsip 이 1300B 초과 INVITE 를 같은 포트 TCP 로 승격하다 실패 → UDP 폴백 시 Content-Length 오기 → SDP 절단 | **`TB_SIP_TCP_PORT` = `TB_SIP_UDP_PORT`** (같은 포트 공용, `.45` 는 15060/15060/15061), CSC 프로비저닝 `tcp_port 0` |
| 3 | 그룹콜 참여는 되는데 PTT 무반응 / VoLTE 연결되나 무음 | `access_services` `media_nat_mode=off`(기본) → CMP 가 SDP 사설 IP 와 다른 소스의 RTP·floor 를 폐기 | `media_nat_mode=auto` (mcptt 는 `latch_ip_guard=off`, volte 는 `strict` — `.45` 동일). 같은 LAN 이면 off 도 동작하지만 단말이 와이파이 NAT 뒤면 필수 |
| 4 | CSP 기동 로그 `TcpConnect(<IP>:4421) error`, xcap-root 폴백 | CSC overlay `Server.Ip=127.0.0.1` → 내부 API 4421 이 loopback 만, CSP 는 서버 IP 로 다이얼 | `csc Server.Ip=0.0.0.0` (또는 CSP `Setup.Csc.Host=127.0.0.1`) |
| 5 | CSP `Illegal mix of collations` (역할 로드 실패) | `role_assignments`(general_ci) vs `*_subscriptions`(unicode_ci) | DB 시드 스키마 collation 통일 |
| 6 | 요청마다 2초 대기, UDP 큐 적체 | CSP `Setup.Csc.Host` 가 다른 서버(리허설에선 10.0.2.46) | 서버 자기 IP 확인 |
| 7 | 포트 변경 후 단말이 못 붙음 | firewalld 에 새 포트 미개방 / TLS 행 재생성으로 인증서 경로가 `csp-site.pem` 으로 회귀 | `tools/tb-firewall.sh` 재실행 / `csp-node` 재실행 |

## 3. 현장 순서 (인증서 담당)

TB-INSTALL 4-7(CSC 기동)까지 끝난 뒤. 묶음 안 `README.txt` 0~6 단계와 같다.

```bash
tar xzf cert-init-<hostname>.tgz
bash cert-init-<hostname>/service-cert.sh install                # SAN 대조 통과 · CSC 핫리로드
bash cert-init-<hostname>/service-cert.sh csp-node --dry-run     # TLS 행 갱신 미리보기
bash cert-init-<hostname>/service-cert.sh csp-node               # admin 비밀번호 → 저장 + verify
bash cert-init-<hostname>/service-cert.sh verify --csp-port <TLS 포트>
```
- `install` 이 SAN 부족으로 거절하면 hostname/IP 가 발급 때와 다른 것이다. 출력된 SAN 으로 `.45` 에서 재발급(왕복).
- 콘솔에서 CSC 를 한 번 재시작하고 `verify` 를 다시 돌려 발급자가 유지되는지 본다.
- 포트를 바꾸거나 local_nodes 행을 다시 만들었으면 `csp-node` 를 다시 돌린다(§2-7).

## 4. 단말 (현장 단말 + 우리 APK)

1. APK 3종 설치(사내 표준 셋업: `install -r -g` → 오버레이 권한 → deviceidle 허용 → PTT 접근성 서비스).
2. **시계.** 폐쇄망이면 NTP 가 없다. 단말 시각이 틀리면 인증서 유효기간 검사에서 로그인이 실패한다
   (`CertPathValidatorException`, 서버 무흔적). 날짜·시간을 수동으로 맞추고 자동 동기는 끈다.
3. 로그인 서버 주소 `192.168.50.100`, 계정은 팀원이 시드한 것. 로그인 → 프로비저닝 → REGISTER 200 → PTT 그룹콜(음성) → VoLTE(음성).
4. Windows 관제조작반: "서버 인증서 검증" 켬 + CA PEM 경로 = 묶음의 `cims-service-ca.crt`.

증상 판독

| 증상 | 축 | 어디를 보나 |
|---|---|---|
| 로그인 즉시 실패, CSC 로그에 `/idms/` 없음 | 인증서 또는 단말 시계 | `verify`; 단말 날짜 |
| 401/403 | 계정·비밀번호 | CSC 시드 |
| 로그인 OK, REGISTER 실패 | 포트 불일치 | CSC 프로비저닝 포트 ↔ CSP local_nodes, firewalld |
| 그룹콜 즉시 종료(488) | TCP 포트 분리 | §2-2 |
| 참여 OK, PTT 무반응 / VoLTE 무음 | NAT 정책 | §2-3, CMP 로그 `drop rtp` |

## 5. 로그 위치 (배포본)

| 모듈 | 파일 |
|---|---|
| CSP | `/opt/cims-agent/modules/csp/<ver>/csp/log/csp_<날짜>_N.log` (`current/log/csp.log` 는 비어 있음) |
| CMP | `/opt/cims-agent/modules/cmp/current/cmp/log/cmp.log` |
| CSC | `/opt/cims-agent/modules/csc/current/log/csc.log` (uvicorn 접근 로그, 시각 없음) |
