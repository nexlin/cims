// 앱 로그 — 코어(pjsip) 로그 + 앱 이벤트를 %APPDATA%\CIMS\dispatch-desktop\logs\app-yyyyMMdd.log 에 덧붙인다. 파일 하나·일 단위, 7일 보관.
using System.Globalization;
using System.IO;
using System.Text;

namespace DispatchDesktop.Services;

public sealed class AppLog : IDisposable
{
    private readonly object _lock = new();
    private StreamWriter? _w;
    private string _day = "";

    public int MinLevel { get; set; } = 3;      // pjsip 레벨: 1 error · 2 warn · 3 info · 4 debug

    public void Core(int level, string message)
    {
        if (level > MinLevel) return;
        Write(level switch { 1 => "E", 2 => "W", 3 => "I", 4 => "D", _ => "T" }, message.TrimEnd());
    }

    public void Info(string message) => Write("I", message);
    public void Warn(string message) => Write("W", message);
    public void Error(string message, Exception? ex = null) => Write("E", ex is null ? message : message + " — " + ex);

    private void Write(string level, string message)
    {
        lock (_lock)
        {
            try
            {
                var now = DateTime.Now;
                string day = now.ToString("yyyyMMdd", CultureInfo.InvariantCulture);
                if (_w is null || day != _day)
                {
                    _w?.Dispose();
                    AppPaths.Ensure();
                    _w = new StreamWriter(Path.Combine(AppPaths.Logs, $"app-{day}.log"), append: true, new UTF8Encoding(false)) { AutoFlush = true };
                    _day = day;
                    Prune();
                }
                _w.WriteLine($"{now:HH:mm:ss.fff} {level} {message}");
            }
            catch (Exception) { /* 로그 실패는 무시 */ }
        }
    }

    private static void Prune()
    {
        try
        {
            foreach (var f in Directory.GetFiles(AppPaths.Logs, "app-*.log"))
                if ((DateTime.Now - File.GetLastWriteTime(f)).TotalDays > 7) File.Delete(f);
        }
        catch (Exception) { }
    }

    public void Dispose() { lock (_lock) { _w?.Dispose(); _w = null; } }
}
