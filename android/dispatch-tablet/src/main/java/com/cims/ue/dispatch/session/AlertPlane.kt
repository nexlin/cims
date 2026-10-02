// 관제 세션의 긴급 평면 — 긴급 경보·조건 해제 자격·미응답 멤버 (android_dispatch_tablet.md §6.2a-1, dispatch_desktop_ui.md §3.2)
//
// 긴급에는 신호가 둘 있다. **세션 조건**(긴급·임박 — mcptt-info `emergency-ind`/`imminentperil-ind`)은 그룹 호의 속성이라
// 세션 스냅샷에서 파생하고(`DispatchSession.alerts`), **긴급 경보**(SIP MESSAGE `alert-ind`, TS 24.379 §12.1)는 세션이 없어도
// 오는 별개 신호라 여기서 배너 목록으로 든다. 둘 다 판단은 서버에 있다 — 앱은 자격으로 버튼을 미리 막고, 거절은 문장으로 알린다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.Capabilities
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.EmergencyAlert
import com.cims.ue.sdk.RequestResult

/** 긴급 경보 배너 한 장 — **그룹·발신자마다 하나**. 발신자의 취소(또는 제3자 취소의 originated-by)로 내린다. */
data class EmergencyAlertBanner(
    val groupId: String,
    /** 경보 발신자의 번호부(bare MCPTT ID) — 같은 그룹·같은 발신자의 취소와 맞물리는 열쇠. */
    val user: String,
    val groupName: String,
    /** 발신자 표시(번호 이름 병기 + 기관). */
    val userLabel: String,
    val sinceMs: Long,
) {
    val key: String get() = "$groupId|$user"
}

/** 경보 하나가 배너 목록을 어떻게 바꾸나. */
internal sealed interface AlertEffect {
    data object None : AlertEffect
    /** 새 경보 — 같은 그룹·같은 발신자의 앞 배너를 갈아 끼운다(경과를 다시 센다). */
    data class Raised(val banner: EmergencyAlertBanner) : AlertEffect
    /** 취소 — [owner] 의 배너를 내린다. 제3자 취소면 [by] 가 취소한 사람이다(같으면 발신자 본인). */
    data class Cleared(val owner: String, val by: String) : AlertEffect
}

/**
 * 경보 수신의 규칙(TS 24.379 §12.1.1.3) — 순수 함수라 JVM 에서 시험한다.
 *
 * `alertInd` 1 = 경보, -1 = 취소, 0 = 경보 없는 그룹 상태 통지(배너와 무관). 내 경보의 에코(`self`)는 배너가 아니다.
 * 제3자 취소는 `originatedBy` 가 **원 경보 발신자**를 가리킨다 — 취소한 사람(`userId`)의 배너를 찾으면 안 내려간다.
 */
internal fun alertEffectOf(a: EmergencyAlert, groupName: String, userLabel: String, nowMs: Long): AlertEffect {
    if (a.self) return AlertEffect.None
    val user = userPart(a.userId)
    return when {
        a.alertInd > 0 -> AlertEffect.Raised(EmergencyAlertBanner(a.groupId, user, groupName, userLabel, nowMs))
        a.alertInd < 0 -> AlertEffect.Cleared(owner = userPart(a.originatedBy).ifEmpty { user }, by = user)
        else -> AlertEffect.None
    }
}

/** 효과를 목록에 적용 — 최신이 위. */
internal fun List<EmergencyAlertBanner>.applying(groupId: String, e: AlertEffect): List<EmergencyAlertBanner> = when (e) {
    AlertEffect.None -> this
    is AlertEffect.Raised -> listOf(e.banner) + filterNot { it.key == e.banner.key }
    is AlertEffect.Cleared -> filterNot { it.groupId == groupId && it.user == e.owner }
}

/** 긴급 경보·취소 수신 → 배너 + ⑤ 이벤트. */
internal fun DispatchSession.applyEmergencyAlert(a: EmergencyAlert) {
    val gname = groupNameOf(a.groupId)
    val label = displayLabel(a.userId) + if (a.mcOrg.isNotEmpty()) " (${a.mcOrg})" else ""
    android.util.Log.i("DispatchSession", "alert group=${a.groupId} user=${a.userId} by=${a.originatedBy} " +
        "alert=${a.alertInd} emg=${a.emergencyInd} peril=${a.imminentPerilInd} self=${a.self}")
    when (val e = alertEffectOf(a, gname, label, System.currentTimeMillis())) {
        AlertEffect.None -> return
        is AlertEffect.Raised -> {
            setAlertBanners(alertBanners.value.applying(a.groupId, e))
            addActivity(a.groupId, gname, "긴급 경보 · $label", ActivityKind.EMERGENCY, emergency = true)
        }
        is AlertEffect.Cleared -> {
            setAlertBanners(alertBanners.value.applying(a.groupId, e))
            val who = if (e.owner == e.by) displayLabel(a.userId)
                      else "${displayLabel(e.owner)} · 해제 ${displayLabel(a.userId)}"
            addActivity(a.groupId, gname, "긴급 경보 해제 · $who", ActivityKind.EMERGENCY, emergency = true)
        }
    }
}

/**
 * 배너 [경보 해제] — 경보 취소 MESSAGE(남의 경보 = 제3자 취소, TS 24.379 §12.1.1.2 4)e)).
 *
 * 서버는 발신자에게 취소를 되돌려 주지 않는다 — 요청으로 배너를 내리고, 최종 응답이 403(미인가 — 서버는 `alert-ind` true 로
 * 경보 유지를 알린다, §12.1.3.2)이면 [applyAlertCancelResult] 가 되살린다.
 */
suspend fun DispatchSession.cancelAlert(b: EmergencyAlertBanner): CimsResult<Unit> {
    val ptt = pttAccount ?: return report(TextArea.ALERT_CANCEL, CimsResult.fail(-1, "PTT 계정 없음"))
    val (r, early) = sendTracked({ it }) { ptt.sendEmergencyAlert(b.groupId, activate = false, originatedBy = b.user) }
    val token = r.value
    if (!r.ok || token == null) return report(TextArea.ALERT_CANCEL, CimsResult.fail(r.code, r.reason))
    setAlertBanners(alertBanners.value.filterNot { it.key == b.key })
    addActivity(b.groupId, b.groupName, "긴급 경보 해제 요청 · ${b.userLabel}", ActivityKind.EMERGENCY, emergency = true)
    // 최종 응답이 명령보다 먼저 와 있었으면 곧바로 맞추고, 아니면 token 을 걸어 둔다(`TokenLedger`).
    if (early != null) applyAlertCancelResult(b, early) else alertCancelTokens[token] = b
    return CimsResult.ok(Unit)
}

/**
 * 경보 취소의 최종 응답 — 표시를 서버 판정에 맞춘다. 403 이면 배너를 되살리고 이유를 알린다. 전송 실패·시한은 서버 판정을
 * 모르므로 표시는 그대로 두고 알리기만 한다(데스크톱 `OnAlertCancelResult` 와 같은 규칙).
 */
internal fun DispatchSession.applyAlertCancelResult(b: EmergencyAlertBanner, r: RequestResult) {
    if (r.code in 200..299) return
    android.util.Log.w("DispatchSession", "alert cancel ${b.key}: ${r.code} ${r.reason}")
    if (r.code == 403) {
        if (alertBanners.value.none { it.key == b.key }) setAlertBanners(listOf(b) + alertBanners.value)
        addActivity(b.groupId, b.groupName, "긴급 경보 해제 거절 · ${b.userLabel}", ActivityKind.ERROR, emergency = true)
    }
    notify(NoticeLevel.ERROR, "${b.groupName} — ${ResponseText.sip(TextArea.ALERT_CANCEL, r.code, r.reason)}",
        "경보 취소 MESSAGE ${r.code} ${r.reason}".trim())
}

/** 경보 배너 [닫기] — 이 화면의 표시만 내린다(취소 신호를 놓쳤을 때의 탈출구). 서버의 경보 상태는 그대로다. */
fun DispatchSession.dismissAlert(b: EmergencyAlertBanner) {
    setAlertBanners(alertBanners.value.filterNot { it.key == b.key })
    android.util.Log.i("DispatchSession", "alert banner dismissed ${b.key}")
}

/**
 * 조건 하향(긴급·임박 해제)을 서버가 받는가 — 서버 판정과 같은 식이다.
 *
 * 긴급 = 내가 올린 조건 ∨ user profile `allow-cancel-group-emergency`(TS 24.379 §6.2.8.1.7·§6.3.3.1.13.4), 임박 =
 * `allow-cancel-imminent-peril`(개시자 예외 없음, §6.2.8.1.10·§6.3.3.1.13.6). 받지 못한 user profile 은 허용으로 읽는다
 * ([Capabilities] 규약) — 서버가 403 으로 거절하면 코어가 이전 값으로 되돌리고 `applyCondition` 이 해제 거절 문구를 낸다.
 * 청취 leg 은 조건을 바꾸지 않는다. 진행 중(pending)인 변경 위에 또 보내지 않는다.
 */
internal fun canCancelCondition(s: SessionItem, caps: Capabilities): Boolean {
    if (!s.isLive || !s.isActive || s.info.listenOnly || s.info.condition.pending) return false
    return when {
        s.isEmergency -> s.info.condition.mine || caps.cancelGroupEmergency
        s.isImminentPeril -> caps.cancelImminentPeril
        else -> false
    }
}

/**
 * 배너·채널 상세의 [긴급 해제]·[임박 해제] — 세션 조건 하향(TS 24.379 §10.1.1.2.1.4·§10.1.1.2.1.5 — `emergency-ind`·
 * `imminentperil-ind` false re-INVITE). 자격이 없으면 보내지 않고 이유를 말한다.
 */
suspend fun DispatchSession.cancelCondition(callId: Int): CimsResult<Unit> {
    val s = sessionOf(callId) ?: return report(TextArea.EMERGENCY_CANCEL, CimsResult.fail(-1, "세션이 끝났습니다"))
    if (!canCancelCondition(s, capabilities.value)) {
        val title = s.title.ifEmpty { groupNameOf(s.info.groupId) }
        return report(TextArea.EMERGENCY_CANCEL, CimsResult.fail(-1,
            if (s.isEmergency) "$title — 긴급을 해제할 수 없습니다 (내가 올린 긴급이 아니고 해제 권한 allow-cancel-group-emergency 가 없습니다)"
            else "$title — 임박 위험 해제 권한이 없습니다 (user profile allow-cancel-imminent-peril)"))
    }
    val kind = if (s.isEmergency) "긴급" else "임박"
    // 요청 줄을 **먼저** 남긴다 — 명령은 suspend 라 그 사이에 결과(«긴급 해제»)가 먼저 도착하고, 뒤에 적으면 ⑤ 의 순서가 뒤집힌다.
    addActivity(s.info.groupId, channelNameOf(s), "$kind 해제 요청", ActivityKind.EMERGENCY, emergency = true)
    return setEmergency(callId, false)
}

/**
 * 내가 연 그룹 통화에 필수 멤버가 응답하지 않은 채 진행됐다(TS 24.379 §6.3.3.3 — 서버 INFO `<non-acknowledged-user>`, 개시자
 * 프로파일 allow-to-receive-non-acknowledged-users-information 일 때만 온다). ⑤ + 토스트 — 누가 듣지 못하는지 관제사가 알아야 한다.
 */
internal fun DispatchSession.applyNonAcknowledged(ci: CallInfo) {
    val s = sessionOf(ci.callId) ?: return
    val users = ci.nonAcknowledgedUsers
    if (users.isEmpty()) return
    val names = users.joinToString(", ") { displayName(it) }
    val title = s.title.ifEmpty { groupNameOf(s.info.groupId) }
    android.util.Log.i("DispatchSession", "non-acknowledged #${ci.callId} ${users.joinToString(",")}")
    addActivity(s.info.groupId, channelNameOf(s), "미응답 멤버 ${users.size}명 · $names", ActivityKind.JOIN, s.isEmergency)
    notify(NoticeLevel.WARN, "$title — 필수 멤버 ${users.size}명이 응답하지 않은 채 통화가 열렸습니다", names)
}

/** 개시 200 OK 의 `P-Answer-State: Unconfirmed` 인가(RFC 4964) — 내가 건 호가 성립했고 아직 적지 않았을 때만. */
internal fun unconfirmedAnswer(s: SessionItem): Boolean =
    !s.answerStateNoted && s.isActive && s.info.dir == CallDir.OUTGOING && s.info.answerState.equals("Unconfirmed", ignoreCase = true)

/**
 * «멤버 확인 전 연결» — 서버가 멤버 확인 전에 개시를 받았다(첫 멤버가 붙기 전의 말은 서버가 담았다 재생한다, TS 24.379
 * §10.1.1.2.1.1 2A) "may indicate to the MCPTT user"). ⑤ 에 한 번 적는다.
 */
internal fun DispatchSession.noteAnswerState(callId: Int) {
    val s = sessionOf(callId) ?: return
    if (!unconfirmedAnswer(s)) return
    updateSession(callId) { it.copy(answerStateNoted = true) }
    addActivity(s.info.groupId, channelNameOf(s),
        "멤버 확인 전 연결 — 첫 멤버가 붙을 때까지 서버가 음성을 담았다 전한다", ActivityKind.NOTE, s.isEmergency)
}
