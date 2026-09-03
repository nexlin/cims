// 앱 데이터 위치 — %APPDATA%\CIMS\dispatch-desktop (설정·배치·메시지 DB·주소록·로그), 비밀은 파사드 CredentialStore(%LOCALAPPDATA%).
using System.IO;

namespace DispatchDesktop.Services;

public static class AppPaths
{
    public const string AppName = "dispatch-desktop";
    public const string InstanceName = "CIMS.DispatchDesktop";

    public static string Root { get; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "CIMS", AppName);
    public static string Settings => Path.Combine(Root, "settings.json");
    public static string Layout => Path.Combine(Root, "layout.json");
    public static string MessagesDb => Path.Combine(Root, "messages.db");
    public static string DirectoryCsv => Path.Combine(Root, "directory.csv");
    public static string Logs => Path.Combine(Root, "logs");

    public static void Ensure()
    {
        Directory.CreateDirectory(Root);
        Directory.CreateDirectory(Logs);
    }
}
