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
  onFloorReject?: () => void
  onMemberStatus?: (userId: string, connected: boolean) => void
  onLocalVideo?: (stream: MediaStream | null) => void
  onAudioLevel?: (level: number) => void  // 0~1 실시간 음량
}

export class PhoneClient {
  private ws: WebSocket | null = null
  private pc: RTCPeerConnection | null = null
  private localStream: MediaStream | null = null
  private audioEl: HTMLAudioElement | null = null
  private videoEl: HTMLVideoElement | null = null
  private videoEnabled = false
  private remoteVideoStream: MediaStream | null = null
  private remoteAudioStream: MediaStream | null = null
  private state: PhoneState = 'disconnected'
  private activeCallId = ''
  private pendingIncoming: IncomingInfo | null = null
  private cb: PhoneCallbacks
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private pingTimer: ReturnType<typeof setInterval> | null = null
  private connArgs: { wsUrl: string; user: string; password: string; domain: string; authId?: string } | null = null
  private intentionalClose = false
  private audioCtx: AudioContext | null = null
  private analyser: AnalyserNode | null = null
  private levelTimer: ReturnType<typeof setInterval> | null = null

  constructor(cb: PhoneCallbacks) {
    this.cb = cb
  }

  setAudioElement(el: HTMLAudioElement) {
    this.audioEl = el
  }

  setVideoElement(el: HTMLVideoElement | null) {
    this.videoEl = el
  }

  setVideoEnabled(enabled: boolean) {
    this.videoEnabled = enabled
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
      // 30초 간격 ping으로 WS 연결 유지 (프록시/TLS 타임아웃 방지)
      this.pingTimer = setInterval(() => { this.send({ type: 'ping' }) }, 30000)
      this.setState('registering')
      this.send({ type: 'register', user, password, domain, auth_id: authId || user })
    }

    this.ws.onmessage = (e) => {
      try { this.handleMessage(JSON.parse(e.data)) }
      catch { /* ignore malformed */ }
    }

    this.ws.onclose = () => {
      this.ws = null
      if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null }
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
    if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null }
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

      case 'ptt_reject':
        this.cb.onFloorReject?.()
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

      // PTT: 초기에 무음 트랙으로 sendrecv 확보 → PUSH 시 replaceTrack으로 실제 미디어 교체
      const silentCtx = new AudioContext()
      const oscillator = silentCtx.createOscillator()
      const dst = silentCtx.createMediaStreamDestination()
      oscillator.connect(dst)
      oscillator.start()
      const silentAudioTrack = dst.stream.getAudioTracks()[0]
      silentAudioTrack.enabled = false  // 무음

      this.pc.addTrack(silentAudioTrack, dst.stream)

      if (this.videoEnabled) {
        // 비디오: 빈 캔버스 트랙으로 sendrecv 확보
        const canvas = document.createElement('canvas')
        canvas.width = 2; canvas.height = 2
        const videoStream = canvas.captureStream(1)
        const dummyVideoTrack = videoStream.getVideoTracks()[0]
        dummyVideoTrack.enabled = false
        this.pc.addTrack(dummyVideoTrack, videoStream)
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

  // 사용자 인터랙션 시 오디오 재생 보장
  ensureAudioPlaying() {
    if (this.audioEl && this.audioEl.srcObject) {
      this.audioEl.play().catch(() => {})
    }
  }

  // ── PTT floor control ───────────────────────────────────────────────────────

  setPttFloor(active: boolean) {
    if (active) {
      // PUSH: 마이크(+카메라) 획득 후 replaceTrack으로 transceiver에 트랙 교체
      const constraints = { audio: true, video: this.videoEnabled }
      navigator.mediaDevices.getUserMedia(constraints).then(stream => {
        if (!this.pc) return
        this.localStream?.getTracks().forEach(t => t.stop())
        this.localStream = stream

        // mid 기반으로 정확한 transceiver 매칭 (mid:0=audio, mid:1=video)
        for (const tr of this.pc.getTransceivers()) {
          if (tr.mid === '0') {
            const t = stream.getAudioTracks()[0]
            if (t) tr.sender.replaceTrack(t)
          } else if (tr.mid === '1') {
            const t = stream.getVideoTracks()[0]
            if (t) tr.sender.replaceTrack(t)
          }
        }

        if (this.videoEnabled) this.cb.onLocalVideo?.(stream)
        this.send({ type: 'ptt_request', call_id: this.activeCallId })
      }).catch(() => {
        this.getMic().then(stream => {
          if (!stream || !this.pc) return
          this.localStream = stream
          for (const tr of this.pc.getTransceivers()) {
            if (tr.mid === '0') {
              const t = stream.getAudioTracks()[0]
              if (t) tr.sender.replaceTrack(t)
            }
          }
          this.send({ type: 'ptt_request', call_id: this.activeCallId })
        })
      })
    } else {
      // RELEASE: 트랙 제거 (null로 교체) + 스트림 정리
      if (this.pc) {
        for (const tr of this.pc.getTransceivers()) {
          tr.sender.replaceTrack(null)
        }
      }
      this.localStream?.getTracks().forEach(t => t.stop())
      this.localStream = null
      // 로컬 카메라 프리뷰 해제 → 리모트 비디오 복원
      this.cb.onLocalVideo?.(null)
      if (this.videoEl && this.remoteVideoStream) {
        this.videoEl.srcObject = this.remoteVideoStream
        this.videoEl.play().catch(() => {})
      }
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
      const track = e.track
      console.log('[PhoneClient] ontrack:', track.kind, 'readyState:', track.readyState, 'mid:', e.transceiver?.mid)
      if (track.kind === 'video') {
        this.remoteVideoStream = e.streams[0] || new MediaStream([track])
        if (this.videoEl) {
          this.videoEl.srcObject = this.remoteVideoStream
          this.videoEl.play().catch(() => { })
        }
      } else if (track.kind === 'audio') {
        this.remoteAudioStream = e.streams[0] || new MediaStream([track])
        if (this.audioEl) {
          this.audioEl.srcObject = this.remoteAudioStream
          this.audioEl.play().catch(() => {})
        }
        this.startAudioLevelMonitor(this.remoteAudioStream)
      }
    }
    return pc
  }

  private startAudioLevelMonitor(stream: MediaStream) {
    this.stopAudioLevelMonitor()
    try {
      this.audioCtx = new AudioContext()
      const source = this.audioCtx.createMediaStreamSource(stream)
      this.analyser = this.audioCtx.createAnalyser()
      this.analyser.fftSize = 256
      source.connect(this.analyser)
      const dataArray = new Uint8Array(this.analyser.frequencyBinCount)
      this.levelTimer = setInterval(() => {
        if (!this.analyser) return
        this.analyser.getByteFrequencyData(dataArray)
        let sum = 0
        for (let i = 0; i < dataArray.length; i++) sum += dataArray[i]
        const avg = sum / dataArray.length / 255  // 0~1
        this.cb.onAudioLevel?.(avg)
      }, 100)
    } catch { /* AudioContext not supported */ }
  }

  private stopAudioLevelMonitor() {
    if (this.levelTimer) { clearInterval(this.levelTimer); this.levelTimer = null }
    if (this.audioCtx) { this.audioCtx.close().catch(() => {}); this.audioCtx = null }
    this.analyser = null
    this.cb.onAudioLevel?.(0)
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
    this.stopAudioLevelMonitor()
    this.pc?.close()
    this.pc = null
    this.localStream?.getTracks().forEach(t => t.stop())
    this.localStream = null
    this.remoteVideoStream = null
    this.remoteAudioStream = null
    if (this.audioEl) this.audioEl.srcObject = null
    if (this.videoEl) this.videoEl.srcObject = null
  }

  private fullCleanup() {
    this.closePC()
    this.activeCallId = ''
    this.pendingIncoming = null
  }
}
