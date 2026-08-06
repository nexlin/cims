// ems/service/console/node_modules → ../../core/console/node_modules 링크 보증.
//
// service/console 은 자체 node_modules 없이 core/console 의 것을 심볼릭 링크로
// 공유한다 (jsx: react-jsx 가 각 소스 파일 위치 기준으로 react/jsx-runtime 을
// 상향 탐색하므로, service/console/src 위쪽에 react 가 보여야 tsc 가 통과).
// 링크는 git 에 추적되지만(120000) Windows 체크아웃(symlink 미지원 모드)·zip 복사
// 등에서 일반 파일로 변질되거나 유실된다 → build/dev 앞에서 항상 자가 복구.
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const coreModules = path.resolve(here, '..', 'node_modules')
const svcDir = path.resolve(here, '..', '..', '..', 'service', 'console')
const link = path.join(svcDir, 'node_modules')
const target = path.join('..', '..', 'core', 'console', 'node_modules')

if (!fs.existsSync(svcDir)) process.exit(0)          // svc 팩 없는 배포본 — 무시
if (!fs.existsSync(coreModules)) {
  console.error('[ensure-svc-modules] core/console/node_modules 없음 — npm install 먼저')
  process.exit(0)                                    // tsc 가 어차피 실패 — 여기서 막지 않음
}

let ok = false
try {
  const st = fs.lstatSync(link)
  if (st.isSymbolicLink()) {
    ok = fs.existsSync(link)                         // 링크 존재 + 대상 유효
    if (!ok) fs.unlinkSync(link)                     // dangling — 재생성
  } else {
    // Windows 체크아웃 변질(대상 문자열이 든 일반 파일) 또는 빈 디렉토리 잔재
    if (st.isDirectory()) fs.rmSync(link, { recursive: true, force: true })
    else fs.unlinkSync(link)
  }
} catch { /* 미존재 — 생성 */ }

if (!ok) {
  // Windows 는 dir symlink 에 권한이 필요할 수 있어 junction fallback
  try { fs.symlinkSync(target, link, 'dir') }
  catch { fs.symlinkSync(coreModules, link, 'junction') }
  console.log(`[ensure-svc-modules] 복구: ${link} → ${target}`)
}
