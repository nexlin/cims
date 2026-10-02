package com.cims.ue.ptt

import android.content.Context
import android.os.SystemClock
import android.util.Log
import com.cims.ue.core.sip.RegState
import com.cims.ue.ptt.PttController.Companion.TAG
import com.cims.ue.ptt.PttController.Companion.bareId
import com.cims.ue.ptt.PttController.Companion.isAdhocId
import com.cims.ue.ptt.csc.GroupDoc
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.McService
import com.cims.ue.sdk.ReceptionEvent
import com.cims.ue.sdk.ReceptionEventKind
import com.cims.ue.sdk.ReceptionState
import com.cims.ue.sdk.TransmissionEvent
import com.cims.ue.sdk.TransmissionEventKind
import com.cims.ue.sdk.TransmissionState
import com.cims.ue.sdk.VideoGroupCallOptions
import com.cims.ue.sdk.VideoTransmitter
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/** 영상을 보내는 중 무전(PTT)을 누를 때 마이크를 누구에게 줄지(mcvideo.md §7 D12 — TS 22.280 R-8.3-003, 사용자가 정한다). */
enum class VideoMicPolicy {
    /** 기본 — PTT 를 누르는 동안 마이크는 음성 무전으로, 영상 호 음성 송신은 멈춘다(영상은 계속). */
    VOICE_FIRST,
    /** 영상 보내는 중에는 PTT 를 눌러도 무전 발언을 요청하지 않는다(무전 마이크 안 씀). 긴급·임박은 여전히 음성 우선(R-8.3-004). */
    VIDEO_FIRST,
}

/** MCVideo 그룹 호의 화면 상태 — MCPTT 세션([GroupCallState])과 독립 다이얼로그다(mcvideo.md §7 D6). */
data class VideoCallState(
    val groupId: String,
    /** -1 = 개시 중. */
    val callId: Int,
    val active: Boolean,
    /** 내 송출(TS 24.581 §6.2.4 'U: …') — [영상 보내기] 토글이 이 값을 그린다. */
    val transmission: TransmissionState,
    /** 다른 사람의 송출 — 알림(NOTIFIED)·수신(RECEIVING) 상태(§6.2.5). 끝난 송출은 빠진다. */
    val transmitters: List<VideoTransmitter>,
    /** 송출자별 처음 알린 시각(elapsedRealtime) — 목록의 경과 표시. */
    val since: Map<String, Long> = emptyMap(),
    /** 내 송출 허가 시각(elapsedRealtime, 0 = 송출 안 함). */
    val sendingSinceMs: Long = 0,
    /** 내 송출을 받기 시작한 사람 수(Media Reception Notification — §6.2.4.5.6). */
    val receivers: Int = 0,
    /** 대기 순번(Queue Position Info — §6.2.4.4.5), 대기 중일 때만. */
    val queuePosition: Int? = null,
    /** 무전 발언 중이라 이 호의 음성 송신을 멈췄다(영상은 계속 — D12). */
    val voiceMuted: Boolean = false,
    /** 내가 연 prearranged 호가 성립 전 — 제어 기능이 멤버를 초대하는 중(TS 24.281 §9.2.1.4.2). [영상 보내기] 끔 = 개시를 거둔다. */
    val opening: Boolean = false,
) {
    /** [영상 보내기] 켜짐 — 요청·대기·허가 중(끝내기 요청 중은 꺼짐). */
    val sendOn: Boolean get() = transmission != TransmissionState.NO_PERMISSION && transmission != TransmissionState.PENDING_END
    /** 송출 허가 — 코어가 마이크·카메라를 연다. */
    val sending: Boolean get() = transmission == TransmissionState.PERMITTED
    /** 보고 있는 송출(1차 수신 스트림 상한 1 — mcvideo_dev_plan.md §1). */
    val receiving: VideoTransmitter? get() = transmitters.firstOrNull { it.state == ReceptionState.RECEIVING }
}

/**
 * MCVideo 평면 — 그룹 영상 호(TS 24.281 §9.2.2 chat · §9.2.1 prearranged)와 전송 제어(TS 24.581)의 화면 투영.
 *
 * 코어가 하는 것: INVITE(mcvideo-info·제어 채널 SDP)·멤버 초대 자동 수락·송출 허가에 따른 마이크·카메라 개폐(§6.2.4.4.6)·
 * 전송 제어 상태 머신(T100~T104). 여기는 정책과 투영만 든다.
 *  - **합류**(mcvideo.md §7 D10) — 영상 채널(그룹 문서 MCVideo 몫)에 있으면 MCVideo 호에도 함께 있는다. 1차 = 주채널만(영상 칸·수신 창이
 *    주채널에만 있다). MCVideo affiliation 은 영상 채널 하나만 — 들어가면 싣고 떠나면 푼다([sync]). chat = 그 호에 합류, prearranged = 멤버
 *    초대 자동 수락(호는 열어 두지 않는다 — 전원 초대라 보낼 사람이 연다, 아래 송출). TS 22.280 R-8.4.2-002(여러 서비스를 한 번의 논리적
 *    제휴로). «채널» = 사용자가 고른
 *    주채널(ChannelStore.primary — 주채널 선택·복원으로 정하고 나가기·주채널 해제로만 지운다, 다른 그룹 팬아웃이 덮지 않는다)이지 무전
 *    세션의 수명이 아니다 — 무전 세션은 T4(hang timer, TS 24.379 §6.3.8.1)로 수시로 끝나지만 그 사이에도 영상 호는 이어진다.
 *  - **송출**(D11) — [영상 보내기] 토글 = 송출 요청·끝내기. [PTT]·하드웨어 PTT 키는 MCPTT 음성만. prearranged 그룹에 영상 호가 없으면
 *    [영상 보내기] 가 호를 연다 — 개시 INVITE 에 송출 요청을 싣는다(암묵적 송출 요청, TS 24.281 §9.2.1.2.1.1 16) · TS 24.581 §14.2.4).
 *  - **수신**(D8) — manual: «영상 n» 목록의 [보기] = Receive Media Request, [그만 보기] = Media Reception End Request.
 *  - **마이크 경합**(D12) — [VideoMicPolicy].
 * 엔진 수신 창이 하나라 영상 호는 한 번에 하나다.
 */
internal class VideoPlane(private val c: PttController, context: Context) {

    private inner class Call(val groupId: String) {
        var callId = -1
        var active = false
        var leaving = false                                            // hangup 을 보냈다(끝나기를 기다린다)
        var opening = false                                            // 내가 연 prearranged 호(§9.2.1.2.1.1)
        var prearranged = false                                        // prearranged 호(초대·개시·재합류) — chat 은 sync 가 다시 합류한다
        var sessionUri = ""                                            // 제어 기능이 준 MCVideo 세션 식별자(재합류 R-URI — §9.2.1.2.4)
        var tx = TransmissionState.NO_PERMISSION
        var sendingSince = 0L
        val receivers = HashSet<String>()
        var queuePosition: Int? = null
        val transmitters = LinkedHashMap<String, VideoTransmitter>()   // 송출자 MCVideo ID → 송출
        val since = HashMap<String, Long>()
        fun toState(voiceMuted: Boolean) = VideoCallState(groupId, callId, active, tx, transmitters.values.toList(), since.toMap(),
            sendingSince, receivers.size, queuePosition.takeIf { tx == TransmissionState.QUEUED }, voiceMuted, opening && !active)
    }

    private val calls = LinkedHashMap<String, Call>()                  // groupId → 호 (c.lock 아래)
    private val _state = MutableStateFlow<List<VideoCallState>>(emptyList())
    val state: StateFlow<List<VideoCallState>> = _state.asStateFlow()

    /** 참여 MCVideo 기능 PSI(ue-init-config, TS 24.484 §7.2) — 없으면 서버가 MCVideo 를 내지 않는다. 등록 때 정한다. */
    @Volatile var serverUri: String = ""
    private val _available = MutableStateFlow(false)
    /** MCVideo 를 쓸 수 있다 — 서버 PSI 가 있다. 그룹 문서의 MCVideo 몫과 함께 영상 채널을 정한다. */
    val available: StateFlow<Boolean> = _available.asStateFlow()
    fun setServer(uri: String) { serverUri = uri; _available.value = uri.isNotEmpty(); requestSync() }

    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val _micPolicy = MutableStateFlow(
        runCatching { VideoMicPolicy.valueOf(prefs.getString(PREF_MIC_POLICY, null) ?: "") }.getOrDefault(VideoMicPolicy.VOICE_FIRST))
    val micPolicy: StateFlow<VideoMicPolicy> = _micPolicy.asStateFlow()
    fun setMicPolicy(p: VideoMicPolicy) { _micPolicy.value = p; prefs.edit().putString(PREF_MIC_POLICY, p.name).apply() }

    private val _openable = MutableStateFlow<String?>(null)
    /** 영상 호가 없어도 [영상 보내기] 를 누를 수 있는 prearranged 영상 채널 — 누르면 호를 연다([open]). */
    val openable: StateFlow<String?> = _openable.asStateFlow()

    /** 영상 채널 = 사용자가 고른 주채널(ChannelStore.primary). */
    private val channel: String? get() = c.channelStore?.primary

    /** 무전 세션 없이 영상 호만 이어지는 채널 — 주채널 화면이 대기 상태로 보여 준다. */
    val channelGroup: String? get() = channel

    private var mcvAffiliated: String? = null                          // MCVideo 로 affiliate 한 영상 채널 (c.lock 아래)
    private var seenDoc: Pair<String, GroupDoc>? = null                // 마지막으로 맞춘 영상 채널과 그 그룹 문서 (c.lock 아래)
    private var wasRegistered = false
    private val retryAt = HashMap<String, Long>()                      // 합류 재시도 가능 시각(elapsedRealtime) (c.lock 아래)
    private val failures = HashMap<String, Int>()
    private val rejoin = HashMap<String, String>()                     // 그룹 → 잃은 prearranged 세션의 식별자(한 번 재합류, c.lock 아래)
    @Volatile private var ducking = false
    @Volatile private var voiceMuted = false                           // 무전 발언 동안 영상 호 음성을 멈췄다(D12)

    // 맞추기(sync)는 한 줄에서 — 세션·문서·등록 변화가 몰려도 마지막 상태로 한 번(재진입 없음)
    private val kick = Channel<Unit>(Channel.CONFLATED)

    init {
        c.scope.launch { for (u in kick) runCatching { sync() }.onFailure { Log.w(TAG, "video sync", it) } }
        c.scope.launch { c._groupDocs.collect { requestSync() } }
        c.scope.launch { c.regState.collect { requestSync() } }
    }

    /** 주채널·그룹 문서·등록이 바뀌었다 — 영상 호를 맞춘다(D10). */
    fun requestSync() { kick.trySend(Unit) }

    /**
     * 영상 채널 = 주채널(활성 MCPTT 그룹 세션 — 1:1·애드혹 제외)이 MCVideo 그룹이면 그 그룹. MCVideo affiliation 은 영상 채널 하나만
     * (TS 24.281 §8.2.1.2 — PUBLISH 는 관심 그룹 전부라 빠진 그룹은 서버가 해제한다): chat 이면 그 호에 합류하고, prearranged 면 제어 기능이
     * MCVideo 로 affiliate 한 멤버만 초대한다(§6.3.5.5). chat 은 합류가 곧 affiliation(§9.2.2.4.1.1 12)) 이지만 나갈 때의 암묵적 해제는 없다
     * — 단말이 풀지 않으면 옮겨 다닌 채널이 쌓여 N2 를 넘는다(486 102). 그래서 chat 도 명시 affiliation 으로 두고 채널을 떠나면 푼다
     * (D10 «채널을 나가면 영상도» — TS 22.280 R-8.4.2-002). 채널을 바꿀 때는 새 그룹을 먼저 싣고 옛 그룹을 뺀다(집합이 비는 PUBLISH =
     * Expires 0 = 그 사용자 제휴 전부 해제를 거치지 않게). affiliation 은 서버가 MCVideo PSI 를 낼 때만(서비스를 가르지 않는 옛 CSP 는
     * presence PUBLISH 를 MCPTT affiliation 으로 읽는다, mcvideo.md §5.4). 그 밖의 영상 호는 나간다.
     *
     * 영상 채널이 바뀌었거나 그 그룹 문서가 바뀌었으면(편성 변경 통지 xcap-diff — 서버가 그룹을 다시 적재한 뒤에 온다) 합류 물러남을
     * 풀고 affiliation 도 다시 보낸다: 갓 만든 그룹은 서버가 아직 MCVideo 그룹으로 모를 때 시도가 거절되거나(404 113) affiliation 이
     * 기록되지 않을 수 있는데(PUBLISH 는 200 — 결과는 NOTIFY 로만 온다, TS 24.281 §8.2.2.2.3), 물러남이 2 분까지 커지면 그동안 영상을 못 쓴다.
     */
    private fun sync() {
        val registered = c.regState.value is RegState.Registered
        val g = channel
        val doc = g?.let { c._groupDocs.value[it] }
        val mv = doc?.mcvideo
        val want = g.takeIf { registered && serverUri.isNotEmpty() && mv != null }
        val docChanged = synchronized(c.lock) {
            val cur = want?.let { it to doc!! }
            (cur != seenDoc).also { changed ->
                seenDoc = cur
                if (changed && want != null) { failures.remove(want); retryAt.remove(want) }
            }
        }
        // affiliation — 등록이 새로 서면 서버 affiliation 도 새로 시작한다(등록 해제가 지운다)
        val affWant = want
        _openable.value = want.takeIf { mv?.prearranged == true }
        val (affOn, affOff) = synchronized(c.lock) {
            if (registered != wasRegistered) { wasRegistered = registered; mcvAffiliated = null }
            val cur = mcvAffiliated
            when {
                !registered -> null to null
                cur != affWant -> { mcvAffiliated = affWant; affWant to cur }
                docChanged && affWant != null -> affWant to null            // 같은 그룹의 문서가 바뀌었다 — 다시 알린다
                else -> null to null
            }
        }
        affOn?.let { g -> c.cmd("affiliate mcvideo $g") { c.account?.affiliate(g, true, McService.MCVIDEO) ?: notRegistered() } }
        affOff?.let { g -> c.cmd("de-affiliate mcvideo $g") { c.account?.affiliate(g, false, McService.MCVIDEO) ?: notRegistered() } }
        // 호
        val stale = synchronized(c.lock) { calls.values.filter { it.groupId != want && !it.leaving }.map { it.groupId } }
        stale.forEach { leave(it) }
        if (want != null && mv?.prearranged == false) {
            val now = SystemClock.elapsedRealtime()
            val due = synchronized(c.lock) { !calls.containsKey(want) && now >= (retryAt[want] ?: 0L) }
            if (due) join(want)
        }
        // 망이 끊겨 잃은 prearranged 영상 호 — 등록이 돌아오면 세션 식별자로 한 번 재합류한다(TS 24.281 §9.2.1.2.4.1). 채널을 떠났으면 버린다.
        val back = synchronized(c.lock) {
            rejoin.keys.filter { it != want }.forEach { rejoin.remove(it) }
            val now = SystemClock.elapsedRealtime()
            if (want != null && registered && !calls.containsKey(want) && now >= (retryAt[want] ?: 0L)) rejoin.remove(want) else null
        }
        if (want != null && back != null)
            start(want, VideoGroupCallOptions(prearranged = true, queueing = true, sessionUri = back), opening = false)
    }

    private fun notRegistered(): CimsResult<Long> = CimsResult.fail(-1, "not registered")

    private fun callOf(callId: Int): Call? = synchronized(c.lock) { calls.values.firstOrNull { it.callId == callId } }

    private fun publish() {
        _state.value = synchronized(c.lock) { calls.values.map { it.toState(voiceMuted) } }
        val duck = receivingAny()
        if (duck != ducking) { ducking = duck; c.applyListenPolicy() }   // 영상을 보는 동안 무전 소리를 줄인다(D6)
    }

    /** 송출자 표시 이름 — 그룹 문서 멤버 이름, 없으면 번호(로컬 표기). */
    private fun nameOf(groupId: String, userId: String): String {
        val id = bareId(userId)
        return c._groupDocs.value[groupId]?.members?.firstOrNull { bareId(it.uri) == id }?.name ?: PttController.fmtNumber(id)
    }

    /** 합류 실패·예상 밖 종료 — 다음 시도까지 물러난다(10 s → 최대 2 분). 정상 종료 뒤 재합류는 [REJOIN_MS]. */
    private fun backoff(groupId: String, failed: Boolean) {
        val ms = synchronized(c.lock) {
            val n = if (failed) (failures[groupId] ?: 0) + 1 else 0
            if (failed) failures[groupId] = n else failures.remove(groupId)
            val wait = if (failed) (BACKOFF_MS shl (n - 1).coerceAtMost(4)).coerceAtMost(BACKOFF_MAX_MS) else REJOIN_MS
            retryAt[groupId] = SystemClock.elapsedRealtime() + wait
            wait
        }
        c.scope.launch { delay(ms); requestSync() }
    }

    // ── 호 ──

    /** chat 그룹 MCVideo 호 합류(§9.2.2 — 합류가 곧 affiliation). 호 종류는 그룹 문서의 `mcvideo-on-network-invite-members` 와 맞아야
     *  한다(§6.3.5.2 — 어긋나면 404 117·118). */
    private fun join(groupId: String) = start(groupId, VideoGroupCallOptions(prearranged = false, queueing = true), opening = false)

    /** prearranged 그룹 MCVideo 호 열기(§9.2.1.2.1.1) — 호가 없는데 [영상 보내기] 를 눌렀다. 송출 요청은 개시 INVITE 에 싣는다(16) · §6.4 —
     *  TS 24.581 §14.2.4 mc_implicit_request). 제어 기능이 MCVideo 로 affiliate 한 멤버를 초대하고 첫 멤버가 붙으면 200 OK, 아무도 붙지 않으면
     *  480(§9.2.1.4.2). 그사이 진행 중 세션이 생겼으면 서버가 합류로 받는다 — 암묵 요청은 받지 않으므로(§14.3.5) 코어가 명시 요청으로 잇는다.
     *  마이크는 요청과 함께 확보한다([setTransmit] 과 같은 이유). */
    private fun open(groupId: String) {
        c.setVideoCapture(true)
        // 송출 요청 대기열을 쓴다 — offer `mc_queueing`(TS 24.581 §14.2.2 «지원하면 싣는다»): 상한에서 거절 #1 대신 «대기 n» 이 된다
        start(groupId, VideoGroupCallOptions(prearranged = true, queueing = true, implicitTransmissionRequest = true), opening = true)
    }

    private fun start(groupId: String, opts: VideoGroupCallOptions, opening: Boolean) {
        // 사전 구성 전용 그룹 — 호를 열지 않고 알린다(TS 24.281 §9.2.1.2.1.1·§9.2.2.2.1.1). 사용자가 누른 것(열기)에만 알림을 띄운다
        if (!CallRules.groupUsable(c._groupDocs.value[groupId]?.preconfiguredOnly)) {
            if (opening) { releaseCaptureIfIdle(); c.feedback?.blocked("이 그룹으로는 영상 통화를 할 수 없습니다") }
            return
        }
        synchronized(c.lock) {
            if (calls.containsKey(groupId)) return
            calls[groupId] = Call(groupId).also {
                it.prearranged = opts.prearranged
                if (opening) { it.opening = true; it.tx = TransmissionState.PENDING_REQUEST }   // 코어도 호 성립까지 'U: pending request'
            }
        }
        c.ctl.launch {
            val r = c.account?.joinVideoGroupCall(groupId, opts)
            if (r != null && r.ok) {
                val id = r.value!!.id
                val leaveNow = synchronized(c.lock) { calls[groupId]?.let { it.callId = id; it.leaving } ?: true }
                if (voiceMuted) c.cmd("video mute") { c.ue.call(id).setMuted(true) }
                if (leaveNow) c.cmd("video hangup") { c.ue.call(id).hangup() }   // 합류하는 사이 채널을 떠났다
                publish()
                return@launch
            }
            Log.w(TAG, "joinVideoGroupCall $groupId 실패: ${r?.code} ${r?.reason ?: "not registered"}")
            synchronized(c.lock) { calls.remove(groupId) }
            if (opening) { releaseCaptureIfIdle(); c.feedback?.blocked("영상 호를 열지 못했습니다") } else backoff(groupId, failed = true)
            publish()
        }
        publish()
    }

    /** 영상 호 나가기 — BYE(MCVideo 다이얼로그만, MCPTT 호는 그대로 — §6.2.4.1). */
    private fun leave(groupId: String) {
        val id = synchronized(c.lock) { calls[groupId]?.also { it.leaving = true }?.callId } ?: return
        if (id >= 0) c.cmd("video hangup") { c.ue.call(id).hangup() }        // 개시 중(-1)이면 합류가 끝나는 대로 끊는다(join)
    }

    // ── 명령 ──

    /** [영상 보내기] 토글 — 켬 = Transmission Request(§6.2.4.3.2), 끔 = Transmission End Request(§6.2.4.5.3 — 대기·요청 중이면 거둔다).
     *  마이크는 요청과 함께 확보해 둔다(허가 뒤 코어가 연다 — PTT 의 발언 캡처와 같은 이유). */
    fun setTransmit(groupId: String, on: Boolean) {
        val call = synchronized(c.lock) { calls[groupId] }
        if (call == null) {
            if (on && _openable.value == groupId) open(groupId)
            return
        }
        val id = synchronized(c.lock) { call.callId.takeIf { call.active } }
        if (id == null) {
            if (!on && call.opening) leave(groupId)                          // 여는 중 끔 = 개시를 거둔다(CANCEL)
            return
        }
        if (on) {
            c.setVideoCapture(true)
            c.cmd("requestTransmission") { c.ue.call(id).requestTransmission() }
        } else {
            c.cmd("releaseTransmission") { c.ue.call(id).releaseTransmission() }
        }
    }

    /** [보기]·[바꿔 보기] — Receive Media Request(§6.2.5.3.3). 1차 수신 상한 1 — 보던 송출이 있으면 먼저 그만 본다(Media Reception End
     *  Request). */
    fun accept(groupId: String, transmitterId: String) {
        val (id, watching) = synchronized(c.lock) {
            val call = calls[groupId] ?: return
            call.callId to call.transmitters.values.filter { it.state == ReceptionState.RECEIVING && it.userId != transmitterId }
                .map { it.userId }
        }
        if (id < 0) return
        watching.forEach { t -> c.cmd("endReception") { c.ue.call(id).endReception(t) } }
        c.cmd("acceptReception") { c.ue.call(id).acceptReception(transmitterId) }
    }

    /** [그만 보기] — Media Reception End Request(§6.2.5.5). 송출은 목록에 남아 다시 [보기] 할 수 있다. */
    fun stopViewing(groupId: String, transmitterId: String) {
        val id = synchronized(c.lock) { calls[groupId]?.callId } ?: return
        if (id >= 0) c.cmd("endReception") { c.ue.call(id).endReception(transmitterId) }
    }

    // ── 마이크 경합(D12) ──

    /** 영상을 보내는 중 이 PTT 는 무전 발언을 요청하지 않는다 — [VideoMicPolicy.VIDEO_FIRST] 이고 긴급·임박이 아닐 때. */
    fun blocksVoiceTalk(emergency: Boolean): Boolean =
        _micPolicy.value == VideoMicPolicy.VIDEO_FIRST && !emergency && sendingAny()

    /** 무전 발언 캡처가 켜지고 꺼질 때(PTT 누름~발언 끝) — 켜진 동안 영상 호 음성 송신을 멈춘다(영상은 계속). 코어가 허가 ∧ ¬음소거 일
     *  때만 음성을 보내므로 송출 중이 아닌 호에 걸어 두어도 무해하다(ue_sdk.md §4.5 setMuted). */
    fun onVoiceTalk(on: Boolean) {
        if (voiceMuted == on) return
        voiceMuted = on
        val ids = synchronized(c.lock) { calls.values.filter { it.callId >= 0 }.map { it.callId } }
        ids.forEach { id -> c.cmd("video mute($on)") { c.ue.call(id).setMuted(on) } }
        publish()
    }

    // ── 코어 이벤트 ──

    /**
     * 제어 기능의 멤버 초대(prearranged — §9.2.1.3). 받을지는 앱이 정한다(`autoAnswerMcvideo` 끔 — [CallRules.acceptVideoInvitation]):
     * 지금 영상 채널의 초대면 받고(합류일 뿐 — 영상 보기는 [받기] 가 따로 정한다), 아니면 **받기 전에 거절한다**(코어가 480 + Warning 110,
     * TS 24.281 §6.2.3.2.2 2)). 받은 뒤에 채널이 바뀌면 [sync] 가 나간다.
     */
    fun onIncomingCall(ci: CallInfo) {
        val gid = bareId(ci.groupId)
        val want = channel.takeIf { c.regState.value is RegState.Registered && serverUri.isNotEmpty() &&
                                    c._groupDocs.value[it]?.mcvideo != null }
        val keep = synchronized(c.lock) {
            val cur = calls[gid]
            val inCall = cur != null && cur.callId >= 0 && cur.callId != ci.callId      // 이미 이 그룹 영상 호에 있다
            CallRules.acceptVideoInvitation(gid, want, inCall).also { ok ->
                if (ok) calls.getOrPut(gid) { Call(gid) }.also { it.callId = ci.callId; it.prearranged = true }
            }
        }
        if (!keep) { c.cmd("video decline $gid") { c.ue.call(ci.callId).reject() }; return }
        c.cmd("video answer $gid") { c.ue.call(ci.callId).answer() }
        if (voiceMuted) c.cmd("video mute") { c.ue.call(ci.callId).setMuted(true) }
        c._status.value = "영상 호 초대 $gid"
        publish()
        requestSync()                                                    // 주채널이 아니면 sync 가 나간다
    }

    fun onCallState(ci: CallInfo) {
        when (ci.state) {
            CallState.ACTIVE -> {
                val gid = synchronized(c.lock) {
                    calls.values.firstOrNull { it.callId == ci.callId }?.also {
                        it.active = true
                        if (ci.sessionUri.isNotBlank()) it.sessionUri = ci.sessionUri
                    }?.groupId
                } ?: return
                synchronized(c.lock) { failures.remove(gid) }
                publish()
                c.enterCallAudio()                                       // 무전 세션 없이 영상 호만 있어도 통화 오디오
            }
            CallState.DISCONNECTED -> {
                val call = synchronized(c.lock) {
                    calls.values.firstOrNull { it.callId == ci.callId }?.also { calls.remove(it.groupId) }
                } ?: return
                releaseCaptureIfIdle()
                if (call.opening && !call.active) {
                    // 내가 연 prearranged 호가 성립하지 못했다 — 다시 열지 않는다(사용자가 다시 누른다)
                    if (!call.leaving) c.feedback?.blocked(openFailText(ci.lastCode, ci.lastReason, ci.warningCode))
                } else if (!call.leaving) {
                    // 내가 나간 게 아니면 다시 맞춘다 — 성립 전 거절은 물러나서, 성립 뒤 서버 해제는 잠깐 뒤(chat 세션은 다시 연다)
                    if (CallRules.rejoinVideoSession(call.prearranged, call.active, ci.lastCode, call.sessionUri))
                        synchronized(c.lock) { rejoin[call.groupId] = call.sessionUri }       // 망 끊김으로 잃은 prearranged 호
                    backoff(call.groupId, failed = !call.active || ci.lastCode >= 300)
                }
                publish()
                c.leaveCallAudioIfIdle()
            }
            else -> Unit
        }
    }

    fun onTransmission(e: TransmissionEvent) {
        val call = callOf(e.callId) ?: return
        synchronized(c.lock) {
            call.tx = e.state
            when {
                e.kind == TransmissionEventKind.GRANTED -> { call.sendingSince = SystemClock.elapsedRealtime(); call.receivers.clear() }
                e.state == TransmissionState.NO_PERMISSION -> { call.sendingSince = 0; call.receivers.clear() }
            }
            if (e.kind == TransmissionEventKind.RECEIVER_JOINED && e.receiverId.isNotBlank()) call.receivers.add(bareId(e.receiverId))
            if (e.kind == TransmissionEventKind.QUEUE_POSITION) call.queuePosition = e.queuePosition
        }
        // 마이크 확보 — 요청·대기·허가 동안. 허가가 끝나면 PTT 발언이 없는 한 스피커 전용으로 돌아간다.
        c.setVideoCapture(e.state != TransmissionState.NO_PERMISSION)
        val fb = c.feedback
        when (e.kind) {
            TransmissionEventKind.GRANTED -> { fb?.grantTone(); c._status.value = "내 영상 보내는 중" }
            TransmissionEventKind.REJECTED -> fb?.blocked(rejectText(call.groupId, e.cause, e.causeText))
            TransmissionEventKind.REVOKED -> { fb?.revokeTone(); c._status.value = revokeText(e.cause, e.causeText) }
            TransmissionEventKind.END_REQUESTED -> { fb?.revokeTone(); c._status.value = revokeText(e.cause, e.causeText) }
            TransmissionEventKind.ENDED -> { fb?.releaseTone(); c._status.value = "영상 보내기를 멈췄습니다" }
            TransmissionEventKind.QUEUE_POSITION -> c._status.value = "대기 ${e.queuePosition}번째 — 앞 송출이 끝나면 보냅니다"
            TransmissionEventKind.QUEUE_CANCELLED -> c._status.value = "영상 보내기 대기가 취소됐습니다"
            TransmissionEventKind.REQUEST_TIMEOUT -> fb?.blocked("서버 응답이 없어 보내기를 요청하지 못했습니다")
            else -> Unit
        }
        publish()
    }

    fun onReception(e: ReceptionEvent) {
        val call = callOf(e.callId) ?: return
        val t = e.transmitter
        synchronized(c.lock) {
            if (e.kind == ReceptionEventKind.ENDED || t.state == ReceptionState.ENDED) {
                call.transmitters.remove(t.userId); call.since.remove(t.userId)
            } else {
                call.transmitters[t.userId] = t
                call.since.getOrPut(t.userId) { SystemClock.elapsedRealtime() }
            }
        }
        val who = nameOf(call.groupId, t.userId)
        when (e.kind) {
            // manual 수신(Reception Mode '1')은 [보기] 를 기다린다 — automatic 이면 서버가 곧바로 허가가 온다(§6.3.6.3.3)
            ReceptionEventKind.NOTIFIED -> if (!t.automatic) { c.feedback?.videoNoticeTone(); c._status.value = "새 영상: $who" }
            ReceptionEventKind.GRANTED -> c._status.value = "$who 영상 보는 중"
            ReceptionEventKind.REJECTED -> c.feedback?.blocked(
                if (e.cause == RX_MAX_STREAMS) "더 받을 수 없습니다 — 동시에 볼 수 있는 영상(1)이 찼습니다"
                else "영상을 받지 못했습니다" + causeSuffix(e.cause, e.causeText))
            ReceptionEventKind.ENDED -> c._status.value = "$who 이(가) 영상 보내기를 멈췄습니다"
            ReceptionEventKind.END_REQUESTED -> c._status.value = "$who 영상 받기가 끝났습니다"
            ReceptionEventKind.REQUEST_TIMEOUT -> c.feedback?.blocked("서버 응답이 없어 영상을 받지 못했습니다")
            else -> Unit
        }
        publish()
    }

    /** 영상을 보는 중인가 — 무전 소리 줄이기(D6) 판정. */
    fun receivingAny(): Boolean = synchronized(c.lock) {
        calls.values.any { call -> call.transmitters.values.any { it.state == ReceptionState.RECEIVING } }
    }

    private fun sendingAny(): Boolean = synchronized(c.lock) { calls.values.any { it.tx == TransmissionState.PERMITTED } }

    /** 송출 요청·대기·허가 중인 호가 없으면 마이크를 놓는다(PTT 발언이 없는 한 스피커 전용). */
    private fun releaseCaptureIfIdle() {
        if (synchronized(c.lock) { calls.values.none { it.tx != TransmissionState.NO_PERMISSION } }) c.setVideoCapture(false)
    }

    // ── 문구(원인 표 = mcvideo_tc_defs.yaml §9.2.6.2·§9.2.10.2·§9.2.15.2) ──

    private fun rejectText(groupId: String, cause: Int, text: String): String = when (cause) {
        1 -> "보내지 못했습니다 — 동시에 보낼 수 있는 수" +
            (c._groupDocs.value[groupId]?.mcvideo?.maxTransmitters?.let { "($it)" } ?: "") + "가 찼습니다"
        3 -> "보내지 못했습니다 — 영상 호에 나 혼자입니다"
        5 -> "이 그룹에서는 영상을 받기만 할 수 있습니다"
        6 -> "보내지 못했습니다 — 서버 자원이 없습니다"
        else -> "보내지 못했습니다" + causeSuffix(cause, text)
    }

    /** 내가 연 prearranged 호의 실패 응답(§9.2.1.4.2) — 480 = 초대가 나가지 못했거나 아무도 붙지 않았다(MCVideo 로 affiliate 한 멤버가 없다). */
    private fun openFailText(code: Int, reason: String, warningCode: Int = 0): String = when {
        CallRules.rejectionText(code, warningCode) != null -> "영상 호를 열지 못했습니다 — " + CallRules.rejectionText(code, warningCode)
        code == 480 -> "영상 호를 열지 못했습니다 — 영상을 받을 멤버가 없습니다"
        else -> "영상 호를 열지 못했습니다" + if (code > 0) " ($code${if (reason.isNotBlank()) " $reason" else ""})" else ""
    }

    private fun revokeText(cause: Int, text: String): String = when (cause) {
        2 -> "보내기가 멈췄습니다 — 한 번에 보낼 수 있는 시간을 넘었습니다"
        4 -> "보내기가 멈췄습니다 — 우선순위가 높은 송출이 들어왔습니다"
        7 -> "보내기가 대기로 바뀌었습니다"
        8 -> "보내기가 멈췄습니다 — 보는 사람이 없습니다"
        else -> "보내기가 멈췄습니다" + causeSuffix(cause, text)
    }

    private fun causeSuffix(cause: Int, text: String) = when {
        text.isNotBlank() -> " — $text"
        cause > 0 -> " (#$cause)"
        else -> ""
    }

    companion object {
        /** MCVideo 호인가 — 코어 CallInfo.service. */
        fun isVideo(ci: CallInfo) = ci.service == McService.MCVIDEO

        private const val PREFS = "ptt_video"
        private const val PREF_MIC_POLICY = "mic_policy"
        private const val BACKOFF_MS = 10_000L
        private const val BACKOFF_MAX_MS = 120_000L
        private const val REJOIN_MS = 3_000L
        /** Receive Media Response 원인 #7 — 동시 수신 스트림 상한(§9.2.15.2). */
        private const val RX_MAX_STREAMS = 7
    }
}
