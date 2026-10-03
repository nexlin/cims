package com.cims.ue.ptt

import android.os.SystemClock
import android.util.Log
import com.cims.ue.ptt.PttController.Companion.TAG
import com.cims.ue.ptt.PttController.Companion.TALK_WARN_MS
import com.cims.ue.ptt.PttController.Companion.bareId
import com.cims.ue.ptt.PttController.Companion.isAdhocId
import com.cims.ue.sdk.FloorEvent
import com.cims.ue.sdk.FloorEventKind
import com.cims.ue.sdk.FloorIndicator
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.Talker
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * floor 평면 — PTT 누름/뗌과 floor 이벤트의 화면 투영(TS 24.380 §6.2.4 participant 는 코어).
 * 코어가 하는 것: Floor Request/Release/Queued Cancel·요청 시한(RequestTimeout)·Granted Duration 자체 종료(TalkLimit)·
 * 승인 톤 뒤 마이크 개방(`EngineConfig.grantMicDelayMs`)·Revoke 회신·일제 통화 개시자 해제. 여기는 **발언 대상 선택·피드백 톤·
 * 마감 임박 알림·이력**만 한다.
 */
internal class FloorPlane(private val c: PttController) {

    /** PTT 발언 대상 — 활성 1:1 이 있으면 그것이 우선(전화>무전 규칙), 애드혹 진행 중이면 애드혹, 없으면 주채널. */
    fun talkSession(): PttController.Session? = synchronized(c.lock) {
        // 받기 전의 수동 응답 1:1([GroupCallState.awaitingAnswer])은 발언 대상이 아니다 — [받기] 로 성립한 뒤부터
        c.sessionMap.values.firstOrNull { v -> v.privatePeer && v.callId >= 0 && !v.awaitingAnswer }
            // 애드혹 진행 중엔 전용 오버레이가 전면이라 PTT 도 애드혹 세션을 향한다(주채널보다 우선).
            ?: c.sessionMap.values.firstOrNull { v -> isAdhocId(v.groupId) && v.callId >= 0 }
    } ?: c.primarySession()

    /** PTT down — 발언 대상에 Floor Request. 승인되면 코어가 승인 톤 길이 뒤 마이크를 연다. */
    fun pttDown() {
        val s = talkSession() ?: run { startOnChosenPrimary(); return }
        // 전이중 1:1(mc_no_floor_ctrl — floor 절차 없음, TS 24.379): PTT 는 서버 요청 없이 로컬 마이크 게이트(setMuted)로만
        //   동작한다. 양쪽이 같이 누르면 동시 발화(서버는 상시 중계). 그룹 승인 경로와 같은 "삑 후 말하기".
        if (s.fullDuplex) {
            c.pttHeld = true
            c.setTalkCapture(true)
            s.floorState = FloorState.SPEAKING     // 로컬 표시 (서버 GRANT 아님)
            s.mySpeakStartMs = SystemClock.elapsedRealtime()
            val tone = c.feedback?.grantTone() ?: 100L
            c.scope.launch {
                delay(tone)
                val p = talkSession()
                if (c.pttHeld && p === s && s.callId >= 0) c.cmd("setMuted(false)") { c.ue.call(s.callId).setMuted(false) }
            }
            c.publish()
            return
        }
        // Floor Taken 이 Permission=0 을 실어 온 세션(일제 통화·ambient 청취 leg)은
        // 요청해봐야 Deny 뿐이다 — 요청 자체를 막고 이유를 알린다(TS 24.380 §6.3.4.4.2-3d).
        if (!s.canRequestFloor) { c.feedback?.denyTone(); c._status.value = "이 채널은 청취 전용"; return }
        when (s.floorState) {
            // QUEUED = 이미 요청이 대기열에 있다(버튼 유지 중) — 다시 보낼 것이 없다.
            FloorState.SPEAKING, FloorState.REQUESTING, FloorState.QUEUED -> return
            // 남이 발언 중 — 동시 발언 세션이면 **서버가 정원을 보고 판단**하므로 요청을 보낸다. 동시 발언 불가(single)일
            //   때만 즉시 거부음으로 알린다. 긴급 개시자도 예외 — 선점(REVOKE→GRANT, TS 24.380 §6.3.4.4.7)은 CMP 의
            //   tier 판정이므로 요청이 서버에 도달해야 발동한다(로컬 차단 = 선점 불가).
            FloorState.LISTENING -> if (!s.multiTalkerSession() && !(s.emergency && s.emergencyMine)) {
                c.feedback?.denyTone(); c._status.value = "다른 사용자가 발언 중"; return
            }
            else -> Unit
        }
        if (s.callId < 0) { c._status.value = "그룹콜 연결 중"; return }
        // 영상 보내는 중 — «영상 우선» 이면 무전 마이크를 쓰지 않는다. 긴급은 설정과 무관하게 음성 우선(mcvideo.md §7 D12)
        if (c.videoPlane.blocksVoiceTalk(s.emergency)) {
            c.feedback?.blocked("영상 보내는 중 — 무전 마이크 안 씀(설정: 영상 우선)"); return
        }
        c.pttHeld = true
        c.setTalkCapture(true)     // 마이크 확보 개시 — volte 양보 + 전이중 전환(floor 요청과 병렬 진행)
        s.floorState = FloorState.REQUESTING
        s.queueCancelByMe = false
        c.publish()
        // 긴급 세션의 발언은 Floor Indicator 에 emergency 비트 — 코어가 세션 조건 현재값으로 싣는다(TS 24.380).
        // Floor Priority 는 싣지 않는다 — 유효 우선순위가 요청값으로 깎이지 않게(§6.3.5.4.4-1a).
        c.cmd("floorRequest") { c.ue.call(s.callId).floorRequest(-1) }
    }

    /**
     * 고른 주채널에 무전 호가 없다(T4·TNG3 해제 — 해제는 호를 끝낼 뿐 그룹 선택·affiliation 은 그대로, TS 24.379 §6.3.8.1) — PTT 가 그
     * 그룹의 새 그룹 호를 개시하고 암묵적 발언 요청을 싣는다(TS 24.379 §10.1.1.2.1.1 · TS 24.380 §14.2.4 — 성립과 함께 발언권). 성립 전에
     * 놓으면 [pttUp] 의 Floor Release 를 코어가 성립 뒤로 넘긴다.
     */
    private fun startOnChosenPrimary() {
        val g = c.channelStore?.primary ?: run { c._status.value = "주채널을 먼저 고르세요"; return }
        if (c.videoPlane.blocksVoiceTalk(false)) {
            c.feedback?.blocked("영상 보내는 중 — 무전 마이크 안 씀(설정: 영상 우선)"); return
        }
        c.pttHeld = true
        c.setTalkCapture(true)
        c.groupsPlane.joinGroupCall(g, implicitFloor = true)
        c.publish()
    }

    /** PTT up — Floor Release(대기 중이면 코어가 Queued Cancel 을 먼저 보낸다). */
    fun pttUp() {
        c.pttHeld = false
        val s = talkSession() ?: return
        // 전이중 1:1 — 로컬 마이크 게이트 닫기 (pttDown 과 쌍, 서버 메시지 없음).
        if (s.fullDuplex) {
            if (s.callId >= 0) c.cmd("setMuted(true)") { c.ue.call(s.callId).setMuted(true) }
            c.setTalkCapture(false)
            if (s.mySpeakStartMs > 0) {
                c.emit(PttEventKind.TALK_ME, s.groupId, durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
                s.mySpeakStartMs = 0
            }
            s.floorState = FloorState.IDLE
            c.publish()
            return
        }
        val wasSpeaking = s.floorState == FloorState.SPEAKING
        clearTalkWarn(s)
        if (wasSpeaking && s.mySpeakStartMs > 0) {
            c.emit(PttEventKind.TALK_ME, s.groupId, durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
            s.mySpeakStartMs = 0
        }
        // 요청/점유/대기 중일 때만 보낸다 — 요청조차 안 한 상태(LISTENING/IDLE)의 Release 는 서버가 무시하는 고아 메시지다.
        //   대기 중이었으면 코어가 Queued Cancel 을 먼저 보낸다(§8.2.15) — 그 취소 통지에는 거부음을 내지 않는다.
        if (s.floorState == FloorState.QUEUED) s.queueCancelByMe = true
        if ((s.floorState == FloorState.SPEAKING || s.floorState == FloorState.REQUESTING ||
                s.floorState == FloorState.QUEUED) && s.callId >= 0) {
            c.cmd("floorRelease") { c.ue.call(s.callId).floorRelease() }
        }
        s.queuePosition = null
        c.setTalkCapture(false)    // 발언 종료 — 스피커 전용 복귀 + volte 마이크 복귀
        // 내 발언만 끝난다 — 동시 발언 중이면 남은 화자를 계속 듣는 상태로 남는다.
        s.talkers = s.talkers.filterNot { it.self }
        s.floorState = if (s.talkers.isEmpty()) FloorState.IDLE else FloorState.LISTENING
        if (s.speaker?.self == true) s.speaker = s.talkers.firstOrNull()
        if (wasSpeaking) c.feedback?.releaseTone()
        c.publish()
    }

    /** Granted Duration(TS 24.380 §8.2.3.3 = 서버 T2) 마감 [TALK_WARN_MS] 전에 알린다. 자체 종료는 코어가 마감 직전에 한다
     *  (FloorEvent TALK_LIMIT). Duration 이 없거나 0(무제한)이면 알리지 않는다. */
    private fun armTalkWarn(s: PttController.Session, durationSec: Int) {
        clearTalkWarn(s)
        val d = durationSec.coerceAtLeast(0).toLong() * 1000L
        if (d <= 0) return
        s.speakDeadlineMs = SystemClock.elapsedRealtime() + d
        val warnAt = d - TALK_WARN_MS
        if (warnAt <= 0) return
        s.talkWarn = c.scope.launch {
            delay(warnAt)
            if (s.floorState == FloorState.SPEAKING) c.feedback?.talkLimitTone()
        }
    }

    private fun clearTalkWarn(s: PttController.Session) {
        s.talkWarn?.cancel()
        s.talkWarn = null
        s.speakDeadlineMs = 0
    }

    /** 동시 발언 화자 집합 반영 (TS 24.380 §8.2.3.17~18) — **이미 말하던 화자의 [Speaker.sinceMs] 는 보존한다**(다른 사람이
     *  끼어들 때마다 목록이 다시 온다). 화자별 SSRC 는 [PttController.Session.talkerSsrc] 에 남긴다(U10 의 입력). */
    private fun applyTalkers(s: PttController.Session, list: List<Talker>) {
        val now = SystemClock.elapsedRealtime()
        val prev = s.talkers.associateBy { bareId(it.id) }
        s.talkers = list.map { t -> prev[bareId(t.id)] ?: Speaker(t.id, self = t.self, sinceMs = now, groupId = s.groupId) }
        s.talkerSsrc = list.filter { it.ssrc != 0L }.associate { bareId(it.id) to it.ssrc }
        // 대표 화자 = 내가 말하고 있으면 나, 아니면 첫 타인(단일 발언이면 종전과 동일).
        s.speaker = s.talkers.firstOrNull { it.self } ?: s.talkers.firstOrNull()
    }

    /** 수신 발언자 교대·종료 — 이력 마감. */
    private fun noteOtherSpeaker(s: PttController.Session) {
        val other = s.talkers.firstOrNull { !it.self }?.let { bareId(it.id) }
        if (other == s.otherSpeaker) return
        s.otherSpeaker?.let { prev ->
            c.emit(PttEventKind.TALK_OTHER, s.groupId, peer = prev, durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
        }
        s.otherSpeaker = other
        s.otherSpeakStartMs = SystemClock.elapsedRealtime()
    }

    fun onFloorEvent(ev: FloorEvent) {
        val s = c.sessionByCall(ev.callId) ?: return
        // 발언 주체 판정은 **pttDown 과 같은 규칙([talkSession])** 이어야 한다 — 1:1 은 주채널을 점유하지 않으므로
        //   (role=NONE) role 로만 보면 내가 요청해 받은 GRANT 를 "비주채널"로 오판해 즉시 반납한다(실측).
        val isPrimary = s.role == ChannelRole.PRIMARY
        val isTalkTarget = s === talkSession()
        when (ev.kind) {
            FloorEventKind.GRANTED -> {
                if (!isTalkTarget || !c.pttHeld) {     // 늦은 GRANT/발언 대상 아님 — 즉시 반납
                    c.cmd("floorRelease(late)") { c.ue.call(s.callId).floorRelease() }
                    if (isPrimary) c.setTalkCapture(false)
                    s.floorState = FloorState.IDLE
                    c.publish()
                    return
                }
                s.floorState = FloorState.SPEAKING
                s.floorIndicator = ev.indicator
                // 동시 발언이면 이미 말하던 화자를 남겨 둔 채 나를 더한다(dual/multi 2번째 자리).
                val me = s.talkers.firstOrNull { it.self }
                    ?: Speaker(c.mcpttId, self = true, sinceMs = SystemClock.elapsedRealtime(), groupId = s.groupId)
                s.talkers = listOf(me) + s.talkers.filterNot { it.self }
                s.speaker = me
                s.mySpeakStartMs = SystemClock.elapsedRealtime()
                armTalkWarn(s, ev.durationSec)
                c._status.value = "발언권 획득"
                // "삑 후 말하기" — 톤은 여기서 울리고, 마이크는 코어가 톤 길이(grantMicDelayMs) 뒤에 연다.
                c.feedback?.grantTone()
            }
            FloorEventKind.DENIED -> {
                clearTalkWarn(s)
                // 거부 피드백은 **요청한 세션** 기준 — 1:1 은 주채널이 아니라 role 로 보면 거부음·사유 표시가 누락된다.
                if (isTalkTarget) {
                    c.setTalkCapture(false)
                    c.feedback?.denyTone()
                    c._status.value = "발언권 거부: ${ev.causeText.ifBlank { ev.cause.toString() }}"
                }
                s.floorState = FloorState.IDLE
            }
            FloorEventKind.REVOKED -> {
                clearTalkWarn(s)
                // Floor Release 회신은 코어가 이미 보냈다(§6.2.4.5.4) — 여기선 캡처·UI·이력만.
                if (isTalkTarget) c.setTalkCapture(false)
                if (s.speaker?.self == true && s.mySpeakStartMs > 0) {
                    c.emit(PttEventKind.TALK_ME, s.groupId, durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
                    s.mySpeakStartMs = 0
                }
                // 내 발언만 회수된다 — 동시 발언 중이면 남은 화자는 계속 들린다.
                s.talkers = s.talkers.filterNot { it.self }
                s.speaker = s.talkers.firstOrNull()
                s.floorState = if (s.talkers.isNotEmpty()) FloorState.LISTENING else FloorState.IDLE
                if (isTalkTarget) {
                    c.feedback?.revokeTone()
                    c._status.value = "발언권 회수: ${ev.causeText.ifBlank { ev.cause.toString() }}"
                }
            }
            FloorEventKind.TAKEN -> {
                // Permission to Request the Floor(§8.2.3.7) — 서버가 일제 통화·ambient 청취 leg 에만 0 을 보낸다. 값이 올 때만 갱신.
                if (ev.permission >= 0) s.canRequestFloor = ev.permission != 0
                s.floorIndicator = ev.indicator
                applyTalkers(s, ev.talkers)
                // 동시 발언(dual/multi)에서 뒤에 승급한 화자의 Taken 은 **먼저 말하던 나에게도** 오고 목록에 내가 있다 —
                //   그때 강등하면 내 발언 표시가 사라진다(§6.3.4.4.7a).
                if (ev.meSpeaking) s.floorState = FloorState.SPEAKING
                else { clearTalkWarn(s); s.floorState = FloorState.LISTENING }
                noteOtherSpeaker(s)
                // CMP 는 긴급 tier 발언자의 TAKEN 에 emergency 비트를 방송 — 수신측 긴급 표시 latch(정본 신호는 CSP 의
                //   재광고 re-INVITE — 이 비트는 재광고 유실·발언 선행 케이스를 덮는 보조 신호)
                if ((ev.indicator and FloorIndicator.EMERGENCY) != 0 && !s.emergency) {
                    s.emergency = true
                    c.feedback?.emergencyTone()
                    c.emit(PttEventKind.EMERGENCY_IN, s.groupId, peer = s.otherSpeaker)
                    if (isPrimary) c._status.value = "🚨 [${s.groupId}] 긴급 발언 수신"
                }
            }
            // 동시 발언 중 한 명만 발언 종료(0x0F) — Idle 이 아니므로 목록만 줄인다.
            FloorEventKind.TALKER_LEFT -> {
                applyTalkers(s, ev.talkers)
                if (ev.meSpeaking) s.floorState = FloorState.SPEAKING
                else {
                    clearTalkWarn(s)
                    s.floorState = if (ev.talkers.isEmpty()) FloorState.IDLE else FloorState.LISTENING
                }
                noteOtherSpeaker(s)
            }
            FloorEventKind.IDLE -> {
                // 일제 통화 개시자가 발언을 놓은 뒤의 B-bit Idle 은 코어가 호를 해제한다(TS 24.380 §6.2.4.6.4) — 여기선 표시만.
                if (s.floorState != FloorState.SPEAKING) {
                    clearTalkWarn(s)
                    s.floorState = FloorState.IDLE
                    s.talkers = emptyList()
                    s.talkerSsrc = emptyMap()
                }
                if (s.speaker?.self != true) s.speaker = null
                s.otherSpeaker?.let { prev ->      // 수신 발언 종료 — 이력 마감
                    c.emit(PttEventKind.TALK_OTHER, s.groupId, peer = prev,
                        durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
                    s.otherSpeaker = null
                }
            }
            // 대기열 진입·위치 변동(§8.2.11) — 버튼을 계속 누르고 있으면 승급을 기다리고, 떼면 pttUp 이 취소한다.
            FloorEventKind.QUEUE_POSITION -> {
                s.floorState = FloorState.QUEUED
                s.queuePosition = ev.queuePosition.takeIf { it > 0 }
                if (isTalkTarget) c._status.value = s.queuePosition?.let { "발언 대기 ${it}번째" } ?: "발언 대기 중"
            }
            // 대기 요청 소멸 — 내 취소의 결과이거나 서버/의장이 지운 통지. 어느 쪽이든 IDLE.
            FloorEventKind.QUEUE_CANCELLED -> {
                s.queuePosition = null
                if (s.floorState == FloorState.QUEUED) s.floorState = FloorState.IDLE
                if (isPrimary && !s.queueCancelByMe) {
                    c.feedback?.denyTone()
                    c._status.value = "발언 대기 취소됨"
                }
                s.queueCancelByMe = false
            }
            // 요청 후 응답 없음 — 코어가 Idle 로 되돌렸다(요청 시한).
            FloorEventKind.REQUEST_TIMEOUT -> {
                if (s.floorState == FloorState.REQUESTING) s.floorState = FloorState.IDLE
                if (isTalkTarget) {
                    c.setTalkCapture(false)
                    c.feedback?.denyTone()
                    c._status.value = "발언권 응답 없음"
                }
            }
            // Granted Duration 마감 — 코어가 스스로 Release 했다(서버 Revoke #2 를 앞지른다).
            FloorEventKind.TALK_LIMIT -> {
                Log.i(TAG, "talk limit reached — core self release")
                clearTalkWarn(s)
                if (s.mySpeakStartMs > 0) {
                    c.emit(PttEventKind.TALK_ME, s.groupId, durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
                    s.mySpeakStartMs = 0
                }
                if (isTalkTarget) c.setTalkCapture(false)
                s.talkers = s.talkers.filterNot { it.self }
                s.speaker = s.talkers.firstOrNull()
                s.floorState = if (s.talkers.isEmpty()) FloorState.IDLE else FloorState.LISTENING
                c._status.value = "발언 시간 초과 — 발언 종료"
            }
            FloorEventKind.OTHER -> Log.d(TAG, "floor other ${ev.rawType}")
        }
        c.publish()
    }
}
