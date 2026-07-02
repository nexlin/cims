package com.cims.ue.core.message

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** 문자 방향 — 수신/발신. */
enum class MsgDirection { IN, OUT }

/** 문자 한 건. [peer] 는 상대 번호(표시용, sip:/@도메인 제거됨), [time] 은 epoch millis. */
data class MessageEntry(
    val peer: String,
    val text: String,
    val time: Long,
    val direction: MsgDirection,
    val read: Boolean,
)

/** 대화(스레드) 요약 — 상대별 마지막 문자 + 안읽음 수. */
data class MessageThread(val peer: String, val last: MessageEntry, val unread: Int)

/**
 * 문자(SIP MESSAGE) 인박스 영속화 — 상대 번호별 대화 스레드의 데이터 소스.
 * 추가 의존성 없이 SharedPreferences + JSON 으로 저장(CallLogStore 와 동일 방침).
 */
class MessageStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("sip_messages", Context.MODE_PRIVATE)

    /** 대화 스레드 목록(마지막 문자 최신순). */
    fun threads(): List<MessageThread> =
        all().groupBy { it.peer }.map { (peer, list) ->
            MessageThread(
                peer = peer,
                last = list.maxBy { it.time },
                unread = list.count { it.direction == MsgDirection.IN && !it.read },
            )
        }.sortedByDescending { it.last.time }

    /** 한 상대와의 대화(시간 오름차순). */
    fun thread(peer: String): List<MessageEntry> =
        all().filter { it.peer == peer }.sortedBy { it.time }

    /** 문자 추가(최대 [MAX] 건 유지). 발신은 읽음 처리, 수신은 안읽음으로 시작. */
    fun add(peer: String, text: String, direction: MsgDirection, time: Long = System.currentTimeMillis()) {
        if (peer.isBlank() || text.isBlank()) return
        val list = all().toMutableList()
        list.add(MessageEntry(peer, text, time, direction, read = direction == MsgDirection.OUT))
        save(list.sortedByDescending { it.time }.take(MAX))
    }

    /** 대화 진입 시 그 상대의 수신 문자를 모두 읽음 처리. @return 실제 변경 여부(불변이면 저장 생략). */
    fun markRead(peer: String): Boolean {
        val list = all()
        if (list.none { it.peer == peer && !it.read }) return false
        save(list.map { if (it.peer == peer && !it.read) it.copy(read = true) else it })
        return true
    }

    /** 전체 안읽음 수(탭 배지용). */
    fun unreadTotal(): Int = all().count { it.direction == MsgDirection.IN && !it.read }

    /** 한 상대와의 대화 삭제. */
    fun clearThread(peer: String) = save(all().filterNot { it.peer == peer })

    private fun all(): List<MessageEntry> {
        val arr = runCatching { JSONArray(prefs.getString(KEY, "[]").orEmpty()) }
            .getOrDefault(JSONArray())
        return (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            MessageEntry(
                peer = o.optString("p"),
                text = o.optString("m"),
                time = o.optLong("ts"),
                direction = runCatching { MsgDirection.valueOf(o.optString("d")) }
                    .getOrDefault(MsgDirection.IN),
                read = o.optBoolean("r", true),
            )
        }
    }

    private fun save(list: List<MessageEntry>) {
        val arr = JSONArray()
        list.forEach {
            arr.put(
                JSONObject().put("p", it.peer).put("m", it.text).put("ts", it.time)
                    .put("d", it.direction.name).put("r", it.read),
            )
        }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    private companion object {
        const val KEY = "entries"
        const val MAX = 500
    }
}
