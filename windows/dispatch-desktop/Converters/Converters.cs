// XAML 값 변환기 — 표시 규약(§3.2): 경과 mm:ss, URI → user part(툴팁에 원 값), 개수/문자열/null → 가시성.
using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace DispatchDesktop.Converters;

public sealed class ElapsedConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value switch
    {
        TimeSpan ts => ts.TotalHours >= 1 ? ts.ToString(@"h\:mm\:ss") : ts.ToString(@"mm\:ss"),
        DateTime dt => dt.ToString("HH:mm"),
        _ => "",
    };
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>sip:1003@domain / tel:+8250… → 1003 / +8250…. parameter="tail4" 면 뒷 4자리 "…0002".</summary>
public sealed class UserPartConverter : IValueConverter
{
    public static string UserPart(string? uri)
    {
        if (string.IsNullOrEmpty(uri)) return "";
        string s = uri;
        int lt = s.IndexOf('<');
        if (lt >= 0) { int gt = s.IndexOf('>', lt); s = gt > lt ? s.Substring(lt + 1, gt - lt - 1) : s[(lt + 1)..]; }
        if (s.StartsWith("sip:", StringComparison.OrdinalIgnoreCase) || s.StartsWith("sips:", StringComparison.OrdinalIgnoreCase))
            s = s[(s.IndexOf(':') + 1)..];
        else if (s.StartsWith("tel:", StringComparison.OrdinalIgnoreCase)) s = s[4..];
        int at = s.IndexOf('@');
        if (at >= 0) s = s[..at];
        int semi = s.IndexOf(';');
        if (semi >= 0) s = s[..semi];
        return s;
    }

    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        string u = UserPart(value as string);
        if (p is string mode && mode == "tail4" && u.Length > 4) return "…" + u[^4..];
        return u;
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

public sealed class CountToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        value is int n && n > 0 ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

public sealed class ZeroToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        value is int n && n == 0 ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>문자열이 있으면 Visible. parameter "invert" = 비었을 때 Visible(입력란 자리표시자).</summary>
public sealed class StringToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        string.IsNullOrEmpty(value as string) == (p is string s && s == "invert") ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

public sealed class InverseBoolConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is bool b ? !b : true;
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c) => value is bool b ? !b : true;
}

public sealed class BoolToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is true ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

public sealed class InverseBoolToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is true ? Visibility.Collapsed : Visibility.Visible;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>value == parameter (문자열 비교) → bool. 세그먼트 RadioButton IsChecked 바인딩용(ConvertBack 은 parameter 를 돌려준다).</summary>
public sealed class EqualsConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => string.Equals(value?.ToString(), p?.ToString(), StringComparison.Ordinal);
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c) =>
        value is true ? (t.IsEnum && p is string s ? Enum.Parse(t, s) : t == typeof(int) && p is string i ? int.Parse(i) : p!) : Binding.DoNothing;
}

public sealed class EqualsToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        string.Equals(value?.ToString(), p?.ToString(), StringComparison.Ordinal) ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>0~1 레벨 → 폭(px). parameter = 최대 폭(기본 120).</summary>
public sealed class LevelToWidthConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        double max = p is string s && double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out double m) ? m : 120;
        double v = value switch { float f => f, double d => d, _ => 0 };
        return Math.Clamp(v, 0, 1) * max;
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

public sealed class NullToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is null ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

public sealed class NotNullToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is null ? Visibility.Collapsed : Visibility.Visible;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>비율(0~1) × 실제 폭 → 길이(px). 타임바의 발언 턴 막대 위치/폭 — values[0]=비율, values[1]=트랙 ActualWidth.</summary>
public sealed class RatioToLengthConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type t, object? p, CultureInfo c)
    {
        double ratio = values.Length > 0 && values[0] is double r ? r : 0;
        double width = values.Length > 1 && values[1] is double w ? w : 0;
        return Math.Max(0, Math.Clamp(ratio, 0, 1) * width);
    }
    public object[] ConvertBack(object v, Type[] t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>배율 × 실제 폭 → 길이(px). 발언 타임라인 트랙 폭(뷰포트 × 확대 배율) — values[0]=배율(1 이상), values[1]=뷰포트 폭.</summary>
public sealed class ScaleToLengthConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type t, object? p, CultureInfo c)
    {
        double scale = values.Length > 0 && values[0] is double s && s > 0 ? s : 1;
        double width = values.Length > 1 && values[1] is double w ? w : 0;
        return Math.Max(0, scale * width);
    }
    public object[] ConvertBack(object v, Type[] t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>출력 라우트(bool RouteIsSpeaker) → 아이콘 지오메트리(Icon.Speaker / Icon.Headphones). 🎧/🔊 글리프 대신 Path 로 그린다.</summary>
public sealed class RouteIconConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
        => Application.Current?.TryFindResource(value is true ? "Icon.Speaker" : "Icon.Headphones") ?? System.Windows.Media.Geometry.Empty;
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>높이 → 알약 모서리(높이/2). WPF CornerRadius 를 큰 값(999)으로 두면 넓은 Border 가 타원이 되므로 실제 높이의 절반을 쓴다.</summary>
public sealed class HalfRadiusConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => new CornerRadius(value is double h && h > 0 ? h / 2 : 0);
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>트리 깊이 → 드롭다운 항목 안쪽 여백(깊이당 14) — 조직 콤보가 펼친 목록에서만 들여 쓰고 닫힌 상자에는 이름만 보이게(ComboBoxItem 기본 여백 9,5 기준).</summary>
public sealed class DepthIndentConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => new Thickness(9 + (value is int d && d > 0 ? d : 0) * 14, 5, 9, 5);
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>첫 글자(아바타 원) — 빈 값이면 "?".</summary>
public sealed class InitialConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) => value is string s && s.Trim().Length > 0 ? s.Trim()[..1] : "?";
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>폭 → 격자 열 수 — parameter "최소 칸 폭|최대 열"(예 "230|4"). 칸이 최소 폭보다 좁아지면 열을 줄인다(작은 창·패널이 연 좁은 칸, §3.3).</summary>
public sealed class ColumnsByWidthConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        var parts = (p as string ?? "240|3").Split('|');
        double min = double.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var m) ? m : 240;
        int max = parts.Length > 1 && int.TryParse(parts[1], out var x) ? x : 3;
        double w = value is double d && d > 0 ? d : min * max;
        return Math.Clamp((int)(w / min), 1, max);
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>이름(또는 번호) → 아바타 색 번호 "0"~"6" — 같은 사람은 어느 목록에서나 같은 색(FNV-1a). 면·글자는 테마 토큰(Avatar 스타일 트리거)이라
/// 테마를 바꾸면 따라간다. 빈 값 = "7"(무채).</summary>
public sealed class AvatarHueConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        string s = (value as string ?? "").Trim();
        if (s.Length == 0) return "7";
        uint h = 2166136261;
        foreach (char ch in s) { h ^= ch; h *= 16777619; }
        return (h % 7).ToString(CultureInfo.InvariantCulture);
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>bool → parameter "참일 때|거짓일 때" 중 하나를 대상 형식으로(열 수·폭·GridLength·가시성 등). 오른쪽 패널이 열리면 오른쪽 칸을 좁히는 데 쓴다(§3.6).</summary>
public sealed class PickConverter : IValueConverter
{
    public object? Convert(object? value, Type t, object? p, CultureInfo c)
    {
        var parts = (p as string ?? "").Split('|');
        string raw = value is true ? parts[0] : parts.Length > 1 ? parts[1] : "";
        if (t == typeof(object) || t == typeof(string)) return raw;
        return System.ComponentModel.TypeDescriptor.GetConverter(t).ConvertFromInvariantString(raw);
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>두 값이 같은 참조인가 — 목록에서 고른 줄 강조(대화 목록·기록 목록).</summary>
public sealed class SameRefConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type t, object? p, CultureInfo c) => values.Length == 2 && values[0] is not null && ReferenceEquals(values[0], values[1]);
    public object[] ConvertBack(object v, Type[] t, object? p, CultureInfo c) => throw new NotSupportedException();
}
