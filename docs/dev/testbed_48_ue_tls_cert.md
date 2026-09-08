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

## 3. .48 용 인증서 발급 (.45 에서 — CA 키는 .45 를 떠나지 않는다)

SAN 은 agent 자동 발급 체계가 요구하는 목록(`DNS:<hostname>`, `IP:127.0.0.1`, 노드 IPv4 전부)의 **상위집합**이어야
한다. `ensure_node_cert` 는 `O=CIMS` 인증서를 CIMS 관리 인증서로 보고 SAN 만 검사하므로, 부족하지 않으면
재기동마다 유지되고 부족하면 그룹 CA 인증서로 **덮어쓴다**(`agent/lib/cert.sh` `_cert_san_missing`).

```bash
umask 077; mkdir -p ~/cert48 && cd ~/cert48
openssl req -newkey rsa:2048 -sha256 -nodes -keyout server.key -out csc48.csr \
  -subj "/C=KR/O=CIMS/CN=121.161.164.48"
cat > ext.cnf <<'EXT'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:121.161.164.48,IP:10.0.2.48,IP:192.168.0.82,IP:127.0.0.1,DNS:media01,DNS:csc.cims.local
EXT
openssl x509 -req -in csc48.csr -CA /tmp/ca.crt -CAkey <CA 키 경로> \
  -CAcreateserial -days 730 -sha256 -extfile ext.cnf -out csc48.crt
cat csc48.crt /tmp/ca.crt > server.crt                 # 체인 PEM (leaf + CA)
openssl verify -CAfile /tmp/ca.crt csc48.crt           # OK
diff <(openssl x509 -in csc48.crt -noout -modulus) <(openssl rsa -in server.key -noout -modulus) && echo key-match
scp server.crt server.key cims@121.161.164.48:/home/cims/work/cims/build/dist/mgmt-server/csc/runtime/cert/
```

CSP 15061 도 TLS 단말을 받으려면 같은 CA 로 한 장 더 발급해(`CN=121.161.164.48`, SAN 동일) 배포 #34 의
`local_nodes` `access-tls` 의 `tls_cert_path`/`tls_key_path` 를 그 파일로 바꾸고 SIGUSR1(무중단 교체, §8.4).

## 4. .48 배치·재시작·검증

```bash
# .48
chmod 600 build/dist/mgmt-server/csc/runtime/cert/server.key
# CSC 재시작 — 콘솔 관리 > 배포 > #31 CSC > 재시작, 또는
TOK=$(curl -sk -X POST https://127.0.0.1:4419/api/v1/auth/login -H 'Content-Type: application/json' \
      -d '{"login_id":"admin","password":"<콘솔 admin 비밀번호>"}' | python3 -c 'import json,sys;print(json.load(sys.stdin)["token"])')
curl -sk -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
     -d '{"job_type":"restart"}' https://127.0.0.1:4419/api/v1/deployments/31/job
# job 상태: GET /api/v1/agents/13/jobs/<job_id>

# 검증 — 체인 2장, CA 검증 + IP 신원 통과, 틀린 이름은 실패해야 정상
openssl s_client -connect 121.161.164.48:4430 -showcerts </dev/null 2>/dev/null | grep -c 'BEGIN CERTIFICATE'
openssl s_client -connect 121.161.164.48:4430 -CAfile /tmp/ca.crt -verify_return_error -verify_ip 121.161.164.48 -brief </dev/null
openssl s_client -connect 121.161.164.48:4430 -CAfile /tmp/ca.crt -verify_return_error -verify_hostname wrong.example -brief </dev/null
grep -h 'SSL Enabled' build/dist/mgmt-server/csc/current/log/csc.log | tail -1
```

그 뒤 단말 로그인. 도메인·IdMS issuer 는 이미 새 규약([dev_test_domain_realm.md](dev_test_domain_realm.md) §4)이라
프로비저닝은 그대로 받아진다. 가입자별 `sip_transport=TLS` override 가 있는 4 건(volte/ptt 각 `+821310001001/2`,
`+82510001001/2`, user_id 5020·5021)은 CSP 15061 인증서를 교체하기 전까지 UDP 로 비워 둔다(콘솔 가입자 편집).

## 5. 남은 과제

- **A. 구조** — agent 자동 발급이 관리평면 그룹 CA(`CIMS-OAM-CA`)를 쓰는 한 새 노드 배포마다 이 불일치가
  재현된다. §8.1 이 남긴 과제대로 그룹 CA 를 Service CA 로 통일하거나, `ensure_node_cert` 가 단말 대면 모듈
  (csc·csp)에는 Service CA 로 발급하도록 CA 를 분리 지정해야 한다.
- **B. CA 키 분실 시** — .48 에서 새 Service CA 를 만들어 발급하고, `CimsTrustStore.CA_BUNDLE` 에 새 CA 를
  **추가**한 APK 를 먼저 배포(구·신 병기) → 서버 교체 → 다음 배포에서 구 CA 제거(§8.5 CA 교체 절차).
- **C.** .45 CSC leaf 개인키가 대화 기록에 노출됐다(2026-09-08). 다음 .45 인증서 갱신 때 키를 함께 교체한다.
