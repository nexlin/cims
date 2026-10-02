// 관제 세션의 영상 평면 — MCVideo 영상 채널 (android_dispatch_tablet.md §6.14, dispatch_desktop_ui.md §10 —
//                                              규격 모델·결정의 정본은 mcvideo.md §7 D6·D8·D10~D12)
//
// MCVideo 는 MCPTT 와 **나란한 서비스**다(TS 23.280 §3) — 같은 그룹에서 음성 = MCPTT 그룹 호, 영상 = MCVideo 그룹 호를 따로 든다
// (TS 24.281 §7.1 — 한 등록을 공유하는 독립 다이얼로그). 데스크톱 `Services/DispatchSession.McVideo.cs` 에 대응한다.
//
//   · **D10 «영상 참여» 단계가 없다** — 내 채널의 영상 채널은 앱이 영상 호에도 함께 합류한다(chat = 합류, 편성 = MCVideo affiliation +
//     멤버 초대 자동 수락, TS 22.280 R-8.4.2-002). «채널» 은 관제사가 내 채널에 둔 의도라 무전 세션의 T4·TNG3 해제(TS 24.379
//     §6.3.8.1)와 무관하게 이어지고, 영상 호가 끝나면 다시 합류한다. 끝내는 것은 그룹이 내 채널에서 빠질 때·자격이 없어질 때·로그아웃뿐.
//   · **D8 수신 manual** — 송출을 골라 [보기](한 번에 하나 — 1차 수신 상한 1), [바꿔 보기] = 보던 것을 그만 보고 새것을 본다.
//   · **D11 내 송출** — [영상 보내기] 는 발언 바 PTT 와 따로다. 허가에서만 코어가 카메라·영상 호 마이크를 연다(TS 24.581 §6.2.4.4.6).
//   · **D12 마이크 경합** — 음성 우선(무전 발언 동안 영상 호 음성만 멈춤) / 영상 우선(영상 송출 동안 무전 발언 막음, 긴급·임박 예외).
//   · **D6 소리** — 같은 그룹의 영상을 보는 동안 그 그룹 무전의 수신 음량을 줄인다.
//
// **영상 호는 세션 목록(`sessions`)에 들지 않는다.** 카드가 아니라 그 그룹의 «영상» 절에 붙는 호라, 통화 카드·착신 배너·감청 수·
// 그룹의 무전 세션 찾기(그룹 id 로 찾는 자리)에 새지 않게 따로 든다([videoCalls]). 호 이벤트는 들머리에서 갈라 받는다([takeVideoCall]).
//
// 전부 세션 스코프(메인 스레드)에서 돈다 — 상태에 잠금이 없다. 엔진 명령은 한 줄([videoCmd])로 보낸다: 명령마다 IO 로 흩어지면
// «그만 보기 → 보기»·«표면 떼기 → 붙이기» 의 순서가 바뀐다.
package com.cims.ue.dispatch.session

import android.view.Surface
import com.cims.ue.sdk.AccountConfig
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.GroupDoc
import com.cims.ue.sdk.McService
import com.cims.ue.sdk.McVideoGroupAttrs
import com.cims.ue.sdk.McVideoUserProfileDoc
import com.cims.ue.sdk.ReceptionEvent
import com.cims.ue.sdk.ReceptionEventKind
import com.cims.ue.sdk.ReceptionState
import com.cims.ue.sdk.RegState
import com.cims.ue.sdk.TransmissionEvent
import com.cims.ue.sdk.TransmissionEventKind
import com.cims.ue.sdk.TransmissionInfo
import com.cims.ue.sdk.TransmissionState
import com.cims.ue.sdk.VideoGroupCallOptions
import com.cims.ue.sdk.VideoTransmitter
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

// ── 모델 ─────────────────────────────────────────────────────────────────────

/** «영상 보내는 중 무전»(D12 — TS 22.280 R-8.3-003, 사용자가 정한다) — 설정에 남는 값. 데스크톱 `VideoMicPolicy` 와 같은 두 값. */
object VideoMicPolicy {
    /** 음성 우선(기본) — 무전 발언(요청·대기·발언) 동안 영상 송출 호의 음성만 멈춘다(영상은 계속). */
    const val VOICE = "voice"
    /** 영상 우선 — 영상을 보내는 동안 발언 바 PTT 가 무전 발언을 요청하지 않는다(긴급·임박 채널은 늘 말할 수 있다 — R-8.3-004). */
    const val VIDEO = "video"
}

/** [영상 보내기] 카메라 — 설정에 남는 값. 엔진의 기본은 앞 카메라다(`Engine::frontCamera`). */
object VideoCamera {
    const val FRONT = "front"
    const val BACK = "back"
}

/**
 * 영상 호 하나 — MCVideo 그룹 호(TS 24.281 §9.2.1 편성·§9.2.2 chat)와 그 전송 제어(TS 24.581)의 투영.
 *
 * [info] 는 코어 스냅샷, [tx]·[transmitters] 는 코어 전송 제어 현재값(`transmissionInfo`)의 복사본이다 — 송출·수신 이벤트마다 다시 읽는다.
 */
data class VideoCall(
    val callId: Int,
    val groupId: String,
    val info: CallInfo,
    val startedAtMs: Long = System.currentTimeMillis(),
    val connectedAtMs: Long? = null,
    /** 내 송출 상태(TS 24.581 §6.2.4 'U: …'). */
    val tx: TransmissionState = TransmissionState.NO_PERMISSION,
    /** 대기 순번(Queue Position Info — §6.2.4.4.5), 대기 중일 때만 뜻이 있다. */
    val queuePosition: Int = 0,
    /** 알림 받은(아직 끝나지 않은) 송출 — 내 것 제외. 목록에 보이는 것. */
    val transmitters: List<VideoTransmitter> = emptyList(),
    /** 송출을 처음 안 시각(송출자 → ms) — 목록의 경과. 코어 값에는 시각이 없다. */
    val since: Map<String, Long> = emptyMap(),
    /** 내 송출 허가 시각(Granted — §6.2.4.4.6), 끝나면 null. */
    val txSinceMs: Long? = null,
    /** 내 송출을 보는 사람(Media Reception Notification 의 수신자 — §6.2.4.4.8, 서버가 알릴 때만). */
    val txReceivers: Set<String> = emptySet(),
    /** 내가 연 편성(prearranged) 호 — 성립 전에는 [영상 보내기] 가 «여는 중…» 이다(TS 24.281 §9.2.1.2.1.1). */
    val opening: Boolean = false,
) {
    val isLive: Boolean get() = info.state != CallState.DISCONNECTED && info.state != CallState.NULL
    val isActive: Boolean get() = info.state == CallState.ACTIVE
    /** 내가 연 편성 호가 아직 성립 전 — 제어 기능이 멤버를 초대하는 중(§9.2.1.4.2). */
    val isOpening: Boolean get() = opening && isLive && !isActive
    /** 지금 받고 있는 송출(1차는 하나) — 수신 상태 Receiving·PendingRelease. */
    val receiving: VideoTransmitter? get() = transmitters.firstOrNull {
        it.state == ReceptionState.RECEIVING || it.state == ReceptionState.PENDING_RELEASE
    }
    /** 보고 있거나 **보겠다고 요청한** 송출 — «한 번에 하나» 의 판정(요청 중인 것도 한 자리를 쥔다). */
    val viewing: VideoTransmitter? get() = transmitters.firstOrNull {
        it.state == ReceptionState.RECEIVING || it.state == ReceptionState.PENDING_RELEASE || it.state == ReceptionState.PENDING_REQUEST
    }
    /** 송출 허가 — 코어가 카메라·영상 호 마이크를 열었다. */
    val sending: Boolean get() = tx == TransmissionState.PERMITTED
    /** 영상을 보내는 중이거나 보내려는 중(요청·대기 포함) — D12 영상 우선의 판정. */
    val txBusy: Boolean get() = tx == TransmissionState.PERMITTED || tx == TransmissionState.PENDING_REQUEST ||
        tx == TransmissionState.QUEUED

    /** 코어 전송 제어 현재값으로 갈아 끼운다 — 끝난 송출은 빼고, 처음 본 송출의 시각을 적는다. */
    fun withTransmission(state: TransmissionState, queue: Int, list: List<VideoTransmitter>, nowMs: Long): VideoCall {
        val shown = list.filter { it.state != ReceptionState.ENDED }
        return copy(tx = state, queuePosition = queue, transmitters = shown,
            since = shown.associate { t -> t.userId to (since.entries.firstOrNull { sameUser(it.key, t.userId) }?.value ?: nowMs) })
    }
}

/** 영상 채널 한 곳의 연결 상태 — 내 멤버 그룹 중 MCVideo user profile 의 그룹 목록에 있는 것(§10.2). */
data class VideoChannel(
    val groupId: String,
    /** 호 방식(TS 24.481 `mcvideo-on-network-invite-members`) — `prearranged` | `chat`, 빈 값 = 아직 그룹 문서를 안 받음. */
    val type: String = "",
    /** 동시 송출 상한(그룹 문서) — 0 = 모름. */
    val maxTransmitters: Int = 0,
    /** MCVideo 로 affiliate 했다(TS 24.281 §8.2.1.2 — 편성 영상 호의 초대 대상). */
    val affiliated: Boolean = false,
    /** 연결되지 않는 까닭 한 줄(연결 중·편성 초대 대기·N2·N6 한도·실패와 재시도) — 영상 절 머리 옆에 선다. */
    val note: String = "",
) {
    val isPrearranged: Boolean get() = type == VideoRules.PREARRANGED
}

/** «새 영상» 배너 — 영상 채널의 새 송출 알림(Media Transmission Notification, TS 24.581 §6.2.5.3.2). 채널마다 하나(가장 최근 송출). */
data class VideoBanner(
    val groupId: String,
    val callId: Int,
    /** 송출자 MCVideo ID — [보기] 가 받는 대상. */
    val transmitterId: String,
    /** 그룹 이름. */
    val title: String,
    /** 송출자 표시(«김현장(현장지휘)»). */
    val who: String,
    val sinceMs: Long = System.currentTimeMillis(),
)

/** 영상 칸의 방향 — 보내는 사람마다 기억한다([DispatchSession.rotateVideo]). */
data class VideoOrient(
    /** 그림 회전(°, 시계 방향 0·90·180·270). */
    val rotation: Int = 0,
    /** 가로로 인코딩한 송출(4:3 — PC 관제 앱 640×480). 기본은 단말의 세로 480×640(3:4)이다. */
    val wide: Boolean = false,
) {
    /** 칸이 가로인가 — 가로 송출을 그대로 보거나, 세로 송출을 90°·270° 돌렸을 때. */
    val landscape: Boolean get() = wide != (rotation % 180 == 90)
}

/** 같은 사용자인가 — MCVideo ID 는 `tel:`·`sip:` 표기가 섞여 온다(번호부로 견준다). */
internal fun sameUser(a: String, b: String): Boolean = userPart(a).equals(userPart(b), ignoreCase = true)

// ── 순수 규칙 ────────────────────────────────────────────────────────────────

/** 영상 평면의 규칙 — 순수 함수라 JVM 에서 시험한다. 값은 데스크톱·현장 앱(`PttVideo`)과 같다. */
internal object VideoRules {
    const val CHAT = "chat"
    const val PREARRANGED = "prearranged"

    /** 합류 실패 뒤 첫 물러남(초) — 실패마다 배로, [BACKOFF_MAX_SEC] 까지. */
    const val BACKOFF_SEC = 10
    const val BACKOFF_MAX_SEC = 120
    /** 섰다가 끝난 영상 호(TNG3·서버 해제) 뒤 재합류까지(초). */
    const val REJOIN_SEC = 3
    /** 그룹 문서를 못 받았을 때 다시 받기까지(초). */
    const val DOC_RETRY_SEC = 30
    /** 같은 그룹의 영상을 보는 동안의 무전 수신 음량(D6 — 현장 앱 `VIDEO_DUCK` 과 같은 값). */
    const val DUCK_LEVEL = 0.3f

    const val PREARRANGED_NOTE = "편성 영상 그룹 — [영상 보내기] 로 영상 호를 엽니다"
    const val OPENING_NOTE = "영상 호를 여는 중 — 멤버가 받기를 기다립니다"
    /** 편성 채널인데 MCVideo 제휴가 아직 서지 않았다 — [영상 보내기] 가 꺼져 있는 까닭(초대 대상이 되려면 제휴가 서야 한다). */
    const val NOT_AFFILIATED_NOTE = "이 채널의 영상(MCVideo) 제휴가 아직 서지 않았습니다 — 잠시 뒤 다시"
    const val JOINING_NOTE = "영상 연결 중…"
    const val NO_PART_NOTE = "그룹 문서에 영상(MCVideo) 몫이 없습니다"
    const val DOC_FAIL_NOTE = "영상 그룹 문서를 받지 못했습니다 — 30초 뒤 다시"

    /** 영상을 쓸 수 있는가 — 사이트(ue-init-config MCVideo PSI, TS 24.484 §7.2.2.1) ∧ 자격(MCVideo user profile, §9.3). */
    fun enabled(psi: String, hasProfile: Boolean): Boolean = psi.isNotEmpty() && hasProfile

    /** [failures] 번째 연속 실패 뒤의 물러남(초) — 10 → 20 → 40 → 80 → 120(최대). */
    fun backoffSec(failures: Int): Int =
        minOf(BACKOFF_MAX_SEC, BACKOFF_SEC shl (failures - 1).coerceIn(0, 4))

    /**
     * 동시 제휴 그룹 한도 N2(user profile `<MaxAffiliationsN2>`) — 카드 순서대로 [n2] 개까지만 MCVideo 로 affiliate 한다.
     * 넘겨 보내면 서버가 PUBLISH 에서 줄이고(제휴 NOTIFY 에 없다) chat 합류는 486 Warning 102 다.
     *
     * @return (제휴할 그룹, 한도에 밀린 그룹)
     */
    fun affiliationPlan(wanted: List<String>, n2: Int): Pair<List<String>, List<String>> {
        val n = n2.coerceAtLeast(0)
        return wanted.take(n) to wanted.drop(n)
    }

    /** 동시 MCVideo 호 상한 N6(user profile `<MaxSimultaneousCallsN6>`) 안인가 — 넘겨 보내면 서버가 486 103. */
    fun withinN6(live: Int, n6: Int): Boolean = live < n6

    /** 0 이하·미기재는 상한 없음으로 본다. */
    fun limitOf(v: Int?): Int = if (v != null && v > 0) v else Int.MAX_VALUE

    /** 그룹 문서의 MCVideo 몫 → 호 방식. 몫이 없으면 빈 값(MCVideo 그룹이 아니다). */
    fun callType(attrs: McVideoGroupAttrs?): String = when {
        attrs == null -> ""
        attrs.inviteMembers -> PREARRANGED
        else -> CHAT
    }

    /** 그룹 문서의 지문 — 문서 ETag, 없으면 영상에 걸리는 값들. 같은 문서를 다시 받은 것은 변경이 아니다. */
    fun docPrint(etag: String, type: String, maxTransmitters: Int, members: List<String>): String =
        etag.ifEmpty { "$type|$maxTransmitters|${members.joinToString(",")}" }

    /** 문서가 **바뀌었나** — 처음 받은 것은 변경이 아니다(물러남·affiliation 을 다시 하지 않는다). */
    fun docChanged(old: String?, new: String): Boolean = old != null && old != new

    /**
     * 끝난 영상 호 뒤의 다음 걸음 — 채널은 남아 있다(D10).
     *
     * @param mine 관제사 쪽에서 끝냈다(그룹이 빠짐·자격 없어짐·로그아웃) — 다시 합류하지 않는다
     * @return null = 다시 합류하지 않는다 · true = 실패로 물러난다 · false = 곧 다시 합류한다([REJOIN_SEC])
     */
    fun rejoinAfterEnd(mine: Boolean, enabled: Boolean, isChannel: Boolean, type: String, connected: Boolean, lastCode: Int): Boolean? =
        if (mine || !enabled || !isChannel || type != CHAT) null else (!connected || lastCode >= 300)

    /** 같은 그룹 영상을 보는 동안 그 그룹 무전을 줄이는가(D6). */
    fun ducks(receiving: Boolean): Boolean = receiving

    /**
     * D12 영상 우선 — 영상을 보내는 동안 무전 발언 요청을 막는가. 긴급·임박 채널은 늘 음성이다(TS 22.280 R-8.3-004).
     */
    fun blocksTalk(policy: String, sendingVideo: Boolean, emergency: Boolean, imminentPeril: Boolean): Boolean =
        policy == VideoMicPolicy.VIDEO && sendingVideo && !emergency && !imminentPeril

    /**
     * D12 음성 우선 — 내가 무전 발언(요청·대기·발언) 중인 동안 영상 송출 호의 음성을 멈추는가. 영상 우선이라도 긴급·임박 발언은
     * 여기를 지난다(마이크가 두 호로 겹치지 않게).
     */
    fun yieldsMic(talking: Boolean, tx: TransmissionState): Boolean = talking && tx == TransmissionState.PERMITTED

    /** 무전 발언 중인가 — floor 요청·대기·발언. */
    fun talking(state: FloorState?): Boolean =
        state == FloorState.REQUESTING || state == FloorState.SPEAKING || state == FloorState.QUEUED

    /** [↻]/[↺] — 90° 씩(시계 방향 +90, 반시계 −90). */
    fun rotate(cur: Int, delta: Int): Int = ((cur + delta) % 360 + 360) % 360

    /** «새 영상» 배너가 아직 맞는가 — 그 송출이 알림 상태(Notified)로 남아 있을 때만. 보기 시작했거나 송출이 끝나면 내린다. */
    fun bannerValid(call: VideoCall?, transmitterId: String): Boolean =
        call != null && call.isLive &&
            call.transmitters.any { it.state == ReceptionState.NOTIFIED && sameUser(it.userId, transmitterId) }

    /** 송출 한 줄의 조작 글 — «보기»·«바꿔 보기»(다른 송출을 보고 있다)·진행 중. */
    fun actionText(state: ReceptionState, otherReceiving: Boolean): String = when (state) {
        ReceptionState.PENDING_REQUEST -> "요청 중…"
        ReceptionState.PENDING_RELEASE -> "끝내는 중…"
        else -> if (otherReceiving) "바꿔 보기" else "보기"
    }
}

/**
 * MCVideo 전송 제어 원인 → 문구(TS 24.581 §9.2.6.2 Transmission Rejected · §9.2.10.2 Revoked · §9.2.15.2 Receive Media Response).
 * 데스크톱 `ResponseText.VideoReceptionText`·`VideoTransmissionText`·`WithIGa` 와 같은 문장이다(dispatch_desktop_ui.md §10.4).
 */
internal object VideoText {
    /** 수신 거절·시한 — #7 = 동시에 볼 수 있는 영상 상한(1차 1개). */
    fun reception(kind: ReceptionEventKind, cause: Int, who: String): String = when {
        kind == ReceptionEventKind.REJECTED && cause == 7 -> "더 볼 수 없습니다 — 동시에 볼 수 있는 영상(1)이 찼습니다 · [바꿔 보기]"
        kind == ReceptionEventKind.REJECTED && cause == 255 -> "$who 영상을 볼 수 없습니다 — 송출이 이미 끝났습니다"
        kind == ReceptionEventKind.REQUEST_TIMEOUT -> "$who 영상 보기 요청에 응답이 없습니다"
        else -> "$who 영상을 볼 수 없습니다"
    }

    /** 내 송출 거절·회수. [maxTransmitters] = 그룹의 동시 송출 상한(모르면 0). */
    fun transmission(kind: TransmissionEventKind, cause: Int, maxTransmitters: Int): String = when {
        kind == TransmissionEventKind.REJECTED && cause == 1 ->
            if (maxTransmitters > 0) "보내지 못했습니다 — 동시에 보낼 수 있는 수($maxTransmitters)가 찼습니다"
            else "보내지 못했습니다 — 동시에 보낼 수 있는 수가 찼습니다"
        kind == TransmissionEventKind.REJECTED && cause == 5 -> "이 그룹에서는 영상을 받기만 할 수 있습니다"
        kind == TransmissionEventKind.REJECTED && cause == 3 -> "보내지 못했습니다 — 영상 호에 다른 참가자가 없습니다"
        kind == TransmissionEventKind.REVOKED && cause == 2 -> "보내기가 멈췄습니다 — 한 번에 보낼 수 있는 시간을 넘었습니다"
        kind == TransmissionEventKind.REVOKED && cause == 4 -> "보내기가 멈췄습니다 — 우선순위가 높은 송출이 들어왔습니다"
        kind == TransmissionEventKind.REVOKED -> "보내기가 멈췄습니다"
        else -> "보내지 못했습니다"
    }

    /** 주격 조사 이/가 — 받침이 있으면 «이»(«김현장이»), 없으면 «가»(«박경수가»). 숫자로 끝나면 읽는 소리로, 그 밖은 «이(가)». */
    fun withIGa(word: String): String {
        if (word.isEmpty()) return word
        var c = word.last()
        if (c == ')') { val i = word.lastIndexOf('('); if (i > 0) c = word[i - 1] }      // «김현장(현장지휘)» → 이름의 끝 글자
        val batchim: Boolean? = when (c) {
            in '가'..'힣' -> (c.code - 0xAC00) % 28 != 0
            '0', '1', '3', '6', '7', '8' -> true                                         // 영·일·삼·육·칠·팔
            '2', '4', '5', '9' -> false                                                  // 이·사·오·구
            else -> null
        }
        return word + when (batchim) { true -> "이"; false -> "가"; null -> "이(가)" }
    }

    fun n2Note(n2: Int): String =
        "동시 제휴 그룹 한도(N2 = $n2)가 찼습니다 — 이 채널 영상은 연결하지 않았습니다(운영자에게 한도 상향 요청)"

    fun n6Note(n6: Int): String =
        "동시 영상 호 한도(N6 = $n6)가 찼습니다 — 이 채널 영상은 연결하지 않았습니다(운영자에게 한도 상향 요청)"

    /** 합류 실패의 영상 절 안내 — 사유 + 다음 시도까지. */
    fun retryNote(code: Int, waitSec: Int): String =
        "영상 연결 안 됨 — ${if (code >= 300) ResponseText.sip(TextArea.VIDEO, code, "") else "호를 열지 못했습니다"} · ${waitSec}초 뒤 다시"

    /** mm:ss(1 시간 넘으면 h:mm:ss) — 이벤트 줄의 길이. */
    fun duration(ms: Long): String {
        val t = (ms / 1000).coerceAtLeast(0)
        return if (t >= 3600) "%d:%02d:%02d".format(t / 3600, (t % 3600) / 60, t % 60) else "%02d:%02d".format(t / 60, t % 60)
    }
}

// ── 상태 ─────────────────────────────────────────────────────────────────────

/** 영상 평면의 상태 — 세션이 한 벌 든다(`DispatchSession.video`). 메인 스레드 전용. */
/** 화면을 벗어난 뒤 송출을 끝내기까지 기다리는 시간 — 화면 재구성처럼 곧 돌아오는 것은 넘긴다. */
private const val VIDEO_BACKGROUND_GRACE_MS = 1_500L

internal class VideoPlaneState {
    val calls = MutableStateFlow<List<VideoCall>>(emptyList())
    val channels = MutableStateFlow<Map<String, VideoChannel>>(emptyMap())
    val banners = MutableStateFlow<List<VideoBanner>>(emptyList())
    /** 이 로그인의 PTT 계정에 MCVideo 를 실었다 — 계정 태그는 로그인 때 정한다(§10.2). */
    val enabled = MutableStateFlow(false)
    /** 엔진이 아는 카메라 이름(pjmedia `Front camera`·`Back camera`) — 비면 카메라가 없다([영상 보내기] 비활성). */
    val cameras = MutableStateFlow<List<String>>(emptyList())
    /** [보기] 를 한 줄로 세운다(`acceptVideo`) — «한 번에 하나». */
    val viewLock = kotlinx.coroutines.sync.Mutex()
    /** D12 — 내가 무전 발언 중이라 영상 송출 호의 음성을 멈춰 둔 동안 true. */
    val micYielded = MutableStateFlow(false)
    /** 영상 칸의 방향(송출자 번호 키) — 앱이 켜져 있는 동안 기억한다. */
    val orients = MutableStateFlow<Map<String, VideoOrient>>(emptyMap())

    /** MCVideo user profile(TS 24.484 §9.3) — 받으면 이용 자격이 있다(404 = 없음). 그룹 목록이 영상 채널. */
    var profile: McVideoUserProfileDoc? = null
    /** 합류 INVITE 를 보냈고 아직 영상 호가 서지 않은 그룹 — 같은 그룹에 두 번 보내지 않는다. */
    val joining = HashSet<String>()
    /** 다음 합류 시도 시각(그룹 id → ms) — 실패·예상 밖 종료 뒤 물러난다. */
    val retryAt = HashMap<String, Long>()
    val failures = HashMap<String, Int>()
    /** 관제사 쪽에서 끝낸 영상 호(그룹이 빠짐·자격 없어짐) — 다시 합류하지 않는다. */
    val leaving = HashSet<Int>()
    /** 내가 연 편성 영상 호(callId) — 성립 전에 끝나면 다시 열지 않고 사유만 알린다. */
    val opening = HashSet<Int>()
    /** 실패 토스트를 내지 않을 종료(내가 거둔 개시·내가 떠난 채널). */
    val quietEnd = HashSet<Int>()
    /** 마지막으로 맞춘 그룹 문서의 지문(그룹 id → 지문). */
    val docPrint = HashMap<String, String>()
    /** 마지막으로 문서를 받았을 때의 GMS 목록 ETag(그룹 id) — 목록이 같은 값을 주면 문서를 다시 받지 않는다. */
    val listEtag = HashMap<String, String>()
    /** N2 를 넘어 MCVideo 로 affiliate 하지 않은 영상 채널. */
    val overN2 = HashSet<String>()
    /** 영상을 받는 동안 수신 음량을 줄여 둔 무전(MCPTT 호 callId). */
    val ducked = HashSet<Int>()
    /** D12 음성 우선으로 음성을 멈춘 영상 호(callId). */
    val micMuted = HashSet<Int>()
    /** «새 영상» 으로 이미 알린 송출(callId → 송출자 번호) — 거절·그만 보기 뒤 Notified 로 돌아온 것을 다시 알리지 않는다. */
    val announced = HashMap<Int, MutableSet<String>>()
    /** 보고 있던 송출(callId, 송출자 번호) — 송출자가 멈추면 «멈췄습니다» 를 알린다. */
    val watched = HashSet<Pair<Int, String>>()
    var blockNotedAtMs = 0L
    /** 그룹 목록을 다시 받았다 — 다음 맞춤 때 영상 채널 목록(user profile)과 바뀐 그룹 문서도 다시 받는다. */
    var docsDirty = false
    /** 엔진의 지금 캡처 카메라가 뒤인가 — 엔진은 앞에서 시작하고 `switchCamera` 로만 바뀐다. 로그아웃해도 엔진이 남으므로 지우지 않는다. */
    var cameraBack = false

    /** 맞춤 요청 — 그룹·등록·문서 변화가 몰려도 마지막 상태로 한 번(재진입 없음). */
    val kick = Channel<Unit>(Channel.CONFLATED)
    /** 엔진 명령의 줄 — 보낸 순서대로 실행된다. */
    val cmd = Mutex()

    /** 로그아웃 — 영상 상태를 비운다. 방향 기억·카메라 추적은 남긴다(엔진과 수명이 같다). */
    fun reset() {
        enabled.value = false; profile = null
        calls.value = emptyList(); channels.value = emptyMap(); banners.value = emptyList()
        micYielded.value = false
        joining.clear(); retryAt.clear(); failures.clear(); leaving.clear(); opening.clear(); quietEnd.clear()
        docPrint.clear(); listEtag.clear(); overN2.clear(); ducked.clear(); micMuted.clear(); announced.clear(); watched.clear()
        docsDirty = false
    }
}

private const val TAG = "DispatchVideo"
/** MCVideo user profile 재조회 주기 — CMS 문서와 같은 5분(ETag 라 안 바뀌었으면 304). */
private const val VIDEO_PROFILE_POLL_MS = 300_000L
/** D12 영상 우선 — 발언을 막았다는 알림을 다시 내기까지. */
private const val BLOCK_NOTE_GAP_MS = 2_000L

// ── 화면이 읽는 것 ───────────────────────────────────────────────────────────

/** 영상 호 — 채널 상세 «영상» 절·카드 «영상 n» 의 원천. */
val DispatchSession.videoCalls: StateFlow<List<VideoCall>> get() = video.calls
/** 영상 채널의 연결 상태(그룹 id →). 영상 채널이 아닌 그룹은 없다. */
val DispatchSession.videoChannels: StateFlow<Map<String, VideoChannel>> get() = video.channels
/** «새 영상» 배너 — 최신 위. */
val DispatchSession.videoBanners: StateFlow<List<VideoBanner>> get() = video.banners
/** 엔진이 아는 카메라 — 설정 [영상]·[영상 보내기] 활성 판정. */
val DispatchSession.videoCameras: StateFlow<List<String>> get() = video.cameras
/** D12 음성 우선 — 무전을 말하는 동안 영상 호 소리를 멈췄다(«무전 중 — 영상은 계속» 안내). */
val DispatchSession.videoMicYielded: StateFlow<Boolean> get() = video.micYielded
/** 영상 칸 방향 기억(송출자 번호 →). */
val DispatchSession.videoOrients: StateFlow<Map<String, VideoOrient>> get() = video.orients

fun DispatchSession.videoOfGroup(groupId: String): VideoCall? = video.calls.value.firstOrNull { it.groupId == groupId && it.isLive }

/** 송출자 표시 — 기능 별칭이 있으면 «이름(별칭)». */
fun DispatchSession.videoSenderName(t: VideoTransmitter): String =
    displayName(t.userId).let { if (t.functionalAlias.isNotEmpty()) "$it(${t.functionalAlias})" else it }

/** 번호 → 이름(주소록) — 영상 절의 송출 줄·캡션. */
fun DispatchSession.videoNameOf(userId: String): String = displayName(userId)

/**
 * 편성(prearranged) 영상 채널에 영상 호가 없다 — [영상 보내기] 가 영상 호를 연다. 초대 대상이 되려면 MCVideo 제휴가 서 있어야 한다.
 */
fun DispatchSession.canOpenVideo(groupId: String): Boolean {
    val v = video
    val ch = v.channels.value[groupId] ?: return false
    return v.enabled.value && ch.isPrearranged && ch.affiliated && videoOfGroup(groupId) == null && groupId !in v.joining &&
        v.cameras.value.isNotEmpty() && pttRegistered() && groups.value.any { it.id == groupId && it.isMember && it.mcVideo }
}

// ── 기동·계정 ────────────────────────────────────────────────────────────────

private fun DispatchSession.videoPsi(): String = ueInitDoc()?.mcvideoServerUri.orEmpty()

private fun DispatchSession.pttRegistered(): Boolean =
    pttAccount?.let { registrations.value[it.id]?.state == RegState.REGISTERED } == true

private fun DispatchSession.n2(): Int = VideoRules.limitOf(video.profile?.maxAffiliationsN2)
private fun DispatchSession.n6(): Int = VideoRules.limitOf(video.profile?.maxSimultaneousCallsN6)

/**
 * 멤버 그룹을 **카드 순서**(서버 GMS 목록 순 — 내 채널 카드·데스크톱과 같다)로 — N2·N6 한도는 카드 순서대로 채운다. 이름순으로
 * 세우면 한도를 넘을 때 데스크톱과 다른 채널에 영상이 붙고 «한도가 찼습니다» 가 앞쪽 카드에 선다.
 */
private fun DispatchSession.memberGroups(): List<GroupInfo> = groups.value.filter { it.isMember }

/** 엔진 명령을 줄 세워 보낸다 — 먼저 건 것이 먼저 간다(잠금은 FIFO). */
private suspend fun <T> DispatchSession.videoCmd(block: suspend () -> CimsResult<T>): CimsResult<T> =
    video.cmd.withLock { block() }

/** 세션 스코프에서 돌린다 — 예외 하나가 스코프를 무너뜨리지 않게 받는다(취소는 그대로 전한다). */
private fun DispatchSession.videoLaunch(what: String, block: suspend () -> Unit) = scopeLaunch {
    try { block() } catch (e: CancellationException) { throw e } catch (t: Throwable) { android.util.Log.w(TAG, what, t) }
}

/**
 * 기동 때 한 번 — MCVideo 를 쓸 수 있는지 정한다(§10.2). 사이트가 MCVideo PSI 를 광고하고(ue-init-config, TS 24.484 §7.2.2.1)
 * 이용 자격(MCVideo user profile §9.3)이 있을 때만 PTT 계정에 싣는다. 계정 태그는 로그인 때 한 번 — 자격이 뒤에 바뀌면 다음
 * 로그인에 반영된다(없어지면 영상 채널은 곧바로 나간다 — [syncVideo]).
 */
internal suspend fun DispatchSession.prepareVideo() {
    val v = video
    v.profile = null
    refreshVideoCameras()
    if (videoPsi().isNotEmpty()) fetchVideoProfile()
    v.enabled.value = VideoRules.enabled(videoPsi(), v.profile != null)
    android.util.Log.i(TAG, "mcvideo enabled=${v.enabled.value} psi=${videoPsi()} cameras=${v.cameras.value}")
}

/**
 * PTT 계정에 MCVideo 를 싣는다 — REGISTER Contact 태그(TS 24.281 §7.2.1AA) · 참여 기능 PSI · 편성 초대 자동 합류(수락은 세션 합류일
 * 뿐 영상 보기는 [보기] 때만 — §6.2.3.1.2).
 */
internal fun DispatchSession.videoAccount(cfg: AccountConfig): AccountConfig =
    if (!video.enabled.value) cfg
    else cfg.copy(mcvideoEnabled = true, mcvideoServerUri = videoPsi(), autoAnswerMcvideo = true)

/** 엔진이 아는 카메라(합성 색 막대 제외). 장치는 엔진 기동 때 한 번 열거한다. */
private suspend fun DispatchSession.refreshVideoCameras() {
    val engine = engineOrNull() ?: return
    video.cameras.value = engine.videoDevices().filter { it.capture && it.driver.equals("Android", ignoreCase = true) }.map { it.name }
}

/**
 * MCVideo user profile 재조회(ETag). 404 = 자격 없음(사본을 버린다), 그 밖의 실패(망·5xx)는 가진 사본을 둔다.
 * @return 판정이 섰다(받음·304·404)
 */
private suspend fun DispatchSession.fetchVideoProfile(): Boolean {
    val c = cscOrNull() ?: return false
    val me = myPttId.ifEmpty { return false }
    if (videoPsi().isEmpty()) return false
    val token = accessToken() ?: return false
    val gen = loginGeneration.value
    val r = c.fetchMcVideoUserProfile(token, me, video.profile?.etag.orEmpty())
    if (gen != loginGeneration.value) return false                 // 받는 사이 로그아웃 — 앞 사람의 문서를 남기지 않는다
    when {
        r.ok -> r.value?.let { video.profile = it }                // null = 304(가진 사본 그대로)
        r.code == 404 -> video.profile = null
        else -> { android.util.Log.w(TAG, "mcvideo user-profile: ${r.code} ${r.reason}"); return false }
    }
    android.util.Log.i(TAG, "mcvideo user-profile " + (video.profile?.let {
        "groups=${it.groups.size} streams=${it.maxSimultaneousVideoStreams} n2=${it.maxAffiliationsN2} n6=${it.maxSimultaneousCallsN6}"
    } ?: "none(자격 없음)"))
    return true
}

/** 코어 이벤트·주기를 붙인다 — 엔진이 선 뒤 `observe()` 가 한 번 부른다. */
internal fun DispatchSession.observeVideo(engine: CimsUe) {
    // 이벤트 하나의 예외가 수집을 끝내지 않게 건마다 받는다.
    fun guard(what: String, block: () -> Unit) = runCatching(block).onFailure { android.util.Log.w(TAG, what, it) }
    videoLaunch("transmission") { engine.transmission.collect { guard("transmission") { applyVideoTransmission(it) } } }   // TS 24.581 §6.2.4
    videoLaunch("reception") { engine.reception.collect { guard("reception") { applyVideoReception(it) } } }               // §6.2.5
    // 화면을 벗어나면 보내던 영상을 끝낸다(처음 값은 지금 상태라 건너뛴다 — 화면 없이 기동했을 때 헛알림을 내지 않는다)
    //   잠깐 내려갔다 곧 돌아오는 것(화면 재구성)은 벗어난 것이 아니다 — 잠시 기다려 그대로일 때만 끝낸다.
    videoLaunch("presence") {
        UiPresence.visible.drop(1).collectLatest { visible ->
            if (!visible) { delay(VIDEO_BACKGROUND_GRACE_MS); releaseVideoOnBackground() }
        }
    }
    // 맞춤은 한 줄에서 — 요청이 몰려도 마지막 상태로 한 번.
    scopeLaunch {
        for (u in video.kick) {
            try { syncVideo() } catch (e: CancellationException) { throw e } catch (t: Throwable) { android.util.Log.w(TAG, "sync", t) }
        }
    }
    // 영상 채널 합류(D10)는 PTT 등록 뒤. 등록이 **새로** 선 때의 MCVideo 제휴 재적재는 유지 평면이 한다([renewVideoAffiliations] —
    //   등록 상태가 줄곧 «등록됨» 으로만 보이는 망 전환은 이 스냅샷 흐름에서 보이지 않는다, §6.7a).
    videoLaunch("registered") {
        registrations.map { pttRegistered() }.distinctUntilChanged().collect { on -> if (on) requestVideoSync() }
    }
    // D12 음성 우선 — 무전 발언(요청·대기·발언)이 시작되고 끝날 때.
    videoLaunch("talking") {
        sessions.map { list -> list.any { it.isLive && it.info.isMcptt && VideoRules.talking(it.floor?.state) } }
            .distinctUntilChanged().collect { syncVideoMic() }
    }
    // 재합류 시각이 된 채널(1초 틱).
    videoLaunch("tick") { while (true) { delay(1000); tickVideo() } }
    // 영상 채널 목록(user profile 의 그룹 목록) — CMS 문서와 같은 주기.
    videoLaunch("profile") {
        while (true) {
            delay(VIDEO_PROFILE_POLL_MS)
            if (isReady && video.enabled.value && pttAccount != null && fetchVideoProfile()) requestVideoSync()
        }
    }
}

/**
 * 영상 채널을 다시 맞춘다. [groupsRefreshed] = 그룹 목록을 방금 다시 받았다(로그인·GMS xcap-diff·편성 재조회) — 영상 채널 목록과
 * 바뀐 그룹 문서도 다시 받는다: 그룹에 영상을 켜거나 멤버로 편성되면 그 그룹이 곧바로 영상 채널이 되어 영상 호에 든다(5분 주기만
 * 기다리면 그동안 그 그룹의 송출 알림이 오지 않는다).
 */
internal fun DispatchSession.requestVideoSync(groupsRefreshed: Boolean = false) {
    if (groupsRefreshed) video.docsDirty = true
    video.kick.trySend(Unit)
}

/**
 * PTT 등록이 새로 섰다 — 서버의 MCVideo 제휴는 등록에 묶여 있어 등록과 함께 내려갔다(현장 앱 `PttVideo` 와 같은 규칙). 실은 것으로
 * 적어 둔 표시를 지우고 다시 맞춘다 — 코어가 관심 그룹 집합을 다시 PUBLISH 한다(TS 24.281 §8.2.1.2). 유지 평면이 부른다.
 */
internal fun DispatchSession.renewVideoAffiliations() {
    video.channels.value = video.channels.value.mapValues { it.value.copy(affiliated = false) }
    requestVideoSync()
}

/** 로그아웃 — 재합류하지 않게 **먼저** 끊는다(호는 `logout()` 이 끊는다). */
internal fun DispatchSession.resetVideo() {
    video.reset()
    videoLaunch("surface") { engineOrNull()?.let { e -> videoCmd { e.setVideoSurface(null) } } }
}

// ── 영상 채널 맞추기(§10.2) ──────────────────────────────────────────────────

private fun DispatchSession.setChannel(id: String, f: (VideoChannel) -> VideoChannel) {
    val cur = video.channels.value
    val next = f(cur[id] ?: return)
    if (next != cur[id]) video.channels.value = cur + (id to next)
}

private fun DispatchSession.markVideoGroup(id: String, on: Boolean) {
    if (groups.value.any { it.id == id && it.mcVideo != on }) updateGroup(id) { it.copy(mcVideo = on) }
}

/**
 * 한 번의 맞춤 — 영상 채널 표시·MCVideo affiliation 을 user profile 그룹 목록에 맞추고(§10.2), 바뀐 그룹 문서를 다시 받고,
 * 영상 호 합류를 맞춘다. 부르는 곳 = 그룹 목록 갱신·PTT 등록 성공·영상 호 종료·재시도 시각·user profile 변경.
 */
private suspend fun DispatchSession.syncVideo() {
    val v = video
    val gen = loginGeneration.value
    val docs = v.docsDirty
    v.docsDirty = false
    if (docs && v.enabled.value) fetchVideoProfile()
    if (gen != loginGeneration.value) return
    applyVideoGroups(gen)
    if (gen != loginGeneration.value) return
    if (docs) refreshVideoDocs(gen)
    if (gen != loginGeneration.value) return
    ensureVideoChannels(gen)
}

/**
 * 멤버 그룹의 영상 채널 표시·MCVideo affiliation 을 맞춘다. affiliation 은 관심 그룹 전부를 한 PUBLISH 로 — 코어가 집합을 들고
 * 호출마다 다시 싣는다(TS 24.281 §8.2.1.2). **켜는 그룹을 먼저, 끄는 그룹을 뒤에** — 먼저 끄면 빈 집합(Expires 0 = 그 사용자 MCVideo
 * 제휴 전부 해제)을 한 번 거친다. 영상 채널이 아니게 된 그룹(자격·user profile 에서 빠짐, 그룹 삭제·멤버 제외로 내 채널에서 빠짐)은
 * 영상 호를 끊고 이어서 제휴도 푼다 — chat 합류의 암묵적 제휴(§9.2.2.4.1.1 12))는 나갈 때 풀어 주는 절차가 규격에 없어(해제는
 * 클라이언트 몫 — §8.2.1.2) 그대로 두면 옮겨 다닌 채널이 쌓여 N2 를 넘는다(새 채널 합류 = 486 Warning 102).
 */
private suspend fun DispatchSession.applyVideoGroups(gen: Int) {
    val v = video
    val ptt = pttAccount
    val ids = v.profile?.groups.orEmpty().map(::userPart).toSet()
    val wanted = if (v.enabled.value) memberGroups().filter { it.id in ids }.map { it.id } else emptyList()
    val (within, over) = VideoRules.affiliationPlan(wanted, n2())
    for (id in within) {
        markVideoGroup(id, true)
        if (id !in v.channels.value) v.channels.value = v.channels.value + (id to VideoChannel(id))
        if (v.overN2.remove(id)) setChannel(id) { it.copy(note = "") }
        if (ptt == null || v.channels.value[id]?.affiliated == true) continue
        val r = videoCmd { ptt.affiliate(id, true, McService.MCVIDEO) }
        if (gen != loginGeneration.value) return
        if (!r.ok) { android.util.Log.w(TAG, "mcvideo affiliate $id true: ${r.code} ${r.reason}"); continue }
        setChannel(id) { it.copy(affiliated = true) }
    }
    for (id in over) {
        markVideoGroup(id, true)
        if (id !in v.channels.value) v.channels.value = v.channels.value + (id to VideoChannel(id))
        if (v.overN2.add(id)) android.util.Log.w(TAG, "mcvideo channel $id: not affiliated — N2 ${n2()} reached")
        setChannel(id) { it.copy(note = VideoText.n2Note(n2())) }
    }
    val keep = wanted.toSet()
    for (id in v.channels.value.keys - keep) {
        leaveVideoChannel(id)
        if (gen != loginGeneration.value) return
        markVideoGroup(id, false)
        v.channels.value = v.channels.value - id
        v.overN2.remove(id)
    }
    groups.value.filter { it.mcVideo && it.id !in keep }.forEach { markVideoGroup(it.id, false) }
    // 영상 채널이 아닌 그룹의 영상 호(편성 초대를 받았다) — 붙을 «영상» 절이 없다. 나간다.
    v.calls.value.filter { it.isLive && it.groupId !in keep && it.callId !in v.leaving }.forEach { hangupVideoQuietly(it.callId) }
    for (id in over) if (v.channels.value[id]?.affiliated == true) {       // 한도가 줄어 밀려난 채널
        leaveVideoChannel(id)
        if (gen != loginGeneration.value) return
    }
}

/** 관제사 쪽에서 끝낸다 — 다시 합류하지 않고 실패 토스트도 내지 않는다. */
private fun DispatchSession.hangupVideoQuietly(callId: Int) {
    val v = video
    v.leaving.add(callId); v.quietEnd.add(callId)
    videoLaunch("hangup") { engineOrNull()?.let { e -> videoCmd { e.call(callId).hangup() } } }
}

/**
 * 영상 채널에서 나간다(그룹이 내 채널에서 빠짐·자격 없어짐) — 영상 호 BYE(MCPTT 호는 그대로 — TS 24.281 §6.2.4.1) 뒤에 MCVideo
 * 제휴를 푼다. 다시 합류하지 않는다.
 */
private suspend fun DispatchSession.leaveVideoChannel(id: String) {
    val v = video
    val engine = engineOrNull()
    v.calls.value.filter { it.groupId == id && it.isLive }.forEach { c ->
        v.leaving.add(c.callId); v.quietEnd.add(c.callId)
        if (engine != null) videoCmd { engine.call(c.callId).hangup() }
    }
    v.joining.remove(id); v.retryAt.remove(id); v.failures.remove(id); v.docPrint.remove(id); v.listEtag.remove(id)
    val ptt = pttAccount
    if (v.channels.value[id]?.affiliated == true && ptt != null) {
        val r = videoCmd { ptt.affiliate(id, false, McService.MCVIDEO) }
        if (!r.ok) android.util.Log.w(TAG, "mcvideo affiliate $id false: ${r.code} ${r.reason}")
    }
    setChannel(id) { it.copy(affiliated = false) }
}

/**
 * 그룹 문서의 MCVideo 몫(호 방식·동시 송출 상한)을 받는다 — 합류 INVITE 의 session-type 이 그룹 종류와 어긋나면 서버가 404 117/118 이다
 * (TS 24.281 §6.3.5.2).
 */
private suspend fun DispatchSession.loadVideoDoc(g: GroupInfo, gen: Int) {
    val r = getGroupDoc(g.uri)
    if (gen != loginGeneration.value) return
    val doc = r.value
    if (!r.ok || doc == null) {
        android.util.Log.w(TAG, "mcvideo attrs ${g.id}: ${r.code} ${r.reason}")
        if (video.channels.value[g.id]?.type.isNullOrEmpty()) {
            setChannel(g.id) { it.copy(note = VideoRules.DOC_FAIL_NOTE) }
            video.retryAt[g.id] = System.currentTimeMillis() + VideoRules.DOC_RETRY_SEC * 1000L
        }
        return
    }
    video.listEtag[g.id] = g.etag
    applyVideoDoc(g.id, doc, gen)
}

/**
 * 그룹 문서의 MCVideo 몫을 그 채널에 맞춘다 — 호 방식·동시 송출 상한이 바뀌었으면 갱신하고, **문서가 바뀌었으면**(지문 = 문서 ETag —
 * 처음 받은 것과 같은 문서 재조회는 변경이 아니다) 그 그룹의 합류 물러남을 지우고 MCVideo affiliation 을 다시 싣는다. 갓 만든 그룹은
 * 서버가 아직 MCVideo 그룹으로 모를 때 합류가 거절되거나(404 Warning 113) affiliation 이 기록되지 않을 수 있는데(PUBLISH 는 200 —
 * 결과는 NOTIFY 로만 온다, TS 24.281 §8.2.2.2.3) 서버는 그룹을 다시 적재한 뒤 문서 변경을 통지한다 — 그때 다시 맞춘다.
 */
private suspend fun DispatchSession.applyVideoDoc(id: String, doc: GroupDoc, gen: Int) {
    val v = video
    val type = VideoRules.callType(doc.mcvideo)
    val max = doc.mcvideo?.maxTransmitters ?: 0
    val print = VideoRules.docPrint(doc.etag, type, max, doc.members.map { it.uri })
    val changed = VideoRules.docChanged(v.docPrint[id], print)
    v.docPrint[id] = print
    val was = v.channels.value[id] ?: return
    if (type != was.type || max != was.maxTransmitters || changed)
        // 영상 채널이 왜 연결되거나 안 되는지 로그에서 가른다 — chat = 앱이 합류, 편성 = [영상 보내기] 가 열거나 멤버의 초대를 받는다
        android.util.Log.i(TAG, "mcvideo channel $id: ${type.ifEmpty { "(no MCVideo part)" }} max-tx=$max" +
            if (changed) " — document changed" else "")
    setChannel(id) {
        it.copy(type = type, maxTransmitters = max, note = when {
            type.isEmpty() -> VideoRules.NO_PART_NOTE
            videoOfGroup(id) == null && id !in v.joining && id !in v.overN2 ->
                if (type == VideoRules.PREARRANGED) VideoRules.PREARRANGED_NOTE else ""
            else -> it.note
        })
    }
    if (!changed) return
    v.retryAt.remove(id); v.failures.remove(id)
    val ptt = pttAccount ?: return
    if (id in v.overN2) return
    val r = videoCmd { ptt.affiliate(id, true, McService.MCVIDEO) }
    if (gen != loginGeneration.value) return
    if (r.ok) setChannel(id) { it.copy(affiliated = true) }
    else android.util.Log.w(TAG, "mcvideo re-affiliate $id: ${r.code} ${r.reason}")
}

/**
 * 그룹 문서가 바뀌었을 수 있다 — 영상 채널의 그룹 문서를 다시 받아 맞춘다: 콘솔에서 호 방식(편성 ↔ chat)·동시 송출 상한을 바꾸면
 * 로그인 중에도 따라간다. GMS 목록이 준 ETag 가 마지막으로 받았을 때와 같으면 받지 않는다(목록이 ETag 를 주지 않으면 늘 받는다).
 */
private suspend fun DispatchSession.refreshVideoDocs(gen: Int) {
    val v = video
    if (!v.enabled.value) return
    for (g in memberGroups().filter { it.mcVideo }) {
        val ch = v.channels.value[g.id] ?: continue
        if (ch.type.isNotEmpty() && g.etag.isNotEmpty() && v.listEtag[g.id] == g.etag) continue
        loadVideoDoc(g, gen)
        if (gen != loginGeneration.value) return
    }
}

/**
 * 내 채널의 영상 채널마다 영상 호를 맞춘다(D10) — chat 은 합류(INVITE, 합류가 곧 affiliation — TS 24.281 §9.2.2), 편성은 초대를
 * 기다린다(멤버 초대 자동 수락 — §9.2.1.3; 앱이 호를 열어 두지 않는다 — 전원 초대라 보낼 사람이 연다, [openVideoTx]). 동시 MCVideo 호
 * 상한 N6 안에서 카드 순서대로, PTT 등록 뒤에만.
 */
private suspend fun DispatchSession.ensureVideoChannels(gen: Int) {
    val v = video
    val ptt = pttAccount ?: return
    if (!v.enabled.value || !pttRegistered()) return
    for (g in memberGroups().filter { it.mcVideo }) {
        val id = g.id
        if (id !in v.channels.value || videoOfGroup(id) != null || id in v.joining || id in v.overN2) continue
        if ((v.retryAt[id] ?: 0L) > System.currentTimeMillis()) continue
        if (v.channels.value[id]?.type.isNullOrEmpty()) {                  // 호 방식을 안 뒤에 합류한다
            loadVideoDoc(g, gen)
            if (gen != loginGeneration.value) return
        }
        val ch = v.channels.value[id] ?: continue
        if (ch.type.isEmpty()) continue
        if (ch.isPrearranged) { setChannel(id) { it.copy(note = VideoRules.PREARRANGED_NOTE) }; continue }
        val live = v.calls.value.count { it.isLive } + v.joining.size
        if (!VideoRules.withinN6(live, n6())) {
            val note = VideoText.n6Note(n6())
            if (ch.note != note) android.util.Log.w(TAG, "mcvideo channel $id: not joined — N6 ${n6()} reached ($live live)")
            setChannel(id) { it.copy(note = note) }
            continue
        }
        v.joining.add(id)
        setChannel(id) { it.copy(note = VideoRules.JOINING_NOTE) }
        val r = videoCmd { ptt.joinVideoGroupCall(id, VideoGroupCallOptions(prearranged = false, queueing = true)) }
        if (gen != loginGeneration.value) {                                // 합류하는 사이 로그아웃 — 남기지 않는다
            r.value?.let { c -> videoCmd { c.hangup() } }
            return
        }
        val call = r.value
        if (!r.ok || call == null) {
            android.util.Log.w(TAG, "mcvideo join $id: ${r.code} ${r.reason}")
            if (v.joining.remove(id)) videoBackoff(id, failed = true, code = r.code)
            continue
        }
        // 호 정보는 엔진에 다시 묻는다 — 돌려받은 핸들은 첫 호 이벤트보다 먼저 만들어졌으면 낡아 정보가 비어 있다(성공한 합류가
        //   «연결 안 됨» 으로 잠깐 선다)
        adoptVideoCall(call.id, id, engineOrNull()?.callInfo(call.id) ?: call.info)
    }
}

/**
 * 방금 건 영상 호를 목록에 올린다 — 호 상태 이벤트보다 명령 결과가 먼저 오면 여기서, 이벤트가 먼저 왔으면 이미 올라 있다.
 * 그 사이 이미 끝난 호면(이벤트가 먼저 지나갔다) 합류 표시만 걷는다.
 */
private fun DispatchSession.adoptVideoCall(callId: Int, groupId: String, info: CallInfo?) {
    val v = video
    if (v.calls.value.any { it.callId == callId }) return
    if (info != null && info.state != CallState.DISCONNECTED && info.state != CallState.NULL) { takeVideoCall(info); return }
    if (v.joining.remove(groupId)) videoBackoff(groupId, failed = true)
}

/** 합류 실패·예상 밖 종료 — 다음 시도까지 물러난다(10 s → 최대 2 분). 정상 종료(예: TNG3 해제) 뒤 재합류는 3 초. */
private fun DispatchSession.videoBackoff(id: String, failed: Boolean, code: Int = 0) {
    val v = video
    val wait: Int
    if (failed) {
        val n = (v.failures[id] ?: 0) + 1
        v.failures[id] = n
        wait = VideoRules.backoffSec(n)
        setChannel(id) { it.copy(note = VideoText.retryNote(code, wait)) }
    } else {
        v.failures.remove(id)
        wait = VideoRules.REJOIN_SEC
    }
    v.retryAt[id] = System.currentTimeMillis() + wait * 1000L
}

/** 1초 틱 — 재합류 시각이 된 채널. */
private fun DispatchSession.tickVideo() {
    val v = video
    if (!v.enabled.value || v.retryAt.isEmpty()) return
    val now = System.currentTimeMillis()
    val due = v.retryAt.filterValues { now >= it }.keys.toList()
    if (due.isEmpty()) return
    due.forEach { v.retryAt.remove(it) }
    requestVideoSync()
}

// ── 호 투영 ──────────────────────────────────────────────────────────────────

private fun DispatchSession.updateVideoCall(callId: Int, f: (VideoCall) -> VideoCall) {
    val cur = video.calls.value
    if (cur.none { it.callId == callId }) return
    video.calls.value = cur.map { if (it.callId == callId) f(it) else it }
}

/**
 * 호 스냅샷 들머리 — **MCVideo 호면 영상 평면이 받고 true**(세션 목록에 넣지 않는다). 아니면 false 다 — 무전(MCPTT 그룹 호)이면
 * 영상을 보는 중의 음량 줄임(D6)을 다시 판정해 둔다.
 */
internal fun DispatchSession.takeVideoCall(ci: CallInfo): Boolean {
    if (ci.service != McService.MCVIDEO) { noteVoiceCall(ci); return false }
    val v = video
    val gid = userPart(ci.groupId)
    val ended = ci.state == CallState.DISCONNECTED || ci.state == CallState.NULL
    val cur = v.calls.value.firstOrNull { it.callId == ci.callId }
    when {
        ended && cur != null -> onVideoCallEnded(cur.copy(info = ci))
        // 목록에 오르기 전에 끝난 합류 — 물러남·사유는 같은 길로 낸다.
        ended && (gid in v.joining || ci.callId in v.opening) ->
            onVideoCallEnded(VideoCall(ci.callId, gid, ci, opening = ci.callId in v.opening))
        // 목록에 없는 호의 끝(로그아웃 뒤·중복으로 나간 호) — 호 번호는 다시 쓰이므로 표시를 남기지 않는다.
        ended -> { v.leaving.remove(ci.callId); v.quietEnd.remove(ci.callId) }
        cur == null -> onVideoCallAdded(ci, gid)
        else -> {
            val activated = !cur.isActive && ci.state == CallState.ACTIVE
            updateVideoCall(ci.callId) {
                it.copy(info = ci, connectedAtMs = it.connectedAtMs ?: if (ci.state == CallState.ACTIVE) System.currentTimeMillis() else null)
            }
            if (activated) onVideoCallActive(ci.callId, gid)
        }
    }
    return true
}

/**
 * 화면 복귀·재구성 — 코어 스냅샷에서 영상 호를 갈라 다시 맞춘다. 살아 있는 호 중 **MCVideo 가 아닌 것**을 돌려준다(세션 목록 몫).
 * 그 사이 이벤트 없이 사라진 영상 호는 끝난 것으로 본다.
 */
internal fun DispatchSession.resyncVideoCalls(live: List<CallInfo>): List<CallInfo> {
    val (videoCalls, others) = live.partition { it.service == McService.MCVIDEO }
    val ids = videoCalls.map { it.callId }.toSet()
    video.calls.value.filter { it.callId !in ids }.forEach { gone ->
        onVideoCallEnded(gone.copy(info = gone.info.copy(state = CallState.DISCONNECTED)))
    }
    videoCalls.forEach { takeVideoCall(it); pullTransmission(it.callId) }
    return others
}

/** 영상 호가 생겼다 — 그 그룹의 «영상» 절에 붙는다(카드를 새로 만들지 않는다). */
private fun DispatchSession.onVideoCallAdded(ci: CallInfo, gid: String) {
    val v = video
    // 같은 그룹의 영상 호가 이미 있다(내가 여는 사이 멤버의 초대가 왔다) — 한 그룹에 영상 호 하나, 새것을 나간다.
    if (v.calls.value.any { it.groupId == gid && it.isLive && it.callId != ci.callId }) { hangupVideoQuietly(ci.callId); return }
    // 영상 채널이 아닌 그룹의 초대 — 붙을 «영상» 절이 없다(자격의 그룹 목록에 없으면 곧바로, 있으면 다음 맞춤이 가른다).
    if (gid !in v.profile?.groups.orEmpty().map(::userPart)) { hangupVideoQuietly(ci.callId); return }
    v.joining.remove(gid)
    val opening = ci.callId in v.opening
    val active = ci.state == CallState.ACTIVE
    v.calls.value = v.calls.value + VideoCall(ci.callId, gid, ci, connectedAtMs = if (active) System.currentTimeMillis() else null, opening = opening)
    setChannel(gid) { it.copy(note = if (opening) VideoRules.OPENING_NOTE else "") }
    android.util.Log.i(TAG, "mcvideo call + #${ci.callId} $gid " +
        if (ci.dir == CallDir.INCOMING) "invited" else if (opening) "opening" else "joined")
    pullTransmission(ci.callId)
    if (active) onVideoCallActive(ci.callId, gid)
}

private fun DispatchSession.onVideoCallActive(callId: Int, gid: String) {
    video.failures.remove(gid)
    setChannel(gid) { it.copy(note = "") }
    addActivity(gid, groupNameOf(gid), "영상 호 연결", ActivityKind.VIDEO)
    pullTransmission(callId)
    syncVideoMic()
}

private fun DispatchSession.onVideoCallEnded(call: VideoCall) {
    val v = video
    val ci = call.info
    val gid = call.groupId
    val mine = v.leaving.remove(call.callId)
    val opened = v.opening.remove(call.callId)                     // 내가 연 편성 호 — 성립 전에 끝났어도 다시 열지 않는다
    val quiet = v.quietEnd.remove(call.callId)
    v.banners.value = v.banners.value.filterNot { it.callId == call.callId }
    v.calls.value = v.calls.value.filterNot { it.callId == call.callId }
    v.micMuted.remove(call.callId); v.announced.remove(call.callId); v.watched.removeAll { it.first == call.callId }
    // 그 그룹의 영상 호가 따로 살아 있다(중복으로 나간 호의 끝) — 채널의 상태는 남은 호가 말한다.
    if (v.calls.value.any { it.groupId == gid && it.isLive }) return
    v.joining.remove(gid)
    unduck(gid)
    val connected = call.connectedAtMs != null
    android.util.Log.i(TAG, "mcvideo call - #${call.callId} $gid code=${ci.lastCode} ${ci.lastReason} mine=$mine opened=$opened connected=$connected")
    val title = groupNameOf(gid)
    if (connected) addActivity(gid, title,
        "영상 호 종료 · ${VideoText.duration(System.currentTimeMillis() - (call.connectedAtMs ?: call.startedAtMs))}", ActivityKind.VIDEO)
    // 서지 못한 내 발신의 사유(§9 사전) — 내가 거둔 개시·내가 떠난 채널은 조용히. 자동 합류는 연속 실패의 처음만 알린다
    //   (물러나며 되풀이하는 같은 사유로 토스트가 쌓이지 않게 — 그 뒤는 영상 절 안내가 말한다).
    if (!connected && ci.dir == CallDir.OUTGOING && ci.lastCode >= 300 && !quiet && (opened || (v.failures[gid] ?: 0) == 0)) {
        notify(NoticeLevel.ERROR, "$title — ${ResponseText.sip(TextArea.VIDEO, ci.lastCode, ci.lastReason)}", "${ci.lastCode} ${ci.lastReason}".trim())
        addActivity(gid, title, "영상 호 실패 ${ci.lastCode}", ActivityKind.ERROR)
    }
    val ch = v.channels.value[gid]
    if (ch != null) {
        // 채널은 남아 있다(D10) — 관제사가 끝낸 게 아니면 다시 합류한다(chat). 서지 못한 합류는 물러나고, 섰다가 끝난 호는 곧바로.
        val isChannel = groups.value.any { it.id == gid && it.isMember && it.mcVideo }
        when (val failed = VideoRules.rejoinAfterEnd(mine, v.enabled.value, isChannel, ch.type, connected, ci.lastCode)) {
            null -> if (ch.isPrearranged) setChannel(gid) { it.copy(note = VideoRules.PREARRANGED_NOTE) }
            else -> videoBackoff(gid, failed, ci.lastCode)
        }
    }
    syncVideoMic()
}

/** 코어 전송 제어 현재값을 다시 읽는다 — 송출 목록·보는 중·무전 음량·배너가 이것의 투영이다. */
private fun DispatchSession.pullTransmission(callId: Int) {
    videoLaunch("transmissionInfo") { refreshTransmission(callId) }
}

/** 같은 일을 **기다려서** 한다 — 뒤따르는 판정이 방금 보낸 명령의 결과를 봐야 할 때(`acceptVideo`). */
private suspend fun DispatchSession.refreshTransmission(callId: Int) {
    val engine = engineOrNull() ?: return
    val ti: TransmissionInfo = engine.transmissionInfo(callId) ?: return
    updateVideoCall(callId) { it.withTransmission(ti.state, ti.queuePosition, ti.transmitters, System.currentTimeMillis()) }
    afterTransmission(callId)
}

/** 송출 목록이 바뀐 뒤 — 무전 음량(D6)과 «새 영상» 배너를 다시 판정한다. */
private fun DispatchSession.afterTransmission(callId: Int) {
    val v = video
    val call = v.calls.value.firstOrNull { it.callId == callId } ?: return
    updateDuck(call)
    v.banners.value.firstOrNull { it.callId == callId }?.let { b ->
        if (!VideoRules.bannerValid(call, b.transmitterId)) v.banners.value = v.banners.value - b   // 보기 시작했거나 송출이 끝났다
    }
}

/**
 * 내 송출(TS 24.581 §6.2.4) — 허가(카메라·영상 호 마이크는 코어가 연다)·대기·거절·회수·끝. 보는 사람 = Media Reception
 * Notification(§6.2.4.4.8)이 올 때만 센다.
 */
private fun DispatchSession.applyVideoTransmission(e: TransmissionEvent) {
    val v = video
    val call = v.calls.value.firstOrNull { it.callId == e.callId } ?: return
    val gid = call.groupId
    val title = groupNameOf(gid)
    android.util.Log.i(TAG, "mcvideo tx #${e.callId} ${e.kind} state=${e.state} cause=${e.cause} ${e.causeText} queue=${e.queuePosition} rx=${e.receiverId}")
    val now = System.currentTimeMillis()
    var c = call.copy(tx = e.state, queuePosition = if (e.kind == TransmissionEventKind.QUEUE_POSITION) e.queuePosition else call.queuePosition)
    when (e.kind) {
        TransmissionEventKind.GRANTED -> {
            c = c.copy(txSinceMs = now, txReceivers = emptySet())
            addActivity(gid, title, "내 영상 보내기 시작", ActivityKind.VIDEO)
            applyCameraPreference(e.callId)
        }
        TransmissionEventKind.RECEIVER_JOINED ->
            if (e.receiverId.isNotEmpty()) c = c.copy(txReceivers = c.txReceivers + userPart(e.receiverId))
        TransmissionEventKind.REJECTED, TransmissionEventKind.REVOKED -> {
            notify(NoticeLevel.WARN, "$title — ${VideoText.transmission(e.kind, e.cause, v.channels.value[gid]?.maxTransmitters ?: 0)}",
                "#${e.cause} ${e.causeText}".trim())
            if (e.kind == TransmissionEventKind.REVOKED)
                addActivity(gid, title, listOf("내 영상 보내기 회수", e.causeText).filter { it.isNotEmpty() }.joinToString(" · "), ActivityKind.ERROR)
        }
        TransmissionEventKind.REQUEST_TIMEOUT -> notify(NoticeLevel.WARN, "$title — 영상 보내기 요청에 응답이 없습니다")
        else -> Unit
    }
    if (e.state == TransmissionState.NO_PERMISSION && call.txSinceMs != null) {
        addActivity(gid, title, "내 영상 보내기 끝 · ${VideoText.duration(now - call.txSinceMs)}", ActivityKind.VIDEO)
        c = c.copy(txSinceMs = null, txReceivers = emptySet())
    }
    updateVideoCall(e.callId) { c }
    pullTransmission(e.callId)
    syncVideoMic()
}

/**
 * 수신 제어(TS 24.581 §6.2.5) — 새 송출 알림·보기·끝. 새 송출은 «새 영상» 배너(채널마다 하나 — 가장 최근 송출)로 알린다: 송출자는
 * 아무도 보지 않으면 서버가 송출을 끝내는 시한(T11, 기본 10 s — §6.3.4.4.13 #8)을 기다리고 있다.
 */
private fun DispatchSession.applyVideoReception(e: ReceptionEvent) {
    val v = video
    val call = v.calls.value.firstOrNull { it.callId == e.callId } ?: return
    val gid = call.groupId
    val title = groupNameOf(gid)
    val t = e.transmitter
    val who = videoSenderName(t)
    val key = userPart(t.userId)
    val now = System.currentTimeMillis()
    android.util.Log.i(TAG, "mcvideo rx #${e.callId} ${e.kind} from=${t.userId} state=${t.state} auto=${t.automatic} cause=${e.cause} ${e.causeText}")
    // 이벤트가 실어 온 값으로 먼저 그리고(즉시 반응), 권위 있는 스냅샷은 뒤따라 채운다.
    val list = call.transmitters.filterNot { sameUser(it.userId, t.userId) } +
        (if (e.kind == ReceptionEventKind.ENDED || t.state == ReceptionState.ENDED) emptyList() else listOf(t))
    updateVideoCall(e.callId) { it.withTransmission(it.tx, it.queuePosition, list, now) }
    when (e.kind) {
        ReceptionEventKind.NOTIFIED ->
            // 거절·그만 보기 뒤의 Notified 복귀는 새 송출이 아니다. automatic(Reception Mode '0')은 서버가 곧바로 허가한다.
            if (t.state == ReceptionState.NOTIFIED && !t.automatic && v.announced.getOrPut(e.callId) { HashSet() }.add(key)) {
                addActivity(gid, title, "$who 영상 보내기 시작", ActivityKind.VIDEO)
                v.banners.value = listOf(VideoBanner(gid, e.callId, t.userId, title, who, now)) + v.banners.value.filterNot { it.groupId == gid }
            }
        ReceptionEventKind.GRANTED -> addActivity(gid, title, "$who 영상 보기", ActivityKind.VIDEO)
        ReceptionEventKind.REJECTED, ReceptionEventKind.REQUEST_TIMEOUT -> {
            notify(NoticeLevel.ERROR, "$title — ${VideoText.reception(e.kind, e.cause, who)}", "#${e.cause} ${e.causeText}".trim())
            addActivity(gid, title, listOf("$who 영상 보기 실패", e.causeText).filter { it.isNotEmpty() }.joinToString(" · "), ActivityKind.ERROR)
        }
        ReceptionEventKind.ENDED -> {
            v.announced[e.callId]?.remove(key)
            addActivity(gid, title, "$who 영상 보내기 끝", ActivityKind.VIDEO)
            if (t.state == ReceptionState.ENDED && v.watched.remove(e.callId to key))
                notify(NoticeLevel.INFO, "$title — ${VideoText.withIGa(who)} 영상 보내기를 멈췄습니다")
        }
        ReceptionEventKind.RELEASED -> addActivity(gid, title, "$who 영상 그만 보기", ActivityKind.VIDEO)
        ReceptionEventKind.END_REQUESTED -> {
            notify(NoticeLevel.INFO, "$title — $who 영상 보기가 끝났습니다", "#${e.cause} ${e.causeText}".trim())
            addActivity(gid, title, listOf("$who 영상 보기 끝(서버)", e.causeText).filter { it.isNotEmpty() }.joinToString(" · "), ActivityKind.VIDEO)
        }
        else -> Unit
    }
    if (e.kind == ReceptionEventKind.GRANTED) v.watched.add(e.callId to key)
    else if (e.kind == ReceptionEventKind.RELEASED || e.kind == ReceptionEventKind.END_REQUESTED) v.watched.remove(e.callId to key)
    afterTransmission(e.callId)
    pullTransmission(e.callId)
}

// ── 소리(D6 — 영상 호 송출 음성 우선) ────────────────────────────────────────

private fun DispatchSession.voiceOfGroup(groupId: String): SessionItem? =
    sessions.value.firstOrNull { it.kind == SessionKind.PTT_CHANNEL && it.info.groupId == groupId && it.isLive }

private fun DispatchSession.setVoiceLevel(callId: Int, level: Float, onFail: () -> Unit = {}) {
    videoLaunch("duck") {
        val engine = engineOrNull() ?: return@videoLaunch
        val r = videoCmd { engine.call(callId).setRxLevel(level) }
        if (!r.ok) { onFail(); android.util.Log.w(TAG, "duck #$callId $level: ${r.code} ${r.reason}") }
    }
}

/** 같은 그룹의 영상을 보는 동안 그 그룹 무전의 수신 음량을 줄이고, 보기가 끝나면 되돌린다. 다른 그룹 무전은 그대로(§10.5). */
private fun DispatchSession.updateDuck(call: VideoCall) {
    if (!VideoRules.ducks(call.receiving != null)) { unduck(call.groupId); return }
    val voice = voiceOfGroup(call.groupId) ?: return
    if (!video.ducked.add(voice.callId)) return
    setVoiceLevel(voice.callId, VideoRules.DUCK_LEVEL) { video.ducked.remove(voice.callId) }
}

private fun DispatchSession.unduck(groupId: String) {
    val voice = voiceOfGroup(groupId) ?: return
    if (video.ducked.remove(voice.callId)) setVoiceLevel(voice.callId, 1f)
}

/** 무전 호의 상태가 왔다 — 새로 생겼으면(영상을 보는 중에 무전 합류) 줄임을 다시 판정하고, 끝났으면 줄임 표시를 걷는다. */
private fun DispatchSession.noteVoiceCall(ci: CallInfo) {
    val v = video
    if (v.calls.value.isEmpty() && v.ducked.isEmpty()) return
    if (SessionKind.of(ci) != SessionKind.PTT_CHANNEL) return
    if (ci.state == CallState.DISCONNECTED || ci.state == CallState.NULL) { v.ducked.remove(ci.callId); return }
    val watching = v.calls.value.any { it.isLive && it.groupId == userPart(ci.groupId) && it.receiving != null }
    if (watching && v.ducked.add(ci.callId)) setVoiceLevel(ci.callId, VideoRules.DUCK_LEVEL) { v.ducked.remove(ci.callId) }
}

// ── 마이크 경합(D12 — TS 22.280 R-8.3-003·-004) ──────────────────────────────

/**
 * D12 영상 우선 — 영상을 보내는 동안 이 채널의 무전 발언 요청을 막는가(긴급·임박 채널은 늘 음성). 막으면 한 번 알린다.
 * `floorRequest` 가 요청 전에 묻는다.
 */
internal fun DispatchSession.blocksTalkForVideo(callId: Int): Boolean {
    val v = video
    val s = sessionOf(callId) ?: return false
    if (!VideoRules.blocksTalk(settingsSnapshot().videoMicPolicy, v.calls.value.any { it.isLive && it.txBusy },
            s.isEmergency, s.isImminentPeril)) return false
    val now = System.currentTimeMillis()
    if (now - v.blockNotedAtMs > BLOCK_NOTE_GAP_MS) {
        v.blockNotedAtMs = now
        notify(NoticeLevel.WARN, "영상을 보내는 중 — 무전 발언을 막았습니다",
            "설정 «영상 보내는 중 무전» = 영상 우선. 긴급·임박 채널은 그대로 말할 수 있습니다.")
    }
    return true
}

/**
 * D12 음성 우선(마이크 경합) — 내가 무전 발언(요청·대기·발언) 중인 동안 영상 송출 호의 음성만 멈추고(코어 `setMuted` = MCVideo 호에서는
 * 오디오 송신만 멈춘다, 영상은 계속 — ue_sdk.md §4.2), 끝나면 되돌린다.
 */
internal fun DispatchSession.syncVideoMic() {
    val v = video
    val talking = sessions.value.any { it.isLive && it.info.isMcptt && VideoRules.talking(it.floor?.state) }
    var yielded = false
    for (c in v.calls.value.filter { it.isLive }) {
        val mute = VideoRules.yieldsMic(talking, c.tx)
        yielded = yielded || mute
        if (mute == (c.callId in v.micMuted)) continue
        if (mute) v.micMuted.add(c.callId) else v.micMuted.remove(c.callId)      // 먼저 적는다 — 같은 값을 두 번 보내지 않게
        videoLaunch("video mic") {
            val engine = engineOrNull() ?: return@videoLaunch
            val r = videoCmd { engine.call(c.callId).setMuted(mute) }
            if (!r.ok) {
                if (mute) v.micMuted.remove(c.callId) else v.micMuted.add(c.callId)
                android.util.Log.w(TAG, "video mic #${c.callId} $mute: ${r.code} ${r.reason}")
            } else android.util.Log.i(TAG, "mcvideo #${c.callId} audio ${if (mute) "yielded to voice talk" else "resumed"} (D12)")
        }
    }
    v.micYielded.value = yielded
}

// ── 관제 동작 ────────────────────────────────────────────────────────────────

/**
 * [보기]·[바꿔 보기](§10.4) — Receive Media Request(TS 24.581 §6.2.5.3.3). **한 번에 하나** — 보던 송출(다른 채널 포함)이 있으면
 * 먼저 그만 본다(1차 수신 상한 1).
 */
fun DispatchSession.acceptVideo(groupId: String, transmitterId: String) = videoLaunch("acceptVideo") {
    // [보기] 는 **한 줄로** 처리한다 — 연달아 누르면(다른 줄·배너) 뒤의 것이 앞 요청의 결과를 본 뒤에 판정한다. 누를 때의
    //   스냅샷으로 판정하면 앞 요청이 아직 «요청 중» 이라 거두지 않고 두 번째 요청이 나가 «한 번에 하나» 가 깨진다.
    video.viewLock.withLock {
        val engine = engineOrNull() ?: return@withLock
        val call = videoOfGroup(groupId) ?: return@withLock
        val h = engine.call(call.callId)
        call.viewing?.let { cur ->
            if (sameUser(cur.userId, transmitterId)) return@withLock
            val e = videoCmd { h.endReception(cur.userId) }
            if (!e.ok) android.util.Log.w(TAG, "mcvideo end reception ${cur.userId}: ${e.code} ${e.reason}")
        }
        video.calls.value.filter { it.callId != call.callId && it.isLive }.forEach { other ->
            other.viewing?.let { r -> videoCmd { engine.call(other.callId).endReception(r.userId) } }
        }
        val r = videoCmd { h.acceptReception(transmitterId) }
        refreshTransmission(call.callId)     // 잠금을 풀기 전에 결과를 반영한다 — 다음 [보기] 가 이 요청을 «보는 중» 으로 본다
        report(TextArea.VIDEO, r)
    }
}

/**
 * 화면을 벗어났다(홈·다른 앱·화면 꺼짐) — **보내던 영상을 끝낸다.** 카메라는 화면이 보이는 동안의 기능이다(서비스에 카메라
 * 타입이 없어 OS 가 배경의 카메라를 막는다). 송출 허가를 쥔 채 두면 수신자는 멈춘 영상을 보고 동시 송출 상한 한 자리를
 * 차지한다. 요청·대기 중인 것도 거둔다.
 */
internal fun DispatchSession.releaseVideoOnBackground() = videoLaunch("background") {
    val engine = engineOrNull() ?: return@videoLaunch
    val mine = video.calls.value.filter { it.isLive && it.tx != TransmissionState.NO_PERMISSION && it.tx != TransmissionState.PENDING_END }
    if (mine.isEmpty()) return@videoLaunch
    mine.forEach { c ->
        val r = videoCmd { engine.call(c.callId).releaseTransmission() }
        pullTransmission(c.callId)
        if (!r.ok) android.util.Log.w(TAG, "mcvideo release on background #${c.callId}: ${r.code} ${r.reason}")
    }
    notify(NoticeLevel.INFO, "화면을 벗어나 영상 보내기를 끝냈습니다", "카메라는 화면이 보이는 동안만 보냅니다")
}

/** [그만 보기] — Media Reception End Request(§6.2.5.5). 영상 호는 남는다 — 다른 송출을 다시 고를 수 있다. */
fun DispatchSession.endVideo(groupId: String) = videoLaunch("endVideo") {
    val engine = engineOrNull() ?: return@videoLaunch
    val call = videoOfGroup(groupId) ?: return@videoLaunch
    val t = call.receiving ?: return@videoLaunch
    val r = videoCmd { engine.call(call.callId).endReception(t.userId) }
    pullTransmission(call.callId)
    report(TextArea.VIDEO, r)
}

/**
 * [영상 보내기] 한 버튼(D11 — MCVideo 송출 요청·해제, 발언 바 PTT 와 따로). 상태에 따라 같은 자리가 끄는 버튼이 된다:
 *   · 여는 중(내가 연 편성 호가 성립 전) → 개시를 거둔다(hangup → CANCEL, 토스트 없음)
 *   · 영상 호 없음 → 편성 채널이면 영상 호를 연다([openVideoTx])
 *   · 송출 안 함 → Transmission Request(TS 24.581 §6.2.4.3.2) — 허가가 오면 코어가 카메라·영상 호 마이크를 연다(§6.2.4.4.6)
 *   · 요청·대기·송출 중 → Transmission End Request(§6.2.4.5.3·§6.2.4.4.7·§6.2.4.9.4) — 끝나면 코어가 카메라를 닫는다
 *
 * @param displayRotation 화면이 자연 방향에서 돈 각도(0·90·180·270) — 카메라 프레임을 화면 방향으로 세운다(가로로 쓰는 태블릿).
 */
fun DispatchSession.toggleVideoSend(groupId: String, displayRotation: Int = 0) = videoLaunch("toggleVideoSend") {
    val engine = engineOrNull() ?: return@videoLaunch
    val call = videoOfGroup(groupId)
    when {
        call?.isOpening == true -> {
            video.quietEnd.add(call.callId)
            report(TextArea.VIDEO, videoCmd { engine.call(call.callId).hangup() })
        }
        call == null -> if (canOpenVideo(groupId)) openVideoTx(groupId, displayRotation)
        !call.isActive -> Unit                                           // 합류 중 — 성립 뒤에 누른다
        call.tx == TransmissionState.NO_PERMISSION -> {
            if (video.cameras.value.isEmpty()) { report(TextArea.VIDEO, CimsResult.fail<Unit>(-1, "카메라 없음")); return@videoLaunch }
            applyVideoCaptureRotation(displayRotation)
            val r = videoCmd { engine.call(call.callId).requestTransmission() }
            pullTransmission(call.callId)
            report(TextArea.VIDEO, r)
        }
        call.tx != TransmissionState.PENDING_END -> {
            val r = videoCmd { engine.call(call.callId).releaseTransmission() }
            pullTransmission(call.callId)
            report(TextArea.VIDEO, r)
        }
    }
}

/**
 * 편성(prearranged) 영상 채널에 영상 호가 없다 — 영상 호를 연다(TS 24.281 §9.2.1.2.1.1). 송출 요청은 개시 INVITE 에 싣는다
 * (16) · §6.4 — TS 24.581 §14.2.4 `mc_implicit_request`). 제어 기능이 MCVideo 로 affiliate 한 멤버를 초대하고 첫 멤버가 붙으면 200 OK,
 * 10 s 안에 아무도 붙지 않으면 480(§9.2.1.4.2). 그사이 다른 멤버가 먼저 열었으면 서버가 합류로 받고 암묵 요청은 받지 않는다(§14.3.5) —
 * 코어가 명시 Transmission Request 로 잇는다. 코어는 호 성립 전에는 송출 이벤트를 내지 않으므로 «여는 중…» 은 앱이 그린다.
 * 실패는 다시 열지 않는다 — 사용자가 다시 누른다(chat 합류의 자동 재시도와 섞지 않는다).
 */
private suspend fun DispatchSession.openVideoTx(groupId: String, displayRotation: Int) {
    val v = video
    val ptt = pttAccount ?: return
    val n6 = n6()
    if (!VideoRules.withinN6(v.calls.value.count { it.isLive } + v.joining.size, n6)) {
        report(TextArea.VIDEO, CimsResult.fail<Unit>(486, "동시 영상 호 한도(N6 = $n6)"))
        return
    }
    val gen = loginGeneration.value
    v.joining.add(groupId)
    setChannel(groupId) { it.copy(note = VideoRules.OPENING_NOTE) }
    applyVideoCaptureRotation(displayRotation)
    val r = videoCmd {
        ptt.joinVideoGroupCall(groupId, VideoGroupCallOptions(prearranged = true, implicitTransmissionRequest = true, queueing = true))
    }
    val call = r.value
    if (gen != loginGeneration.value) { call?.let { c -> videoCmd { c.hangup() } }; return }
    if (!r.ok || call == null) {
        android.util.Log.w(TAG, "mcvideo open $groupId: ${r.code} ${r.reason}")
        v.joining.remove(groupId)
        setChannel(groupId) { it.copy(note = VideoRules.PREARRANGED_NOTE) }
        report(TextArea.VIDEO, CimsResult.fail<Unit>(r.code, r.reason))
        return
    }
    android.util.Log.i(TAG, "mcvideo open prearranged $groupId → call ${call.id} (implicit transmission request)")
    // 그사이 채널에서 빠졌다 — 남기지 않는다.
    if (groupId !in v.channels.value) { hangupVideoQuietly(call.id); v.joining.remove(groupId); return }
    // 그사이 이미 끝났다(곧바로 거절) — 사유는 호 종료가 알렸다. 호 번호는 다시 쓰이므로 «내가 연 호» 표시를 남기지 않는다.
    val info = engineOrNull()?.callInfo(call.id) ?: call.info
    if (info == null || info.state == CallState.DISCONNECTED || info.state == CallState.NULL) {
        v.joining.remove(groupId)
        if (videoOfGroup(groupId) == null) setChannel(groupId) { it.copy(note = VideoRules.PREARRANGED_NOTE) }
        return
    }
    // 그사이 멤버의 초대가 먼저 붙었다 — 한 그룹에 영상 호는 하나라 내 개시 호는 나간다(`onVideoCallAdded`). 보내려고 누른
    //   것이므로 남은 호에 명시 요청으로 잇는다(TS 24.581 §6.2.4.3.2 — 성립해 있을 때. 아니면 성립 뒤 다시 누른다).
    val other = v.calls.value.firstOrNull { it.groupId == groupId && it.isLive && it.callId != call.id }
    if (other != null) {
        v.joining.remove(groupId)
        if (call.id !in v.leaving) hangupVideoQuietly(call.id)
        val engine = engineOrNull()
        if (engine != null && other.isActive && other.tx == TransmissionState.NO_PERMISSION && v.cameras.value.isNotEmpty()) {
            val q = videoCmd { engine.call(other.callId).requestTransmission() }
            pullTransmission(other.callId)
            report(TextArea.VIDEO, q)
        } else if (!other.isActive) {
            // 누른 것이 조용히 사라지지 않게 — 합류가 끝나면 다시 누른다
            notify(NoticeLevel.INFO, "영상 호에 합류하는 중입니다", "다른 멤버가 먼저 열었습니다 — 연결되면 [영상 보내기] 를 다시 누르세요")
        }
        return
    }
    v.opening.add(call.id)
    if (v.calls.value.any { it.callId == call.id }) updateVideoCall(call.id) { it.copy(opening = true) }
    else takeVideoCall(info)
    if (videoOfGroup(groupId)?.isOpening == true) setChannel(groupId) { it.copy(note = VideoRules.OPENING_NOTE) }
}

/** [영상 소리](§10.5 — TS 22.280 R-8.3-002 동시 오디오 원천의 상대 음량) — 영상 호 수신 음량 0~2(코어가 호에 기억한다). */
fun DispatchSession.setVideoVolume(groupId: String, level: Float) = videoLaunch("videoVolume") {
    val engine = engineOrNull() ?: return@videoLaunch
    val call = videoOfGroup(groupId) ?: return@videoLaunch
    val v = level.coerceIn(0f, 2f)
    val r = videoCmd { engine.call(call.callId).setRxLevel(v) }
    // 호 스냅샷은 상태·미디어 이벤트 때만 다시 온다 — 패널을 다시 열어도 막대가 맞게 가진 사본에 적어 둔다.
    if (r.ok) updateVideoCall(call.callId) { it.copy(info = it.info.copy(rxLevel = v)) }
}

/** [↻]/[↺] — 보내는 사람의 영상을 90° 씩 돌린다(보내는 쪽 카메라 방향이 다를 때). 송출자마다 기억한다. */
fun DispatchSession.rotateVideo(transmitterId: String, delta: Int) {
    val key = userPart(transmitterId)
    val cur = video.orients.value[key] ?: VideoOrient()
    video.orients.value = video.orients.value + (key to cur.copy(rotation = VideoRules.rotate(cur.rotation, delta)))
}

/** 칸 모양 — 세로 송출(단말 480×640) ↔ 가로 송출(PC 관제 앱 640×480). 엔진이 프레임 크기를 알려 주지 않아 사람이 고른다. */
fun DispatchSession.toggleVideoWide(transmitterId: String) {
    val key = userPart(transmitterId)
    val cur = video.orients.value[key] ?: VideoOrient()
    video.orients.value = video.orients.value + (key to cur.copy(wide = !cur.wide))
}

/** «새 영상» 배너 [보기] — 그 송출을 본다(채널 상세는 화면이 연다). 보던 것이 있으면 바꿔 본다. */
fun DispatchSession.acceptVideoBanner(b: VideoBanner) {
    acceptVideo(b.groupId, b.transmitterId)
    dismissVideoBanner(b)
}

/** «새 영상» 배너 [닫기] — 배너만 내린다. 채널 «영상 n» 목록에서 다시 [보기] 할 수 있다(TS 22.281 R-5.2.6.2.2-009 NOTE 3). */
fun DispatchSession.dismissVideoBanner(b: VideoBanner) { video.banners.value = video.banners.value - b }

// ── 영상 장치(Android 접점 — 수신 창 하나·셀프뷰·카메라) ─────────────────────

/** 수신 영상을 그릴 Surface(null = 해제) — 엔진의 수신 창은 하나다(보는 송출도 한 번에 하나, D8). */
fun DispatchSession.setVideoSurface(surface: Surface?) = videoLaunch("videoSurface") {
    val engine = engineOrNull() ?: return@videoLaunch
    val r = videoCmd { engine.setVideoSurface(surface) }
    if (!r.ok) android.util.Log.w(TAG, "video surface: ${r.code} ${r.reason}")
}

/** 셀프뷰(내 카메라) Surface(null = 해제) — 코어가 송출 중일 때만 프레임이 온다(셀프뷰만으로 카메라를 열지 않는다). */
fun DispatchSession.setVideoPreviewSurface(surface: Surface?) { engineOrNull()?.setPreviewSurface(surface) }

/** 카메라 프레임을 화면 방향으로 세운다 — 화면이 돌 때·송출을 걸 때. */
private suspend fun DispatchSession.applyVideoCaptureRotation(displayRotation: Int) {
    val engine = engineOrNull() ?: return
    val r = videoCmd { engine.setCaptureRotation(appContext(), displayRotation) }
    if (!r.ok) android.util.Log.w(TAG, "capture rotation $displayRotation: ${r.code} ${r.reason}")
}

/** 화면이 돌았다 — 보내는 중이면 카메라 방향을 다시 맞춘다. */
fun DispatchSession.onVideoDisplayRotation(displayRotation: Int) {
    if (video.calls.value.none { it.isLive && it.txBusy }) return
    videoLaunch("captureRotation") { applyVideoCaptureRotation(displayRotation) }
}

/** 송출이 허가됐다 — 설정의 카메라(앞/뒤)로 맞춘다. 엔진은 앞에서 시작하고 전환은 호의 영상이 열린 뒤에만 된다. */
private fun DispatchSession.applyCameraPreference(callId: Int) {
    val v = video
    val wantBack = settingsSnapshot().videoCamera == VideoCamera.BACK
    if (wantBack == v.cameraBack || v.cameras.value.size < 2) return
    videoLaunch("camera") {
        val engine = engineOrNull() ?: return@videoLaunch
        val r = videoCmd { engine.call(callId).switchCamera() }
        if (r.ok) v.cameraBack = wantBack else android.util.Log.w(TAG, "camera → ${if (wantBack) "back" else "front"}: ${r.code} ${r.reason}")
    }
}

/** 설정 [영상] · 내 송출 줄의 [카메라 전환] — 앞/뒤. 보내는 중이면 곧바로 바꾸고, 아니면 다음 송출부터. */
fun DispatchSession.setVideoCamera(camera: String) {
    updateSettings { it.copy(videoCamera = if (camera == VideoCamera.BACK) VideoCamera.BACK else VideoCamera.FRONT) }
    video.calls.value.firstOrNull { it.isLive && it.sending }?.let { applyCameraPreference(it.callId) }
}

/** [카메라 전환] — 지금 설정의 반대로. */
fun DispatchSession.switchVideoCamera() =
    setVideoCamera(if (settingsSnapshot().videoCamera == VideoCamera.BACK) VideoCamera.FRONT else VideoCamera.BACK)
