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

public sealed class StringToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        string.IsNullOrEmpty(value as string) ? Visibility.Collapsed : Visibility.Visible;
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
        value is true ? (t.IsEnum && p is string s ? Enum.Parse(t, s) : p!) : Binding.DoNothing;
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

/// <summary>RegState → 점등 색(§3.2): 회색 미등록 · 노랑 등록중 · 녹색 등록 · 빨강 실패. 테마 리소스를 찾는다.</summary>
public sealed class RegStateToBrushConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        string key = value is CimsUe.RegState s ? s switch
        {
            CimsUe.RegState.Registered => "Brush.Talk", CimsUe.RegState.Registering => "Brush.Ring", CimsUe.RegState.Failed => "Brush.Emg", _ => "Brush.Wire",
        } : "Brush.Wire";
        return Application.Current?.TryFindResource(key) ?? System.Windows.Media.Brushes.Gray;
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
