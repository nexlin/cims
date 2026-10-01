// 메시지 보관 — SQLite(%APPDATA%\CIMS\dispatch-desktop\messages.db), MCData/SMS 공용, 최근 N 일 유지 (§4.1·§4.3).
// 격리 단위 = 로그인 ID(owner 열 — 관제석은 자리별 ID 라 교대해도 같다). 조회·읽음 표시는 지금 주인의 행만, 보존 정리·재기동 PENDING 마감은 주인 무관.
using DispatchDesktop.Models;
using Microsoft.Data.Sqlite;

using System.IO;

namespace DispatchDesktop.Services;

public sealed class MessageStore : IDisposable
{
    private readonly SqliteConnection _db;

    public MessageStore(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        _db = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = path, Mode = SqliteOpenMode.ReadWriteCreate }.ToString());
        _db.Open();
        Exec("""
            CREATE TABLE IF NOT EXISTS messages (
              id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER NOT NULL, thread_key TEXT NOT NULL, direction INTEGER NOT NULL,
              peer TEXT NOT NULL DEFAULT '', peer_name TEXT NOT NULL DEFAULT '', group_uri TEXT NOT NULL DEFAULT '',
              conv_id TEXT NOT NULL DEFAULT '', msg_id TEXT NOT NULL DEFAULT '', token INTEGER NOT NULL DEFAULT 0,
              text TEXT NOT NULL DEFAULT '', time INTEGER NOT NULL, state INTEGER NOT NULL DEFAULT 0,
              file_name TEXT NOT NULL DEFAULT '', file_url TEXT NOT NULL DEFAULT '', file_size INTEGER NOT NULL DEFAULT 0,
              read INTEGER NOT NULL DEFAULT 0);
            CREATE INDEX IF NOT EXISTS ix_messages_thread ON messages(kind, thread_key, time);
            CREATE INDEX IF NOT EXISTS ix_messages_msgid ON messages(msg_id);
            """);
        // FD 첨부 열(mcdata_messaging.md §4.5) — 앞선 판의 DB 에는 없어 없으면 붙인다
        AddColumn("file_type", "TEXT NOT NULL DEFAULT ''");
        AddColumn("local_path", "TEXT NOT NULL DEFAULT ''");
        // 보관 주인(로그인 ID) — 앞선 판의 행은 '' 로 붙고 첫 로그인이 한 번 가져간다(SetOwner)
        AddColumn("owner", "TEXT NOT NULL DEFAULT ''");
        Exec("CREATE INDEX IF NOT EXISTS ix_messages_owner ON messages(owner, kind, thread_key)");
        // 그룹 이름(표시용) — 목록에서 사라진 그룹의 대화 제목. 앞선 판의 행은 '' 로 붙고 제목은 그룹 id 로 선다
        AddColumn("group_name", "TEXT NOT NULL DEFAULT ''");
    }

    /// <summary>지금 보관 주인(로그인 ID). 비면(로그인 전) 조회는 빈 목록이다.</summary>
    public string Owner { get; private set; } = "";

    /// <summary>로그인 — 주인을 정하고, 주인 없는 옛 행(owner 열 이전 판)을 이 로그인 ID 에 한 번 귀속한다(자리 PC 는 같은 자리 ID 로 로그인한다).</summary>
    public void SetOwner(string loginId)
    {
        Owner = loginId ?? "";
        if (Owner.Length > 0) Exec("UPDATE messages SET owner=@o WHERE owner=''", ("@o", Owner));
    }

    private void AddColumn(string name, string decl)
    {
        using var cmd = _db.CreateCommand();
        cmd.CommandText = "SELECT COUNT(*) FROM pragma_table_info('messages') WHERE name=@n";
        cmd.Parameters.AddWithValue("@n", name);
        if ((long)cmd.ExecuteScalar()! == 0) Exec($"ALTER TABLE messages ADD COLUMN {name} {decl}");
    }

    /// <summary>재기동 시 잔존 PENDING 은 FAILED 로 마감(재전송 유도 — mcdata_messaging.md §5).</summary>
    public void FailPending() => Exec("UPDATE messages SET state=@f WHERE state=@p", ("@f", (int)SendState.Failed), ("@p", (int)SendState.Pending));

    public void Prune(int retentionDays)
    {
        long cutoff = DateTimeOffset.Now.AddDays(-Math.Max(1, retentionDays)).ToUnixTimeSeconds();
        Exec("DELETE FROM messages WHERE time < @t", ("@t", cutoff));
    }

    public void Insert(Message m)
    {
        using var cmd = _db.CreateCommand();
        cmd.CommandText = """
            INSERT INTO messages(kind, thread_key, direction, peer, peer_name, group_uri, conv_id, msg_id, token, text, time, state, file_name, file_url, file_size, read, file_type, local_path, owner, group_name)
            VALUES(@kind, @thread, @dir, @peer, @peer_name, @group, @conv, @msg, @token, @text, @time, @state, @fname, @furl, @fsize, @read, @ftype, @local, @owner, @group_name);
            SELECT last_insert_rowid();
            """;
        cmd.Parameters.AddWithValue("@kind", (int)m.Kind);
        cmd.Parameters.AddWithValue("@thread", m.ThreadKey);
        cmd.Parameters.AddWithValue("@dir", (int)m.Direction);
        cmd.Parameters.AddWithValue("@peer", m.Peer);
        cmd.Parameters.AddWithValue("@peer_name", m.PeerName);
        cmd.Parameters.AddWithValue("@group", m.GroupUri);
        cmd.Parameters.AddWithValue("@conv", m.ConvId);
        cmd.Parameters.AddWithValue("@msg", m.MsgId);
        cmd.Parameters.AddWithValue("@token", m.Token);
        cmd.Parameters.AddWithValue("@text", m.Text);
        cmd.Parameters.AddWithValue("@time", new DateTimeOffset(m.Time).ToUnixTimeSeconds());
        cmd.Parameters.AddWithValue("@state", (int)m.State);
        cmd.Parameters.AddWithValue("@fname", m.FileName);
        cmd.Parameters.AddWithValue("@furl", m.FileUrl);
        cmd.Parameters.AddWithValue("@fsize", m.FileSize);
        cmd.Parameters.AddWithValue("@read", m.Read ? 1 : 0);
        cmd.Parameters.AddWithValue("@ftype", m.FileType);
        cmd.Parameters.AddWithValue("@local", m.LocalPath);
        cmd.Parameters.AddWithValue("@owner", Owner);
        cmd.Parameters.AddWithValue("@group_name", m.GroupName);
        m.Id = (long)cmd.ExecuteScalar()!;
    }

    /// <summary>FD 발신 — 업로드가 끝나 FILEURL 이 생겼다(+ 알림 발신의 msgId·token·상태).</summary>
    public void UpdateFile(long id, string fileUrl, string msgId, long token, SendState state) =>
        Exec("UPDATE messages SET file_url=@u, msg_id=@m, token=@t, state=@s WHERE id=@id",
             ("@u", fileUrl), ("@m", msgId), ("@t", token), ("@s", (int)state), ("@id", id));
    /// <summary>FD 수신 — 받은 파일의 로컬 경로.</summary>
    public void UpdateLocalPath(long id, string path) => Exec("UPDATE messages SET local_path=@p WHERE id=@id", ("@p", path), ("@id", id));

    public void UpdateState(long id, SendState state) => Exec("UPDATE messages SET state=@s WHERE id=@id", ("@s", (int)state), ("@id", id));
    public void UpdateToken(long id, long token) => Exec("UPDATE messages SET token=@t WHERE id=@id", ("@t", token), ("@id", id));
    /// <summary>재전송으로 바뀐 MCData msgId·token 을 함께 갱신.</summary>
    public void UpdateResend(long id, string msgId, long token, SendState state) =>
        Exec("UPDATE messages SET msg_id=@m, token=@t, state=@s WHERE id=@id", ("@m", msgId), ("@t", token), ("@s", (int)state), ("@id", id));
    public void MarkRead(string threadKey, MessageKind kind) =>
        Exec("UPDATE messages SET read=1 WHERE thread_key=@k AND kind=@kind AND owner=@o", ("@k", threadKey), ("@kind", (int)kind), ("@o", Owner));

    /// <summary>지금 주인의 행 전부(시간순). 로그인 전이면 빈 목록.</summary>
    public List<Message> LoadAll()
    {
        var list = new List<Message>();
        if (Owner.Length == 0) return list;
        using var cmd = _db.CreateCommand();
        cmd.CommandText = "SELECT id, kind, thread_key, direction, peer, peer_name, group_uri, conv_id, msg_id, token, text, time, state, file_name, file_url, file_size, read, file_type, local_path, group_name FROM messages WHERE owner=@o ORDER BY time";
        cmd.Parameters.AddWithValue("@o", Owner);
        using var r = cmd.ExecuteReader();
        while (r.Read())
        {
            list.Add(new Message
            {
                Id = r.GetInt64(0), Kind = (MessageKind)r.GetInt32(1), ThreadKey = r.GetString(2), Direction = (MessageDirection)r.GetInt32(3),
                Peer = r.GetString(4), PeerName = r.GetString(5), GroupUri = r.GetString(6), ConvId = r.GetString(7), MsgId = r.GetString(8),
                Token = r.GetInt64(9), Text = r.GetString(10), Time = DateTimeOffset.FromUnixTimeSeconds(r.GetInt64(11)).LocalDateTime,
                State = (SendState)r.GetInt32(12), FileName = r.GetString(13), FileUrl = r.GetString(14), FileSize = r.GetInt64(15),
                Read = r.GetInt32(16) != 0, FileType = r.GetString(17), LocalPath = r.GetString(18), GroupName = r.GetString(19),
            });
        }
        return list;
    }

    private void Exec(string sql, params (string, object)[] args)
    {
        using var cmd = _db.CreateCommand();
        cmd.CommandText = sql;
        foreach (var (k, v) in args) cmd.Parameters.AddWithValue(k, v);
        cmd.ExecuteNonQuery();
    }

    public void Dispose() => _db.Dispose();
}
