# CIMS DB 부트스트랩 (db-bootstrap)

CIMS 최초 구축 시 **DB / 앱 계정 / 스키마**를 한 번에 생성하는 대화식 도구.
`cims-bootstrap`(OAM+Console+Agent 운영평면 인스톨러)와 짝을 이룬다.

## 무엇을 만드나

- 데이터베이스 `cims` (utf8mb4)
- 앱(서비스) 계정 + 해당 DB 전권 GRANT
- 통합 스키마 `cims_schema.sql` — **8개 테이블**:
  `organizations`, `users`(가입자 person 전용), `volte_subscriptions`,
  `ptt_subscriptions`, `user_rejects`, `ptt_groups`, `ptt_group_members`,
  `ptt_affiliations`
- (선택 `--cleanup`) 기존 DB 의 미사용 테이블 16종 정리

> **DB 에 두지 않는 것** — 콘솔 로그인 계정(OAM users)은 OAM file_store 도메인
> `console_accounts` 에서 관리한다. 운영 런타임(agents/ha_groups/packages/
> deployments/call_logs/recordings/csp_runtime/idms 토큰 등)은 전부 file_store.

## 실행

대화식 (실행 과정에서 접속정보 입력):
```bash
python3 db_bootstrap.py
```

비대화식 (자동화):
```bash
python3 db_bootstrap.py --yes \
    --host 127.0.0.1 --port 3306 \
    --admin-user root --admin-pass '****' \
    --db cims --app-user cims --app-pass '****' --grant-host '%'
```

기존 DB 정리까지:
```bash
python3 db_bootstrap.py --yes ... --cleanup
```

환경변수(플래그 미지정 시 기본값): `CIMS_DB_HOST` `CIMS_DB_PORT`
`CIMS_DB_ADMIN_USER` `CIMS_DB_ADMIN_PASS` `CIMS_DB_NAME`
`CIMS_DB_APP_USER` `CIMS_DB_APP_PASS` `CIMS_DB_GRANT_HOST`.

## 특성

- **air-gapped**: 시스템 mysql 클라이언트 불필요. vendored pymysql(순수 파이썬)을
  `vendor/` 동봉본 → `oam/vendor` → `csc/vendor` → 시스템 순으로 자동 탐색.
- **멱등**: 스키마 `CREATE ... IF NOT EXISTS`, DB/계정 존재 시 건너뜀 → 재실행 안전.
- 관리자 계정은 `CREATE DATABASE`/`CREATE USER`/`GRANT` 권한 필요 (보통 root).

## 관련 파일 (sql/)

- `cims_schema.sql` — 통합 스키마 (SoT)
- `migrate_drop_unused_tables.sql` — 미사용 16 테이블 정리 (`--cleanup` 가 사용)
- `migrate_users_person_only.sql` — 기존 DB 의 users 를 person 전용으로 (login_id/password/role 컬럼 제거)
- `export_console_accounts.py` — 컬럼 제거 전, DB 의 콘솔 계정(role≠user)을 OAM console_accounts(file)로 이관
