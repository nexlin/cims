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
    /// <summary>받은 파일(MCData FD) — 관제사가 탐색기에서 찾는 자리라 앱 데이터가 아니라 사용자 다운로드 폴더 아래에 둔다.</summary>
    public static string ReceivedFilesDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "CIMS");

    public static void Ensure()
    {
        Directory.CreateDirectory(Root);
        Directory.CreateDirectory(Logs);
    }

    /// <summary>dir 안의 겹치지 않는 파일 경로 — 이름의 경로 문자는 걸러 내고, 있으면 "이름 (n).확장자".</summary>
    public static string UniqueFile(string dir, string name)
    {
        Directory.CreateDirectory(dir);
        foreach (char c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
        name = name.Trim().TrimStart('.');
        if (name.Length == 0) name = "file.bin";
        string stem = Path.GetFileNameWithoutExtension(name), ext = Path.GetExtension(name);
        string path = Path.Combine(dir, name);
        for (int i = 1; File.Exists(path); ++i) path = Path.Combine(dir, $"{stem} ({i}){ext}");
        return path;
    }

    /// <summary>파일 MIME — Windows 등록 정보(HKCR\.ext Content Type), 없으면 application/octet-stream.</summary>
    public static string MimeOf(string path)
    {
        try
        {
            using var k = Microsoft.Win32.Registry.ClassesRoot.OpenSubKey(Path.GetExtension(path));
            if (k?.GetValue("Content Type") is string ct && ct.Length > 0) return ct;
        }
        catch (Exception) { }
        return "application/octet-stream";
    }
}
