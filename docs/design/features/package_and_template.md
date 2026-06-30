# 패키지 포맷 + config_template.json 스키마

> base 8 + 변종 4 = **12 tarball**, manifest.json 자동 생성, csc 의 `Packages.Dir` 직접 편집 노출.

모듈 패키지(`cims.sh pkg`) 가 생성하는 tarball 의 구조와 내부 메타데이터 스키마를 정의합니다.

## 1. tarball 구조

```
<module>-<version>.tar.gz
├── meta.json              ← 패키지 메타 (name/version/service)
├── config_template.json   ← UI 렌더링용 설정 스키마 (선택)
├── <module>/              ← 바이너리/리소스 (모듈 이름 디렉토리)
│   ├── bin/
│   ├── config/  (초기 샘플 설정)
│   └── ...
└── cims.sh                ← 공통 런처 스크립트
```

## 2. meta.json 스키마

```json
{
  "name": "csp",
  "version": "1.2.0",
  "description": "...",
  "build_date": "2026-04-21T10:44:54Z",
  "git_sha": "955aab8",
  "git_branch": "feature/...",
  "packaged_at": "2026-04-21T10:44:56Z",
  "packaged_by": "nex@host",
  "changelog": "...",

  "service": {
    "functions": [
      { "name": "volte", "desc": "VoLTE 호처리" },
      { "name": "ptt",   "desc": "PTT 호처리" },
      { "name": "ibcf",  "desc": "IBCF (IP-PBX 트렁크)" }
    ],
    "processes": ["CSP", "PSP", "ISP"]
  }
}
```

- `service.functions[]` — 모듈이 제공 가능한 기능 목록. `name` 은 머신명 (DB 저장), `desc` 는 표시용
- `service.processes[]` — 모듈이 지원하는 **프로세스 변종** (단일 코드베이스의 배포 프로필)
- 두 목록은 **독립적**. Admin 이 조합을 자유롭게 선택 (예: process=CSP + functions=[volte,ptt])

## 3. config_template.json 스키마

스칼라 설정과 컬렉션 설정을 모두 선언적으로 기술합니다.

```json
{
  "version": 1,
  "sections": [
    {
      "key": "sip",
      "title": "SIP",
      "description": "...",
      "fields": [
        {
          "key": "SipServer.Realm",
          "label": "Realm",
          "type": "string",
          "default": "csp",
          "required": true,
          "restart": true,
          "help": "...",
          "advanced": false
        }
      ]
    }
  ],
  "collections": [
    {
      "key": "listeners",
      "title": "SIP 리스너",
      "restart": false,
      "reload_hint": "SIGUSR1 로 즉시 rebind",
      "schema": {
        "primary_key": ["id"],
        "id_field": "id",
        "id_type": "uuid",
        "unique_keys": [["bind_ip","bind_port","protocol"]],
        "fields": [
          { "key": "id", "type": "string", "readonly": true, "auto": "uuid" },
          { "key": "name", "type": "string", "required": true },
          { "key": "bind_ip", "type": "string", "default": "0.0.0.0", "required": true },
          { "key": "bind_port", "type": "int", "default": 5060, "min": 1, "max": 65535 },
          { "key": "protocol", "type": "enum", "options": ["UDP","TCP","TLS","WS","WSS"] }
        ]
      },
      "storage": { "kind": "jsonl", "file": "config/listeners.jsonl" }
    }
  ]
}
```

### 3.1 필드 타입

| type | 입력 컴포넌트 | 저장 |
|---|---|---|
| `string` | text input | string |
| `int` | number input (min/max) | int |
| `bool` | checkbox | bool |
| `enum` | dropdown (options 사용) | string |
| `path` | text input (파일/디렉토리) | string |
| `password` | password input (마스킹) | string |

### 3.2 공통 속성

- `key` — 저장 키 (점표기 허용, 예: `SipServer.Realm`)
- `label` — 화면 표시
- `default` — 초기값
- `required` — 필수 여부
- `restart` — `true` 면 🔁 (재기동 필요), `false` 면 ⚡ (즉시 적용)
- `advanced` — 기본 숨김, "고급 필드" 토글 시 표시
- `help` / `reload_hint` — 사용자 힌트

### 3.3 Collection 전용

- `schema.primary_key` — 레코드 식별 키
- `schema.id_field` / `id_type` — auto 생성 ID 필드 (현재 `uuid` 만 지원)
- `schema.unique_keys` — 추가 unique 제약 (필드 조합)
- `schema.fields` — 각 행의 필드 정의 (위 "필드 타입" 그대로 사용)
- `storage.kind` — 현재 `jsonl` 만 지원. 다른 kind (`db_table`, `file_include`) 는 미래 확장
- `storage.file` — install_path 기준 상대 경로 (e.g. `config/listeners.jsonl`)

## 4. 빌드 & 패키징 흐름

CLI / 콘솔 / API 셋 다 동등. 자세한 워크플로우 (4단계 카드, ▶ 빌드 & 패키징
job, 🗑 정리, ⤓ 다운로드, manifest schema) 는 `build_and_packaging.md` 참고.
이 문서는 패키지 포맷 자체에 집중.

업로드 (배포 메뉴 `/deploy/packages`) 시 CSC 는 tarball 루트에서 `meta.json`
+ `config_template.json` 을 추출해 `cims_package.{meta_json,
config_template_json}` 컬럼에 저장. 파일은 `csc.json` 의 `Packages.Dir`
(default `<csc-root>/packages/`) 에 보관. 동일 (name, version) 재업로드는
`force=true` 로 덮어쓰기.

## 5. 버전 관리 원칙

- `config_template.json` 변경 = 패키지 버전 올리기
- 기존 배포된 deployment 의 config 는 자동 이관, 새 필드는 default, 제거된 필드는 무시 (CSP 가 읽지 않음)
- **빌드 시점 버전 결정**: 빌드 단계에서 `-v X.Y.Z` 로 모든 base pkg.json 일괄 동기화 후 `pkg --no-bump` 로 그 버전 그대로 산출. 변종 12종이 동일 버전을 받음 (drift 방지)

## 6. 현재 템플릿이 있는 모듈

| 모듈 | sections | collections | 변종 |
|---|---|---|---|
| csp | sip, roles, log, db | listeners, trunks, routes, acl | + psp / isp |
| cmp | network, rtp_pool, log | — | + pmp / imp |
| csc | network, tls, notify (Csp+Psp), log, db, **packages** | — | — |
| 그 외 (agent/console/cwrtc/phone/cspsim) | 없음 | 없음 | — |

> csc 의 `Packages.Dir` / `Packages.BackupDir` 는 카드의 ¹ 설정 모달
> "패키지 저장소" 그룹으로 사용자가 직접 편집 가능.

## 7. manifest.json

`cims.sh pkg` 끝에서 `build/dist/packages/manifest.json` 자동 생성 — 12 tarball 의 `{name, size, sha256, mtime}` + git/host/ts. `_self_sha256` 은 S6 immutability gate 의 SoT. 스키마와 사용처는 [`build_and_packaging.md` §5](./build_and_packaging.md) 참고.
