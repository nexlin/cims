// 내역 링 버퍼 — 패널(②④)당 200 행·하루, CSV 내보내기 (§4.2·§4.4).
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
        list.Insert(0, row);                                // 최신 위
        while (list.Count > Capacity) list.RemoveAt(list.Count - 1);
        Added?.Invoke(this, row);
    }

    public void Add(ActivityPanel panel, ActivityKind kind, string title, string detail = "", bool emergency = false,
                    bool missed = false, string number = "", bool pilot = false) =>
        Add(new ActivityRow(DateTime.Now, panel, kind, title, detail, emergency, missed, number, pilot));

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
