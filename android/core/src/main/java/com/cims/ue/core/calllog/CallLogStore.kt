package com.cims.ue.core.calllog

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** 통화 종류 — 발신/수신(연결됨)/부재중. */
enum class CallType { OUTGOING, INCOMING, MISSED }

/**
 * 통화 기록 한 건. [number] 는 표시용 번호(sip:/@도메인 제거됨), [time] 은 epoch millis,
 * [durationSec] 은 연결 후 통화시간(초, 미연결=0), [video] 는 영상통화 여부.
 */
data class CallEntry(
    val number: String,
    val type: CallType,
    val time: Long,
    val durationSec: Int = 0,
    val video: Boolean = false,
)

/**
 * 최근 통화 기록 영속화. 일반 전화앱의 "최근기록/부재중" 탭 데이터 소스.
 * 추가 의존성 없이 SharedPreferences + JSON 으로 저장(ConfigStore 와 동일 방침).
 */
class CallLogStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("call_log", Context.MODE_PRIVATE)

    /** 최신순 전체 기록. */
    fun all(): List<CallEntry> {
        val arr = runCatching { JSONArray(prefs.getString(KEY, "[]").orEmpty()) }
            .getOrDefault(JSONArray())
        return (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            CallEntry(
                number = o.optString("n"),
                type = runCatching { CallType.valueOf(o.optString("t")) }.getOrDefault(CallType.OUTGOING),
                time = o.optLong("ts"),
                durationSec = o.optInt("d"),
                video = o.optBoolean("v"),
            )
        }.sortedByDescending { it.time }
    }

    /** 새 기록 추가(최대 [MAX] 건 유지). */
    fun add(
        number: String,
        type: CallType,
        time: Long = System.currentTimeMillis(),
        durationSec: Int = 0,
        video: Boolean = false,
    ) {
        if (number.isBlank()) return
        val list = all().toMutableList()
        list.add(0, CallEntry(number, type, time, durationSec, video))
        save(list.take(MAX))
    }

    /** 기록 한 건 삭제 — 전 필드 일치 항목 제거(스와이프 삭제). */
    fun remove(entry: CallEntry) = save(all().filterNot { it == entry })

    /** 선택 삭제 — 전달된 기록들과 전 필드 일치 항목 일괄 제거. */
    fun removeAll(entries: Collection<CallEntry>) {
        val victims = entries.toSet()
        save(all().filterNot { it in victims })
    }

    private fun save(list: List<CallEntry>) {
        val arr = JSONArray()
        list.forEach {
            arr.put(
                JSONObject().put("n", it.number).put("t", it.type.name).put("ts", it.time)
                    .put("d", it.durationSec).put("v", it.video),
            )
        }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    private companion object {
        const val KEY = "entries"
        const val MAX = 200
    }
}
