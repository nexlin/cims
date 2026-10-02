// 내역 링 버퍼 — 종류(PTT → «이벤트», 통화 → «기록»)당 200 행·하루, CSV 내보내기 (§4.4).
using System.Collections.ObjectModel;
using System.Globalization;
using System.Text;
using DispatchDesktop.Models;

using System.IO;

namespace DispatchDesktop.Services;

public sealed class ActivityLog
{
    public const int Capacity = 200;

    public ObservableCollection<ActivityRow> Ptt { get; } = new();
    public ObservableCollection<ActivityRow> Call { get; } = new();
    public event EventHandler<ActivityRow>? Added;

    public void Add(ActivityRow row)
    {
        var list = row.Panel == ActivityPanel.Ptt ? Ptt : Call;
        // 최신 위 — 제 시각의 자리에 끼운다. 서버 이력은 수초 늦게(첫 폴링은 지난 한 시간치가) 제 시각을 싣고 오므로, 무조건 맨 위에 올리면
        //   지난 일이 방금 일 위에 선다. 대개 맨 앞이라 싸다.
        int at = 0;
        while (at < list.Count && list[at].Time > row.Time) at++;
        list.Insert(at, row);
        while (list.Count > Capacity) list.RemoveAt(list.Count - 1);
        Added?.Invoke(this, row);
    }

    public void Add(ActivityPanel panel, ActivityKind kind, string title, string detail = "", bool emergency = false,
                    bool missed = false, string number = "", bool pilot = false) =>
        Add(new ActivityRow(DateTime.Now, panel, kind, title, detail, emergency, missed, number, pilot));

    /// <summary>전부 비운다 — 로그아웃(다음 로그인은 다른 사람일 수 있다).</summary>
    public void Clear() { Ptt.Clear(); Call.Clear(); }

    /// <summary>하루 지난 행 정리(자정 넘김).</summary>
    public void Prune(DateTime now)
    {
        foreach (var list in new[] { Ptt, Call })
            for (int i = list.Count - 1; i >= 0; --i)
                if ((now - list[i].Time).TotalHours > 24) list.RemoveAt(i);
    }

    public void ExportCsv(ActivityPanel panel, string path)
    {
        var list = panel == ActivityPanel.Ptt ? Ptt : Call;
        var sb = new StringBuilder();
        sb.AppendLine("time,kind,title,detail,emergency,missed,number");
        foreach (var r in list.Reverse())
            sb.Append(r.Time.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)).Append(',')
              .Append(Q(r.KindText)).Append(',').Append(Q(r.Title)).Append(',').Append(Q(r.Detail)).Append(',')
              .Append(r.IsEmergency ? 1 : 0).Append(',').Append(r.IsMissed ? 1 : 0).Append(',').Append(Q(r.Number)).AppendLine();
        File.WriteAllText(path, sb.ToString(), new UTF8Encoding(true));
    }

    private static string Q(string s) => "\"" + s.Replace("\"", "\"\"") + "\"";
}
