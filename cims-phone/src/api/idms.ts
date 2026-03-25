const MCPTT_BASE = '/mcptt'

export interface McpttTokens {
  access_token: string
  refresh_token: string
  id_token: string
  mcptt_id: string   // "tel:+821357007001"
}

function generateCodeVerifier(): string {
  const buf = new Uint8Array(32)
  crypto.getRandomValues(buf)
  return btoa(String.fromCharCode(...buf))
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '')
}

async function generateCodeChallenge(verifier: string): Promise<string> {
  const data = new TextEncoder().encode(verifier)
  const hash = await crypto.subtle.digest('SHA-256', data)
  return btoa(String.fromCharCode(...new Uint8Array(hash)))
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '')
}

// Phone number → tel: URI (idempotent)
function toTelUri(phoneNumber: string): string {
  const n = phoneNumber.trim()
  if (n.startsWith('tel:')) return n
  return `tel:${n.startsWith('+') ? n : '+' + n}`
}

export async function idmsLogin(phoneNumber: string, password: string): Promise<McpttTokens> {
  const verifier   = generateCodeVerifier()
  const challenge  = await generateCodeChallenge(verifier)
  const mcpttId    = toTelUri(phoneNumber)
  const redirectUri = `${window.location.origin}/callback`
  const state       = generateCodeVerifier().slice(0, 16)

  // Step 1: authreq
  const params = new URLSearchParams({
    user_name: mcpttId,
    user_password: password,
    client_id: 'MCPTT_UE',
    redirect_uri: redirectUri,
    code_challenge: challenge,
    code_challenge_method: 'S256',
    scope: 'openid mcptt',
    state,
  })
  const authRes = await fetch(`${MCPTT_BASE}/idms/authreq?${params}`)
  if (!authRes.ok) {
    const err = await authRes.json().catch(() => ({}))
    throw new Error(err.error_description || err.error || '인증 실패')
  }
  const authData = await authRes.json()
  const code = authData.code
  if (!code) throw new Error('auth code 없음')

  // Step 2: tokenreq
  const tokenRes = await fetch(`${MCPTT_BASE}/idms/tokenreq`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ grant_type: 'authorization_code', code, code_verifier: verifier, client_id: 'MCPTT_UE', redirect_uri: redirectUri }),
  })
  if (!tokenRes.ok) {
    const err = await tokenRes.json().catch(() => ({}))
    throw new Error(err.error_description || err.error || '토큰 발급 실패')
  }
  const tokenData = await tokenRes.json()

  // Decode mcptt_id from id_token JWT payload (base64url)
  const [, b64] = tokenData.id_token.split('.')
  const idPayload = JSON.parse(atob(b64.replace(/-/g, '+').replace(/_/g, '/')))

  return {
    access_token: tokenData.access_token,
    refresh_token: tokenData.refresh_token,
    id_token: tokenData.id_token,
    mcptt_id: idPayload.mcptt_id || mcpttId,
  }
}

export async function idmsRefresh(refreshToken: string): Promise<McpttTokens> {
  const res = await fetch(`${MCPTT_BASE}/idms/tokenreq`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ grant_type: 'refresh_token', refresh_token: refreshToken, client_id: 'MCPTT_UE' }),
  })
  if (!res.ok) throw new Error('토큰 갱신 실패')
  const data = await res.json()
  const [, b64] = data.id_token.split('.')
  const idPayload = JSON.parse(atob(b64.replace(/-/g, '+').replace(/_/g, '/')))
  return {
    access_token: data.access_token,
    refresh_token: data.refresh_token,
    id_token: data.id_token,
    mcptt_id: idPayload.mcptt_id || '',
  }
}
