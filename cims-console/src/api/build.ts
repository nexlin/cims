import { api } from './client'

// ─────────────────────────────────────────────────────────────
// 빌드 / 패키지화 / 다운로드 API client — backend csc/src/handlers/build.py
// ─────────────────────────────────────────────────────────────

export type BuildVerdict = 'PASS' | 'FAIL'

export type BuildJobKind = 'build' | 'pkg' | 'release'

export interface BuildJobStart {
  job_id: string
  kind: BuildJobKind
  module?: string
  modules?: string[]
  argv: string[]
  started_at: number
  message: string
}

export interface BuildJobStatus {
  job_id: string
  kind: BuildJobKind
  label: string
  argv: string[]
  started_at: number
  ended_at: number | null
  elapsed: number
  done: boolean
  returncode: number | null
  verdict: BuildVerdict | null
  stdout_tail: string
}

export interface CleanResult {
  removed_tarballs: number
  removed_manifest: boolean
  errors: string[]
}

export interface ManifestPackage {
  name: string
  size: number
  sha256: string
  mtime: string
}

export interface ManifestResponse {
  ts?: string
  git?: { branch?: string; sha?: string }
  host?: string
  ens_ip?: string
  packages?: ManifestPackage[]
  _self_sha256?: string
}

export interface PackagesListResponse {
  manifest_present: boolean
  ts?: string
  git?: { branch?: string; sha?: string }
  host?: string
  packages: ManifestPackage[]
}

const BASE = '/build'

async function downloadBlob(path: string, fallbackName: string): Promise<void> {
  // build/dist/packages/<m>-<ver>.tar.gz 를 브라우저 다운로드로 흘려보냄.
  // 인증 토큰을 헤더로 실어야 하므로 fetch 직접 사용 (api.get 은 JSON 만 다룸).
  const token = localStorage.getItem('cims_token')
  const headers: Record<string, string> = {}
  if (token) headers['Authorization'] = `Bearer ${token}`
  const url = `/api/v1${path}`
  const res = await fetch(url, { headers })
  if (!res.ok) {
    let msg = `HTTP ${res.status}`
    try {
      const j = await res.json()
      if (typeof j?.error === 'string') msg = j.error
    } catch { /* ignore */ }
    throw new Error(msg)
  }
  const cd = res.headers.get('Content-Disposition') || ''
  const m = /filename="([^"]+)"/.exec(cd)
  const fname = m ? m[1] : fallbackName
  const blob = await res.blob()
  const objUrl = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = objUrl
  a.download = fname
  document.body.appendChild(a)
  a.click()
  document.body.removeChild(a)
  URL.revokeObjectURL(objUrl)
}

export const buildApi = {
  runBuild: (opts: { version?: string } = {}) => {
    const body: Record<string, unknown> = {}
    // version 명시 시 cims.sh build -v <ver> — 모든 컴포넌트의 pkg.json 갱신.
    if (opts.version && opts.version.trim()) body.version = opts.version.trim()
    return api.post<BuildJobStart>(`${BASE}/run`, body)
  },

  runPkg: (modules: string | string[], opts: { no_bump?: boolean; version?: string } = {}) => {
    const body: Record<string, unknown> = { no_bump: opts.no_bump ?? true }
    if (Array.isArray(modules)) body.modules = modules
    else body.module = modules
    // version 명시 시 cims.sh pkg -v <ver> 로 모든 대상 모듈이 그 버전으로 산출됨.
    if (opts.version && opts.version.trim()) body.version = opts.version.trim()
    return api.post<BuildJobStart>(`${BASE}/pkg`, body)
  },

  // 빌드 + 패키징 통합 — 한 job 으로 cims.sh build [-v X.Y.Z] && pkg --no-bump
  runRelease: (opts: { version?: string } = {}) => {
    const body: Record<string, unknown> = {}
    if (opts.version && opts.version.trim()) body.version = opts.version.trim()
    return api.post<BuildJobStart>(`${BASE}/release`, body)
  },

  // 패키지 산출물 정리 — packages/*.tar.gz + manifest.json 삭제. 빌드 산출물은 유지.
  cleanPackages: () =>
    api.post<CleanResult>(`${BASE}/clean`, {}),

  getJob: (jobId: string) =>
    api.get<BuildJobStatus>(`${BASE}/jobs/${jobId}`),

  getManifest: () =>
    api.get<ManifestResponse>(`${BASE}/manifest`),

  listPackages: () =>
    api.get<PackagesListResponse>(`${BASE}/packages`),

  downloadPackage: (module: string) =>
    downloadBlob(`${BASE}/packages/${module}`, `${module}.tar.gz`),
}
