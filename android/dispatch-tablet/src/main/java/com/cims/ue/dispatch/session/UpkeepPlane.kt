// 관제 세션의 유지 평면 — 등록에 묶인 서버 상태(제휴·구독)를 지킨다 (android_dispatch_tablet.md §6.7a)
//
// 서버는 등록이 사라지면 그 가입자의 제휴(affiliation)를 전부 내린다(TS 24.379 §9 — 제휴는 등록에 묶인다). 등록이 사라지는 것은
// 해지 REGISTER 만이 아니다 — 등록 만료, 그리고 TCP/TLS 연결이 끊긴 바인딩의 회수(registration_binding_set.md §4 계기 2)도 같다.
// 망이 끊겼다 돌아오면 코어가 다시 등록하지만(`Engine::handleNetworkChange`), 제휴 PUBLISH 와 구독 SUBSCRIBE 는 코어가 **한 번
// 보낼 뿐** 유지하지 않는다(ue_sdk.md §4.2 — 목표 집합·재시도는 앱). 다시 싣지 않으면 편성 그룹의 [참여] 가 403(Warning 120 —
// 미제휴)으로 거절되고 로스터·회선 감시·그룹 변경 통지도 오지 않는다. 앱을 다시 켜야만 풀린다.
//
// 그 둘의 수명은 3600 초다(코어가 요청하는 값 = 서버 상한) — 끊기지 않아도 한 시간 뒤에는 사라진다. 수명 절반마다 다시 싣는다
// (RFC 3903 §4.1 게시 갱신 · RFC 6665 §4.1.2.2 구독 갱신).
//
// 계기는 넷이다.
//   ① 등록이 끊겼다 다시 섰다(등록 이벤트 기준 — 상태 스냅샷은 같은 값을 합쳐 Registered→Registered 를 못 본다)
//   ② 망이 바뀐 뒤의 첫 등록 성공 — 등록 상태는 그대로 «등록됨» 으로만 보여도 서버의 바인딩은 새것일 수 있다
//   ③ 수명 절반 경과(1분 틱)
//   ④ [참여] 가 403 으로 거절됨 — 서버가 말해 준 어긋남이다. 제휴를 다시 싣고 한 번 더 건다([rejoinsAfterAffiliation])
package com.cims.ue.dispatch.session

import android.os.SystemClock
import com.cims.ue.sdk.Account
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.RegInfo
import com.cims.ue.sdk.RegState
import com.cims.ue.sdk.RequestResult
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeoutOrNull

private const val TAG = "DispatchUpkeep"

/** 유지 평면의 순수 규칙 — 세션 없이 시험한다(`UpkeepTest`). */
object UpkeepRules {
    /** 제휴 PUBLISH·구독 SUBSCRIBE 의 수명(초) — 코어가 요청하는 값이자 서버 상한(CSP `SUBSCRIBE_MAX_EXPIRES_SEC`). */
    const val LIFETIME_SEC = 3600
    /** 갱신 시각을 보는 주기. */
    const val TICK_MS = 60_000L
    /** 제휴 PUBLISH 의 최종 응답을 기다리는 시한 — 넘기면 응답 없음으로 보고 다음 틱에 다시 싣는다. */
    const val CONFIRM_TIMEOUT_MS = 8_000L
    /** 같은 그룹의 [참여] 를 제휴 재적재 뒤 다시 거는 최소 간격 — 다시 건 것까지 403 이면 그대로 알린다(되풀이하지 않는다). */
    const val REJOIN_GAP_MS = 10_000L
    private const val RETRY_BASE_MS = 60_000L
    private const val RETRY_MAX_MS = 30 * 60_000L

    /** 다시 실을 때인가 — 실은 적이 없거나([atMs] 0) 수명 절반이 지났다. */
    fun due(atMs: Long, nowMs: Long, lifetimeSec: Int = LIFETIME_SEC): Boolean =
        atMs <= 0L || nowMs - atMs >= lifetimeSec * 500L

    /**
     * 이 등록 이벤트로 등록에 묶인 상태를 다시 세워야 하나.
     *
     * @param was 이 계정의 앞선 등록 이벤트가 «등록됨» 이었나(null = 처음 — 로그인 절차가 처음 것을 건다)
     * @param networkChanged 망이 바뀐 뒤 이 계정의 등록 결과를 아직 보지 못했다
     */
    fun renewed(was: Boolean?, registered: Boolean, networkChanged: Boolean): Boolean =
        registered && (was == false || networkChanged)

    /** 거절·응답 없음 뒤 다음 시도까지 — 1분부터 배로, 최대 30분(멤버가 아닌 그룹에 매 분 PUBLISH 를 보내지 않는다). */
    fun retryDelayMs(failures: Int): Long =
        (RETRY_BASE_MS shl (failures - 1).coerceIn(0, 5)).coerceAtMost(RETRY_MAX_MS)

    /**
     * 거절된 [참여] 를 제휴 재적재 뒤 다시 걸 것인가 — 403 이고 내가 멤버인 그룹이며, 방금 그렇게 다시 건 호가 아닐 때.
     * 편성 그룹의 미제휴 거절은 403 + Warning 120(TS 24.379 §10.1.1.4.2)인데 비멤버 거절도 403 이다 — 코어가 Warning 을
     * 올리지 않으므로 목록(GMS)의 멤버십으로 가른다.
     */
    fun rejoin(code: Int, member: Boolean, lastRejoinAtMs: Long, nowMs: Long): Boolean =
        code == 403 && member && (lastRejoinAtMs <= 0L || nowMs - lastRejoinAtMs >= REJOIN_GAP_MS)
}

/** 유지 평면의 상태. 메인 스레드 전용(세션 스코프가 `Dispatchers.Main.immediate`). */
internal class UpkeepState {
    /** 맞춤은 한 줄에서 — 요청이 몰려도 마지막 상태로 한 번(영상 평면의 `kick` 과 같은 규칙). */
    val kick = Channel<Unit>(Channel.CONFLATED)
    /** 계정별 — 마지막 등록 이벤트가 «등록됨» 이었나. */
    val registered = HashMap<Int, Boolean>()
    /** 망이 바뀐 뒤 등록 결과를 아직 보지 못한 계정. */
    val networkChanged = HashSet<Int>()
    /** 등록이 새로 섰다 — 다시 세울 것이 남았다(계정이 등록된 때의 맞춤이 거둔다). */
    var pttOwed = false
    var phoneOwed = false

    /** 그룹 하나의 제휴 — 서버가 2xx 로 받은 시각(0 = 확인 없음)과 거절·무응답 뒤의 물러남. */
    class Aff(var atMs: Long = 0L, var failures: Int = 0, var retryAtMs: Long = 0L)
    val aff = HashMap<String, Aff>()
    /** 제휴 PUBLISH 의 token → 최종 응답을 기다리는 쪽(`applyRequestResult` 가 채운다). */
    val waiters = HashMap<Long, CompletableDeferred<RequestResult>>()
    /** conference·xcap-diff 구독을 건 시각 / 회선 감시(dialog) 구독을 건 시각. */
    var subsAtMs = 0L
    var watchAtMs = 0L
    /** 그룹별 — 제휴 재적재 뒤 [참여] 를 다시 건 시각. */
    val rejoinAtMs = HashMap<String, Long>()

    fun reset() {
        registered.clear(); networkChanged.clear(); pttOwed = false; phoneOwed = false
        aff.clear(); rejoinAtMs.clear(); subsAtMs = 0L; watchAtMs = 0L
        waiters.clear()                 // 기다리던 쪽은 시한으로 풀린다 — 취소하면 그 코루틴(맞춤 한 줄)이 함께 끝난다
    }
}

/** 수명·간격의 시계 — 잠든 시간도 센다(서버의 수명은 단말이 자는 동안에도 흐른다). */
private fun now(): Long = SystemClock.elapsedRealtime()

/** 엔진이 선 뒤 한 번 — 등록 이벤트·1분 틱을 맞춤 한 줄로 모은다. */
internal fun DispatchSession.observeUpkeep(engine: CimsUe) {
    fun launch(what: String, block: suspend () -> Unit) = scopeLaunch {
        try { block() } catch (e: CancellationException) { throw e } catch (t: Throwable) { android.util.Log.w(TAG, what, t) }
    }
    launch("regState") { engine.regState.collect { r -> runCatching { applyRegEvent(r) }.onFailure { android.util.Log.w(TAG, "regState", it) } } }
    scopeLaunch {
        for (u in upkeep.kick) {
            try { upkeepPass() } catch (e: CancellationException) { throw e } catch (t: Throwable) { android.util.Log.w(TAG, "pass", t) }
        }
    }
    launch("tick") { while (true) { delay(UpkeepRules.TICK_MS); upkeep.kick.trySend(Unit) } }
}

/** 로그인 절차가 처음 구독을 건다 — 그 시각을 갱신의 기준으로 잡는다(곧바로 한 번 더 걸지 않게). */
internal fun DispatchSession.beginUpkeep() {
    val t = now()
    upkeep.subsAtMs = t
    upkeep.watchAtMs = t
}

/** 망이 바뀌었다(`handleNetworkChange`) — 다음 등록 성공에 등록에 묶인 상태를 다시 세운다. */
internal fun DispatchSession.noteNetworkChanged() {
    listOfNotNull(pttAccount, phoneAccount).forEach { upkeep.networkChanged.add(it.id) }
}

/** 등록 이벤트(REGISTER 응답마다 — 갱신 포함) → 다시 세울 것이 생겼나. */
internal fun DispatchSession.applyRegEvent(r: RegInfo) {
    val st = upkeep
    val registered = r.state == RegState.REGISTERED
    val was = st.registered.put(r.accountId, registered)
    val changed = registered && st.networkChanged.remove(r.accountId)
    if (!UpkeepRules.renewed(was, registered, changed)) return
    when (r.accountId) {
        pttAccount?.id -> st.pttOwed = true
        phoneAccount?.id -> st.phoneOwed = true
        else -> return
    }
    st.kick.trySend(Unit)
}

private fun DispatchSession.registeredNow(a: Account): Boolean = registrations.value[a.id]?.state == RegState.REGISTERED

/**
 * 한 번의 맞춤 — 등록된 계정마다 낡은 것만 다시 싣는다: 멤버 그룹의 제휴(확인 없음·수명 절반), conference·xcap-diff 구독,
 * 회선 감시 구독. 등록이 새로 선 계정은 전부 낡은 것으로 본다. 등록 전이면 남겨 둔다 — 등록 이벤트가 다시 부른다.
 */
internal suspend fun DispatchSession.upkeepPass() {
    if (!isReady) return
    val st = upkeep
    val gen = loginGeneration.value
    pttAccount?.takeIf { registeredNow(it) }?.let { ptt ->
        if (st.pttOwed) {
            st.pttOwed = false
            st.aff.values.forEach { it.atMs = 0L; it.retryAtMs = 0L }
            st.subsAtMs = 0L
            renewVideoAffiliations()                     // MCVideo 제휴도 등록에 묶여 있다(§6.14)
            android.util.Log.i(TAG, "ptt registration renewed — re-affiliating, re-subscribing")
        }
        affiliateDue(ptt, gen)
        if (gen != loginGeneration.value) return
        if (UpkeepRules.due(st.subsAtMs, now())) {
            for (g in groups.value) {
                if (gen != loginGeneration.value) return
                val r = ptt.subscribeConference(g.id, true)
                if (!r.ok) android.util.Log.w(TAG, "conference subscribe ${g.id}: ${r.code} ${r.reason}")
            }
            subscribeGroupChanges()
            st.subsAtMs = now()
        }
    }
    if (gen != loginGeneration.value) return
    phoneAccount?.takeIf { registeredNow(it) }?.let {
        if (st.phoneOwed) {
            st.phoneOwed = false
            st.watchAtMs = 0L
            android.util.Log.i(TAG, "phone registration renewed — re-subscribing dialog watch")
        }
        if (hasDesk && UpkeepRules.due(st.watchAtMs, now())) {
            watchAll()
            st.watchAtMs = now()
        }
    }
}

/** 멤버 그룹 가운데 제휴가 낡은 것(확인 없음·수명 절반)을 다시 싣는다. 응답이 없으면 거기서 멈춘다 — 망이 없는 것이다. */
private suspend fun DispatchSession.affiliateDue(ptt: Account, gen: Int) {
    val st = upkeep
    for (g in groups.value.filter { it.isMember }) {
        if (gen != loginGeneration.value) return
        val a = st.aff.getOrPut(g.id) { UpkeepState.Aff() }
        val t = now()
        if (!UpkeepRules.due(a.atMs, t) || a.retryAtMs > t) continue
        if (affiliateConfirmed(ptt, g.id) <= 0) return
    }
}

/**
 * 제휴 PUBLISH 한 건을 보내고 **최종 응답까지** 본다 — 명령이 받아들여진 것은 제휴가 선 것이 아니다(서버가 거절하면 그룹콜이
 * 오지 않고 [참여] 가 403 이다). 2xx 만 제휴로 적는다.
 *
 * @return 최종 응답 코드. 0 = 응답 없음(시한), -1 = 명령 실패
 */
internal suspend fun DispatchSession.affiliateConfirmed(ptt: Account, id: String): Int {
    val st = upkeep
    val a = st.aff.getOrPut(id) { UpkeepState.Aff() }
    val (r, early) = sendTracked({ it }) { ptt.affiliate(id, true) }
    val token = r.value
    var code = -1
    var reason = r.reason
    if (r.ok && token != null) {
        val res = early ?: run {
            val w = CompletableDeferred<RequestResult>()
            st.waiters[token] = w
            try { withTimeoutOrNull(UpkeepRules.CONFIRM_TIMEOUT_MS) { w.await() } } finally { st.waiters.remove(token) }
        }
        code = res?.code ?: 0
        reason = res?.reason ?: "no answer"
    }
    val ok = code in 200..299
    val t = now()
    if (ok) { a.atMs = t; a.failures = 0; a.retryAtMs = 0L }
    else {
        a.atMs = 0L; a.failures++; a.retryAtMs = t + UpkeepRules.retryDelayMs(a.failures)
        android.util.Log.w(TAG, "affiliate $id: $code $reason — retry in ${UpkeepRules.retryDelayMs(a.failures) / 1000}s")
    }
    updateGroup(id) { it.copy(affiliated = ok) }
    return code
}

/** 내 채널에서 빠진 그룹 — 유지 대상에서 뺀다. */
internal fun DispatchSession.forgetUpkeep(groupId: String) {
    upkeep.aff.remove(groupId)
    upkeep.rejoinAtMs.remove(groupId)
}

/**
 * 연결되지 못하고 403 으로 끝난 [참여] — 멤버 그룹이면 제휴를 다시 싣고 **한 번 더** 건다. 서버가 등록을 새것으로 보는 사이
 * (연결이 끊긴 바인딩의 회수·등록 만료) 제휴가 내려갔는데 단말은 등록이 이어진 것으로만 보는 경우가 있다 — 등록 이벤트로는
 * 알 수 없고 이 거절이 유일한 신호다. 다시 걸었으면 true — 이 끝은 실패로 알리지 않는다(다시 건 호의 결과가 알린다).
 *
 * 일제 통화 개시는 누르는 동안만 유효한 호라 다시 걸지 않고 제휴만 다시 싣는다(다음 누름이 성립한다). 긴급 참여는 서버가
 * 암묵적으로 제휴시키므로(TS 24.379 §9.2.2.3.7) 대상이 아니다.
 */
internal fun DispatchSession.rejoinsAfterAffiliation(s: SessionItem, c: CallInfo): Boolean {
    if (s.connectedAtMs != null || c.dir != CallDir.OUTGOING || s.operation != Operation.PTT_JOIN) return false
    val gid = s.info.groupId
    val st = upkeep
    val t = now()
    val member = groups.value.any { it.id == gid && it.isMember }
    if (!UpkeepRules.rejoin(c.lastCode, member, st.rejoinAtMs[gid] ?: 0L, t)) return false
    val ptt = pttAccount ?: return false
    val gen = loginGeneration.value
    if (c.callId in broadcastPending) {
        scopeLaunch { affiliateConfirmed(ptt, gid) }
        return false
    }
    st.rejoinAtMs[gid] = t
    scopeLaunch {
        val code = affiliateConfirmed(ptt, gid)
        if (gen != loginGeneration.value) return@scopeLaunch
        // 제휴도 서지 않았다 — 정말 멤버가 아니거나 서버에 닿지 않는다. 처음의 거절을 그대로 알린다.
        if (code !in 200..299) { report(TextArea.PTT_JOIN, CimsResult.fail<Unit>(c.lastCode, c.lastReason)); return@scopeLaunch }
        android.util.Log.i(TAG, "join $gid 403 — re-affiliated, joining again")
        joinGroup(gid)
    }
    return true
}
