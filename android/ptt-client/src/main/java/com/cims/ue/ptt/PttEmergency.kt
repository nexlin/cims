package com.cims.ue.ptt

import android.util.Log
import com.cims.ue.ptt.PttController.Companion.TAG
import com.cims.ue.ptt.PttController.Companion.bareId
import com.cims.ue.ptt.PttController.Companion.isAdhocId
import com.cims.ue.sdk.ConditionCause
import com.cims.ue.sdk.ConditionChange
import com.cims.ue.sdk.EmergencyAlert
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * 긴급 평면 — SOS 개시·해제, 긴급 경보, 세션 조건의 화면 투영(mcptt_emergency_modes.md §4.3).
 * 규격 절차(상향·하향 re-INVITE + Resource-Priority, 403 복원, 서버 재광고 해석, 경보 MESSAGE 빌드·해석)는 코어가 하고
 * (ue_sdk.md §4.2 «긴급·임박 세션 조건»), 여기는 **대상 결정·403 뒤 normal 재발신·경보 정합·배너 latch** 정책만 한다.
 */
internal class EmergencyPlane(private val c: PttController) {

    /**
     * 긴급(SOS) 개시 — 하드웨어 SOS 키/화면 SOS 버튼.
     * 주채널 통화 중이면 조건 상향(re-INVITE emergency-ind=true), 미참여면 긴급 그룹콜 발신.
     * 새 긴급콜 대상은 프로파일 entry-info(TS 24.484)가 결정: DedicatedGroup=전용 긴급그룹,
     * UseCurrentlySelectedGroup=선택 그룹(마지막 주채널) — 프로파일을 아직 받지 못했으면 먼저 취득한다([startEmergencyCall]).
     * 서버가 미인가로 403 거절하면 normal 재발신 폴백(개시)·latch 복원(상향 — 코어 DENIED)한다.
     */
    fun startEmergency() {
        if (!emergencyAllowed()) return
        val s = c.primarySession()
        if (s == null) {
            if (startingCall) { c._status.value = "긴급: 개시 중"; return }
            startingCall = true
            c.scope.launch { try { startEmergencyCall() } finally { startingCall = false } }
            return
        }
        if (s.emergency) { c._status.value = "[${s.groupId}] 이미 긴급 상태"; return }
        s.emergency = true
        s.emergencyMine = true
        sendAlert(s.groupId, true)
        if (s.callId >= 0) c.cmd("setCondition(emergency)") { c.ue.call(s.callId).setCondition(emergency = true) }
        c.feedback?.emergencyTone()
        c._status.value = "🚨 [${s.groupId}] 긴급 개시"
        c.emit(PttEventKind.EMERGENCY, s.groupId)
        c.publish()
    }

    /** 개시 인가 — 사용자 인가(ruleset allow-emergency-group-call, TS 24.484 §8.3.2.7). 서버도 미인가를 403 으로 거절하나
     *  (normal 폴백), 개시 전에 알리는 편이 정확하다. 문서 미수신이면 막지 않는다. */
    private fun emergencyAllowed(): Boolean {
        if (c.userProfile.value?.allowEmergencyCall == false) {
            c._status.value = "긴급: 개시 권한 없음"
            c.feedback?.blocked("긴급통화 개시 권한이 없습니다")
            return false
        }
        return true
    }

    /** SOS 가 개시 대상을 정하는 동안(프로파일 취득) 겹친 SOS 를 무시한다 — 경보·긴급콜이 두 번 나가지 않게. */
    @Volatile private var startingCall = false

    /** 미참여 상태의 SOS — 새 긴급 그룹콜. 대상은 프로파일(TS 24.484)이 정하므로 아직 받지 못했으면(등록 직후·CSC 조회
     *  경합) 먼저 취득하고(최대 [PROFILE_WAIT_MS]) 인가를 다시 본다. 받은 대상으로 경보 먼저(말 못 해도 신원·그룹은 전파), 통화 다음. */
    private suspend fun startEmergencyCall() {
        if (c.userProfile.value == null) {
            withTimeoutOrNull(PROFILE_WAIT_MS) { c.loadUserProfile().join() }   // 긴급은 오래 기다리지 않는다
            if (!emergencyAllowed()) return
        }
        val gid = emergencyTargetGroup() ?: return
        sendAlert(gid, true)
        c.joinGroupCall(gid, emergency = true)
        c.feedback?.emergencyTone()
    }

    /** SOS 새 긴급콜 대상 결정 (TS 24.484 MCPTTGroupInitiation entry-info).
     *  DedicatedGroup(기본)=프로비저닝된 전용 긴급그룹 — 미지정이면 불발(서버도 미인가 403).
     *  UseCurrentlySelectedGroup=선택 그룹(마지막 주채널). 취득을 시도하고도 프로파일이 없으면(CSC 불통) 선택 그룹 —
     *  긴급은 막지 않는다(서버 게이트가 최종 판정). null 반환 시 상태 메시지는 이미 표시됨. */
    private fun emergencyTargetGroup(): String? {
        if (c.userProfile.value == null) Log.w(TAG, "긴급: user-profile 없음 — 선택 그룹 ${c._selectedGroup.value} 으로 개시")
        val p = c.userProfile.value
        if (p != null && p.emergencyGroupMode == "DedicatedGroup") {
            return p.emergencyGroupId ?: run {
                c._status.value = "긴급: 전용 긴급그룹 미지정 — 관리자에게 문의"
                c.feedback?.blocked("긴급 불가: 전용 긴급그룹 미지정 — 관리자에게 문의")
                null
            }
        }
        return c._selectedGroup.value ?: run {
            c._status.value = "긴급: 대상 그룹 없음"
            c.feedback?.blocked("긴급 불가: 대상 그룹 없음")
            null
        }
    }

    /** 긴급 1:1 개시 (TS 24.379 §11 emergency private call) — 대상 결정은 프로파일 MCPTTPrivateRecipient
     *  entry-info(TS 24.484): UsePreConfigured=사전 지정 수신자(미지정이면 불발, 탭한 상대와 달라도 지정 수신자에게 고정),
     *  LocallyDetermined(기본)=사용자가 고른 상대. 프로파일 미수신이면 고른 상대로 낙관 발신(서버가 최종 판정). */
    fun startEmergencyPrivateCall(peer: String, fullDuplex: Boolean = false) {
        val p = c.userProfile.value
        val target: String
        if (p?.privateEmergencyMode == "UsePreConfigured") {
            target = p.emergencyPrivateRecipient ?: run {
                c._status.value = "긴급 1:1: 지정 수신자 미지정 — 관리자에게 문의"
                c.feedback?.blocked("긴급 1:1 불가: 지정 수신자 미지정 — 관리자에게 문의")
                return
            }
            if (target != bareId(peer)) c._status.value = "긴급 1:1: 지정 수신자($target)로 발신"
        } else target = bareId(peer)
        c.startPrivateCall(target, fullDuplex, emergency = true)
    }

    /** 미인가 긴급콜 403 (TS 24.379 §6.3.3.1.14) — 성립 전 거절된 긴급 개시를 normal 재발신으로 폴백한다.
     *  긴급이 아닌 403(스캐너 차단 등)은 비대상. 세션 정리(onCallEnded) 전에 불린다. */
    fun handleEmergencyDenied(callId: Int, code: Int) {
        if (code != 403) return
        val s = synchronized(c.lock) {
            c.sessionMap.values.firstOrNull { it.callId == callId && it.emergency && it.emergencyMine }
        } ?: return
        val gid = s.groupId
        if (isAdhocId(gid)) return
        if (s.privatePeer) {
            // 긴급 1:1 미인가 — 일반 1:1 로 폴백. 1:1 은 그룹 경보 선발신이 없어 경보 정합 불요.
            val fd = s.fullDuplex
            c._status.value = "[$gid] 긴급 1:1 미인가 — 일반 1:1 로 전환"
            c.scope.launch {
                delay(300)         // 거절 세션 teardown(onCallEnded) 정리 후 재발신
                c.startPrivateCall(gid, fullDuplex = fd)
            }
            return
        }
        c._status.value = "[$gid] 긴급 미인가 — 일반 통화로 전환"
        c.scope.launch {
            delay(300)             // 거절 세션 teardown(onCallEnded) 정리 후 재발신
            c.joinGroupCall(gid)
        }
        reconcileAlertAfterDenied(gid)
    }

    /** 콜 403 뒤 선발신 경보 정합 — 경보와 콜은 별개 게이트(서버: 경보 미인가=스트립·무전파)라 일괄 자동회수는 틀리다.
     *  프로파일을 재조회해 경보 인가까지 없다고 판명되면(=서버가 스트립해 아무도 받지 못한 유령 배너) 로컬 표시만 회수한다 —
     *  서버에 활성 경보가 없으므로 취소 MESSAGE 는 보내지 않는다. 인가가 확인되면 유지. 재조회 실패 시도 유지(fail-open). */
    private fun reconcileAlertAfterDenied(groupId: String) = c.scope.launch {
        c.loadUserProfile().join()
        if (c.userProfile.value?.allowEmergencyAlert != false) return@launch
        val me = bareId(c.mcpttId)
        if (c._alerts.value.none { it.groupId == groupId && it.userId == me && it.mine }) return@launch
        removeAlert(groupId, me)
        c._status.value = "[$groupId] 긴급경보 미인가 — 표시 회수"
        c.emit(PttEventKind.ALERT_END, groupId)
        c.publish()
    }

    /** 긴급 해제 — 개시자만 유효(서버는 비개시자의 취소 re-INVITE 를 무시, TS 24.379). */
    fun cancelEmergency() {
        val s = synchronized(c.lock) { c.sessionMap.values.firstOrNull { it.emergency && it.emergencyMine } }
        if (s == null) {
            // 호 성립 전 SOS(경보만 나감)·호 실패 잔존 — 내 경보만이라도 취소한다.
            val mine = c._alerts.value.firstOrNull { it.mine }
                ?: run { c._status.value = "해제할 긴급 없음(개시자만 해제 가능)"; return }
            sendAlert(mine.groupId, false)
            c._status.value = "[${mine.groupId}] 긴급경보 해제"
            c.publish()
            return
        }
        s.emergency = false
        s.emergencyMine = false
        sendAlert(s.groupId, false)
        if (s.callId >= 0) c.cmd("setCondition(normal)") { c.ue.call(s.callId).setCondition(emergency = false) }
        c._status.value = "[${s.groupId}] 긴급 해제"
        c.emit(PttEventKind.EMERGENCY_END, s.groupId)
        c.publish()
    }

    /** 내 긴급경보 해제 — 긴급 세션 없이 경보만 남은 경우(403 미인가로 긴급콜이 normal 로 바뀐 뒤·호 성립 전)의 [해제].
     *  취소 MESSAGE 는 개시자만 유효(TS 24.379 §12.1.1.2)라 내 경보만 대상이다. */
    fun cancelAlert(groupId: String) {
        val me = bareId(c.mcpttId)
        if (c._alerts.value.none { it.groupId == groupId && it.userId == me && it.mine }) return
        sendAlert(groupId, false)
        c._status.value = "[$groupId] 긴급경보 해제"
        c.publish()
    }

    /** 긴급경보 MESSAGE 발신/취소 — SOS 개시/해제와 한 쌍 (TS 24.379 §12.1.1.1·§12.1.1.2, 본문·헤더는 코어).
     *  통화(INVITE)와 독립 경로라 호 성립 여부와 무관하게 신원·그룹이 전파된다. */
    private fun sendAlert(groupId: String, activate: Boolean) {
        // 활성 인가 — 사용자 인가(allow-activate-emergency-alert).
        //   취소(activate=false)는 막지 않는다 — 이미 걸린 경보의 회수는 언제나 허용한다.
        if (activate && c.userProfile.value?.allowEmergencyAlert == false) {
            c._status.value = "긴급경보: 권한 없음"
            c.feedback?.blocked("긴급경보 권한이 없습니다")
            return
        }
        c.ctl.launch {
            val r = c.account?.sendEmergencyAlert(groupId, activate)
            if (r == null || !r.ok) Log.w(TAG, "긴급경보 ${if (activate) "발신" else "취소"} 실패: ${r?.code} ${r?.reason}")
        }
        val me = bareId(c.mcpttId)
        if (activate) {
            addAlert(ActiveAlert(groupId, me, System.currentTimeMillis(), mine = true))
            c.emit(PttEventKind.ALERT, groupId)
        } else {
            removeAlert(groupId, me)
            c.emit(PttEventKind.ALERT_END, groupId)
        }
    }

    private fun addAlert(a: ActiveAlert) {
        c._alerts.value = c._alerts.value.filterNot { it.groupId == a.groupId && it.userId == a.userId } + a
    }

    private fun removeAlert(groupId: String, userId: String) {
        c._alerts.value = c._alerts.value.filterNot { it.groupId == groupId && it.userId == userId }
    }

    /** 수신 경보 배너 수동 닫기 — 이 단말의 표시만 제거(발신측 경보 상태와 무관). */
    fun dismissAlert(groupId: String, userId: String) = removeAlert(groupId, userId)

    /** 수신측 세션 긴급 배너 수동 닫기 — 이 단말의 표시 latch 만 해제(개시자 상태와 무관).
     *  경보 취소 MESSAGE 유실 시의 탈출구 — 개시자 자신은 cancelEmergency 로만 해제한다. */
    fun dismissEmergency(groupId: String) {
        synchronized(c.lock) {
            c.sessionMap[groupId]?.takeIf { it.emergency && !it.emergencyMine }?.emergency = false
        }
        c.publish()
    }

    /** 수신 긴급경보·취소·그룹 긴급 통지(TS 24.379 §12.1.1.3 — 코어가 해석). */
    fun onAlert(a: EmergencyAlert) {
        if (a.self) return                          // 내 발신 에코(서버는 발신자 제외 — 방어)
        val gid = a.groupId.ifBlank { return }
        if (a.alertInd == 0) {
            // 경보 없는 그룹 긴급 통지(§12.1.1.3 3)·4)) — 참여 중인 세션의 긴급 표시만 맞춘다.
            if (a.emergencyInd != 0) applySessionEmergency(gid, a.emergencyInd > 0, peer = a.userId)
            return
        }
        val user = a.userId
        if (a.alertInd > 0) {
            addAlert(ActiveAlert(gid, user, System.currentTimeMillis(), mine = false))
            c.feedback?.emergencyTone()
            c._status.value = "🚨 [$gid] 긴급경보 수신 — $user"
            c.emit(PttEventKind.ALERT_IN, gid, peer = user)
        } else {
            // 제3자 취소는 원 발신자(originated-by)의 경보를 지운다(§12.1.1.2 4)e))
            removeAlert(gid, a.originatedBy.ifBlank { user })
            // 세션 긴급 표시 un-latch — 정본 신호는 CSP 의 하향 재광고 re-INVITE(코어 ADVERTISED)이고, 경보 취소 정합은 재광고
            //   유실 대비 보조 경로다(§4.3). 같은 그룹에 다른 활성 경보가 남아 있으면 유지한다.
            if (a.emergencyInd < 0 || c._alerts.value.none { it.groupId == gid }) applySessionEmergency(gid, false, peer = null)
            c._status.value = "[$gid] 긴급경보 해제 — $user"
            c.emit(PttEventKind.ALERT_END, gid, peer = user)
        }
        c.publish()
    }

    /** 세션 조건 변화(코어) — 상향·하향의 결과와 서버 재광고. 개시자 자신은 자기 취소로만 해제한다(서버 하향 전파는 actor 를
     *  제외하므로 여기 오는 하향은 타인의 것 — 방어 가드). */
    fun onCondition(ch: ConditionChange) {
        val s = c.sessionByCall(ch.call.callId) ?: return
        val cond = ch.call.condition
        when (ch.cause) {
            ConditionCause.LOCAL, ConditionCause.CONFIRMED -> Unit          // 앱이 보낼 때 이미 반영했다
            ConditionCause.DENIED -> {
                // 미인가 상향(403 + emergency-ind=false, TS 24.379 §6.3.3.1.14) — 코어가 이전 값으로 되돌렸다. 재-INVITE 거절은
                //   통화를 끊지 않는다. 선발신된 경보는 별개 게이트라 프로파일 재조회로 미인가가 판명될 때만 회수한다.
                val wasUpgrade = s.emergency && !cond.emergency
                s.emergency = cond.emergency
                s.emergencyMine = cond.emergency && cond.mine
                if (wasUpgrade) {
                    c._status.value = "[${s.groupId}] 긴급 상향 미인가"
                    reconcileAlertAfterDenied(s.groupId)
                } else c._status.value = "[${s.groupId}] 긴급 해제 거절 (${cond.lastCode})"
            }
            ConditionCause.ADVERTISED -> applySessionEmergency(s.groupId, cond.emergency, peer = null)
        }
        c.publish()
    }

    /** 세션 긴급 표시 latch/un-latch — 서버 재광고·경보 통지 공용. 개시자 표시는 자기 해제로만 내린다. */
    private fun applySessionEmergency(groupId: String, on: Boolean, peer: String?) {
        val s = synchronized(c.lock) { c.sessionMap[groupId] } ?: return
        if (on) {
            if (s.emergency) return
            s.emergency = true
            c.feedback?.emergencyTone()
            c._status.value = "🚨 [${s.groupId}] 긴급 통화"
            c.emit(PttEventKind.EMERGENCY_IN, s.groupId, peer = peer)
        } else {
            if (!s.emergency || s.emergencyMine) return
            s.emergency = false
            c._status.value = "[${s.groupId}] 긴급 해제"
            c.emit(PttEventKind.EMERGENCY_END, s.groupId)
        }
        c.publish()
    }

    private companion object {
        /** SOS 가 프로파일 취득을 기다리는 상한 — 넘으면 가진 정보(선택 그룹)로 개시한다. */
        const val PROFILE_WAIT_MS = 3000L
    }
}
