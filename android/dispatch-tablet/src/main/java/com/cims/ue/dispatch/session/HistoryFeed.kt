// 서버 통합 이력 폴링 — 관제 범위 안 **타인**의 PTT 세션·메시지를 ⑤ 이벤트·⑥ 통화내역에 합친다
//   (android_ue_provisioning.md §3-2 `GET /provisioning/history`, dispatch_desktop_ui.md §13)
//
// 진행 중 상태는 구독(dialog·conference)이 정본이라 이 폴링은 live 를 대체하지 않는다 — **끝난 것**을 수초 지연으로 채운다.
// 내가 당사자인 항목은 이미 로컬 행이 있으니 건너뛴다. 통화(`kind=call`)는 묻지 않는다 — 감시 대상의 통화는 dialog 이벤트가
// 끝나는 즉시 ⑥ 에 남기고(`applyDialog`), 같은 통화를 이력으로 또 받으면 두 줄이 된다.
//
// 서버가 이 API 를 내지 않거나(404·501·405) 범위가 없으면(403) 첫 탐침에서 조용히 꺼진다. 응답은 ETag 라 새 항목이 없으면 304 다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CscClient
import kotlinx.coroutines.delay
import org.json.JSONObject

/** 이력 한 건 — 폴링 병합이 읽는 공통 필드만(종류별 확장 필드는 [이력] 화면의 몫이다). */
internal data class HistoryFeedItem(
    val id: String, val atMs: Long, val kind: String, val event: String,
    val from: String, val to: String, val group: String,
    val durationSec: Int, val emergency: Boolean, val text: String,
)

internal object HistoryFeed {
    /** 폴링 주기 — 종류 둘(ptt·message)을 한 바퀴 돈 뒤 쉰다(데스크톱 `HistoryClient.DefaultIntervalMs`). */
    const val INTERVAL_MS = 2_500L
    /** CSC 에 닿지 않을 때의 백오프 상한. */
    const val MAX_BACKOFF_MS = 30_000L
    const val PAGE_LIMIT = 200
    /** 중복 제거 키를 이만큼만 기억한다 — 커서가 앞으로만 가므로 최근분이면 된다. */
    const val SEEN_LIMIT = 5_000
    val KINDS = listOf("ptt", "message")

    /** 탐침·폴링 응답이 «서버가 이 API 를 내지 않는다 / 범위 밖» 인가 — 그러면 폴링을 끈다(재시도하지 않는다). */
    fun unavailable(code: Int): Boolean = code == 404 || code == 501 || code == 405 || code == 403

    /** 백오프 — 닿지 않은 횟수만큼 두 배(최대 [MAX_BACKOFF_MS]), 닿으면 원래 주기. */
    fun delayMs(unreachable: Int): Long =
        if (unreachable <= 0) INTERVAL_MS else minOf(INTERVAL_MS shl minOf(unreachable, 6), MAX_BACKOFF_MS)

    /**
     * 응답 본문 → 항목(오래된 것부터)과 다음 커서. 모르는 필드는 무시하고, 필수(`id`·`time`)가 없는 항목은 건너뛴다.
     * 본문이 JSON 이 아니면 빈 결과다 — 한 번의 깨진 응답이 폴링을 멈추지 않는다.
     */
    fun parse(kind: String, json: String): Pair<List<HistoryFeedItem>, String> {
        val root = runCatching { JSONObject(json) }.getOrNull() ?: return emptyList<HistoryFeedItem>() to ""
        val next = root.str("next")
        val arr = root.optJSONArray("items") ?: return emptyList<HistoryFeedItem>() to next
        val items = ArrayList<HistoryFeedItem>(arr.length())
        for (i in 0 until arr.length()) {
            val it = arr.optJSONObject(i) ?: continue
            val id = it.str("id")
            val at = parseTime(it.str("time"))
            if (id.isEmpty() || at == null) continue
            items += HistoryFeedItem(id, at, it.str("kind").ifEmpty { kind }, it.str("event"),
                it.str("from"), it.str("to"), it.str("group"),
                it.optInt("duration"), it.optBoolean("emergency"), it.str("text"))
        }
        return items.sortedBy { it.atMs } to next
    }

    /** ISO8601(+offset 또는 naive-local) → epoch ms. 못 읽으면 null. */
    fun parseTime(s: String, zone: java.time.ZoneId = java.time.ZoneId.systemDefault()): Long? {
        if (s.isBlank()) return null
        runCatching { return java.time.OffsetDateTime.parse(s).toInstant().toEpochMilli() }
        runCatching { return java.time.LocalDateTime.parse(s).atZone(zone).toInstant().toEpochMilli() }
        return null
    }

    /** 이력 event 이름표 → 한 낱말(데스크톱 `HistoryEventText`). 모르는 이름은 그대로. */
    fun eventText(ev: String): String = when (ev) {
        "ptt.talk" -> "발언"
        "ptt.session.start" -> "세션 시작"
        "ptt.session.end" -> "세션 종료"
        "ptt.emergency" -> "긴급"
        "ptt.private" -> "개별 통화"
        "ptt.adhoc" -> "애드혹 그룹 통화"
        else -> ev
    }

    /** PTT 이력 event → ⑤ 이벤트 종류. */
    fun activityKind(ev: String): ActivityKind = when (ev) {
        "ptt.talk" -> ActivityKind.TALK
        "ptt.session.end" -> ActivityKind.LEAVE
        "ptt.emergency" -> ActivityKind.EMERGENCY
        else -> ActivityKind.JOIN
    }

    fun durationText(sec: Int): String = if (sec > 0) "%d:%02d".format(sec / 60, sec % 60) else ""
}

/** 폴링의 진행 상태 — 종류별 커서(since·ETag)와 이미 본 항목. 세션이 하나 들고 로그아웃 때 비운다. */
internal class HistoryFeedState {
    val cursor = HashMap<String, Pair<String, String>>()      // kind → (since, etag)
    val seen = HashSet<String>()
    var unreachable = 0
    fun clear() { cursor.clear(); seen.clear(); unreachable = 0 }
}

/**
 * 이력 폴링 한 벌 — 기동 뒤 한 번 부른다. 로그인 세대가 바뀌면(로그아웃) 스스로 끝난다.
 * 탐침에서 서버가 API 를 내지 않거나 범위가 없으면 돌지 않는다.
 */
internal suspend fun DispatchSession.runHistoryFeed() {
    if (!hasDesk) return
    val gen = loginGeneration.value
    val st = historyFeed
    st.clear()
    val probe = historyGet("/provisioning/history?kind=ptt&limit=1", "") ?: return
    if (!probe.ok && HistoryFeed.unavailable(probe.code)) {
        android.util.Log.i("DispatchSession", "history: server does not provide it (${probe.code} ${probe.reason}) — polling off")
        return
    }
    while (loginGeneration.value == gen && isReady) {
        for (kind in HistoryFeed.KINDS) {
            if (loginGeneration.value != gen) return
            if (!pollHistory(kind, st)) return
        }
        delay(HistoryFeed.delayMs(st.unreachable))
    }
}

/** GET 한 번 — 401 이면 토큰을 강제 갱신해 딱 한 번 다시 보낸다. 로그인 전이면 null(«요청 자체를 안 했음»). */
private suspend fun DispatchSession.historyGet(path: String, etag: String): com.cims.ue.sdk.CimsResult<com.cims.ue.sdk.XcapDoc>? {
    val c = cscOrNull() ?: return null
    val token = accessToken() ?: return null
    var r = c.xcapGet(token, path, "application/json", etag)
    if (!r.ok && r.code == 401) {
        val fresh = renewAccessToken()
        if (fresh != null && fresh != token) r = c.xcapGet(fresh, path, "application/json", etag)
    }
    return r
}

/** 종류 하나를 한 번 묻는다. @return false = 폴링을 끈다(서버가 내지 않음·범위 밖). */
private suspend fun DispatchSession.pollHistory(kind: String, st: HistoryFeedState): Boolean {
    val (since, etag) = st.cursor[kind] ?: ("" to "")
    val path = "/provisioning/history?kind=$kind&limit=${HistoryFeed.PAGE_LIMIT}" +
        if (since.isNotEmpty()) "&since=" + CscClient.urlEncode(since) else ""
    val gen = loginGeneration.value
    val r = historyGet(path, etag) ?: return true
    if (loginGeneration.value != gen) return false               // 받는 사이 로그아웃 — 앞 사람의 이력을 남기지 않는다
    val doc = r.value
    if (!r.ok || doc == null) {
        if (HistoryFeed.unavailable(r.code)) {
            android.util.Log.w("DispatchSession", "history $kind: ${r.code} — polling off")
            return false
        }
        // 닿지 않음·서버 오류·갱신해도 거절되는 401 — 어느 것이든 물러난다. 401 을 빼 두면 2.5초마다 토큰을 강제 갱신하며 돈다.
        if (st.unreachable++ == 0)
            android.util.Log.w("DispatchSession", "history $kind: ${r.code} ${r.reason} — backing off")
        return true
    }
    st.unreachable = 0
    if (doc.notModified) return true
    val (items, next) = HistoryFeed.parse(kind, doc.body)
    st.cursor[kind] = next.ifEmpty { since } to doc.etag
    items.forEach { if (st.seen.add(it.id)) applyHistoryItem(it) }
    if (st.seen.size > HistoryFeed.SEEN_LIMIT) st.seen.clear()
    return true
}

/** 내 회선이 당사자인가 — 전화·PTT 어느 번호든. */
private fun DispatchSession.isMyLine(uri: String): Boolean {
    val n = userPart(uri)
    return n.isNotEmpty() && DirectoryBook.normalize(n) in myLineKeys()
}

/**
 * 이력 항목 → ⑤ 이벤트(PTT 세션·그룹 SDS) 또는 ⑥ 통화내역(타인의 문자). 내가 당사자인 항목은 건너뛴다 — 로컬 행이 이미 있다.
 */
internal fun DispatchSession.applyHistoryItem(e: HistoryFeedItem) {
    if (isMyLine(e.from) || isMyLine(e.to)) return
    val from = if (e.from.isEmpty()) "" else displayName(e.from)
    when (e.kind) {
        "ptt" -> {
            val gid = userPart(e.group)
            // 내가 들어가 있는(청취 포함) 그룹의 발언은 내 세션의 floor 가 이미 ⑤ 에 적었다 — 같은 발언이 두 줄이 되지 않게 건너뛴다
            if (e.event == "ptt.talk" && sessions.value.any { it.isLive && it.info.isMcptt && it.info.groupId.equals(gid, ignoreCase = true) }) return
            val dur = HistoryFeed.durationText(e.durationSec)
            val text = listOf(HistoryFeed.eventText(e.event), from, dur).filter { it.isNotEmpty() }.joinToString(" · ")
            addActivityAt(e.atMs, gid, groupNameOf(gid), text, HistoryFeed.activityKind(e.event), e.emergency)
        }
        "message" -> {
            val body = if (e.text.length > 60) e.text.take(60) + "…" else e.text
            if (e.event.startsWith("message.sms")) {
                // 타인의 1:1 문자 — 전화 축이라 ⑥ 에 남긴다(감시 대상 표시). 다시 걸기·문자는 달지 않는다.
                addCallLog(CallLogRow(atMs = e.atMs, peer = "$from → ${displayName(e.to)}", text = body,
                    kind = CallLogKind.SMS, others = true, startedAtMs = e.atMs))
            } else {
                val gid = userPart(e.group).ifEmpty { userPart(e.to) }
                val target = if (e.group.isNotEmpty()) groupNameOf(gid) else displayName(e.to)
                addActivityAt(e.atMs, gid, target, "메시지 · $from" + if (body.isNotEmpty()) " — $body" else "",
                    ActivityKind.SDS, e.emergency)
            }
        }
    }
}
