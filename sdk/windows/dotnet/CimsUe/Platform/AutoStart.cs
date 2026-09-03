// CimsUe.Platform — 로그온 시 자동 시작 (HKCU\Software\Microsoft\Windows\CurrentVersion\Run)
using Microsoft.Win32;

namespace CimsUe.Platform;

public static class AutoStart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public static bool IsEnabled(string appName)
    {
        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKey, writable: false);
        return key?.GetValue(appName) is string s && s.Length > 0;
    }

    /// <summary>등록/해제. exePath 는 실행 파일 전체 경로(인자는 arguments).</summary>
    public static void SetEnabled(string appName, bool enabled, string? exePath = null, string? arguments = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(appName);
        using RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey, writable: true)
                                ?? throw new InvalidOperationException("Run 키를 열 수 없다");
        if (!enabled) { key.DeleteValue(appName, throwOnMissingValue: false); return; }
        exePath ??= Environment.ProcessPath ?? throw new InvalidOperationException("실행 파일 경로를 알 수 없다");
        string cmd = "\"" + exePath + "\"" + (string.IsNullOrEmpty(arguments) ? "" : " " + arguments);
        key.SetValue(appName, cmd, RegistryValueKind.String);
    }
}
