package com.cims.ue.core.contacts

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** 즐겨찾기 항목 — 이름/번호. 회사·개인 어느 쪽에서든 별표로 추가. */
data class Favorite(val name: String, val number: String)

/**
 * 즐겨찾기 영속화. 회사/개인 연락처에서 ★ 토글로 추가/삭제, '즐겨찾기' 세그먼트에서 모아 본다.
 * 번호(MSISDN)를 키로 식별. SharedPreferences + JSON (추가 의존성 없음).
 */
class FavoriteStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("favorites", Context.MODE_PRIVATE)

    fun all(): List<Favorite> {
        val arr = runCatching { JSONArray(prefs.getString(KEY, "[]").orEmpty()) }.getOrDefault(JSONArray())
        return (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            Favorite(o.optString("name"), o.optString("num"))
        }.sortedBy { it.name }
    }

    fun isFavorite(number: String): Boolean = all().any { it.number == number }

    /** 토글 — 추가/삭제 후 새 상태(true=즐겨찾기됨) 반환. */
    fun toggle(name: String, number: String): Boolean {
        val cur = all().toMutableList()
        val idx = cur.indexOfFirst { it.number == number }
        val nowFav: Boolean
        if (idx >= 0) { cur.removeAt(idx); nowFav = false }
        else { cur.add(Favorite(name, number)); nowFav = true }
        save(cur)
        return nowFav
    }

    private fun save(list: List<Favorite>) {
        val arr = JSONArray()
        list.forEach { arr.put(JSONObject().put("name", it.name).put("num", it.number)) }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    private companion object { const val KEY = "list" }
}
