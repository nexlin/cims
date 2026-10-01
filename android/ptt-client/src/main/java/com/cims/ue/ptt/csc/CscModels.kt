package com.cims.ue.ptt.csc

// 화면이 보는 CSC 모델 — 문서 해석은 단말 SDK 코어가 한다(ue_sdk.md §5.3 P3). 여기는 SDK 값의 투영이다.

/** GMS 그룹 목록 항목 (GET /org.openmobilealliance.groups/users/{me}).
 *  우선순위·영상 여부 등 그룹 속성은 표준 경로인 그룹 문서(TS 24.481, [GroupDoc])에서 조회한다. */
data class GroupSummary(
    val uri: String,
    val displayName: String?,
    val etag: String?,
    val memberCount: Int?,
) {
    companion object {
        fun of(g: com.cims.ue.sdk.GroupSummary) = GroupSummary(
            g.uri, g.displayName.ifBlank { null }, g.etag.ifBlank { null }, g.memberCount.takeIf { it >= 0 })
    }
}

/** TS 24.481 그룹 문서의 멤버 `<entry>` — uri(tel:번호)·display-name·participant-type·user-priority. */
data class GroupMember(
    val uri: String,
    val name: String?,
    /** TS 24.380 participant-type — "chair" | "participant". */
    val role: String,
    /** TS 24.481 user-priority (발언권 우선순위, 0~255 클수록 높음). */
    val priority: Int?,
    /** 직함/직위 — CIMS 확장 `cims:user-title`(urn:cims:groupinfo:1.0, 3GPP 미정의). */
    val title: String? = null,
)

/** TS 24.481 그룹 문서(list-service) — 채널 상세 화면용 요약. SDK `GroupDoc` 에서 만든다([of]). */
data class GroupDoc(
    val uri: String,
    val displayName: String?,
    val members: List<GroupMember>,
    /** on-network-group-priority. */
    val priority: Int?,
    /** 그룹 종류 prearranged/chat — 문서의 on-network-invite-members(TS 24.481 §7.2.2 a). */
    val sessionType: String?,
    val maxParticipants: Int?,
    /** mcdata-on-network-max-data-size-auto-recv — 파일(FD) 자동 다운로드 임계 octets (TS 24.481). */
    val autoRecvBytes: Int? = null,
    val etag: String?,
    /** MCVideo 몫(TS 24.481 MCVideo `<service>`) — null 이면 MCVideo 그룹이 아니다(영상 참여 없음, mcvideo.md §7 D4). */
    val mcvideo: McVideoAttrs? = null,
) {
    companion object {
        fun of(d: com.cims.ue.sdk.GroupDoc) = GroupDoc(
            uri = d.uri,
            displayName = d.displayName.ifBlank { null },
            members = d.members.map { m ->
                GroupMember(m.uri, m.name.ifBlank { null }, m.role, m.priority, m.title.ifBlank { null })
            },
            priority = d.priority,
            sessionType = d.sessionType.ifBlank { null },
            maxParticipants = d.maxParticipants.takeIf { it > 0 },
            autoRecvBytes = d.maxAutoRecv,
            etag = d.etag.ifBlank { null },
            mcvideo = d.mcvideo?.let { McVideoAttrs(prearranged = it.inviteMembers, maxTransmitters = it.maxTransmitters) },
        )
    }
}

/** 그룹 문서의 MCVideo 몫 요약 — 호 종류(`mcvideo-on-network-invite-members`, TS 24.281 §6.3.5.2)·동시 송출 상한. */
data class McVideoAttrs(
    /** true = prearranged(제어 기능이 affiliate 한 멤버 전원 초대), false = chat(원하는 사람만 — 합류가 affiliation). */
    val prearranged: Boolean,
    /** mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members — null = 미기재. */
    val maxTransmitters: Int?,
)

/** CSC 접속 설정. */
data class CscConfig(
    val host: String,
    val port: Int = 4430,
)
