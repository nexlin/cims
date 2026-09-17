// SDS 메시지 보관 (docs/design/features/android_dispatch_tablet.md §6.9, dispatch_desktop_ui.md §4.4)
//
// 데스크톱의 `MessageStore`(SQLite `messages.db`, 최근 30일) 대응. 스키마 의미도 같다.
// **Room 을 쓰지 않는다** — 표가 하나뿐이고 질의가 넷이라 애노테이션 처리기와 의존을 늘릴 값이 없다.
// `SettingsStore` 가 DataStore 대신 SharedPreferences 를 쓴 것과 같은 판단이다.
//
// 보관하지 않으면 앱을 껐다 켤 때마다 SDS 스레드가 통째로 사라진다 — 관제사는 «아까 뭐라고 했더라» 를
// 앱 밖에서 찾을 수 없다. 서버에 SDS 이력 API 가 없으므로 이 로컬 보관이 유일한 근거다.
package com.cims.ue.dispatch.session

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

class MessageStore(context: Context) :
    SQLiteOpenHelper(context.applicationContext, DB_NAME, null, DB_VERSION) {

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """
            CREATE TABLE IF NOT EXISTS messages (
              row_id    INTEGER PRIMARY KEY AUTOINCREMENT,
              id        TEXT NOT NULL,
              group_id  TEXT NOT NULL,
              from_uri  TEXT NOT NULL DEFAULT '',
              from_name TEXT NOT NULL DEFAULT '',
              text      TEXT NOT NULL DEFAULT '',
              at_ms     INTEGER NOT NULL,
              outgoing  INTEGER NOT NULL DEFAULT 0,
              msg_id    TEXT NOT NULL DEFAULT '',
              token     INTEGER NOT NULL DEFAULT 0,
              state     INTEGER NOT NULL DEFAULT 0,
              read      INTEGER NOT NULL DEFAULT 1)
            """.trimIndent())
        db.execSQL("CREATE INDEX IF NOT EXISTS ix_msg_thread ON messages(group_id, at_ms)")
        db.execSQL("CREATE INDEX IF NOT EXISTS ix_msg_msgid ON messages(msg_id)")
        // 같은 메시지를 두 번 넣지 않는다 — 재수신·재적재에서 스레드가 부풀지 않게.
        db.execSQL("CREATE UNIQUE INDEX IF NOT EXISTS ux_msg_id ON messages(id)")
    }

    override fun onUpgrade(db: SQLiteDatabase, old: Int, new: Int) {
        db.execSQL("DROP TABLE IF EXISTS messages")
        onCreate(db)
    }

    /**
     * 기동 정리 — **잔존 PENDING 은 FAILED 로 마감**한다.
     *
     * 앱이 죽는 순간 보낸 메시지는 최종 응답(`requestResult`)을 받을 길이 없다. 영원히 «보내는 중»
     * 으로 두면 관제사가 갔는지 안 갔는지 알 수 없다 — 실패로 닫아 재전송을 유도한다
     * ([mcdata_messaging.md](mcdata_messaging.md) §5, 데스크톱 `FailPending` 과 같은 규약).
     */
    fun failPending() = runCatching {
        writableDatabase.execSQL(
            "UPDATE messages SET state=? WHERE state=? AND outgoing=1",
            arrayOf<Any>(SendState.FAILED.ordinal, SendState.PENDING.ordinal))
    }

    /** 보관 기간을 넘긴 것을 지운다. */
    fun prune(days: Int = RETENTION_DAYS) = runCatching {
        val cutoff = System.currentTimeMillis() - days.coerceAtLeast(1) * 86_400_000L
        writableDatabase.delete("messages", "at_ms < ?", arrayOf(cutoff.toString()))
    }

    fun insert(m: Message) = runCatching {
        writableDatabase.insertWithOnConflict(
            "messages", null, values(m), SQLiteDatabase.CONFLICT_REPLACE)
    }

    /** disposition 통지·최종 응답이 바꾼 발신 상태. */
    fun setStateByMsgId(msgId: String, state: SendState) = runCatching {
        if (msgId.isEmpty()) return@runCatching
        writableDatabase.execSQL(
            "UPDATE messages SET state=? WHERE msg_id=? AND outgoing=1",
            arrayOf<Any>(state.ordinal, msgId))
    }

    fun setStateByToken(token: Long, state: SendState) = runCatching {
        if (token <= 0) return@runCatching
        writableDatabase.execSQL(
            "UPDATE messages SET state=? WHERE token=? AND outgoing=1",
            arrayOf<Any>(state.ordinal, token))
    }

    fun markRead(groupId: String) = runCatching {
        writableDatabase.execSQL("UPDATE messages SET read=1 WHERE group_id=?", arrayOf<Any>(groupId))
    }

    /** 스레드별 최근 [limit] 건(오래된 것부터). 화면이 그리는 순서 그대로. */
    fun load(limit: Int = LOAD_LIMIT): Map<String, List<Message>> = runCatching {
        val out = LinkedHashMap<String, MutableList<Message>>()
        readableDatabase.rawQuery(
            "SELECT id, group_id, from_uri, from_name, text, at_ms, outgoing, msg_id, token, state, read " +
                "FROM messages ORDER BY at_ms DESC LIMIT ?", arrayOf(limit.toString())).use { c ->
            while (c.moveToNext()) {
                val m = Message(
                    id = c.getString(0), groupId = c.getString(1),
                    fromUri = c.getString(2), fromName = c.getString(3), text = c.getString(4),
                    atMs = c.getLong(5), outgoing = c.getInt(6) != 0,
                    msgId = c.getString(7), token = c.getLong(8),
                    state = SendState.entries.getOrElse(c.getInt(9)) { SendState.NONE },
                    read = c.getInt(10) != 0)
                out.getOrPut(m.groupId) { ArrayList() }.add(m)
            }
        }
        out.mapValues { (_, v) -> v.asReversed().toList() }     // 조회는 최신순, 화면은 오래된 것부터
    }.getOrElse { emptyMap() }

    private fun values(m: Message) = ContentValues().apply {
        put("id", m.id); put("group_id", m.groupId)
        put("from_uri", m.fromUri); put("from_name", m.fromName)
        put("text", m.text); put("at_ms", m.atMs)
        put("outgoing", if (m.outgoing) 1 else 0)
        put("msg_id", m.msgId); put("token", m.token)
        put("state", m.state.ordinal); put("read", if (m.read) 1 else 0)
    }

    companion object {
        private const val DB_NAME = "messages.db"
        private const val DB_VERSION = 1
        /** 데스크톱과 같은 기본 보관 기간(§4.4). */
        const val RETENTION_DAYS = 30
        /** 기동 적재 상한 — 스레드 전부가 아니라 최근 것만 든다. */
        const val LOAD_LIMIT = 2000
    }
}
