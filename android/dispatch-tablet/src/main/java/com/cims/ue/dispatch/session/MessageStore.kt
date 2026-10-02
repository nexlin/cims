// 메시지 보관 — SDS·파일(FD)·문자 (docs/design/features/android_dispatch_tablet.md §6.2e·§6.9, dispatch_desktop_ui.md §4.4)
//
// 데스크톱의 `MessageStore`(SQLite `messages.db`, 최근 30일) 대응. 스키마 의미도 같다.
// **Room 을 쓰지 않는다** — 표가 하나뿐이고 질의가 몇 안 돼 애노테이션 처리기와 의존을 늘릴 값이 없다.
// `SettingsStore` 가 DataStore 대신 SharedPreferences 를 쓴 것과 같은 판단이다.
//
// 보관하지 않으면 앱을 껐다 켤 때마다 SDS 스레드가 통째로 사라진다 — 관제사는 «아까 뭐라고 했더라» 를
// 앱 밖에서 찾을 수 없다. 서버에 SDS 이력 API 가 없으므로 이 로컬 보관이 유일한 근거다.
//
// **격리 단위 = 로그인 ID**(`owner` 열). 관제석은 자리별 로그인 ID 를 쓴다(교대해도 같다) — 한 기기에 다른 자리 ID 로
// 로그인하면 앞 ID 의 대화가 보이지 않는다. 조회·읽음 표시·상태 갱신·새 행은 지금 주인의 것만, 보존 정리([prune])와
// 재기동 PENDING 마감([failPending])은 주인과 무관하다(데스크톱 `MessageStore` 와 같은 규칙 — [OwnerRule]).
package com.cims.ue.dispatch.session

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

/**
 * 보관 주인의 규칙 — 순수 논리라 JVM 에서 시험한다. [MessageStore] 의 질의가 이 셋을 SQL 로 옮긴 것이다.
 */
internal object OwnerRule {
    /** 주인으로 삼을 로그인 ID — 프로파일이 준 값, 비면 로그인에 쓴(저장된) 값(데스크톱 `FetchProfileAsync`). */
    fun pick(profileLoginId: String, savedLoginId: String): String = profileLoginId.ifEmpty { savedLoginId }

    /** 이 행이 지금 주인에게 보이는가 — 주인이 없으면(로그인 전) 아무것도 보이지 않는다. */
    fun visible(rowOwner: String, owner: String): Boolean = owner.isNotEmpty() && rowOwner == owner

    /** 로그인 뒤 이 행의 주인 — 주인 없는 행(`owner` 열 이전 판)은 그 기기의 **첫 로그인 ID** 가 한 번 이어받는다. */
    fun adopt(rowOwner: String, owner: String): String = if (rowOwner.isEmpty() && owner.isNotEmpty()) owner else rowOwner
}

class MessageStore(context: Context) :
    SQLiteOpenHelper(context.applicationContext, DB_NAME, null, DB_VERSION) {

    /** 지금 보관 주인(로그인 ID). 비면(로그인 전) 조회는 빈 목록이다. */
    @Volatile
    var owner: String = ""
        private set

    /**
     * 로그인 — 주인을 정하고, 주인 없는 옛 행을 이 로그인 ID 에 한 번 귀속한다([OwnerRule.adopt] — 자리 기기는 같은 자리
     * ID 로 로그인한다). 같은 id 의 행을 이미 가진 주인에게는 넘기지 않는다(`OR IGNORE` — 유일 색인이 주인 안에서 선다).
     */
    fun setOwner(loginId: String) = runCatching {
        owner = loginId
        if (loginId.isNotEmpty())
            writableDatabase.execSQL("UPDATE OR IGNORE messages SET owner=? WHERE owner=''", arrayOf<Any>(loginId))
    }

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """
            CREATE TABLE IF NOT EXISTS messages (
              row_id     INTEGER PRIMARY KEY AUTOINCREMENT,
              id         TEXT NOT NULL,
              group_id   TEXT NOT NULL,
              from_uri   TEXT NOT NULL DEFAULT '',
              from_name  TEXT NOT NULL DEFAULT '',
              text       TEXT NOT NULL DEFAULT '',
              at_ms      INTEGER NOT NULL,
              outgoing   INTEGER NOT NULL DEFAULT 0,
              msg_id     TEXT NOT NULL DEFAULT '',
              token      INTEGER NOT NULL DEFAULT 0,
              state      INTEGER NOT NULL DEFAULT 0,
              read       INTEGER NOT NULL DEFAULT 1,
              kind       TEXT NOT NULL DEFAULT 'SDS',
              file_name  TEXT NOT NULL DEFAULT '',
              file_url   TEXT NOT NULL DEFAULT '',
              file_type  TEXT NOT NULL DEFAULT '',
              file_size  INTEGER NOT NULL DEFAULT 0,
              local_path TEXT NOT NULL DEFAULT '',
              owner      TEXT NOT NULL DEFAULT '')
            """.trimIndent())
        createIndexes(db)
    }

    private fun createIndexes(db: SQLiteDatabase) {
        db.execSQL("CREATE INDEX IF NOT EXISTS ix_msg_thread ON messages(group_id, at_ms)")
        db.execSQL("CREATE INDEX IF NOT EXISTS ix_msg_msgid ON messages(msg_id)")
        db.execSQL("CREATE INDEX IF NOT EXISTS ix_msg_owner ON messages(owner, kind, at_ms)")
        // 같은 메시지를 두 번 넣지 않는다 — 재수신·재적재에서 스레드가 부풀지 않게. 주인 안에서만 유일하다
        //   (같은 그룹 메시지를 두 자리 ID 가 한 기기에서 차례로 받을 수 있다).
        db.execSQL("CREATE UNIQUE INDEX IF NOT EXISTS ux_msg_owner_id ON messages(owner, id)")
    }

    /**
     * **보관을 버리지 않는다.** 판이 바뀔 때마다 표를 지우면 SDS 스레드가 통째로 사라지고, 관제사는 «아까 뭐라고 했더라» 를
     * 찾을 데가 없다(서버에 SDS 이력 API 가 없다). 판 2 = 종류 열(기존 행은 SDS), 판 3 = 파일 열 다섯 + 주인 열(기존 행은
     * 주인 없음 — 첫 [setOwner] 가 이어받는다)과 주인 안 유일 색인.
     */
    override fun onUpgrade(db: SQLiteDatabase, old: Int, new: Int) {
        runCatching {
            if (old < 2) db.execSQL("ALTER TABLE messages ADD COLUMN kind TEXT NOT NULL DEFAULT 'SDS'")
            if (old < 3) {
                V3_COLUMNS.forEach { db.execSQL("ALTER TABLE messages ADD COLUMN $it") }
                db.execSQL("DROP INDEX IF EXISTS ux_msg_id")
                createIndexes(db)
            }
        }.onFailure {
            db.execSQL("DROP TABLE IF EXISTS messages")
            onCreate(db)
        }
    }

    /**
     * 기동 정리 — **잔존 PENDING 은 FAILED 로 마감**한다(주인 무관).
     *
     * 앱이 죽는 순간 보낸 메시지는 최종 응답(`requestResult`)을 받을 길이 없다. 영원히 «보내는 중»
     * 으로 두면 관제사가 갔는지 안 갔는지 알 수 없다 — 실패로 닫아 재전송을 유도한다
     * ([mcdata_messaging.md](mcdata_messaging.md) §5, 데스크톱 `FailPending` 과 같은 규약). 올리던 파일도 여기서 닫힌다 —
     * FILEURL 이 없는 채 실패한 말풍선은 재전송이 업로드부터 다시 한다.
     */
    fun failPending(beforeMs: Long = Long.MAX_VALUE) = runCatching {
        // [beforeMs] 앞의 것만 — 이번 기동이 시작한 뒤에 보낸 메시지는 응답을 기다리는 중이다(마감하면 전달된 메시지가 «실패» 로
        //   남고, 최종 응답은 PENDING 행만 고치므로 되돌릴 길이 없다).
        writableDatabase.execSQL(
            "UPDATE messages SET state=? WHERE state=? AND outgoing=1 AND at_ms < ?",
            arrayOf<Any>(SendState.FAILED.ordinal, SendState.PENDING.ordinal, beforeMs))
    }

    /** 보관 기간을 넘긴 것을 지운다(주인 무관 — 기기의 보관 정책이다). */
    fun prune(days: Int = RETENTION_DAYS) = runCatching {
        val cutoff = System.currentTimeMillis() - days.coerceAtLeast(1) * 86_400_000L
        writableDatabase.delete("messages", "at_ms < ?", arrayOf(cutoff.toString()))
    }

    fun insert(m: Message) = runCatching {
        writableDatabase.insertWithOnConflict(
            "messages", null, values(m), SQLiteDatabase.CONFLICT_REPLACE)
    }

    /** disposition 통지가 바꾼 발신 상태. */
    fun setStateByMsgId(msgId: String, state: SendState) = runCatching {
        if (msgId.isEmpty()) return@runCatching
        writableDatabase.execSQL(
            "UPDATE messages SET state=? WHERE msg_id=? AND outgoing=1 AND owner=?",
            arrayOf<Any>(state.ordinal, msgId, owner))
    }

    /** 재전송 — 같은 말풍선(행 id)이 새 token·상태를 받는다(데스크톱 `UpdateResend`). msgId 는 코어가 돌려준 값 — SDS 는 처음 것 그대로다. */
    fun updateResend(id: String, msgId: String, token: Long, state: SendState) = runCatching {
        writableDatabase.execSQL(
            "UPDATE messages SET msg_id=?, token=?, state=? WHERE id=? AND outgoing=1 AND owner=?",
            arrayOf<Any>(msgId, token, state.ordinal, id, owner))
    }

    /**
     * 최종 응답이 바꾼 발신 상태 — **기다리던(PENDING) 행만**. token 은 엔진이 뜰 때마다 다시 세므로, 상태를 가리지 않으면
     * 지난 기동의 같은 token 행(이미 «전달됨» 인 말풍선)까지 덮는다. 화면 쪽 판정(`applyRequestResult`)과 같은 조건이다.
     */
    fun setStateByToken(token: Long, state: SendState) = runCatching {
        if (token <= 0) return@runCatching
        writableDatabase.execSQL(
            "UPDATE messages SET state=? WHERE token=? AND outgoing=1 AND state=? AND owner=?",
            arrayOf<Any>(state.ordinal, token, SendState.PENDING.ordinal, owner))
    }

    /**
     * 파일 말풍선의 진행 — 업로드가 끝나 FILEURL 이 생겼다 · FD 알림을 보냈다(msgId·token) · 실패했다 · 받은 파일의 기기 경로가
     * 생겼다(데스크톱 `UpdateFile`·`UpdateState`·`UpdateLocalPath`). 말풍선의 지금 값을 그 행에 그대로 적는다.
     */
    fun updateFile(m: Message) = runCatching {
        writableDatabase.execSQL(
            "UPDATE messages SET file_url=?, local_path=?, msg_id=?, token=?, state=? WHERE id=? AND owner=?",
            arrayOf<Any>(m.fileUrl, m.localPath, m.msgId, m.token, m.state.ordinal, m.id, owner))
    }

    /** 대화 키를 바꾼다 — 문자 대화 키를 정규형으로 모을 때(`smsKey`). 지금 주인의 그 종류만. */
    fun rekey(from: String, to: String, kind: MessageKind) = runCatching {
        writableDatabase.execSQL("UPDATE OR IGNORE messages SET group_id=? WHERE group_id=? AND kind=? AND owner=?",
            arrayOf<Any>(to, from, kind.name, owner))
    }

    fun markRead(groupId: String, kind: MessageKind = MessageKind.SDS) = runCatching {
        writableDatabase.execSQL("UPDATE messages SET read=1 WHERE group_id=? AND kind=? AND owner=?",
            arrayOf<Any>(groupId, kind.name, owner))
    }

    /** 종류별 스레드 — 지금 주인의 최근 [limit] 건(오래된 것부터). 화면이 그리는 순서 그대로. 로그인 전이면 빈 지도. */
    fun load(kind: MessageKind = MessageKind.SDS, limit: Int = LOAD_LIMIT): Map<String, List<Message>> = runCatching {
        val me = owner
        if (me.isEmpty()) return@runCatching emptyMap()
        val out = LinkedHashMap<String, MutableList<Message>>()
        readableDatabase.rawQuery(
            "SELECT id, group_id, from_uri, from_name, text, at_ms, outgoing, msg_id, token, state, read, " +
                "file_name, file_url, file_type, file_size, local_path " +
                "FROM messages WHERE owner=? AND kind=? ORDER BY at_ms DESC LIMIT ?",
            arrayOf(me, kind.name, limit.toString())).use { c ->
            while (c.moveToNext()) {
                val m = Message(
                    id = c.getString(0), groupId = c.getString(1),
                    fromUri = c.getString(2), fromName = c.getString(3), text = c.getString(4),
                    atMs = c.getLong(5), outgoing = c.getInt(6) != 0,
                    msgId = c.getString(7), token = c.getLong(8),
                    state = SendState.entries.getOrElse(c.getInt(9)) { SendState.NONE },
                    read = c.getInt(10) != 0, kind = kind,
                    fileName = c.getString(11), fileUrl = c.getString(12), fileType = c.getString(13),
                    fileSize = c.getLong(14), localPath = c.getString(15))
                out.getOrPut(m.groupId) { ArrayList() }.add(m)
            }
        }
        out.mapValues { (_, v) -> v.asReversed().toList() }     // 조회는 최신순, 화면은 오래된 것부터
    }.getOrElse { emptyMap() }

    /**
     * 말풍선이 가리키는 기기 파일 전부(주인 무관) — 보관 정리 뒤 **어느 말풍선도 가리키지 않는 파일**을 지우는 근거다
     * (`sweepFiles`). 다른 자리 ID 의 파일을 지우지 않게 주인을 가리지 않는다. 읽지 못하면 null — 그때는 아무것도 지우지 않는다.
     */
    fun localPaths(): Set<String>? = runCatching {
        val out = HashSet<String>()
        readableDatabase.rawQuery("SELECT local_path FROM messages WHERE local_path<>''", null).use { c ->
            while (c.moveToNext()) out.add(c.getString(0))
        }
        out
    }.getOrNull()

    private fun values(m: Message) = ContentValues().apply {
        put("id", m.id); put("group_id", m.groupId)
        put("from_uri", m.fromUri); put("from_name", m.fromName)
        put("text", m.text); put("at_ms", m.atMs)
        put("outgoing", if (m.outgoing) 1 else 0)
        put("msg_id", m.msgId); put("token", m.token)
        put("state", m.state.ordinal); put("read", if (m.read) 1 else 0)
        put("kind", m.kind.name)
        put("file_name", m.fileName); put("file_url", m.fileUrl); put("file_type", m.fileType)
        put("file_size", m.fileSize); put("local_path", m.localPath)
        put("owner", owner)
    }

    companion object {
        private const val DB_NAME = "messages.db"
        private const val DB_VERSION = 3
        /** 판 3 이 더한 열 — 파일(FD) 다섯 + 보관 주인. */
        private val V3_COLUMNS = listOf(
            "file_name TEXT NOT NULL DEFAULT ''", "file_url TEXT NOT NULL DEFAULT ''", "file_type TEXT NOT NULL DEFAULT ''",
            "file_size INTEGER NOT NULL DEFAULT 0", "local_path TEXT NOT NULL DEFAULT ''", "owner TEXT NOT NULL DEFAULT ''")
        /** 데스크톱과 같은 기본 보관 기간(§4.4). */
        const val RETENTION_DAYS = 30
        /** 기동 적재 상한 — 스레드 전부가 아니라 최근 것만 든다. */
        const val LOAD_LIMIT = 2000
    }
}
