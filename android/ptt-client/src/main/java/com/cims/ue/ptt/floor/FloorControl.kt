package com.cims.ue.ptt.floor

/**
 * MCPTT Floor Control 메시지 코딩 — **3GPP TS 24.380 §8 (Media Plane Control)** 정합.
 *
 * 전송: RTCP APP 패킷(RFC 3550), PT=204, name="MCPT", 5비트 subtype 가 **메시지 타입**을 운반한다.
 * 본문은 floor control specific field 들의 나열이며 각 필드는 `Field ID(8) + Length(8) + value(Length)`.
 *
 * 정렬 규칙(TS 24.380 §8.1.3): **모든 필드**가 헤더(ID+Length)와 값의 합을 32비트(4바이트)
 * 경계로 패딩한다 — 고정 2옥텟 값 필드는 2+2=4 라 패딩이 0 이고, 가변 길이 필드(User ID/
 * Granted Party's Identity/Queued User ID/Track Info/List of Granted Users)만 실제로 채워진다.
 * 헤더가 12옥텟이므로 이 규칙만 지키면 모든 필드가 4옥텟 경계에서 시작해 **미지 필드도 건너뛸 수
 * 있다**(§8.1.4). 전체 패킷은 32비트 경계(length=words-1).
 *
 * 서버(CMP `cmp/PFloorCodec.cpp`)와 동일 규약이다.
 */

/** Floor control 메시지 타입 = RTCP APP subtype (TS 24.380 Table 8.2.2-1). */
object FloorMsgType {
    const val REQUEST = 0             // Floor Request        (UE→서버)
    const val GRANTED = 1             // Floor Granted        (서버→UE)
    const val TAKEN = 2               // Floor Taken          (서버→ALL)
    const val DENY = 3                // Floor Deny           (서버→UE)
    const val RELEASE = 4             // Floor Release        (UE→서버)
    const val IDLE = 5                // Floor Idle           (서버→ALL)
    const val REVOKE = 6              // Floor Revoke         (서버→화자)
    const val QUEUE_POS_REQUEST = 8   // Floor Queue Position Request (UE→서버)
    const val QUEUE_POS_INFO = 9      // Floor Queue Position Info    (서버→UE)
    const val ACK = 10                // Floor Ack
    const val MEDIA_FLOW = 0x0B       // Unicast Media Flow Control (UE→서버)
    const val QUEUED_CANCEL = 0x0E    // Queued Floor Requests (양방향)
    const val RELEASE_MULTI = 0x0F    // Floor Release Multi Talker (서버→UE 전용)

    /** subtype 첫 비트 = "Acknowledgment is required" 변종 (§8.2.2 Table 8.2.2.1-1).
     *  Granted/Taken/Deny/Release/Idle/QueuePosInfo 등에 정의된다 — 수신 시 이 비트를 걷어내
     *  기본 타입으로 처리하고 Floor Ack 로 회신해야 한다. */
    const val ACK_REQUIRED_BIT = 0x10

    /** subtype → 기본 메시지 타입(ack 요구 비트 제거). */
    fun op(subtype: Int): Int = subtype and 0x0f

    fun name(t: Int): String = when (op(t)) {
        REQUEST -> "Request"; GRANTED -> "Granted"; TAKEN -> "Taken"; DENY -> "Deny"
        RELEASE -> "Release"; IDLE -> "Idle"; REVOKE -> "Revoke"
        QUEUE_POS_REQUEST -> "QueuePosRequest"; QUEUE_POS_INFO -> "QueuePosInfo"; ACK -> "Ack"
        MEDIA_FLOW -> "MediaFlowControl"; QUEUED_CANCEL -> "QueuedFloorRequests"
        RELEASE_MULTI -> "ReleaseMultiTalker"
        else -> "Unknown($t)"
    }
}

/** Floor control field ID (TS 24.380 §8.2.3). */
object FloorFieldId {
    const val PRIORITY = 0
    const val DURATION = 1
    const val REJECT_CAUSE = 2
    const val QUEUE_INFO = 3
    const val GRANTED_PARTY = 4
    const val PERMISSION = 5
    const val USER_ID = 6
    const val QUEUE_SIZE = 7
    const val MSG_SEQ = 8
    const val QUEUED_USER_ID = 9
    const val SOURCE = 10
    const val TRACK_INFO = 11
    const val MSG_TYPE = 12
    const val FLOOR_INDICATOR = 13
    const val SSRC = 14
    const val GRANTED_USERS = 15      // List of Granted Users (동시 발언, 문자열 리스트)
    const val SSRC_LIST = 16          // List of SSRCs         (동시 발언, 화자 순서 동일)
    const val QUEUED_PURPOSE = 21     // Queued Floor Requests Purpose (0 취소/1 결과/2 통지)
    const val QUEUED_USERS = 22       // List of Queued Users
    const val QUEUED_RESULT = 23      // Queued Floor Requests Result
    const val MEDIA_FLOW = 24         // Media Flow Control Indicator (MSB=1 재개 / 0 중단)

    /** 32비트 패딩이 적용되는 가변 길이 문자열 필드(TS 24.380 §8.2.3). */
    val STRING_FIELDS = setOf(GRANTED_PARTY, USER_ID, QUEUED_USER_ID, TRACK_INFO)
}

/** Floor Ack 의 Source 필드 값 (TS 24.380 §8.2.3.12) — "누가 보낸 확인인지". */
object FloorSource {
    const val PARTICIPANT = 0         // floor participant = 단말
    const val PARTICIPATING = 1
    const val CONTROLLING = 2         // CMP = controlling MCPTT function 의 미디어 평면
    const val NON_CONTROLLING = 3
}

/** Permission to Request the Floor 값 (TS 24.380 §8.2.3.7) — Floor Taken 수신자의 발언 요청 가부. */
object FloorPermission {
    const val DENIED = 0              // broadcast 그룹·ambient(recv_only) 청취 leg
    const val ALLOWED = 1
}

/** Queued Floor Requests Purpose (TS 24.380 §8.2.3.23) — 0x0E 메시지의 용도. */
object FloorQueuedPurpose {
    const val CANCEL_REQUEST = 0      // UE→서버: 대기 요청 취소
    const val CANCEL_RESULT = 1       // 서버→요청자: 취소 결과
    const val CANCEL_NOTIFY = 2       // 서버→대기자: 네 요청이 취소됐다
}

/** Queued Floor Requests Result (TS 24.380 §8.2.3.25) — Cancel Result 의 결과 코드. */
object FloorQueuedResult {
    val TEXT = mapOf(
        0 to "취소됨", 2 to "대기열이 비어 있음", 3 to "대기 요청 없음", 5 to "일부만 취소됨",
    )
}

/** Reject/Revoke 원인 코드 (TS 24.380 §8.2.3.4 / §8.2.3.x). */
object FloorCause {
    val REJECT = mapOf(
        1 to "Another MCPTT client has permission", 2 to "Internal floor control server error",
        3 to "Only one participant", 4 to "Retry-after timer has not expired",
        5 to "Receive only", 6 to "No resources available", 7 to "Queue full", 255 to "Other reason",
    )
    val REVOKE = mapOf(
        1 to "Only one MCPTT client", 2 to "Media burst too long", 3 to "No permission to send a Media Burst",
        4 to "Media Burst pre-empted", 6 to "No resources available", 255 to "Other reason",
    )
}

/** Floor Indicator 비트마스크 (TS 24.380 §8.2.3.13). */
object FloorIndicator {
    const val NORMAL = 0x8000
    const val BROADCAST_GROUP = 0x4000
    const val SYSTEM = 0x2000
    const val EMERGENCY = 0x1000
    const val IMMINENT_PERIL = 0x0800
    const val QUEUEING = 0x0400
    const val DUAL_FLOOR = 0x0200
    const val TEMPORARY_GROUP = 0x0100
    const val MULTI_TALKER = 0x0080
}

/**
 * 발언 중인 화자 한 명. [ssrc] 는 그 화자의 RTP SSRC — 동시 발언에서 수신 스트림을 화자별로
 * 가르는 키다(TS 24.380 §6.2.4.3.4 NOTE: 믹싱은 단말 media mixer 몫). [self]=나.
 */
data class FloorTalker(val id: String, val ssrc: Long? = null, val self: Boolean = false)

/** 하나의 floor control 필드. [value] 는 패딩을 제외한 실제 값 바이트. */
data class FloorField(val id: Int, val value: ByteArray) {
    /** uint16 (big-endian) 해석 — Duration/Cause/Source/Indicator/Permission/QueueSize 등 고정 2옥텟 필드. */
    fun asU16(): Int = if (value.size >= 2) ((value[0].toInt() and 0xff) shl 8) or (value[1].toInt() and 0xff)
    else if (value.size == 1) value[0].toInt() and 0xff else 0

    /** UTF-8 문자열 — User ID/Granted Party's Identity 등. */
    fun asString(): String = String(value, Charsets.UTF_8)

    /** uint32 (big-endian) 선두 4옥텟 — SSRC 필드(§8.2.3.16: SSRC 4옥텟 + spare 2옥텟). */
    fun asU32(): Long = if (value.size >= 4)
        ((value[0].toLong() and 0xff) shl 24) or ((value[1].toLong() and 0xff) shl 16) or
            ((value[2].toLong() and 0xff) shl 8) or (value[3].toLong() and 0xff)
    else 0L

    /** List of Granted Users(§8.2.3.17) — No of users(1) + [len(1)+UTF-8]* . */
    fun asUserList(): List<String> {
        if (value.isEmpty()) return emptyList()
        val n = value[0].toInt() and 0xff
        val out = ArrayList<String>(n)
        var p = 1
        repeat(n) {
            if (p >= value.size) return out
            val l = value[p].toInt() and 0xff
            if (p + 1 + l > value.size) return out
            out.add(String(value, p + 1, l, Charsets.UTF_8))
            p += 1 + l
        }
        return out
    }

    /** List of SSRCs(§8.2.3.18) — Number of SSRCs(1) + spare(1) + SSRC(4)* . */
    fun asSsrcList(): List<Long> {
        if (value.size < 2) return emptyList()
        val n = value[0].toInt() and 0xff
        val out = ArrayList<Long>(n)
        var p = 2
        repeat(n) {
            if (p + 4 > value.size) return out
            out.add(((value[p].toLong() and 0xff) shl 24) or ((value[p + 1].toLong() and 0xff) shl 16) or
                ((value[p + 2].toLong() and 0xff) shl 8) or (value[p + 3].toLong() and 0xff))
            p += 4
        }
        return out
    }

    override fun equals(other: Any?): Boolean =
        other is FloorField && id == other.id && value.contentEquals(other.value)

    override fun hashCode(): Int = 31 * id + value.contentHashCode()
}

/**
 * 파싱·생성된 floor control 메시지.
 *
 * [type] 은 **ack 요구 비트를 걷어낸 기본 메시지 타입**이고, 그 비트는 [ackRequired] 로 분리해
 * 노출한다(§8.2.2) — 수신 시엔 Floor Ack 회신 여부의 판단 근거이고, 송신 시엔 subtype 에 다시
 * 실린다. 단말은 ack 를 요구하지 않으므로 송신은 항상 false.
 */
data class FloorMessage(
    val type: Int,
    val ssrc: Long,
    val fields: List<FloorField>,
    val ackRequired: Boolean = false,
) {
    fun field(id: Int): FloorField? = fields.firstOrNull { it.id == id }

    val userId: String? get() = field(FloorFieldId.USER_ID)?.asString()
    val grantedParty: String? get() = field(FloorFieldId.GRANTED_PARTY)?.asString()

    /** Floor Priority — 값 첫 옥텟(TS 24.380 §8.2.3.2, 2옥텟 중 우선순위는 MSB octet). */
    val priority: Int? get() = field(FloorFieldId.PRIORITY)?.value?.getOrNull(0)?.toInt()?.and(0xff)
    val durationSec: Int? get() = field(FloorFieldId.DURATION)?.asU16()
    val rejectCause: Int? get() = field(FloorFieldId.REJECT_CAUSE)?.asU16()
    val queuePosition: Int? get() = field(FloorFieldId.QUEUE_INFO)?.value?.getOrNull(0)?.toInt()?.and(0xff)
    val floorIndicator: Int? get() = field(FloorFieldId.FLOOR_INDICATOR)?.asU16()

    /** Permission to Request the Floor(5) — 0 이면 이 수신자는 발언 요청 불가([FloorPermission]). */
    val permission: Int? get() = field(FloorFieldId.PERMISSION)?.asU16()

    /** Message Sequence Number(8) — Taken/Idle 의 순서 식별(65535 순환). */
    val msgSeq: Int? get() = field(FloorFieldId.MSG_SEQ)?.asU16()

    /** SSRC(14) — **화자**의 RTP SSRC. 헤더 [ssrc] 는 서버 SSRC 라 화자 식별에 쓰면 안 된다(§8.2.5). */
    val speakerSsrc: Long? get() = field(FloorFieldId.SSRC)?.asU32()

    /** List of Granted Users(15) — 동시 발언 시 화자 목록. [ssrcList] 와 순서가 대응한다. */
    val grantedUsers: List<String> get() = field(FloorFieldId.GRANTED_USERS)?.asUserList() ?: emptyList()
    val ssrcList: List<Long> get() = field(FloorFieldId.SSRC_LIST)?.asSsrcList() ?: emptyList()

    /** Queued Floor Requests(0x0E)의 Purpose(21)/Result(23) — [FloorQueuedPurpose]/[FloorQueuedResult]. */
    val queuedPurpose: Int? get() = field(FloorFieldId.QUEUED_PURPOSE)?.asU16()
    val queuedResult: Int? get() = field(FloorFieldId.QUEUED_RESULT)?.asU16()

    /**
     * 이 메시지가 알리는 화자 집합. 동시 발언이면 리스트 필드(15/16)를, 단일 화자면
     * Granted Party(4)+SSRC(14)를 쓴다 — 서버는 화자가 2명 이상일 때만 리스트를 싣는다.
     * [FloorTalker.self] 는 여기서 정하지 않는다(내 ID 를 모르므로 — `FloorClient` 가 표시).
     */
    val talkers: List<FloorTalker>
        get() {
            val users = grantedUsers
            if (users.isNotEmpty()) {
                val ssrcs = ssrcList
                return users.mapIndexed { i, u -> FloorTalker(u, ssrcs.getOrNull(i)) }
            }
            val one = grantedParty ?: userId ?: return emptyList()
            return listOf(FloorTalker(one, speakerSsrc))
        }

    fun typeName(): String = FloorMsgType.name(type)
}
