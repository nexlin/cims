import { api } from './client'

export type ServiceName = 'cmp' | 'csp' | 'cwrtc' | 'csc' | 'console' | 'phone' | 'agent'
export type ServiceAction = 'start' | 'stop' | 'restart'

export interface ServicesStatus {
  driver: 'cims_sh' | 'systemd'
  output: string          // ANSI-colored status text from cims.sh
  stderr?: string | null
}

export interface ServiceActionResult {
  driver: string
  service: ServiceName
  action: ServiceAction
  returncode: number
  stdout: string
  stderr: string
}

export const servicesApi = {
  status: () => api.get<ServicesStatus>('/services'),
  act: (name: ServiceName, action: ServiceAction) =>
    api.post<ServiceActionResult>(`/services/${name}/${action}`, {}),
}

// cims.sh status 출력을 파싱 (간단한 정규식)
export function parseServiceStatus(output: string): Record<string, { running: boolean; pid?: number }> {
  // ANSI 코드 제거
  const clean = output.replace(/\u001b\[[0-9;]*m/g, '')
  const out: Record<string, { running: boolean; pid?: number }> = {}
  const lines = clean.split('\n')
  for (const line of lines) {
    // "● cmp           실행 중  (pid=N)" or "● cmp           중지됨"
    const m = line.match(/●\s+(\w+)\s+(\S+)(?:\s*\(pid=(\d+)\))?/)
    if (m) {
      out[m[1]] = {
        running: m[2].includes('실행') || m[2].toLowerCase().includes('running'),
        pid: m[3] ? parseInt(m[3], 10) : undefined,
      }
    }
  }
  return out
}
