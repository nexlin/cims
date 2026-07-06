package com.cims.ue.ptt.csc

/** IdMS 토큰 응답 (POST /idms/tokenreq). */
data class TokenSet(
    val accessToken: String,
    val tokenType: String,
    val refreshToken: String?,
    val idToken: String?,
    val expiresInSec: Int,
    val scope: String?,
)

/** GMS 그룹 목록 항목 (GET /org.openmobilealliance.groups/users/{me}).
 *  우선순위·영상 여부 등 그룹 속성은 표준 경로인 그룹 문서(TS 24.481, [GroupDoc])에서 조회한다. */
data class GroupSummary(
    val uri: String,
    val displayName: String?,
    val etag: String?,
    val memberCount: Int?,
)

/** TS 24.481 그룹 문서의 멤버 `<entry>` — uri(tel:번호)·display-name·participant-type·user-priority. */
data class GroupMember(
    val uri: String,
    val name: String?,
    /** TS 24.380 participant-type — "chair" | "participant". */
    val role: String,
    /** TS 24.481 user-priority (발언권 우선순위, 0~255 클수록 높음). */
    val priority: Int?,
)

/** TS 24.481 그룹 문서(list-service) — 채널 상세 화면용 요약. [GroupDoc.parse] 로 XML 에서 생성. */
data class GroupDoc(
    val uri: String,
    val displayName: String?,
    val members: List<GroupMember>,
    /** on-network-group-priority. */
    val priority: Int?,
    /** mcptt-video — 영상 그룹 여부. */
    val video: Boolean,
    /** session-type — prearranged/chat. */
    val sessionType: String?,
    val maxParticipants: Int?,
    val etag: String?,
) {
    companion object {
        private val entryRe = Regex("<entry\\s+uri=\"([^\"]+)\"[^>]*>(.*?)</entry>", RegexOption.DOT_MATCHES_ALL)
        private val nameRe = Regex("<(?:\\w+:)?display-name[^>]*>(.*?)</(?:\\w+:)?display-name>")
        private val roleRe = Regex("<(?:\\w+:)?participant-type>\\s*(\\w+)")
        private val prioRe = Regex("<(?:\\w+:)?user-priority>\\s*(\\d+)")

        private fun unescape(s: String) = s
            .replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"")
            .replace("&apos;", "'").replace("&amp;", "&")

        /** OMA POC groups XML(TS 24.481) 파싱 — 서버(csc get_group_xml)와 정합하는 관용 파서. */
        fun parse(uri: String, xml: String, etag: String?): GroupDoc {
            val members = entryRe.findAll(xml).map { m ->
                val body = m.groupValues[2]
                GroupMember(
                    uri = m.groupValues[1],
                    name = nameRe.find(body)?.groupValues?.get(1)?.let { unescape(it.trim()) }?.ifBlank { null },
                    role = roleRe.find(body)?.groupValues?.get(1) ?: "participant",
                    priority = prioRe.find(body)?.groupValues?.get(1)?.toIntOrNull(),
                )
            }.toList()
            // 그룹 display-name = <entry> 밖(list-service 직하) 첫 display-name
            val headName = nameRe.find(xml.substringBefore("<list>"))?.groupValues?.get(1)
                ?.let { unescape(it.trim()) }?.ifBlank { null }
            return GroupDoc(
                uri = uri,
                displayName = headName,
                members = members,
                priority = Regex("group-priority>\\s*(\\d+)").find(xml)?.groupValues?.get(1)?.toIntOrNull(),
                video = Regex("mcptt-video>\\s*true").containsMatchIn(xml),
                sessionType = Regex("session-type>\\s*(\\w+)").find(xml)?.groupValues?.get(1),
                maxParticipants = Regex("max-participant-count>\\s*(\\d+)").find(xml)?.groupValues?.get(1)?.toIntOrNull(),
                etag = etag,
            )
        }
    }
}

/** ETag 캐시가 적용되는 XCAP 문서 조회 결과. [notModified] 면 [body] 는 캐시 사용. */
data class XcapDoc(
    val notModified: Boolean,
    val body: String?,
    val etag: String?,
    val contentType: String?,
)

/** CSC 접속 설정. */
data class CscConfig(
    val host: String,
    val port: Int = 4430,
    val clientId: String = "MCPTT_UE",
    val redirectUri: String = "https://localhost/callback",
    val scope: String = "openid 3gpp:mcptt:ptt_server",
) {
    val baseUrl: String get() = "https://$host:$port"
}
