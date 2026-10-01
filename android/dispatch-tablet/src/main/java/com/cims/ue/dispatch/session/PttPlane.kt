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

/**
 * 일제 통화 개시(TS 24.379 §4.12 — 편성 그룹에 prearranged + `broadcast-ind`, mcptt_broadcast_group_call.md) — 채널 머리의
 * [일제 통화] 한 버튼. 누르는 동안 개시하고 말하므로 개시 INVITE 가 암묵적 발언 요청이다(TS 24.380 §14.2.5 — `implicitFloorRequest`).
 * 진행 중인 호는 일제 통화로 바꿀 수 없고(TS 24.379 §10.1.1.3.1.1 15) — 합류가 된다) 채팅 그룹은 서버가 broadcast-ind 를 무시하므로
 * 여기서 막는다. 그룹 종류를 모르면 GMS 그룹 문서(`on-network-invite-members`)로 먼저 확인한다. 반환 = 개시한 호 id. 끝은 [releaseBroadcast].
 */
suspend fun DispatchSession.startBroadcast(groupId: String): CimsResult<Int> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val g = groups.value.firstOrNull { it.id == groupId && it.isMember } ?: return CimsResult.fail(-1, "멤버 그룹이 아닙니다")
    fun ongoing() = groups.value.firstOrNull { it.id == groupId }?.hasSession == true ||
        sessions.value.any { it.kind == SessionKind.PTT_CHANNEL && it.info.groupId == groupId }
    if (ongoing()) return CimsResult.fail(-1, "${g.name} — 진행 중인 그룹 통화가 있어 일제 통화를 열 수 없습니다")
    var type = g.sessionType
    if (type.isEmpty()) {
        val d = getGroupDoc(g.uri)
        if (d.ok) { type = d.value!!.sessionType; updateGroup(groupId) { it.copy(sessionType = type) } }
    }
    if (type == "chat") return CimsResult.fail(-1, "${g.name} — 채팅 그룹은 일제 통화를 열 수 없습니다(편성 그룹만)")
    if (ongoing()) return CimsResult.fail(-1, "${g.name} — 일제 통화를 열 수 없습니다")      // 조회하는 사이 바뀜
    val r = ptt.joinGroupCall(groupId, GroupCallOptions(broadcast = true, implicitFloorRequest = true))
    if (!r.ok) return CimsResult.fail(r.code, r.reason)
    val id = r.value!!.id
    noteOperation(id, Operation.PTT_JOIN)
    broadcastPending.add(id)
    return CimsResult.ok(id)
}

/**
 * 일제 통화 한 버튼을 놓았다 — 호 성립 전이면 호 취소(CANCEL — 말하지 않은 일제 통화는 열지 않는다), 성립 뒤면 Floor Release.
 * 승인 전에 놓아도 코어가 발언권을 돌려주고, 서버의 B-bit Floor Idle 에 코어가 호를 해제한다(TS 24.380 §6.2.4.6.4).
 */
suspend fun DispatchSession.releaseBroadcast(callId: Int) {
    val ue = engineOrNull() ?: return
    val s = sessionOf(callId)
    if (s != null && !s.isLive) return
    if (s == null || !s.isActive) ue.call(callId).hangup() else ue.call(callId).floorRelease()
}

/** 그룹콜 참여 — ① 카드의 [참여]. 이미 세션이 있으면 코어가 그 호를 돌려준다. */
suspend fun DispatchSession.joinGroup(groupId: String, emergency: Boolean = false): CimsResult<Unit> {
    val area = if (emergency) TextArea.EMERGENCY else TextArea.PTT_JOIN
    val ptt = pttAccount ?: return report(area, CimsResult.fail(-1, "PTT 계정 없음"))
    val r = ptt.joinGroupCall(groupId, GroupCallOptions(emergency = emergency))
    if (r.ok) noteOperation(r.value!!.id, if (emergency) Operation.EMERGENCY else Operation.PTT_JOIN)
    return report(area, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/** 청취 합류 — ② 카드의 [청취]. `a=recvonly` 라 발언 버튼이 비활성된다. */
suspend fun DispatchSession.listenGroup(groupId: String): CimsResult<Unit> {
    val ptt = pttAccount ?: return report(TextArea.PTT_LISTEN, CimsResult.fail(-1, "PTT 계정 없음"))
    // 상한은 앱에서 먼저 본다 — 넘겨 보내면 서버가 486 으로 거절하고 관제사는 이유를 모른다(§6.5).
    if (listenLimitReached())
        return report(TextArea.PTT_LISTEN, CimsResult.fail(-1, "동시 청취 상한 ${settingsSnapshot().maxListen}"))
    val r = ptt.joinGroupCall(groupId, GroupCallOptions(listenOnly = true))
    if (r.ok) noteOperation(r.value!!.id, Operation.PTT_LISTEN)
    return report(TextArea.PTT_LISTEN, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/** 세션 이탈 — 카드의 [나가기]·[청취 끄기]. */
suspend fun DispatchSession.leave(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return report(TextArea.PTT_JOIN, CimsResult.fail(-1, "엔진 없음"))
    return report(TextArea.PTT_JOIN, ue.call(callId).leaveGroupCall())
}

/**
 * 진행 중 그룹콜의 긴급 상향·하향(TS 24.379 §10.1.1.2.1.3~5) — 채널 상세의 [긴급]·[긴급 해제]. 결과는 코어의 조건 이벤트로 온다
 * (확정 = 2xx, 거절 = 이전 값 복원 — 미인가 상향 403, 호는 유지 — `applyCondition`). 곧바로 실패한 것만 여기서 토스트로 남긴다.
 */
suspend fun DispatchSession.setEmergency(callId: Int, on: Boolean): CimsResult<Unit> {
    val area = if (on) TextArea.EMERGENCY else TextArea.EMERGENCY_CANCEL
    val ue = engineOrNull() ?: return report(area, CimsResult.fail(-1, "엔진 없음"))
    if (!on) conditionCancel.add(callId)                 // 해제 거절(403)은 상향 거절과 다른 문구 — applyCondition
    val r = ue.call(callId).setCondition(emergency = on)
    if (!r.ok && !on) conditionCancel.remove(callId)
    return report(area, r)
}

/** PTT 누름 — 그 세션의 floor 를 요청한다. */
suspend fun DispatchSession.floorRequest(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return report(TextArea.PTT_JOIN, CimsResult.fail(-1, "엔진 없음"))
    return report(TextArea.PTT_JOIN, ue.call(callId).floorRequest())
}

/** PTT 뗌 — 대기 중이면 코어가 Queued Cancel 을 먼저 보낸다. */
suspend fun DispatchSession.floorRelease(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return ue.call(callId).floorRelease()
}

/** 그룹 SDS 발신 — 최종 응답은 token 으로 `requestResult` 에서 맞춘다. */
suspend fun DispatchSession.sendGroupSds(groupId: String, text: String): CimsResult<Unit> {
    val ptt = pttAccount ?: return report(TextArea.SDS, CimsResult.fail(-1, "PTT 계정 없음"))
    val (r, early) = sendTracked({ it.token }) { ptt.sendGroupSds(groupId, text) }
    if (!r.ok) return report(TextArea.SDS, CimsResult.fail(r.code, r.reason))
    addOutgoingMessage(groupId, text, r.value!!.msgId, r.value!!.token, early)
    return CimsResult.ok(Unit)
}

/**
 * 1:1 SDS 발신 — 사람에게 직접(TS 24.282 `one-to-one-sds`).
 *
 * **그룹 경로로 보내면 안 된다.** request-type·Request-URI·conversation ID 가 다르고, 그룹으로 보내면
 * 서버가 그룹 게이트를 거쳐 받는 쪽 스레드 귀속도 틀어진다([mcdata_messaging.md](…) §4 표).
 * 스레드 키는 **상대 PTT 번호**다 — 수신 경로(`applySds`)가 `groupUri` 가 없을 때 쓰는 키와 같아야
 * 보낸 말풍선과 받은 말풍선이 한 대화에 선다.
 */
suspend fun DispatchSession.sendSds(peer: String, text: String): CimsResult<Unit> {
    val ptt = pttAccount ?: return report(TextArea.SDS, CimsResult.fail(-1, "PTT 계정 없음"))
    val to = userPart(peer)
    if (to.isEmpty() || text.isBlank()) return report(TextArea.SDS, CimsResult.fail(-1, "받는 사람·내용 없음"))
    val (r, early) = sendTracked({ it.token }) { ptt.sendSds(to, text) }
    if (!r.ok) return report(TextArea.SDS, CimsResult.fail(r.code, r.reason))
    addOutgoingMessage(to, text, r.value!!.msgId, r.value!!.token, early)
    return CimsResult.ok(Unit)
}

/**
 * 스레드 키 하나로 보낸다 — 키가 **편성 그룹이면 그룹 SDS**, 아니면 1:1.
 *
 * 화면이 «이 대화가 그룹인가» 를 다시 판정하지 않게 여기 한 곳에 둔다. 판정 근거는 받아 둔 그룹 목록이다
 * (서버가 준 편성이 정본 — 번호 모양으로 추측하면 숫자 그룹 id 에서 틀린다).
 */
suspend fun DispatchSession.sendSdsTo(key: String, text: String): CimsResult<Unit> =
    if (isPttGroup(key)) sendGroupSds(key, text) else sendSds(key, text)

/**
 * 실패한 SDS 재전송 — 같은 스레드 규칙(그룹이면 그룹 SDS, 아니면 1:1)으로 다시 보내고 **같은 말풍선**을 갱신한다.
 * **처음의 msgId 로** 보낸다 — 앞 발신이 일부에게 닿았어도 받는 쪽이 같은 메시지로 대조하고, 전달 확인 통지도 그 msgId 로
 * 맞물린다(SDK `sendSds`·`sendGroupSds` 의 msgId, mcdata_messaging.md §5 «탭=같은 msgId 재전송»). token 만 새로 받는다.
 */
suspend fun DispatchSession.resendSds(m: Message): CimsResult<Unit> {
    if (m.kind != MessageKind.SDS) return CimsResult.ok(Unit)
    val ptt = pttAccount ?: return report(TextArea.SDS, CimsResult.fail(-1, "PTT 계정 없음"))
    if (!beginResend(m)) return CimsResult.ok(Unit)                 // 이미 다시 보내는 중이거나 실패가 아니다
    val group = isPttGroup(m.groupId)
    val (r, early) = sendTracked({ it.token }) {
        if (group) ptt.sendGroupSds(m.groupId, m.text, msgId = m.msgId) else ptt.sendSds(m.groupId, m.text, msgId = m.msgId)
    }
    markResent(m, r.value?.msgId.orEmpty(), r.value?.token ?: 0L, failed = !r.ok, early = early)
    return report(TextArea.SDS, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/** 이 키가 편성 그룹인가 — 그룹 목록에 있으면 그룹, 없으면 사람. */
fun DispatchSession.isPttGroup(key: String): Boolean =
    groups.value.any { it.id == key }

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

    // 일제 통화로 연 호의 첫 서버 floor 메시지에 B-bit 가 없다 = 서버가 일반 통화로 열었다(mcptt_broadcast_group_call.md §3.2·R13).
    //   서버 메시지만 본다 — 코어 타이머 이벤트와 200 OK 의 승인 표시(`mc_granted`)는 rawType -1 이다.
    if (ev.rawType in BROADCAST_JUDGE_OPS && broadcastPending.remove(ev.callId) &&
        (ev.indicator and FLOOR_IND_BROADCAST) == 0) {
        addActivity(s.info.groupId, gname,
            if (s.kind == SessionKind.PTT_ADHOC) "일제 통화로 열리지 않았습니다 — 서버가 애드혹 일제 통화를 받지 않아 일반 애드혹 그룹 통화로 이어집니다"
            else "일제 통화로 열리지 않았습니다 — 진행 중 통화에 합류했거나 편성 그룹이 아닙니다(일반 그룹 통화로 이어집니다)",
            ActivityKind.ERROR, s.isEmergency)
    }

    pullFloor(ev.callId)          // 권위 있는 스냅샷으로 덮는다
}

/** 일제 통화 판정에 쓰는 서버 floor 메시지(TS 24.380 §8.2.2) — Granted·Taken·Deny·Idle·Revoke. 서버는 여기에 Floor Indicator 를 싣는다. */
private val BROADCAST_JUDGE_OPS = setOf(0x01, 0x02, 0x03, 0x05, 0x06)

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

/** SDS 수신 → ④ 스레드 + ⑤ 줄. 전달 확인을 요청했으면 DELIVERED 를 되돌린다([deliveryReplyTo]). */
internal fun DispatchSession.applySds(msg: SdsMessage) {
    if (msg.notification) return updateSendState(msg.msgId, msg.notifType)
    val groupId = userPart(msg.groupUri).ifEmpty { userPart(msg.fromUri) }
    addIncomingMessage(groupId, msg)
    addActivity(groupId, groupNameOf(groupId), "메시지 · ${displayName(msg.fromUri)}", ActivityKind.SDS)
    deliveryReplyTo(msg)?.let { peer ->
        val ptt = pttAccount ?: return@let
        scopeLaunch {
            // 이 발신에는 말풍선이 없다 — 실패를 남기지 않으면 상대가 «보냄» 에 멈춘 이유를 어디서도 못 찾는다.
            //   즉시 실패는 여기서, 최종 거절(480 등)은 token 으로 `applyRequestResult` 가 남긴다.
            val (r, early) = sendTracked({ it.token }) {
                // 규격형 통지(TS 24.282 §12.2.1.1) — 그룹 SDS 면 mcdata-calling-group-id. PSI 는 계정의 mcdataServerUri(ue-init-config)
                ptt.sendSdsNotification(peer, msg.convId, msg.msgId, SDS_NOTIF_DELIVERED, msg.groupUri)
            }
            val sent = r.value
            val what = "→ $peer msg=${msg.msgId}"
            if (r.ok && sent != null) {
                // 최종 응답이 명령보다 먼저 와 있었으면 곧바로 맞추고, 아니면 token 을 걸어 둔다(`TokenLedger`).
                if (early != null) logNotificationResult(what, early) else notificationTokens[sent.token] = what
            } else android.util.Log.w(SDS_TAG, "전달 확인 회신 실패 $what: ${r.code} ${r.reason}")
        }
    }
}

internal const val SDS_TAG = "DispatchSds"

/** SDS NOTIFICATION 종류 «전달됨»(TS 24.282 §15 — 1 미전달·2 전달·3 읽음·4 전달+읽음). */
internal const val SDS_NOTIF_DELIVERED = 2

/**
 * 전달 확인을 되돌릴 상대 — 없으면 null.
 *
 * 발신자가 delivery 를 요청했으면(disposition 1 delivery·3 both) **받은 즉시** DELIVERED 를 원 발신자에게 1:1 로
 * 되돌린다 — 발신 말풍선의 ✓ 가 이것으로 선다(mcdata_messaging.md §3). 보내지 않으면 상대 화면은 영영 «보냄» 에
 * 멈춘다. 읽음(read) 통지는 보내지 않는다 — 최소 프로파일이 DELIVERED 만 쓴다(같은 문서 §7 편차 표). 데스크톱
 * `McDataMessagesViewModel.OnSds` 와 같은 규칙이다.
 */
internal fun deliveryReplyTo(msg: SdsMessage): String? {
    if (msg.notification || (msg.dispositionReq != 1 && msg.dispositionReq != 3)) return null
    if (msg.msgId.isEmpty()) return null
    return userPart(msg.fromUri).ifEmpty { null }
}

/** 호 상태 변화 → 세션 목록. 종료된 호는 ⑤ 에 남기고 목록에서 뺀다. */
internal fun DispatchSession.applyCallState(c: CallInfo) {
    if (c.state == CallState.DISCONNECTED) {
        sessionOf(c.callId)?.let { s ->
            noteFailedAttempt(s, c)
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

/**
 * 연결되지 못하고 끝난 **내 발신**의 사유 → 토스트(데스크톱 `End` 의 «실패한 발신 동작의 사유»).
 *
 * 발신·당겨받기·감청·합류·개별 통화는 명령이 받아들여진 뒤 SIP 응답으로 실패한다 — 명령 결과로는 알 수 없고, 호가 끝날
 * 때 `lastCode` 로만 안다. 착신과 연결됐다 끝난 호(BYE)는 실패가 아니다. 영역은 그 호를 만든 동작으로 고르고, 동작을
 * 모르는 MCPTT 발신은 개별 통화/그룹 참여로 본다.
 */
private fun DispatchSession.noteFailedAttempt(s: SessionItem, c: CallInfo) {
    if (s.connectedAtMs != null || c.dir != com.cims.ue.sdk.CallDir.OUTGOING || c.lastCode < 300) return
    val area = if (s.operation == Operation.DIAL && c.isMcptt)
        (if (s.kind == SessionKind.PTT_PRIVATE) TextArea.PTT_PRIVATE else TextArea.PTT_JOIN)
    else ResponseText.areaOf(s.operation)
    notify(NoticeLevel.ERROR, ResponseText.sip(area, c.lastCode, c.lastReason), "${c.lastCode} ${c.lastReason}".trim())
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
        s.consultFor != null && answered -> "상담 전달"     // 데스크톱 «전달 … attended» — 원 통화는 제 내역이 따로 남는다
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
        answeredAtMs = s.connectedAtMs,
        // 내 착신이 대표번호를 거쳐 왔나 — 카드 «대표» 배지와 같은 근거(`calledParty` 가 차면 재타게팅된 호).
        viaPilot = c.calledParty.isNotEmpty()))
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

// ── 개별 통화·애드혹 그룹 통화(§4.1) ──────────────────────────────────────────

/**
 * 애드혹(ad hoc) 그룹 id — `adhoc-<내 PTT 번호>-<epoch초>`.
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
 * 개별 통화 발신 — PTT 사용자 1명과 1:1(TS 24.379 private call).
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
    // 곧바로 실패하면 [채널 추가] 패널이 그 자리에 적는다 — 토스트를 겹치지 않고 사유만 사전 문장으로 바꿔 준다.
    val area = if (emergency) TextArea.EMERGENCY else TextArea.PTT_PRIVATE
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, ResponseText.sip(area, r.code, r.reason))
}

/**
 * 애드혹 그룹 통화 개설 — PTT 사용자 N명(최소 1).
 *
 * 참가자는 `resource-lists` 로 싣는다. 서버가 그 목록으로 초대하며, 서버에 편성이 없는 그룹이라 목록을 **앱이
 * 기억한다** — 로스터 구독 대상이 아니라 카드에 몇 명인지 보이려면 여기밖에 없다.
 */
suspend fun DispatchSession.startAdhoc(members: List<String>,
                                       emergency: Boolean = false): CimsResult<Unit> {
    val r = joinAdhoc(members, GroupCallOptions(emergency = emergency),
                      if (emergency) Operation.EMERGENCY else Operation.PTT_ADHOC)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
}

/**
 * 애드혹 일제 통화(TS 24.379 §17.2.2.1.1 9) "broadcast adhoc group call") — 고른 사람들에게만 일제 통화. 채널 머리의
 * [일제 통화] 와 같은 한 버튼이라 개시 INVITE 가 암묵적 발언 요청이다. 서버가 ad hoc 의 broadcast-ind 를 받지 않으면 일반 애드혹
 * 그룹 통화로 열린다(`applyFloor` 가 ⑤ 에 알린다). 반환 = 개시한 호 id. 끝은 [releaseBroadcast].
 */
suspend fun DispatchSession.startAdhocBroadcast(members: List<String>): CimsResult<Int> {
    val r = joinAdhoc(members, GroupCallOptions(broadcast = true, implicitFloorRequest = true), Operation.PTT_ADHOC)
    if (r.ok) broadcastPending.add(r.value!!)
    return r
}

/** 애드혹 그룹 id 로 참가자 목록을 실어 연다 — 반환 = 호 id. */
private suspend fun DispatchSession.joinAdhoc(members: List<String>, opts: GroupCallOptions,
                                              op: Operation): CimsResult<Int> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val tels = members.map { telUri(userPart(it)) }.filter { it != "tel:" }.distinct()
    if (tels.isEmpty()) return CimsResult.fail(-1, "대상을 고르세요")
    val id = adhocIdOf(myPttId)
    val r = ptt.joinGroupCall(id, opts.copy(members = tels))
    // 곧바로 실패하면 [채널 추가] 패널이 그 자리에 적는다(개별 통화와 같다).
    if (!r.ok) return CimsResult.fail(r.code,
        ResponseText.sip(if (op == Operation.EMERGENCY) TextArea.EMERGENCY else TextArea.PTT_ADHOC, r.code, r.reason))
    val callId = r.value!!.id
    noteOperation(callId, op)
    rememberAdhocMembers(callId, tels)
    return CimsResult.ok(callId)
}
