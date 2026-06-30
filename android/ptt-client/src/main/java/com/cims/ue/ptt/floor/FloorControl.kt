package com.cims.ue.ptt.floor

/**
 * MCPTT Floor Control 메시지 코딩 — **3GPP TS 24.380 §8 (Media Plane Control)** 정합.
 *
 * 전송: RTCP APP 패킷(RFC 3550), PT=204, name="MCPT", 5비트 subtype 가 **메시지 타입**을 운반한다.
 * 본문은 floor control specific field 들의 나열이며 각 필드는 `Field ID(8) + Length(8) + value(Length)`.
 *
 * 정렬 규칙(TS 24.380 §8.2.3): **가변 길이 문자열 필드**(User ID/Granted Party's Identity/
 * Queued User ID/Track Info)만 32비트(4바이트) 경계로 패딩하고, 고정 길이 필드는 패딩 없이
 * `2+Length` 만큼 전진한다. 전체 패킷은 32비트 경계(length=words-1).
 *
 * ※ 이 코덱은 CMP 의 현행 simplified 포맷이 아니라 **TS 24.380 규격**을 따른다(서버측 정렬은 별도 작업).
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

    fun name(t: Int): String = when (t) {
        REQUEST -> "Request"; GRANTED -> "Granted"; TAKEN -> "Taken"; DENY -> "Deny"
        RELEASE -> "Release"; IDLE -> "Idle"; REVOKE -> "Revoke"
        QUEUE_POS_REQUEST -> "QueuePosRequest"; QUEUE_POS_INFO -> "QueuePosInfo"; ACK -> "Ack"
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

    /** 32비트 패딩이 적용되는 가변 길이 문자열 필드(TS 24.380 §8.2.3). */
    val STRING_FIELDS = setOf(GRANTED_PARTY, USER_ID, QUEUED_USER_ID, TRACK_INFO)
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

/** 하나의 floor control 필드. [value] 는 패딩을 제외한 실제 값 바이트. */
data class FloorField(val id: Int, val value: ByteArray) {
    /** uint16 (big-endian) 해석 — Duration/Cause/Source/Indicator/Permission/QueueSize 등 고정 2옥텟 필드. */
    fun asU16(): Int = if (value.size >= 2) ((value[0].toInt() and 0xff) shl 8) or (value[1].toInt() and 0xff)
    else if (value.size == 1) value[0].toInt() and 0xff else 0

    /** UTF-8 문자열 — User ID/Granted Party's Identity 등. */
    fun asString(): String = String(value, Charsets.UTF_8)

    override fun equals(other: Any?): Boolean =
        other is FloorField && id == other.id && value.contentEquals(other.value)

    override fun hashCode(): Int = 31 * id + value.contentHashCode()
}

/** 파싱·생성된 floor control 메시지. */
data class FloorMessage(
    val type: Int,
    val ssrc: Long,
    val fields: List<FloorField>,
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

    fun typeName(): String = FloorMsgType.name(type)
}
