// 로그인 창(§6) — 아이디·비밀번호·CSC 주소(고급 접힘)·자동 로그인. 비밀번호는 저장하지 않는다.
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class LoginViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    [ObservableProperty] private string _host;
    [ObservableProperty] private int _port;
    [ObservableProperty] private string _loginId;
    [ObservableProperty] private string _password = "";
    [ObservableProperty] private bool _autoLogin;
    [ObservableProperty] private bool _verifyServer;
    [ObservableProperty] private string _caPemPath;
    [ObservableProperty] private bool _advanced;
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private string _status = "";

    public event EventHandler? Succeeded;

    public LoginViewModel(DispatchSession s)
    {
        _s = s;
        var c = s.Settings.Current;
        _host = c.CscHost; _port = c.CscPort; _loginId = c.LoginId; _autoLogin = c.AutoLogin; _verifyServer = c.CscVerifyServer; _caPemPath = c.TlsCaPemPath;
        _advanced = c.CscHost.Length == 0;
    }

    public bool CanLogin => !Busy && Host.Trim().Length > 0 && LoginId.Trim().Length > 0 && Password.Length > 0;
    partial void OnBusyChanged(bool v) => OnPropertyChanged(nameof(CanLogin));
    partial void OnHostChanged(string v) => OnPropertyChanged(nameof(CanLogin));
    partial void OnLoginIdChanged(string v) => OnPropertyChanged(nameof(CanLogin));
    partial void OnPasswordChanged(string v) => OnPropertyChanged(nameof(CanLogin));

    [RelayCommand] private void ToggleAdvanced() => Advanced = !Advanced;

    [RelayCommand]
    private async Task LoginAsync()
    {
        if (!CanLogin) return;
        Busy = true; Error = ""; Status = "로그인 중…";
        _s.Settings.Update(x => { x.AutoLogin = AutoLogin; x.CscVerifyServer = VerifyServer; x.TlsCaPemPath = CaPemPath.Trim(); });
        var r = await _s.LoginAsync(Host.Trim(), Port, LoginId.Trim(), Password);
        if (!r.Ok) { Error = r.Code is 401 or 403 ? "아이디 또는 비밀번호가 올바르지 않습니다" : $"로그인 실패 — {r.Reason} ({r.Code})"; Busy = false; Status = ""; return; }
        Status = "프로파일 적용·등록 중…";
        var st = await _s.StartAsync();
        if (!st.Ok) { Error = $"엔진 기동 실패 — {st.Reason} ({st.Code})"; Busy = false; Status = ""; return; }
        Busy = false;
        Succeeded?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>저장 토큰 자동 로그인.</summary>
    public async Task<bool> ResumeAsync()
    {
        Busy = true; Status = "저장된 로그인으로 접속 중…";
        var r = await _s.ResumeAsync();
        if (r.Ok) r = await _s.StartAsync();
        Busy = false; Status = "";
        if (!r.Ok) { Error = $"자동 로그인 실패 — {r.Reason}"; return false; }
        Succeeded?.Invoke(this, EventArgs.Empty);
        return true;
    }
}
