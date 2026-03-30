export type PhoneState =
  | 'disconnected'
  | 'connecting'
  | 'registering'
  | 'registered'
  | 'calling'
  | 'ringing'
  | 'incoming'
  | 'active'

export interface IncomingInfo {
  callId: string
  from: string
  groupId?: string   // PTT 그룹 ID (ptt_auto_answer 전용)
  sdp: string
  ptt: boolean
}

export interface PhoneCallbacks {
  onState: (s: PhoneState) => void
  onIncoming: (info: IncomingInfo) => void
  onError: (msg: string) => void
  onFloor?: (speaker: string | null) => void
  onMemberStatus?: (userId: string, connected: boolean) => void
}

export class PhoneClient {
  private ws: WebSocket | null = null
  private pc: RTCPeerConnection | null = null
  private localStream: MediaStream | null = null
  private audioEl: HTMLAudioElement | null = null
  private state: PhoneState = 'disconnected'
  private activeCallId = ''
  private pendingIncoming: IncomingInfo | null = null
  private cb: PhoneCallbacks
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private connArgs: { wsUrl: string; user: string; password: string; domain: string; authId?: string } | null = null
  private intentionalClose = false

  constructor(cb: PhoneCallbacks) {
    this.cb = cb
  }

  setAudioElement(el: HTMLAudioElement) {
    this.audioEl = el
  }

  getState(): PhoneState { return this.state }
  getActiveCallId(): string { return this.activeCallId }
  getPendingIncoming(): IncomingInfo | null { return this.pendingIncoming }

  private setState(s: PhoneState) {
    this.state = s
    this.cb.onState(s)
  }

  private send(obj: object) {
    if (this.ws?.readyState === WebSocket.OPEN)
      this.ws.send(JSON.stringify(obj))
  }

  // ── Connection ──────────────────────────────────────────────────────────────

  connect(wsUrl: string, user: string, password: string, domain: string, authId?: string) {
    if (this.ws) return
    this.connArgs = { wsUrl, user, password, domain, authId }
    this.intentionalClose = false
    this.setState('connecting')
    try {
      this.ws = new WebSocket(wsUrl)
    } catch {
      this.cb.onError('Invalid WebSocket URL')
      this.setState('disconnected')
      return
    }

    this.ws.onopen = () => {
      if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null }
      this.setState('registering')
      this.send({ type: 'register', user, password, domain, auth_id: authId || user })
    }

    this.ws.onmessage = (e) => {
      try { this.handleMessage(JSON.parse(e.data)) }
      catch { /* ignore malformed */ }
    }

    this.ws.onclose = () => {
      this.ws = null
      this.fullCleanup()
      this.setState('disconnected')
      if (!this.intentionalClose && this.connArgs) {
        const args = this.connArgs
        this.reconnectTimer = setTimeout(() => {
          this.reconnectTimer = null
          this.connect(args.wsUrl, args.user, args.password, args.domain, args.authId)
        }, 3000)
      }
    }

    this.ws.onerror = () => {
      this.cb.onError('WebSocket connection failed')
    }
  }

  disconnect() {
    this.intentionalClose = true
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null }
    this.ws?.close()
    this.ws = null
    this.fullCleanup()
    this.setState('disconnected')
  }

  // ── Message handler ─────────────────────────────────────────────────────────

  private handleMessage(msg: Record<string, string>) {
    console.log('[PhoneClient] handleMessage:', msg)
    switch (msg.type) {
      case 'registered':
        this.setState('registered')
        break

      case 'register_failed':
        this.cb.onError(`등록 실패: ${msg.reason ?? 'auth'}`)
        this.setState('disconnected')
        break

      case 'progress':
        if (this.state === 'calling') this.setState('ringing')
        break

      case 'answered':
        // Outgoing: cwrtc sends DTLS SDP as answer
        this.activeCallId = msg.call_id
        if (this.pc) {
          this.pc.setRemoteDescription({ type: 'answer', sdp: msg.sdp })
            .then(() => this.setState('active'))
            .catch(err => this.cb.onError(`SDP answer error: ${err}`))
        }
        break

      case 'incoming':
        this.pendingIncoming = {
          callId: msg.call_id,
          from: msg.from,
          sdp: msg.sdp,
          ptt: msg.ptt === 'true',
        }
        this.cb.onIncoming(this.pendingIncoming)
        this.setState('incoming')
        break

      // CSP가 PTT 그룹 콜로 초대 → cwrtc가 SIP 200 OK 자동 응답 완료.
      // 브라우저는 제공된 SDP로 WebRTC 연결만 수립하면 됨.
      case 'ptt_auto_answer':
        this.pendingIncoming = {
          callId: msg.call_id,
          from: msg.from,
          groupId: msg.group_id || undefined,
          sdp: msg.sdp,
          ptt: true,
        }
        this.setState('incoming')
        this.cb.onIncoming(this.pendingIncoming)
        break

      case 'ptt_floor':
        this.cb.onFloor?.(msg.speaker ?? null)
        break

      case 'ptt_idle':
        this.cb.onFloor?.(null)
        break

      case 'ptt_member_joined':
        this.cb.onMemberStatus?.(msg.user_id, true)
        break

      case 'ptt_member_left':
        this.cb.onMemberStatus?.(msg.user_id, false)
        break

      case 'ended':
        this.closePC()
        this.activeCallId = ''
        this.pendingIncoming = null
        if (this.state !== 'disconnected') this.setState('registered')
        break
    }
  }

  // ── Outgoing call ───────────────────────────────────────────────────────────

  async call(to: string): Promise<void> {
    if (this.state !== 'registered') return

    const stream = await this.getMic()
    if (!stream) return
    this.localStream = stream

    this.pc = this.createPC()
    this.localStream.getTracks().forEach(t => this.pc!.addTrack(t, this.localStream!))

    const offer = await this.pc.createOffer()
    await this.pc.setLocalDescription(offer)
    await this.waitForIce()

    this.setState('calling')
    this.send({ type: 'call', to, sdp: this.pc.localDescription!.sdp })
  }

  // ── Incoming call answer ────────────────────────────────────────────────────

  async answer(): Promise<void> {
    if (this.state !== 'incoming' || !this.pendingIncoming) return
    const info = this.pendingIncoming

    if (!info.ptt) {
      // 일반 통화: 마이크 필수
      const stream = await this.getMic()
      if (!stream) return
      this.localStream = stream
    }
    // PTT: 초기 수신에는 마이크 불필요 — setPttFloor(true) 시 지연 획득

    try {
      this.pc = this.createPC()
      if (this.localStream) {
        this.localStream.getTracks().forEach(t => this.pc!.addTrack(t, this.localStream!))
      } else {
        // PTT 수신 전용: 오디오 수신 트랜시버 명시 설정
        this.pc.addTransceiver('audio', { direction: 'recvonly' })
      }

      await this.pc.setRemoteDescription({ type: 'offer', sdp: info.sdp })
      const ans = await this.pc.createAnswer()
      await this.pc.setLocalDescription(ans)
      await this.waitForIce()

      this.activeCallId = info.callId
      this.pendingIncoming = null
      this.send({ type: 'answer', call_id: info.callId, sdp: this.pc.localDescription!.sdp })
      this.setState('active')
    } catch (e: unknown) {
      this.cb.onError(`PTT answer 실패: ${(e as Error)?.message ?? String(e)}`)
      console.error('[PhoneClient] answer() error:', e)
    }
  }

  reject() {
    if (this.state !== 'incoming' || !this.pendingIncoming) return
    this.send({ type: 'hangup', call_id: this.pendingIncoming.callId })
    this.pendingIncoming = null
    this.setState('registered')
  }

  hangup() {
    const callId = this.activeCallId || this.pendingIncoming?.callId
    if (callId) this.send({ type: 'hangup', call_id: callId })
    this.closePC()
    this.activeCallId = ''
    this.pendingIncoming = null
    if (this.state !== 'disconnected') this.setState('registered')
  }

  // ── PTT floor control ───────────────────────────────────────────────────────

  setPttFloor(active: boolean) {
    if (active) {
      // PUSH: 마이크가 없으면 지연 획득 후 트랙 추가
      if (!this.localStream) {
        this.getMic().then(stream => {
          if (!stream || !this.pc) return
          this.localStream = stream
          stream.getAudioTracks().forEach(t => {
            this.pc!.addTrack(t, stream)
            t.enabled = true
          })
          this.send({ type: 'ptt_request', call_id: this.activeCallId })
        })
        return
      }
      this.localStream.getAudioTracks().forEach(t => { t.enabled = true })
      this.send({ type: 'ptt_request', call_id: this.activeCallId })
    } else {
      if (this.localStream)
        this.localStream.getAudioTracks().forEach(t => { t.enabled = false })
      this.send({ type: 'ptt_release', call_id: this.activeCallId })
    }
  }

  // ── Helpers ─────────────────────────────────────────────────────────────────

  private async getMic(): Promise<MediaStream | null> {
    if (!navigator.mediaDevices?.getUserMedia) {
      this.cb.onError('마이크 접근 불가: HTTPS 또는 localhost 환경이 필요합니다 (insecure origin)')
      return null
    }
    try {
      return await navigator.mediaDevices.getUserMedia({ audio: true, video: false })
    } catch (err: unknown) {
      const name = (err as DOMException)?.name ?? ''
      if (name === 'NotAllowedError' || name === 'PermissionDeniedError')
        this.cb.onError('마이크 권한이 거부되었습니다. 브라우저 설정에서 허용하세요.')
      else if (name === 'NotFoundError')
        this.cb.onError('마이크를 찾을 수 없습니다.')
      else
        this.cb.onError(`마이크 접근 실패: ${name || String(err)}`)
      return null
    }
  }

  private createPC(): RTCPeerConnection {
    const pc = new RTCPeerConnection({ iceServers: [] })
    pc.ontrack = (e) => {
      if (this.audioEl && e.streams[0]) {
        this.audioEl.srcObject = e.streams[0]
        this.audioEl.play().catch(() => { })
      }
    }
    return pc
  }

  private waitForIce(): Promise<void> {
    return new Promise(resolve => {
      if (!this.pc || this.pc.iceGatheringState === 'complete') { resolve(); return }
      const handler = () => {
        if (this.pc?.iceGatheringState === 'complete') {
          this.pc.removeEventListener('icegatheringstatechange', handler)
          resolve()
        }
      }
      this.pc.addEventListener('icegatheringstatechange', handler)
      setTimeout(resolve, 3000)  // 3s fallback
    })
  }

  private closePC() {
    this.pc?.close()
    this.pc = null
    this.localStream?.getTracks().forEach(t => t.stop())
    this.localStream = null
    if (this.audioEl) this.audioEl.srcObject = null
  }

  private fullCleanup() {
    this.closePC()
    this.activeCallId = ''
    this.pendingIncoming = null
  }
}
