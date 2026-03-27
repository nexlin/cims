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

// Pure-JS SHA-256 (RFC 6234) — works without crypto.subtle (HTTP context)
function sha256(message: string): Uint8Array {
  const K = [
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
  ]
  const H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]
  const bytes = new TextEncoder().encode(message)
  const bits = bytes.length * 8
  const padded = new Uint8Array(Math.ceil((bytes.length + 9) / 64) * 64)
  padded.set(bytes)
  padded[bytes.length] = 0x80
  const view = new DataView(padded.buffer)
  view.setUint32(padded.length - 4, bits >>> 0, false)
  view.setUint32(padded.length - 8, Math.floor(bits / 2**32), false)
  const r = (n: number, x: number) => (n >>> x) | (n << (32 - x))
  for (let i = 0; i < padded.length; i += 64) {
    const w = new Uint32Array(64)
    for (let t = 0; t < 16; t++) w[t] = view.getUint32(i + t * 4, false)
    for (let t = 16; t < 64; t++) {
      const s0 = r(w[t-15],7) ^ r(w[t-15],18) ^ (w[t-15] >>> 3)
      const s1 = r(w[t-2],17) ^ r(w[t-2],19) ^ (w[t-2] >>> 10)
      w[t] = (w[t-16] + s0 + w[t-7] + s1) >>> 0
    }
    let [a,b,c,d,e,f,g,h] = H
    for (let t = 0; t < 64; t++) {
      const S1 = r(e,6) ^ r(e,11) ^ r(e,25)
      const ch = (e & f) ^ (~e & g)
      const tmp1 = (h + S1 + ch + K[t] + w[t]) >>> 0
      const S0 = r(a,2) ^ r(a,13) ^ r(a,22)
      const maj = (a & b) ^ (a & c) ^ (b & c)
      const tmp2 = (S0 + maj) >>> 0
      h=g; g=f; f=e; e=(d+tmp1)>>>0; d=c; c=b; b=a; a=(tmp1+tmp2)>>>0
    }
    H[0]=(H[0]+a)>>>0; H[1]=(H[1]+b)>>>0; H[2]=(H[2]+c)>>>0; H[3]=(H[3]+d)>>>0
    H[4]=(H[4]+e)>>>0; H[5]=(H[5]+f)>>>0; H[6]=(H[6]+g)>>>0; H[7]=(H[7]+h)>>>0
  }
  const out = new Uint8Array(32)
  const ov = new DataView(out.buffer)
  H.forEach((h, i) => ov.setUint32(i * 4, h, false))
  return out
}

async function generateCodeChallenge(verifier: string): Promise<string> {
  const hash = sha256(verifier)
  return btoa(String.fromCharCode(...hash))
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
