// 관제 세션의 PTT 평면 — 그룹·floor·로스터·SDS (docs/design/features/android_dispatch_tablet.md §6.7)
//
// DispatchSession 의 확장으로 둔다 — 세션이 하나인 것은 유지하면서 파일은 평면별로 나눈다.
// 여기 있는 것도 전부 **코어 상태의 투영**이다. 판단(참여 자격·floor 정책)은 코어와 서버에 있다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.FloorEvent
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.GroupCallOptions
import com.cims.ue.sdk.RosterUpdate
import com.cims.ue.sdk.SdsMessage

/** PTT 그룹 목록 갱신 — GMS 목록(멤버) + 프로비저닝 `pttTargets`(청취 범위). */
suspend fun DispatchSession.refreshGroups(): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val c = cscOrNull() ?: return CimsResult.fail(-1, "로그인 전")
    val token = accessToken() ?: return CimsResult.fail(-1, "로그인 전")

    val listed = c.listGroups(token, myPttId)
    if (!listed.ok) return CimsResult.fail(listed.code, listed.reason)

    val next = LinkedHashMap<String, GroupInfo>()
    val fresh = ArrayList<Pair<String, Boolean>>()          // (groupId, 멤버인가) — 새로 구독할 것

    // ① 멤버 그룹 — GMS 목록.
    listed.value.orEmpty().forEach { g ->
        val id = userPart(g.uri)
        if (id.isEmpty() || next.containsKey(id)) return@forEach
        val prev = groups.value.firstOrNull { it.id == id && it.isMember }
        next[id] = (prev ?: GroupInfo(id, g.uri, g.displayName.ifBlank { id })).copy(
            uri = g.uri, name = g.displayName.ifBlank { id },
            memberCount = g.memberCount, isOwner = g.isOwner, etag = g.etag, isMember = true)
        if (prev == null) fresh.add(id to true)
    }
    // ② 청취 범위 그룹 — 서버가 pttListen 범위를 해석한 목록. 참여하지 않고 로스터만 받는다.
    if (canListenPtt) dispatch.pttTargets.forEach { t ->
        if (t.id.isEmpty() || next.containsKey(t.id)) return@forEach
        val prev = groups.value.firstOrNull { it.id == t.id && !it.isMember }
        next[t.id] = prev ?: GroupInfo(t.id, t.uri.ifBlank { "tel:${t.id}" }, t.name.ifBlank { t.id },
                                       isMember = false)
        if (prev == null) fresh.add(t.id to false)
    }

    // 빠진 그룹 — 구독·affiliation 을 푼다.
    val gone = groups.value.filter { !next.containsKey(it.id) }

    // **모델을 먼저 게시하고 그다음 구독한다.**
    // 서버는 구독을 받아들이는 즉시 현재 로스터를 NOTIFY 로 보낸다(`CscfModule`·`CspServer`).
    // 구독을 먼저 걸면 그 NOTIFY 가 «아직 목록에 없는 그룹» 으로 도착해 `applyRoster` 가 조용히
    // 버린다 — 이미 진행 중이던 세션이 빈 로스터로 남는다. `watchAll` 과 같은 불변이다.
    setGroups(next.values.toList())

    gone.forEach {
        if (it.isMember) ptt.affiliate(it.id, false)
        ptt.subscribeConference(it.id, false)
    }
    fresh.forEach { (id, isMember) ->
        // 범위 밖·자격 없음은 서버가 403 + Warning 138 로 거절한다 — 구독은 한 번, 재시도 루프는 없다.
        if (isMember) {
            val aff = ptt.affiliate(id, true)
            updateGroup(id) { it.copy(affiliated = aff.ok) }
        }
        ptt.subscribeConference(id, true)
    }
    return CimsResult.ok(Unit)
}

/** 그룹콜 참여 — ① 카드의 [참여]. 이미 세션이 있으면 코어가 그 호를 돌려준다. */
suspend fun DispatchSession.joinGroup(groupId: String, emergency: Boolean = false): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val r = ptt.joinGroupCall(groupId, GroupCallOptions(emergency = emergency))
    if (r.ok) noteOperation(r.value!!.id, if (emergency) Operation.EMERGENCY else Operation.PTT_JOIN)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
}

/** 청취 합류 — ② 카드의 [청취]. `a=recvonly` 라 발언 버튼이 비활성된다. */
suspend fun DispatchSession.listenGroup(groupId: String): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val r = ptt.joinGroupCall(groupId, GroupCallOptions(listenOnly = true))
    if (r.ok) noteOperation(r.value!!.id, Operation.PTT_LISTEN)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
}

/** 세션 이탈 — 카드의 [나가기]·[청취 끄기]. */
suspend fun DispatchSession.leave(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return ue.call(callId).leaveGroupCall()
}

/** PTT 누름 — 그 세션의 floor 를 요청한다. */
suspend fun DispatchSession.floorRequest(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return ue.call(callId).floorRequest()
}

/** PTT 뗌 — 대기 중이면 코어가 Queued Cancel 을 먼저 보낸다. */
suspend fun DispatchSession.floorRelease(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return ue.call(callId).floorRelease()
}

/** 그룹 SDS 발신 — 최종 응답은 token 으로 `requestResult` 에서 맞춘다. */
suspend fun DispatchSession.sendGroupSds(groupId: String, text: String): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val r = ptt.sendGroupSds(groupId, text)
    if (!r.ok) return CimsResult.fail(r.code, r.reason)
    addOutgoingMessage(groupId, text, r.value!!.msgId, r.value!!.token)
    return CimsResult.ok(Unit)
}

// ── 이벤트 접기 ───────────────────────────────────────────────────────────────
// 아래는 DispatchSession 이 코어 이벤트를 받아 부르는 것들이다. 화면이 읽는 모양으로 접기만 한다.

/** floor 이벤트 → 세션 상태 + ⑤ 이벤트 줄. */
internal fun DispatchSession.applyFloor(ev: FloorEvent) {
    val s = sessionOf(ev.callId) ?: return
    val speakerName = ev.talkers.firstOrNull { !it.self }?.id?.let { displayName(it) } ?: ""
    val speaking = ev.state == FloorState.SPEAKING
    val taken = ev.talkers.isNotEmpty() && !speaking

    // 이벤트가 실어 온 값으로 먼저 그리고(즉시 반응), 권위 있는 스냅샷은 뒤따라 채운다(§F3).
    updateSession(ev.callId) {
        it.copy(
            floor = it.floor?.copy(state = ev.state, queuePosition = ev.queuePosition,
                                   talkers = ev.talkers, canRequest = ev.permission != 0)
                ?: com.cims.ue.sdk.FloorInfo(
                    state = ev.state, talkers = ev.talkers, canRequest = ev.permission != 0,
                    indicator = ev.indicator, queuePosition = ev.queuePosition,
                    localPort = 0, remoteIp = "", remotePort = 0,
                    grantedCount = 0, takenCount = 0, denyCount = 0),
            lastFloor = ev,
            speaker = when {
                speaking -> "나"
                taken -> speakerName
                else -> ""
            },
            speakerSinceMs = when {
                speaking || taken -> it.speakerSinceMs ?: System.currentTimeMillis()
                else -> null
            },
            floorNote = floorNoteOf(ev))
    }

    // ⑤ 이벤트 — 진행 중 행은 없다. 전이만 남긴다.
    val gname = groupNameOf(s.info.groupId)
    when (ev.state) {
        FloorState.SPEAKING -> addActivity(s.info.groupId, gname, "발언 시작 · 나", ActivityKind.TALK, s.isEmergency)
        FloorState.IDLE -> if (s.isSpeaking || s.speaker.isNotEmpty())
            addActivity(s.info.groupId, gname, "발언 종료", ActivityKind.TALK, s.isEmergency)
        else -> Unit
    }
    if (taken && speakerName.isNotEmpty() && s.speaker != speakerName)
        addActivity(s.info.groupId, gname, "발언 $speakerName", ActivityKind.TALK, s.isEmergency)
    if (ev.causeText.isNotEmpty() && ev.state != FloorState.SPEAKING)
        addActivity(s.info.groupId, gname, ev.causeText, ActivityKind.ERROR, s.isEmergency)

    pullFloor(ev.callId)          // 권위 있는 스냅샷으로 덮는다
}

/** 로스터 NOTIFY → 그룹 참가자·진행 여부. 참여하지 않은 청취 범위 그룹도 이걸로 안다. */
internal fun DispatchSession.applyRoster(u: RosterUpdate) {
    val before = groups.value.firstOrNull { it.id == u.groupId }
    updateGroup(u.groupId) { it.withRoster(u.users) }
    val after = groups.value.firstOrNull { it.id == u.groupId } ?: return
    if (before?.hasSession != true && after.hasSession)
        addActivity(after.id, after.name, "세션 시작 · 참가 ${after.connectedCount}", ActivityKind.JOIN)
    else if (before?.hasSession == true && !after.hasSession)
        addActivity(after.id, after.name, "세션 종료", ActivityKind.LEAVE)
}

/** SDS 수신 → ④ 스레드 + ⑤ 줄. disposition 요청이 있으면 통지를 되돌린다. */
internal fun DispatchSession.applySds(msg: SdsMessage) {
    if (msg.notification) return updateSendState(msg.msgId, msg.notifType)
    val groupId = userPart(msg.groupUri).ifEmpty { userPart(msg.fromUri) }
    addIncomingMessage(groupId, msg)
    addActivity(groupId, groupNameOf(groupId), "메시지 · ${displayName(msg.fromUri)}", ActivityKind.SDS)
}

/** 호 상태 변화 → 세션 목록. 종료된 호는 ⑤ 에 남기고 목록에서 뺀다. */
internal fun DispatchSession.applyCallState(c: CallInfo) {
    if (c.state == CallState.DISCONNECTED) {
        sessionOf(c.callId)?.let { s ->
            if (s.kind.isPttCard || s.kind == SessionKind.PTT_LISTEN)
                addActivity(s.info.groupId, groupNameOf(s.info.groupId),
                    if (c.lastCode >= 300) "세션 실패 ${c.lastCode}" else "세션 종료", ActivityKind.LEAVE, s.isEmergency)
            else noteMyCall(s, c)
        }
        removeSession(c.callId)
        return
    }
    upsertSession(c)

    // 새 통화가 붙으면 **기존 활성 통화를 보류한다**(데스크톱 `DispatchSession.cs` 의 AutoHoldOnAnswer).
    // 그러지 않으면 두 통화가 동시에 들려 관제사가 어느 쪽에 말하는지 알 수 없다.
    if (c.state == CallState.ACTIVE && SessionKind.of(c) == SessionKind.PHONE_CALL &&
        settingsSnapshot().autoHoldOnAnswer) {
        holdOtherCalls(c.callId)
    }
}

/** [exceptCallId] 를 뺀 활성 전화 통화를 전부 보류한다. */
private fun DispatchSession.holdOtherCalls(exceptCallId: Int) {
    val others = sessions.value.filter {
        it.callId != exceptCallId && it.kind == SessionKind.PHONE_CALL && it.isActive
    }
    if (others.isEmpty()) return
    scopeLaunch { others.forEach { hold(it.callId, true) } }
}

/**
 * 끝난 **내 전화**를 ⑥ 내역과 오늘 집계에 남긴다.
 *
 * 내 통화의 권위는 **내 세션**이다. dialog 이벤트(`applyDialog`)로 대신할 수 없다 — 그건 관제 그룹원
 * 감시용이라 ① 관제 역할이 없으면 아예 오지 않고, ② 내가 `members[]` 에 없거나 거기 실린 번호가
 * 내 등록 회선(유선 voip)과 다르면 «타인 통화» 로 분류돼 내 내역에서 빠진다.
 */
private fun DispatchSession.noteMyCall(s: SessionItem, c: CallInfo) {
    val answered = s.connectedAtMs != null
    val outgoing = c.dir == com.cims.ue.sdk.CallDir.OUTGOING
    val kind = when {
        s.kind == SessionKind.PHONE_MONITOR -> CallLogKind.MONITOR
        s.operation == Operation.PICKUP -> CallLogKind.PICKUP
        s.operation == Operation.TRANSFER -> CallLogKind.TRANSFER
        outgoing -> CallLogKind.OUTGOING
        answered -> CallLogKind.ANSWERED
        else -> CallLogKind.MISSED
    }
    val text = when {
        s.kind == SessionKind.PHONE_MONITOR -> "감청 종료"
        kind == CallLogKind.MISSED && c.lastCode >= 300 -> "부재 ${c.lastCode}"
        kind == CallLogKind.MISSED -> "부재"
        answered -> "통화 종료"
        else -> "미응답"                         // 내가 걸었는데 상대가 안 받았다
    }
    addCallLog(CallLogRow(
        atMs = System.currentTimeMillis(),
        peer = displayName(c.remoteUri),
        text = text,
        kind = kind,
        others = false,
        number = userPart(c.remoteUri),
        startedAtMs = s.startedAtMs,
        answeredAtMs = s.connectedAtMs))
}

/** Denied/Revoked 사유를 화면 문구로 — 사전은 dispatch_desktop_ui.md §9 가 정본이다. */
private fun floorNoteOf(ev: FloorEvent): String = when {
    ev.causeText.isNotEmpty() -> ev.causeText
    ev.state == FloorState.QUEUED && ev.queuePosition > 0 -> "대기 ${ev.queuePosition}번째"
    else -> ""
}

// ── GMS 그룹 관리(§4.7 — TS 24.481 XCAP) ─────────────────────────────────────

/** 새 그룹 uri — `tel:g-<8hex>`. 충돌(409 `uri_taken`)은 [saveGroup] 이 한 번 조용히 다시 만든다. */
fun newGroupUri(): String = "tel:g-" + java.util.UUID.randomUUID().toString().replace("-", "").take(8)

/** 편집할 그룹 문서를 받는다 — ETag 가 함께 오고, 저장은 그 값을 If-Match 로 쓴다. */
suspend fun DispatchSession.getGroupDoc(groupUri: String): CimsResult<com.cims.ue.sdk.GroupDoc> {
    val c = cscOrNull() ?: return CimsResult.fail(-1, "로그인 전")
    val t = accessToken() ?: return CimsResult.fail(-1, "로그인 전")
    return c.getGroup(t, myPttId, groupUri)
}

/**
 * 그룹 생성·수정(XCAP PUT).
 *
 * `ifMatch` 는 편집을 시작할 때의 ETag — 다른 곳에서 먼저 바뀌었으면 412 가 온다. 신규 id 충돌
 * (409 `uri_taken`)은 id 를 다시 만들어 **한 번만** 조용히 재시도한다. 성공 문서의 uri 가 정본이라
 * 호출자는 응답의 uri 를 쓴다.
 */
suspend fun DispatchSession.saveGroup(doc: com.cims.ue.sdk.GroupDoc,
                                      ifMatch: String = ""): CimsResult<com.cims.ue.sdk.GroupDoc> {
    val c = cscOrNull() ?: return CimsResult.fail(-1, "로그인 전")
    val t = accessToken() ?: return CimsResult.fail(-1, "로그인 전")
    val isNew = ifMatch.isBlank()
    var r = c.putGroup(t, myPttId, doc, ifMatch)
    if (!r.ok && isNew && r.code == 409) r = c.putGroup(t, myPttId, doc.copy(uri = newGroupUri()), "")
    if (r.ok) {
        val d = r.value!!
        addActivity(userPart(d.uri), d.displayName,
            "그룹 ${if (isNew) "생성" else "편집"} · 멤버 ${d.members.size}", ActivityKind.JOIN)
    }
    return r
}

/** 그룹 삭제 — 관리 범위 안이거나 본인 소유만(서버 판정, 403). */
suspend fun DispatchSession.deleteGroup(groupUri: String): CimsResult<Unit> {
    val c = cscOrNull() ?: return CimsResult.fail(-1, "로그인 전")
    val t = accessToken() ?: return CimsResult.fail(-1, "로그인 전")
    val r = c.deleteGroup(t, myPttId, groupUri)
    if (r.ok) addActivity(userPart(groupUri), groupNameOf(userPart(groupUri)), "그룹 삭제", ActivityKind.LEAVE)
    return r
}

/** `tel:` 정규형 — 번호만 들어와도 uri 로 만든다. */
fun telUri(number: String): String =
    if (number.contains(':')) number else "tel:${number.trim()}"

// ── 사설콜·애드혹(§4.1) ──────────────────────────────────────────────────────

/**
 * 애드혹 임시 그룹 id — `adhoc-<내 PTT 번호>-<epoch초>`.
 *
 * 규약은 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) §6 이고 `adhoc-`·`priv-` 는 편성 그룹
 * 예약어다. **앱이 만든다** — 서버에 없는 임시 세션이라 채널 영속·affiliation·로스터 구독 대상이 아니다.
 * 데스크톱 `Services/AdhocIdFactory.cs` 와 같은 규칙.
 */
internal fun adhocIdOf(myPttId: String, nowSec: Long = System.currentTimeMillis() / 1000): String {
    var n = myPttId.trim()
    for (scheme in listOf("tel:", "sip:", "sips:"))
        if (n.startsWith(scheme, ignoreCase = true)) { n = n.substring(scheme.length); break }
    n = n.substringBefore('@').substringBefore(';')
    return "adhoc-$n-$nowSec"
}

internal fun isAdhocId(groupId: String): Boolean = groupId.startsWith(SessionKind.ADHOC_PREFIX)

/**
 * 사설콜 발신 — PTT 사용자 1명과 1:1.
 *
 * 반이중(floor)이 기본이다. 전이중(`fullDuplex` = `mc_no_floor_ctrl`)은 마이크가 늘 열려 있어
 * 발언 대상 체크가 비활성되고 카드의 [음소거]로 다룬다(§4.1).
 */
suspend fun DispatchSession.startPrivateCall(peer: String, fullDuplex: Boolean = false,
                                             emergency: Boolean = false): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val target = userPart(peer).ifEmpty { return CimsResult.fail(-1, "대상을 고르세요") }
    val r = ptt.startPrivateCall(target, GroupCallOptions(fullDuplex = fullDuplex, emergency = emergency))
    if (r.ok) noteOperation(r.value!!.id, if (emergency) Operation.EMERGENCY else Operation.PTT_PRIVATE)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
}

/**
 * 애드혹 세션 개설 — PTT 사용자 N명(최소 1).
 *
 * 참가자는 `resource-lists` 로 싣는다. 서버가 그 목록으로 초대하며, 임시 그룹이라 목록을 **앱이
 * 기억한다** — 로스터 구독 대상이 아니라 카드에 몇 명인지 보이려면 여기밖에 없다.
 */
suspend fun DispatchSession.startAdhoc(members: List<String>,
                                       emergency: Boolean = false): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val tels = members.map { telUri(userPart(it)) }.filter { it != "tel:" }.distinct()
    if (tels.isEmpty()) return CimsResult.fail(-1, "대상을 고르세요")
    val id = adhocIdOf(myPttId)
    val r = ptt.joinGroupCall(id, GroupCallOptions(emergency = emergency, members = tels))
    if (!r.ok) return CimsResult.fail(r.code, r.reason)
    noteOperation(r.value!!.id, if (emergency) Operation.EMERGENCY else Operation.PTT_ADHOC)
    rememberAdhocMembers(r.value!!.id, tels)
    return CimsResult.ok(Unit)
}
