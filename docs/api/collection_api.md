# Collection API (Admin ↔ CSC ↔ Agent 프록시)

Deployment 의 jsonl 설정(리스너/트렁크/라우팅/ACL 등) 을 Admin Console 이 CRUD 하기 위한 경로. CSC 는 원본 저장소가 아니라 **Agent 로의 프록시**로 동작합니다.

```
Admin Browser ── HTTPS + JWT ──> CSC ── HTTPS + X-Agent-Token ──> Agent :9900
                                                                  ↓
                                                    install_path/config/*.jsonl
                                                                  +
                                                     SIGUSR1 → 대상 프로세스
```

**인증**: Admin JWT (Bearer). CSC 가 Agent 호출 시 사용하는 X-Agent-Token 은 자동 관리.

---

## GET /api/v1/deployments/{id}/collection/{name}

**path**:
- `id` — `agent_deployment.id`
- `name` — `config_template.collections[].key` (예: `listeners`, `trunks`, `routes`, `acl`)

**동작**:
1. deployment 조회 → agent ip_address/sync_port 획득
2. 템플릿에서 `collections[name].schema` 조회 (없으면 404 `collection_not_in_template`)
3. Agent `GET /collection?install_path=&name=` 호출
4. Agent 가 jsonl 파일 읽어 반환

**Response 200**
```json
{
  "records": [
    { "id": "d60bccef...", "name": "voip-udp", "bind_ip": "0.0.0.0", "bind_port": 5060, "protocol": "UDP", "service": "volte" }
  ],
  "schema": {
    "primary_key": ["id"],
    "fields": [ { "key": "id", "type": "string", ... }, ... ]
  }
}
```

**오류**
- 404 `deployment_not_found`
- 409 `not_installed` — deployment.install_path 가 없음 (install 먼저 필요)
- 404 `collection_not_in_template` — 해당 패키지 템플릿에 collection 정의 없음
- 502 `agent_proxy_failed` — Agent 에 연결 실패 / sync_port 미보고 상태

---

## PUT /api/v1/deployments/{id}/collection/{name}

Collection 전체 치환 (서버는 받은 `records` 배열로 jsonl 파일을 덮어씀).

**Request**
```json
{
  "records": [
    { "name": "voip-udp", "bind_ip": "0.0.0.0", "bind_port": 5060, "protocol": "UDP", "service": "volte" },
    { "name": "mcptt",    "bind_ip": "0.0.0.0", "bind_port": 25061, "protocol": "TCP", "service": "mcptt" }
  ],
  "signal": true
}
```

- 각 record 의 `id` 가 없으면 서버가 자동 생성 (16 hex char UUID)
- `signal` (default true): jsonl 저장 후 Agent 가 대상 프로세스에 SIGUSR1 전송

**검증**: 서버가 템플릿 schema 로 validation. 실패 시 400.

**Response 200**
```json
{ "ok": true, "count": 2, "signaled": [136579] }
```

- `signaled` — SIGUSR1 이 전달된 pid 목록. `install_path/run/*.pid` 를 찾아 전송

**Response 400 (validation)**
```json
{
  "error": "validation_failed",
  "details": [
    {"index": 0, "errors": ["protocol: must be one of ['UDP','TCP','TLS','WS','WSS']"]}
  ]
}
```

---

## Agent 측 엔드포인트 (참고)

CSC 가 내부적으로 호출하지만, 디버깅 시 직접 호출 가능.

**Base**: `https://<agent_ip>:9900` (self-signed, TLS verify 생략)
**Auth**: `X-Agent-Token: <agent.agent_token>` (DB 에서 확인)

| Method | Path | Body/Query |
|---|---|---|
| GET | `/health` | — |
| GET | `/collection?install_path=&name=` | — |
| PUT | `/collection?install_path=&name=` | `{records, signal?}` |
| POST | `/signal?install_path=&sig=usr1\|hup` | — |

---

## 반영 흐름 (예: Listener 추가)

1. Admin Console 에서 Deployment 의 "설정 → 리스너" 탭 선택
2. "+ 추가" → 새 행 편집 → 저장
3. UI → `PUT /api/v1/deployments/{id}/collection/listeners`
4. CSC: 템플릿 schema 로 검증 → UUID 부여 → Agent 에 PUT
5. Agent: `install_path/config/listeners.jsonl` 원자 쓰기 → SIGUSR1 → CSP
6. CSP: `CspConfigCache::ReloadFromJsonl()` → `CspListenerManager::Sync()` → `AddUdpListener()` 호출 → 새 포트 bind
