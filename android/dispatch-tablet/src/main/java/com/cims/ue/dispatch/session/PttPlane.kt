// 관제 세션의 PTT 평면 — 그룹·floor·로스터·SDS (docs/design/features/android_dispatch_tablet.md §6.7)
//
// DispatchSession 의 확장으로 둔다 — 세션이 하나인 것은 유지하면서 파일은 평면별로 나눈다.
// 여기 있는 것도 전부 **코어 상태의 투영**이다. 판단(참여 자격·floor 정책)은 코어와 서버에 있다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.FloorEvent
import com.cims.ue.sdk.FloorEventKind
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.GroupCallOptions
import com.cims.ue.sdk.RosterUpdate
import com.cims.ue.sdk.SdsMessage
import kotlinx.coroutines.sync.withLock

/** PTT 그룹 목록 갱신 — GMS 목록(멤버) + 프로비저닝 `pttTargets`(청취 범위). */
suspend fun DispatchSession.refreshGroups(): CimsResult<Unit> {
    val ptt = pttAccount ?: return CimsResult.fail(-1, "PTT 계정 없음")
    val c = cscOrNull() ?: return CimsResult.fail(-1, "로그인 전")
    val token = accessToken() ?: return CimsResult.fail(-1, "로그인 전")

    val gen = loginGeneration.value
    val listed = c.listGroups(token, myPttId)
    if (!listed.ok) {
        // 조용히 넘기면 내 채널이 빈 채로 «멤버 그룹이 없습니다» 가 떠 서버 문제인지 편성 문제인지 가를 수 없다. 실패가 이어지는
        //   동안은 처음 한 번만 알린다.
        if (!groupListFailed) notify(NoticeLevel.WARN, "그룹 목록을 받지 못했습니다", ResponseText.of(TextArea.GROUP, listed.code, listed.reason))
        groupListFailed = true
        return CimsResult.fail(listed.code, listed.reason)
    }
    groupListFailed = false
    // 받는 사이 로그아웃 — 앞 사람의 그룹을 비운 목록에 다시 싣지 않는다. 실으면 다음 로그인이 그것을 «이미 구독한 그룹» 으로
    //   읽어 겹치는 그룹에 affiliation·conference 구독을 걸지 않는다(화면은 멀쩡한데 그룹콜이 오지 않는다).
    if (gen != loginGeneration.value) return CimsResult.fail(-1, "로그아웃됨")

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
    // 멤버였다가 GMS 목록에서 빠진 그룹 — 지워졌을 수 있다. 낡은 `pttTargets` 가 아직 들고 있어도 **이번 회차에는** 청취 범위로
    //   다시 넣지 않고 편성 재조회를 당긴다(정당하게 범위에 남는 그룹이면 재조회 뒤 다음 회차에 선다 — 데스크톱 `goneIds`).
    val dropped = groups.value.filter { it.isMember && !next.containsKey(it.id) }.map { it.id }.toSet()
    // ② 청취 범위 그룹 — 서버가 pttListen 범위를 해석한 목록. 참여하지 않고 로스터만 받는다.
    if (canListenPtt) dispatch.pttTargets.forEach { t ->
        if (t.id.isEmpty() || next.containsKey(t.id) || t.id in dropped) return@forEach
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
        forgetUpkeep(it.id)
    }
    // 서버 pttTargets(ptt_listen=all 은 전 그룹)가 지워진 그룹을 아직 들고 있을 수 있다 — 편성 재조회를 당겨, 삭제된 그룹을
    //   청취 범위로 다시 구독하는 창을 없앤다(정상 주기는 60초).
    if (hasDesk && gone.any { it.isMember }) scopeLaunch {
        refreshDispatch()
        // 편성이 그대로면(304) 재조회가 그룹을 다시 맞추지 않는다 — 빠진 그룹이 청취 범위에는 정당하게 남아 있을 수 있으니
        //   한 번 더 맞춘다(이번에는 «멤버였던 그룹» 이 아니라 범위 줄로 선다)
        if (dropped.isNotEmpty()) refreshGroups()
    }
    // 제휴는 서버의 2xx 까지 본다(`affiliateConfirmed`) — 서지 못한 것·응답이 없던 것은 유지 평면이 다시 싣는다(§6.7a). 응답이
    //   없으면(망이 없다) 남은 그룹은 기다리지 않는다 — 그룹마다 시한을 다 쓰면 목록이 그만큼 늦게 선다.
    var answered = true
    fresh.forEach { (id, isMember) ->
        if (gen != loginGeneration.value) return CimsResult.fail(-1, "로그아웃됨")    // 구독을 거는 사이 로그아웃 — 더 걸지 않는다
        // 범위 밖·자격 없음은 서버가 403 + Warning 138 로 거절한다 — conference 구독은 여기서 한 번, 갱신은 유지 평면이 한다.
        if (isMember && answered) answered = affiliateConfirmed(ptt, id) != 0
        ptt.subscribeConference(id, true)
    }
    requestVideoSync(groupsRefreshed = true)       // 영상 채널 표시·MCVideo affiliation·영상 호 합류를 다시 맞춘다(§6.14)
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
    // 세션 줄이 없다 = 아직 목록에 오르기 전(개시 직후)이거나 **이미 끝난 호**다. 끝난 호의 id 로 끊으면 pjsua 가 그 id 를 물려준
    //   다른 호(울리는 착신·영상 호)가 끊긴다 — 코어에 살아 있을 때만 끊는다.
    if (s == null && !isCallAlive(callId)) return
    if (s == null || !s.isActive) { noteLocalHangup(callId); ue.call(callId).hangup() } else ue.call(callId).floorRelease()
}

/** 그룹콜 참여 — ① 카드의 [참여]. 이미 세션이 있으면 코어가 그 호를 돌려준다. */
suspend fun DispatchSession.joinGroup(groupId: String, emergency: Boolean = false): CimsResult<Unit> {
    val area = if (emergency) TextArea.EMERGENCY else TextArea.PTT_JOIN
    val ptt = pttAccount ?: return report(area, CimsResult.fail(-1, "PTT 계정 없음"))
    // 정책 게이트(user profile ruleset, TS 24.484 §8.3.2.7) — UX 선차단, 최종 판정은 서버(403)
    if (emergency && !capabilities.value.emergencyGroupCall)
        return report(area, CimsResult.fail(-1, NO_EMERGENCY_GROUP_CALL))
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
    noteLocalHangup(callId)
    return report(TextArea.PTT_JOIN, ue.call(callId).leaveGroupCall())
}

/**
 * 진행 중 그룹콜의 긴급 상향·하향(TS 24.379 §10.1.1.2.1.3~5) — 채널 상세의 [긴급]·[긴급 해제]. 결과는 코어의 조건 이벤트로 온다
 * (확정 = 2xx, 거절 = 이전 값 복원 — 미인가 상향 403, 호는 유지 — `applyCondition`). 곧바로 실패한 것만 여기서 토스트로 남긴다.
 */
suspend fun DispatchSession.setEmergency(callId: Int, on: Boolean): CimsResult<Unit> {
    val area = if (on) TextArea.EMERGENCY else TextArea.EMERGENCY_CANCEL
    val ue = engineOrNull() ?: return report(area, CimsResult.fail(-1, "엔진 없음"))
    if (on && !capabilities.value.emergencyGroupCall) return report(area, CimsResult.fail(-1, NO_EMERGENCY_GROUP_CALL))
    if (on) sessionOf(callId)?.let { s ->
        // 상향은 성립한 통화에만, 앞선 변경이 끝난 뒤에(데스크톱 `EmergencyCall` 과 같은 선검사 — 서버에 보내 거절당하지 않는다)
        val name = channelNameOf(s)
        if (s.isEmergency) return report(area, CimsResult.fail(-1, "$name — 이미 긴급 상태입니다"))
        if (!s.isActive || s.info.condition.pending)
            return report(area, CimsResult.fail(-1, "$name — 통화가 성립한 뒤 긴급으로 올릴 수 있습니다"))
    }
    if (!on) conditionCancel.add(callId)                 // 해제 거절(403)은 상향 거절과 다른 문구 — applyCondition
    // 상향 요청 줄 — 명령보다 먼저 남긴다(확정 «긴급 개시» 가 그 뒤에 선다. 거절되면 요청 줄과 토스트가 남는다)
    else sessionOf(callId)?.let { addActivity(it.info.groupId, channelNameOf(it), "긴급 격상 요청", ActivityKind.EMERGENCY, emergency = true) }
    val r = ue.call(callId).setCondition(emergency = on)
    if (!r.ok && !on) conditionCancel.remove(callId)
    return report(area, r)
}

/**
 * PTT 누름 — 그 세션의 floor 를 요청한다.
 *
 * 요청(→ Requesting)·해제(→ Idle/Listening)는 코어가 **이벤트 없이** 상태만 바꾼다 — 명령 뒤에 스냅샷을 당겨 세션 투영을 맞춘다
 * (데스크톱 `SyncFloor`). 그러지 않으면 다음 floor 이벤트 전까지 «요청 중» 이 옛 값이라 발언 바 칩과 잠금 발언 판정이 어긋난다.
 */
suspend fun DispatchSession.floorRequest(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return report(TextArea.PTT_JOIN, CimsResult.fail(-1, "엔진 없음"))
    // D12 영상 우선 — 영상을 보내는 동안은 무전 발언을 요청하지 않는다(긴급·임박 채널 예외, 알림은 그쪽이 낸다 — §6.14)
    if (blocksTalkForVideo(callId)) return CimsResult.fail(-1, "영상 우선 — 영상을 보내는 중")
    return floorCmd.withLock { report(TextArea.PTT_JOIN, ue.call(callId).floorRequest()).also { pullFloor(callId) } }
}

/** PTT 뗌 — 대기 중이면 코어가 Queued Cancel 을 먼저 보낸다. */
suspend fun DispatchSession.floorRelease(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return floorCmd.withLock { ue.call(callId).floorRelease().also { pullFloor(callId) } }
}

/** 그룹 SDS 발신 — 최종 응답은 token 으로 `requestResult` 에서 맞춘다. */
suspend fun DispatchSession.sendGroupSds(groupId: String, text: String): CimsResult<Unit> {
    val ptt = pttAccount ?: return report(TextArea.SDS, CimsResult.fail(-1, "PTT 계정 없음"))
    val gen = loginGeneration.value
    val (r, early) = sendTracked({ it.token }) { ptt.sendGroupSds(groupId, text) }
    if (gen != loginGeneration.value) return CimsResult.fail(-1, "로그아웃됨")   // 비운 대화에 말풍선을 세우지 않는다
    if (!r.ok) {
        // 곧바로 실패해도 말풍선은 세운다(실패 + [재전송]) — 입력란은 이미 비었고, 토스트만 남기면 쓴 글이 사라진다
        addOutgoingMessage(groupId, text, "", 0L, failed = true)
        return report(TextArea.SDS, CimsResult.fail(r.code, r.reason))
    }
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
    val gen = loginGeneration.value
    val (r, early) = sendTracked({ it.token }) { ptt.sendSds(to, text) }
    if (gen != loginGeneration.value) return CimsResult.fail(-1, "로그아웃됨")
    if (!r.ok) {
        addOutgoingMessage(to, text, "", 0L, failed = true)
        return report(TextArea.SDS, CimsResult.fail(r.code, r.reason))
    }
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
    val now = System.currentTimeMillis()
    // 발언자를 **싣지 않는** 이벤트 — 코어 타이머의 «요청 시간 초과»(T101)와 앱 쪽 거부(수신 전용)는 talkers 없이 온다. 그것으로
    //   지금 말하는 사람을 지우지 않는다(데스크톱도 그대로 둔다) — 지우면 남의 발언이 일찍 끝난 것으로 ⑤ 에 적힌다.
    val keepSpeaker = ev.kind == FloorEventKind.REQUEST_TIMEOUT || (ev.kind == FloorEventKind.DENIED && ev.rawType < 0)
    val nextSpeaker = when {
        speaking -> "나"
        keepSpeaker -> s.speaker.takeIf { it != "나" }.orEmpty()
        taken -> speakerName
        else -> ""
    }
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
            speaker = nextSpeaker,
            // 발언 경과는 **그 발언**의 것이다 — 새 승인·발언자 교체는 거기서 다시 센다(남은 발언 게이지의 기준이기도 하다).
            speakerSinceMs = when {
                nextSpeaker.isEmpty() -> null
                ev.kind == FloorEventKind.GRANTED || nextSpeaker != it.speaker -> now
                else -> it.speakerSinceMs ?: now
            },
            grantedSec = when {
                ev.kind == FloorEventKind.GRANTED -> ev.durationSec.coerceAtLeast(0)
                speaking -> it.grantedSec
                else -> 0
            },
            talkLimit = speaking && ev.kind != FloorEventKind.GRANTED && (ev.kind == FloorEventKind.TALK_LIMIT || it.talkLimit),
            floorNote = floorNoteOf(ev))
    }

    // ⑤ 이벤트 — 진행 중 행은 없다. 발언이 **끝날 때 한 줄**(«박현장 발언 14초» — 데스크톱 `CloseTalk`)만 남긴다. 지금 누가
    //   말하는지는 카드가 보인다. 같은 발언 중에 오는 이벤트(시한 임박·대기열 위치)로 줄이 늘지 않는다.
    val gname = channelNameOf(s)
    if (nextSpeaker != s.speaker && s.speaker.isNotEmpty())
        addActivity(s.info.groupId, gname, talkEndText(s.speaker, s.speakerSinceMs, now), ActivityKind.TALK, s.isEmergency)
    floorFailureText(ev)?.let { addActivity(s.info.groupId, gname, it, ActivityKind.ERROR, s.isEmergency) }

    // 일제 통화로 연 호의 첫 서버 floor 메시지에 B-bit 가 없다 = 서버가 일반 통화로 열었다(mcptt_broadcast_group_call.md §3.2·R13).
    //   서버 메시지만 본다 — 코어 타이머 이벤트와 200 OK 의 승인 표시(`mc_granted`)는 rawType -1 이다.
    if (ev.rawType in BROADCAST_JUDGE_OPS && broadcastPending.remove(ev.callId) &&
        (ev.indicator and FLOOR_IND_BROADCAST) == 0) {
        val why = if (s.kind == SessionKind.PTT_ADHOC) "서버가 애드혹 일제 통화를 받지 않아 일반 애드혹 그룹 통화로 이어집니다"
                  else "진행 중 통화에 합류했거나 편성 그룹이 아닙니다(일반 그룹 통화로 이어집니다)"
        addActivity(s.info.groupId, gname, "일제 통화로 열리지 않았습니다 — $why", ActivityKind.ERROR, s.isEmergency)
        // 말하는 사람은 «채널» 면을 보고 있다 — 나만 말하는 통화가 아니라는 것을 곧바로 알린다(데스크톱 경고 토스트)
        notify(NoticeLevel.WARN, "$gname — 일제 통화로 열리지 않았습니다", why)
    }

    pullFloor(ev.callId)          // 권위 있는 스냅샷으로 덮는다
}

/** 긴급 그룹콜 자격이 없을 때의 문장 — 긴급 참여·격상·긴급 애드혹이 같은 말을 쓴다(데스크톱 `EmergencyCall`). */
private const val NO_EMERGENCY_GROUP_CALL = "긴급 그룹콜 자격이 없습니다 (user profile allow-emergency-group-call)"

/** 일제 통화 판정에 쓰는 서버 floor 메시지(TS 24.380 §8.2.2) — Granted·Taken·Deny·Idle·Revoke. 서버는 여기에 Floor Indicator 를 싣는다. */
private val BROADCAST_JUDGE_OPS = setOf(0x01, 0x02, 0x03, 0x05, 0x06)

/** 로스터 NOTIFY → 그룹 참가자·진행 여부. 참여하지 않은 청취 범위 그룹도 이걸로 안다. */
internal fun DispatchSession.applyRoster(u: RosterUpdate) {
    val before = groups.value.firstOrNull { it.id == u.groupId } ?: return
    val next = mergeRoster(before.roster, u.users, u.full)
    updateGroup(u.groupId) { it.withRoster(next).copy(rosterSeen = true) }
    val after = groups.value.firstOrNull { it.id == u.groupId } ?: return
    if (!before.hasSession && after.hasSession)
        addActivity(after.id, after.name, "세션 시작 · 참가 ${after.connectedCount}", ActivityKind.JOIN)
    else if (before.hasSession && !after.hasSession)
        addActivity(after.id, after.name, "세션 종료", ActivityKind.LEAVE)
    // 내 채널의 입퇴장 — 구독 직후의 첫 스냅샷은 건너뛴다(이미 있던 참가자를 «합류» 로 적지 않는다). 세션이 통째로 서거나
    //   끝나는 순간은 위의 한 줄이 말하므로 사람마다 적지 않는다. 나 자신은 적지 않는다(내 참여·이탈은 세션 줄이 남는다).
    if (!before.isMember || !before.rosterSeen || !before.hasSession || !after.hasSession) return
    val mine = myLineKeys()
    fun me(uri: String) = DirectoryBook.normalize(userPart(uri)) in mine
    val (joined, left) = rosterMoves(before.roster, after.roster)
    joined.filterNot(::me).forEach { addActivity(after.id, after.name, "${displayName(it)} 합류", ActivityKind.JOIN) }
    left.filterNot(::me).forEach { addActivity(after.id, after.name, "${displayName(it)} 이탈", ActivityKind.LEAVE) }
}

/** SDS 수신 → ④ 스레드 + ⑤ 줄. 전달 확인을 요청했으면 DELIVERED 를 되돌린다([deliveryReplyTo]). */
internal fun DispatchSession.applySds(msg: SdsMessage) {
    if (msg.notification) return updateSendState(msg.msgId, msg.notifType)
    val groupId = userPart(msg.groupUri).ifEmpty { userPart(msg.fromUri) }
    // 이미 받은 메시지(재전송·중복 배달)면 ⑤ 줄도 또 남기지 않는다 — 전달 확인만 다시 돌려준다(상대는 그것을 못 받아 다시 보냈다).
    if (addIncomingMessage(groupId, msg))
        addActivity(groupId, groupNameOf(groupId), sdsEventText(displayName(msg.fromUri), msg.text, msg.fileName), ActivityKind.SDS)
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

/** ⑤ 의 SDS 한 줄 — «메시지 · 보낸 사람 — 본문 앞머리». 파일(FD)이면 본문 자리에 파일 이름(데스크톱 `McDataMessagesViewModel.OnSds`). */
internal fun sdsEventText(from: String, text: String, fileName: String = ""): String {
    val body = text.ifBlank { fileName }.split(Regex("\\s+")).joinToString(" ").trim()
    val head = if (body.length > SDS_EVENT_BODY_MAX) body.take(SDS_EVENT_BODY_MAX - 1) + "…" else body
    return "메시지 · $from" + if (head.isEmpty()) "" else " — $head"
}

private const val SDS_EVENT_BODY_MAX = 40

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
    if (takeVideoCall(c)) return             // MCVideo 호 — 영상 평면이 받는다(통화 내역·⑤ 세션 줄·자동 보류에 들지 않는다, §6.14)
    if (c.state == CallState.DISCONNECTED) {
        // 내가 거둔 호인가 — 연결 전에 끊으면 CANCEL → 487 로 끝나는데, 그것은 실패가 아니다(토스트도 «세션 실패» 도 아니다).
        val mine = takeLocalHangup(c.callId)
        sessionOf(c.callId)?.let { s ->
            // 403 으로 거절된 [참여] — 제휴를 다시 싣고 한 번 더 건다(§6.7a). 그 끝은 실패가 아니다: 다시 건 호의 결과가 알린다.
            if (!mine && rejoinsAfterAffiliation(s, c)) { removeSession(c.callId); return }
            if (!mine) noteFailedAttempt(s, c)
            if (s.kind.isPttCard || s.kind == SessionKind.PTT_LISTEN)
                addActivity(s.info.groupId, channelNameOf(s),
                    sessionEndText(s.kind, s.isBroadcast || s.info.mcptt.broadcast,
                        s.connectedAtMs?.let { System.currentTimeMillis() - it }, s.adhocMembers.size, c.lastCode, mine,
                        incoming = s.info.dir == com.cims.ue.sdk.CallDir.INCOMING),
                    ActivityKind.LEAVE, s.isEmergency)
            else noteMyCall(s, c, mine)
        }
        removeSession(c.callId)
        return
    }
    // 이 호가 **처음** 성립하는가 — 자동 보류는 새 통화가 붙을 때만 건다. 이미 성립한 호가 다시 Active 가 되는 것(상대가 보류를 풀어 줌)에
    //   걸면 내가 말하던 다른 통화가 보류된다. 내가 보류를 푸는 것은 `hold(callId, false)` 가 직접 다른 통화를 보류한다(데스크톱 `Resume`).
    val firstActive = c.state == CallState.ACTIVE && sessionOf(c.callId)?.connectedAtMs == null
    upsertSession(c)
    noteAnswerState(c.callId)                // «멤버 확인 전 연결»(P-Answer-State: Unconfirmed) — 한 호에 한 번
    // 코어는 200 OK 의 `P-Answer-State` 를 **성립 이벤트를 낸 뒤에** 적는다(이 스냅샷에는 비어 있다). 내가 연 MCPTT 호가 막
    //   성립했으면 잠시 뒤 호 정보를 다시 물어 한 번 더 본다 — 뒤따르는 미디어 이벤트에만 기대면, 18x 에서 이미 협상이 끝난
    //   호(미디어 이벤트가 없다)나 그 이벤트가 먼저 접힌 호에서는 끝내 적히지 않는다.
    if (c.state == CallState.ACTIVE && c.isMcptt && c.dir == com.cims.ue.sdk.CallDir.OUTGOING && c.answerState.isEmpty()) {
        val callId = c.callId
        scopeLaunch {
            kotlinx.coroutines.delay(ANSWER_STATE_RECHECK_MS)
            val now = engineOrNull()?.callInfo(callId) ?: return@scopeLaunch
            if (now.state != CallState.ACTIVE || now.answerState.isEmpty() || now.remoteUri != c.remoteUri) return@scopeLaunch
            if (sessionOf(callId) == null) return@scopeLaunch
            updateSession(callId) { it.copy(info = now) }
            noteAnswerState(callId)
        }
    }

    // 새 통화가 붙으면 **기존 활성 통화를 보류한다**(데스크톱 `DispatchSession.cs` 의 AutoHoldOnAnswer).
    // 그러지 않으면 두 통화가 동시에 들려 관제사가 어느 쪽에 말하는지 알 수 없다.
    if (firstActive && SessionKind.of(c) == SessionKind.PHONE_CALL &&
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
internal fun DispatchSession.holdOtherCalls(exceptCallId: Int) {
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
private fun DispatchSession.noteMyCall(s: SessionItem, c: CallInfo, mine: Boolean = false) {
    val answered = s.connectedAtMs != null
    val outgoing = c.dir == com.cims.ue.sdk.CallDir.OUTGOING
    // 대표번호 포크의 **내 leg** 이 응답 없이 끝났다 — 동료가 받아 CANCEL 된 것일 수 있어 여기서는 부재로 세지 않는다. 전원 무응답
    //   부재는 대표번호 dialog 가 confirmed 없이 끝날 때 한 줄만 남긴다(`noteDialogEnd` — 데스크톱과 같다).
    if (!outgoing && !answered && s.kind == SessionKind.PHONE_CALL && isPilot(c.calledParty)) return
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
        kind == CallLogKind.MISSED && mine -> "부재 · 거절"        // 내가 거절했다 — 받지 않은 호라 부재로 세되 놓친 것과 구분한다
        kind == CallLogKind.MISSED && c.lastCode >= 300 -> "부재 ${c.lastCode}"
        kind == CallLogKind.MISSED -> "부재"
        answered -> "통화 종료"
        mine -> "취소"                           // 내가 걸고 받기 전에 끊었다
        c.lastCode >= 300 -> "실패 ${c.lastCode}"   // 내가 걸었는데 연결되지 못했다(데스크톱 «실패 486»)
        else -> "미응답"
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
        viaPilot = isPilot(c.calledParty)))
}

/** Denied/Revoked 사유를 화면 문구로 — 사전은 dispatch_desktop_ui.md §9 가 정본이다. */
internal fun floorNoteOf(ev: FloorEvent): String = when (ev.kind) {
    FloorEventKind.DENIED -> ev.causeText.ifEmpty { "요청 거부" }
    FloorEventKind.REVOKED -> ev.causeText.ifEmpty { "발언권 회수" }
    FloorEventKind.REQUEST_TIMEOUT -> "요청 시간 초과"
    FloorEventKind.QUEUE_POSITION -> queueText(ev.queuePosition)
    FloorEventKind.QUEUE_CANCELLED, FloorEventKind.GRANTED -> ""
    else -> when {
        ev.causeText.isNotEmpty() -> ev.causeText
        ev.state == FloorState.QUEUED -> queueText(ev.queuePosition)
        else -> ""
    }
}

/**
 * 대기열 문구 — Queue Position Info(TS 24.380 §8.2.3.9)의 위치는 **1 부터**다(서버 `PMcpttGroup::_queuePositionOf`). 254(대기 아님)·
 * 255(알 수 없음)·그 밖은 위치를 적지 않는다.
 */
internal fun queueText(pos: Int): String = if (pos in 1..253) "대기 ${pos}번째" else "대기열"

/** 끝난 발언의 ⑤ 한 줄 — «박현장 발언 14초»(데스크톱 `CloseTalk`). 시작 시각을 모르면 길이를 적지 않는다. */
internal fun talkEndText(speaker: String, sinceMs: Long?, nowMs: Long): String {
    val sec = sinceMs?.let { ((nowMs - it + 500) / 1000).coerceAtLeast(0) }
    return "$speaker 발언" + if (sec != null) " ${sec}초" else ""
}

/**
 * 새 세션의 ⑤ «시작» 줄(데스크톱 `Create`) — 개별 통화·애드혹·청취·일제 통화. 편성 그룹의 보통 호는 로스터가 «세션 시작» 으로
 * 남기므로 여기서 적지 않는다(null).
 */
internal fun sessionStartText(kind: SessionKind, incoming: Boolean, broadcast: Boolean, adhocCount: Int, initiator: String): String? = when {
    kind == SessionKind.PTT_PRIVATE -> "개별 통화 · " + if (incoming) "착신" else "발신"
    kind == SessionKind.PTT_LISTEN -> "청취 시작"
    broadcast && (kind == SessionKind.PTT_CHANNEL || kind == SessionKind.PTT_ADHOC) ->
        if (incoming) "일제 통화" + (if (initiator.isNotBlank()) " · $initiator" else "") else "일제 통화 개시"
    kind == SessionKind.PTT_ADHOC -> "애드혹 그룹" + (if (adhocCount > 0) " · ${adhocCount}명" else if (incoming) " · 착신" else "")
    else -> null
}

/**
 * 끝난 세션의 ⑤ 한 줄(데스크톱 `Remove`) — 종류마다 낱말이 다르고 길이가 붙는다. 내 청취만 끊어도 «세션 종료» 로 적으면
 * 그 그룹의 세션이 끝난 것으로 읽힌다. 성립하지 못하고 끝난 것은 길이 대신 «실패 <코드>»(내가 거둔 것은 실패가 아니다).
 */
internal fun sessionEndText(kind: SessionKind, broadcast: Boolean, connectedMs: Long?, adhocCount: Int, lastCode: Int, mine: Boolean,
                            incoming: Boolean = false): String {
    val dur = connectedMs?.let { ms -> (ms / 1000).let { " · %d:%02d".format(it / 60, it % 60) } }.orEmpty()
    // 실패는 **내가 건** 호가 성립하지 못한 것이다 — 상대가 응답 전에 거둔 착신(487)·내가 거절한 착신(486)은 실패가 아니다
    val failed = connectedMs == null && lastCode >= 300 && !mine && !incoming
    return when {
        kind == SessionKind.PTT_LISTEN -> "청취 종료" + if (failed) " · 실패 $lastCode" else dur
        kind == SessionKind.PTT_PRIVATE -> "개별 통화 종료" + if (failed) " · 실패 $lastCode" else dur
        broadcast -> "일제 통화 종료" + if (failed) " · 실패 $lastCode" else dur
        kind == SessionKind.PTT_ADHOC -> "애드혹 종료" + (if (failed) " · 실패 $lastCode" else dur) + if (adhocCount > 0) " · 참가 $adhocCount" else ""
        failed -> "세션 실패 $lastCode"
        else -> "세션 종료$dur"
    }
}

/** 새 세션이 목록에 오를 때 — ⑤ 시작 줄. */
internal fun DispatchSession.noteSessionStart(s: SessionItem) {
    val ci = s.info
    if (!ci.isMcptt) return
    val incoming = ci.dir == com.cims.ue.sdk.CallDir.INCOMING
    val text = sessionStartText(s.kind, incoming, ci.mcptt.broadcast, adhocMembersOf(s.callId).size,
        if (ci.mcptt.callingUserId.isBlank()) "" else displayName(ci.mcptt.callingUserId)) ?: return
    addActivity(ci.groupId, channelNameOf(s), text, ActivityKind.JOIN, s.isEmergency)
}

/**
 * 발언 요청이 안 된 까닭의 ⑤ 한 줄 — 거부·회수·시간 초과(데스크톱 `OnFloor` 와 같은 낱말). 그 밖의 이벤트가 사유를 실어 왔으면
 * 그대로 적는다. 승인·정상 종료에는 줄이 없다.
 */
internal fun floorFailureText(ev: FloorEvent): String? {
    fun with(head: String) = if (ev.causeText.isEmpty()) head else "$head · ${ev.causeText}"
    return when (ev.kind) {
        FloorEventKind.DENIED -> with("발언 요청 거부")
        FloorEventKind.REVOKED -> with("발언권 회수")
        FloorEventKind.REQUEST_TIMEOUT -> "발언 요청 시간 초과"
        // 승인된 발언 시간(Granted Duration)이 다해 코어가 스스로 놓았다(TS 24.380 T2) — 왜 발언이 끊겼는지 ⑤ 에 남긴다. 카드의
        //   사유 줄에는 적지 않는다(뒤따르는 Floor Idle 이 곧 지우고, 그사이 발언 칩이 «거부» 로 읽힌다).
        FloorEventKind.TALK_LIMIT -> "발언 시간 초과 — 발언이 끝났습니다"
        else -> ev.causeText.takeIf { it.isNotEmpty() && ev.state != FloorState.SPEAKING }
    }
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
    // 클라이언트가 지은 id 가 남의 그룹과 겹쳤을 때만(`uri_taken`, mcptt_api.md §2) — 다른 409 를 새 id 로 되풀이하지 않는다.
    if (!r.ok && isNew && r.code == 409 && r.reason.contains("uri_taken"))
        r = c.putGroup(t, myPttId, doc.copy(uri = newGroupUri()), "")
    if (r.ok) {
        val d = r.value!!
        addActivity(userPart(d.uri), d.displayName,
            "그룹 ${if (isNew) "생성" else "편집"} · 멤버 ${d.members.size}", ActivityKind.NOTE)
    }
    return r
}

/** 그룹 삭제 — 관리 범위 안이거나 본인 소유만(서버 판정, 403). */
suspend fun DispatchSession.deleteGroup(groupUri: String): CimsResult<Unit> {
    val c = cscOrNull() ?: return CimsResult.fail(-1, "로그인 전")
    val t = accessToken() ?: return CimsResult.fail(-1, "로그인 전")
    val r = c.deleteGroup(t, myPttId, groupUri)
    if (r.ok) addActivity(userPart(groupUri), groupNameOf(userPart(groupUri)), "그룹 삭제", ActivityKind.NOTE)
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
    // 정책 게이트(user profile ruleset, TS 24.484 §8.3.2.7) — UX 선차단, 최종 판정은 서버(403)
    val caps = capabilities.value
    if (!caps.privateCall) return CimsResult.fail(-1, "개별 통화 자격이 없습니다 (user profile allow-private-call)")
    if (emergency && !caps.emergencyPrivateCall)
        return CimsResult.fail(-1, "긴급 개별 통화 자격이 없습니다 (user profile allow-emergency-private-call)")
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
    val caps = capabilities.value
    if (!caps.adhocGroupCall) return CimsResult.fail(-1, "애드혹 그룹 통화 자격이 없습니다 (user profile allow-adhoc-group-call)")
    if (opts.emergency && !caps.emergencyGroupCall) return CimsResult.fail(-1, NO_EMERGENCY_GROUP_CALL)
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

/** 성립 뒤 `P-Answer-State` 를 다시 보는 지연 — 코어가 200 OK 의 헤더를 적을 시간(같은 pjsip 스레드의 다음 콜백). */
private const val ANSWER_STATE_RECHECK_MS = 300L
