// 관리 평면 자료 모형 — 조직·구성원·회선 / 이력·세션 상세 / 녹취
// (docs/design/features/android_dispatch_tablet.md §6.4, dispatch_desktop_ui.md §4.5·§4.6·§4.7)
//
// 전부 **서버 응답의 앱 표현**이다. 판정(범위·자격·충돌)은 서버가 하고 앱은 받은 것만 그린다.
// 서버 계약은 android_ue_provisioning.md §3-2/§3-2a/§3-3/§3-4.
package com.cims.ue.dispatch.session

// ── 관리 화면(§4.5) ──────────────────────────────────────────────────────────

/** 회선 종류 — 와이어의 한 축. `members[].{kind}` · `services.{kind}[]` · `PUT …/members/{id}/{kind}` 가 같은 값을 쓴다. */
object LineKind {
    const val VOLTE = "volte"      // 이동
    const val VOIP = "voip"        // 유선
    const val PTT = "ptt"
    val all = listOf(VOLTE, VOIP, PTT)

    fun label(kind: String): String = when (kind) {
        VOLTE -> "VoLTE 번호"
        VOIP -> "VoIP 번호"
        else -> "PTT 번호"
    }
}

/** SIP transport 선택지 — ANY 는 "가입자 override 없음"(서버 NULL)의 **명시값**이라 되돌릴 수 있다. */
val SIP_TRANSPORTS = listOf("TLS", "TCP", "UDP", "ANY")

/** 서버가 알려 준 내 관리 범위. 앱은 enum 을 해석하지 않는다 — 있는지 없는지만 본다. */
data class AdminScope(val groupId: String = "", val directoryWrite: String = "", val orgCode: String = "") {
    /** `own`|`all` 이면 관리 화면이 열린다. 빈 값·`none` 이면 잠긴다. */
    val canWrite: Boolean get() = directoryWrite.isNotBlank() && directoryWrite != "none"
}

/** 접속서비스 후보 한 건. */
data class ServiceRef(val kind: String, val name: String, val domain: String = "")

/** 조직 노드 — 코드/이름/상위/정렬. */
data class OrgNode(val code: String, val name: String, val parent: String = "", val sort: Int = 0)

/** 회선 한 개(종류당 첫 회선). `pickupGroup` 은 읽기 전용 — 콘솔 전화 그룹에서 서버가 파생한다. */
data class NumberInfo(
    val msisdn: String = "",
    val imsi: String = "",
    val serviceRef: String = "",
    val sipTransport: String = "",
    val authScheme: String = "",
    val profile: Map<String, Boolean> = emptyMap(),
    val pickupGroup: String = "",
) {
    val isEmpty: Boolean get() = msisdn.isBlank()
}

/** 구성원 한 명 + 종류별 첫 회선. */
data class MemberInfo(
    val userId: Long,
    val name: String,
    val loginId: String = "",
    val org: String = "",
    val title: String = "",
    val volte: NumberInfo? = null,
    val voip: NumberInfo? = null,
    val ptt: NumberInfo? = null,
) {
    fun line(kind: String): NumberInfo? = when (kind) {
        LineKind.VOLTE -> volte
        LineKind.VOIP -> voip
        else -> ptt
    }

    /** 그룹 생성 자격 — PTT 회선 프로파일에 실려 온다. */
    val allowCreateGroup: Boolean get() = ptt?.profile?.get("allowCreateGroup") == true
    /** 원격 청취 자격 — 표시만 한다(역할 배정의 결과라 앱에서 못 바꾼다, §4.5). */
    val allowAmbientListening: Boolean get() = ptt?.profile?.get("allowAmbientListening") == true
}

/** 관리 화면 한 벌(`GET /provisioning/directory/admin`). */
data class AdminView(
    val scope: AdminScope = AdminScope(),
    val services: List<ServiceRef> = emptyList(),
    val orgs: List<OrgNode> = emptyList(),
    val members: List<MemberInfo> = emptyList(),
    val etag: String = "",
) {
    /** 종류에 맞는 접속서비스 후보. */
    fun servicesOf(kind: String): List<ServiceRef> = services.filter { it.kind == kind }

    /** "CIMS › 제1본부 › 팀01" — 루트부터의 경로. 고리가 있으면 거기서 멈춘다. */
    fun orgPath(code: String): String {
        if (code.isBlank()) return ""
        val byCode = orgs.associateBy { it.code }
        val names = ArrayList<String>()
        var cur = byCode[code]
        val seen = HashSet<String>()
        while (cur != null && seen.add(cur.code)) {
            names.add(0, cur.name.ifBlank { cur.code })
            cur = byCode[cur.parent]
        }
        return names.joinToString(" › ")
    }

    /** code 와 그 하위 전부(빈 code = 전체 → null = 필터 없음). */
    fun orgSubtree(code: String): Set<String>? {
        if (code.isBlank()) return null
        val out = linkedSetOf(code)
        var added = true
        while (added) {
            added = false
            orgs.forEach { if (it.parent in out && out.add(it.code)) added = true }
        }
        return out
    }
}

// ── 회사 전화번호부(§4.7 멤버 후보) ─────────────────────────────────────────

/** 전화번호부 한 줄 — `GET /provisioning/directory?service=volte|ptt` 의 `entries[]`. */
data class DirectoryEntry(val org: String, val name: String, val msisdn: String)

/** 전화번호부 한 벌. ETag 로 버전을 맞춘다(304 면 내용 유지). */
data class DirectoryBook(
    val orgs: List<OrgNode> = emptyList(),
    val entries: List<DirectoryEntry> = emptyList(),
    val etag: String = "",
) {
    /** "CIMS › 본부 › 팀01" — 루트부터의 경로. 고리가 있으면 거기서 멈춘다. */
    fun orgPath(code: String): String {
        if (code.isBlank()) return ""
        val byCode = orgs.associateBy { it.code }
        val names = ArrayList<String>()
        var cur = byCode[code]
        val seen = HashSet<String>()
        while (cur != null && seen.add(cur.code)) {
            names.add(0, cur.name.ifBlank { cur.code })
            cur = byCode[cur.parent]
        }
        return names.joinToString(" › ")
    }

    /** 번호 → 이름. 없으면 빈 문자열. */
    fun nameOf(number: String): String {
        val key = normalize(number)
        return entries.firstOrNull { normalize(it.msisdn) == key }?.name.orEmpty()
    }

    companion object {
        /**
         * 비교 정규형 — 숫자·`+` 만 남기고 국내 로컬 표기(`010…`)는 E.164 로 올린다.
         *
         * 전화번호부의 `01012345678` 과 서버의 `+821012345678` 이 한 사람이 되게 한다.
         * 내선처럼 짧은 번호(6자리 이하)는 그대로 둔다 — 국가코드를 붙이면 다른 번호가 된다.
         */
        fun normalize(s: String, countryCode: String = "82"): String {
            val digits = s.filter { it.isDigit() || it == '+' }
            if (digits.isEmpty()) return ""
            if (digits.startsWith("+")) return digits
            if (digits.length <= 6) return digits
            if (digits.startsWith("0")) return "+" + countryCode + digits.drop(1)
            return digits
        }
    }
}

// ── PTT 그룹 화면(§4.7) ──────────────────────────────────────────────────────

/**
 * 관리 목록의 그룹 한 행(`GET /provisioning/directory/groups`).
 *
 * 원천은 **관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 내 멤버 그룹**이라 보기 전용 행이 섞인다 —
 * [편집]·[삭제]는 `canManage` 인 행에만 붙는다(서버 GMS 게이트와 같은 판정).
 */
data class ManagedGroup(
    val id: String,
    val uri: String,
    val name: String,
    val memberCount: Int = 0,
    val isOwner: Boolean = false,
    val orgCode: String = "",
    val sessionType: String = "",
    val etag: String = "",
    val canManage: Boolean = true,
    val inListenScope: Boolean = false,
    val isMember: Boolean = false,
) {
    /** 관계 배지 — 멤버 › 청취 범위 › 소유 › 범위 순으로 하나만 고른다(§4.7). */
    val relation: String get() = when {
        isMember -> "멤버"
        inListenScope -> "청취 범위"
        isOwner -> "소유"
        else -> "범위"
    }
}

// ── 이력 화면(§4.6) ──────────────────────────────────────────────────────────

enum class HistoryKind(val wire: String, val label: String) {
    CALL("call", "통화(VoLTE)"),
    PTT("ptt", "PTT 세션"),
    MESSAGE("message", "메시지"),
}

/**
 * 이력 한 건. 공통 필드 + 종류별 확장 필드(서버가 안 실으면 기본값).
 *
 * 진행 중 상태는 dialog/conference 구독이 정본이고 이 자료는 **끝난 일의 사본**이다.
 */
data class HistoryEntry(
    val id: String,
    val atMs: Long,
    val kind: HistoryKind,
    val event: String = "",
    val from: String = "",
    val to: String = "",
    val group: String = "",
    val durationSec: Int = 0,
    val emergency: Boolean = false,
    val text: String = "",
    val recordingId: String = "",
    val hasRecording: Boolean = false,
    // 통화 확장
    val state: String = "",
    val callType: String = "",
    val inviteAtMs: Long? = null,
    val answerAtMs: Long? = null,
    val endAtMs: Long? = null,
    val endReason: String = "",
    val sipStatus: Int = 0,
    // PTT 확장
    val sessionKind: String = "",
    val startAtMs: Long? = null,
    val groupName: String = "",
    val memberCount: Int = 0,
    val turnCount: Int = 0,
    val speakerCount: Int = 0,
    val totalSpeechMs: Int = 0,
    val talkMs: Int = 0,
    val maxConcurrent: Int = 0,
    val floorControl: String = "",
    val floorPolicy: String = "",
    val maxTalkers: Int = 0,
    val people: List<String> = emptyList(),
) {
    /** 시간대 밴드·필터 축 — 통화는 INVITE, PTT 는 세션 시작(서버 `hours` 와 같은 규칙). */
    val axisAtMs: Long get() = inviteAtMs ?: startAtMs ?: atMs
    val isActive: Boolean get() = state == "active" || state == "ringing"
    /** 전이중(통화형) 세션 — floor 축이 꺼져 있었다. */
    val isFullDuplex: Boolean get() = floorControl == "off"
}

/** 창 조회 한 페이지 — 항목(시각 오름차순) + 다음 커서 + 시간대 분포(HH → 건수). */
data class HistoryPage(
    val items: List<HistoryEntry> = emptyList(),
    val next: String = "",
    val hours: Map<String, Int> = emptyMap(),
)

/** PTT 세션 참여자(입퇴장 기록) — role = initiator|member. */
data class PttParticipant(val msisdn: String, val role: String = "",
                          val joinAtMs: Long? = null, val leaveAtMs: Long? = null)

/** PTT 세션 이벤트 — session_start/session_end/member_join/member_leave/member_invite/config_change. */
data class PttEvent(val atMs: Long?, val type: String, val member: String = "",
                    val role: String = "", val durationSec: Int? = null)

/** floor 중재 이벤트(TS 24.380 op 8종) — 부가 정보는 op 별로 채워진다. */
data class PttFloorEvent(
    val atMs: Long?, val op: String, val user: String = "",
    val slot: Int? = null, val prio: Int? = null, val talkers: Int? = null,
    val policy: String = "", val preempt: Boolean = false, val preemptedFrom: String = "",
    val reason: String = "", val cause: Int? = null, val owner: String = "",
    val pos: Int? = null, val qsize: Int? = null, val revoked: String = "",
    val removed: Int? = null, val graceSec: Int? = null, val idleMs: Int? = null,
    val preemptedBy: String = "",
)

/** PTT 세션 상세(`GET /provisioning/history/ptt/{recordingId}`). */
data class PttSessionDetail(
    val recordingId: String,
    val participants: List<PttParticipant> = emptyList(),
    val events: List<PttEvent> = emptyList(),
    val floor: List<PttFloorEvent> = emptyList(),
    val hasRecording: Boolean = false,
)

// ── 녹취(§4.6) ──────────────────────────────────────────────────────────────

/** 트랙 안 화자 한 구간 — 발언 턴의 원자다. */
data class SpeakerSpan(val id: String, val offsetMs: Int, val durMs: Int)

/** 슬롯 트랙 — 동시 발언·전이중이면 여럿. */
data class SegmentTrack(val slot: Int, val kind: String = "", val speakers: List<SpeakerSpan> = emptyList(),
                        val hasVideo: Boolean = false, val status: String = "")

data class RecordingSegment(
    val seq: Int, val type: String = "", val speakerId: String = "",
    val startAtMs: Long? = null, val endAtMs: Long? = null, val durationMs: Int = 0,
    val hasVideo: Boolean = false, val status: String = "",
    val speakerIds: List<String> = emptyList(), val talkerCount: Int = 0,
    val tracks: List<SegmentTrack> = emptyList(),
) {
    val playable: Boolean get() = status != "failed"
}

data class RecordingInfo(
    val id: String, val callType: String = "", val caller: String = "", val callee: String = "",
    val groupId: String = "", val startAtMs: Long? = null, val endAtMs: Long? = null,
    val durationSec: Int = 0, val status: String = "", val segments: List<RecordingSegment> = emptyList(),
)

/** 발언 타임라인의 막대 하나 — 세션 시작 기준 오프셋. 재생은 (seq, slot) 로 건다. */
data class TurnBar(val speaker: String, val offsetMs: Int, val durMs: Int, val seq: Int, val slot: Int?)
