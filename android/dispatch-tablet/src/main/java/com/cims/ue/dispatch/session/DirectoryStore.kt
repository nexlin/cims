// 주소록 — 서버 전화번호부 캐시 + 로컬 CSV (android_dispatch_tablet.md §6.2b, dispatch_desktop_ui.md §14.2)
//
// **캐시** — 서버 전화번호부(`/provisioning/directory?service=volte|ptt`)를 ETag 와 함께 앱 저장소에 둔다. 켜자마자 그 캐시로
// 이름을 그리고, 서버에는 If-None-Match 로 묻는다(304 = 그대로). 캐시가 없으면 로그인 직후 몇 초 동안 이름 대신 번호가 보인다.
//
// **로컬 CSV** — 서버가 아직 주지 않는 것(외부망 번호·서버에 없는 이름)을 보탠다. 데스크톱 `directory.csv` 와 **같은 파일**을
// 쓴다: `kind,number,name,tags`(kind = ext | external | ptt | group). 같은 번호가 서버에 있으면 서버 이름이 이기고 CSV 이름은
// 빈 곳만 채운다 — 서버 가입자를 CSV 가 외부망으로 바꾸지 못한다.
package com.cims.ue.dispatch.session

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** CSV 한 줄의 종류 — 데스크톱 `ContactKind` 와 같은 낱말. */
enum class CsvKind { EXT, EXTERNAL, PTT, GROUP }

data class CsvContact(val kind: CsvKind, val number: String, val name: String, val tags: List<String> = emptyList())

internal object DirectoryCsv {
    /**
     * `kind,number,name,tags` — 머리줄(`kind,`)·빈 줄·`#` 주석은 건너뛴다. 모르는 kind 는 외부망으로 본다(데스크톱과 같다).
     * 같은 종류·같은 번호가 다시 나오면 뒤의 줄이 이긴다.
     */
    fun parse(text: String): List<CsvContact> {
        val out = LinkedHashMap<String, CsvContact>()
        text.removePrefix("﻿").lineSequence().forEach { raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#") || line.startsWith("kind,", ignoreCase = true)) return@forEach
            val f = split(line)
            if (f.size < 2) return@forEach
            val kind = when (f[0].trim().lowercase()) {
                "ext", "extension" -> CsvKind.EXT
                "ptt" -> CsvKind.PTT
                "group" -> CsvKind.GROUP
                else -> CsvKind.EXTERNAL                                  // external·ext-out·모르는 값
            }
            val number = f[1].trim()
            if (number.isEmpty()) return@forEach
            val name = f.getOrNull(2)?.trim().orEmpty()
            val tags = f.getOrNull(3)?.split(';')?.map { it.trim() }?.filter { it.isNotEmpty() }.orEmpty()
            val key = kind.name + ":" + DirectoryBook.normalize(number)
            out.remove(key)
            out[key] = CsvContact(kind, number, name, tags)
        }
        return out.values.toList()
    }

    /** 한 줄 분리 — 큰따옴표 필드 안의 쉼표("김철수, 팀장")와 `""` 이스케이프를 지킨다(RFC 4180, 데스크톱 `SplitCsv`). */
    fun split(line: String): List<String> {
        val fields = ArrayList<String>()
        val sb = StringBuilder()
        var quoted = false
        var i = 0
        while (i < line.length) {
            val c = line[i]
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < line.length && line[i + 1] == '"') { sb.append('"'); i++ } else quoted = false
                } else sb.append(c)
            } else when (c) {
                '"' -> quoted = true
                ',' -> { fields.add(sb.toString()); sb.setLength(0) }
                else -> sb.append(c)
            }
            i++
        }
        fields.add(sb.toString())
        return fields
    }
}

/**
 * 서버 전화번호부 + CSV 의 [kinds] 줄 — 같은 번호(정규형)면 **서버 줄에** CSV 이름을 빈 곳만 채우고, CSV 에만 있는 번호는 뒤에
 * 붙인다(조직 없음). 외부망 표시는 CSV 에만 있는 external 줄에만 선다 — 서버 가입자는 외부망이 아니다.
 */
internal fun mergeBook(server: DirectoryBook, csv: List<CsvContact>, kinds: Set<CsvKind>,
                       countryCode: String = "82"): DirectoryBook {
    val extra = LinkedHashMap<String, CsvContact>()
    // 숫자가 없는 번호(영숫자 id)는 정규형이 비어 서로 같아진다 — 그런 것은 원문이 키다(한 줄로 뭉개지지 않게)
    fun keyOf(n: String) = DirectoryBook.normalize(n, countryCode).ifEmpty { n.trim() }
    csv.filter { it.kind in kinds }.forEach { extra[keyOf(it.number)] = it }
    if (extra.isEmpty()) return server
    val merged = server.entries.map { e ->
        val c = extra.remove(keyOf(e.msisdn))
        if (c == null || e.name.isNotBlank()) e else e.copy(name = c.name)
    }
    val added = extra.values.map { DirectoryEntry("", it.name, it.number, external = it.kind == CsvKind.EXTERNAL) }
    return server.copy(entries = merged + added)
}

/** 캐시 파일의 모양 — 서비스마다 서버 본문과 같은 `orgs`·`entries` + `etag`. 손상되면 빈 것으로 읽는다(서버가 다시 채운다). */
internal object DirectoryCache {
    fun encode(books: Map<String, DirectoryBook>): String {
        val root = JSONObject()
        books.forEach { (svc, b) ->
            root.put(svc, JSONObject()
                .put("etag", b.etag)
                .put("orgs", JSONArray().apply {
                    b.orgs.forEach { o ->
                        put(JSONObject().put("code", o.code).put("name", o.name).put("parent", o.parent).put("sort", o.sort))
                    }
                })
                .put("entries", JSONArray().apply {
                    b.entries.forEach { e -> put(JSONObject().put("org", e.org).put("name", e.name).put("msisdn", e.msisdn)) }
                }))
        }
        return root.toString()
    }

    fun decode(text: String): Map<String, DirectoryBook> = runCatching {
        val root = JSONObject(text)
        root.keys().asSequence().associateWith { svc ->
            val o = root.getJSONObject(svc)
            ManagementClient.parseDirectory(o, o.str("etag", ""))
        }
    }.getOrDefault(emptyMap())
}

/** 앱 저장소의 두 파일 — 캐시와 가져온 CSV. 실패는 삼킨다: 없으면 서버만으로 선다. */
internal class DirectoryFiles(dir: File) {
    private val cache = File(dir, "directory-cache.json")
    private val csv = File(dir, "directory.csv")

    fun loadCache(): Map<String, DirectoryBook> =
        runCatching { if (cache.exists()) DirectoryCache.decode(cache.readText()) else emptyMap() }.getOrDefault(emptyMap())

    fun saveCache(books: Map<String, DirectoryBook>) { runCatching { writeAtomic(cache, DirectoryCache.encode(books)) } }

    fun loadCsv(): List<CsvContact> =
        runCatching { if (csv.exists()) DirectoryCsv.parse(csv.readText()) else emptyList() }.getOrDefault(emptyList())

    fun saveCsv(text: String) = writeAtomic(csv, text)

    fun deleteCsv() { runCatching { csv.delete() } }

    /** 쓰다 죽어도 반쪽 파일이 남지 않게 — 옆에 쓰고 바꿔 끼운다. */
    private fun writeAtomic(f: File, text: String) {
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.writeText(text)
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }
}
