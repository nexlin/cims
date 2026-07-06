package com.cims.ue.ptt.mcptt

import org.w3c.dom.Element
import java.io.ByteArrayInputStream
import javax.xml.parsers.DocumentBuilderFactory

/**
 * MCPTT XML 본문 빌더/파서 — 3GPP TS 24.379(call/affiliation) · TS 24.481(GMS) 정합.
 *
 * 네임스페이스/요소는 서버 소스(csp/GroupCallService.cpp, csc) 와 정합 확인됨:
 *  - mcptt-info:    `urn:3gpp:ns:mcpttInfo:1.0`        (multipart `application/vnd.3gpp.mcptt-info+xml`)
 *  - resource-lists:`urn:ietf:params:xml:ns:resource-lists` + `urn:3gpp:ns:mcpttGroupInfo:1.0`
 *  - GMS group doc: `urn:oma:xml:poc:list-service`     (`application/vnd.oma.poc.groups+xml`)
 *
 * 파서는 javax.xml(DOM) — Android·JVM 양쪽 동작(단위테스트 가능).
 */
object McpttXml {

    const val NS_MCPTT_INFO = "urn:3gpp:ns:mcpttInfo:1.0"
    const val NS_GROUP_INFO = "urn:3gpp:ns:mcpttGroupInfo:1.0"
    const val NS_RESOURCE_LISTS = "urn:ietf:params:xml:ns:resource-lists"
    const val NS_POC_LIST = "urn:oma:xml:poc:list-service"
    const val NS_AFFILIATION = "urn:3gpp:ns:mcpttAffiliation:1.0"

    const val CT_MCPTT_INFO = "application/vnd.3gpp.mcptt-info+xml"
    const val CT_RESOURCE_LISTS = "application/resource-lists+xml"
    const val CT_AFFILIATION = "application/vnd.3gpp.mcptt-affiliation-command+xml"

    enum class SessionType(val v: String) { PREARRANGED("prearranged"), CHAT("chat"), BROADCAST("broadcast") }

    // ── 빌더: 키업 그룹 INVITE multipart 본문 ──

    /** `application/vnd.3gpp.mcptt-info+xml` 본문 (TS 24.379 §F.1).
     *  [emergency]/[imminentPeril] — null=미기재, true=상향, false=명시 하향(긴급 취소 re-INVITE). */
    fun mcpttInfo(
        sessionType: SessionType,
        requestUri: String,
        callingUserId: String,
        callingGroupId: String,
        emergency: Boolean? = null,
        imminentPeril: Boolean? = null,
    ): String = buildString {
        append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        append("<mcpttinfo xmlns=\"$NS_MCPTT_INFO\">\n  <mcptt-Params>\n")
        append("    <session-type>${sessionType.v}</session-type>\n")
        append("    <mcptt-request-uri>${esc(requestUri)}</mcptt-request-uri>\n")
        append("    <mcptt-calling-user-id>${esc(callingUserId)}</mcptt-calling-user-id>\n")
        append("    <mcptt-calling-group-id>${esc(callingGroupId)}</mcptt-calling-group-id>\n")
        emergency?.let { append("    <emergency-ind>$it</emergency-ind>\n") }
        imminentPeril?.let { append("    <imminentperil-ind>$it</imminentperil-ind>\n") }
        append("  </mcptt-Params>\n</mcpttinfo>\n")
    }

    /** `application/resource-lists+xml` 본문 (TS 24.379) — 임시그룹/대상 멤버. */
    fun resourceLists(members: List<ResourceEntry>): String = buildString {
        append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        append("<resource-lists xmlns=\"$NS_RESOURCE_LISTS\" xmlns:mcpttgi=\"$NS_GROUP_INFO\">\n  <list>\n")
        for (m in members) {
            append("    <entry uri=\"${esc(m.uri)}\">\n")
            append("      <mcpttgi:participant-type>${esc(m.participantType)}</mcpttgi:participant-type>\n")
            append("      <mcpttgi:user-priority>${m.userPriority}</mcpttgi:user-priority>\n")
            append("    </entry>\n")
        }
        append("  </list>\n</resource-lists>\n")
    }

    /** `application/vnd.3gpp.mcptt-affiliation-command+xml` 본문 (TS 24.379 §F.3). de-affiliate=Expires:0 동반. */
    fun affiliationCommand(groupUri: String, affiliate: Boolean): String = buildString {
        append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        append("<affiliation-command xmlns=\"$NS_AFFILIATION\">\n  <actions>\n")
        if (affiliate) append("    <affiliate group=\"${esc(groupUri)}\"/>\n")
        else append("    <de-affiliate group=\"${esc(groupUri)}\"/>\n")
        append("  </actions>\n</affiliation-command>\n")
    }

    data class ResourceEntry(val uri: String, val participantType: String = "participant", val userPriority: Int = 0)

    // ── 파서: GMS 그룹 문서 (OMA POC list-service) ──

    data class GroupDoc(
        val uri: String?,
        val displayName: String?,
        val sessionType: String?,
        val maxParticipants: Int?,
        val requireAffiliation: Boolean,
        val members: List<GroupMember>,
    )

    data class GroupMember(val uri: String, val displayName: String?, val participantType: String?, val userPriority: Int?)

    /** OMA POC `list-service` 그룹 문서 파싱 (TS 24.481 / `urn:oma:xml:poc:list-service`). */
    fun parseGroupDoc(xml: String): GroupDoc {
        val doc = parse(xml)
        val ls = doc.getElementsByTagNameNS(NS_POC_LIST, "list-service").item(0) as? Element
        val members = ArrayList<GroupMember>()
        val entries = doc.getElementsByTagNameNS(NS_RESOURCE_LISTS, "entry")
        // resource-lists 가 없을 수도 있어 list-service 내 entry 도 폭넓게 수집
        val allEntries = if (entries.length > 0) entries else doc.getElementsByTagName("entry")
        for (i in 0 until allEntries.length) {
            val e = allEntries.item(i) as? Element ?: continue
            members.add(
                GroupMember(
                    uri = e.getAttribute("uri"),
                    displayName = firstText(e, "display-name"),
                    participantType = firstTextNs(e, NS_GROUP_INFO, "participant-type"),
                    userPriority = firstTextNs(e, NS_GROUP_INFO, "user-priority")?.toIntOrNull(),
                ),
            )
        }
        return GroupDoc(
            uri = ls?.getAttribute("uri"),
            displayName = ls?.let { firstText(it, "display-name") },
            sessionType = firstTextNs(doc.documentElement, NS_GROUP_INFO, "session-type"),
            maxParticipants = firstTextNs(doc.documentElement, NS_GROUP_INFO, "on-network-max-participant-count")?.toIntOrNull(),
            requireAffiliation = firstTextNs(doc.documentElement, NS_GROUP_INFO, "on-network-require-affiliation")?.toBoolean() ?: false,
            members = members,
        )
    }

    // ── 내부 헬퍼 ──

    private fun parse(xml: String): org.w3c.dom.Document {
        val f = DocumentBuilderFactory.newInstance().apply {
            isNamespaceAware = true
            runCatching { setFeature("http://apache.org/xml/features/disallow-doctype-decl", true) }
        }
        return f.newDocumentBuilder().parse(ByteArrayInputStream(xml.toByteArray(Charsets.UTF_8)))
    }

    private fun firstText(scope: Element, local: String): String? {
        val nl = scope.getElementsByTagName(local)
        for (i in 0 until nl.length) {
            val e = nl.item(i) as? Element ?: continue
            if (e.localName == local || e.tagName == local || e.tagName.endsWith(":$local")) return e.textContent?.trim()
        }
        return null
    }

    private fun firstTextNs(scope: Element, ns: String, local: String): String? {
        val nl = scope.getElementsByTagNameNS(ns, local)
        return (nl.item(0) as? Element)?.textContent?.trim()
    }

    private fun esc(s: String): String =
        s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;")
}
