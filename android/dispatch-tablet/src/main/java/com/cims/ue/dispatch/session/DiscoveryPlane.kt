// 관제 세션의 발견 평면 — 편성·그룹 문서가 서버에서 바뀌는 것을 따라간다 (android_dispatch_tablet.md §6.7, android_ue_provisioning.md §3)
//
// 관제석은 며칠씩 떠 있다. 그 사이 운영자가 그룹원을 옮기고 그룹을 만들고 지운다 — 재로그인해야 보이면 관제사는 낡은 편성으로
// 일한다. 통로는 둘이다: 그룹 문서는 서버가 밀어 준다(xcap-diff NOTIFY, RFC 5875), 관제 편성(`/provisioning/me` 의 dispatch)은
// 앱이 60초마다 묻는다(ETag — 안 바뀌었으면 304).
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CscClient
import com.cims.ue.sdk.DispatchProfile
import com.cims.ue.sdk.SipMessage
import kotlinx.coroutines.delay

/** 연속 xcap-diff 통지를 합치는 창 — 그룹 하나를 고치면 문서 여러 장의 통지가 잇달아 온다. */
private const val GROUP_REFRESH_DEBOUNCE_MS = 500L

/** GMS PSI 에 xcap-diff 를 구독한다 — 서버발 그룹 변경(`GROUP_CHANGED`)을 받아 목록을 자동으로 다시 받는다. */
internal suspend fun DispatchSession.subscribeGroupChanges() {
    val ptt = pttAccount ?: return
    val domain = profile.value?.pttService?.domain.orEmpty().ifEmpty { return }
    val r = ptt.subscribeXcapDiff("sip:gms_psi@$domain", true)
    if (!r.ok) android.util.Log.w("DispatchSession", "xcap-diff subscribe: ${r.code} ${r.reason}")
}

/** 이 NOTIFY 가 그룹 문서 변경인가 — xcap-diff 본문이 GMS AUID(`org.openmobilealliance.groups`)를 가리킬 때만. */
internal fun isGroupDocChange(contentType: String, body: String): Boolean =
    contentType.contains("xcap-diff", ignoreCase = true) && body.contains("org.openmobilealliance.groups")

/** xcap-diff NOTIFY(GMS 축) → 그룹 목록 재조회. 연속 통지는 [GROUP_REFRESH_DEBOUNCE_MS] 로 합친다 — 마지막 통지만 조회를 건다. */
internal fun DispatchSession.applyXcapDiff(m: SipMessage) {
    if (!isGroupDocChange(m.contentType, m.body)) return
    val seq = ++groupRefreshSeq
    scopeLaunch {
        delay(GROUP_REFRESH_DEBOUNCE_MS)
        if (seq != groupRefreshSeq || pttAccount == null) return@scopeLaunch
        android.util.Log.i("DispatchSession", "xcap-diff: group document changed — refreshing")
        refreshGroups()
    }
}

/**
 * 관제 편성이 바뀌었나 — 그룹원(회선·그룹·이름), 청취 대상 그룹, 범위·대표번호.
 *
 * 순서는 보지 않는다(집합 비교). 이름까지 보는 것은 그룹원 띠의 표시가 이름이기 때문이다.
 */
internal fun dispatchChanged(old: DispatchProfile, new: DispatchProfile): Boolean {
    fun members(d: DispatchProfile) = d.members.map { "${it.volteAor}|${it.groupId}|${it.name}" }.toSet()
    fun targets(d: DispatchProfile) = d.pttTargets.map { it.id }.toSet()
    return members(old) != members(new) || targets(old) != targets(new) ||
        old.monitorScope != new.monitorScope || old.pttListen != new.pttListen ||
        old.groupId != new.groupId || old.pilotId != new.pilotId ||
        old.listenVisibility != new.listenVisibility          // 은닉/투명 라벨이 역할 변경을 따라간다
}

/**
 * 관제 편성 재조회 — `/provisioning/me` 를 `If-None-Match` 로 묻는다. 304 면 끝, 바뀌었으면 프로파일을 갈아 끼우고
 * **바뀐 것만** dialog 감시와 conference 구독에 다시 적용한다(데스크톱 `RefreshDispatchAsync`).
 */
internal suspend fun DispatchSession.refreshDispatch() {
    val c = cscOrNull() ?: return
    val before = profile.value ?: return
    val token = accessToken() ?: return
    val gen = loginGeneration.value
    val r = c.xcapGet(token, "/provisioning/me", "application/json", profileEtag)
    val doc = r.value
    if (!r.ok || doc == null) {
        android.util.Log.w("DispatchSession", "provisioning/me poll: ${r.code} ${r.reason}")
        return
    }
    if (loginGeneration.value != gen) return
    if (doc.notModified) return retryWatch()             // 편성은 그대로 — 걸리지 못한 감시 구독만 다시 건다
    profileEtag = doc.etag
    val next = CscClient.parseProfile(doc.body) ?: run {
        android.util.Log.w("DispatchSession", "provisioning/me poll: bad profile")
        return
    }
    if (!dispatchChanged(before.dispatch, next.dispatch)) {
        // 편성은 같아도 서비스 능력(외부망 문자 게이트 `smsGateway`)은 바뀔 수 있다 — 프로파일만 갈아 끼운다
        if (before.services != next.services) setProfile(next)
        return retryWatch()
    }
    android.util.Log.i("DispatchSession", "dispatch discovery changed: members ${before.dispatch.members.size}→${next.dispatch.members.size} " +
        "pttTargets ${before.dispatch.pttTargets.size}→${next.dispatch.pttTargets.size} scope ${next.dispatch.monitorScope}/${next.dispatch.pttListen}")
    setProfile(next)
    if (phoneAccount != null) rewatch()                 // 데스크가 빠졌으면 대상이 비어 전부 푼다
    if (loginGeneration.value != gen) return            // 구독을 고치는 사이 로그아웃
    if (pttAccount != null) refreshGroups()
    if (loginGeneration.value != gen) return
    notify(NoticeLevel.INFO, "관제 편성이 바뀌었습니다",
        "그룹원 ${next.dispatch.members.size} · 감시 ${watchedTargets().size} · 청취 그룹 ${groups.value.count { !it.isMember }}")
}
