// 세션·그룹 모델 — 코어 스냅샷의 투영 (docs/design/features/android_dispatch_tablet.md §6.7)
//
// **앱은 별도 상태 기계를 갖지 않는다.** 여기 있는 것은 코어가 준 `CallInfo`·`FloorInfo`·로스터를
// 화면이 읽기 좋은 모양으로 접은 것뿐이고, 권위는 언제나 코어 스냅샷이다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.FloorEvent
import com.cims.ue.sdk.FloorInfo
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.RosterEntry

/** 화면 배치 기준의 세션 종류 — `isMcptt`/`listenOnly`/`privateCall`/`adhoc-`/`joinedDialog` 로 판정. */
enum class SessionKind {
    /** ③ 내 통화 — VoLTE/VoIP 1:1(외부망 포함). */
    PHONE_CALL,
    /** 감청 시트 — INVITE-Join 청취 leg. */
    PHONE_MONITOR,
    /** ① 멤버 채널 카드 — 그룹콜. */
    PTT_CHANNEL,
    /** ① 개별 통화 카드(TS 24.379 private call). */
    PTT_PRIVATE,
    /** ① 애드혹 그룹 통화 카드(ad hoc group call). */
    PTT_ADHOC,
    /** 청취 시트 — 그룹콜 recvonly. */
    PTT_LISTEN;

    /** 시트로 여는 종류인가(카드가 아니라). */
    val isSheet: Boolean get() = this == PHONE_MONITOR || this == PTT_LISTEN
    /** ① 내 채널 카드로 서는 종류인가. */
    val isPttCard: Boolean get() = this == PTT_CHANNEL || this == PTT_PRIVATE || this == PTT_ADHOC

    companion object {
        const val ADHOC_PREFIX = "adhoc-"

        fun of(c: CallInfo): SessionKind = when {
            c.isMcptt && c.listenOnly -> PTT_LISTEN
            c.isMcptt && c.mcptt.privateCall -> PTT_PRIVATE
            c.isMcptt && c.groupId.startsWith(ADHOC_PREFIX) -> PTT_ADHOC
            c.isMcptt -> PTT_CHANNEL
            c.listenOnly && c.joinedDialog.isNotEmpty() -> PHONE_MONITOR
            else -> PHONE_CALL
        }
    }
}

/** Floor Indicator B-bit "Broadcast group call"(TS 24.380 §8.2.3.15) — 코어 `floor::indicator::BROADCAST_GROUP` 과 같은 값. */
const val FLOOR_IND_BROADCAST = 0x4000
/**
 * 채널의 긴급 상태 — MCPTT 그룹 호의 조건(condition, mcptt-info `emergency-ind`/`imminentperil-ind`).
 *
 * 서열은 긴급 › 임박이다(mcptt_emergency_modes.md §1) — 둘이 같이 서면 긴급으로 본다. 조건은 그룹 종류가 아니라
 * **세션의 속성**이라 그룹콜·애드혹·청취 세션 모두에 선다. 개별 통화는 여기 들지 않는다 — 착신 배너·카드가 받는다
 * (데스크톱 `UpdateEmergencyBanner` 와 같은 경계).
 */
enum class AlertKind(
    /** ⑤ 이벤트·태그의 한 낱말. */
    val label: String,
    /** 전역 배너의 머리. */
    val bannerTitle: String,
) {
    EMERGENCY("긴급", "긴급"),
    IMMINENT_PERIL("임박", "임박 위험"),
}

/** 긴급 상태가 바뀐 모양 — ⑤ 이벤트에 «개시»·«해제» 를 남기는 근거. */
sealed interface AlertChange {
    data object None : AlertChange
    data class Started(val kind: AlertKind) : AlertChange
    data class Cleared(val kind: AlertKind) : AlertChange
}

/**
 * 긴급 상태 전이 — 이전·다음 세션에서 «언제부터» 와 남길 변화를 정한다(데스크톱 `UpdateEmergencyBanner`).
 *
 * 종류가 바뀌면(임박 → 긴급 격상) 새로 개시한 것으로 본다 — 경과도 거기서 다시 센다. 세션이 끝나 사라지는 것은
 * 해제가 아니다(⑤ 는 «세션 종료» 로 이미 남는다).
 *
 * @return 다음 세션의 `alertSinceMs` 와 변화
 */
internal fun alertTransition(prev: SessionItem?, next: SessionItem, nowMs: Long): Pair<Long?, AlertChange> {
    val was = prev?.alertKind
    val now = next.alertKind
    return when {
        now == null -> null to (if (was != null) AlertChange.Cleared(was) else AlertChange.None)
        now == was -> (prev?.alertSinceMs ?: nowMs) to AlertChange.None
        else -> nowMs to AlertChange.Started(now)
    }
}

/**
 * 전역 배너 스택 — 긴급·임박 세션을 **채널마다 하나**, 최신 위로.
 *
 * 한 채널에 leg 가 둘이면 **처음 본 것**이 남는다 — 채널의 긴급은 그때부터다(데스크톱 `BannerOfGroup` 이 그룹 배너를
 * 처음 세운 세션으로 두는 것과 같다).
 */
internal fun alertStack(sessions: List<SessionItem>): List<SessionItem> =
    sessions.filter { it.isLive && it.alertKind != null }
        .groupBy { it.channelId }
        .map { (_, legs) -> legs.minBy { it.alertSinceMs ?: it.startedAtMs } }
        .sortedByDescending { it.alertSinceMs ?: it.startedAtMs }

/** 세션을 만든 관제 동작 — 종료 코드의 문구 사전(dispatch_desktop_ui.md §9) 선택에 쓴다. */
enum class Operation { INCOMING, DIAL, PICKUP, JOIN, TRANSFER, PTT_JOIN, PTT_LISTEN, PTT_PRIVATE, PTT_ADHOC, EMERGENCY }

/**
 * 살아 있는 호 하나. [info]·[floor] 는 코어 스냅샷 복사본이다.
 *
 * 불변 객체로 두고 갱신은 `copy` 로 한다 — Compose 가 변화를 알아채고, 이전 값과 비교할 수 있다.
 */
data class SessionItem(
    val callId: Int,
    val account: AccountKind,
    val operation: Operation,
    val info: CallInfo,
    val floor: FloorInfo? = null,
    val lastFloor: FloorEvent? = null,
    val startedAtMs: Long = System.currentTimeMillis(),
    val connectedAtMs: Long? = null,
    /** 현재 발언자(로스터·주소록으로 해석한 표시명). */
    val speaker: String = "",
    val speakerSinceMs: Long? = null,
    /** Denied/Revoked 사유 한 줄(카드 하단에 잠시). */
    val floorNote: String = "",
    /** 표시 이름(그룹명·상대 이름). */
    val title: String = "",
    /** 애드혹 멤버(응답 상태는 로스터가 준다). */
    val adhocMembers: List<String> = emptyList(),
    /** 지금의 긴급 상태([alertKind])가 선 때 — 배너의 경과. 긴급이 아니면 null(`alertTransition`). */
    val alertSinceMs: Long? = null,
    /** 상담 전달의 **상담 호**면 원 통화의 callId(데스크톱 `ConsultFor`) — 카드 «상담» 배지·[전달 완결]·[취소]. */
    val consultFor: Int? = null,
    /** 전달을 걸었다(«전달 중 → 이순경») — 서버가 받아들이면 이 leg 은 BYE 로 끝난다. */
    val transferNote: String = "",
) {
    val kind: SessionKind get() = SessionKind.of(info)
    val isLive: Boolean get() = info.state != CallState.DISCONNECTED && info.state != CallState.NULL
    val isActive: Boolean get() = info.state == CallState.ACTIVE
    val isEmergency: Boolean get() = info.mcptt.emergency
    val isImminentPeril: Boolean get() = info.mcptt.imminentPeril
    /** 전이중 개별 통화 — floor 가 없어 마이크가 늘 열려 있다(발언 대상이 될 수 없다). */
    val isFullDuplex: Boolean get() = info.mcptt.noFloorCtrl

    /** 채널의 긴급 상태 — MCPTT 세션 중 개별 통화가 아닌 것만(개별 통화·전화는 null). 긴급이 임박보다 앞선다. */
    val alertKind: AlertKind? get() = when {
        !info.isMcptt || kind == SessionKind.PTT_PRIVATE -> null
        info.mcptt.emergency -> AlertKind.EMERGENCY
        info.mcptt.imminentPeril -> AlertKind.IMMINENT_PERIL
        else -> null
    }

    /** 이 세션이 서는 채널 id — ① 카드·② 행·[채널] 화면이 쓰는 것과 같다(개별 통화·애드혹은 그룹 id 가 없을 수 있다). */
    val channelId: String get() = info.groupId.ifEmpty { "call-$callId" }

    val isSpeaking: Boolean get() = floor?.state == FloorState.SPEAKING
    val isRequesting: Boolean get() = floor?.state == FloorState.REQUESTING
    val isQueued: Boolean get() = floor?.state == FloorState.QUEUED
    /** 청취 중 발언 버튼 비활성의 근거 — Floor Taken 의 `Permission to Request the Floor`(TS 24.380). */
    val canRequestFloor: Boolean get() = floor?.canRequest != false

    /**
     * 일제 통화(TS 24.379 §4.12 — 그룹 종류가 아니라 **호 속성**)인가 — 서버가 알린 값으로 본다: 착신 mcptt-info
     * `broadcast-ind` 또는 floor 메시지의 B-bit(TS 24.380 §8.2.3.15 — 진행 중 일제 통화에 늦게 합류한 leg 은 이것으로만 안다).
     */
    val isBroadcast: Boolean get() =
        (info.dir == com.cims.ue.sdk.CallDir.INCOMING && info.mcptt.broadcast) ||
            ((floor?.indicator ?: 0) and FLOOR_IND_BROADCAST) != 0 ||
            ((lastFloor?.indicator ?: 0) and FLOOR_IND_BROADCAST) != 0
    /** 내가 연 일제 통화 — 개시 INVITE 에 broadcast-ind 를 실었고 서버가 일제로 열었다(개시자만 발언 요청 가능). */
    val isBroadcastInitiator: Boolean get() =
        info.dir == com.cims.ue.sdk.CallDir.OUTGOING && info.mcptt.broadcast && isBroadcast && canRequestFloor

    val elapsedMs: Long get() = System.currentTimeMillis() - (connectedAtMs ?: startedAtMs)
    val speakerElapsedMs: Long get() = speakerSinceMs?.let { System.currentTimeMillis() - it } ?: 0L

    /** 남은 발언 게이지 0~1 — Granted Duration 기준. 승인 상태가 아니면 0. */
    fun talkGauge(grantedSec: Int): Float {
        if (!isSpeaking || grantedSec <= 0) return 0f
        val left = grantedSec * 1000L - speakerElapsedMs
        return (left.toFloat() / (grantedSec * 1000f)).coerceIn(0f, 1f)
    }
}

/**
 * PTT 그룹 하나 — 멤버 그룹(①) 또는 청취 범위 그룹(②).
 *
 * 진행 여부는 **로스터**로 안다(RFC 4575 conference NOTIFY) — 참여하지 않은 그룹도 세션이 도는지 보인다.
 */
data class GroupInfo(
    val id: String,
    val uri: String,
    val name: String,
    val memberCount: Int = 0,
    /** 멤버 그룹(true) / 청취 범위 그룹(false — `pttTargets`). */
    val isMember: Boolean = true,
    /** 내가 소유(authorized user)한 그룹 — 편집·삭제 가능(GMS 목록 `is_owner`). */
    val isOwner: Boolean = false,
    val etag: String = "",
    /**
     * 그룹 종류 — `prearranged`(편성)/`chat`(TS 24.481 `<on-network-invite-members>`). GMS 목록에 없어 모르면 빈 값이고,
     * 일제 통화 개시 때 그룹 문서로 확인해 채운다(채팅 그룹은 서버가 broadcast-ind 를 무시한다).
     */
    val sessionType: String = "",
    val affiliated: Boolean = false,
    val roster: List<RosterEntry> = emptyList(),
    /** 로스터에 접속 참가자가 생긴 시각 — ② 진행 중 카드의 경과. */
    val sessionSinceMs: Long? = null,
) {
    val connectedCount: Int get() = roster.count { it.status == "connected" }
    /** 진행 중 세션이 있는가(참여하지 않아도 로스터로 안다). */
    val hasSession: Boolean get() = connectedCount > 0

    /** 로스터를 갈아끼운다 — 세션 시작 시각을 함께 관리한다. */
    fun withRoster(next: List<RosterEntry>): GroupInfo {
        val connected = next.count { it.status == "connected" } > 0
        return copy(
            roster = next,
            sessionSinceMs = when {
                !connected -> null
                sessionSinceMs != null -> sessionSinceMs
                else -> System.currentTimeMillis()
            })
    }
}

/** ⑤ PTT 이벤트 한 줄 — 링 버퍼에 쌓인다(진행 중 행은 없다). */
data class ActivityRow(
    val atMs: Long,
    val groupId: String,
    val groupName: String,
    val text: String,
    val kind: ActivityKind,
    val emergency: Boolean = false,
)

enum class ActivityKind { TALK, JOIN, LEAVE, EMERGENCY, SDS, ERROR }

/**
 * 메시지 종류 — 같은 표에 담되 **섞이지 않게** 가른다.
 *
 * 둘은 망도 규격도 다르다: [SDS] 는 PTT 채널의 MCData SDS(TS 24.282), [SMS] 는 전화 축의
 * SIP MESSAGE text/plain 1:1(volte_supplementary_services.md §4.3). 스레드 키가 번호로 겹칠 수 있어
 * (PTT 1:1 SDS 도 번호가 키다) 종류를 안 가르면 한 대화에 두 망의 글이 섞인다.
 */
enum class MessageKind { SDS, SMS }

/** 메시지 한 통 — PTT 채널의 SDS 또는 전화 축의 SMS/LMS. */
data class Message(
    val id: String,
    val groupId: String,
    val fromUri: String,
    val fromName: String,
    val text: String,
    val atMs: Long,
    val outgoing: Boolean,
    val msgId: String = "",
    /** 발신 상관 키 — 최종 응답이 `requestResult` 에 이 token 으로 온다. */
    val token: Long = 0,
    val state: SendState = SendState.NONE,
    val read: Boolean = true,
    val kind: MessageKind = MessageKind.SDS,
)

enum class SendState { NONE, PENDING, SENT, DELIVERED, READ, FAILED }

/**
 * 재전송의 규칙 — 순수 논리라 JVM 에서 시험한다(android_dispatch_tablet.md §6.2e).
 *
 * 재전송은 **새 말풍선을 세우지 않는다**(같은 말이 두 번 보이면 두 번 보낸 줄 안다 — 데스크톱 `ResendCore`). 스레드
 * 지도에서 행 id 하나만 바꾼다. 다시 보낼 수 있는지는 화면이 넘긴 사본이 아니라 **지금의 말풍선**으로 판정한다 — 사본은
 * 한 번 누른 뒤에도 실패로 남아 있어, 발신 명령이 도는 동안 한 번 더 누르면 두 번 나간다.
 */
internal object Resend {
    /** 지금의 말풍선이 다시 보낼 수 있는 실패 발신인가. */
    fun allowed(threads: Map<String, List<Message>>, m: Message): Boolean =
        threads[m.groupId]?.firstOrNull { it.id == m.id }?.let { it.outgoing && it.state == SendState.FAILED } ?: false

    /** 행 id 하나만 바꾼다 — 다른 말풍선·다른 스레드는 그대로. */
    fun patch(threads: Map<String, List<Message>>, id: String, f: (Message) -> Message): Map<String, List<Message>> =
        threads.mapValues { (_, list) -> if (list.none { it.id == id }) list else list.map { if (it.id == id) f(it) else it } }
}
