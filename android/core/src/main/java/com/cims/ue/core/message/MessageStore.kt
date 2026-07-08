package com.cims.ue.core.message

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** 문자 방향 — 수신/발신. */
enum class MsgDirection { IN, OUT }

/** 발신 문자 전송 상태 — [SENT]=완료(기본, C-plane 즉시), [PENDING]=미디어평면(MSRP) 전송 중,
 *  [FAILED]=실패(말풍선 탭으로 재전송). 수신 문자는 항상 SENT. */
enum class SendState { SENT, PENDING, FAILED }

/** 문자 한 건. [peer] 는 상대 번호 또는 그룹 ID(표시용, sip:/@도메인 제거됨), [time] 은 epoch millis.
 *  [sender] 는 그룹 수신 문자의 실제 발신자(1:1 은 빈 값), [msgId]/[delivered] 는 MCData SDS
 *  message ID·전달확인(disposition DELIVERED) 상태. */
data class MessageEntry(
    val peer: String,
    val text: String,
    val time: Long,
    val direction: MsgDirection,
    val read: Boolean,
    val sender: String = "",
    val msgId: String = "",
    val delivered: Boolean = false,
    val sendState: SendState = SendState.SENT,
    // MCData FD 첨부 (파일전송) — [attPath] 는 로컬 다운로드 완료 시 채워짐
    val attName: String = "",
    val attUrl: String = "",
    val attSize: Long = 0,
    val attPath: String = "",
)

/** 대화(스레드) 요약 — 상대별 마지막 문자 + 안읽음 수. */
data class MessageThread(val peer: String, val last: MessageEntry, val unread: Int)

/**
 * 문자(SIP MESSAGE) 인박스 영속화 — 상대 번호별 대화 스레드의 데이터 소스.
 * 추가 의존성 없이 SharedPreferences + JSON 으로 저장(CallLogStore 와 동일 방침).
 */
class MessageStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("sip_messages", Context.MODE_PRIVATE)

    /** 대화 스레드 목록(마지막 문자 최신순). */
    fun threads(): List<MessageThread> =
        all().groupBy { it.peer }.map { (peer, list) ->
            MessageThread(
                peer = peer,
                last = list.maxBy { it.time },
                unread = list.count { it.direction == MsgDirection.IN && !it.read },
            )
        }.sortedByDescending { it.last.time }

    /** 한 상대와의 대화(시간 오름차순). */
    fun thread(peer: String): List<MessageEntry> =
        all().filter { it.peer == peer }.sortedBy { it.time }

    /** 문자 추가(최대 [MAX] 건 유지). 발신은 읽음 처리, 수신은 안읽음으로 시작. */
    fun add(
        peer: String,
        text: String,
        direction: MsgDirection,
        time: Long = System.currentTimeMillis(),
        sender: String = "",
        msgId: String = "",
        attName: String = "",
        attUrl: String = "",
        attSize: Long = 0,
        attPath: String = "",
        sendState: SendState = SendState.SENT,
    ) {
        if (peer.isBlank() || (text.isBlank() && attName.isBlank())) return
        val list = all().toMutableList()
        list.add(MessageEntry(peer, text, time, direction, read = direction == MsgDirection.OUT,
            sender = sender, msgId = msgId, sendState = sendState,
            attName = attName, attUrl = attUrl, attSize = attSize, attPath = attPath))
        save(list.sortedByDescending { it.time }.take(MAX))
    }

    /** 발신 문자 전송 상태 갱신(PENDING→SENT/FAILED, 재전송 시 →PENDING). @return 실제 변경 여부. */
    fun setSendState(msgId: String, state: SendState): Boolean {
        if (msgId.isBlank()) return false
        val list = all()
        if (list.none { it.msgId == msgId && it.direction == MsgDirection.OUT && it.sendState != state })
            return false
        save(list.map {
            if (it.msgId == msgId && it.direction == MsgDirection.OUT) it.copy(sendState = state) else it
        })
        return true
    }

    /** FD 첨부 다운로드 완료 — 로컬 경로 기록. @return 실제 변경 여부. */
    fun setAttachmentPath(msgId: String, path: String): Boolean {
        if (msgId.isBlank()) return false
        val list = all()
        if (list.none { it.msgId == msgId && it.attPath != path }) return false
        save(list.map { if (it.msgId == msgId) it.copy(attPath = path) else it })
        return true
    }

    /** MCData SDS DELIVERED 통지 반영 — 발신 문자를 전달확인 상태로. @return 실제 변경 여부. */
    fun markDelivered(msgId: String): Boolean {
        if (msgId.isBlank()) return false
        val list = all()
        if (list.none { it.msgId == msgId && it.direction == MsgDirection.OUT && !it.delivered }) return false
        save(list.map {
            if (it.msgId == msgId && it.direction == MsgDirection.OUT) it.copy(delivered = true) else it
        })
        return true
    }

    /** 서비스 재기동으로 결과 이벤트를 못 받게 된 PENDING 발신을 실패로 마감(재전송 유도). */
    fun failStalePending(): Boolean {
        val list = all()
        if (list.none { it.direction == MsgDirection.OUT && it.sendState == SendState.PENDING }) return false
        save(list.map {
            if (it.direction == MsgDirection.OUT && it.sendState == SendState.PENDING)
                it.copy(sendState = SendState.FAILED) else it
        })
        return true
    }

    /** 대화 진입 시 그 상대의 수신 문자를 모두 읽음 처리. @return 실제 변경 여부(불변이면 저장 생략). */
    fun markRead(peer: String): Boolean {
        val list = all()
        if (list.none { it.peer == peer && !it.read }) return false
        save(list.map { if (it.peer == peer && !it.read) it.copy(read = true) else it })
        return true
    }

    /** 전체 안읽음 수(탭 배지용). */
    fun unreadTotal(): Int = all().count { it.direction == MsgDirection.IN && !it.read }

    /** 한 상대와의 대화 삭제. */
    fun clearThread(peer: String) = save(all().filterNot { it.peer == peer })

    private fun all(): List<MessageEntry> {
        val arr = runCatching { JSONArray(prefs.getString(KEY, "[]").orEmpty()) }
            .getOrDefault(JSONArray())
        return (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            MessageEntry(
                peer = o.optString("p"),
                text = o.optString("m"),
                time = o.optLong("ts"),
                direction = runCatching { MsgDirection.valueOf(o.optString("d")) }
                    .getOrDefault(MsgDirection.IN),
                read = o.optBoolean("r", true),
                sender = o.optString("s"),
                msgId = o.optString("mid"),
                delivered = o.optBoolean("dlv", false),
                sendState = runCatching { SendState.valueOf(o.optString("st")) }
                    .getOrDefault(SendState.SENT),
                attName = o.optString("an"),
                attUrl = o.optString("au"),
                attSize = o.optLong("al", 0),
                attPath = o.optString("ap"),
            )
        }
    }

    private fun save(list: List<MessageEntry>) {
        val arr = JSONArray()
        list.forEach {
            val o = JSONObject().put("p", it.peer).put("m", it.text).put("ts", it.time)
                .put("d", it.direction.name).put("r", it.read)
            if (it.sender.isNotEmpty()) o.put("s", it.sender)
            if (it.msgId.isNotEmpty()) o.put("mid", it.msgId)
            if (it.delivered) o.put("dlv", true)
            if (it.sendState != SendState.SENT) o.put("st", it.sendState.name)
            if (it.attName.isNotEmpty()) o.put("an", it.attName)
            if (it.attUrl.isNotEmpty()) o.put("au", it.attUrl)
            if (it.attSize > 0) o.put("al", it.attSize)
            if (it.attPath.isNotEmpty()) o.put("ap", it.attPath)
            arr.put(o)
        }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    private companion object {
        const val KEY = "entries"
        const val MAX = 500
    }
}
