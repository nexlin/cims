package com.cims.ue.ptt.history

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONArray
import org.json.JSONObject

/** 통화이력 한 건. [kind] 는 [com.cims.ue.ptt.PttEventKind].name, [peer] 는 상대 MCPTT ID(발언 수신 시). */
data class HistEntry(
    val time: Long,
    val groupId: String,
    val kind: String,
    val peer: String?,
    val durationMs: Long,
)

/**
 * PTT 통화이력 영속화 — 참여/이탈/발언(송·수신)/긴급 이벤트의 데이터 소스.
 * 추가 의존성 없이 SharedPreferences + JSON 으로 저장(core MessageStore 와 동일 방침).
 */
class HistoryStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("ptt_history", Context.MODE_PRIVATE)

    private val _changes = MutableStateFlow(0)
    /** 변경 틱 — UI 재조회 트리거. */
    val changes: StateFlow<Int> = _changes

    fun add(groupId: String, kind: String, peer: String? = null, durationMs: Long = 0,
            time: Long = System.currentTimeMillis()) {
        if (groupId.isBlank()) return
        val list = all().toMutableList()
        list.add(0, HistEntry(time, groupId, kind, peer, durationMs))
        save(list.take(MAX))
        _changes.value++
    }

    /** 전체 이력(최신순). */
    fun list(): List<HistEntry> = all()

    fun clear() {
        prefs.edit().remove(KEY).apply()
        _changes.value++
    }

    private fun all(): List<HistEntry> = runCatching {
        val arr = JSONArray(prefs.getString(KEY, "[]") ?: "[]")
        (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            HistEntry(o.getLong("t"), o.getString("g"), o.getString("k"),
                o.optString("p").ifBlank { null }, o.optLong("d"))
        }
    }.getOrDefault(emptyList())

    private fun save(list: List<HistEntry>) {
        val arr = JSONArray()
        list.forEach { e ->
            arr.put(JSONObject().put("t", e.time).put("g", e.groupId).put("k", e.kind)
                .put("p", e.peer ?: "").put("d", e.durationMs))
        }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    companion object {
        private const val KEY = "entries"
        private const val MAX = 500
    }
}
