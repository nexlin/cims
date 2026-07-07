// Agent name → display label 매핑 (verify P1 토폴로지)
// _native_steps._INSTANCES.display_name + 1 management 항목.
const AGENT_DISPLAY: Record<string, string> = {
  'mgmt-server':         'CIMS 관리 서버',
  'volte-sip-server':    'VoLTE SIP Server',
  'volte-media-server':  'VoLTE Media Server',
  'ptt-sip-server':      'PTT SIP Server',
  'ptt-media-server':    'PTT Media Server',
}

export function agentDisplayName(agentName: string | null | undefined): string {
  if (!agentName) return ''
  return AGENT_DISPLAY[agentName] ?? agentName
}
