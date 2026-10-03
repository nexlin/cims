package com.cims.ue.ptt

import android.os.SystemClock
import android.util.Log
import com.cims.ue.core.sip.ImMessage
import com.cims.ue.core.sip.RegState
import com.cims.ue.ptt.PttController.Companion.SUB_CONFIRM_TIMEOUT_MS
import com.cims.ue.ptt.PttController.Companion.REJOIN_GAP_MS
import com.cims.ue.ptt.PttController.Companion.SUB_REASSERT_MS
import com.cims.ue.ptt.PttController.Companion.TAG
import com.cims.ue.ptt.PttController.Companion.XCAP_CMS
import com.cims.ue.ptt.PttController.Companion.XCAP_GMS
import com.cims.ue.ptt.PttController.Companion.bareId
import com.cims.ue.ptt.PttController.Companion.isAdhocId
import com.cims.ue.ptt.csc.GroupDoc
import com.cims.ue.ptt.csc.GroupSummary
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CscClient
import com.cims.ue.sdk.GroupCallOptions
import com.cims.ue.sdk.RequestResult
import com.cims.ue.sdk.RosterUpdate
import com.cims.ue.sdk.SipMessage
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * 그룹 평면 — 참여·이탈·착신 자동 참여, affiliation 목표 집합, 로스터·문서(xcap-diff) 구독, CSC 조회(GMS·CMS).
 * 규격 절차(INVITE 조립·PUBLISH ETag·412·구독 갱신·문서 해석)는 코어가 하고, 여기는 **무엇을 언제 다시 할지**만 정한다.
 */
internal class GroupPlane(private val c: PttController) {

    // ── 참여/이탈 ──

    /** [takePrimary] = 주채널 세션이 없을 때 이 그룹을 주채널로 삼는다(사용자 참여). 채널 복원은 고른 주채널에만 준다 — 다른 그룹 복원이
     *  고른 주채널(영상 채널, [VideoPlane])을 덮지 않게. [implicitFloor] = 개시 INVITE 에 암묵적 발언 요청(TS 24.380 §14.2.4 —
     *  PTT 를 누른 채 호를 여는 경우, 성립과 함께 발언권). */
    fun joinGroupCall(groupId: String, members: List<String> = emptyList(), emergency: Boolean = false,
                      broadcast: Boolean = false, takePrimary: Boolean = true, implicitFloor: Boolean = false) {
        // 사전 구성 전용 그룹 — 호를 열지 않고 알린다(TS 24.379 §10.1.1.2.1.1·§10.1.2.2.1.1)
        if (!CallRules.groupUsable(c._groupDocs.value[groupId]?.preconfiguredOnly)) {
            c._status.value = "통화할 수 없는 그룹 $groupId"
            c.feedback?.blocked("이 그룹으로는 통화할 수 없습니다")
            return
        }
        val s = synchronized(c.lock) {
            if (c.sessionMap.containsKey(groupId)) return
            c.Session(groupId).also {
                it.role = if (takePrimary && c.sessionMap.values.none { v -> v.role == ChannelRole.PRIMARY }) ChannelRole.PRIMARY
                else ChannelRole.NONE
                it.emergency = emergency
                it.emergencyMine = emergency
                it.broadcast = broadcast
                if (implicitFloor) it.floorState = FloorState.REQUESTING
                c.sessionMap[groupId] = it
            }
        }
        // 애드혹 임시 그룹은 편성 자산이 아니다 — affiliation(사전 가입 없음)·채널 영속·
        //   로스터 구독(참가자는 in-dialog NOTIFY 로 수신)을 모두 건너뛴다.
        val adhoc = isAdhocId(groupId)
        if (!adhoc) {
            ensureAffiliated(groupId)
            c.channelStore?.let { st -> st.add(groupId); if (s.role == ChannelRole.PRIMARY) st.primary = groupId }
            if (s.role == ChannelRole.PRIMARY) c._selectedGroup.value = groupId
        }
        c.ctl.launch {
            val acc = c.account
            // 음성만 — 그룹 영상은 MCVideo 호다([VideoPlane], mcvideo.md §7 D9)
            val r = acc?.joinGroupCall(groupId, GroupCallOptions(emergency = emergency, broadcast = broadcast, members = members,
                implicitFloorRequest = implicitFloor))
            if (r != null && r.ok) { c.bindCall(groupId, r.value!!.id); return@launch }
            Log.w(TAG, "joinGroupCall $groupId 실패: ${r?.code} ${r?.reason ?: "not registered"}")
            synchronized(c.lock) { if (c.sessionMap[groupId] === s) c.sessionMap.remove(groupId) }
            c._status.value = "그룹콜 발신 실패 $groupId"
            c.publish()
        }
        if (!adhoc) subscribeRoster(groupId, true)
        c._status.value = when {
            emergency -> "🚨 긴급 그룹콜 개시 $groupId"
            broadcast -> "일제 통화 개시 $groupId"
            else -> "그룹콜 참여 $groupId"
        }
        c.emit(PttEventKind.JOIN, groupId)
        if (emergency) c.emit(PttEventKind.EMERGENCY, groupId)
        c.publish()
    }

    /** 애드혹 그룹통화 발신 (TS 22.179 Rel-18) — 편성 없이 즉석 멤버 지정. 임시 그룹 ID 로
     *  INVITE 에 resource-lists 멤버 명단을 실어 보내면 서버(CSP)가 비영속 임시 그룹을 합성해
     *  전원 fan-out 한다. 그룹은 통화 종료와 함께 소멸(양쪽 모두 ephemeral). */
    fun startAdhocCall(members: List<String>) {
        val me = bareId(c.mcpttId)
        val peers = members.map { bareId(it) }.filter { it.isNotBlank() && it != me }.distinct()
        if (peers.isEmpty()) { c._status.value = "애드혹: 대상 없음"; return }
        // 사용자 단위 ad hoc 개시 인가 (프로파일) — 서버(403)가 최종 판정이나 UX 를 위해 선차단.
        if (userProfile.value?.allowAdhocCall == false) {
            c._status.value = "애드혹: 개시 권한 없음"
            c.feedback?.blocked("애드혹 개시 권한이 없습니다")
            return
        }
        // 임시 ID = adhoc-<발신자>-<epoch초> — 가입자 번호(숫자)·편성 그룹(접두사 예약)과 충돌 불가.
        val gid = "adhoc-${me.trimStart('+')}-${System.currentTimeMillis() / 1000}"
        joinGroupCall(gid, members = peers.map { "tel:$it" })
        c._status.value = "애드혹 그룹통화 개시 — ${peers.size}명"
    }

    /** 1:1 private call 발신 (TS 24.379 §11.1 on-demand — mcptt-info session-type=private).
     *  사전 그룹편성·affiliation 불요(서버가 멤버십 게이트 우회). 세션 키=[peer](상대 번호).
     *  [fullDuplex]=true 면 mc_no_floor_ctrl 을 협상해 floor 없는 전이중으로 연다 — PTT 는 로컬 마이크 게이트(setMuted)다. */
    fun startPrivateCall(peer: String, fullDuplex: Boolean = false, emergency: Boolean = false) {
        val target = bareId(peer)
        if (target.isBlank() || target == bareId(c.mcpttId)) return
        // 사용자 인가 게이트 (TS 24.484 §8.3.2.7 allow-private-call) — 서버(403)가 최종 판정이나, 발신 전에
        //   알리는 편이 정확하다. 착신은 막지 않는다(서버가 이미 성립시킨 세션은 받는다).
        if (userProfile.value?.allowPrivateCall == false) {
            c._status.value = "1:1 통화: 권한 없음"
            c.feedback?.blocked("1:1 통화가 허용되지 않습니다")
            return
        }
        // 긴급 1:1 개시 인가 (allow-emergency-private-call) — 프로파일 명시 미인가만 선차단,
        //   미수신(null)은 낙관 발신(서버 403 이 최종 판정 → handleEmergencyDenied 폴백).
        if (emergency && userProfile.value?.allowEmergencyPrivateCall == false) {
            c._status.value = "긴급 1:1: 개시 권한 없음"
            c.feedback?.blocked("긴급 1:1 개시 권한이 없습니다")
            return
        }
        val s = synchronized(c.lock) {
            if (c.sessionMap.containsKey(target)) return            // 이미 그 상대와 세션 중
            c.Session(target).also {
                it.privatePeer = true
                it.fullDuplex = fullDuplex
                if (emergency) { it.emergency = true; it.emergencyMine = true }
                // 1:1 은 주채널을 점유하지 않는다 — 활성 1:1 이 있는 동안 PTT 키가 1:1 에
                // 우선하는 규칙(talkSession)으로 발언을 라우팅하고, 끝나면 주채널로 복귀한다.
                it.role = ChannelRole.NONE
                c.sessionMap[target] = it
            }
        }
        // 편성 채널이 아니므로 channelStore/affiliation/roster 구독 없음 — 즉석 세션.
        c.ctl.launch {
            val acc = c.account
            val r = acc?.startPrivateCall(target, GroupCallOptions(fullDuplex = fullDuplex, emergency = emergency))
            if (r != null && r.ok) {
                val id = r.value!!.id
                c.bindCall(target, id)
                // 전이중도 오디오는 반이중 규칙 — 마이크는 PTT 로만 연다(코어: 전이중이면 setMuted 가 로컬 게이트)
                if (fullDuplex) c.ue.call(id).setMuted(true)
                return@launch
            }
            Log.w(TAG, "startPrivateCall $target 실패: ${r?.code} ${r?.reason ?: "not registered"}")
            synchronized(c.lock) { if (c.sessionMap[target] === s) c.sessionMap.remove(target) }
            c._status.value = "1:1 발신 실패 $target"
            c.publish()
        }
        c._status.value = when {
            emergency -> "🚨 긴급 1:1 발신 $target"
            fullDuplex -> "1:1 통화 발신 $target"
            else -> "1:1 무전 발신 $target"
        }
        if (emergency) {
            c.feedback?.emergencyTone()
            c.emit(PttEventKind.EMERGENCY, target)
        }
        c.emit(PttEventKind.JOIN, target)
        c.publish()
    }

    /** MCPTT 착신 — 코어가 이미 200 까지 보냈다(autoAnswerMcptt, ptt_ue.md §12.3). 세션만 세운다. */
    fun onIncomingCall(ci: CallInfo) {
        if (!ci.isMcptt) { Log.i(TAG, "MCPTT 가 아닌 착신 무시 call=${ci.callId} ${ci.remoteUri}"); return }
        if (ci.mcptt.privateCall) autoJoinPrivateCall(ci) else autoJoinGroupCall(ci)
    }

    /** 1:1 private call 착신 — 세션 키=발신자 번호(mcptt-calling-user-id). 전이중이면 PTT 게이트로 마이크를 닫아 둔다. */
    private fun autoJoinPrivateCall(ci: CallInfo) {
        val peer = bareId(ci.groupId)
        if (peer.isBlank()) return
        val stale = synchronized(c.lock) {
            val old = c.sessionMap[peer]
            when {
                old == null -> null
                old.callId == ci.callId -> return                  // 같은 호 재통지
                else -> c.sessionMap.remove(peer)?.also { it.close() }   // 상대가 새 호를 걸었다 = 이전 1:1 은 끝났다
            }
        }
        stale?.let { if (it.callId >= 0) c.cmd("hangup stale 1:1") { c.ue.call(it.callId).hangup() } }
        val s = synchronized(c.lock) {
            if (c.sessionMap.containsKey(peer)) return             // 경합 재확인
            c.Session(peer).also {
                it.callId = ci.callId
                it.privatePeer = true
                it.fullDuplex = ci.mcptt.noFloorCtrl
                it.emergency = ci.condition.emergency || ci.mcptt.emergency
                it.role = ChannelRole.NONE                          // 1:1 은 주채널 비점유
                c.sessionMap[peer] = it
            }
        }
        if (s.fullDuplex) c.cmd("setMuted(1:1)") { c.ue.call(ci.callId).setMuted(true) }
        c._status.value = if (s.fullDuplex) "1:1 통화 수신: $peer" else "1:1 무전 수신: $peer"
        c.emit(PttEventKind.JOIN, peer)
        c.publish()
    }

    /** 그룹콜 착신 — 미참여 그룹이면 세션 생성. fan-out INVITE 의 emergency-ind → 긴급 표시 + 경고 톤. */
    private fun autoJoinGroupCall(ci: CallInfo) {
        val groupId = bareId(ci.groupId)
        if (groupId.isBlank()) return
        // 이미 세션이 있는데 **다른 callId** 의 새 leg 이 오면 = 옛 leg 이 대체된 것(재설치 후 서버가 세션 타이머로 살려 둔
        //   다이얼로그를 새 앱 인스턴스가 착신으로 받는 경합). 옛 호를 끊고 새 callId 로 세션을 재동기한다.
        val existing = synchronized(c.lock) { c.sessionMap[groupId] }
        if (existing != null) {
            if (existing.callId == ci.callId) return
            val old = existing.callId
            if (old >= 0) c.cmd("hangup replaced leg") { c.ue.call(old).hangup() }
            synchronized(c.lock) { existing.callId = ci.callId }
            c.publish()
            return
        }
        val emergency = ci.condition.emergency || ci.mcptt.emergency
        // 주채널(선택 그룹)은 사용자가 고른다 — 고른 주채널이 있으면 다른 그룹의 팬아웃 착신은 듣기만 하고 주채널 자리를 차지하지
        //   않는다(TS 22.179 그룹 스캐닝: 여러 그룹을 받되 선택 그룹은 그대로). 고른 주채널이 없을 때만 받은 그룹을 주채널로 삼는다.
        val chosen = c.channelStore?.primary
        val s = synchronized(c.lock) {
            if (c.sessionMap.containsKey(groupId)) return          // 경합 재확인
            c.Session(groupId).also {
                it.callId = ci.callId
                it.role = if (c.sessionMap.values.none { v -> v.role == ChannelRole.PRIMARY } && (chosen == null || chosen == groupId))
                    ChannelRole.PRIMARY
                else ChannelRole.NONE
                it.emergency = emergency
                c.sessionMap[groupId] = it
            }
        }
        // 애드혹 임시 그룹 수신 — 편성 채널이 아니므로 영속/구독 제외 (발신측 joinGroupCall 과 대칭)
        if (!isAdhocId(groupId)) {
            subscribeRoster(groupId, true)
            c.channelStore?.let { st -> st.add(groupId); if (s.role == ChannelRole.PRIMARY) st.primary = groupId }
            if (s.role == ChannelRole.PRIMARY) c._selectedGroup.value = groupId
        }
        if (emergency) c.feedback?.emergencyTone()
        c._status.value = if (emergency) "🚨 긴급 그룹콜 자동 참여: $groupId" else "그룹콜 자동 참여: $groupId"
        c.emit(PttEventKind.JOIN, groupId)
        if (emergency) c.emit(PttEventKind.EMERGENCY_IN, groupId)
        c.publish()
    }

    /** 그룹별 나가기. */
    fun leaveGroup(groupId: String) {
        c.channelStore?.remove(groupId)                 // 명시적 이탈 = 재조인 의도 해제
        // orphan leg 회수 — 앱 세션이 추적하는 callId 와 무관하게, 그 그룹으로 살아 있는 호를 모두 끊는다(재설치 후 서버가
        //   세션 타이머로 살려 둔 옛 leg 을 엔진이 자동 수락해 유지하는 경우). 제휴는 유지 — 새 그룹콜이 오면 다시 초대받는다.
        val tracked = synchronized(c.lock) { c.sessionMap[groupId]?.callId ?: -1 }
        c.ctl.launch {
            c.ue.calls().filter { id -> id == tracked || c.ue.callInfo(id)?.let { it.isMcptt && bareId(it.groupId) == groupId } == true }
                .forEach { id -> c.ue.call(id).hangup() }
        }
        if (tracked < 0) {
            synchronized(c.lock) {
                c.sessionMap.remove(groupId)?.close()
                // 미참여 채널의 접속 인원은 "나 외의" 참여자 — 나가는 즉시 본인을 지워 stale(1) 박제 방지.
                c.rosterMap[groupId]?.let { m ->
                    c.rosterMap[groupId] = m.toMutableMap().apply { remove(bareId(c.mcpttId)) }.toMap()
                }
            }
            publishRosters()
            invalidateRosterConfirm(groupId)
            syncRosterSubs()   // 편성 채널이면 구독 유지 (인원수 표시 계속)
            c.publish()
        }
        // 호가 있으면 끊긴 뒤 onCallEnded 가 세션/로스터를 정리한다
    }

    /** 세션이 끝난 그룹 — 로스터에서 본인을 지우고 구독을 재확인한다. */
    fun onSessionLeft(gid: String) {
        // 나가는 순간의 마지막 이탈 NOTIFY 는 통화 다이얼로그 teardown 과 겹쳐 앱까지 못 오는 경우가 있어(특히 마지막
        //   이탈자 — 08-11 W999 실측), 그 NOTIFY 에 의존하면 본인이 로스터에 남아 접속 인원이 stale(1) 로 박제된다.
        synchronized(c.lock) {
            c.rosterMap[gid]?.let { m ->
                c.rosterMap[gid] = m.toMutableMap().apply { remove(bareId(c.mcpttId)) }.toMap()
            }
        }
        publishRosters()
        // 세션 종료 시점은 서버측 구독이 사라진 채 발견된 실측 지점이다 — 확인을 무효화해 즉시 재확인한다(살아 있으면
        //   엔진이 in-dialog 갱신으로 흡수, 죽었으면 여기서 되살아난다).
        invalidateRosterConfirm(gid)
        syncRosterSubs()   // 이탈해도 편성 채널이면 구독 유지 — 인원수는 계속 보여야 한다
    }

    // ── 로스터 구독 (RFC 4575 conference) ──

    /** 채널 참가자 로스터 구독 시작/해지. 갱신은 엔진이 in-dialog 로 자동 수행한다.
     *  구독 대상은 **참여 채널이 아니라 제휴(편성) 채널 전체**다 — 참여하지 않은 채널의 접속 인원도 목록에 표시한다.
     *  멱등이 필요하다(등록·제휴·조인이 각자 부른다 — 확인 전 두 번 나가면 서버에 구독이 중복된다). 그 가드는 발행 후
     *  확인까지의 창에만 걸리고, 확인 없이 [SUB_CONFIRM_TIMEOUT_MS] 가 지나면 재발행 대상으로 되돌린다. */
    fun subscribeRoster(groupId: String, on: Boolean) {
        if (on) {
            val now = SystemClock.elapsedRealtime()
            synchronized(c.lock) {
                val confirmedAt = c.confirmedRosters[groupId]
                if (confirmedAt != null) {
                    if (now - confirmedAt < SUB_REASSERT_MS) return    // 아직 유효 — 재확인 시점 아님
                    c.confirmedRosters[groupId] = now                  // 재확인 발행 — 다음 주기까지 유효 취급
                } else {
                    val at = c.pendingRosters[groupId]
                    if (at != null && now - at < SUB_CONFIRM_TIMEOUT_MS) return   // 첫 확인 대기 중
                    c.pendingRosters[groupId] = now
                }
            }
        } else {
            synchronized(c.lock) {
                val wasConfirmed = c.confirmedRosters.remove(groupId) != null
                val wasPending = c.pendingRosters.remove(groupId) != null
                if (!wasConfirmed && !wasPending) return   // 걸어둔 적 없는 구독 — 해지 불필요
                c.rosterMap.remove(groupId)
            }
            publishRosters()
        }
        Log.i(TAG, "conference 구독 ${if (on) "발행" else "해지"} $groupId")
        c.ctl.launch {
            val r = c.account?.subscribeConference(groupId, on)
            if (r == null || !r.ok) {
                Log.w(TAG, "conference 구독($on) 실패 $groupId: ${r?.reason}")
                synchronized(c.lock) { if (on) c.pendingRosters.remove(groupId) }
            }
        }
    }

    /** 특정 그룹의 구독 확인만 무효화 — 다음 [syncRosterSubs] 가 재확인(SUBSCRIBE)을 발행한다. */
    fun invalidateRosterConfirm(groupId: String) {
        if (groupId.isBlank()) return
        synchronized(c.lock) { c.confirmedRosters.remove(groupId); c.pendingRosters.remove(groupId) }
    }

    /** 구독 확인 상태 일괄 초기화 — 서버가 우리 상태를 잃었다고 판단했을 때만. 호출자는 [PttController.lock] 을 보유한다. */
    fun clearSubStateLocked() {
        c.confirmedRosters.clear()
        c.pendingRosters.clear()
        c.rosterMap.clear()
        c.xcapConfirmedAt.clear()
        c.xcapPendingAt.clear()
    }

    /** 제휴(편성) 채널 집합에 로스터 구독을 맞춘다 — 새로 편성된 채널은 구독하고, 빠진 채널은 해지. */
    fun syncRosterSubs() {
        if (c.regState.value !is RegState.Registered) return
        val want = desiredAffiliations()
        val have = synchronized(c.lock) { c.confirmedRosters.keys + c.pendingRosters.keys }
        val drop = have - want
        if (drop.isNotEmpty()) {
            Log.i(TAG, "syncRosterSubs 해지 대상=$drop (want=$want have=$have " +
                    "groups=${c._groups.value.map { bareId(it.uri) }} joined=${c.channelStore?.joined} sel=${c._selectedGroup.value})")
        }
        want.forEach { subscribeRoster(it, true) }
        drop.forEach { subscribeRoster(it, false) }
    }

    fun publishRosters() {
        c._channelRosters.value = synchronized(c.lock) { c.rosterMap.mapValues { it.value.toMap() } }
    }

    /** 로스터 NOTIFY(구독·in-dialog) — 도착 = 구독 확인. 참여 중이면 세션 participants 도 같은 값으로 맞춘다.
     *  ⚠️"본인은 항상 접속"은 **참여 중일 때만** 성립한다 — 미조인 채널에 자신을 넣으면 참여하지도 않은 채널에 내가 있는
     *  것으로 보인다. */
    fun onRoster(u: RosterUpdate) {
        val gid = bareId(u.groupId)
        if (gid.isBlank()) return
        val me = bareId(c.mcpttId)
        synchronized(c.lock) {
            c.pendingRosters.remove(gid)
            c.confirmedRosters[gid] = SystemClock.elapsedRealtime()
            val s = c.sessionMap[gid]
            val base = if (u.full) mutableMapOf() else (s?.participants ?: c.rosterMap[gid]?.toMutableMap() ?: mutableMapOf())
            var selfDisconnected = false
            for (e in u.users) {
                val id = bareId(e.uri)
                if (id.isBlank()) continue
                val status = e.status.ifBlank { "connected" }
                if (status.equals("disconnected", ignoreCase = true)) {
                    base.remove(id)
                    if (id == me) selfDisconnected = true
                } else base[id] = status
            }
            // 참여 중이면 본인은 항상 포함 — 단, 이 NOTIFY 가 본인 이탈을 명시하면 재추가하지 않는다(나가기 직후의 마지막
            //   NOTIFY 가 세션 제거보다 먼저 처리되는 레이스 — 08-11 실측).
            if (s != null && !selfDisconnected) base[me] = "connected"
            s?.participants = base
            c.rosterMap[gid] = base.toMap()
        }
        publishRosters()
        c.publish()
    }

    // ── 문서 변경 구독 (RFC 5875 xcap-diff) ──

    /** 축마다 서버 PSI 하나에 대한 단일 구독 — [XCAP_GMS]=편성, [XCAP_CMS]=사용자 프로파일·시스템 설정.
     *  멱등·재확인 규율은 로스터 구독과 같다. 확인 신호는 **그 축의 NOTIFY 도착**. */
    fun subscribeXcap(kind: String, on: Boolean, force: Boolean = false) {
        val now = SystemClock.elapsedRealtime()
        synchronized(c.lock) {
            if (on && force) {
                c.xcapPendingAt[kind] = now                       // 문서 목록이 바뀌었다 — 재확인 간격과 무관하게 re-SUBSCRIBE
            } else if (on) {
                val confirmedAt = c.xcapConfirmedAt[kind]
                if (confirmedAt != null) {
                    if (now - confirmedAt < SUB_REASSERT_MS) return
                    c.xcapConfirmedAt[kind] = now
                } else {
                    val at = c.xcapPendingAt[kind]
                    if (at != null && now - at < SUB_CONFIRM_TIMEOUT_MS) return
                    c.xcapPendingAt[kind] = now
                }
            } else {
                val wasConfirmed = c.xcapConfirmedAt.remove(kind) != null
                val wasPending = c.xcapPendingAt.remove(kind) != null
                if (!wasConfirmed && !wasPending) return
            }
        }
        c.ctl.launch {
            // 규격형 구독(TS 24.481 §6.3.13.2.1 · TS 24.484 §6.3.13.2.2) — 액세스 토큰 + 문서 목록. 토큰이 아직 없으면 본문 없는 구독(옛 형식)
            val acc = c.account
            val token = c.token
            val docs = if (on && token != null) xcapDocuments(kind) else emptyList()
            val r = when {
                acc == null -> null
                !on -> acc.subscribeXcapDiff(xcapPsi(kind), emptyList(), "", false)
                token == null -> acc.subscribeXcapDiff(xcapPsi(kind), true)
                docs.isEmpty() -> { synchronized(c.lock) { c.xcapPendingAt.remove(kind) }; return@launch }   // 구독할 문서가 아직 없다(그룹 0개)
                else -> acc.subscribeXcapDiff(xcapPsi(kind), docs, token, true)
            }
            if (r == null || !r.ok) {
                Log.w(TAG, "$kind 구독($on) 실패: ${r?.reason}")
                synchronized(c.lock) { if (on) c.xcapPendingAt.remove(kind) }
            }
        }
    }

    /** 서버 PSI — CSP 는 SUBSCRIBE Request-URI 의 `gms`/`cms` 로 이벤트 축을 판별한다. */
    private fun xcapPsiAor(kind: String) = "sip:${kind}_psi@${c.sipConfig.domain}"

    /** 축의 구독 프록시 PSI — GMS 는 UE initial configuration `<GMS-URI>`, CMS 는 설정값(`sip:cms_psi@<도메인>` — TS 24.484 §6.3.13.3.1). */
    private fun xcapPsi(kind: String) = if (kind == XCAP_GMS) c.gmsPsi.ifBlank { xcapPsiAor(kind) } else xcapPsiAor(kind)

    /** 축의 구독 문서 — GMS = 편성 그룹마다 그룹 ID 로 찾는 문서, CMS = UE initial configuration·user profile·service configuration
     *  (MCVideo 를 쓰면 그 두 문서도). */
    private fun xcapDocuments(kind: String): List<String> =
        if (kind == XCAP_GMS) CscClient.gmsSubscriptionDocuments(c._groups.value.map { it.uri })
        else CscClient.cmsSubscriptionDocuments(c.mcpttId, c.mcsUeId, if (c.videoPlane.available.value) c.mcpttId else "")

    private fun xcapConfirmed(kind: String) = synchronized(c.lock) { c.xcapConfirmedAt.containsKey(kind) }

    /** MCData·로스터·dialog 가 아닌 MESSAGE/NOTIFY — xcap-diff 는 여기서, text/plain 문자는 서비스로. */
    fun onSipMessage(m: SipMessage) {
        if (m.contentType.contains("xcap-diff", ignoreCase = true)) {
            // 어느 축의 구독이 확인됐는지는 notifier 신원(From = 서버 PSI)이 정본이다 — 본문은 변경 문서가 없으면 빈
            //   xcap-diff 라 축을 알 수 없다.
            val kind = if (bareId(m.fromUri) == "${XCAP_CMS}_psi") XCAP_CMS else XCAP_GMS
            synchronized(c.lock) {
                c.xcapPendingAt.remove(kind)
                c.xcapConfirmedAt[kind] = SystemClock.elapsedRealtime()
            }
            runCatching { onXcapDiff(m.body) }
            return
        }
        if (m.contentType.startsWith("text/", ignoreCase = true)) {
            c._incomingText.trySend(ImMessage(m.fromUri, m.contentType, m.body))
            return
        }
        Log.d(TAG, "미처리 본문 ${m.contentType} from=${m.fromUri}")
    }

    /** xcap-diff NOTIFY — "어느 문서가 바뀌었다"는 신호만 온다. 실제 내용은 XCAP HTTP 로 재조회.
     *  본문 예: `<document new-etag=".." sel="org.openmobilealliance.groups/users/tel:{나}/tel:{그룹}"/>` */
    private fun onXcapDiff(xml: String) {
        val sels = Regex("sel=\"([^\"]+)\"").findAll(xml).map { it.groupValues[1] }.toList()
        val changed = sels
            .filter { it.contains("openmobilealliance.groups", ignoreCase = true) }
            .mapNotNull { it.substringAfterLast("tel:").takeIf { g -> g.isNotBlank() } }
        val profileChanged = sels.any { it.contains("mcptt.user-profile", ignoreCase = true) }
        val svcCfgChanged = sels.any { it.contains("mcptt.service-config", ignoreCase = true) }
        val mcvSvcCfgChanged = sels.any { it.contains("mcvideo.service-config", ignoreCase = true) }
        val ueInitChanged = sels.any { it.contains("mcptt.ue-init-config", ignoreCase = true) }
        Log.i(TAG, "xcap-diff NOTIFY — 편성 $changed / 프로파일 $profileChanged / 시스템설정 $svcCfgChanged / 단말초기설정 $ueInitChanged")
        if (profileChanged) loadUserProfile()
        if (svcCfgChanged) loadServiceConfig()
        if (mcvSvcCfgChanged) loadMcVideoServiceConfig()
        if (ueInitChanged) c.reloadUeInitConfig()
        if (changed.isEmpty() && (profileChanged || svcCfgChanged || mcvSvcCfgChanged || ueInitChanged)) {
            c._status.value = "설정 변경 통지"
            // user profile 변경은 편성(소속 그룹)이 바뀐 것일 수 있다 — 규격형 GMS 구독은 구독하지 않은 새 그룹을 통지하지 않으므로
            //   목록을 다시 받는다(바뀌었으면 loadGroups 가 GMS 를 새 목록으로 re-SUBSCRIBE)
            if (profileChanged) loadGroups()
            return
        }
        c._status.value = if (changed.isEmpty()) "편성 변경 통지" else "편성 변경: ${changed.joinToString()}"
        loadGroups()
        changed.forEach { loadGroupDetail(it) }
    }

    // ── affiliation (TS 24.379 §9) — 목표 집합·재시도는 앱, PUBLISH·ETag·412 는 코어 ──

    /** 희망 affiliation 집합 — 편성 채널 전체(CSC 목록 + 영속 참여 채널 + 선택 채널). */
    fun desiredAffiliations(): Set<String> = buildSet {
        c._groups.value.forEach { add(bareId(it.uri)) }
        c.channelStore?.joined?.forEach { add(it) }
        c._selectedGroup.value?.let { add(it) }
    }

    /** 서버가 받은 제휴인가. 수명 갱신(TS 24.379 §9.2.1.2 — 규격형은 만료 없음)·등록 재성립 뒤 다시 싣기는 코어가 한다. */
    private fun affValid(groupId: String): Boolean = groupId in c.affConfirmed

    /** 확정이 없거나 낡았고, in-flight·백오프 대기 중도 아닐 때만 발행(트리거 중복 억제). */
    fun ensureAffiliated(groupId: String) {
        if (affValid(groupId)) return
        if (c.affPending.values.any { it.first == groupId && it.second }) return
        if ((c.affBackoffUntil[groupId] ?: 0L) > SystemClock.elapsedRealtime()) return
        affiliate(groupId, true)
    }

    /** 등록 상태에서 희망 집합 전체를 보장 — 등록 성공·그룹 목록 적재·주기 루프에서 호출. */
    fun affiliateAll() {
        if (c.regState.value !is RegState.Registered) return
        val want = desiredAffiliations()
        // 동시 제휴 상한(TS 24.484 N2)은 **서버가 강제**한다 — 앱이 임의로 잘라내면 어느 채널을
        //   버릴지 정책 없이 fan-out 을 잃는다. 초과는 근거만 남기고 요청은 그대로 보낸다.
        val n2 = userProfile.value?.maxAffiliations ?: 0          // user-profile MaxAffiliationsN2 (TS 24.484 §8.3.2.1)
        if (n2 > 0 && want.size > n2)
            Log.w(TAG, "편성 채널 ${want.size}개 > 상한 N2=$n2 — 초과분은 서버가 거절할 수 있다")
        want.forEach { ensureAffiliated(it) }
    }

    /** affiliation PUBLISH — 성공 여부는 [PttController.affiliated] 에 낙관 기록하지 않는다(token 상관 응답 2xx 에서만 확정).
     *  ETag(SIP-If-Match)·412 초기 재발행은 코어가 한다(ue_sdk.md §4.2). */
    fun affiliate(groupId: String, on: Boolean = true) {
        c._status.value = if (on) "affiliate $groupId" else "de-affiliate $groupId"
        c.ctl.launch {
            val r = c.account?.affiliate(groupId, on)
            val token = r?.getOrNull() ?: run { Log.w(TAG, "affiliate $groupId: ${r?.reason ?: "not registered"}"); return@launch }
            c.affPending[token] = groupId to on
            c.takeEarly(token)?.let { onAffiliationResult(it) }
            // 응답이 영영 없는 경우(전송 실패) pending 이 남아 재발행을 막는 것 방지 — Timer B(≈32s)보다 넉넉히 기다린 뒤 회수.
            c.scope.launch {
                delay(40_000)
                if (c.affPending.remove(token) != null)
                    Log.w(TAG, "affiliate $groupId 응답 없음(타임아웃) — 주기 갱신 루프가 재시도")
            }
        }
    }

    /** PUBLISH 최종 응답 — 2xx 확정, 403 은 조건 변화 대기, 그 밖은 지수 백오프 재시도. 이 평면의 token 이면 true. */
    fun onAffiliationResult(r: RequestResult): Boolean {
        val (g, on) = c.affPending.remove(r.token) ?: return false
        if (r.code in 200..299) {
            c.affAttempts.remove(g)
            c.affBackoffUntil.remove(g)
            if (on) {
                c.affConfirmed.add(g)
                c._affiliated.value = c._affiliated.value + g
                if (rejoinPending.remove(g)) rejoinAfterAffiliation(g)
            } else {
                c.affConfirmed.remove(g)
                c._affiliated.value = c._affiliated.value - g
            }
            return true
        }
        if (!on) return true
        c.affConfirmed.remove(g)
        rejoinPending.remove(g)
        c._affiliated.value = c._affiliated.value - g
        val n = ((c.affAttempts[g] ?: 0) + 1).also { c.affAttempts[g] = it }
        if (r.code == 403) {
            // RFC 3261 §21.4.4 — 403 은 "수정 없이 반복하지 말 것". 조건이 바뀌어야 낫는 실패(등록 소실 / 그룹 비멤버)
            //   이므로 타이머 재시도를 걸지 않고, 조건 변화 이벤트(등록 성공→affiliateAll, 그룹목록 재적재, 채널 선택)에 맡긴다.
            //   등록 소실이 원인일 수 있으므로 등록 갱신만 트리거해 조건 자체를 바꾼다(60s 스로틀).
            Log.w(TAG, "affiliate $g 403 ${r.reason} — 타이머 재시도 없음(조건 변화 대기)")
            if (SystemClock.elapsedRealtime() - c.affReRegisterAt > 60_000L) {
                c.affReRegisterAt = SystemClock.elapsedRealtime()
                Log.w(TAG, "affiliate 403 — 등록 소실 추정, 등록 갱신 요청")
                // 등록이 서버에서 사라졌다면 구독도 함께 사라졌다 — 확인 상태를 직접 비워 다음 syncRosterSubs 가 재발행하게 한다.
                synchronized(c.lock) { clearSubStateLocked() }
                publishRosters()
                c.cmd("refreshRegistration") { c.account?.refreshRegistration() ?: com.cims.ue.sdk.CimsResult.ok(Unit) }
            }
            return true
        }
        // 일시적 실패(타임아웃·5xx 등)는 재시도가 정당 — 지수 백오프.
        val backoffMs = (30_000L shl (n - 1).coerceAtMost(3)).coerceAtMost(300_000L)
        c.affBackoffUntil[g] = SystemClock.elapsedRealtime() + backoffMs
        Log.w(TAG, "affiliate $g 실패 ${r.code} ${r.reason} — ${backoffMs / 1000}s 후 재시도(#$n)")
        c.scope.launch {
            delay(backoffMs)
            if (c.regState.value is RegState.Registered && g in desiredAffiliations() && !affValid(g)) affiliate(g, true)
        }
        return true
    }

    // ── 미제휴 거절의 자기 복구(TS 24.379 §10.1.1.4.2) ──

    /** 미제휴 403 뒤 제휴 2xx 를 기다리는 그룹(다시 걸 그룹)과, 그 그룹을 마지막으로 다시 건 시각. */
    private val rejoinPending: MutableSet<String> = java.util.concurrent.ConcurrentHashMap.newKeySet()
    private val rejoinAt = java.util.concurrent.ConcurrentHashMap<String, Long>()
    private val rejoinPrimary: MutableSet<String> = java.util.concurrent.ConcurrentHashMap.newKeySet()

    /**
     * 편성 그룹의 [참여]·개시가 403 + Warning 120(미제휴 — TS 24.379 §10.1.1.4.2 «user is not affiliated to this group»)으로 끝났다.
     * 서버만 제휴를 잃은 경우(망 순단으로 등록이 풀렸던 동안 등)라 단말이 알 수 있는 유일한 신호다 — 제휴를 다시 싣고 2xx 뒤 **한 번**
     * 다시 건다. [REJOIN_GAP_MS] 안의 두 번째 120 은 그대로 둔다(되풀이하지 않는다). 비멤버 403(Warning 이 다르거나 없다)·긴급(서버
     * 암묵적 제휴)·애드혹·1:1 은 대상이 아니고, 일제 통화는 제휴만 다시 싣는다(다시 걸면 전원에게 다시 울린다).
     */
    fun handleNotAffiliated(callId: Int, code: Int, warningCode: Int) {
        if (code != 403 || warningCode != 120) return
        val s = synchronized(c.lock) { c.sessionMap.values.firstOrNull { it.callId == callId } } ?: return
        val gid = s.groupId
        if (isAdhocId(gid) || s.privatePeer || s.emergency) return
        val now = SystemClock.elapsedRealtime()
        c.affConfirmed.remove(gid)
        c._affiliated.value = c._affiliated.value - gid
        if (s.broadcast || now - (rejoinAt[gid] ?: 0L) < REJOIN_GAP_MS) {
            Log.w(TAG, "[$gid] 403 120(미제휴) — 제휴만 다시 싣는다")
            affiliate(gid, true)
            return
        }
        rejoinAt[gid] = now
        rejoinPending.add(gid)
        if (s.role == ChannelRole.PRIMARY) rejoinPrimary.add(gid) else rejoinPrimary.remove(gid)
        Log.i(TAG, "[$gid] 403 120(미제휴) — 제휴를 다시 싣고 한 번 더 건다")
        c._status.value = "[$gid] 제휴 다시 싣는 중"
        affiliate(gid, true)
    }

    private fun rejoinAfterAffiliation(groupId: String) {
        val primary = rejoinPrimary.remove(groupId)
        c.scope.launch {
            delay(300)                 // 거절 세션 teardown(onCallEnded) 정리 후
            if (c.regState.value !is RegState.Registered) return@launch
            Log.i(TAG, "[$groupId] 제휴 2xx — 그룹콜 다시 건다")
            joinGroupCall(groupId, takePrimary = primary)
        }
    }

    fun selectGroup(groupId: String) {
        c._selectedGroup.value = groupId
        if (c.regState.value is RegState.Registered) ensureAffiliated(groupId)
    }

    // ── CSC (GMS·CMS) ──

    fun login(userName: String, password: String) = c.scope.launch {
        val cl = c.csc ?: run { c._status.value = "CSC 미설정"; return@launch }
        val r = cl.login(userName, password)
        if (r.ok) { c.token = r.value!!.accessToken; c._status.value = "CSC 인증 성공"; loadGroups() }
        else c._status.value = "CSC 인증 실패: ${r.code} ${r.reason}"
    }

    /** SSO: CIMS 공유 계정에서 받은 MCPTT(TS 33.180) access_token 직접 적용 — 수동 CSC 로그인 대체. */
    fun setAccessToken(accessToken: String) {
        c.token = accessToken
        c._status.value = "CIMS 계정 토큰 적용"
        loadGroups()
    }

    /** 편성에서 사라진 채널 정리 — 그룹이 삭제됐거나 내가 멤버에서 빠졌다(TS 24.481 그룹 목록 = 내가 멤버인 그룹).
     *  참여 의도(joined)·고른 주채널·선택 그룹에서 지운다: 남겨 두면 주채널 화면이 없는 그룹(id)을 가리키고,
     *  PTT 가 그 그룹으로 개시해 거절되고, 주기 affiliation 이 그 그룹을 계속 PUBLISH 한다.
     *  애드혹 임시 그룹은 편성 목록에 없는 것이 정상이라 건드리지 않는다. 진행 중 세션은 서버가 해제한다. */
    private fun dropRemovedChannels(present: Set<String>) {
        val st = c.channelStore ?: return
        val gone = (st.joined + listOfNotNull(st.primary, c._selectedGroup.value))
            .filter { !isAdhocId(it) && it !in present }.distinct()
        if (gone.isEmpty()) return
        Log.i(TAG, "편성에서 사라진 채널 정리: $gone")
        gone.forEach { st.remove(it) }                       // primary 였으면 함께 비운다
        if (c._selectedGroup.value in gone) c._selectedGroup.value = null
        c.publish()                                           // 고른 주채널(chosenPrimary) 화면 갱신
    }

    fun loadGroups() = c.scope.launch {
        if (c.token == null) { c._status.value = "토큰 없음"; return@launch }
        val r = c.withToken { cl, t -> cl.listGroups(t, c.mcpttId) }
        if (r.ok) {
            val list = r.value.orEmpty().map { GroupSummary.of(it) }
            val before = c._groups.value.map { it.uri }.toSet()
            c._groups.value = list
            // 규격형 구독은 구독한 그룹 문서만 통지한다 — 편성 목록이 바뀌면 새 목록으로 re-SUBSCRIBE(TS 24.481 §6.3.13.2.1)
            if (list.map { it.uri }.toSet() != before && c.regState.value is RegState.Registered) subscribeXcap(XCAP_GMS, true, force = true)
            val ids = list.map { bareId(it.uri) }
            dropRemovedChannels(ids.toSet())
            // 선택 그룹(TS 24.484 currently-selected group) 복원 — 마지막 주채널 우선,
            // 이력이 없거나 편성에서 빠졌으면 목록 첫 그룹(최초 1회 폴백).
            if (c._selectedGroup.value == null) {
                c._selectedGroup.value = c.channelStore?.lastPrimary?.takeIf { it in ids } ?: ids.firstOrNull()
            }
            affiliateAll()   // 편성 채널 전체 affiliation (등록 전이면 등록 완료 트리거가 수행)
            syncRosterSubs() // 목록이 채워졌으니 로스터 구독도 그 집합으로 맞춘다
            c._status.value = "그룹 ${list.size}개"
        } else {
            // 갱신 뒤에도 401 = 계정 문제(refresh 만료·회수) — 재로그인 안내. 그 외는 원문.
            c._status.value = if (r.code == 401) "그룹 조회 실패: 인증 만료 — CIMS 앱에서 다시 로그인"
            else "그룹 조회 실패: ${r.code} ${r.reason}"
        }
        // 설정 문서(user-profile·service-config)는 cms 구독이 있으면 NOTIFY 로 온다 — 목록 갱신 계기마다 재조회하지 않는다
        //   (편성 변경 NOTIFY 는 그룹마다 1건씩 와서 증폭된다). 구독이 없거나 죽었을 때, 또는 아직 한 번도 받지 못했을 때
        //   이 계기가 취득한다 — 등록 직후 cms NOTIFY 가 토큰보다 먼저 오면 그 조회는 토큰 없음으로 건너뛰어지고,
        //   구독은 확인된 채라 이 계기가 유일한 취득 기회다(없으면 SOS 가 긴급 대상 없이 선택 그룹으로 간다).
        val cmsLive = xcapConfirmed(XCAP_CMS)
        if (!cmsLive || _userProfile.value == null) loadUserProfile()
        if (!cmsLive || _serviceConfig.value == null) loadServiceConfig()
        if (!cmsLive || !mcvServiceConfigLoaded) loadMcVideoServiceConfig()
    }

    /** 그룹 문서(TS 24.481, GMS XCAP) 조회 — 채널 상세 진입·편성 변경 통지 때. */
    fun loadGroupDetail(groupId: String) = c.scope.launch {
        if (c.token == null) return@launch
        val uri = c._groups.value.firstOrNull { bareId(it.uri) == groupId }?.uri ?: "tel:$groupId"
        val r = c.withToken { cl, t -> cl.getGroup(t, c.mcpttId, uri) }
        if (r.ok) c._groupDocs.value = c._groupDocs.value + (groupId to GroupDoc.of(r.value!!))
        else Log.d(TAG, "group doc $groupId 조회 실패: ${r.code} ${r.reason}")
    }

    private val _userProfile = MutableStateFlow<PttController.UserProfile?>(null)
    val userProfile: StateFlow<PttController.UserProfile?> = _userProfile.asStateFlow()
    @Volatile private var userProfileEtag = ""

    private val _serviceConfig = MutableStateFlow<PttController.ServiceConfig?>(null)
    /** 시스템 서비스 설정 (TS 24.484). null = 아직 수신하지 못함. */
    val serviceConfig: StateFlow<PttController.ServiceConfig?> = _serviceConfig.asStateFlow()
    @Volatile private var serviceConfigEtag = ""

    /** user-profile 문서 조회 — 해석은 코어(UserProfileDoc). 실패해도 치명 아님(서버 게이트가 최종 판정). ETag 캐시. */
    fun loadUserProfile() = c.scope.launch {
        if (c.token == null) return@launch
        val r = c.withToken { cl, t -> cl.fetchUserProfile(t, c.mcpttId, userProfileEtag) }
        if (!r.ok) { Log.d(TAG, "user-profile 조회 실패(프로파일 없이 동작): ${r.code} ${r.reason}"); return@launch }
        val d = r.value ?: return@launch                          // 304 — 가진 사본 유지
        userProfileEtag = d.etag
        val p = PttController.UserProfile(
            emergencyGroupMode = d.emergencyGroup.mode.ifBlank { "DedicatedGroup" },
            emergencyGroupId = bareId(d.emergencyGroup.uri).ifBlank { null },
            allowEmergencyCall = d.allowEmergencyGroupCall,
            allowEmergencyAlert = d.allowActivateEmergencyAlert,
            allowAdhocCall = d.allowAdhocGroupCall,
            allowPrivateCall = d.allowPrivateCall,
            maxAffiliations = d.maxAffiliationsN2 ?: 0,
            allowEmergencyPrivateCall = d.allowEmergencyPrivateCall,
            privateEmergencyMode = d.emergencyPrivateRecipient.mode.ifBlank { "LocallyDetermined" },
            emergencyPrivateRecipient = bareId(d.emergencyPrivateRecipient.uri).ifBlank { null },
        )
        _userProfile.value = p
        Log.i(TAG, "user-profile 적재 — 긴급대상=${p.emergencyGroupMode}/${p.emergencyGroupId}" +
            " 인가(긴급콜·경보·애드혹)=${p.allowEmergencyCall}/${p.allowEmergencyAlert}/${p.allowAdhocCall}")
    }

    /** service-config 문서 조회 — user-profile 과 같은 규율(실패 무해 · ETag 캐시). */
    fun loadServiceConfig() = c.scope.launch {
        if (c.token == null) return@launch
        val r = c.withToken { cl, t -> cl.fetchServiceConfig(t, c.mcpttId, serviceConfigEtag) }
        if (!r.ok) { Log.d(TAG, "service-config 조회 실패(시스템 설정 없이 동작): ${r.code} ${r.reason}"); return@launch }
        val d = r.value ?: return@launch
        serviceConfigEtag = d.etag
        val cfg = PttController.ServiceConfig(
            rpEmergency = d.rpEmergency.ifBlank { null },
            rpImminentPeril = d.rpImminentPeril.ifBlank { null },
            rpNormal = d.rpNormal.ifBlank { null },
        )
        _serviceConfig.value = cfg
        Log.i(TAG, "service-config 적재 — RP 긴급=${cfg.rpEmergency} 임박=${cfg.rpImminentPeril} 일반=${cfg.rpNormal}")
    }

    @Volatile private var mcvServiceConfigEtag = ""
    @Volatile private var mcvServiceConfigLoaded = false

    /** MCVideo service configuration 조회(TS 24.484 §9.4) — 전송 제어 참여자 타이머 T100~T104(`<tc-timers-counters-R14>`, TS 24.581
     *  표 11.1.1-1)를 계정에 싣는다. 다음 MCVideo 호부터 쓴다. MCVideo 를 쓰지 않는 계정(PSI 없음)은 받지 않는다. 실패 무해 · ETag 캐시. */
    fun loadMcVideoServiceConfig() = c.scope.launch {
        if (c.token == null || !c.videoPlane.available.value) return@launch
        val r = c.withToken { cl, t -> cl.fetchMcVideoServiceConfig(t, mcvServiceConfigEtag) }
        if (!r.ok) { Log.d(TAG, "mcvideo service-config 조회 실패(기본 타이머로 동작): ${r.code} ${r.reason}"); return@launch }
        mcvServiceConfigLoaded = true
        val d = r.value ?: return@launch                          // 304 — 실은 값 그대로
        mcvServiceConfigEtag = d.etag
        val set = c.account?.setTcTimers(d.tcTimers)
        Log.i(TAG, "mcvideo service-config 적재 — 전송 제어 타이머 ${d.tcTimers} (${set?.ok})")
    }
}
