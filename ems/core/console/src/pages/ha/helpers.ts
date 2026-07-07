// HaServicesPage + ServersPage 공유 helper.

/** IPv4 prefix/host 분리. 표준 mask (8/16/24) 만 지원. 비표준 또는 잘못된 IP 면 null. */
export function splitPrefixHost(ip: string, mask: number): { prefix: string; host: string } | null {
  const parts = ip.split('.')
  if (parts.length !== 4 || parts.some(p => p === '')) return null
  if (mask === 24) return { prefix: parts.slice(0, 3).join('.') + '.', host: parts[3] }
  if (mask === 16) return { prefix: parts.slice(0, 2).join('.') + '.', host: parts.slice(2).join('.') }
  if (mask === 8)  return { prefix: parts[0] + '.',                     host: parts.slice(1).join('.') }
  return null
}

/** apply 결과 stdout 의 [OK]/[SKIP]/[DENY]/[FAIL] line 개수 카운트. */
export function summarizeApplyResult(stdout: string | null): { ok: number; skip: number; deny: number; fail: number } {
  const s = stdout ?? ''
  return {
    ok:   (s.match(/^\[OK\]/gm) || []).length,
    skip: (s.match(/^\[SKIP\]/gm) || []).length,
    deny: (s.match(/^\[DENY\]/gm) || []).length,
    fail: (s.match(/^\[FAIL\]/gm) || []).length,
  }
}
