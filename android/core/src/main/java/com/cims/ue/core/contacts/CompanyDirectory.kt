package com.cims.ue.core.contacts

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** 조직 노드 — 코드/이름/상위 조직 코드/정렬. 회사 연락처 트리 구성용. */
data class CompanyOrg(val code: String, val name: String, val parent: String, val sort: Int)

/** 회사 전화번호부 항목(읽기전용) — 소속 조직 코드/이름/VoLTE 번호. */
data class CompanyContact(val orgCode: String, val name: String, val number: String)

/** 회사 전화번호부 = 조직 트리 + 가입자. */
data class CompanyDirectory(val orgs: List<CompanyOrg>, val members: List<CompanyContact>)

/**
 * 회사 전화번호부 캐시. 서버(`/provisioning/directory`)에서 받은 조직 트리+가입자를 로컬에 저장해
 * 오프라인에서도 표시한다(개인 연락처와 달리 단말에서 편집 불가).
 * SharedPreferences + JSON (추가 의존성 없음).
 */
class CompanyDirectoryStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("company_dir", Context.MODE_PRIVATE)

    /** 캐시된 조직 트리 + 가입자. */
    fun load(): CompanyDirectory {
        val orgArr = runCatching { JSONArray(prefs.getString(K_ORGS, "[]").orEmpty()) }.getOrDefault(JSONArray())
        val memArr = runCatching { JSONArray(prefs.getString(K_MEM, "[]").orEmpty()) }.getOrDefault(JSONArray())
        val orgs = (0 until orgArr.length()).mapNotNull { i ->
            val o = orgArr.optJSONObject(i) ?: return@mapNotNull null
            CompanyOrg(o.optString("code"), o.optString("name"), o.optString("parent"), o.optInt("sort"))
        }
        val members = (0 until memArr.length()).mapNotNull { i ->
            val o = memArr.optJSONObject(i) ?: return@mapNotNull null
            CompanyContact(o.optString("org"), o.optString("name"), o.optString("num"))
        }
        return CompanyDirectory(orgs, members)
    }

    /** 서버에서 받은 전체 디렉터리로 교체. */
    fun replace(dir: CompanyDirectory) {
        val orgArr = JSONArray()
        dir.orgs.forEach { orgArr.put(JSONObject().put("code", it.code).put("name", it.name)
            .put("parent", it.parent).put("sort", it.sort)) }
        val memArr = JSONArray()
        dir.members.forEach { memArr.put(JSONObject().put("org", it.orgCode).put("name", it.name).put("num", it.number)) }
        prefs.edit().putString(K_ORGS, orgArr.toString()).putString(K_MEM, memArr.toString()).apply()
    }

    private companion object {
        const val K_ORGS = "orgs"
        const val K_MEM = "members"
    }
}
