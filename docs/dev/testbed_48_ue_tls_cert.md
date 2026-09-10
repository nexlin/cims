# 테스트베드(.48) 단말 대면 TLS 인증서 — Service CA 발급 절차

> 개발·시험 환경 운영 메모. 단말(Android CIMS 통합 로그인·VoLTE·PTT 앱, Windows 관제조작반)이
> .48 테스트베드에 로그인하지 못하는 원인과, .45 에 있는 **CIMS Service CA** 로 .48 인증서를 발급해
> 해소하는 절차. PKI 설계 정본은 [sip_tls_signaling.md §8](../design/features/sip_tls_signaling.md#8-인증서-운영).
> 절차가 끝나고 §5 과제가 정리되면 이 문서는 삭제한다.

## 1. 현상과 원인

단말이 .48(`121.161.164.48:4430`) 로 로그인하면 실패하고, CSC 로그(`mgmt-server/csc/current/log/csc.log`)에
`/idms/authreq` 요청이 **한 건도 남지 않는다** — TLS 핸드셰이크에서 끊긴다.

| 항목 | 상태 |
|---|---|
| 단말 신뢰 앵커 | APK 동봉 `CimsTrustStore.CA_BUNDLE` = **CIMS Service CA** 한 장 (`C=KR, O=CIMS, CN=CIMS Service CA`, RSA 4096, 2026-08-18 ~ 2036-08-15). OS 신뢰 저장소·trust-all 은 쓰지 않는다(`core/net/CimsTls`) |
| .45 CSC 4430 인증서 | Service CA 발급 leaf `CN=121.161.164.45` (SAN `121.161.164.45, 10.0.2.45, 127.0.0.1, ctrl01, csc.cims.local`, ~2028-08-18) + CA 체인. 위치 `/opt/cims-agent/modules/csc/runtime/cert/server.{crt,key}` → **단말 접속 가능** |
| .48 CSC 4430 인증서 | `CIMS-OAM-CA` 발급 leaf `CN=media01` (SAN `media01, 127.0.0.1, 121.161.164.48, 10.0.2.48, 192.168.0.82`). 배포 시 agent `ensure_node_cert`(`agent/lib/cert.sh`) 가 관리평면 그룹 CA 로 자동 발급해 `build/dist/mgmt-server/csc/runtime/cert/` 에 넣은 것 → 발급자가 달라 **단말이 거절** |
| .48 CSP 15061 인증서 | 패키지 동봉 자가서명 `CN=csp` (`cert/csp.pem`) → TLS 접속 단말은 역시 거절. 현재 프로비저닝 transport 가 UDP 라 등록에는 영향 없음 |
| .48 에 Service CA | **없다** — 파일시스템 전체 대조 결과 Service CA 발급 인증서·CA 키 모두 부재 |
| Windows 관제조작반 | .45·.48 **모두 로그인 가능** — 로그인 창 "서버 인증서 검증" 체크를 꺼 둔 상태(`%APPDATA%` settings.json `CscVerifyServer=false`). 꺼지면 SDK `OpenSslTransport` 가 `SSL_VERIFY_PEER` 를 걸지 않아 어떤 인증서든 수락한다. 켜 두고 `TlsCaPemPath` 가 비면 OpenSSL 시스템 신뢰 경로만 보므로 사설 CA 인 .45 도 실패한다. 시험용 예외이며, 운영은 검증 켬 + `TlsCaPemPath`=Service CA PEM |
| Android 앱 | 검증 스위치가 **없다** — `CimsTls` 가 APK 동봉 Service CA 한 장으로 항상 검증(호스트명 검사 포함, trust-all 경로 제거됨). 설계가 의도한 동작이라 .45 만 통과 |

즉 도메인/realm 전환([dev_test_domain_realm.md](dev_test_domain_realm.md))과는 무관하며, SIP 를 UDP 로
바꿔도 풀리지 않는다(막힌 채널은 HTTPS 4430 이고 앱 `baseUrl` 은 `https://` 고정, CSC 는 `runtime/cert` 가
있으면 항상 HTTPS). 콘솔에는 인증서 열람·교체 기능이 없다(agent 가 만료일만 보고 → `cert_expiring` 알람).

**임시 운용**: 단말은 .45 로 접속한다. .48 은 cspsim·cimsue-cli·Windows 관제조작반(`VerifyTls` 끄기 가능) 시험용.

## 2. 필요한 것 — Service CA 개인키

.45 의 `runtime/cert/server.key` 는 **leaf 의 키**(RSA 2048)라 서명에 쓸 수 없다. CA 키(RSA 4096, PEM 약 3.2KB)를
.45 에서 찾는다. 후보: `/opt/cims-agent/modules/csp/runtime/cert/`, `…/csp/current/csp/cert/`, 2026-08-18~19 에
작업한 홈 디렉터리, 이름은 대개 `cims-service-ca.key`.

```bash
# .45 — CA 인증서 분리
awk '/BEGIN CERT/{n++} n==2' /opt/cims-agent/modules/csc/runtime/cert/server.crt > /tmp/ca.crt
openssl x509 -in /tmp/ca.crt -noout -subject          # CN=CIMS Service CA 여야 한다

# 4096 비트 키 후보를 크기로 걸러 공개키 대조
sudo find / -xdev -type f \( -name '*.key' -o -name '*.pem' \) -size +3k 2>/dev/null \
  | while read f; do
      diff -q <(openssl x509 -in /tmp/ca.crt -noout -pubkey) <(openssl pkey -in "$f" -pubout 2>/dev/null) >/dev/null \
        && echo "CA KEY: $f"
    done
```

CA 키가 어디에도 없으면 §5-B(새 CA + APK 재빌드)로 간다.

## 3. 발급·배치·검증 — `scripts/service-cert.sh`

절차 정본은 [sip_tls_signaling.md §8.3](../design/features/sip_tls_signaling.md#83-발급배치-절차--새-노드는-scriptsservice-certsh-로).
.48 은 개발 레이아웃(`build/dist/mgmt-server`)이라 `--prefix` 를 준다. CA 키는 .45 를 떠나지 않는다.

```bash
# .48 — 필요한 SAN (hostname media01 · 121.161.164.48 · 10.0.2.48 · 127.0.0.1 …)
ssh cims@121.161.164.48 'bash -s -- collect --prefix /home/cims/work/cims/build/dist/mgmt-server' < scripts/service-cert.sh

# .45 — 발급 (collect 가 출력한 HOST/SAN 그대로)
scripts/service-cert.sh issue --host media01 --san "<SAN>"

# .45 → .48 — 전달 + 배치(CSC 백업·교체·핫리로드 확인, CSP runtime/cert 배치) + 검증. csp-port 는
# local_nodes 저장 전이면 0 으로 두고 저장 후 verify 를 다시 돌린다.
scripts/service-cert.sh push --ssh cims@121.161.164.48 --bundle ./service-cert-media01 \
    --prefix /home/cims/work/cims/build/dist/mgmt-server --csp-port 0
```

## 4. .48 CSP `local_nodes` 저장·최종 검증

콘솔(.48 OAM) `local_nodes` 의 `access-tls` 행: `tls_cert_path` =
`/home/cims/work/cims/build/dist/mgmt-server/csp/runtime/cert/csp-chain.pem`, `tls_key_path` =
`…/csp/runtime/cert/csp.key`(지금은 상대경로 `cert/csp.pem` 이라 절대경로로 바꾼다). 저장 시 SIGUSR1 무중단 교체.

```bash
scripts/service-cert.sh verify --ip 121.161.164.48 --csc-port 4430 --csp-port 15061   # 전부 PASS
```

CSC 를 한 번 재시작한 뒤 verify 를 다시 돌려 발급자가 `CIMS Service CA` 로 유지되는지 본다. 그 뒤
단말 로그인. 가입자별 `sip_transport=TLS` override 4 건(volte/ptt 각 `+821310001001/2`,
`+82510001001/2`)은 CSP 인증서 교체가 끝나면 다시 TLS 로 둘 수 있다.

## 5. 남은 과제

- **A. 구조** — agent 자동 발급이 관리평면 그룹 CA(`CIMS-OAM-CA`)를 쓰는 한 새 노드 배포마다 이 불일치가
  재현된다. §8.1 이 남긴 과제대로 그룹 CA 를 Service CA 로 통일하거나, `ensure_node_cert` 가 단말 대면 모듈
  (csc·csp)에는 Service CA 로 발급하도록 CA 를 분리 지정해야 한다.
- **B. CA 키 분실 시** — .48 에서 새 Service CA 를 만들어 발급하고, `CimsTrustStore.CA_BUNDLE` 에 새 CA 를
  **추가**한 APK 를 먼저 배포(구·신 병기) → 서버 교체 → 다음 배포에서 구 CA 제거(§8.5 CA 교체 절차).
- **C.** .45 CSC leaf 개인키가 대화 기록에 노출됐다(2026-09-08). 다음 .45 인증서 갱신 때 키를 함께 교체한다.
