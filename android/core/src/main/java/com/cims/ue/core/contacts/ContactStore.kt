package com.cims.ue.core.contacts

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/** 연락처 한 건. [id] 는 내부 식별자, [name] 표시 이름, [number] MSISDN. */
data class Contact(val id: String, val name: String, val number: String)

/**
 * 앱 로컬 연락처 영속화. 일반 전화앱의 "연락처" 탭 데이터 소스.
 * SharedPreferences + JSON (ConfigStore 와 동일 방침, 추가 의존성 없음).
 */
class ContactStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("contacts", Context.MODE_PRIVATE)

    /** 이름순 전체 연락처. */
    fun all(): List<Contact> {
        val arr = runCatching { JSONArray(prefs.getString(KEY, "[]").orEmpty()) }
            .getOrDefault(JSONArray())
        return (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            Contact(o.optString("id"), o.optString("name"), o.optString("num"))
        }.sortedBy { it.name }
    }

    /** id 가 비어 있으면 신규 추가, 있으면 갱신. 추가된/갱신된 연락처 반환. */
    fun upsert(name: String, number: String, id: String? = null): Contact {
        val list = all().toMutableList()
        val c = Contact(id?.takeIf { it.isNotBlank() } ?: UUID.randomUUID().toString(), name.trim(), number.trim())
        val idx = list.indexOfFirst { it.id == c.id }
        if (idx >= 0) list[idx] = c else list.add(c)
        save(list)
        return c
    }

    fun delete(id: String) = save(all().filterNot { it.id == id })

    /** 번호로 연락처 이름 조회(최근기록 표시용). 없으면 null. */
    fun nameFor(number: String): String? =
        all().firstOrNull { it.number == number }?.name

    private fun save(list: List<Contact>) {
        val arr = JSONArray()
        list.forEach { arr.put(JSONObject().put("id", it.id).put("name", it.name).put("num", it.number)) }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    private companion object { const val KEY = "list" }
}
