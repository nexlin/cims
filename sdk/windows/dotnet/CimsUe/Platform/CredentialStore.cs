// CimsUe.Platform — 자격 저장 (DPAPI ProtectedData, 현재 사용자 범위)
//
// PKCE refresh 토큰 같은 비밀을 %LOCALAPPDATA%\CIMS\<app>\secrets\<key>.bin 에 사용자 키로 암호화해 둔다.
// 비밀번호·H(A1) 는 저장 대상이 아니다(dispatch_desktop_ui.md §6 — sipHa1 은 매 로그인 프로파일에서 받는다).
using System.Security.Cryptography;
using System.Text;

namespace CimsUe.Platform;

public sealed class CredentialStore
{
    private readonly string _dir;
    private readonly byte[] _entropy;

    /// <param name="appName">앱 식별자(디렉터리·엔트로피).</param>
    /// <param name="rootDir">저장 루트(기본 %LOCALAPPDATA%\CIMS) — 시험용.</param>
    public CredentialStore(string appName, string? rootDir = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(appName);
        rootDir ??= Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CIMS");
        _dir = Path.Combine(rootDir, appName, "secrets");
        _entropy = SHA256.HashData(Encoding.UTF8.GetBytes("CimsUe.CredentialStore." + appName));
    }

    public string Directory => _dir;

    public void Save(string key, string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        System.IO.Directory.CreateDirectory(_dir);
        byte[] enc = ProtectedData.Protect(Encoding.UTF8.GetBytes(value), _entropy, DataProtectionScope.CurrentUser);
        File.WriteAllBytes(PathOf(key), enc);
    }

    /// <summary>없거나 복호 실패(다른 사용자·손상)면 null.</summary>
    public string? Load(string key)
    {
        string path = PathOf(key);
        if (!File.Exists(path)) return null;
        try
        {
            byte[] dec = ProtectedData.Unprotect(File.ReadAllBytes(path), _entropy, DataProtectionScope.CurrentUser);
            return Encoding.UTF8.GetString(dec);
        }
        catch (CryptographicException) { return null; }
        catch (IOException) { return null; }
    }

    public void Delete(string key)
    {
        string path = PathOf(key);
        if (File.Exists(path)) File.Delete(path);
    }

    private string PathOf(string key)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        var sb = new StringBuilder(key.Length);
        foreach (char c in key) sb.Append(char.IsLetterOrDigit(c) || c is '-' or '_' or '.' ? c : '_');
        return Path.Combine(_dir, sb + ".bin");
    }
}
