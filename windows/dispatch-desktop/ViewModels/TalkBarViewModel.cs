// ① 발언 바(§4.1) — 발언 대상 집합(① 카드 체크)의 투영: PTT 버튼(누르는 동안 대상 전부 floorRequest / 떼면 floorRelease) · 대상 칩(대상별 floor 상태) ·
// 남은 발언 게이지(승인된 대상 중 최소) · 잠금 발언 토글. 다중 채널 동시 발언은 단말 팬아웃(SDK 과제, §13) — 그 전엔 대상 1개만 허용(MultiTalkSupported).
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
    public void Refresh() { foreach (var p in new[] { nameof(Name), nameof(IsGranted), nameof(IsRequesting), nameof(IsQueued), nameof(IsDenied), nameof(IsEmergency), nameof(StateText) }) OnPropertyChanged(p); }
}

public sealed partial class TalkBarViewModel : ObservableObject
{
    /// <summary>libcimsue 발언 대상 집합 API(setTalkTargets) 도입 뒤 true — 그 전엔 체크 1개(PttChannelsViewModel.MaxTargets).</summary>
    public const bool MultiTalkSupported = false;

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
            if (e.Event.Kind == CimsUe.FloorEventKind.TalkLimit || e.Event.Kind == CimsUe.FloorEventKind.Revoked) IsLocked = false;
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
    public bool CanPtt => HasTargets;
    public string Hint => HasTargets ? (Targets.Count > 1 ? $"동시 발언 {Targets.Count}채널" : "발언 대상 1 · 내 채널") : "내 채널 카드의 체크로 고르세요";
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
                                  nameof(IsEmergency), nameof(CanPtt), nameof(Hint), nameof(PttText), nameof(MinGauge), nameof(TalkLimitNear), nameof(SpeakerElapsed), nameof(HotKeyText) })
            OnPropertyChanged(p);
    }

    partial void OnIsLockedChanged(bool value) => OnPropertyChanged(nameof(PttText));
    partial void OnAllDeniedFlashChanged(bool value) => OnPropertyChanged(nameof(PttText));

    /// <summary>누름 — 대상 전부 floorRequest. 잠금 발언 설정이면 클릭 토글(눌러서 켬, 다시 눌러서 끔).</summary>
    public void PttDown()
    {
        if (!HasTargets) return;
        if (LockTalkEnabled && IsLocked) { PttUpCore(); IsLocked = false; return; }
        foreach (var t in Targets) t.Card.PttDown();
        if (LockTalkEnabled) IsLocked = true;
        Refresh();
    }
    /// <summary>뗌 — 잠금 중이면 무시(다음 클릭이 해제).</summary>
    public void PttUp() { if (LockTalkEnabled && IsLocked) return; PttUpCore(); }
    private void PttUpCore() { foreach (var t in Targets) t.Card.PttUp(); Refresh(); }

    [RelayCommand] private void Focus(TalkTargetChip t) => FocusRequested?.Invoke(this, t.Card);
    [RelayCommand] private void Remove(TalkTargetChip t) => _channels.ToggleTargetCommand.Execute(t.Card);
    [RelayCommand] private void ClearAll() => _channels.ClearTargetsCommand.Execute(null);
}
