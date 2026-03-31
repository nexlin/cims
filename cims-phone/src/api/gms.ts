const MCPTT_BASE = '/mcptt'

export interface GmsMember {
  uri: string
  name: string
  priority: number
}

export interface GmsGroup {
  uri: string        // "tel:+82571900000"
  id: string         // "+82571900000" (without tel: prefix)
  display_name: string
  video_enabled: boolean
  etag: string
  member_count: number
  members: GmsMember[]
}

function uriToId(uri: string): string {
  return uri.replace(/^tel:/, '')
}

function parseGroupXml(xml: string, groupUri: string): GmsGroup {
  const parser = new DOMParser()
  const doc = parser.parseFromString(xml, 'application/xml')

  const display_name = doc.querySelector('display-name')?.textContent || groupUri
  // 네임스페이스 접두사 포함 (mcpttgi:mcptt-video) → getElementsByTagNameNS 또는 로컬명 검색
  let video_enabled = false
  const allEls = doc.getElementsByTagName('*')
  for (let i = 0; i < allEls.length; i++) {
    if (allEls[i].localName === 'mcptt-video') {
      video_enabled = allEls[i].textContent === 'true'
      break
    }
  }

  const members: GmsMember[] = []
  doc.querySelectorAll('list > entry').forEach(entry => {
    const uri = entry.getAttribute('uri') || ''
    const name = entry.querySelector('display-name')?.textContent || uri
    const priorityEl = entry.querySelector('user-priority')
    const priority = priorityEl ? parseInt(priorityEl.textContent || '5', 10) : 5
    if (uri) members.push({ uri, name, priority })
  })

  return { uri: groupUri, id: uriToId(groupUri), display_name, video_enabled, etag: '', member_count: members.length, members }
}

export async function listMyGroups(userUri: string, accessToken: string): Promise<GmsGroup[]> {
  const res = await fetch(
    `${MCPTT_BASE}/org.openmobilealliance.groups/users/${encodeURIComponent(userUri)}`,
    { headers: { Authorization: `Bearer ${accessToken}` } }
  )
  if (!res.ok) throw new Error('GMS list 실패')
  const list: Array<{ uri: string; display_name: string; etag: string; member_count: number }> = await res.json()

  // Fetch full XML for each group in parallel
  const groups = await Promise.all(
    list.map(async summary => {
      try {
        const gRes = await fetch(
          `${MCPTT_BASE}/org.openmobilealliance.groups/users/${encodeURIComponent(userUri)}/${encodeURIComponent(summary.uri)}`,
          { headers: { Authorization: `Bearer ${accessToken}`, Accept: 'application/vnd.oma.poc.groups+xml' } }
        )
        if (!gRes.ok) throw new Error()
        const xml = await gRes.text()
        const g = parseGroupXml(xml, summary.uri)
        return { ...g, etag: summary.etag }
      } catch {
        return { uri: summary.uri, id: uriToId(summary.uri), display_name: summary.display_name, video_enabled: false, etag: summary.etag, member_count: summary.member_count, members: [] }
      }
    })
  )
  return groups
}
