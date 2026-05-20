---
name: csc 소스 변경 시 dist sync + 양쪽 csc 재시작
description: csc/src 변경 시 build/dist 동기화와 csc/tb-csc 둘 다 재시작이 필요한 환경 트랩
type: feedback
originSessionId: 22f65774-47fd-42b9-aa56-f1b875d3ac93
---
CSC 핸들러/모듈을 `csc/src/` 에서 수정한 뒤 동작이 반영 안 되면, 거의 항상
**dist 동기화 또는 옛 인스턴스 잔존** 이 원인이다.

## 운영 절차

```bash
./cims.sh sync csc        # csc/src/ → build/dist/csc/src/ rsync
./cims.sh restart csc     # 4421 csc 재시작
./cims.sh restart tb-csc  # 4419 TB-CSC — TB-Console(3000) 사용 시 필수
```

## Why

- `cims.sh restart csc` 가 띄우는 csc 는 `build/dist/csc/src/csc_app.py`
  (= dist 의 복제본). 소스 트리는 직접 안 띄움. → sync 안 하면 옛 코드.
- TB-Console(`192.168.199.129:3000`, vite mode=tb) 의 proxy 가
  `cims-console/.env.tb.local` 의 `VITE_ADMIN_TARGET=https://127.0.0.1:4419`
  → **TB-CSC** 로 향함. 그래서 일반 csc 만 재시작하면 dev-console 에서는
  여전히 옛 핸들러가 응답함 (404 등).
- TB-CSC 와 CSC 는 **같은 dist** 의 csc_app.py 를 띄우지만 띄운 시점이
  다르면 메모리 안의 코드가 다르다. sync 후 둘 다 재시작 필수.

## How to apply

- csc 핸들러/라우트 추가·수정·삭제 시: 위 3 명령 모두 실행 후 검증.
- 콘솔 dev 에서 `/api/v1/<x>` 가 404 이면 먼저 어느 csc 가 응답하는지
  포트로 의심 (3000 → 4419 / 직접 → 4421). `ss -tlnp | grep ':441[0-9]'`.
- search-up 패턴 사용: csc 가 dist 안에서 띄워질 때 build/실행 cwd 를
  실제 소스 루트로 잡으려면 `cims.sh + CMakeLists.txt` 가 함께 있는
  부모 디렉토리를 6단계까지 탐색 (verification.py / build.py 의 init).
- mcptt 4430 은 TB-CSC 와 CSC 가 같은 포트를 노린다 — 한쪽이 점유 중
  이면 다른 쪽 mcptt 만 죽고 admin(4421/4419) 은 정상 동작. 무시 가능.

## 관련 커밋
- `14c36c1` — build.py init 의 search-up 패턴 적용
