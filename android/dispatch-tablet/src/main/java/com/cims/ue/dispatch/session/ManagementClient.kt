// 관리 평면 접점 — 조직/구성원/번호 · PTT 그룹 목록 · 이력 창 조회 · 녹취
// (docs/design/features/android_dispatch_tablet.md §6.4)
//
// SDK 파사드의 범용 요청(`CscClient.request`, Bearer) 위에 **경로와 JSON 만** 얹는다. 인증·전송·TLS 는
// 코어가 하고 이 클래스는 와이어 모양만 안다. 데스크톱의 `Services/ManagementClient.cs` 와 같은 계약이다.
//
//  서버 계약(android_ue_provisioning.md §3-2/§3-2a/§3-3/§3-4, CSC 4430, PKCE provisioning 토큰):
//    GET    /provisioning/directory/admin                 -> {scope, services{volte[],voip[],ptt[]}, orgs[], members[]} + ETag/304
//    POST   /provisioning/directory/orgs                  {code,name,parent,sort}
//    PUT    /provisioning/directory/orgs/{code}           {name?,parent?,sort?}      DELETE ...
//    POST   /provisioning/directory/members               {name,org,title,loginId,password,volte{..},voip{..},ptt{..}} -> {userId}
//    PUT    /provisioning/directory/members/{id}          {name?,org?,title?,loginId?,password?}   DELETE ...
//    PUT    /provisioning/directory/members/{id}/{kind}   {msisdn,imsi,serviceRef,sipTransport,password}  DELETE ...
//    PUT    /provisioning/directory/members/{id}/ptt/profile {allowCreateGroup,...}
//    GET    /provisioning/directory/groups                -> {groups[]}
//    GET    /provisioning/history?kind=&since=&until=&limit=
//    GET    /provisioning/history/ptt/{recordingId}       -> {session, participants[], events[], floor[], hasRecording}
//    GET    /provisioning/recordings/{id}                 -> 세션·세그먼트 메타
//    GET    /provisioning/recordings/{id}/segments/{seq}/audio?slot=&retry=  -> 200 MP4 · 202 변환 중 · 500 실패
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CscClient
import com.cims.ue.sdk.HttpResponse
import kotlinx.coroutines.delay
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.time.Instant
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

class ManagementClient(
    private val csc: CscClient,
    /** **유효한** access token 공급자 — 만료가 가까우면 갱신한 뒤 돌려준다(그래서 suspend 다). */
    private val token: suspend () -> String?,
    /** 401 을 받았을 때 **강제로** 갱신한다. 지역 만료 시각과 무관하다. */
    private val renew: suspend () -> String?,
) {

    // ── 공통 ──────────────────────────────────────────────────────────────────

    /**
     * 2xx/304 가 아니면 본문을 문구로 바꿔 실패로 만든다 — 서버 오류 JSON 은 사전이 읽는다.
     *
     * **401 은 한 번 되살려 본다.** 지역 만료 시각만 믿으면 서버가 토큰을 버린 경우를 못 넘는다 —
     * CSC `configure` 재실행으로 `IdMs.JwtSecret` 이 재생성되면 발급된 토큰이 **즉시 전부 무효**가
     * 되는데(그 설정의 help 가 그렇게 적고 있다) 앱의 시계는 아직 한참 남았다고 본다. 그대로 두면
     * 관제사는 «다시 로그인하세요» 만 보고, SIP 은 H(A1) 이라 통화는 멀쩡해서 더 헷갈린다.
     * refresh token 은 서버가 저장한 기록으로 검증하므로 시크릿이 바뀌어도 갱신은 성립한다.
     */
    private suspend fun send(area: TextArea, method: String, path: String,
                             json: String? = null, ifNoneMatch: String = ""): CimsResult<HttpResponse> {
        val t = token() ?: return CimsResult.fail(-1, "로그인 전")
        var r = csc.requestJson(t, method, path, json, ifNoneMatch = ifNoneMatch)
        if (!r.ok && r.value?.status == 401) {
            val fresh = renew()
            // 같은 토큰이 돌아왔으면 갱신이 실패한 것이다 — 두 번 보내 봐야 같은 401 이다.
            if (fresh != null && fresh != t)
                r = csc.requestJson(fresh, method, path, json, ifNoneMatch = ifNoneMatch)
        }
        if (r.ok) return r
        val body = r.value?.text.orEmpty()
        return CimsResult(false, r.code, ResponseText.of(area, r.code, body.ifEmpty { r.reason }), r.value)
    }

    private fun <T> map(r: CimsResult<HttpResponse>, parse: (JSONObject) -> T): CimsResult<T> {
        if (!r.ok) return CimsResult.fail(r.code, r.reason)
        val body = r.value?.text.orEmpty().ifBlank { "{}" }
        return try {
            CimsResult.ok(parse(JSONObject(body)))
        } catch (e: Exception) {
            CimsResult.fail(-2, "응답 해석 실패: ${e.message}")
        }
    }

    // ── 조직·구성원·번호(§4.5) ───────────────────────────────────────────────

    /** 관리 화면 한 벌. `etag` 가 같으면 값이 null 이다(304 — 내용 유지). */
    suspend fun adminView(etag: String = ""): CimsResult<AdminView?> {
        val r = send(TextArea.MANAGEMENT, "GET", "/provisioning/directory/admin", ifNoneMatch = etag)
        if (!r.ok) return CimsResult.fail(r.code, r.reason)
        if (r.value?.notModified == true) return CimsResult.ok<AdminView?>(null)
        return map(r) { parseAdminView(it, r.value?.etag.orEmpty()) }
    }

    suspend fun createOrg(code: String, name: String, parent: String, sort: Int): CimsResult<Unit> =
        unit(send(TextArea.MANAGEMENT, "POST", "/provisioning/directory/orgs",
            JSONObject().put("code", code).put("name", name).put("parent", parent).put("sort", sort).toString()))

    suspend fun updateOrg(code: String, name: String, parent: String, sort: Int): CimsResult<Unit> =
        unit(send(TextArea.MANAGEMENT, "PUT", "/provisioning/directory/orgs/${enc(code)}",
            JSONObject().put("name", name).put("parent", parent).put("sort", sort).toString()))

    suspend fun deleteOrg(code: String): CimsResult<Unit> =
        unit(send(TextArea.MANAGEMENT, "DELETE", "/provisioning/directory/orgs/${enc(code)}"))

    /** 구성원 개설 — 응답의 `userId` 를 돌려준다(회선·자격은 이어서 PUT). */
    suspend fun createMember(name: String, org: String, title: String,
                             loginId: String, password: String): CimsResult<Long> {
        val body = JSONObject().put("name", name).put("org", org).put("title", title)
        if (loginId.isNotBlank()) body.put("loginId", loginId)
        if (password.isNotBlank()) body.put("password", password)
        val r = send(TextArea.MANAGEMENT, "POST", "/provisioning/directory/members", body.toString())
        return map(r) { it.optLong("userId", 0L) }
    }

    suspend fun updateMember(userId: Long, name: String, org: String, title: String,
                             loginId: String, password: String): CimsResult<Unit> {
        val body = JSONObject().put("name", name).put("org", org).put("title", title)
        if (loginId.isNotBlank()) body.put("loginId", loginId)
        if (password.isNotBlank()) body.put("password", password)
        return unit(send(TextArea.MANAGEMENT, "PUT", "/provisioning/directory/members/$userId", body.toString()))
    }

    suspend fun deleteMember(userId: Long): CimsResult<Unit> =
        unit(send(TextArea.MANAGEMENT, "DELETE", "/provisioning/directory/members/$userId"))

    /** 회선 개설·변경. 빈 값은 싣지 않는다 — 서버는 없는 필드를 "현재값 유지" 로 읽는다. */
    suspend fun putNumber(userId: Long, kind: String, n: NumberInfo, password: String): CimsResult<Unit> {
        val body = JSONObject().put("msisdn", n.msisdn)
        if (n.imsi.isNotBlank()) body.put("imsi", n.imsi)
        if (n.serviceRef.isNotBlank()) body.put("serviceRef", n.serviceRef)
        if (n.sipTransport.isNotBlank()) body.put("sipTransport", n.sipTransport)
        if (password.isNotBlank()) body.put("password", password)
        return unit(send(TextArea.MANAGEMENT, "PUT",
            "/provisioning/directory/members/$userId/$kind", body.toString()))
    }

    suspend fun deleteNumber(userId: Long, kind: String): CimsResult<Unit> =
        unit(send(TextArea.MANAGEMENT, "DELETE", "/provisioning/directory/members/$userId/$kind"))

    /** PTT 자격 — 원격 청취는 읽기 전용이라 싣지 않는다(서버가 `not_editable` 로 거절한다). */
    suspend fun putPttProfile(userId: Long, flags: Map<String, Boolean>): CimsResult<Unit> {
        val body = JSONObject()
        flags.forEach { (k, v) -> body.put(k, v) }
        return unit(send(TextArea.MANAGEMENT, "PUT",
            "/provisioning/directory/members/$userId/ptt/profile", body.toString()))
    }

    // ── 회사 전화번호부(§4.7) ───────────────────────────────────────────────

    /** 전화번호부 한 벌. `etag` 가 같으면 값이 null 이다(304). `service` = volte|voip|ptt. */
    suspend fun directory(service: String, etag: String = ""): CimsResult<DirectoryBook?> {
        val r = send(TextArea.MANAGEMENT, "GET", "/provisioning/directory?service=${enc(service)}", ifNoneMatch = etag)
        if (!r.ok) return CimsResult.fail(r.code, r.reason)
        if (r.value?.notModified == true) return CimsResult.ok<DirectoryBook?>(null)
        return map(r) { parseDirectory(it, r.value?.etag.orEmpty()) }
    }

    // ── PTT 그룹 목록(§4.7) ──────────────────────────────────────────────────

    suspend fun listGroups(): CimsResult<List<ManagedGroup>> =
        map(send(TextArea.MANAGEMENT, "GET", "/provisioning/directory/groups")) { parseGroups(it) }

    // ── 이력 창 조회(§4.6) ───────────────────────────────────────────────────

    /**
     * `[from, to)` 창의 이력. 서버 스캔이 48 시간 버킷 상한이라 **하루 단위**로 부른다.
     *
     * 시각은 오프셋 없는 로컬 표기로 보낸다 — 서버가 제 시간대로 읽는다(콘솔 이력과 같은 규약).
     */
    suspend fun history(kind: HistoryKind, fromMs: Long, toMs: Long, limit: Int = 1000): CimsResult<HistoryPage> {
        val path = "/provisioning/history?kind=${kind.wire}" +
            "&since=${enc(localIso(fromMs))}&until=${enc(localIso(toMs))}" +
            "&limit=${limit.coerceIn(1, 1000)}"
        return map(send(TextArea.MANAGEMENT, "GET", path)) { parseHistory(kind, it) }
    }

    suspend fun pttSessionDetail(recordingId: String): CimsResult<PttSessionDetail> =
        map(send(TextArea.MANAGEMENT, "GET", "/provisioning/history/ptt/${encPath(recordingId)}")) {
            parsePttDetail(it, recordingId)
        }

    // ── 녹취(§4.6) ──────────────────────────────────────────────────────────

    suspend fun recording(id: String): CimsResult<RecordingInfo> =
        map(send(TextArea.RECORDING, "GET", "/provisioning/recordings/${encPath(id)}")) { parseRecording(it, id) }

    /**
     * 세그먼트 오디오(MP4/AAC)를 받아 로컬 파일로. `slot` = 단독 발언자 트랙(null = 믹스).
     *
     * 202 는 "변환 중" 이라 서버가 만들 때까지 기다린다 — 0.7→1.5초 간격으로 최대 120초
     * (콘솔 SegmentPlayer 와 같은 규약).
     *
     * **재생할 때마다 서버에 다시 묻는다.** 인가와 감사가 서버에 있기 때문이다(아래 주석).
     */
    suspend fun fetchSegment(dir: File, id: String, seq: Int, slot: Int?, retry: Boolean,
                             onStatus: (String) -> Unit = {}): CimsResult<File> {
        var t = token() ?: return CimsResult.fail(-1, "로그인 전")
        val q = buildList {
            if (slot != null) add("slot=$slot")
            if (retry) add("retry=1")
        }
        val path = "/provisioning/recordings/${encPath(id)}/segments/$seq/audio" +
            if (q.isEmpty()) "" else "?" + q.joinToString("&")

        dir.mkdirs()
        val base = "${sanitize(id)}_${seq}_${slot?.toString() ?: "mix"}"

        // **캐시로 서버 호출을 건너뛰지 않는다.** 녹취 재생은 당사자 모르게 통화를 여는 동작이라
        // 서버가 요청마다 범위를 다시 보고(`dispatch_recordings.py` `in_scope` → 403 `out_of_scope`)
        // 감사를 남긴다(`E-AUD-016 tap_mode=recording`, 재생 단위). 받아 둔 파일을 그대로 돌려주면
        // 청취 범위가 회수된 뒤에도 계속 재생되고 **감사 기록이 남지 않는다**
        // ([dispatch_center.md](dispatch_center.md) §5.7b 의 인가 경계를 앱 캐시가 대신하게 된다).
        // 로컬 파일은 «이번 요청의 산출을 담는 자리» 일 뿐이다.

        val deadline = System.currentTimeMillis() + FETCH_TIMEOUT_MS
        var wait = 700L
        while (true) {
            var r = csc.request(t, "GET", path, accept = "audio/mp4")
            if (r.value?.status == 401) {                 // 이진 경로도 같은 복구(send 를 타지 않는다)
                val fresh = renew()
                if (fresh != null && fresh != t) { t = fresh; r = csc.request(t, "GET", path, accept = "audio/mp4") }
            }
            val v = r.value
            if (r.ok && v != null && v.status == 200 && v.body.isNotEmpty()) {
                // 재생 중인 파일을 덮어쓰면 MediaPlayer 가 깨지므로 매번 새 이름으로 쓴다.
                val out = File(dir, "${base}_${System.currentTimeMillis()}.mp4")
                out.writeBytes(v.body)
                return CimsResult.ok(out)
            }
            if (v?.status == 200) return CimsResult.fail(200, "녹취 응답이 비었습니다 — [다시 변환]")
            if (v?.status != 202) {
                val body = v?.text.orEmpty()
                return CimsResult.fail(v?.status ?: r.code,
                    ResponseText.of(TextArea.RECORDING, v?.status ?: r.code, body.ifEmpty { r.reason }))
            }
            if (System.currentTimeMillis() >= deadline) return CimsResult.fail(408, "녹취 변환이 끝나지 않았습니다 — 잠시 후 다시")
            onStatus("변환 중…")
            delay(wait)
            wait = (wait + 200L).coerceAtMost(1500L)
        }
    }

    /** 오래된 임시 녹취 정리 — 화면을 떠날 때 부른다(6시간 지난 것). */
    fun sweepRecordings(dir: File, olderThanMs: Long = REC_KEEP_MS) {
        val cutoff = System.currentTimeMillis() - olderThanMs
        dir.listFiles()?.forEach { if (it.isFile && it.lastModified() < cutoff) it.delete() }
    }

    private fun unit(r: CimsResult<HttpResponse>): CimsResult<Unit> =
        if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)

    companion object {
        private const val FETCH_TIMEOUT_MS = 120_000L
        private const val REC_KEEP_MS = 6 * 60 * 60 * 1000L

        /**
         * 경로·질의 조각 인코딩 — RFC 3986 unreserved(`A-Za-z0-9-._~`)만 남긴다.
         *
         * 코어의 인코더(`CscClient.urlEncode`)를 쓰지 않는다. 그건 네이티브 호출이라 JVM 단위시험에서
         * 돌지 않고, 여기서 필요한 것은 문자열 규칙 하나뿐이다. `URLEncoder` 도 쓰지 않는다 —
         * 공백을 `+` 로 바꿔 경로 조각에서 틀린다.
         */
        internal fun enc(s: String): String {
            val out = StringBuilder(s.length)
            s.toByteArray(Charsets.UTF_8).forEach { b ->
                val c = b.toInt().toChar()
                if (c.isLetterOrDigit() && b.toInt() in 0..127 || c == '-' || c == '.' || c == '_' || c == '~')
                    out.append(c)
                else out.append('%').append("%02X".format(b.toInt() and 0xFF))
            }
            return out.toString()
        }

        /** 녹취 id 는 `ptt/24/2026/09/07/10/S…_1` 처럼 경로형이다 — 구분자는 남기고 조각만 인코딩한다. */
        internal fun encPath(id: String): String = id.split('/').joinToString("/") { enc(it) }

        private fun sanitize(s: String): String = s.map { if (it.isLetterOrDigit() || it == '-') it else '_' }.joinToString("")

        /** 오프셋 없는 로컬 표기 `2026-09-15T00:00:00`. */
        internal fun localIso(ms: Long): String =
            LocalDateTime.ofInstant(Instant.ofEpochMilli(ms), ZoneId.systemDefault())
                .format(DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss"))

        /** ISO8601(오프셋 있으면 그대로, 없으면 로컬) → epoch ms. 못 읽으면 null. */
        internal fun timeMs(o: JSONObject, name: String): Long? {
            val s = o.optString(name, "")
            if (s.isBlank()) return null
            return try {
                OffsetDateTime.parse(s).toInstant().toEpochMilli()
            } catch (_: Exception) {
                try {
                    LocalDateTime.parse(s).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()
                } catch (_: Exception) { null }
            }
        }

        private fun strings(a: JSONArray?): List<String> {
            if (a == null) return emptyList()
            return (0 until a.length()).mapNotNull { i -> a.optString(i, "").takeIf { it.isNotBlank() } }
        }

        private fun objects(o: JSONObject, name: String): List<JSONObject> {
            val a = o.optJSONArray(name) ?: return emptyList()
            return (0 until a.length()).mapNotNull { a.optJSONObject(it) }
        }

        private fun nInt(o: JSONObject, name: String): Int? = if (o.has(name) && !o.isNull(name)) o.optInt(name) else null

        // ── 파서(시험 대상) ──────────────────────────────────────────────────

        internal fun parseAdminView(root: JSONObject, etag: String): AdminView {
            val sc = root.optJSONObject("scope") ?: JSONObject()
            // 전환기 서버는 관리 범위를 `directoryAdmin` 으로 냈다 — 새 이름을 먼저 본다.
            val write = sc.optString("directoryWrite", "").ifBlank { sc.optString("directoryAdmin", "") }
            val scope = AdminScope(sc.optString("groupId", ""), write, sc.optString("orgCode", ""))

            // 접속서비스 후보 — 버킷이 곧 회선 종류다. 항목이 제 kind 를 실으면 그것을 따른다
            // (전환기 서버는 voip 항목을 volte 버킷에 함께 실었다).
            val services = ArrayList<ServiceRef>()
            root.optJSONObject("services")?.let { sv ->
                LineKind.all.forEach { bucket ->
                    objects(sv, bucket).forEach { x ->
                        val raw = x.optString("kind", "").lowercase()
                        val kind = when {
                            raw.isBlank() -> bucket
                            raw == "mcptt" -> LineKind.PTT
                            raw in LineKind.all -> raw
                            else -> bucket
                        }
                        services.add(ServiceRef(kind, x.optString("name", ""), x.optString("domain", "")))
                    }
                }
            }

            val orgs = objects(root, "orgs").map {
                OrgNode(it.optString("code", ""), it.optString("name", ""), it.optString("parent", ""), it.optInt("sort", 0))
            }
            val members = objects(root, "members").map { m ->
                MemberInfo(
                    userId = m.optLong("userId", 0L),
                    name = m.optString("name", ""),
                    loginId = m.optString("loginId", ""),
                    org = m.optString("org", ""),
                    title = m.optString("title", ""),
                    volte = parseNumber(m, LineKind.VOLTE),
                    voip = parseNumber(m, LineKind.VOIP),
                    ptt = parseNumber(m, LineKind.PTT))
            }
            return AdminView(scope, services, orgs, members, etag)
        }

        private fun parseNumber(m: JSONObject, kind: String): NumberInfo? {
            val n = m.optJSONObject(kind) ?: return null
            val prof = n.optJSONObject("profile")?.let { p ->
                p.keys().asSequence().associateWith { p.optBoolean(it, false) }
            } ?: emptyMap()
            // 픽업 그룹은 읽기 전용 — 서버가 실어 줄 때만 보인다(편성은 콘솔 전화 그룹).
            val pickup = n.optString("pickupGroup", "").ifBlank { n.optString("pickup_group", "") }
            return NumberInfo(n.optString("msisdn", ""), n.optString("imsi", ""), n.optString("serviceRef", ""),
                n.optString("sipTransport", ""), n.optString("authScheme", ""), prof, pickup)
        }

        internal fun parseDirectory(root: JSONObject, etag: String): DirectoryBook {
            val orgs = objects(root, "orgs").map {
                OrgNode(it.optString("code", ""), it.optString("name", ""), it.optString("parent", ""), it.optInt("sort", 0))
            }
            val entries = objects(root, "entries").mapNotNull { e ->
                val n = e.optString("msisdn", "")
                if (n.isBlank()) null else DirectoryEntry(e.optString("org", ""), e.optString("name", ""), n)
            }
            return DirectoryBook(orgs, entries, etag)
        }

        internal fun parseGroups(root: JSONObject): List<ManagedGroup> =
            objects(root, "groups").map { g ->
                ManagedGroup(
                    id = g.optString("id", ""), uri = g.optString("uri", ""), name = g.optString("name", ""),
                    memberCount = g.optInt("memberCount", 0), isOwner = g.optBoolean("isOwner", false),
                    orgCode = g.optString("orgCode", ""), sessionType = g.optString("sessionType", ""),
                    etag = g.optString("etag", ""),
                    // 구 서버(필드 없음) = 종전대로 전부 관리 가능
                    canManage = g.optBoolean("canManage", true),
                    inListenScope = g.optBoolean("inListenScope", false),
                    isMember = g.optBoolean("isMember", false))
            }

        internal fun parseHistory(kind: HistoryKind, root: JSONObject): HistoryPage {
            val hours = HashMap<String, Int>()
            root.optJSONObject("hours")?.let { h -> h.keys().forEach { hours[it] = h.optInt(it, 0) } }
            val items = objects(root, "items").mapNotNull { it ->
                val id = it.optString("id", "")
                val at = timeMs(it, "time") ?: return@mapNotNull null
                if (id.isBlank()) return@mapNotNull null
                HistoryEntry(
                    id = id, atMs = at,
                    kind = when (it.optString("kind", "")) {
                        "call" -> HistoryKind.CALL
                        "ptt" -> HistoryKind.PTT
                        "message" -> HistoryKind.MESSAGE
                        else -> kind
                    },
                    event = it.optString("event", ""), from = it.optString("from", ""), to = it.optString("to", ""),
                    group = it.optString("group", ""), durationSec = it.optInt("duration", 0),
                    emergency = it.optBoolean("emergency", false), text = it.optString("text", ""),
                    recordingId = it.optString("recordingId", ""), hasRecording = it.optBoolean("hasRecording", false),
                    state = it.optString("state", ""), callType = it.optString("callType", ""),
                    inviteAtMs = timeMs(it, "inviteTime"), answerAtMs = timeMs(it, "answerTime"),
                    endAtMs = timeMs(it, "endTime"), endReason = it.optString("endReason", ""),
                    sipStatus = it.optInt("sipStatus", 0),
                    sessionKind = it.optString("sessionKind", ""), startAtMs = timeMs(it, "startTime"),
                    groupName = it.optString("groupName", ""), memberCount = it.optInt("memberCount", 0),
                    turnCount = it.optInt("turnCount", 0), speakerCount = it.optInt("speakerCount", 0),
                    totalSpeechMs = it.optInt("totalSpeechMs", 0), talkMs = it.optInt("talkMs", 0),
                    maxConcurrent = it.optInt("maxConcurrent", 0), floorControl = it.optString("floorControl", ""),
                    floorPolicy = it.optString("floorPolicy", ""), maxTalkers = it.optInt("maxTalkers", 0),
                    people = strings(it.optJSONArray("people")))
            }.sortedBy { it.atMs }
            return HistoryPage(items, root.optString("next", ""), hours)
        }

        internal fun parsePttDetail(root: JSONObject, recordingId: String): PttSessionDetail {
            val parts = objects(root, "participants").mapNotNull { p ->
                val id = p.optString("msisdn", "")
                if (id.isBlank()) null
                else PttParticipant(id, p.optString("role", ""), timeMs(p, "join_time"), timeMs(p, "leave_time"))
            }
            val events = objects(root, "events").map { e ->
                PttEvent(timeMs(e, "ts"), e.optString("type", ""), e.optString("member", ""),
                    e.optString("role", ""), nInt(e, "duration"))
            }
            val floor = objects(root, "floor").map { f ->
                PttFloorEvent(timeMs(f, "ts"), f.optString("op", ""), f.optString("user", ""),
                    nInt(f, "slot"), nInt(f, "prio"), nInt(f, "talkers"), f.optString("policy", ""),
                    f.optBoolean("preempt", false), f.optString("preempted_from", ""), f.optString("reason", ""),
                    nInt(f, "cause"), f.optString("owner", ""), nInt(f, "pos"), nInt(f, "qsize"),
                    f.optString("revoked", ""), nInt(f, "removed"), nInt(f, "grace_sec"), nInt(f, "idle_ms"),
                    f.optString("preempted_by", ""))
            }
            return PttSessionDetail(root.optString("recordingId", "").ifBlank { recordingId },
                parts, events, floor, root.optBoolean("hasRecording", false))
        }

        internal fun parseRecording(root: JSONObject, id: String): RecordingInfo {
            val segs = objects(root, "segments").map { s ->
                val tracks = objects(s, "tracks").map { tr ->
                    SegmentTrack(tr.optInt("slot", 0), tr.optString("kind", ""),
                        objects(tr, "speakers").map {
                            SpeakerSpan(it.optString("id", ""), it.optInt("offset_ms", 0), it.optInt("dur_ms", 0))
                        },
                        tr.optBoolean("has_video", false), tr.optString("status", ""))
                }
                RecordingSegment(s.optInt("seq", 0), s.optString("type", ""), s.optString("speaker_id", ""),
                    timeMs(s, "start_time"), timeMs(s, "end_time"), s.optInt("duration_ms", 0),
                    s.optBoolean("has_video", false), s.optString("status", ""),
                    strings(s.optJSONArray("speaker_ids")), s.optInt("talker_count", 0), tracks)
            }
            return RecordingInfo(root.optString("id", "").ifBlank { id }, root.optString("call_type", ""),
                root.optString("caller", ""), root.optString("callee", ""), root.optString("group_id", ""),
                timeMs(root, "start_time"), timeMs(root, "end_time"), root.optInt("duration", 0),
                root.optString("status", ""), segs)
        }
    }
}
