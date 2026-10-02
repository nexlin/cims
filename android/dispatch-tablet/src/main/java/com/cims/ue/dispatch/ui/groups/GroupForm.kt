// PTT 그룹 편집 폼의 값과 규칙 — GMS 그룹 문서(TS 24.481 §7.2.2·§7.2.4.2) ↔ 폼
// (docs/design/features/android_dispatch_tablet.md §6.12, dispatch_desktop_ui.md §4.7·§10.6)
//
// 화면([PttGroupsScreen]·[NewGroupPanel])은 [EditForm] 만 그리고, 문서로 되돌리는 규칙은 여기 **순수 함수**가 갖는다(시험 대상).
// 데스크톱 `GroupEditViewModel` 과 같은 규칙 셋이다:
//   · **미기재 보존** — `GroupDoc` 의 그룹 호·한도 칸은 `null` = 미기재다. 문서에 없던 칸이 기본값 그대로면 저장 때도 미기재로
//     둔다. 폼을 연 것만으로 서버 값을 명시값으로 굳히지 않는다.
//   · **멤버 우선순위 보존** — XCAP PUT 은 `<list>` 가 있으면 멤버 전체를 교체한다. 역할을 바꾸지 않은 멤버는 읽은
//     `user-priority` 를 그대로 되돌려, 콘솔이 준 멤버별 값을 지우지 않는다.
//   · **MCVideo 는 켜기만** — 서버가 MCVideo `<service>` 없는 PUT 을 «그대로 둠» 으로 읽는다(mcvideo.md §5.1). 켜진 그룹은
//     스위치가 잠기고, 폼에 없는 속성(코덱·해상도·실시간 모드)은 읽은 값을 되돌린다.
package com.cims.ue.dispatch.ui.groups

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.session.telUri
import com.cims.ue.dispatch.session.userPart
import com.cims.ue.sdk.GroupDoc
import com.cims.ue.sdk.GroupMember
import com.cims.ue.sdk.McVideoGroupAttrs

/**
 * 폼 칸의 범위·기본값 — 콘솔 «서비스 › PTT 그룹» 편집과 같은 값이다(dispatch_desktop_ui.md §4.7 표). MCVideo 몫은 서버
 * (csc `services/mcvideo.py` 의 기본값·검증)와 같다(§10.6 표).
 */
object GroupDefaults {
    /** on-network-hang-timer(T4, 초) — 0 = 미사용. */
    const val HANG_TIMER = 30
    const val HANG_TIMER_MAX = 3600
    /** on-network-maximum-duration(초) — 0 = 무제한. */
    const val MAX_DURATION = 3600
    const val MAX_DURATION_MAX = 86400
    /** mcdata-on-network-max-data-size-for-SDS(octet) — 0 = 무제한. */
    const val MAX_SDS_SIZE = 10000
    /** mcdata-on-network-max-data-size-auto-recv(octet) — 0 = 무제한. */
    const val MAX_AUTO_RECV = 1048576
    /** on-network-minimum-number-to-start — 0 = 기다리지 않음(TS 24.379 §6.3.3.3). */
    const val MIN_NUMBER_TO_START_MAX = 65535
    /** on-network-timeout-for-acknowledgement-of-required-members(TNG1, 초). */
    const val ACK_TIMEOUT = 5
    const val ACK_TIMEOUT_MIN = 1
    const val ACK_TIMEOUT_MAX = 300
    /** TNG1 만료 동작 — 통화 포기(기본) / 없이 진행. */
    const val ACK_ABANDON = "abandon"
    const val ACK_PROCEED = "proceed"

    const val PRIORITY = 5
    const val PRIORITY_MAX = 15
    /** 멤버 `user-priority` 의 역할 기본값. */
    const val CHAIR_PRIORITY = 7
    const val PARTICIPANT_PRIORITY = 5

    /** mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members. */
    const val MCV_MAX_TRANSMITTERS = 2
    const val MCV_MAX_TRANSMITTERS_MIN = 1
    const val MCV_MAX_TRANSMITTERS_MAX = 16
    /** mcvideo-on-network-maximum-duration(TNG3, 초). */
    const val MCV_MAX_DURATION = 3600
    /** on-network-reception-hang-timer(T5, 초 — TS 24.581 §11.1.3). */
    const val MCV_RECEPTION_HANG = 30
    const val MCV_RECEPTION_HANG_MAX = 3600
    /** mcvideo-on-network-group-priority. */
    const val MCV_GROUP_PRIORITY_MAX = 255
    /** MCVideo 호 방식(mcvideo-on-network-invite-members) — chat 이 기본(mcvideo.md D5). */
    const val MCV_CHAT = "chat"
    const val MCV_PREARRANGED = "prearranged"
    /** 새로 켜는 그룹의 코덱 — 서버 기본값. */
    val MCV_AUDIO = listOf("AMR-WB")
    val MCV_VIDEO = listOf("H264")

    fun memberPriority(chair: Boolean): Int = if (chair) CHAIR_PRIORITY else PARTICIPANT_PRIORITY
}

/**
 * 편집 폼의 멤버 한 줄.
 *
 * @param readChair 문서에서 읽은 역할(의장인가) — 새로 더한 멤버는 null.
 * @param readPriority 문서에서 읽은 `user-priority` — 새로 더한 멤버는 null.
 * @param mcvideoId MCVideo entry 의 MCVideo ID — 폼은 편집하지 않고 읽은 값을 되돌린다.
 */
data class MemberRow(val uri: String, val name: String, val number: String,
                     val isChair: Boolean = false, val isMe: Boolean = false,
                     /** 필수 멤버 `<on-network-required>` — 개시자 응답 전에 이 멤버의 200 을 기다린다(TNG1, TS 24.379 §6.3.3.3). */
                     val required: Boolean = false,
                     val readChair: Boolean? = null,
                     val readPriority: Int? = null,
                     val mcvideoId: String = "") {
    val label: String get() = name.ifBlank { number }

    /**
     * 저장할 `user-priority` — 역할을 바꾸지 않았으면 **읽은 값 그대로**, 바꿨거나 새로 더한 멤버는 역할 기본값(의장 7 · 참가자 5).
     * PUT 이 멤버 목록 전체를 교체하므로 기본값으로 덮으면 콘솔이 준 멤버별 우선순위가 사라진다.
     */
    val priority: Int get() =
        if (readPriority != null && readChair == isChair) readPriority else GroupDefaults.memberPriority(isChair)
}

/**
 * 편집 폼 상태. 화면은 이 값만 그리고 판정은 [PttGroupsViewModel] 이 한다.
 *
 * `ifMatch` 는 폼을 **열 때**의 ETag 다 — 저장 사이에 다른 곳에서 바뀌면 412 로 걸린다.
 * `read` 는 열 때 받은 문서(신규 = null)다 — 어느 칸이 미기재였는지와 폼에 없는 MCVideo 속성을 여기서 되찾는다.
 */
data class EditForm(
    val isNew: Boolean = true,
    val uri: String = "",
    val groupId: String = "",
    val name: String = "",
    val sessionType: String = "prearranged",
    val allowSds: Boolean = true,
    val allowFd: Boolean = false,
    val emergencyCall: Boolean = true,
    val emergencyAlert: Boolean = true,
    val requireAffiliation: Boolean = true,
    val encryption: Boolean = false,
    val priority: Int = GroupDefaults.PRIORITY,
    val maxParticipants: Int = 0,
    val orgCode: String = "",
    // ── 그룹 호(TS 24.481 §7.2.2) ──
    val hangTimerSec: Int = GroupDefaults.HANG_TIMER,
    val maxDurationSec: Int = GroupDefaults.MAX_DURATION,
    val minNumberToStart: Int = 0,
    val ackTimeoutSec: Int = GroupDefaults.ACK_TIMEOUT,
    val ackAction: String = GroupDefaults.ACK_ABANDON,
    val allowConferenceState: Boolean = true,
    // ── 한도(MCData) ──
    val maxSdsSize: Int = GroupDefaults.MAX_SDS_SIZE,
    val maxAutoRecv: Int = GroupDefaults.MAX_AUTO_RECV,
    // ── 서비스 — MCVideo(§10.6) ──
    val mcVideo: Boolean = false,
    /** 읽은 문서에서 이미 켜져 있었다 — 스위치를 잠근다(끄기는 운영 콘솔). */
    val mcVideoLocked: Boolean = false,
    val mcVideoType: String = GroupDefaults.MCV_CHAT,
    val mcVideoMaxTransmitters: Int = GroupDefaults.MCV_MAX_TRANSMITTERS,
    val mcVideoMaxDurationSec: Int = GroupDefaults.MCV_MAX_DURATION,
    val mcVideoReceptionHangSec: Int = GroupDefaults.MCV_RECEPTION_HANG,
    val mcVideoMinNumberToStart: Int = 0,
    /** mcvideo-on-network-group-priority 0~255 — 빈 값 = 미기재(가장 낮음). */
    val mcVideoGroupPriority: String = "",
    val mcVideoAllowConferenceState: Boolean = true,
    val members: List<MemberRow> = emptyList(),
    val ifMatch: String = "",
    val search: String = "",
    val loaded: Boolean = false,
    val busy: Boolean = false,
    val error: String = "",
    val read: GroupDoc? = null,
) {
    val title: String get() = if (isNew) "새 PTT 그룹" else "그룹 편집 — $name"

    /** 그룹 uri 정규형 = `tel:<id>`(mcptt_api.md §2). 기존 그룹은 문서의 uri 그대로. */
    val docUri: String get() = if (isNew) telUri(groupId.trim()) else uri

    val canSave: Boolean get() = loaded && !busy && name.isNotBlank() && members.isNotEmpty() &&
        (!isNew || groupId.isNotBlank())

    /** [저장] 버튼의 글자. */
    val saveLabel: String get() = when {
        busy && loaded -> "저장 중…"
        isNew -> "그룹 만들기"
        else -> "저장"
    }
}

/** 문서에 없던 칸(read = null)이 기본값 그대로면 미기재(null) — 폼을 연 것만으로 서버 값을 명시값으로 굳히지 않는다. */
internal fun withUnset(read: Int?, value: Int, default: Int): Int? = if (read == null && value == default) null else value

/** 스위치 칸의 같은 규칙 — 문서에 없었고 기본값 그대로면 미기재. */
internal fun withUnset(read: Boolean?, value: Boolean, default: Boolean): Boolean? =
    if (read == null && value == default) null else value

/**
 * 문서 → 폼([편집]). 미기재 칸은 기본값으로 **보이기만** 한다(되돌릴 때 [withUnset] 이 다시 미기재로 만든다).
 *
 * @param fallback 목록의 행 — 문서가 uri·ETag 를 비워 보낼 때 쓴다.
 * @param myPttNumber 내 PTT 번호 — «(나)» 표시.
 * @param nameOf 번호 → 이름(주소록). 문서에 이름이 없는 멤버를 채운다.
 */
internal fun editFormOf(d: GroupDoc, fallback: ManagedGroup?, myPttNumber: String,
                        nameOf: (String) -> String = { "" }): EditForm {
    val uri = d.uri.ifBlank { fallback?.uri.orEmpty() }
    val me = DirectoryBook.normalize(myPttNumber)
    val mv = d.mcvideo
    return EditForm(
        isNew = false,
        uri = uri,
        groupId = userPart(uri),
        name = d.displayName,
        sessionType = if (d.sessionType in PttGroupsViewModel.SESSION_TYPES) d.sessionType else "prearranged",
        allowSds = d.allowSds, allowFd = d.allowFd,
        emergencyCall = d.emergencyCall, emergencyAlert = d.emergencyAlert,
        requireAffiliation = d.requireAffiliation, encryption = d.encryption,
        priority = d.priority, maxParticipants = d.maxParticipants, orgCode = d.orgCode,
        hangTimerSec = d.hangTimerSec ?: GroupDefaults.HANG_TIMER,
        maxDurationSec = d.maxDurationSec ?: GroupDefaults.MAX_DURATION,
        minNumberToStart = d.minNumberToStart ?: 0,
        ackTimeoutSec = d.ackTimeoutSec ?: GroupDefaults.ACK_TIMEOUT,
        ackAction = if (d.ackAction == GroupDefaults.ACK_PROCEED) GroupDefaults.ACK_PROCEED else GroupDefaults.ACK_ABANDON,
        allowConferenceState = d.allowConferenceState ?: true,
        maxSdsSize = d.maxSdsSize ?: GroupDefaults.MAX_SDS_SIZE,
        maxAutoRecv = d.maxAutoRecv ?: GroupDefaults.MAX_AUTO_RECV,
        mcVideo = mv != null, mcVideoLocked = mv != null,
        mcVideoType = if (mv?.inviteMembers == true) GroupDefaults.MCV_PREARRANGED else GroupDefaults.MCV_CHAT,
        mcVideoMaxTransmitters = mv?.maxTransmitters ?: GroupDefaults.MCV_MAX_TRANSMITTERS,
        mcVideoMaxDurationSec = mv?.maxDurationSec ?: GroupDefaults.MCV_MAX_DURATION,
        mcVideoReceptionHangSec = mv?.receptionHangTimerSec ?: GroupDefaults.MCV_RECEPTION_HANG,
        mcVideoMinNumberToStart = mv?.minNumberToStart ?: 0,
        mcVideoGroupPriority = mv?.groupPriority?.toString().orEmpty(),
        mcVideoAllowConferenceState = mv?.allowConferenceState ?: true,
        members = d.members.map { m ->
            val num = userPart(m.uri)
            val chair = m.role == "chair"
            MemberRow(m.uri, m.name.ifBlank { nameOf(num) }, num,
                isChair = chair, isMe = me.isNotEmpty() && DirectoryBook.normalize(num) == me,
                required = m.required, readChair = chair, readPriority = m.priority, mcvideoId = m.mcvideoId)
        },
        ifMatch = d.etag.ifBlank { fallback?.etag.orEmpty() },
        loaded = true,
        read = d)
}

/**
 * 폼 → 문서(XCAP PUT 본문). 범위 밖 값은 범위로 자르고, 미기재였던 칸이 기본값 그대로면 미기재로 둔다.
 */
internal fun groupDocOf(f: EditForm): GroupDoc {
    val r = f.read
    return GroupDoc(
        uri = f.docUri,
        displayName = f.name.trim(),
        members = f.members.map {
            GroupMember(it.uri, it.name, if (it.isChair) "chair" else "participant", it.priority,
                required = it.required, mcvideoId = it.mcvideoId)
        },
        sessionType = f.sessionType,
        encryption = f.encryption,
        emergencyCall = f.emergencyCall, emergencyAlert = f.emergencyAlert,
        allowSds = f.allowSds, allowFd = f.allowFd,
        requireAffiliation = f.requireAffiliation,
        priority = f.priority.coerceIn(0, GroupDefaults.PRIORITY_MAX),
        maxParticipants = f.maxParticipants.coerceAtLeast(0),
        orgCode = f.orgCode,
        hangTimerSec = withUnset(r?.hangTimerSec,
            f.hangTimerSec.coerceIn(0, GroupDefaults.HANG_TIMER_MAX), GroupDefaults.HANG_TIMER),
        maxDurationSec = withUnset(r?.maxDurationSec,
            f.maxDurationSec.coerceIn(0, GroupDefaults.MAX_DURATION_MAX), GroupDefaults.MAX_DURATION),
        allowConferenceState = withUnset(r?.allowConferenceState, f.allowConferenceState, true),
        maxSdsSize = withUnset(r?.maxSdsSize, f.maxSdsSize.coerceAtLeast(0), GroupDefaults.MAX_SDS_SIZE),
        maxAutoRecv = withUnset(r?.maxAutoRecv, f.maxAutoRecv.coerceAtLeast(0), GroupDefaults.MAX_AUTO_RECV),
        minNumberToStart = withUnset(r?.minNumberToStart,
            f.minNumberToStart.coerceIn(0, GroupDefaults.MIN_NUMBER_TO_START_MAX), 0),
        ackTimeoutSec = withUnset(r?.ackTimeoutSec,
            f.ackTimeoutSec.coerceIn(GroupDefaults.ACK_TIMEOUT_MIN, GroupDefaults.ACK_TIMEOUT_MAX), GroupDefaults.ACK_TIMEOUT),
        ackAction = if (r?.ackAction == null && f.ackAction == GroupDefaults.ACK_ABANDON) null else f.ackAction,
        mcvideo = if (f.mcVideo) mcVideoOf(f) else null)
}

/**
 * MCVideo 몫 — 읽은 것(없으면 새것)에 폼 값을 얹는다. 보호 둘은 **false 를 명시**한다(요소가 없으면 true 로 읽힌다 —
 * TS 24.481 §7.2.8, mcvideo.md D7). 새로 켜는 그룹의 코덱은 서버 기본값(AMR-WB · H264)을 싣는다.
 */
internal fun mcVideoOf(f: EditForm): McVideoGroupAttrs {
    val r = f.read?.mcvideo
    val base = r ?: McVideoGroupAttrs(audioEncodings = GroupDefaults.MCV_AUDIO, videoEncodings = GroupDefaults.MCV_VIDEO)
    return base.copy(
        protectMedia = false, protectTransmissionControl = false,
        inviteMembers = f.mcVideoType == GroupDefaults.MCV_PREARRANGED,
        maxTransmitters = withUnset(r?.maxTransmitters,
            f.mcVideoMaxTransmitters.coerceIn(GroupDefaults.MCV_MAX_TRANSMITTERS_MIN, GroupDefaults.MCV_MAX_TRANSMITTERS_MAX),
            GroupDefaults.MCV_MAX_TRANSMITTERS),
        maxDurationSec = withUnset(r?.maxDurationSec,
            f.mcVideoMaxDurationSec.coerceIn(0, GroupDefaults.MAX_DURATION_MAX), GroupDefaults.MCV_MAX_DURATION),
        receptionHangTimerSec = withUnset(r?.receptionHangTimerSec,
            f.mcVideoReceptionHangSec.coerceIn(0, GroupDefaults.MCV_RECEPTION_HANG_MAX), GroupDefaults.MCV_RECEPTION_HANG),
        minNumberToStart = withUnset(r?.minNumberToStart,
            f.mcVideoMinNumberToStart.coerceIn(0, GroupDefaults.MIN_NUMBER_TO_START_MAX), 0),
        groupPriority = f.mcVideoGroupPriority.trim().toIntOrNull()?.coerceIn(0, GroupDefaults.MCV_GROUP_PRIORITY_MAX),
        allowConferenceState = withUnset(r?.allowConferenceState, f.mcVideoAllowConferenceState, true))
}

// ── 상세 카드의 문구(순수 함수, 시험 대상) ───────────────────────────────────

/** 능력 줄의 MCVideo 몫 — "MCVideo chat · 송출 2" / "MCVideo 편성 · 송출 2". MCVideo 그룹이 아니면 빈 값. */
internal fun mcVideoText(a: McVideoGroupAttrs?): String =
    if (a == null) ""
    else "MCVideo " + (if (a.inviteMembers) "편성" else "chat") + (a.maxTransmitters?.let { " · 송출 $it" } ?: "")

/** 능력 칩 — 음성(늘) · 메시지(SDS) · 파일(FD) · MCVideo · 암호화 · affiliation 필요. 문서가 주는 것만 선다. */
internal fun capabilityChips(d: GroupDoc): List<String> = listOf(
    "MCPTT 음성",
    if (d.allowSds) "메시지(SDS)" else "",
    if (d.allowFd) "파일(FD)" else "",
    mcVideoText(d.mcvideo),
    if (d.encryption) "암호화" else "",
    if (d.requireAffiliation) "affiliation 필요" else "",
).filter { it.isNotEmpty() }

/** 정보 칸 «긴급» — 허용된 것만 잇고, 둘 다 꺼져 있으면 «허용 안 함». */
internal fun emergencyText(d: GroupDoc): String =
    listOf(if (d.emergencyCall) "긴급 통화" else "", if (d.emergencyAlert) "긴급 경보" else "")
        .filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { "허용 안 함" }

/** 정보 칸 «세션 종류». */
internal fun sessionTypeText(type: String): String = if (type == "chat") "채팅(chat)" else "사전편성(prearranged)"

/**
 * 정보 칸 «소유자» — 내 것은 "이름(나)", 남의 것은 주소록 이름(없으면 번호), 문서에 없으면 «—».
 *
 * @param owner 문서의 authorized user(uri).
 */
internal fun ownerLabel(owner: String, myPttId: String, myName: String, nameOf: (String) -> String = { "" }): String {
    if (owner.isBlank()) return "—"
    val number = userPart(owner)
    val name = nameOf(number)
    val me = DirectoryBook.normalize(userPart(myPttId))
    return if (me.isNotEmpty() && DirectoryBook.normalize(number) == me) name.ifBlank { myName }.ifBlank { number } + "(나)"
    else name.ifBlank { number }
}

/** 목록 바닥의 범위 안내 — 전부 고칠 수 있으면 그렇다고, 보기 전용 행이 섞였으면 몇 개가 관리 가능한지. */
internal fun groupListHint(groups: List<ManagedGroup>): String {
    if (groups.isEmpty()) return ""
    val manageable = groups.count { it.canManage }
    return if (manageable == groups.size) "관리 범위 안 그룹 전부"
    else "관리 가능 ${manageable}개 · 나머지는 청취 범위·멤버 그룹(보기만)"
}
