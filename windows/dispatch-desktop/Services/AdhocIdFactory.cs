// 애드혹 임시 그룹 id — adhoc-<내 PTT 번호>-<epoch초> (mcptt_emergency_modes.md §6). adhoc-/priv- 는 편성 그룹 예약어.
namespace DispatchDesktop.Services;

public static class AdhocIdFactory
{
    public static string Create(string myPttNumber)
    {
        string n = myPttNumber;
        if (n.StartsWith("tel:", StringComparison.OrdinalIgnoreCase)) n = n[4..];
        int at = n.IndexOf('@'); if (at >= 0) n = n[..at];
        if (n.StartsWith("sip:", StringComparison.OrdinalIgnoreCase)) n = n[4..];
        return $"adhoc-{n}-{DateTimeOffset.UtcNow.ToUnixTimeSeconds()}";
    }

    public static bool IsAdhoc(string groupId) => groupId.StartsWith("adhoc-", StringComparison.Ordinal);
}
