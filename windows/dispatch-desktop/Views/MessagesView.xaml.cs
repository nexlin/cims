// 메시지 공용 뷰 — 제목·부제·글자 수 표시(SMS 는 "n/70 SMS")·Enter 전송·새 메시지 자동 스크롤.
using System.Collections.Specialized;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class MessagesView : UserControl
{
    public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(nameof(Title), typeof(string), typeof(MessagesView), new PropertyMetadata(""));
    public static readonly DependencyProperty SubtitleProperty = DependencyProperty.Register(nameof(Subtitle), typeof(string), typeof(MessagesView), new PropertyMetadata(""));
    public static readonly DependencyProperty CountTextProperty = DependencyProperty.Register(nameof(CountText), typeof(string), typeof(MessagesView), new PropertyMetadata(""));
    public static readonly DependencyProperty InputTipProperty = DependencyProperty.Register(nameof(InputTip), typeof(string), typeof(MessagesView), new PropertyMetadata("메시지…"));

    public string Title { get => (string)GetValue(TitleProperty); set => SetValue(TitleProperty, value); }
    public string Subtitle { get => (string)GetValue(SubtitleProperty); set => SetValue(SubtitleProperty, value); }
    public string CountText { get => (string)GetValue(CountTextProperty); set => SetValue(CountTextProperty, value); }
    public string InputTip { get => (string)GetValue(InputTipProperty); set => SetValue(InputTipProperty, value); }

    /// <summary>두 값이 같은 참조인가 — 선택 스레드 칩 강조.</summary>
    public static IMultiValueConverter SameRef { get; } = new SameRefConverter();

    private sealed class SameRefConverter : IMultiValueConverter
    {
        public object Convert(object[] values, Type t, object p, CultureInfo c) => values.Length == 2 && values[0] is not null && ReferenceEquals(values[0], values[1]);
        public object[] ConvertBack(object v, Type[] t, object p, CultureInfo c) => throw new NotSupportedException();
    }

    private INotifyCollectionChanged? _watched;

    public MessagesView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Hook();
    }

    private void Hook()
    {
        if (DataContext is not MessagesViewModelBase vm) return;
        vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(MessagesViewModelBase.Selected)) { Watch(vm.Selected?.Messages); ScrollToEnd(); }
            if (vm is SmsMessagesViewModel sms && e.PropertyName is nameof(SmsMessagesViewModel.CountText) or nameof(MessagesViewModelBase.Input)) CountText = sms.CountText;
        };
        if (vm is SmsMessagesViewModel s0) CountText = s0.CountText;
        Watch(vm.Selected?.Messages);
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
        if (List.Items.Count > 0) Dispatcher.BeginInvoke(() => List.ScrollIntoView(List.Items[List.Items.Count - 1]));
    }

    private void Input_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && Keyboard.Modifiers != ModifierKeys.Shift && DataContext is MessagesViewModelBase vm && vm.SendCommand.CanExecute(null))
        {
            vm.SendCommand.Execute(null);
            e.Handled = true;
        }
    }
}
