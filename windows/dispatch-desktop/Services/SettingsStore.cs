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

    // 영상 (§10) — 카메라도 엔진 장치 목록의 이름으로 기억한다
    /// <summary>[영상 보내기] 카메라(엔진 영상 장치 이름 — DirectShow). 비면 첫 카메라.</summary>
    public string VideoCaptureDevice { get; set; } = "";
    /// <summary>«영상 보내는 중 무전»(mcvideo.md §7 D12, TS 22.280 R-8.3-003) — voice = 음성 우선(무전 발언 동안 영상 호 소리만 멈춘다, 영상은 계속 — 기본) ·
    /// video = 영상 우선(영상을 보내는 동안 무전 발언 요청을 막는다, 긴급·임박 채널은 예외).</summary>
    public string VideoMicPolicy { get; set; } = "voice";

    // 핫키 (§8)
    public Dictionary<string, string> HotKeys { get; set; } = new()
    {
        ["ptt"] = "Space", ["answer"] = "F9", ["hangup"] = "F10", ["pickup"] = "F8", ["hold"] = "F11", ["mute"] = "F12",
    };

    // 관제 (§4.3) — 당겨받기 피처코드는 접속서비스 pickup_feature_code 값(프로파일 공급 전까지 설정)
    public string PickupFeatureCode { get; set; } = "**";
    public bool AutoHoldOnAnswer { get; set; } = true;
    public bool ConfirmCloseMonitor { get; set; } = true;
    public int MaxMonitorWindows { get; set; } = 4;
    /// <summary>[무전] «메시지» 따라가기 — 채널 카드를 누르면 그 채널 대화로(기본 켬, §4.4).</summary>
    public bool FollowChannelThread { get; set; } = true;
    /// <summary>[무전] «이벤트» 따라가기 — 새 줄이 오면 맨 위로(기본 켬, §4.4).</summary>
    public bool FollowEvents { get; set; } = true;
    /// <summary>잠금 발언(§4.1) — PTT 를 클릭으로 누름 유지(풋스위치·긴 공지용). 기본 꺼짐.</summary>
    public bool LockTalk { get; set; } = false;
    /// <summary>창 닫기(×)를 종료가 아니라 최소화로 — 기본 꺼짐(× = 종료, 진행 중인 세션이 있으면 확인). 켜면 종료는 메뉴 [종료] 로만.</summary>
    public bool MinimizeToTray { get; set; } = false;
    public int MessageRetentionDays { get; set; } = 30;

    // 표시
    /// <summary>light | dark — 기본 밝게(«모드마다 한 화면» 시안이 밝은 테마로 그려졌다, §3.2).</summary>
    public string Theme { get; set; } = "light";
    /// <summary>설정 판 — 2 = 모드마다 한 화면(옛 판 0·1 — 도킹 6패널의 설정을 처음 읽을 때 테마를 밝게로 한 번 되돌린다) · 3 = PTT 기본 키 Space
    /// (옛 기본값 Ctrl+Space 를 그대로 둔 설정을 한 번 Space 로 옮긴다) · 4 = 창 닫기(×) 기본 = 종료(옛 기본값 «최소화» 를 한 번 끈다).</summary>
    public int UiVersion { get; set; } = SettingsStore.CurrentUiVersion;
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

    public const int CurrentUiVersion = 4;
    public AppSettings Current { get; private set; } = new();
    public event EventHandler? Changed;

    public void Load()
    {
        try
        {
            if (File.Exists(AppPaths.Settings))
            {
                string text = File.ReadAllText(AppPaths.Settings);
                Current = JsonSerializer.Deserialize<AppSettings>(text, Json) ?? new AppSettings();
                // 옛 판의 저장값을 한 번씩 옮긴다 — 판 2: UiVersion 이 없던 판(도킹 6패널)은 새 화면의 기본 테마(밝게)로(어둡게는 설정에서 다시 고른다) ·
                //   판 3: PTT 기본 키 Space — 옛 기본값(Ctrl+Space)을 그대로 둔 설정만 옮긴다(직접 고른 다른 키는 그대로) ·
                //   판 4: 창 닫기(×) = 종료가 기본 — 옛 기본값(최소화)이 저장돼 있으므로 한 번 끈다(다시 켜는 것은 설정 › 관제)
                int from = text.Contains("\"UiVersion\"", StringComparison.Ordinal) ? Current.UiVersion : 0;
                if (from < CurrentUiVersion)
                {
                    if (from < 2) Current.Theme = "light";
                    if (from < 3 && Current.HotKeys.TryGetValue("ptt", out var ptt) && string.Equals(ptt.Replace(" ", ""), "Ctrl+Space", StringComparison.OrdinalIgnoreCase))
                        Current.HotKeys["ptt"] = "Space";
                    if (from < 4) Current.MinimizeToTray = false;
                    Current.UiVersion = CurrentUiVersion;
                    Save();
                }
            }
        }
        catch (Exception) { Current = new AppSettings(); }
    }

    /// <summary>저장 실패(읽기 전용·잠김 %APPDATA%)는 삼킨다 — 메모리 설정은 유효하고, 창 닫기·토글마다 오류 창이 뜨면 안 된다.</summary>
    public bool LastSaveFailed { get; private set; }

    public void Save()
    {
        try
        {
            AppPaths.Ensure();
            File.WriteAllText(AppPaths.Settings, JsonSerializer.Serialize(Current, Json));
            LastSaveFailed = false;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { LastSaveFailed = true; }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    public void Update(Action<AppSettings> mutate)
    {
        mutate(Current);
        Save();
    }
}
