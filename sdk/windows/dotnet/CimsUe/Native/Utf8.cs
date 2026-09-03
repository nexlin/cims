// CimsUe — UTF-8 문자열 경계.
//
// 입력: 코어는 호출이 반환할 때까지만 입력 문자열을 읽는다 → NativeStrings 가 한 호출 동안의 버퍼를 모아 잡고 Dispose 에서 푼다.
// 산출: 코어 소유 문자열은 콜백 동안 / 다음 조회 전까지만 유효 → 받는 즉시 관리 문자열로 복사한다(Utf8.Str).
using System.Runtime.InteropServices;
using System.Text;

namespace CimsUe.Native;

internal static unsafe class Utf8
{
    /// <summary>NULL 은 빈 문자열 — 코어 산출 문자열은 NULL 이 아니지만(빈 std::string 도 "") 방어한다.</summary>
    public static string Str(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((IntPtr)p) ?? "";

    /// <summary>"길이만 계산 → 버퍼 채우기" 두 단계 규약의 문자열 산출 헬퍼 호출.</summary>
    public delegate int OutFn(byte* buf, int cap);
    public static string Call(OutFn fn)
    {
        int need = fn(null, 0);
        if (need <= 0) return "";
        byte[] buf = new byte[need + 1];
        fixed (byte* p = buf)
        {
            int n = fn(p, buf.Length);
            if (n > need) n = need;                 // 방어 — 규약상 같다
            return Encoding.UTF8.GetString(buf, 0, n);
        }
    }
}

/// <summary>한 네이티브 호출 동안만 살아 있는 UTF-8 입력 버퍼 묶음(구조체 필드용). 문자열 null 은 NULL 포인터(코어 기본값 유지).</summary>
internal sealed unsafe class NativeStrings : IDisposable
{
    private readonly List<IntPtr> _allocs = new();

    public byte* Add(string? s)
    {
        if (s is null) return null;
        IntPtr p = Marshal.StringToCoTaskMemUTF8(s);
        _allocs.Add(p);
        return (byte*)p;
    }

    /// <summary>const char* const* 배열. 비어 있으면 NULL/0.</summary>
    public byte** AddArray(IReadOnlyList<string>? items, out int count)
    {
        count = items?.Count ?? 0;
        if (count == 0) return null;
        IntPtr arr = Marshal.AllocCoTaskMem(IntPtr.Size * count);
        _allocs.Add(arr);
        byte** a = (byte**)arr;
        for (int i = 0; i < count; ++i) a[i] = Add(items![i]);
        return a;
    }

    /// <summary>임의 크기의 원시 버퍼(구조체 배열 등).</summary>
    public void* Alloc(int bytes)
    {
        IntPtr p = Marshal.AllocCoTaskMem(bytes);
        new Span<byte>((void*)p, bytes).Clear();
        _allocs.Add(p);
        return (void*)p;
    }

    public void Dispose()
    {
        foreach (IntPtr p in _allocs) Marshal.FreeCoTaskMem(p);
        _allocs.Clear();
    }
}
