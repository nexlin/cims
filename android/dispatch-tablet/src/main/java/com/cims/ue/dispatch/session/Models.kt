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
    /** ① 사설콜 카드. */
    PTT_PRIVATE,
    /** ① 애드혹 카드. */
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
) {
    val kind: SessionKind get() = SessionKind.of(info)
    val isLive: Boolean get() = info.state != CallState.DISCONNECTED && info.state != CallState.NULL
    val isActive: Boolean get() = info.state == CallState.ACTIVE
    val isEmergency: Boolean get() = info.mcptt.emergency
    val isImminentPeril: Boolean get() = info.mcptt.imminentPeril
    /** 전이중 사설콜 — floor 가 없어 마이크가 늘 열려 있다(발언 대상이 될 수 없다). */
    val isFullDuplex: Boolean get() = info.mcptt.noFloorCtrl

    val isSpeaking: Boolean get() = floor?.state == FloorState.SPEAKING
    val isRequesting: Boolean get() = floor?.state == FloorState.REQUESTING
    val isQueued: Boolean get() = floor?.state == FloorState.QUEUED
    /** 청취 중 발언 버튼 비활성의 근거 — Floor Taken 의 `Permission to Request the Floor`(TS 24.380). */
    val canRequestFloor: Boolean get() = floor?.canRequest != false

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

/** ④ PTT 메시지 한 통(MCData SDS). */
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
)

enum class SendState { NONE, PENDING, SENT, DELIVERED, READ, FAILED }
