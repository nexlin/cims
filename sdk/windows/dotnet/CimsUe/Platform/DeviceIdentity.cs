// CimsUe.Platform — 단말 속성: REGISTER User-Agent 의 OS·모델과 Contact +sip.instance 의 기기 고유 URN (mcptt_management_views.md §4.1, ue_sdk.md §4.2).
// 형식 규칙은 코어(userAgentOf)에 하나만 두고 여기서는 Windows 에서만 얻는 값(OS 판·모델·기기 GUID)만 모은다. Android 짝 = android/core DeviceIdentity.
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace CimsUe.Platform;

public static class DeviceIdentity
{
    /// <summary>`제품/앱 버전 (OS; 모델)` — 예: `CIMS-Dispatch/0.1.0 (Windows 11 25H2; 960XGL)`. 앱이 EngineConfig.UserAgent 에 넣는다.</summary>
    public static string UserAgent(string product, string version) => Engine.UserAgentOf(product, version, OsText(), Model());

    /// <summary>OS 판 — `Windows 11 25H2`(빌드 22000 이상은 11, 뒤는 표시 판 DisplayVersion). 표시 판이 없으면 `Windows 10 10.0.19045` 처럼 빌드.</summary>
    public static string OsText()
    {
        var v = Environment.OSVersion.Version;
        string name = v.Major == 10 ? (v.Build >= 22000 ? "Windows 11" : "Windows 10") : $"Windows {v.Major}.{v.Minor}";
        string display = ReadMachine(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "DisplayVersion");
        return display.Length > 0 ? $"{name} {display}" : $"{name} {v.Major}.{v.Minor}.{v.Build}";
    }

    /// <summary>기기 모델 — BIOS SystemProductName(예: `960XGL`, `Surface Laptop 5`). 못 읽으면 빈 값(User-Agent 괄호 안에서 빠진다).</summary>
    public static string Model() => ReadMachine(@"HARDWARE\DESCRIPTION\System\BIOS", "SystemProductName");

    /// <summary>`+sip.instance` 로 쓸 기기 고유 URN(꺾쇠 없이). Windows 는 IMEI 가 없으므로 기기 GUID(MachineGuid)에서 이름 기반 UUID(RFC 4122 v3)를
    /// 만든다 — Android(ANDROID_ID)와 같은 규칙(`cims-ue:` 접두 MD5)이라 같은 기기의 CIMS 앱이 같은 값을 쓰고, 재설치해도 바뀌지 않는다.
    /// 못 얻으면 null(pjsip 기본값 — 호스트명 해시라 기기마다 같을 수 있다, registration_binding_set.md §8).</summary>
    public static string? InstanceUrn()
    {
        string id = ReadMachine(@"SOFTWARE\Microsoft\Cryptography", "MachineGuid");
        return id.Length == 0 ? null : "urn:uuid:" + NameUuid("cims-ue:" + id);
    }

    /// <summary>이름 기반 UUID v3(MD5 — 이름공간 없이 이름 바이트만) 텍스트. Java `UUID.nameUUIDFromBytes` 와 같은 값.</summary>
    internal static string NameUuid(string name)
    {
        byte[] h = MD5.HashData(Encoding.UTF8.GetBytes(name));
        h[6] = (byte)((h[6] & 0x0F) | 0x30);                    // version 3
        h[8] = (byte)((h[8] & 0x3F) | 0x80);                    // variant RFC 4122
        string x = Convert.ToHexStringLower(h);
        return $"{x[..8]}-{x[8..12]}-{x[12..16]}-{x[16..20]}-{x[20..]}";
    }

    private static string ReadMachine(string key, string name)
    {
        try
        {
            using RegistryKey? k = Registry.LocalMachine.OpenSubKey(key, writable: false);
            return (k?.GetValue(name) as string)?.Trim() ?? "";
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { return ""; }
    }
}
