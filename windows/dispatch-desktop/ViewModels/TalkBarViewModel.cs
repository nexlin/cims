// 발언 바(§3.2 — 모든 화면 하단 80) — 발언 대상 집합(내 채널 카드 ✓)의 투영: PTT 버튼(누르는 동안 대상 전부 floorRequest / 떼면 floorRelease) · 대상 칩(대상별 floor 상태) ·
// 남은 발언 게이지(승인된 대상 중 최소) · 잠금 발언 토글. 다중 채널 동시 발언 = 단말 팬아웃 — floor 요청·해제의 소유는 PttChannelsViewModel(_talking).
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>발언 바의 대상 칩 하나 — 채널 이름 + 상태 점 + 문구(요청 전·승인·대기 n번째·거부 사유).</summary>
public sealed partial class TalkTargetChip : ObservableObject
{
    public ChannelCard Card { get; }
    public TalkTargetChip(ChannelCard c) { Card = c; }
    public string Name => Card.Title;
    public bool IsGranted => Card.IsSpeaking;
    public bool IsRequesting => Card.IsRequesting;
    public bool IsQueued => Card.IsQueued;
    public bool IsDenied => Card.DeniedFlash;
    public bool IsEmergency => Card.IsEmergency;
    public string StateText => IsGranted ? "승인" : IsQueued ? Card.FloorNote : IsRequesting ? "요청" : IsDenied ? (Card.FloorNote.Length > 0 ? Card.FloorNote : "거부") : "";
    /// <summary>칩 글자 — "순찰1 · 승인" / "상황실 · 대기"(요청 전).</summary>
    public string ChipText => $"{Name} · {(StateText.Length > 0 ? StateText : "대기")}";
    public void Refresh() { foreach (var p in new[] { nameof(Name), nameof(IsGranted), nameof(IsRequesting), nameof(IsQueued), nameof(IsDenied), nameof(IsEmergency), nameof(StateText), nameof(ChipText) }) OnPropertyChanged(p); }
}

public sealed partial class TalkBarViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly PttChannelsViewModel _channels;
    public ObservableCollection<TalkTargetChip> Targets { get; } = new();
    /// <summary>잠금 발언 중(클릭으로 누름 유지) — 다시 클릭 또는 TalkLimit 로 해제.</summary>
    [ObservableProperty] private bool _isLocked;
    [ObservableProperty] private bool _allDeniedFlash;

    /// <summary>대상 칩 클릭 → 그 카드 포커스.</summary>
    public event EventHandler<ChannelCard>? FocusRequested;

    public TalkBarViewModel(DispatchSession s, PttChannelsViewModel channels)
    {
        _s = s; _channels = channels;
        channels.TargetsChanged += (_, _) => Rebuild();
        channels.Cards.CollectionChanged += (_, _) => Rebuild();
        s.Floor += (_, e) =>
        {
            Refresh();
            // 잠금 발언은 요청해 둔 대상이 전부 끝나야 풀린다 — 한 채널의 회수·시한(TalkLimit·Revoked·Denied)이 나머지 발언을 풀지 않는다
            if (IsLocked && !_channels.IsTalking) IsLocked = false;
            if (e.Event.Kind == CimsUe.FloorEventKind.Denied && Targets.Count > 0 && Targets.All(t => !t.IsGranted && !t.IsRequesting && !t.IsQueued))
            { AllDeniedFlash = true; _ = Task.Delay(1000).ContinueWith(_ => AllDeniedFlash = false, TaskScheduler.FromCurrentSynchronizationContext()); }
        };
        s.Settings.Changed += (_, _) => OnPropertyChanged(nameof(LockTalkEnabled));
        Rebuild();
    }

    public int TargetCount => Targets.Count;
    public bool HasTargets => Targets.Count > 0;
    public string TargetNames => Targets.Count == 0 ? "발언 대상 없음" : string.Join("·", Targets.Take(4).Select(t => t.Name)) + (Targets.Count > 4 ? $" +{Targets.Count - 4}" : "");
    public int GrantedCount => Targets.Count(t => t.IsGranted);
    public bool IsSpeaking => GrantedCount > 0;
    public bool IsPartial => IsSpeaking && GrantedCount < Targets.Count;
    public bool IsRequesting => !IsSpeaking && Targets.Any(t => t.IsRequesting || t.IsQueued);
    public bool IsEmergency => Targets.Any(t => t.IsEmergency);
    /// <summary>대상에 내가 연 일제 통화가 있다 — 발언을 놓으면 코어가 호를 해제한다(TS 24.380 §6.2.4.6.4).</summary>
    public bool IsBroadcast => Targets.Any(t => t.Card.IsBroadcastInitiator);
    public bool CanPtt => HasTargets;
    public string Hint => HasTargets ? (IsBroadcast ? "일제 통화 · 발언을 놓으면 통화가 끝납니다" : Targets.Count > 1 ? $"동시 발언 {Targets.Count}채널" : "발언 대상 1 · 내 채널") : "채널 카드의 ✓ 를 누르세요";
    /// <summary>PTT 버튼 둘째 줄 — 대상 없음 / 누르고 말하기 · 키 / 말하세요 / 요청 중.</summary>
    public string PttHint => !HasTargets ? "대상 없음" : IsSpeaking ? (IsBroadcast ? "놓으면 끝납니다" : "말하세요") : IsRequesting ? "발언권 요청 중" : IsLocked ? "다시 누르면 끝" : $"누르고 말하기 · {HotKeyText}";
    public string PttText => IsSpeaking ? (IsPartial ? $"발언 {GrantedCount}/{Targets.Count}" : "발언 중") : IsRequesting ? "요청 중" : AllDeniedFlash ? "거부" : IsLocked ? "잠금" : "PTT";
    /// <summary>남은 발언 게이지 — 승인된 대상 중 최소값.</summary>
    public double MinGauge => IsSpeaking ? Targets.Where(t => t.IsGranted).Min(t => t.Card.TalkGauge) : 0;
    public bool TalkLimitNear => Targets.Any(t => t.IsGranted && t.Card.TalkLimitNear);
    public TimeSpan SpeakerElapsed => Targets.Where(t => t.IsGranted).Select(t => t.Card.SpeakerElapsed).DefaultIfEmpty(TimeSpan.Zero).Max();
    public bool LockTalkEnabled => _s.Settings.Current.LockTalk;
    public string HotKeyText => HotKeyMap.DisplayOf(_s.Settings.Current.HotKeys, "ptt");

    private void Rebuild()
    {
        Targets.Clear();
        foreach (var c in _channels.Cards.Where(c => c.IsChecked)) Targets.Add(new TalkTargetChip(c));
        if (Targets.Count == 0) IsLocked = false;
        Refresh();
    }

    public void Refresh()
    {
        foreach (var t in Targets) t.Refresh();
        foreach (var p in new[] { nameof(TargetCount), nameof(HasTargets), nameof(TargetNames), nameof(GrantedCount), nameof(IsSpeaking), nameof(IsPartial), nameof(IsRequesting),
                                  nameof(IsEmergency), nameof(IsBroadcast), nameof(CanPtt), nameof(Hint), nameof(PttText), nameof(PttHint), nameof(MinGauge), nameof(TalkLimitNear), nameof(SpeakerElapsed), nameof(HotKeyText) })
            OnPropertyChanged(p);
    }

    partial void OnIsLockedChanged(bool value) { OnPropertyChanged(nameof(PttText)); OnPropertyChanged(nameof(PttHint)); }
    partial void OnAllDeniedFlashChanged(bool value) => OnPropertyChanged(nameof(PttText));

    /// <summary>누름 — 대상 전부 floorRequest. 잠금 발언 설정이면 클릭 토글(눌러서 켬, 다시 눌러서 끔).</summary>
    [RelayCommand]
    public void PttDown()
    {
        if (!HasTargets) return;
        if (LockTalkEnabled && IsLocked) { PttUpCore(); IsLocked = false; return; }
        _channels.PttDown();
        if (LockTalkEnabled) IsLocked = true;
        Refresh();
    }
    /// <summary>뗌 — 잠금 중이면 무시(다음 클릭이 해제).</summary>
    [RelayCommand]
    public void PttUp() { if (LockTalkEnabled && IsLocked) return; PttUpCore(); }
    private void PttUpCore() { _channels.PttRelease(); Refresh(); }

    [RelayCommand] private void Focus(TalkTargetChip t) => FocusRequested?.Invoke(this, t.Card);
    [RelayCommand] private void Remove(TalkTargetChip t) => _channels.ToggleTargetCommand.Execute(t.Card);
    [RelayCommand] private void ClearAll() => _channels.ClearTargetsCommand.Execute(null);
}
