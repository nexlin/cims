// 대화 말풍선 — 새 메시지·대화 전환 때 맨 아래로, 파일 끌어 놓기(MCData FD — 📎 와 같은 경로).
using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class MessagesView : UserControl
{
    private INotifyCollectionChanged? _watched;
    private MessagesViewModelBase? _vm;

    public MessagesView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Hook();
    }

    private void Hook()
    {
        if (_vm is not null) _vm.PropertyChanged -= OnVm;
        _vm = DataContext as MessagesViewModelBase;
        if (_vm is null) return;
        _vm.PropertyChanged += OnVm;
        Watch(_vm.Selected?.Messages);
    }

    private void OnVm(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MessagesViewModelBase.Selected)) { Watch(_vm?.Selected?.Messages); ScrollToEnd(); }
    }

    private void Watch(INotifyCollectionChanged? c)
    {
        if (_watched is not null) _watched.CollectionChanged -= OnMessages;
        _watched = c;
        if (_watched is not null) _watched.CollectionChanged += OnMessages;
    }

    private void OnMessages(object? s, NotifyCollectionChangedEventArgs e) => ScrollToEnd();

    private void ScrollToEnd()
    {
        // 항목 수는 지연 실행 시점에 다시 본다 — 채널 따라가기로 빈 대화로 바뀐 뒤 실행되면 Items[-1] 이 된다.
        Dispatcher.BeginInvoke(() => { int n = List.Items.Count; if (n > 0) List.ScrollIntoView(List.Items[n - 1]); });
    }

    // 파일 끌어 놓기 — 📎 와 같은 경로(MCData FD). 첨부를 쓰지 않는 대화(SMS)·대화 미선택이면 받지 않는다.
    private void Root_DragOver(object sender, DragEventArgs e)
    {
        bool ok = DataContext is McDataMessagesViewModel { CanAttach: true } && e.Data.GetDataPresent(DataFormats.FileDrop);
        e.Effects = ok ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private async void Root_Drop(object sender, DragEventArgs e)
    {
        if (DataContext is not McDataMessagesViewModel { CanAttach: true, Selected: { } t } vm) return;
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] files) return;
        e.Handled = true;
        foreach (var f in files.Where(System.IO.File.Exists)) await vm.SendFileAsync(t, f);
    }
}
