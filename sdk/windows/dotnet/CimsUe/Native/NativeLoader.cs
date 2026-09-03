// CimsUe — cimsue.dll 위치 해석.
//
// 탐색 순서: CIMSUE_NATIVE_DIR 환경변수 → 앱 디렉터리/runtimes/win-x64/native(NuGet 배치·개발 출력) → 앱 디렉터리 → 기본 검색.
// 앱 디렉터리를 뒤에 두는 이유: 관리 어셈블리 CimsUe.dll 이 그곳에 있고 Windows 는 cimsue.dll 과 같은 이름으로 본다.
// cimsue.dll 은 vcpkg OpenSSL 런타임(libcrypto-3-x64·libssl-3-x64)에 의존한다 — 같은 디렉터리의 그 둘을 먼저 올려 두면
// 의존 DLL 검색 경로와 무관하게 해석된다.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace CimsUe.Native;

internal static class NativeLoader
{
    private static readonly string[] Deps = { "libcrypto-3-x64.dll", "libssl-3-x64.dll" };
    private static int s_installed;

    [ModuleInitializer]
    internal static void Install()
    {
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeLoader).Assembly, Resolve);
    }

    /// <summary>탐색 후보 디렉터리(존재하는 것만).</summary>
    public static IEnumerable<string> CandidateDirs()
    {
        string? env = Environment.GetEnvironmentVariable("CIMSUE_NATIVE_DIR");
        if (!string.IsNullOrEmpty(env)) yield return env;
        string baseDir = AppContext.BaseDirectory;
        yield return Path.Combine(baseDir, "runtimes", "win-x64", "native");
        yield return baseDir;
    }

    private static IntPtr Resolve(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != NativeMethods.Lib) return IntPtr.Zero;
        foreach (string dir in CandidateDirs())
        {
            string path = Path.Combine(dir, "cimsue.dll");
            if (!File.Exists(path)) continue;
            foreach (string dep in Deps)
            {
                string depPath = Path.Combine(dir, dep);
                if (File.Exists(depPath)) NativeLibrary.TryLoad(depPath, out _);
            }
            if (NativeLibrary.TryLoad(path, out IntPtr h)) return h;
        }
        return IntPtr.Zero;                      // 기본 검색(PATH 등)으로 넘긴다
    }
}
