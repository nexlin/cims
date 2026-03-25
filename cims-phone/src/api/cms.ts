const MCPTT_BASE = '/mcptt'

export interface McpttUserProfile {
  mcptt_id: string
  display_name: string
}

function parseUserProfileXml(xml: string, fallback: string): McpttUserProfile {
  const doc = new DOMParser().parseFromString(xml, 'application/xml')
  const display_name = doc.querySelector('display-name')?.textContent?.trim() || fallback
  const mcptt_id = doc.querySelector('MCPTTUserID')?.textContent?.trim() || fallback
  return { mcptt_id, display_name }
}

export async function getUserProfile(userUri: string, accessToken: string): Promise<McpttUserProfile> {
  const res = await fetch(
    `${MCPTT_BASE}/org.3gpp.mcptt.user-profile/users/${encodeURIComponent(userUri)}/user-profile`,
    { headers: { Authorization: `Bearer ${accessToken}` } }
  )
  if (!res.ok) return { mcptt_id: userUri, display_name: userUri.replace(/^tel:/, '') }
  const xml = await res.text()
  return parseUserProfileXml(xml, userUri)
}
