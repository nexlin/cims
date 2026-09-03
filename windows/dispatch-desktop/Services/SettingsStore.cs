// 설정(settings.json) — 로그인·오디오 장치·핫키·기본 라우트 정책·표시 모드. 비밀(토큰)은 여기 없다(CredentialStore).
using System.Text.Json;
using System.Text.Json.Serialization;

using System.IO;

namespace DispatchDesktop.Services;

public sealed class AppSettings
{
    // 로그인 (§6)
    public string CscHost { get; set; } = "";
    public int CscPort { get; set; } = 4430;
    public string LoginId { get; set; } = "";
    public bool AutoLogin { get; set; } = true;
    public bool CscVerifyServer { get; set; } = true;
    /// <summary>사설 CA PEM 파일 경로(SIP TLS·HTTPS 공용 신뢰 앵커). 비면 시스템 기본.</summary>
    public string TlsCaPemPath { get; set; } = "";

    // 오디오 (§7) — 엔진 장치 목록의 이름으로 기억한다(id 는 재부팅마다 바뀔 수 있다)
    public string CaptureDevice { get; set; } = "";
    public string HeadsetDevice { get; set; } = "";
    public string SpeakerDevice { get; set; } = "";
    public bool SpeakerRouteEnabled { get; set; } = true;
    public bool AutoReturnToPreferredDevice { get; set; } = true;

    // 핫키 (§8)
    public Dictionary<string, string> HotKeys { get; set; } = new()
    {
        ["ptt"] = "Ctrl+Space", ["answer"] = "F9", ["hangup"] = "F10", ["pickup"] = "F8", ["hold"] = "F11", ["mute"] = "F12",
    };

    // 관제 (§4.3) — 당겨받기 피처코드는 접속서비스 pickup_feature_code 값(프로파일 공급 전까지 설정)
    public string PickupFeatureCode { get; set; } = "**";
    public bool AutoHoldOnAnswer { get; set; } = true;
    public bool ConfirmCloseMonitor { get; set; } = true;
    public int MaxMonitorWindows { get; set; } = 4;
    public bool FollowChannelThread { get; set; } = true;
    public bool MinimizeToTray { get; set; } = true;
    public int MessageRetentionDays { get; set; } = 30;

    // 표시
    /// <summary>light | dark</summary>
    public string Theme { get; set; } = "light";
    /// <summary>pad | book | recent — 일반통화 발신 세그먼트, 마지막 선택 기억(§4.3).</summary>
    public string OriginateMode { get; set; } = "book";
    /// <summary>card | tile</summary>
    public string ChannelViewMode { get; set; } = "card";
    /// <summary>카드로 보일 채널(그룹 id). 비면 멤버 그룹 전부.</summary>
    public List<string> SelectedChannels { get; set; } = new();
    /// <summary>주소록 CSV 경로 재지정(비면 %APPDATA% 의 directory.csv, 없으면 앱 옆 directory.sample.csv).</summary>
    public string DirectoryCsv { get; set; } = "";
    public int LogLevel { get; set; } = 3;
}

public sealed class SettingsStore
{
    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.Never, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public AppSettings Current { get; private set; } = new();
    public event EventHandler? Changed;

    public void Load()
    {
        try
        {
            if (File.Exists(AppPaths.Settings))
                Current = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(AppPaths.Settings), Json) ?? new AppSettings();
        }
        catch (Exception) { Current = new AppSettings(); }
    }

    public void Save()
    {
        AppPaths.Ensure();
        File.WriteAllText(AppPaths.Settings, JsonSerializer.Serialize(Current, Json));
        Changed?.Invoke(this, EventArgs.Empty);
    }

    public void Update(Action<AppSettings> mutate)
    {
        mutate(Current);
        Save();
    }
}
