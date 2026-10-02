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

    /** 회선 카드 제목 옆의 보조 글자 — 이동 / 유선. PTT 는 없다. */
    fun note(kind: String): String = when (kind) {
        VOLTE -> "이동"
        VOIP -> "유선"
        else -> ""
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
data class ServiceRef(val kind: String, val name: String, val domain: String = "") {
    /** 고르는 칸의 글자 — "이름 (도메인)". */
    val label: String get() = if (domain.isNotBlank()) "$name ($domain)" else name
}

/** 조직 노드 — 코드/이름/상위/정렬. */
data class OrgNode(val code: String, val name: String, val parent: String = "", val sort: Int = 0)

/**
 * 조직 트리를 **깊이 우선**으로 평탄화 — 부모 다음에 그 자식들, 형제는 `sort`·이름 순(순수 함수, 시험 대상). 관리 화면의 조직
 * 트리와 주소록 거르기 칩이 같은 순서를 본다(§6.2b·§6.13).
 *
 * 부모가 목록에 없으면(범위로 잘린 트리 — 주소록은 내가 볼 수 있는 조직만 온다) 그 노드를 뿌리로 세운다. 상위 고리가 있으면 남은
 * 것을 뿌리로 올려 잃지 않는다. 짝의 두 번째 값은 깊이(뿌리 = 0)다.
 */
fun flattenOrgs(orgs: List<OrgNode>): List<Pair<OrgNode, Int>> {
    val codes = orgs.mapTo(HashSet()) { it.code }
    val byParent = orgs.groupBy { if (it.parent in codes) it.parent else "" }
    val out = ArrayList<Pair<OrgNode, Int>>()
    val seen = HashSet<String>()
    fun walk(parent: String, depth: Int) {
        byParent[parent].orEmpty().sortedWith(compareBy({ it.sort }, { it.name })).forEach {
            if (seen.add(it.code)) { out.add(it to depth); walk(it.code, depth + 1) }
        }
    }
    walk("", 0)
    orgs.forEach { if (seen.add(it.code)) out.add(it to 0) }
    return out
}

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

/**
 * 전화번호부 한 줄 — `GET /provisioning/directory?service=volte|ptt` 의 `entries[]`, 또는 로컬 CSV 줄(§6.2b).
 * [external] = CSV 의 외부망 번호(서버 가입자가 아니다) — 문자·전달의 외부망 판정이 이것을 본다.
 */
data class DirectoryEntry(val org: String, val name: String, val msisdn: String, val external: Boolean = false)

/** 전화번호부 한 벌. ETag 로 버전을 맞춘다(304 면 내용 유지). */
/** 홈 국가 번호 — 프로파일의 `countryCode`(세션이 로그인 때 적는다). 번호의 **표시**만 가른다([DirectoryBook.displayNumber]). */
object HomeCountry {
    @Volatile var code: String = "82"
}

/** 표시용 번호([DirectoryBook.displayNumber]) — 화면이 번호를 적을 때 쓴다. */
fun localNumber(number: String?): String = DirectoryBook.displayNumber(number.orEmpty())

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
        // 숫자가 없는 신원(영숫자 id)은 정규형이 비어 서로 같아진다 — 그런 것은 원문으로 견준다(엉뚱한 사람의 이름이 서지 않게)
        if (key.isEmpty()) return number.trim().let { raw -> entries.firstOrNull { it.msisdn.trim() == raw }?.name.orEmpty() }
        return entries.firstOrNull { normalize(it.msisdn) == key }?.name.orEmpty()
    }

    /**
     * 번호를 치는 동안의 제안 — 이름에 들었거나(대소문자 무시) 친 숫자가 번호의 **어느 표기에든** 들었으면 앞에서부터
     * [limit] 개. 표기는 셋이다: 저장된 그대로 · E.164(`+8210…`) · 국내 로컬(`010…`) — 치다 만 `010333` 이 `+821033334444`
     * 로 저장된 사람에게 걸려야 한다(부분 입력은 정규형으로 올릴 수 없다). URI 를 치는 중이면(`:`) 제안하지 않는다 —
     * 주소록 번호가 아니다(데스크톱 `CallOriginateViewModel.UpdateSuggestions` 와 같은 규칙).
     */
    fun suggest(query: String, limit: Int = 8, countryCode: String = HomeCountry.code): List<DirectoryEntry> {
        val q = query.trim()
        if (q.isEmpty() || ':' in q) return emptyList()
        val qd = q.filter { it.isDigit() || it == '+' }
        return entries.asSequence().filter { e ->
            e.name.contains(q, ignoreCase = true) ||
                qd.isNotEmpty() && numberForms(e.msisdn, countryCode).any { it.contains(qd) }
        }.take(limit).toList()
    }

    companion object {
        /** 한 번호의 표기 셋 — 저장된 그대로(숫자·`+`) · E.164 · 국내 로컬(`+<cc>…` → `0…`). */
        internal fun numberForms(msisdn: String, countryCode: String = HomeCountry.code): Set<String> {
            val raw = msisdn.filter { it.isDigit() || it == '+' }
            val e164 = normalize(msisdn, countryCode)
            val local = if (e164.startsWith("+$countryCode") && e164.length > countryCode.length + 2)
                "0" + e164.drop(countryCode.length + 1) else raw
            return setOf(raw, e164, local).filterTo(LinkedHashSet()) { it.isNotEmpty() }
        }

        /**
         * 비교 정규형 — 숫자·`+` 만 남기고 국내 로컬 표기(`010…`)는 E.164 로 올린다.
         *
         * 전화번호부의 `01012345678` 과 서버의 `+821012345678` 이 한 사람이 되게 한다.
         * 내선처럼 짧은 번호(6자리 이하)는 그대로 둔다 — 국가코드를 붙이면 다른 번호가 된다.
         */
        /**
         * **표시용** 번호 — 홈 국가 번호는 국내 표기(`+8210…` → `010…`), 그 밖(국제·내선·그룹 id)은 그대로(데스크톱
         * `DirectoryService.DisplayNumber`). 다이얼·비교·키에는 쓰지 않는다 — 그쪽은 원 번호와 [normalize] 다.
         */
        fun displayNumber(number: String, countryCode: String = HomeCountry.code): String =
            if (countryCode.isNotEmpty() && number.startsWith("+$countryCode") && number.length > countryCode.length + 2)
                "0" + number.substring(countryCode.length + 1)
            else number

        fun normalize(s: String, countryCode: String = HomeCountry.code): String {
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
    /** 태블릿에 채널이 있는가 — 멤버(① 카드)·청취 범위(② 행). 관리 범위만 있는 그룹에는 [채널로] 가 없다. */
    val hasChannel: Boolean get() = isMember || inListenScope

    /** 관계 배지 — 멤버 › 청취 범위 › 소유 › 범위 순으로 하나만 고른다(§4.7). */
    val relation: String get() = when {
        isMember -> "멤버"
        inListenScope -> "청취 범위"
        isOwner -> "소유"
        else -> "범위"
    }
}

// ── 이력 화면(§4.6) ──────────────────────────────────────────────────────────

/** 이력 종류 — [label] 은 [이력] 도구줄의 세그먼트 낱말이다(관제 탭 줄 [무전|통화] 와 같다). */
enum class HistoryKind(val wire: String, val label: String) {
    CALL("call", "통화"),
    PTT("ptt", "무전"),
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
    /** 발언 턴 = 화자 구간 수(동시 발언 세그먼트는 턴이 여럿). 세션 인덱스 집계라 스캔 폴백이면 0 이다. */
    val turnCount: Int = 0,
    /** 항목에 `turnCount` 가 실려 왔다 — 없으면 [turnCount] 0 은 «발언 없음» 이 아니라 «모름» 이다. */
    val hasTurnCount: Boolean = false,
    val speakerCount: Int = 0,
    /** 발화 구간 합(겹침은 한 번) / [talkMs] = 화자별 누적. */
    val totalSpeechMs: Int = 0,
    val talkMs: Int = 0,
    val maxConcurrent: Int = 0,
    /** 세션 당시 floor 축 — on(반이중·무전) | off(전이중·통화형) | ""(미기록). */
    val floorControl: String = "",
    /** single | dual | multi (TS 24.380 동시 발언 정책). */
    val floorPolicy: String = "",
    val maxTalkers: Int = 0,
    /** 참여자(발언 안 한 참가자 포함) — 1:1·애드혹은 개시자를 뺀 나머지가 상대다. */
    val people: List<String> = emptyList(),
    /**
     * MC 서비스 — `ptt`(MCPTT 그룹 호) | `mcvideo`(MCVideo 그룹 호 — 같은 그룹의 영상 호). 서버가 싣지 않으면 ""(무전으로
     * 읽는다 — 그때는 녹취 메타의 `service` 로만 영상 세션을 안다).
     */
    val service: String = "",
    /** mcvideo: 호 방식 chat | prearranged · 동시 송출 상한(세션 당시 그룹 문서 값) — 없으면 ""/0. */
    val mcvSessionType: String = "",
    val mcvMaxTransmitters: Int = 0,
) {
    /** 시간대 밴드·필터 축 — 통화는 INVITE, PTT 는 세션 시작(서버 `hours` 와 같은 규칙). */
    val axisAtMs: Long get() = inviteAtMs ?: startAtMs ?: atMs
    /** 진행 중 — 통화 `active`/`ringing`, 무전은 `ptt.session.start` 이벤트로도 안다(폴링 항목). */
    val isLive: Boolean get() = state == "active" || state == "ringing" || event == "ptt.session.start"
    /** 전이중(통화형) 세션 — floor 축이 꺼져 있었다. */
    val isFullDuplex: Boolean get() = floorControl == "off"
    /** MCVideo 그룹 호(영상 세션) — 무전 목록에 같이 서되 «영상» 라벨, 세는 말은 발언이 아니라 송출(TS 24.581). */
    val isMcVideo: Boolean get() = service == "mcvideo"
    /** 영상 통화(VoLTE 영상). */
    val isVideoCall: Boolean get() = callType == "volte_video"
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

/** 슬롯 트랙 — 동시 발언·전이중이면 여럿. [kind] = audio | video(서버가 안 실으면 audio), [slot] = PTT 슬롯(VoIP 는 -1). */
data class SegmentTrack(val slot: Int, val kind: String = "audio", val speakers: List<SpeakerSpan> = emptyList(),
                        val hasVideo: Boolean = false, val status: String = "")

/** 녹취 세그먼트 — [type] = volte | ptt | mcvideo(MCVideo 송출 구간). */
data class RecordingSegment(
    val seq: Int, val type: String = "", val speakerId: String = "",
    val startAtMs: Long? = null, val endAtMs: Long? = null, val durationMs: Int = 0,
    val hasVideo: Boolean = false, val status: String = "",
    val speakerIds: List<String> = emptyList(), val talkerCount: Int = 0,
    val tracks: List<SegmentTrack> = emptyList(),
) {
    val playable: Boolean get() = status != "failed"
    /** 이 세그먼트의 MP4 에 영상이 있다 — 재생하면 영상 칸이 열린다(영상 통화 녹취·MCVideo 송출 구간). */
    val showsVideo: Boolean get() = hasVideo || tracks.any { it.kind == "video" }
}

/** 녹취 세션(`GET /provisioning/recordings/{id}`). */
data class RecordingInfo(
    val id: String, val callType: String = "", val caller: String = "", val callee: String = "",
    val groupId: String = "", val startAtMs: Long? = null, val endAtMs: Long? = null,
    val durationSec: Int = 0, val status: String = "", val segments: List<RecordingSegment> = emptyList(),
    /** PTT 세션의 MC 서비스 — `ptt` | `mcvideo`(세그먼트 = 송출 구간). 통화 녹취는 "". */
    val service: String = "",
) {
    /** 영상 세션 — 이력 항목에 서비스 축이 없어도 녹취 메타(`service`·세그먼트 `type`)로 안다. */
    val isMcVideo: Boolean get() = service == "mcvideo" || segments.any { it.type == "mcvideo" }
}

/**
 * 문자열 필드 — **JSON null 은 없는 것**이다. Android 의 `optString(name, fallback)` 은 값이 JSON null 이면 fallback 이 아니라
 * 글자 «null» 을 돌려준다(JVM 단위시험의 org.json 은 fallback 을 낸다 — 시험으로는 드러나지 않는다). 그대로 쓰면 이름·번호·화자
 * 자리에 «null» 이 선다. 서버 응답을 읽는 곳은 이것을 쓴다.
 */
internal fun org.json.JSONObject.str(name: String, fallback: String = ""): String =
    if (isNull(name)) fallback else optString(name, fallback)
